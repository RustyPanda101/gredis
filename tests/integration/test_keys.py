"""Key-space commands independent of value type (DEL/EXISTS/TYPE/UNLINK).
UNLINK's background-freeing behavior lives in test_thread_pool.py instead,
since it needs a large aggregate to be meaningful."""

import unittest

from helpers import RespReader, cmd, send_raw, start_server


class KeyCommandsTest(unittest.TestCase):
    def test_del_counts_only_existing_keys(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SET", "a", "1"))
                reader.read_one()
                send_raw(sock, cmd("SET", "b", "2"))
                reader.read_one()
                send_raw(sock, cmd("DEL", "a", "b", "nosuch"))
                self.assertEqual(reader.read_one(), b":2\r\n")
                send_raw(sock, cmd("EXISTS", "a"))
                self.assertEqual(reader.read_one(), b":0\r\n")

    def test_unlink_counts_only_existing_keys(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SET", "a", "1"))
                reader.read_one()
                send_raw(sock, cmd("SET", "b", "2"))
                reader.read_one()
                send_raw(sock, cmd("UNLINK", "a", "b", "nosuch"))
                self.assertEqual(reader.read_one(), b":2\r\n")
                send_raw(sock, cmd("EXISTS", "a"))
                self.assertEqual(reader.read_one(), b":0\r\n")

    def test_unlink_on_missing_key_returns_zero(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("UNLINK", "nosuch"))
                self.assertEqual(reader.read_one(), b":0\r\n")

    def test_exists_counts_duplicates(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SET", "a", "1"))
                reader.read_one()
                send_raw(sock, cmd("EXISTS", "a", "a", "nosuch"))
                self.assertEqual(reader.read_one(), b":2\r\n")

    def test_type_on_string_and_missing_key(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("TYPE", "nosuch"))
                self.assertEqual(reader.read_one(), b"+none\r\n")
                send_raw(sock, cmd("SET", "s", "v"))
                reader.read_one()
                send_raw(sock, cmd("TYPE", "s"))
                self.assertEqual(reader.read_one(), b"+string\r\n")

    def test_server_stays_alive_after_all_of_the_above(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("PING"))
                self.assertEqual(reader.read_one(), b"+PONG\r\n")
            self.assertIsNone(server.process.poll())


if __name__ == "__main__":
    unittest.main()
