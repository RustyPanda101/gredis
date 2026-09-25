"""String commands end to end over a real socket, plus DBSIZE/FLUSHALL."""

import unittest

from helpers import RespReader, cmd, send_raw, start_server


class StringCommandsTest(unittest.TestCase):
    def test_get_on_missing_key_is_null_bulk(self):
        with start_server() as server:
            with server.connect() as sock:
                send_raw(sock, cmd("GET", "nosuch"))
                self.assertEqual(RespReader(sock).read_one(), b"$-1\r\n")

    def test_set_then_get_round_trips(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SET", "k", "v"))
                self.assertEqual(reader.read_one(), b"+OK\r\n")
                send_raw(sock, cmd("GET", "k"))
                self.assertEqual(reader.read_one(), b"$1\r\nv\r\n")

    def test_set_overwrites_and_binary_safe_value(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                value = b"a\r\nb\x00c"
                send_raw(sock, cmd("SET", "k", value))
                self.assertEqual(reader.read_one(), b"+OK\r\n")
                send_raw(sock, cmd("GET", "k"))
                self.assertEqual(reader.read_one(), b"$6\r\n" + value + b"\r\n")

    def test_binary_safe_key(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                key = b"k\x00ey"
                send_raw(sock, cmd("SET", key, "v"))
                self.assertEqual(reader.read_one(), b"+OK\r\n")
                send_raw(sock, cmd("GET", key))
                self.assertEqual(reader.read_one(), b"$1\r\nv\r\n")

    def test_100kib_value_round_trips(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                value = (b"x" * 100 * 1024)
                send_raw(sock, cmd("SET", "big", value))
                self.assertEqual(reader.read_one(), b"+OK\r\n")
                send_raw(sock, cmd("GET", "big"))
                self.assertEqual(reader.read_one(), f"${len(value)}\r\n".encode() + value + b"\r\n")

    def test_mget_mixes_present_and_missing(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SET", "a", "1"))
                reader.read_one()
                send_raw(sock, cmd("MGET", "a", "nosuch", "a"))
                self.assertEqual(
                    reader.read_one(),
                    b"*3\r\n$1\r\n1\r\n$-1\r\n$1\r\n1\r\n",
                )

    def test_mset_sets_every_pair(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("MSET", "a", "1", "b", "2"))
                self.assertEqual(reader.read_one(), b"+OK\r\n")
                send_raw(sock, cmd("MGET", "a", "b"))
                self.assertEqual(reader.read_one(), b"*2\r\n$1\r\n1\r\n$1\r\n2\r\n")

    def test_mset_odd_arg_count_is_arity_error(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("MSET", "a", "1", "b"))
                reply = reader.read_one()
                self.assertTrue(reply.startswith(b"-ERR wrong number of arguments"))
                send_raw(sock, cmd("PING"))
                self.assertEqual(reader.read_one(), b"+PONG\r\n")

    def test_incr_and_decr_on_missing_key(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("INCR", "counter"))
                self.assertEqual(reader.read_one(), b":1\r\n")
                send_raw(sock, cmd("DECR", "counter"))
                self.assertEqual(reader.read_one(), b":0\r\n")

    def test_incrby_decrby(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("INCRBY", "n", "10"))
                self.assertEqual(reader.read_one(), b":10\r\n")
                send_raw(sock, cmd("DECRBY", "n", "3"))
                self.assertEqual(reader.read_one(), b":7\r\n")
                send_raw(sock, cmd("INCRBY", "n", "-7"))
                self.assertEqual(reader.read_one(), b":0\r\n")

    def test_incr_on_non_integer_is_an_error(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SET", "s", "not a number"))
                reader.read_one()
                send_raw(sock, cmd("INCR", "s"))
                reply = reader.read_one()
                self.assertEqual(reply, b"-ERR value is not an integer or out of range\r\n")

    def test_incrby_bad_argument_is_an_error(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("INCRBY", "n", "abc"))
                reply = reader.read_one()
                self.assertEqual(reply, b"-ERR value is not an integer or out of range\r\n")

    def test_incr_overflow_at_int64_max(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SET", "n", "9223372036854775807"))
                reader.read_one()
                send_raw(sock, cmd("INCR", "n"))
                reply = reader.read_one()
                self.assertEqual(reply, b"-ERR increment or decrement would overflow\r\n")

    def test_decr_overflow_at_int64_min(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SET", "n", "-9223372036854775808"))
                reader.read_one()
                send_raw(sock, cmd("DECR", "n"))
                reply = reader.read_one()
                self.assertEqual(reply, b"-ERR increment or decrement would overflow\r\n")

    def test_decrby_int64_min_argument_is_rejected_before_negation_overflow(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("DECRBY", "n", "-9223372036854775808"))
                reply = reader.read_one()
                self.assertEqual(reply, b"-ERR increment or decrement would overflow\r\n")

    def test_append_creates_and_extends(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("APPEND", "s", "Hello "))
                self.assertEqual(reader.read_one(), b":6\r\n")
                send_raw(sock, cmd("APPEND", "s", "World"))
                self.assertEqual(reader.read_one(), b":11\r\n")
                send_raw(sock, cmd("GET", "s"))
                self.assertEqual(reader.read_one(), b"$11\r\nHello World\r\n")

    def test_strlen(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("STRLEN", "nosuch"))
                self.assertEqual(reader.read_one(), b":0\r\n")
                send_raw(sock, cmd("SET", "s", "hello"))
                reader.read_one()
                send_raw(sock, cmd("STRLEN", "s"))
                self.assertEqual(reader.read_one(), b":5\r\n")

    def test_dbsize_and_flushall(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SET", "a", "1"))
                reader.read_one()
                send_raw(sock, cmd("SET", "b", "2"))
                reader.read_one()
                send_raw(sock, cmd("DBSIZE"))
                self.assertEqual(reader.read_one(), b":2\r\n")
                send_raw(sock, cmd("FLUSHALL"))
                self.assertEqual(reader.read_one(), b"+OK\r\n")
                send_raw(sock, cmd("DBSIZE"))
                self.assertEqual(reader.read_one(), b":0\r\n")

    def test_flushall_async_keyword_accepted(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SET", "a", "1"))
                reader.read_one()
                send_raw(sock, cmd("FLUSHALL", "ASYNC"))
                self.assertEqual(reader.read_one(), b"+OK\r\n")
                send_raw(sock, cmd("DBSIZE"))
                self.assertEqual(reader.read_one(), b":0\r\n")

    def test_flushall_bad_argument_is_a_syntax_error(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("FLUSHALL", "bogus"))
                reply = reader.read_one()
                self.assertEqual(reply, b"-ERR syntax error\r\n")


if __name__ == "__main__":
    unittest.main()
