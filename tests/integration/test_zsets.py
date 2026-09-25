"""Sorted-set commands end to end over a real socket."""

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


def bulk(s) -> bytes:
    """Encodes a single RESP2 bulk string -- for asserting on individual
    ZRANGE/ZPOPMIN reply elements read via read_array(), not a whole
    command (that's cmd())."""
    b = s.encode() if isinstance(s, str) else s
    return f"${len(b)}\r\n".encode() + b + b"\r\n"


class ZSetCommandsTest(unittest.TestCase):
    def test_zadd_counts_only_new_members(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("ZADD", "z", "1", "a", "2", "b"))
                self.assertEqual(reader.read_one(), b":2\r\n")
                send_raw(sock, cmd("ZADD", "z", "5", "a", "3", "c"))
                self.assertEqual(reader.read_one(), b":1\r\n")  # only "c" is new; "a" re-scored

    def test_zadd_duplicate_member_in_one_call_last_score_wins(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("ZADD", "z", "1", "a", "9", "a"))
                self.assertEqual(reader.read_one(), b":1\r\n")  # counted once
                send_raw(sock, cmd("ZSCORE", "z", "a"))
                self.assertEqual(reader.read_one(), bulk("9"))

    def test_zadd_nx_skips_existing_members(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("ZADD", "z", "1", "a"))
                reader.read_one()
                send_raw(sock, cmd("ZADD", "z", "NX", "9", "a", "2", "b"))
                self.assertEqual(reader.read_one(), b":1\r\n")  # only "b" added
                send_raw(sock, cmd("ZSCORE", "z", "a"))
                self.assertEqual(reader.read_one(), bulk("1"))  # unchanged

    def test_zadd_xx_only_updates_existing_members(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("ZADD", "z", "1", "a"))
                reader.read_one()
                send_raw(sock, cmd("ZADD", "z", "XX", "9", "a", "2", "b"))
                self.assertEqual(reader.read_one(), b":0\r\n")  # "b" is new but XX blocks it
                send_raw(sock, cmd("ZSCORE", "z", "a"))
                self.assertEqual(reader.read_one(), bulk("9"))
                send_raw(sock, cmd("ZSCORE", "z", "b"))
                self.assertEqual(reader.read_one(), b"$-1\r\n")

    def test_zadd_xx_on_missing_key_creates_nothing(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("ZADD", "nosuch", "XX", "1", "a"))
                self.assertEqual(reader.read_one(), b":0\r\n")
                send_raw(sock, cmd("EXISTS", "nosuch"))
                self.assertEqual(reader.read_one(), b":0\r\n")

    def test_zadd_nx_and_xx_together_is_an_error(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("ZADD", "z", "NX", "XX", "1", "a"))
                self.assertEqual(
                    reader.read_one(),
                    b"-ERR XX and NX options at the same time are not compatible\r\n",
                )

    def test_zadd_ch_counts_added_and_changed(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("ZADD", "z", "1", "a", "2", "b"))
                reader.read_one()
                send_raw(sock, cmd("ZADD", "z", "CH", "1", "a", "9", "b", "3", "c"))
                # a unchanged, b changed, c added -> 2
                self.assertEqual(reader.read_one(), b":2\r\n")

    def test_zadd_invalid_score_is_an_error(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("ZADD", "z", "notanumber", "a"))
                self.assertEqual(reader.read_one(), b"-ERR value is not a valid float\r\n")
                send_raw(sock, cmd("ZADD", "z", "nan", "a"))
                self.assertEqual(reader.read_one(), b"-ERR value is not a valid float\r\n")

    def test_zrem_and_last_member_deletes_the_key(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("ZADD", "z", "1", "a", "2", "b"))
                reader.read_one()
                send_raw(sock, cmd("ZREM", "z", "a", "nosuch"))
                self.assertEqual(reader.read_one(), b":1\r\n")
                send_raw(sock, cmd("EXISTS", "z"))
                self.assertEqual(reader.read_one(), b":1\r\n")
                send_raw(sock, cmd("ZREM", "z", "b"))
                self.assertEqual(reader.read_one(), b":1\r\n")
                send_raw(sock, cmd("EXISTS", "z"))
                self.assertEqual(reader.read_one(), b":0\r\n")

    def test_zrem_on_missing_key_returns_zero(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("ZREM", "nosuch", "a"))
                self.assertEqual(reader.read_one(), b":0\r\n")

    def test_zscore_missing_member_or_key(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("ZADD", "z", "1", "a"))
                reader.read_one()
                send_raw(sock, cmd("ZSCORE", "z", "nosuch"))
                self.assertEqual(reader.read_one(), b"$-1\r\n")
                send_raw(sock, cmd("ZSCORE", "nosuch", "a"))
                self.assertEqual(reader.read_one(), b"$-1\r\n")

    def test_zincrby_basic(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("ZINCRBY", "z", "5", "a"))
                self.assertEqual(reader.read_one(), bulk("5"))
                send_raw(sock, cmd("ZINCRBY", "z", "2.5", "a"))
                self.assertEqual(reader.read_one(), bulk("7.5"))

    def test_zincrby_nan_is_an_error(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("ZADD", "z", "inf", "a"))
                reader.read_one()
                send_raw(sock, cmd("ZINCRBY", "z", "-inf", "a"))
                self.assertEqual(reader.read_one(), b"-ERR resulting score is not a number (NaN)\r\n")

    def test_zcard(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("ZCARD", "nosuch"))
                self.assertEqual(reader.read_one(), b":0\r\n")
                send_raw(sock, cmd("ZADD", "z", "1", "a", "2", "b", "3", "c"))
                reader.read_one()
                send_raw(sock, cmd("ZCARD", "z"))
                self.assertEqual(reader.read_one(), b":3\r\n")

    def test_zrank_and_zrevrank(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("ZADD", "z", "1", "a", "2", "b", "3", "c"))
                reader.read_one()
                send_raw(sock, cmd("ZRANK", "z", "a"))
                self.assertEqual(reader.read_one(), b":0\r\n")
                send_raw(sock, cmd("ZRANK", "z", "c"))
                self.assertEqual(reader.read_one(), b":2\r\n")
                send_raw(sock, cmd("ZREVRANK", "z", "a"))
                self.assertEqual(reader.read_one(), b":2\r\n")
                send_raw(sock, cmd("ZREVRANK", "z", "c"))
                self.assertEqual(reader.read_one(), b":0\r\n")
                send_raw(sock, cmd("ZRANK", "z", "nosuch"))
                self.assertEqual(reader.read_one(), b"$-1\r\n")

    def test_equal_scores_ordered_by_member(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("ZADD", "z", "1", "charlie", "1", "alpha", "1", "bravo"))
                reader.read_one()
                send_raw(sock, cmd("ZRANGE", "z", "0", "-1"))
                members = [unwrap_bulk(m) for m in read_array(reader)]
                self.assertEqual(members, [b"alpha", b"bravo", b"charlie"])

    def test_zrange_basic_and_negative_indices(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("ZADD", "z", "1", "a", "2", "b", "3", "c", "4", "d"))
                reader.read_one()
                send_raw(sock, cmd("ZRANGE", "z", "0", "-1"))
                members = [unwrap_bulk(m) for m in read_array(reader)]
                self.assertEqual(members, [b"a", b"b", b"c", b"d"])
                send_raw(sock, cmd("ZRANGE", "z", "-2", "-1"))
                members = [unwrap_bulk(m) for m in read_array(reader)]
                self.assertEqual(members, [b"c", b"d"])
                send_raw(sock, cmd("ZRANGE", "z", "5", "10"))
                self.assertEqual(read_array(reader), [])

    def test_zrange_withscores(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("ZADD", "z", "1", "a", "2.5", "b"))
                reader.read_one()
                send_raw(sock, cmd("ZRANGE", "z", "0", "-1", "WITHSCORES"))
                items = read_array(reader)
                self.assertEqual(items, [bulk("a"), bulk("1"), bulk("b"), bulk("2.5")])

    def test_zrevrange(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("ZADD", "z", "1", "a", "2", "b", "3", "c"))
                reader.read_one()
                send_raw(sock, cmd("ZREVRANGE", "z", "0", "-1"))
                members = [unwrap_bulk(m) for m in read_array(reader)]
                self.assertEqual(members, [b"c", b"b", b"a"])

    def test_zrangebyscore_bounds(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("ZADD", "z", "1", "a", "2", "b", "3", "c", "4", "d"))
                reader.read_one()
                send_raw(sock, cmd("ZRANGEBYSCORE", "z", "2", "3"))
                members = [unwrap_bulk(m) for m in read_array(reader)]
                self.assertEqual(members, [b"b", b"c"])
                # exclusive bounds
                send_raw(sock, cmd("ZRANGEBYSCORE", "z", "(2", "4"))
                members = [unwrap_bulk(m) for m in read_array(reader)]
                self.assertEqual(members, [b"c", b"d"])
                send_raw(sock, cmd("ZRANGEBYSCORE", "z", "2", "(4"))
                members = [unwrap_bulk(m) for m in read_array(reader)]
                self.assertEqual(members, [b"b", b"c"])
                # inf bounds
                send_raw(sock, cmd("ZRANGEBYSCORE", "z", "-inf", "+inf"))
                members = [unwrap_bulk(m) for m in read_array(reader)]
                self.assertEqual(members, [b"a", b"b", b"c", b"d"])

    def test_zrangebyscore_limit(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("ZADD", "z", "1", "a", "2", "b", "3", "c", "4", "d"))
                reader.read_one()
                send_raw(sock, cmd("ZRANGEBYSCORE", "z", "-inf", "+inf", "LIMIT", "1", "2"))
                members = [unwrap_bulk(m) for m in read_array(reader)]
                self.assertEqual(members, [b"b", b"c"])

    def test_zrangebyscore_invalid_bound_is_an_error(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("ZADD", "z", "1", "a"))
                reader.read_one()
                send_raw(sock, cmd("ZRANGEBYSCORE", "z", "abc", "10"))
                self.assertEqual(reader.read_one(), b"-ERR min or max is not a float\r\n")

    def test_zcount(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("ZADD", "z", "1", "a", "2", "b", "3", "c", "4", "d"))
                reader.read_one()
                send_raw(sock, cmd("ZCOUNT", "z", "2", "3"))
                self.assertEqual(reader.read_one(), b":2\r\n")
                send_raw(sock, cmd("ZCOUNT", "z", "(2", "3"))
                self.assertEqual(reader.read_one(), b":1\r\n")
                send_raw(sock, cmd("ZCOUNT", "z", "-inf", "+inf"))
                self.assertEqual(reader.read_one(), b":4\r\n")
                send_raw(sock, cmd("ZCOUNT", "nosuch", "-inf", "+inf"))
                self.assertEqual(reader.read_one(), b":0\r\n")

    def test_zpopmin_default_count_one(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("ZADD", "z", "3", "c", "1", "a", "2", "b"))
                reader.read_one()
                send_raw(sock, cmd("ZPOPMIN", "z"))
                items = read_array(reader)
                self.assertEqual(items, [bulk("a"), bulk("1")])
                send_raw(sock, cmd("ZCARD", "z"))
                self.assertEqual(reader.read_one(), b":2\r\n")

    def test_zpopmin_with_count(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("ZADD", "z", "3", "c", "1", "a", "2", "b"))
                reader.read_one()
                send_raw(sock, cmd("ZPOPMIN", "z", "2"))
                items = read_array(reader)
                self.assertEqual(items, [bulk("a"), bulk("1"), bulk("b"), bulk("2")])

    def test_zpopmin_last_member_deletes_key(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("ZADD", "z", "1", "a"))
                reader.read_one()
                send_raw(sock, cmd("ZPOPMIN", "z"))
                read_array(reader)
                send_raw(sock, cmd("EXISTS", "z"))
                self.assertEqual(reader.read_one(), b":0\r\n")

    def test_zpopmin_on_missing_key_returns_empty_array(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("ZPOPMIN", "nosuch"))
                self.assertEqual(read_array(reader), [])
                send_raw(sock, cmd("ZPOPMIN", "nosuch", "5"))
                self.assertEqual(read_array(reader), [])

    def test_zpopmin_count_larger_than_zset_returns_everything(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("ZADD", "z", "1", "a", "2", "b"))
                reader.read_one()
                send_raw(sock, cmd("ZPOPMIN", "z", "100"))
                items = read_array(reader)
                self.assertEqual(items, [bulk("a"), bulk("1"), bulk("b"), bulk("2")])

    def test_zpopmin_negative_count_is_an_error(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("ZADD", "z", "1", "a"))
                reader.read_one()
                send_raw(sock, cmd("ZPOPMIN", "z", "-1"))
                self.assertEqual(reader.read_one(), b"-ERR value is out of range, must be positive\r\n")

    def test_wrongtype_both_ways(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SET", "str", "v"))
                reader.read_one()
                send_raw(sock, cmd("ZADD", "str", "1", "a"))
                self.assertEqual(reader.read_one(), b"-WRONGTYPE Operation against a key holding the wrong kind of value\r\n")
                send_raw(sock, cmd("ZCARD", "str"))
                self.assertEqual(reader.read_one(), b"-WRONGTYPE Operation against a key holding the wrong kind of value\r\n")

                send_raw(sock, cmd("ZADD", "z", "1", "a"))
                reader.read_one()
                send_raw(sock, cmd("GET", "z"))
                self.assertEqual(reader.read_one(), b"-WRONGTYPE Operation against a key holding the wrong kind of value\r\n")

    def test_type_reports_zset(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("ZADD", "z", "1", "a"))
                reader.read_one()
                send_raw(sock, cmd("TYPE", "z"))
                self.assertEqual(reader.read_one(), b"+zset\r\n")

    def test_100k_members_zrank_and_zrange_match_expected(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                n = 100_000
                # one ZADD per member, pipelined -- a single huge multi-bulk
                # command reassembled from small TCP reads hits the parser's
                # per-attempt re-scan cost, many small pipelined ones don't
                expected_members = [f"m{i:06d}" for i in range(n)]
                pipeline = b"".join(cmd("ZADD", "z", str(float(i)), expected_members[i]) for i in range(n))
                send_raw(sock, pipeline)
                for _ in range(n):
                    self.assertEqual(reader.read_one(), b":1\r\n")

                expected_members.sort()  # scores are unique and monotonic with i already

                send_raw(sock, cmd("ZRANK", "z", expected_members[0]))
                self.assertEqual(reader.read_one(), b":0\r\n")
                send_raw(sock, cmd("ZRANK", "z", expected_members[-1]))
                self.assertEqual(reader.read_one(), f":{n - 1}\r\n".encode())

                send_raw(sock, cmd("ZRANGE", "z", "0", "2"))
                members = [unwrap_bulk(m).decode() for m in read_array(reader)]
                self.assertEqual(members, expected_members[:3])

                send_raw(sock, cmd("ZRANGE", "z", "-3", "-1"))
                members = [unwrap_bulk(m).decode() for m in read_array(reader)]
                self.assertEqual(members, expected_members[-3:])

    def test_server_stays_alive(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("PING"))
                self.assertEqual(reader.read_one(), b"+PONG\r\n")
            self.assertIsNone(server.process.poll())


if __name__ == "__main__":
    unittest.main()
