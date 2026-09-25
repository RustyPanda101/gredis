"""End to end: raw bytes -> parser -> dispatcher -> handler -> writer -> outbuf.

Also covers the old raw-echo transport tests (concurrency, partial I/O,
RST/half-close, backpressure, idle CPU) re-done as real RESP commands,
since the server only speaks RESP now.
"""

import os
import socket
import struct
import threading
import time
import unittest

from helpers import RespReader, cmd, pick_free_port, run_server_once, send_raw, start_server


class BasicCommandsTest(unittest.TestCase):
    def test_ping_no_args(self):
        with start_server() as server:
            with server.connect() as sock:
                send_raw(sock, cmd("PING"))
                self.assertEqual(RespReader(sock).read_one(), b"+PONG\r\n")

    def test_ping_with_message(self):
        with start_server() as server:
            with server.connect() as sock:
                send_raw(sock, cmd("PING", "hello world"))
                self.assertEqual(RespReader(sock).read_one(), b"$11\r\nhello world\r\n")

    def test_ping_too_many_args_is_wrong_arity_but_stays_open(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("PING", "a", "b"))
                reply = reader.read_one()
                self.assertTrue(reply.startswith(b"-ERR wrong number of arguments"))
                # Connection must still be usable afterward.
                send_raw(sock, cmd("PING"))
                self.assertEqual(reader.read_one(), b"+PONG\r\n")

    def test_echo(self):
        with start_server() as server:
            with server.connect() as sock:
                send_raw(sock, cmd("ECHO", "hi there"))
                self.assertEqual(RespReader(sock).read_one(), b"$8\r\nhi there\r\n")

    def test_quit_replies_ok_then_closes(self):
        with start_server() as server:
            with server.connect() as sock:
                sock.settimeout(5.0)
                send_raw(sock, cmd("QUIT"))
                self.assertEqual(RespReader(sock).read_one(), b"+OK\r\n")
                self.assertEqual(sock.recv(16), b"")

    def test_command_stub_returns_empty_array(self):
        with start_server() as server:
            with server.connect() as sock:
                send_raw(sock, cmd("COMMAND"))
                self.assertEqual(RespReader(sock).read_one(), b"*0\r\n")

    def test_config_get_stub_returns_empty_array(self):
        with start_server() as server:
            with server.connect() as sock:
                send_raw(sock, cmd("CONFIG", "GET", "save"))
                self.assertEqual(RespReader(sock).read_one(), b"*0\r\n")

    def test_hello_is_an_error(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("HELLO", "3"))
                reply = reader.read_one()
                self.assertTrue(reply.startswith(b"-ERR"))
                # RESP2 fallback: the connection stays open and usable.
                send_raw(sock, cmd("PING"))
                self.assertEqual(reader.read_one(), b"+PONG\r\n")

    def test_unknown_command_keeps_connection_open(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("FROBNICATE"))
                reply = reader.read_one()
                self.assertTrue(reply.startswith(b"-ERR unknown command"))
                send_raw(sock, cmd("PING"))
                self.assertEqual(reader.read_one(), b"+PONG\r\n")

    def test_mixed_case_command_names(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                for name in ("ping", "PING", "PiNg", "pInG"):
                    send_raw(sock, cmd(name))
                    self.assertEqual(reader.read_one(), b"+PONG\r\n")

    def test_request_with_zero_args_is_ignored(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                # empty array should produce no reply, real command after it still works
                send_raw(sock, b"*0\r\n" + cmd("PING"))
                self.assertEqual(reader.read_one(), b"+PONG\r\n")


class PipeliningAndFramingTest(unittest.TestCase):
    def test_1000_pipelined_pings_in_one_send_get_1000_pongs_in_order(self):
        with start_server() as server:
            with server.connect() as sock:
                sock.settimeout(10.0)
                batch = cmd("PING") * 1000
                sock.sendall(batch)
                reader = RespReader(sock)
                for _ in range(1000):
                    self.assertEqual(reader.read_one(), b"+PONG\r\n")

    def test_command_split_across_three_sends(self):
        with start_server() as server:
            with server.connect() as sock:
                sock.settimeout(5.0)
                full = cmd("ECHO", "hello world")
                third = len(full) // 3
                parts = [full[:third], full[third : 2 * third], full[2 * third :]]
                for part in parts:
                    sock.sendall(part)
                    time.sleep(0.02)  # ensure each arrives as a separate recv()
                self.assertEqual(RespReader(sock).read_one(), b"$11\r\nhello world\r\n")

    def test_byte_at_a_time_send_is_reassembled_correctly(self):
        with start_server() as server:
            with server.connect() as sock:
                sock.settimeout(5.0)
                full = cmd("ECHO", "x")
                for b in full:
                    sock.send(bytes([b]))
                self.assertEqual(RespReader(sock).read_one(), b"$1\r\nx\r\n")

    def test_huge_echo_within_limits_works(self):
        payload = (bytes(range(256)) * ((5 * 1024 * 1024) // 256 + 1))[: 5 * 1024 * 1024]
        with start_server() as server:
            with server.connect() as sock:
                sock.settimeout(15.0)

                send_errors = []

                def sender():
                    try:
                        sock.sendall(cmd("ECHO", payload))
                    except OSError as e:
                        send_errors.append(e)

                t = threading.Thread(target=sender)
                t.start()

                reader = RespReader(sock)
                reply = reader.read_one()
                t.join(timeout=15)

                self.assertFalse(send_errors, f"sender failed: {send_errors}")
                self.assertEqual(reply, b"$" + str(len(payload)).encode() + b"\r\n" + payload + b"\r\n")


class ProtocolErrorTest(unittest.TestCase):
    def test_malformed_input_gets_error_and_closes_only_that_connection(self):
        with start_server() as server:
            with server.connect() as victim:
                victim.settimeout(5.0)
                with server.connect() as bystander:
                    bystander.settimeout(5.0)

                    # bad type byte, not valid RESP at all
                    send_raw(victim, b"#3\r\n")
                    reply = RespReader(victim).read_one()
                    self.assertTrue(reply.startswith(b"-"))
                    self.assertEqual(victim.recv(16), b"")

                    send_raw(bystander, cmd("PING"))
                    self.assertEqual(RespReader(bystander).read_one(), b"+PONG\r\n")

    def test_oversized_bulk_length_is_a_protocol_error(self):
        # never actually sends the payload -- must reject on declared length alone
        with start_server() as server:
            with server.connect() as sock:
                sock.settimeout(5.0)
                send_raw(sock, b"*1\r\n$99999999999\r\n")
                reply = RespReader(sock).read_one()
                self.assertTrue(reply.startswith(b"-"))
                self.assertEqual(sock.recv(16), b"")

    def test_port_already_in_use_exits_cleanly(self):
        port = pick_free_port()
        with start_server(port=port):
            result = run_server_once("--port", str(port), timeout=5.0)
            self.assertEqual(result.returncode, 1)
            self.assertIn("bind()", result.stderr)


class TransportRegressionTest(unittest.TestCase):
    """Old transport-layer guarantees, re-checked over real RESP commands."""

    def test_50_concurrent_clients_each_get_correct_replies(self):
        with start_server() as server:
            n_clients = 50
            errors = [None] * n_clients

            def worker(i):
                try:
                    with server.connect() as sock:
                        sock.settimeout(10.0)
                        reader = RespReader(sock)
                        for round_num in range(5):
                            msg = f"client-{i}-round-{round_num}"
                            send_raw(sock, cmd("ECHO", msg))
                            reply = reader.read_one()
                            expected = f"${len(msg)}\r\n{msg}\r\n".encode()
                            if reply != expected:
                                raise AssertionError(f"client {i}: expected {expected!r}, got {reply!r}")
                except Exception as e:
                    errors[i] = e

            threads = [threading.Thread(target=worker, args=(i,)) for i in range(n_clients)]
            for t in threads:
                t.start()
            for t in threads:
                t.join(timeout=15)
            for i, err in enumerate(errors):
                self.assertIsNone(err, f"client {i} failed: {err}")

    def test_client_reset_does_not_kill_the_server(self):
        with start_server() as server:
            sock1 = server.connect()
            sock1.settimeout(5.0)
            send_raw(sock1, cmd("PING"))
            self.assertEqual(RespReader(sock1).read_one(), b"+PONG\r\n")

            sock1.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            sock1.close()

            with server.connect() as sock2:
                sock2.settimeout(5.0)
                send_raw(sock2, cmd("PING"))
                self.assertEqual(RespReader(sock2).read_one(), b"+PONG\r\n")

    def test_hard_output_limit_disconnects_a_client_that_never_reads(self):
        with start_server(
            "--client-output-soft-limit", "4096",
            "--client-output-hard-limit", "16384",
        ) as server:
            with server.connect() as sock:
                sock.settimeout(5.0)
                # kernel send buffer autotunes up to ~4MiB before send() actually
                # blocks/fills server-side, so the batch needs to clear that, not
                # just the 16KiB hard limit. SO_RCVBUF cap just makes the client
                # look like a real non-draining reader.
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 2048)
                batch = cmd("ECHO", "x" * 4000) * 3000  # ~12 MiB of replies
                disconnected = False
                try:
                    sock.sendall(batch)
                except OSError:
                    disconnected = True
                if not disconnected:
                    try:
                        disconnected = sock.recv(1) == b""
                    except OSError:
                        disconnected = True
                self.assertTrue(
                    disconnected,
                    "server must disconnect a client that exceeds the output hard limit",
                )

    def test_idle_server_with_many_idle_clients_uses_almost_no_cpu(self):
        with start_server() as server:
            sockets = [server.connect() for _ in range(100)]
            try:
                pid = server.process.pid
                clk_tck = os.sysconf("SC_CLK_TCK")

                def cpu_seconds() -> float:
                    with open(f"/proc/{pid}/stat") as f:
                        fields = f.read().split()
                    return (int(fields[13]) + int(fields[14])) / clk_tck

                time.sleep(0.3)
                before = cpu_seconds()
                time.sleep(1.0)
                after = cpu_seconds()
                self.assertLess(after - before, 0.05)
            finally:
                for s in sockets:
                    s.close()

    def test_command_path_stays_single_threaded_and_fd_count_returns_to_baseline(self):
        # process has worker threads now (for UNLINK/FLUSHALL ASYNC), but they
        # never touch parsing/dispatch/db -- pin --threads so task count is fixed
        with start_server("--threads", "2") as server:
            pid = server.process.pid
            time.sleep(0.2)
            self.assertEqual(len(os.listdir(f"/proc/{pid}/task")), 3)

            baseline_fds = len(os.listdir(f"/proc/{pid}/fd"))
            for _ in range(20):
                sock = server.connect()
                sock.settimeout(5.0)
                send_raw(sock, cmd("PING"))
                self.assertEqual(RespReader(sock).read_one(), b"+PONG\r\n")
                sock.close()
            time.sleep(0.2)
            self.assertEqual(len(os.listdir(f"/proc/{pid}/fd")), baseline_fds)


if __name__ == "__main__":
    unittest.main()
