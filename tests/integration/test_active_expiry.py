"""Cron-driven active expiration reclaims never-read keys and keeps the
server responsive while doing it."""

import time
import unittest

from helpers import RespReader, cmd, send_raw, start_server


def dbsize(reader, sock) -> int:
    send_raw(sock, cmd("DBSIZE"))
    reply = reader.read_one()
    assert reply.startswith(b":")
    return int(reply[1:-2])


class ActiveExpiryTest(unittest.TestCase):
    def test_10k_unread_keys_are_reclaimed_without_ever_being_read(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                # PX 50 is short enough that some keys can expire before all
                # 10k +OKs are even read back, so we don't assert the starting
                # size, just that it reaches 0 within budget
                pipeline = b"".join(cmd("SET", f"k:{i}", "v", "PX", "50") for i in range(10_000))
                send_raw(sock, pipeline)
                for _ in range(10_000):
                    self.assertEqual(reader.read_one(), b"+OK\r\n")

                deadline = time.monotonic() + 2.0
                size = dbsize(reader, sock)
                while size > 0 and time.monotonic() < deadline:
                    time.sleep(0.05)
                    size = dbsize(reader, sock)
                self.assertEqual(size, 0, "active expiration never reclaimed all 10k keys within 2s")

    def test_server_stays_responsive_during_mass_expiry(self):
        with start_server() as server:
            with server.connect() as write_sock, server.connect() as ping_sock:
                writer = RespReader(write_sock)
                pinger = RespReader(ping_sock)

                pipeline = b"".join(cmd("SET", f"k:{i}", "v", "PX", "30") for i in range(10_000))
                send_raw(write_sock, pipeline)
                for _ in range(10_000):
                    self.assertEqual(writer.read_one(), b"+OK\r\n")

                # ping while the keys above expire in the background; generous
                # slack here (well under a second) for a loaded CI machine
                deadline = time.monotonic() + 1.5
                worst_latency = 0.0
                while time.monotonic() < deadline:
                    t0 = time.monotonic()
                    send_raw(ping_sock, cmd("PING"))
                    reply = pinger.read_one()
                    latency = time.monotonic() - t0
                    worst_latency = max(worst_latency, latency)
                    self.assertEqual(reply, b"+PONG\r\n")
                    time.sleep(0.01)
                self.assertLess(worst_latency, 0.5, f"worst PING latency during mass expiry: {worst_latency:.3f}s")

    def test_persisted_key_survives_its_original_deadline(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SET", "k", "v", "PX", "100"))
                reader.read_one()
                send_raw(sock, cmd("PERSIST", "k"))
                self.assertEqual(reader.read_one(), b":1\r\n")

                time.sleep(0.3)  # well past the original 100ms deadline
                send_raw(sock, cmd("GET", "k"))
                self.assertEqual(reader.read_one(), b"$1\r\nv\r\n")

    def test_server_stays_alive(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("PING"))
                self.assertEqual(reader.read_one(), b"+PONG\r\n")
            self.assertIsNone(server.process.poll())


if __name__ == "__main__":
    unittest.main()
