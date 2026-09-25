"""SAVE/BGSAVE/LASTSAVE and startup loading, through real server processes
and real files on disk."""

import os
import shutil
import tempfile
import time
import unittest

from helpers import RespReader, cmd, pick_free_port, run_server_once, send_raw, start_server


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


class PersistenceTest(unittest.TestCase):
    def setUp(self):
        self.tmpdir = tempfile.mkdtemp(prefix="gredis_snap_")

    def tearDown(self):
        # a permission test may leave the dir non-writable
        os.chmod(self.tmpdir, 0o755)
        shutil.rmtree(self.tmpdir, ignore_errors=True)

    def test_save_then_restart_restores_every_type_and_ttl(self):
        snap = os.path.join(self.tmpdir, "dump.grds")
        with start_server("--snapshot", snap) as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SET", "str", "hello"))
                reader.read_one()
                send_raw(sock, cmd("HSET", "h", "f1", "v1", "f2", "v2"))
                reader.read_one()
                send_raw(sock, cmd("RPUSH", "l", "a", "b", "c"))
                reader.read_one()
                send_raw(sock, cmd("SADD", "s", "m1", "m2"))
                reader.read_one()
                send_raw(sock, cmd("ZADD", "z", "1", "x", "2", "y"))
                reader.read_one()
                send_raw(sock, cmd("SET", "with_ttl", "v", "PX", "60000"))
                reader.read_one()

                send_raw(sock, cmd("SAVE"))
                self.assertEqual(reader.read_one(), b"+OK\r\n")

        self.assertTrue(os.path.exists(snap))
        self.assertFalse(os.path.exists(snap + ".tmp"))

        with start_server("--snapshot", snap) as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("DBSIZE"))
                self.assertEqual(reader.read_one(), b":6\r\n")

                send_raw(sock, cmd("GET", "str"))
                self.assertEqual(unwrap_bulk(reader.read_one()), b"hello")

                send_raw(sock, cmd("HGET", "h", "f1"))
                self.assertEqual(unwrap_bulk(reader.read_one()), b"v1")
                send_raw(sock, cmd("HGET", "h", "f2"))
                self.assertEqual(unwrap_bulk(reader.read_one()), b"v2")

                send_raw(sock, cmd("LRANGE", "l", "0", "-1"))
                items = [unwrap_bulk(m) for m in read_array(reader)]
                self.assertEqual(items, [b"a", b"b", b"c"])

                send_raw(sock, cmd("SMEMBERS", "s"))
                members = {unwrap_bulk(m) for m in read_array(reader)}
                self.assertEqual(members, {b"m1", b"m2"})

                send_raw(sock, cmd("ZRANGE", "z", "0", "-1", "WITHSCORES"))
                zitems = [unwrap_bulk(m) for m in read_array(reader)]
                self.assertEqual(zitems, [b"x", b"1", b"y", b"2"])

                send_raw(sock, cmd("PTTL", "with_ttl"))
                pttl_reply = reader.read_one()
                self.assertTrue(pttl_reply.startswith(b":"))
                remaining = int(pttl_reply[1:-2])
                self.assertTrue(0 < remaining <= 60_000, remaining)

    def test_zset_scores_and_ranks_survive_restart(self):
        snap = os.path.join(self.tmpdir, "dump.grds")
        with start_server("--snapshot", snap) as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("ZADD", "z", "1.5", "alice", "2.5", "bob"))
                reader.read_one()
                send_raw(sock, cmd("SAVE"))
                self.assertEqual(reader.read_one(), b"+OK\r\n")

        with start_server("--snapshot", snap) as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("ZSCORE", "z", "alice"))
                self.assertEqual(unwrap_bulk(reader.read_one()), b"1.5")
                send_raw(sock, cmd("ZRANK", "z", "bob"))
                self.assertEqual(reader.read_one(), b":1\r\n")

    def test_no_snapshot_file_yet_starts_empty(self):
        snap = os.path.join(self.tmpdir, "does_not_exist_yet.grds")
        with start_server("--snapshot", snap) as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("DBSIZE"))
                self.assertEqual(reader.read_one(), b":0\r\n")

    def test_save_without_a_configured_snapshot_path_is_an_error(self):
        with start_server() as server:  # no --snapshot at all
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SAVE"))
                reply = reader.read_one()
                self.assertTrue(reply.startswith(b"-ERR"), reply)

    def test_lastsave_updates_after_save(self):
        snap = os.path.join(self.tmpdir, "dump.grds")
        with start_server("--snapshot", snap) as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("LASTSAVE"))
                before_reply = reader.read_one()
                self.assertTrue(before_reply.startswith(b":"))

                send_raw(sock, cmd("SET", "k", "v"))
                reader.read_one()
                send_raw(sock, cmd("SAVE"))
                self.assertEqual(reader.read_one(), b"+OK\r\n")

                send_raw(sock, cmd("LASTSAVE"))
                after_reply = reader.read_one()
                self.assertTrue(after_reply.startswith(b":"))
                # same second is possible, just check it never goes backwards
                self.assertGreaterEqual(int(after_reply[1:-2]), int(before_reply[1:-2]))

    def test_bgsave_completes_and_updates_lastsave(self):
        snap = os.path.join(self.tmpdir, "dump.grds")
        with start_server("--snapshot", snap) as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SET", "k", "v"))
                reader.read_one()

                send_raw(sock, cmd("BGSAVE"))
                self.assertEqual(reader.read_one(), b"+Background saving started\r\n")

                deadline = time.monotonic() + 2.0
                saved = False
                while time.monotonic() < deadline:
                    if os.path.exists(snap):
                        saved = True
                        break
                    time.sleep(0.02)
                self.assertTrue(saved, "BGSAVE never produced the snapshot file")

        with start_server("--snapshot", snap) as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("GET", "k"))
                self.assertEqual(unwrap_bulk(reader.read_one()), b"v")

    def test_second_bgsave_while_one_is_running_is_an_error(self):
        snap = os.path.join(self.tmpdir, "dump.grds")
        with start_server("--snapshot", snap) as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                # large dataset so this BGSAVE is still running when the second one lands
                n = 500_000
                chunk = 10_000
                for start in range(0, n, chunk):
                    fields = []
                    for i in range(start, start + chunk):
                        fields.append(f"f{i}")
                        fields.append("v")
                    send_raw(sock, cmd("HSET", "big", *fields))
                    reader.read_one()

                send_raw(sock, cmd("BGSAVE"))
                self.assertEqual(reader.read_one(), b"+Background saving started\r\n")
                send_raw(sock, cmd("BGSAVE"))
                reply = reader.read_one()
                self.assertEqual(reply, b"-ERR Background save already in progress\r\n")

                # wait for the first BGSAVE to finish so shutdown isn't mid-write
                deadline = time.monotonic() + 5.0
                while time.monotonic() < deadline and not os.path.exists(snap):
                    time.sleep(0.02)

    def test_snapshot_write_failure_leaves_old_file_intact(self):
        snap = os.path.join(self.tmpdir, "dump.grds")

        # First, a normal SAVE while the directory is still writable.
        with start_server("--snapshot", snap) as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("SET", "original", "v"))
                reader.read_one()
                send_raw(sock, cmd("SAVE"))
                self.assertEqual(reader.read_one(), b"+OK\r\n")

        original_mtime = os.path.getmtime(snap)

        # read+execute only: existing files still readable, but nothing new can be created
        os.chmod(self.tmpdir, 0o555)
        try:
            with start_server("--snapshot", snap) as server:
                with server.connect() as sock:
                    reader = RespReader(sock)
                    send_raw(sock, cmd("SET", "should_not_persist", "v"))
                    reader.read_one()
                    send_raw(sock, cmd("SAVE"))
                    reply = reader.read_one()
                    self.assertTrue(reply.startswith(b"-ERR"), reply)
        finally:
            os.chmod(self.tmpdir, 0o755)

        self.assertEqual(os.path.getmtime(snap), original_mtime)
        self.assertFalse(os.path.exists(snap + ".tmp"))

        with start_server("--snapshot", snap) as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("GET", "original"))
                self.assertEqual(unwrap_bulk(reader.read_one()), b"v")
                send_raw(sock, cmd("EXISTS", "should_not_persist"))
                self.assertEqual(reader.read_one(), b":0\r\n")

    def test_corrupt_snapshot_refuses_to_start(self):
        snap = os.path.join(self.tmpdir, "dump.grds")
        with open(snap, "wb") as f:
            f.write(b"this is not a valid gredis snapshot file")

        result = run_server_once("--port", str(pick_free_port()), "--snapshot", snap, timeout=5.0)
        self.assertNotEqual(result.returncode, 0)

    def test_server_stays_alive(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("PING"))
                self.assertEqual(reader.read_one(), b"+PONG\r\n")
            self.assertIsNone(server.process.poll())


if __name__ == "__main__":
    unittest.main()
