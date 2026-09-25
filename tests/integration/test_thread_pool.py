"""UNLINK and FLUSHALL ASYNC free large detached values without blocking
the loop thread. The DEL-vs-UNLINK latency numbers are just printed
(observation only, not asserted on)."""

import time
import unittest

from helpers import RespReader, cmd, send_raw, start_server


def build_large_set(sock, reader, key: str, n: int, chunk: int = 10_000) -> None:
    """Populates `key` via pipelined SADD calls of `chunk` members each --
    avoids one huge multi-bulk command and n slow single-member calls."""
    assert n % chunk == 0
    for start in range(0, n, chunk):
        members = [f"m{i}" for i in range(start, start + chunk)]
        send_raw(sock, cmd("SADD", key, *members))
        reader.read_one()


def ping_latency_ms(sock, reader) -> float:
    start = time.monotonic()
    send_raw(sock, cmd("PING"))
    reply = reader.read_one()
    elapsed_ms = (time.monotonic() - start) * 1000
    assert reply == b"+PONG\r\n", reply
    return elapsed_ms


class ThreadPoolTest(unittest.TestCase):
    def test_unlink_of_a_1m_member_set_keeps_the_server_responsive(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                build_large_set(sock, reader, "big", 1_000_000)

                send_raw(sock, cmd("UNLINK", "big"))
                self.assertEqual(reader.read_one(), b":1\r\n")

                # reply above only confirms detachment -- this checks that
                # freeing it on a worker thread doesn't block the loop
                latencies = [ping_latency_ms(sock, reader) for _ in range(20)]
                print(f"\n  UNLINK(1M-member set) -> immediate PING latencies (ms): {latencies}")
                self.assertLess(max(latencies), 200)

                send_raw(sock, cmd("EXISTS", "big"))
                self.assertEqual(reader.read_one(), b":0\r\n")

    def test_del_vs_unlink_of_equally_large_sets_observation(self):
        # just prints both numbers, not asserted on -- depends on machine allocator/cache behavior
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                build_large_set(sock, reader, "for_del", 1_000_000)
                build_large_set(sock, reader, "for_unlink", 1_000_000)

                del_start = time.monotonic()
                send_raw(sock, cmd("DEL", "for_del"))
                self.assertEqual(reader.read_one(), b":1\r\n")
                del_ms = (time.monotonic() - del_start) * 1000

                unlink_start = time.monotonic()
                send_raw(sock, cmd("UNLINK", "for_unlink"))
                self.assertEqual(reader.read_one(), b":1\r\n")
                unlink_ms = (time.monotonic() - unlink_start) * 1000

                print(f"\n  observation: DEL(1M-member set) reply latency = {del_ms:.2f} ms, "
                      f"UNLINK(1M-member set) reply latency = {unlink_ms:.2f} ms")

    def test_flushall_async_resets_dbsize_immediately_and_stays_responsive(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                build_large_set(sock, reader, "big", 1_000_000)
                send_raw(sock, cmd("SET", "other", "v"))
                reader.read_one()

                send_raw(sock, cmd("FLUSHALL", "ASYNC"))
                self.assertEqual(reader.read_one(), b"+OK\r\n")

                # keyspace reset is synchronous, only the freeing happens in the background
                send_raw(sock, cmd("DBSIZE"))
                self.assertEqual(reader.read_one(), b":0\r\n")

                latencies = [ping_latency_ms(sock, reader) for _ in range(20)]
                print(f"\n  FLUSHALL ASYNC(1M keys) -> immediate PING latencies (ms): {latencies}")
                self.assertLess(max(latencies), 200)

    # thread-count check lives in test_dispatch.py instead -- TSan adds its
    # own instrumentation thread, which would break an exact count here

    def test_server_stays_alive(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("PING"))
                self.assertEqual(reader.read_one(), b"+PONG\r\n")
            self.assertIsNone(server.process.poll())


if __name__ == "__main__":
    unittest.main()
