#!/usr/bin/env python3
"""Measures PING round-trip latency on one connection while BGSAVE (issued
on a second connection) runs, to see the loop-side snapshot-encode stall
directly. Not part of run_suite.sh's matrix.

start_bgsave() runs encode_snapshot() synchronously before its own reply
is queued, so BGSAVE's own round-trip time on its connection IS the
loop-side encode time, and every other connection sees the same stall
since gredis is single-threaded end to end.

Requires an already-running, already-populated gredis-server (this
script only issues BGSAVE and PINGs -- populate it first, e.g. with
`redis-benchmark -t set -n 1000000 -r 1000000 --sequential -q`, and start
it with `--snapshot <path>` or BGSAVE will just return an error).

Usage: bgsave_stall.py --port 6380 --duration-sec 5 --bgsave-after-sec 1.0
Prints one CSV row per PING to stdout ("seq,t_since_start_sec,latency_ms")
and a summary (BGSAVE's own latency, the max PING latency) to stderr.
"""
import argparse
import socket
import sys
import threading
import time


def make_conn(host: str, port: int) -> socket.socket:
    s = socket.create_connection((host, port))
    s.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
    return s


def send_command(sock: socket.socket, *args: str) -> None:
    parts = [f"*{len(args)}\r\n".encode()]
    for a in args:
        b = a.encode()
        parts.append(f"${len(b)}\r\n".encode() + b + b"\r\n")
    sock.sendall(b"".join(parts))


def read_line(sock: socket.socket, buf: bytes) -> tuple[bytes, bytes]:
    while b"\r\n" not in buf:
        chunk = sock.recv(65536)
        if not chunk:
            raise ConnectionError("server closed connection")
        buf += chunk
    line, _, rest = buf.partition(b"\r\n")
    return line, rest


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=6380)
    ap.add_argument("--duration-sec", type=float, default=5.0,
                     help="wall-clock duration, not a fixed ping count (a fixed count risks "
                          "finishing before BGSAVE even fires)")
    ap.add_argument("--bgsave-after-sec", type=float, default=1.0,
                     help="delay before issuing BGSAVE, so a baseline is visible first")
    args = ap.parse_args()

    ping_conn = make_conn(args.host, args.port)
    bgsave_conn = make_conn(args.host, args.port)

    results: list[tuple[int, float, float]] = []
    bgsave_result: dict = {}
    start = time.perf_counter()

    def do_bgsave() -> None:
        time.sleep(args.bgsave_after_sec)
        t0 = time.perf_counter()
        send_command(bgsave_conn, "BGSAVE")
        line, _ = read_line(bgsave_conn, b"")
        t1 = time.perf_counter()
        bgsave_result["issued_at_sec"] = t0 - start
        bgsave_result["reply_latency_ms"] = (t1 - t0) * 1000
        bgsave_result["reply"] = line.decode(errors="replace")

    bgsave_thread = threading.Thread(target=do_bgsave)
    bgsave_thread.start()

    buf = b""
    seq = 0
    while time.perf_counter() - start < args.duration_sec:
        t0 = time.perf_counter()
        send_command(ping_conn, "PING")
        _, buf = read_line(ping_conn, buf)
        t1 = time.perf_counter()
        results.append((seq, t0 - start, (t1 - t0) * 1000))
        seq += 1

    bgsave_thread.join()

    print("seq,t_since_start_sec,latency_ms")
    for seq, t, lat in results:
        print(f"{seq},{t:.6f},{lat:.3f}")

    if "reply_latency_ms" not in bgsave_result:
        print("# BGSAVE thread never got a reply -- see stderr above", file=sys.stderr)
        return 1

    max_ping = max(results, key=lambda r: r[2])
    print(
        f"# BGSAVE issued at t={bgsave_result['issued_at_sec']:.3f}s, replied "
        f"'{bgsave_result['reply']}' after {bgsave_result['reply_latency_ms']:.3f} ms "
        "-- this IS the loop-side encode_snapshot() time (see server.cpp's "
        "start_bgsave()).",
        file=sys.stderr,
    )
    print(
        f"# Max PING latency: seq={max_ping[0]} t={max_ping[1]:.3f}s "
        f"latency={max_ping[2]:.3f} ms",
        file=sys.stderr,
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
