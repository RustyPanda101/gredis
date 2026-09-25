"""Hash commands end to end over a real socket."""

import time
import unittest

from helpers import RespReader, cmd, send_raw, start_server


def _parse_one(raw: bytes, pos: int):
    prefix = raw[pos:pos + 1]
    line_end = raw.index(b"\r\n", pos)
    if prefix in (b"+", b"-", b":"):
        return raw[pos:line_end + 2], line_end + 2
    if prefix == b"$":
        n = int(raw[pos + 1:line_end])
        if n == -1:
            return raw[pos:line_end + 2], line_end + 2
        payload_end = line_end + 2 + n + 2
        return raw[pos:payload_end], payload_end
    if prefix == b"*":
        n = int(raw[pos + 1:line_end])
        cursor = line_end + 2
        for _ in range(n):
            _, cursor = _parse_one(raw, cursor)
        return raw[pos:cursor], cursor
    raise ValueError(f"unexpected RESP type byte: {prefix!r}")


def read_array(reader) -> list:
    # read_one() returns the whole array's raw bytes in one call, so
    # elements have to be re-parsed out of that blob, not read one by one
    raw = reader.read_one()
    assert raw.startswith(b"*"), raw
    line_end = raw.index(b"\r\n")
    n = int(raw[1:line_end])
    items = []
    pos = line_end + 2
    for _ in range(n):
        item, pos = _parse_one(raw, pos)
        items.append(item)
    return items


class HashCommandsTest(unittest.TestCase):
    def test_hset_then_hget(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("HSET", "h", "f1", "v1"))
                self.assertEqual(reader.read_one(), b":1\r\n")
                send_raw(sock, cmd("HGET", "h", "f1"))
                self.assertEqual(reader.read_one(), b"$2\r\nv1\r\n")
                send_raw(sock, cmd("HGET", "h", "nosuch"))
                self.assertEqual(reader.read_one(), b"$-1\r\n")
                send_raw(sock, cmd("HGET", "nosuchkey", "f"))
                self.assertEqual(reader.read_one(), b"$-1\r\n")

    def test_hset_counts_only_new_fields_and_last_value_wins_on_duplicate(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("HSET", "h", "f", "first", "f", "second"))
                self.assertEqual(reader.read_one(), b":1\r\n")
                send_raw(sock, cmd("HGET", "h", "f"))
                self.assertEqual(reader.read_one(), b"$6\r\nsecond\r\n")
                send_raw(sock, cmd("HSET", "h", "f", "third"))
                self.assertEqual(reader.read_one(), b":0\r\n")

    def test_hset_odd_field_value_count_is_arity_error(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("HSET", "h", "f1", "v1", "f2"))
                reply = reader.read_one()
                self.assertTrue(reply.startswith(b"-ERR wrong number of arguments"))

    def test_hmget_mixes_present_and_missing_fields(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("HSET", "h", "a", "1", "b", "2"))
                reader.read_one()
                send_raw(sock, cmd("HMGET", "h", "a", "nosuch", "b"))
                self.assertEqual(read_array(reader), [b"$1\r\n1\r\n", b"$-1\r\n", b"$1\r\n2\r\n"])
                send_raw(sock, cmd("HMGET", "nosuchkey", "a", "b"))
                self.assertEqual(read_array(reader), [b"$-1\r\n", b"$-1\r\n"])

    def test_hdel_and_last_field_deletes_the_key(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("HSET", "h", "a", "1", "b", "2"))
                reader.read_one()
                send_raw(sock, cmd("HDEL", "h", "a", "nosuch"))
                self.assertEqual(reader.read_one(), b":1\r\n")
                send_raw(sock, cmd("EXISTS", "h"))
                self.assertEqual(reader.read_one(), b":1\r\n")
                send_raw(sock, cmd("HDEL", "h", "b"))
                self.assertEqual(reader.read_one(), b":1\r\n")
                send_raw(sock, cmd("EXISTS", "h"))
                self.assertEqual(reader.read_one(), b":0\r\n")

    def test_hdel_on_missing_key_returns_zero(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("HDEL", "nosuch", "f"))
                self.assertEqual(reader.read_one(), b":0\r\n")

    def test_hexists(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("HSET", "h", "f", "v"))
                reader.read_one()
                send_raw(sock, cmd("HEXISTS", "h", "f"))
                self.assertEqual(reader.read_one(), b":1\r\n")
                send_raw(sock, cmd("HEXISTS", "h", "nosuch"))
                self.assertEqual(reader.read_one(), b":0\r\n")
                send_raw(sock, cmd("HEXISTS", "nosuchkey", "f"))
                self.assertEqual(reader.read_one(), b":0\r\n")

    def test_hlen(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("HLEN", "nosuch"))
                self.assertEqual(reader.read_one(), b":0\r\n")
                send_raw(sock, cmd("HSET", "h", "a", "1", "b", "2", "c", "3"))
                reader.read_one()
                send_raw(sock, cmd("HLEN", "h"))
                self.assertEqual(reader.read_one(), b":3\r\n")

    def test_hgetall_hkeys_hvals(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("HSET", "h", "a", "1", "b", "2"))
                reader.read_one()

                send_raw(sock, cmd("HGETALL", "h"))
                pairs = read_array(reader)
                self.assertEqual(len(pairs), 4)
                as_dict = dict(zip(pairs[0::2], pairs[1::2]))
                self.assertEqual(as_dict, {b"$1\r\na\r\n": b"$1\r\n1\r\n", b"$1\r\nb\r\n": b"$1\r\n2\r\n"})

                send_raw(sock, cmd("HKEYS", "h"))
                self.assertEqual(sorted(read_array(reader)), sorted([b"$1\r\na\r\n", b"$1\r\nb\r\n"]))

                send_raw(sock, cmd("HVALS", "h"))
                self.assertEqual(sorted(read_array(reader)), sorted([b"$1\r\n1\r\n", b"$1\r\n2\r\n"]))

                send_raw(sock, cmd("HGETALL", "nosuch"))
                self.assertEqual(read_array(reader), [])

    def test_hincrby(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("HINCRBY", "h", "counter", "5"))
                self.assertEqual(reader.read_one(), b":5\r\n")
                send_raw(sock, cmd("HINCRBY", "h", "counter", "-2"))
                self.assertEqual(reader.read_one(), b":3\r\n")

    def test_hincrby_on_non_integer_field_is_an_error(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("HSET", "h", "f", "not a number"))
                reader.read_one()
                send_raw(sock, cmd("HINCRBY", "h", "f", "1"))
                self.assertEqual(reader.read_one(), b"-ERR value is not an integer or out of range\r\n")

    def test_hincrby_overflow(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("HSET", "h", "f", "9223372036854775807"))
                reader.read_one()
                send_raw(sock, cmd("HINCRBY", "h", "f", "1"))
                self.assertEqual(reader.read_one(), b"-ERR increment or decrement would overflow\r\n")

    def test_wrongtype_both_ways(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SET", "s", "v"))
                reader.read_one()
                send_raw(sock, cmd("HSET", "s", "f", "v"))
                self.assertEqual(reader.read_one(), b"-WRONGTYPE Operation against a key holding the wrong kind of value\r\n")
                send_raw(sock, cmd("HGET", "s", "f"))
                self.assertEqual(reader.read_one(), b"-WRONGTYPE Operation against a key holding the wrong kind of value\r\n")

                send_raw(sock, cmd("HSET", "h", "f", "v"))
                reader.read_one()
                send_raw(sock, cmd("GET", "h"))
                self.assertEqual(reader.read_one(), b"-WRONGTYPE Operation against a key holding the wrong kind of value\r\n")
                send_raw(sock, cmd("APPEND", "h", "x"))
                self.assertEqual(reader.read_one(), b"-WRONGTYPE Operation against a key holding the wrong kind of value\r\n")

    def test_type_reports_hash(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("HSET", "h", "f", "v"))
                reader.read_one()
                send_raw(sock, cmd("TYPE", "h"))
                self.assertEqual(reader.read_one(), b"+hash\r\n")

    def test_ttl_on_a_hash_key_expires_it(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("HSET", "h", "f", "v"))
                reader.read_one()
                send_raw(sock, cmd("PEXPIRE", "h", "100"))
                self.assertEqual(reader.read_one(), b":1\r\n")

                deadline = time.monotonic() + 2.0
                while time.monotonic() < deadline:
                    send_raw(sock, cmd("EXISTS", "h"))
                    if reader.read_one() == b":0\r\n":
                        break
                    time.sleep(0.02)
                else:
                    self.fail("hash key with a TTL never expired")

                send_raw(sock, cmd("HGET", "h", "f"))
                self.assertEqual(reader.read_one(), b"$-1\r\n")

    def test_100k_fields_trigger_a_rehash_and_survive(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                n = 100_000
                pipeline = b"".join(cmd("HSET", "big", f"f{i}", str(i)) for i in range(n))
                send_raw(sock, pipeline)
                for _ in range(n):
                    reply = reader.read_one()
                    self.assertEqual(reply, b":1\r\n")

                send_raw(sock, cmd("HLEN", "big"))
                self.assertEqual(reader.read_one(), f":{n}\r\n".encode())

                for i in (0, n // 2, n - 1):
                    send_raw(sock, cmd("HGET", "big", f"f{i}"))
                    self.assertEqual(reader.read_one(), f"${len(str(i))}\r\n{i}\r\n".encode())

    def test_server_stays_alive(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("PING"))
                self.assertEqual(reader.read_one(), b"+PONG\r\n")
            self.assertIsNone(server.process.poll())


if __name__ == "__main__":
    unittest.main()
