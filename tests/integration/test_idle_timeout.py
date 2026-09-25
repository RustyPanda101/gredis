"""--idle-timeout-sec closes idle connections, leaves active ones (including
ones just making write progress) alone. Uses a short 1s timeout throughout."""

import socket
import time
import unittest

from helpers import RespReader, cmd, send_raw, start_server


def is_closed(sock: socket.socket) -> bool:
    sock.settimeout(1.0)
    try:
        data = sock.recv(1)
        return data == b""  # orderly close reads as EOF (empty bytes)
    except OSError:
        return True  # RST or similar -- also "closed" for this test's purposes


class IdleTimeoutTest(unittest.TestCase):
    def test_idle_client_is_closed_within_roughly_2x_the_timeout(self):
        with start_server("--idle-timeout-sec", "1") as server:
            with server.connect() as sock:
                send_raw(sock, cmd("PING"))
                self.assertEqual(RespReader(sock).read_one(), b"+PONG\r\n")
                self.assertTrue(is_closed(sock), "idle client was not closed within the timeout")

    def test_client_pinging_faster_than_the_timeout_survives(self):
        with start_server("--idle-timeout-sec", "1") as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                deadline = time.monotonic() + 3.0
                while time.monotonic() < deadline:
                    send_raw(sock, cmd("PING"))
                    self.assertEqual(reader.read_one(), b"+PONG\r\n")
                    time.sleep(0.3)
                send_raw(sock, cmd("PING"))
                self.assertEqual(reader.read_one(), b"+PONG\r\n")

    def test_idle_timeout_disabled_by_default(self):
        with start_server() as server:  # no --idle-timeout-sec -> 0 -> disabled
            with server.connect() as sock:
                sock.settimeout(1.5)
                send_raw(sock, cmd("PING"))
                self.assertEqual(RespReader(sock).read_one(), b"+PONG\r\n")
                time.sleep(1.5)
                send_raw(sock, cmd("PING"))
                self.assertEqual(RespReader(sock).read_one(), b"+PONG\r\n")

    def test_fd_count_returns_to_baseline_after_idle_closes(self):
        import os

        with start_server("--idle-timeout-sec", "1") as server:
            proc_fd_dir = f"/proc/{server.process.pid}/fd"
            # without this, baseline can transiently include the readiness-probe
            # fd the server hasn't reaped yet -- caused a real flake before
            time.sleep(0.2)
            baseline = len(os.listdir(proc_fd_dir))

            socks = [server.connect() for _ in range(10)]
            for s in socks:
                send_raw(s, cmd("PING"))
                self.assertEqual(RespReader(s).read_one(), b"+PONG\r\n")

            try:
                # 5s covers timeout + cron granularity + scheduling slack
                # under a loaded machine (3s margin flaked before)
                deadline = time.monotonic() + 5.0
                while time.monotonic() < deadline and len(os.listdir(proc_fd_dir)) > baseline:
                    time.sleep(0.1)
                self.assertEqual(len(os.listdir(proc_fd_dir)), baseline)
            finally:
                for s in socks:
                    s.close()

    def test_server_survives_a_close_and_an_idle_timeout_in_the_same_tick(self):
        # a QUIT-closed connection must not use-after-free if it's also
        # swept as idle in the same cron tick
        with start_server("--idle-timeout-sec", "1") as server:
            idle_socks = [server.connect() for _ in range(5)]
            try:
                time.sleep(0.9)  # close to the 1s timeout, but not past it yet
                with server.connect() as quit_sock:
                    send_raw(quit_sock, cmd("QUIT"))
                    self.assertEqual(RespReader(quit_sock).read_one(), b"+OK\r\n")
                for s in idle_socks:
                    self.assertTrue(is_closed(s))

                with server.connect() as sock:
                    send_raw(sock, cmd("PING"))
                    self.assertEqual(RespReader(sock).read_one(), b"+PONG\r\n")
                self.assertIsNone(server.process.poll())
            finally:
                for s in idle_socks:
                    s.close()


if __name__ == "__main__":
    unittest.main()
