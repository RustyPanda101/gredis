"""TTL semantics: SET's EX/PX/NX/XX, EXPIRE family, TTL/PTTL, PERSIST,
lazy expiration. Millisecond TTLs and short polling loops only -- no long sleeps.
"""

import time
import unittest

from helpers import RespReader, cmd, send_raw, start_server


def poll_until_nil(reader, sock, key, timeout=2.0):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        send_raw(sock, cmd("GET", key))
        if reader.read_one() == b"$-1\r\n":
            return True
        time.sleep(0.02)
    return False


class TtlTest(unittest.TestCase):
    def test_set_px_then_get_before_and_after_expiry(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SET", "k", "v", "PX", "100"))
                self.assertEqual(reader.read_one(), b"+OK\r\n")
                send_raw(sock, cmd("GET", "k"))
                self.assertEqual(reader.read_one(), b"$1\r\nv\r\n")
                self.assertTrue(poll_until_nil(reader, sock, "k"))

    def test_ttl_and_pttl_missing_and_no_ttl(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("TTL", "nosuch"))
                self.assertEqual(reader.read_one(), b":-2\r\n")
                send_raw(sock, cmd("PTTL", "nosuch"))
                self.assertEqual(reader.read_one(), b":-2\r\n")
                send_raw(sock, cmd("SET", "k", "v"))
                reader.read_one()
                send_raw(sock, cmd("TTL", "k"))
                self.assertEqual(reader.read_one(), b":-1\r\n")
                send_raw(sock, cmd("PTTL", "k"))
                self.assertEqual(reader.read_one(), b":-1\r\n")

    def test_expire_sets_ttl_and_persist_clears_it(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SET", "k", "v"))
                reader.read_one()
                send_raw(sock, cmd("EXPIRE", "k", "100"))
                self.assertEqual(reader.read_one(), b":1\r\n")
                send_raw(sock, cmd("TTL", "k"))
                reply = reader.read_one()
                self.assertTrue(reply.startswith(b":"))
                self.assertNotEqual(reply, b":-1\r\n")
                send_raw(sock, cmd("PERSIST", "k"))
                self.assertEqual(reader.read_one(), b":1\r\n")
                send_raw(sock, cmd("TTL", "k"))
                self.assertEqual(reader.read_one(), b":-1\r\n")
                send_raw(sock, cmd("PERSIST", "k"))
                self.assertEqual(reader.read_one(), b":0\r\n")  # already had none

    def test_expire_on_missing_key_returns_zero(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("EXPIRE", "nosuch", "100"))
                self.assertEqual(reader.read_one(), b":0\r\n")

    def test_expire_with_negative_seconds_deletes_the_key(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SET", "k", "v"))
                reader.read_one()
                send_raw(sock, cmd("EXPIRE", "k", "-1"))
                self.assertEqual(reader.read_one(), b":1\r\n")
                send_raw(sock, cmd("EXISTS", "k"))
                self.assertEqual(reader.read_one(), b":0\r\n")

    def test_set_clears_a_previous_ttl(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SET", "k", "v", "EX", "100"))
                reader.read_one()
                send_raw(sock, cmd("TTL", "k"))
                self.assertNotEqual(reader.read_one(), b":-1\r\n")
                send_raw(sock, cmd("SET", "k", "v2"))
                reader.read_one()
                send_raw(sock, cmd("TTL", "k"))
                self.assertEqual(reader.read_one(), b":-1\r\n")

    def test_incr_preserves_ttl(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SET", "n", "1", "EX", "100"))
                reader.read_one()
                send_raw(sock, cmd("INCR", "n"))
                self.assertEqual(reader.read_one(), b":2\r\n")
                send_raw(sock, cmd("TTL", "n"))
                self.assertNotEqual(reader.read_one(), b":-1\r\n")

    def test_append_preserves_ttl(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SET", "s", "a", "EX", "100"))
                reader.read_one()
                send_raw(sock, cmd("APPEND", "s", "b"))
                self.assertEqual(reader.read_one(), b":2\r\n")
                send_raw(sock, cmd("TTL", "s"))
                self.assertNotEqual(reader.read_one(), b":-1\r\n")

    def test_set_nx_and_xx_combinations(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                # NX on a missing key succeeds.
                send_raw(sock, cmd("SET", "k", "v1", "NX"))
                self.assertEqual(reader.read_one(), b"+OK\r\n")
                # NX on an existing key fails (nil, not an error).
                send_raw(sock, cmd("SET", "k", "v2", "NX"))
                self.assertEqual(reader.read_one(), b"$-1\r\n")
                send_raw(sock, cmd("GET", "k"))
                self.assertEqual(reader.read_one(), b"$2\r\nv1\r\n")
                # XX on an existing key succeeds.
                send_raw(sock, cmd("SET", "k", "v3", "XX"))
                self.assertEqual(reader.read_one(), b"+OK\r\n")
                # XX on a missing key fails (nil).
                send_raw(sock, cmd("SET", "nosuch", "v", "XX"))
                self.assertEqual(reader.read_one(), b"$-1\r\n")

    def test_set_nx_and_xx_together_is_a_syntax_error(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SET", "k", "v", "NX", "XX"))
                self.assertEqual(reader.read_one(), b"-ERR syntax error\r\n")

    def test_set_ex_and_px_together_is_a_syntax_error(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SET", "k", "v", "EX", "1", "PX", "1"))
                self.assertEqual(reader.read_one(), b"-ERR syntax error\r\n")

    def test_set_ex_zero_negative_and_non_numeric_are_errors(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                for bad in ("0", "-1", "abc"):
                    send_raw(sock, cmd("SET", "k", "v", "EX", bad))
                    reply = reader.read_one()
                    self.assertTrue(
                        reply.startswith(b"-ERR"), f"EX {bad} should be an error, got {reply!r}"
                    )

    def test_pexpire_overflow_is_an_error(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SET", "k", "v"))
                reader.read_one()
                send_raw(sock, cmd("PEXPIRE", "k", "9223372036854775807"))
                reply = reader.read_one()
                self.assertTrue(reply.startswith(b"-ERR invalid expire time"))

    def test_expireat_with_past_unix_time_deletes_key(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SET", "k", "v"))
                reader.read_one()
                send_raw(sock, cmd("EXPIREAT", "k", "1"))  # 1970, long past
                self.assertEqual(reader.read_one(), b":1\r\n")
                send_raw(sock, cmd("EXISTS", "k"))
                self.assertEqual(reader.read_one(), b":0\r\n")

    def test_pexpireat_with_future_unix_ms_sets_a_ttl(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SET", "k", "v"))
                reader.read_one()
                future_ms = int((time.time() + 100) * 1000)
                send_raw(sock, cmd("PEXPIREAT", "k", str(future_ms)))
                self.assertEqual(reader.read_one(), b":1\r\n")
                send_raw(sock, cmd("TTL", "k"))
                reply = reader.read_one()
                self.assertNotEqual(reply, b":-1\r\n")
                self.assertNotEqual(reply, b":-2\r\n")

    def test_server_stays_alive_and_responsive(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("PING"))
                self.assertEqual(reader.read_one(), b"+PONG\r\n")
            self.assertIsNone(server.process.poll())


if __name__ == "__main__":
    unittest.main()
