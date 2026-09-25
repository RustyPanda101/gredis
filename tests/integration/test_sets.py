"""Set commands end to end over a real socket."""

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


def unwrap_bulk(raw: bytes) -> bytes:
    line_end = raw.index(b"\r\n")
    return raw[line_end + 2:-2]


class SetCommandsTest(unittest.TestCase):
    def test_sadd_counts_only_new_members(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SADD", "s", "a", "b", "a"))
                self.assertEqual(reader.read_one(), b":2\r\n")  # "a" deduped within one call
                send_raw(sock, cmd("SADD", "s", "a", "c"))
                self.assertEqual(reader.read_one(), b":1\r\n")  # only "c" is new

    def test_srem_and_last_member_deletes_the_key(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SADD", "s", "a", "b"))
                reader.read_one()
                send_raw(sock, cmd("SREM", "s", "a", "nosuch"))
                self.assertEqual(reader.read_one(), b":1\r\n")
                send_raw(sock, cmd("EXISTS", "s"))
                self.assertEqual(reader.read_one(), b":1\r\n")
                send_raw(sock, cmd("SREM", "s", "b"))
                self.assertEqual(reader.read_one(), b":1\r\n")
                send_raw(sock, cmd("EXISTS", "s"))
                self.assertEqual(reader.read_one(), b":0\r\n")

    def test_srem_on_missing_key_returns_zero(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SREM", "nosuch", "a"))
                self.assertEqual(reader.read_one(), b":0\r\n")

    def test_sismember(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SADD", "s", "a"))
                reader.read_one()
                send_raw(sock, cmd("SISMEMBER", "s", "a"))
                self.assertEqual(reader.read_one(), b":1\r\n")
                send_raw(sock, cmd("SISMEMBER", "s", "b"))
                self.assertEqual(reader.read_one(), b":0\r\n")
                send_raw(sock, cmd("SISMEMBER", "nosuch", "a"))
                self.assertEqual(reader.read_one(), b":0\r\n")

    def test_scard(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SCARD", "nosuch"))
                self.assertEqual(reader.read_one(), b":0\r\n")
                send_raw(sock, cmd("SADD", "s", "a", "b", "c"))
                reader.read_one()
                send_raw(sock, cmd("SCARD", "s"))
                self.assertEqual(reader.read_one(), b":3\r\n")

    def test_smembers(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SADD", "s", "a", "b", "c"))
                reader.read_one()
                send_raw(sock, cmd("SMEMBERS", "s"))
                members = {unwrap_bulk(m) for m in read_array(reader)}
                self.assertEqual(members, {b"a", b"b", b"c"})
                send_raw(sock, cmd("SMEMBERS", "nosuch"))
                self.assertEqual(read_array(reader), [])

    def test_spop_without_count_removes_one_member(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SADD", "s", "a", "b", "c"))
                reader.read_one()
                send_raw(sock, cmd("SPOP", "s"))
                popped = unwrap_bulk(reader.read_one())
                self.assertIn(popped, (b"a", b"b", b"c"))
                send_raw(sock, cmd("SCARD", "s"))
                self.assertEqual(reader.read_one(), b":2\r\n")
                send_raw(sock, cmd("SISMEMBER", "s", popped))
                self.assertEqual(reader.read_one(), b":0\r\n")

    def test_spop_on_missing_key(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SPOP", "nosuch"))
                self.assertEqual(reader.read_one(), b"$-1\r\n")
                # unlike LPOP, SPOP's count form replies an empty array here, not null
                send_raw(sock, cmd("SPOP", "nosuch", "3"))
                self.assertEqual(reader.read_one(), b"*0\r\n")

    def test_spop_with_count_drains_the_whole_set_each_member_exactly_once(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                members = {f"m{i}" for i in range(50)}
                send_raw(sock, cmd("SADD", "s", *members))
                reader.read_one()

                send_raw(sock, cmd("SPOP", "s", "50"))
                popped = {unwrap_bulk(m).decode() for m in read_array(reader)}
                self.assertEqual(popped, members)  # every member exactly once, none repeated

                send_raw(sock, cmd("EXISTS", "s"))
                self.assertEqual(reader.read_one(), b":0\r\n")

    def test_spop_count_larger_than_set_returns_everything(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SADD", "s", "a", "b"))
                reader.read_one()
                send_raw(sock, cmd("SPOP", "s", "100"))
                popped = {unwrap_bulk(m) for m in read_array(reader)}
                self.assertEqual(popped, {b"a", b"b"})

    def test_spop_negative_count_is_an_error(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SADD", "s", "a"))
                reader.read_one()
                send_raw(sock, cmd("SPOP", "s", "-1"))
                self.assertEqual(reader.read_one(), b"-ERR value is out of range, must be positive\r\n")

    def test_sinter_basic(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SADD", "s1", "a", "b", "c"))
                reader.read_one()
                send_raw(sock, cmd("SADD", "s2", "b", "c", "d"))
                reader.read_one()
                send_raw(sock, cmd("SINTER", "s1", "s2"))
                result = {unwrap_bulk(m) for m in read_array(reader)}
                self.assertEqual(result, {b"b", b"c"})

    def test_sinter_with_missing_key_is_empty(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SADD", "s1", "a", "b"))
                reader.read_one()
                send_raw(sock, cmd("SINTER", "s1", "nosuch"))
                self.assertEqual(read_array(reader), [])

    def test_sinter_three_way(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SADD", "s1", "a", "b", "c"))
                reader.read_one()
                send_raw(sock, cmd("SADD", "s2", "b", "c", "d"))
                reader.read_one()
                send_raw(sock, cmd("SADD", "s3", "c", "d", "e"))
                reader.read_one()
                send_raw(sock, cmd("SINTER", "s1", "s2", "s3"))
                result = {unwrap_bulk(m) for m in read_array(reader)}
                self.assertEqual(result, {b"c"})

    def test_wrongtype_both_ways(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SET", "str", "v"))
                reader.read_one()
                send_raw(sock, cmd("SADD", "str", "a"))
                self.assertEqual(reader.read_one(), b"-WRONGTYPE Operation against a key holding the wrong kind of value\r\n")
                send_raw(sock, cmd("SCARD", "str"))
                self.assertEqual(reader.read_one(), b"-WRONGTYPE Operation against a key holding the wrong kind of value\r\n")

                send_raw(sock, cmd("SADD", "s", "a"))
                reader.read_one()
                send_raw(sock, cmd("GET", "s"))
                self.assertEqual(reader.read_one(), b"-WRONGTYPE Operation against a key holding the wrong kind of value\r\n")

    def test_type_reports_set(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SADD", "s", "a"))
                reader.read_one()
                send_raw(sock, cmd("TYPE", "s"))
                self.assertEqual(reader.read_one(), b"+set\r\n")

    def test_server_stays_alive(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("PING"))
                self.assertEqual(reader.read_one(), b"+PONG\r\n")
            self.assertIsNone(server.process.poll())


if __name__ == "__main__":
    unittest.main()
