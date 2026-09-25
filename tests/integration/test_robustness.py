"""Garbage suite (malformed/hostile input), graceful shutdown, maxclients,
and fd exhaustion."""

import os
import signal
import socket
import tempfile
import time
import unittest

from helpers import RespReader, cmd, send_raw, start_server


class GarbageSuiteTest(unittest.TestCase):
    def test_random_bytes_are_a_protocol_error_and_dont_affect_others(self):
        with start_server() as server:
            with server.connect() as bad_sock, server.connect() as good_sock:
                send_raw(bad_sock, os.urandom(256))
                # random bytes could look like a plausible RESP header for a
                # while, so just drain until close instead of checking exact bytes
                bad_sock.settimeout(2.0)
                saw_close = False
                try:
                    while True:
                        chunk = bad_sock.recv(4096)
                        if chunk == b"":
                            saw_close = True
                            break
                except socket.timeout:
                    pass
                self.assertTrue(saw_close, "server never closed the connection after random garbage")

                reader = RespReader(good_sock)
                send_raw(good_sock, cmd("PING"))
                self.assertEqual(reader.read_one(), b"+PONG\r\n")
            self.assertIsNone(server.process.poll())

    def test_valid_prefix_followed_by_garbage_is_a_protocol_error(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("PING") + b"\xff\xfenotresp\x00\x01")
                self.assertEqual(reader.read_one(), b"+PONG\r\n")
                sock.settimeout(2.0)
                reply_or_close = b""
                try:
                    reply_or_close = sock.recv(4096)
                except socket.timeout:
                    self.fail("connection neither replied nor closed after trailing garbage")
                if reply_or_close != b"":
                    self.assertTrue(reply_or_close.startswith(b"-"))

    def test_huge_declared_array_count_is_a_protocol_error(self):
        with start_server() as server:
            with server.connect() as sock:
                send_raw(sock, b"*1000000000\r\n")
                sock.settimeout(2.0)
                reply = sock.recv(4096)
                self.assertTrue(reply.startswith(b"-"), reply)

    def test_negative_bulk_length_other_than_minus_one_is_a_protocol_error(self):
        with start_server() as server:
            with server.connect() as sock:
                send_raw(sock, b"*1\r\n$-2\r\n")
                sock.settimeout(2.0)
                reply = sock.recv(4096)
                self.assertTrue(reply.startswith(b"-"), reply)

    def test_oversized_bulk_header_declared_but_never_sent_is_closed(self):
        # 100 MiB exceeds the default 64 MiB max_bulk_len, rejected as soon
        # as the length header is parsed
        with start_server() as server:
            with server.connect() as sock:
                send_raw(sock, b"*1\r\n$104857600\r\n")
                sock.settimeout(3.0)
                saw_close_or_error = False
                try:
                    reply = sock.recv(4096)
                    saw_close_or_error = reply == b"" or reply.startswith(b"-")
                except socket.timeout:
                    pass
                self.assertTrue(saw_close_or_error, "connection was neither closed nor error'd")

    def test_max_query_buf_closes_a_connection_over_the_limit(self):
        # bulk length well under max_bulk_len but only partially sent --
        # buffered bytes alone trip the lowered max-query-buf
        with start_server("--max-query-buf", "512") as server:
            with server.connect() as sock:
                send_raw(sock, b"*1\r\n$2000\r\n" + b"x" * 1000)
                sock.settimeout(2.0)
                reply = sock.recv(4096)
                self.assertTrue(reply.startswith(b"-"), reply)
                self.assertEqual(sock.recv(4096), b"")  # then closes

    def test_slowloris_client_does_not_block_other_clients(self):
        with start_server() as server:
            with server.connect() as slow_sock, server.connect() as fast_sock:
                slow_sock.settimeout(1.0)
                fast_reader = RespReader(fast_sock)
                payload = cmd("PING")
                for i in range(min(10, len(payload))):
                    send_raw(slow_sock, payload[i:i + 1])
                    send_raw(fast_sock, cmd("PING"))
                    self.assertEqual(fast_reader.read_one(), b"+PONG\r\n")
                    time.sleep(0.05)
            self.assertIsNone(server.process.poll())


class GracefulShutdownTest(unittest.TestCase):
    def test_sigterm_exits_zero_and_writes_the_snapshot(self):
        with tempfile.TemporaryDirectory() as tmpdir:
            snap = os.path.join(tmpdir, "dump.grds")
            with start_server("--snapshot", snap) as server:
                with server.connect() as sock:
                    reader = RespReader(sock)
                    send_raw(sock, cmd("SET", "k", "v"))
                    reader.read_one()

                os.kill(server.process.pid, signal.SIGTERM)
                server.process.wait(timeout=5.0)
                self.assertEqual(server.process.returncode, 0)

            self.assertTrue(os.path.exists(snap))

            with start_server("--snapshot", snap) as server:
                with server.connect() as sock:
                    reader = RespReader(sock)
                    send_raw(sock, cmd("GET", "k"))
                    self.assertEqual(reader.read_one(), b"$1\r\nv\r\n")


class MaxClientsTest(unittest.TestCase):
    def test_extra_client_beyond_maxclients_is_rejected(self):
        with start_server("--maxclients", "5") as server:
            sockets = [server.connect() for _ in range(5)]
            try:
                for sock in sockets:
                    reader = RespReader(sock)
                    send_raw(sock, cmd("PING"))
                    self.assertEqual(reader.read_one(), b"+PONG\r\n")

                extra = server.connect()
                extra.settimeout(2.0)
                reply = extra.recv(4096)
                self.assertEqual(reply, b"-ERR max number of clients reached\r\n")
                self.assertEqual(extra.recv(4096), b"")  # server closes it
                extra.close()

                # freeing a slot lets a new connection through again
                sockets[0].close()
                sockets = sockets[1:]
                time.sleep(0.1)
                new_sock = server.connect()
                reader = RespReader(new_sock)
                send_raw(new_sock, cmd("PING"))
                self.assertEqual(reader.read_one(), b"+PONG\r\n")
                sockets.append(new_sock)
            finally:
                for sock in sockets:
                    sock.close()
            self.assertIsNone(server.process.poll())


class FdExhaustionTest(unittest.TestCase):
    def test_survives_emfile_under_a_low_fd_limit(self):
        with start_server(rlimit_nofile=64) as server:
            successes = 0
            failures = 0
            sockets = []
            try:
                for _ in range(100):
                    try:
                        s = socket.create_connection(("127.0.0.1", server.port), timeout=0.5)
                        sockets.append(s)
                        successes += 1
                    except OSError:
                        failures += 1
            finally:
                for s in sockets:
                    s.close()

            # exact split doesn't matter, just that the server survives EMFILE
            # without spinning or crashing
            self.assertGreater(successes + failures, 0)

            pid = server.process.pid
            clk_tck = os.sysconf("SC_CLK_TCK")

            def cpu_seconds() -> float:
                with open(f"/proc/{pid}/stat") as f:
                    fields = f.read().split()
                return (int(fields[13]) + int(fields[14])) / clk_tck

            time.sleep(0.2)
            before = cpu_seconds()
            time.sleep(1.0)
            after = cpu_seconds()
            self.assertLess(after - before, 0.3, "server appears to be spinning after EMFILE")

            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("PING"))
                self.assertEqual(reader.read_one(), b"+PONG\r\n")


class ServerAliveTest(unittest.TestCase):
    def test_server_stays_alive(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("PING"))
                self.assertEqual(reader.read_one(), b"+PONG\r\n")
            self.assertIsNone(server.process.poll())


if __name__ == "__main__":
    unittest.main()
