"""List commands end to end over a real socket."""

import unittest

from helpers import RespReader, cmd, send_raw, start_server


def read_array_bulks(reader) -> list:
    """Reads an array reply of bulk strings/nils, returns decoded values."""
    raw = reader.read_one()
    assert raw.startswith(b"*"), raw
    line_end = raw.index(b"\r\n")
    n = int(raw[1:line_end])
    pos = line_end + 2
    out = []
    for _ in range(n):
        prefix = raw[pos:pos + 1]
        item_line_end = raw.index(b"\r\n", pos)
        length = int(raw[pos + 1:item_line_end])
        if length == -1:
            out.append(None)
            pos = item_line_end + 2
        else:
            start = item_line_end + 2
            out.append(raw[start:start + length])
            pos = start + length + 2
        assert prefix == b"$"
    return out


class ListCommandsTest(unittest.TestCase):
    def test_rpush_appends_in_order(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("RPUSH", "l", "a", "b", "c"))
                self.assertEqual(reader.read_one(), b":3\r\n")
                send_raw(sock, cmd("LRANGE", "l", "0", "-1"))
                self.assertEqual(read_array_bulks(reader), [b"a", b"b", b"c"])

    def test_lpush_prepends_reversing_argument_order(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("LPUSH", "l", "a", "b", "c"))
                self.assertEqual(reader.read_one(), b":3\r\n")
                send_raw(sock, cmd("LRANGE", "l", "0", "-1"))
                self.assertEqual(read_array_bulks(reader), [b"c", b"b", b"a"])

    def test_llen(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("LLEN", "nosuch"))
                self.assertEqual(reader.read_one(), b":0\r\n")
                send_raw(sock, cmd("RPUSH", "l", "a", "b"))
                reader.read_one()
                send_raw(sock, cmd("LLEN", "l"))
                self.assertEqual(reader.read_one(), b":2\r\n")

    def test_lpop_rpop_single_element_and_last_element_deletes_key(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("RPUSH", "l", "a", "b"))
                reader.read_one()
                send_raw(sock, cmd("LPOP", "l"))
                self.assertEqual(reader.read_one(), b"$1\r\na\r\n")
                send_raw(sock, cmd("RPOP", "l"))
                self.assertEqual(reader.read_one(), b"$1\r\nb\r\n")
                send_raw(sock, cmd("EXISTS", "l"))
                self.assertEqual(reader.read_one(), b":0\r\n")

    def test_lpop_rpop_on_missing_key(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("LPOP", "nosuch"))
                self.assertEqual(reader.read_one(), b"$-1\r\n")
                send_raw(sock, cmd("RPOP", "nosuch"))
                self.assertEqual(reader.read_one(), b"$-1\r\n")
                # with a count, missing key is a null array, not an empty one
                send_raw(sock, cmd("LPOP", "nosuch", "3"))
                self.assertEqual(reader.read_one(), b"*-1\r\n")

    def test_lpop_with_count(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("RPUSH", "l", "a", "b", "c", "d"))
                reader.read_one()
                send_raw(sock, cmd("LPOP", "l", "2"))
                self.assertEqual(read_array_bulks(reader), [b"a", b"b"])
                send_raw(sock, cmd("LRANGE", "l", "0", "-1"))
                self.assertEqual(read_array_bulks(reader), [b"c", b"d"])

    def test_lpop_count_zero_is_an_empty_array_not_an_error(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("RPUSH", "l", "a"))
                reader.read_one()
                send_raw(sock, cmd("LPOP", "l", "0"))
                self.assertEqual(reader.read_one(), b"*0\r\n")
                send_raw(sock, cmd("EXISTS", "l"))
                self.assertEqual(reader.read_one(), b":1\r\n")  # untouched

    def test_lpop_negative_count_is_an_error(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("RPUSH", "l", "a"))
                reader.read_one()
                send_raw(sock, cmd("LPOP", "l", "-1"))
                self.assertEqual(reader.read_one(), b"-ERR value is out of range, must be positive\r\n")

    def test_lpop_count_larger_than_length_returns_everything(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("RPUSH", "l", "a", "b"))
                reader.read_one()
                send_raw(sock, cmd("LPOP", "l", "100"))
                self.assertEqual(read_array_bulks(reader), [b"a", b"b"])
                send_raw(sock, cmd("EXISTS", "l"))
                self.assertEqual(reader.read_one(), b":0\r\n")

    def test_lrange_negative_indices(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("RPUSH", "l", "a", "b", "c", "d", "e"))
                reader.read_one()
                send_raw(sock, cmd("LRANGE", "l", "-3", "-1"))
                self.assertEqual(read_array_bulks(reader), [b"c", b"d", b"e"])
                send_raw(sock, cmd("LRANGE", "l", "-100", "100"))
                self.assertEqual(read_array_bulks(reader), [b"a", b"b", b"c", b"d", b"e"])

    def test_lrange_start_greater_than_stop_is_empty(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("RPUSH", "l", "a", "b", "c"))
                reader.read_one()
                send_raw(sock, cmd("LRANGE", "l", "2", "1"))
                self.assertEqual(reader.read_one(), b"*0\r\n")
                send_raw(sock, cmd("LRANGE", "l", "5", "10"))
                self.assertEqual(reader.read_one(), b"*0\r\n")
                send_raw(sock, cmd("LRANGE", "nosuch", "0", "-1"))
                self.assertEqual(reader.read_one(), b"*0\r\n")

    def test_lindex(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("RPUSH", "l", "a", "b", "c"))
                reader.read_one()
                send_raw(sock, cmd("LINDEX", "l", "0"))
                self.assertEqual(reader.read_one(), b"$1\r\na\r\n")
                send_raw(sock, cmd("LINDEX", "l", "-1"))
                self.assertEqual(reader.read_one(), b"$1\r\nc\r\n")
                send_raw(sock, cmd("LINDEX", "l", "99"))
                self.assertEqual(reader.read_one(), b"$-1\r\n")
                send_raw(sock, cmd("LINDEX", "l", "-99"))
                self.assertEqual(reader.read_one(), b"$-1\r\n")
                send_raw(sock, cmd("LINDEX", "nosuch", "0"))
                self.assertEqual(reader.read_one(), b"$-1\r\n")

    def test_wrongtype_both_ways(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SET", "s", "v"))
                reader.read_one()
                send_raw(sock, cmd("LPUSH", "s", "a"))
                self.assertEqual(reader.read_one(), b"-WRONGTYPE Operation against a key holding the wrong kind of value\r\n")
                send_raw(sock, cmd("LLEN", "s"))
                self.assertEqual(reader.read_one(), b"-WRONGTYPE Operation against a key holding the wrong kind of value\r\n")

                send_raw(sock, cmd("RPUSH", "l", "a"))
                reader.read_one()
                send_raw(sock, cmd("GET", "l"))
                self.assertEqual(reader.read_one(), b"-WRONGTYPE Operation against a key holding the wrong kind of value\r\n")

    def test_type_reports_list(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("RPUSH", "l", "a"))
                reader.read_one()
                send_raw(sock, cmd("TYPE", "l"))
                self.assertEqual(reader.read_one(), b"+list\r\n")

    def test_100k_pushes_then_pops(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                n = 100_000
                pipeline = b"".join(cmd("RPUSH", "big", str(i)) for i in range(n))
                send_raw(sock, pipeline)
                for i in range(1, n + 1):
                    self.assertEqual(reader.read_one(), f":{i}\r\n".encode())

                send_raw(sock, cmd("LLEN", "big"))
                self.assertEqual(reader.read_one(), f":{n}\r\n".encode())

                pop_pipeline = b"".join(cmd("LPOP", "big") for _ in range(n))
                send_raw(sock, pop_pipeline)
                for i in range(n):
                    expected = str(i).encode()
                    self.assertEqual(reader.read_one(), f"${len(expected)}\r\n".encode() + expected + b"\r\n")

                send_raw(sock, cmd("EXISTS", "big"))
                self.assertEqual(reader.read_one(), b":0\r\n")

    def test_server_stays_alive(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("PING"))
                self.assertEqual(reader.read_one(), b"+PONG\r\n")
            self.assertIsNone(server.process.poll())


if __name__ == "__main__":
    unittest.main()
