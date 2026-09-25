"""Replays a deterministic ~2000-command script across every type against
both gredis and local Valkey (/usr/bin/redis-server) and compares replies.

Known intentional differences (excluded from the script, not bugs):
- stub commands (COMMAND, CONFIG GET, INFO, HELLO): gredis returns fixed
  stubs, Valkey returns real data -- not comparable.
- SPOP: return value is nondeterministic in both servers, can't compare
  exactly (SREM/SCARD after it are still exercised).
- TTL/PTTL countdown: wall-clock dependent, flaky to compare; EXPIRE/PERSIST
  0/1 results are deterministic and still checked. Semantics are covered
  separately in test_ttl.py.
- ZADD/ZINCRBY scores kept to integers/halves so float formatting matches
  byte for byte.
"""

import random
import socket
import subprocess
import time
import unittest

from helpers import RespReader, cmd, pick_free_port, send_raw, start_server

VALKEY_BINARY = "/usr/bin/redis-server"


class RespError(str):
    """A decoded RESP error reply's message, kept as a distinct type so
    it can never compare equal to an ordinary string/bulk reply that
    happens to contain the same bytes."""


def decode(buf: bytes, pos: int):
    prefix = buf[pos:pos + 1]
    line_end = buf.index(b"\r\n", pos)
    line = buf[pos + 1:line_end]
    if prefix == b"+":
        return line, line_end + 2
    if prefix == b"-":
        return RespError(line.decode("utf-8", "replace")), line_end + 2
    if prefix == b":":
        return int(line), line_end + 2
    if prefix == b"$":
        n = int(line)
        if n == -1:
            return None, line_end + 2
        start = line_end + 2
        return buf[start:start + n], start + n + 2
    if prefix == b"*":
        n = int(line)
        if n == -1:
            return None, line_end + 2
        pos = line_end + 2
        items = []
        for _ in range(n):
            value, pos = decode(buf, pos)
            items.append(value)
        return items, pos
    raise ValueError(f"unexpected RESP type byte: {prefix!r} at {pos}")


def normalize(cmd_name: str, value):
    """Sorts unordered replies before comparing; reshapes HGETALL's flat
    pair list into a dict first (sorting it flat would scramble pairing)."""
    name = cmd_name.upper()
    if name in ("SMEMBERS", "SINTER", "HKEYS", "HVALS") and isinstance(value, list):
        return sorted(value)
    if name == "HGETALL" and isinstance(value, list):
        return dict(zip(value[0::2], value[1::2]))
    return value


def run_batch(sock: socket.socket, commands) -> list:
    """Pipelines every command in one send, reads that many replies back in order."""
    payload = b"".join(cmd(*c) for c in commands)
    send_raw(sock, payload)
    reader = RespReader(sock)
    decoded = []
    for _ in commands:
        raw = reader.read_one()
        value, _ = decode(raw, 0)
        decoded.append(value)
    return decoded


def build_script(seed: int = 20260921) -> list:
    """Builds a deterministic ~2000-command script covering every type
    plus key/TTL commands (see module docstring for exclusions)."""
    rng = random.Random(seed)
    commands = []

    # strings
    string_keys = [f"d:str:{i}" for i in range(40)]
    for key in string_keys:
        commands.append(("SET", key, f"v{rng.randint(0, 10_000)}"))
        commands.append(("GET", key))
        commands.append(("APPEND", key, "-suffix"))
        commands.append(("STRLEN", key))
        commands.append(("SET", key, str(rng.randint(-1000, 1000))))
        commands.append(("INCR", key))
        commands.append(("INCRBY", key, str(rng.randint(-50, 50))))
        commands.append(("DECR", key))
        commands.append(("GET", key))
    commands.append(("MSET", *[x for k in string_keys[:10] for x in (k, "msetval")]))
    commands.append(("MGET", *string_keys[:10], "d:str:nosuch"))

    # hashes
    hash_keys = [f"d:hash:{i}" for i in range(30)]
    for key in hash_keys:
        fields = [f"f{j}" for j in range(5)]
        for f in fields:
            commands.append(("HSET", key, f, f"val-{rng.randint(0, 1000)}"))
        commands.append(("HGET", key, fields[0]))
        commands.append(("HMGET", key, fields[0], fields[1], "nosuch"))
        commands.append(("HLEN", key))
        commands.append(("HEXISTS", key, fields[0]))
        commands.append(("HEXISTS", key, "nosuch"))
        commands.append(("HGETALL", key))
        commands.append(("HKEYS", key))
        commands.append(("HVALS", key))
        commands.append(("HSET", key, "counter", "10"))
        commands.append(("HINCRBY", key, "counter", str(rng.randint(-5, 5))))
        commands.append(("HDEL", key, fields[-1]))
        commands.append(("HLEN", key))

    # lists
    list_keys = [f"d:list:{i}" for i in range(30)]
    for key in list_keys:
        items = [f"i{j}" for j in range(8)]
        commands.append(("RPUSH", key, *items[:4]))
        commands.append(("LPUSH", key, *items[4:]))
        commands.append(("LLEN", key))
        commands.append(("LRANGE", key, "0", "-1"))
        commands.append(("LINDEX", key, "0"))
        commands.append(("LINDEX", key, "-1"))
        commands.append(("LPOP", key, "2"))
        commands.append(("RPOP", key, "2"))
        commands.append(("LRANGE", key, "0", "-1"))

    # sets
    set_keys = [f"d:set:{i}" for i in range(30)]
    for idx, key in enumerate(set_keys):
        members = [f"m{j}" for j in range(6)]
        commands.append(("SADD", key, *members))
        commands.append(("SCARD", key))
        commands.append(("SISMEMBER", key, members[0]))
        commands.append(("SISMEMBER", key, "nosuch"))
        commands.append(("SMEMBERS", key))
        commands.append(("SREM", key, members[0], "nosuch"))
        commands.append(("SCARD", key))
        if idx > 0:
            commands.append(("SINTER", key, set_keys[idx - 1]))

    # sorted sets
    zset_keys = [f"d:zset:{i}" for i in range(30)]
    for key in zset_keys:
        members = [(f"m{j}", str(rng.choice([0, 1, 2, 3, 4, 5]) + rng.choice([0, 0.5]))) for j in range(6)]
        flat = [x for pair in members for x in (pair[1], pair[0])]
        commands.append(("ZADD", key, *flat))
        commands.append(("ZCARD", key))
        commands.append(("ZSCORE", key, members[0][0]))
        commands.append(("ZRANK", key, members[0][0]))
        commands.append(("ZREVRANK", key, members[0][0]))
        commands.append(("ZRANGE", key, "0", "-1", "WITHSCORES"))
        commands.append(("ZREVRANGE", key, "0", "-1"))
        commands.append(("ZCOUNT", key, "-inf", "+inf"))
        commands.append(("ZRANGEBYSCORE", key, "1", "4", "WITHSCORES"))
        commands.append(("ZINCRBY", key, "1", members[0][0]))
        commands.append(("ZREM", key, members[-1][0]))
        commands.append(("ZPOPMIN", key))
        commands.append(("ZCARD", key))

    # keys / ttl
    for i in range(20):
        key = f"d:ttl:{i}"
        commands.append(("SET", key, "v"))
        commands.append(("EXPIRE", key, "100"))
        commands.append(("TYPE", key))
        commands.append(("PERSIST", key))
        commands.append(("EXISTS", key))
    commands.append(("DEL", *[f"d:ttl:{i}" for i in range(20)]))
    commands.append(("EXISTS", *[f"d:ttl:{i}" for i in range(20)]))

    return commands


def start_valkey(port: int) -> subprocess.Popen:
    proc = subprocess.Popen(
        [
            VALKEY_BINARY, "--port", str(port), "--bind", "127.0.0.1",
            "--save", "", "--appendonly", "no", "--protected-mode", "no",
            "--daemonize", "no", "--logfile", "/dev/null",
        ],
        stdout=subprocess.DEVNULL,
        stderr=subprocess.DEVNULL,
    )
    deadline = time.monotonic() + 5.0
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            raise RuntimeError("valkey-server exited before becoming ready")
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2) as sock:
                sock.settimeout(0.2)
                sock.sendall(cmd("PING"))
                if sock.recv(64) == b"+PONG\r\n":
                    return proc
        except OSError:
            time.sleep(0.05)
    proc.kill()
    raise RuntimeError("valkey-server never became ready")


class DifferentialTest(unittest.TestCase):
    def test_gredis_matches_valkey_across_all_types(self):
        script = build_script()
        self.assertGreater(len(script), 1500, "script should be roughly 2000 commands")

        valkey_port = pick_free_port()
        valkey_proc = start_valkey(valkey_port)
        try:
            with start_server() as gredis_server:
                with gredis_server.connect() as gsock, socket.create_connection(
                    ("127.0.0.1", valkey_port), timeout=5.0
                ) as vsock:
                    gredis_replies = run_batch(gsock, script)
                    valkey_replies = run_batch(vsock, script)

            self.assertEqual(len(gredis_replies), len(valkey_replies))

            mismatches = []
            for i, (command, g, v) in enumerate(zip(script, gredis_replies, valkey_replies)):
                name = command[0]
                g_norm = normalize(name, g)
                v_norm = normalize(name, v)
                if g_norm != v_norm:
                    mismatches.append((i, command, g_norm, v_norm))

            if mismatches:
                lines = [f"{len(mismatches)} mismatch(es) out of {len(script)} commands:"]
                for i, command, g_norm, v_norm in mismatches[:20]:
                    lines.append(f"  [{i}] {command} -> gredis={g_norm!r} valkey={v_norm!r}")
                self.fail("\n".join(lines))
        finally:
            valkey_proc.terminate()
            try:
                valkey_proc.wait(timeout=5.0)
            except subprocess.TimeoutExpired:
                valkey_proc.kill()
                valkey_proc.wait(timeout=5.0)


if __name__ == "__main__":
    unittest.main()
