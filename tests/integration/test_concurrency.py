"""Correctness under real concurrent clients (each thread = its own TCP
connection). Worker threads can't call unittest assertions directly, so
they append to a shared `errors` list instead and the test checks it's
empty after joining."""

import random
import shutil
import socket
import subprocess
import threading
import time
import unittest

from helpers import RespReader, cmd, send_raw, start_server


def dbsize(reader, sock) -> int:
    send_raw(sock, cmd("DBSIZE"))
    reply = reader.read_one()
    assert reply.startswith(b":")
    return int(reply[1:-2])


def model_client_worker(host, port, client_id, num_ops, seed, errors):
    """Runs random string/counter ops under this client's own key prefix,
    checking each reply against a local Python model."""
    try:
        rng = random.Random(seed)
        prefix = f"conc:{client_id}:"
        string_keys = [f"{prefix}s{j}" for j in range(5)]
        counter_keys = [f"{prefix}c{j}" for j in range(3)]
        model_strings = {}
        model_counters = {}

        with socket.create_connection((host, port), timeout=10.0) as sock:
            reader = RespReader(sock)
            for op_index in range(num_ops):
                choice = rng.randint(0, 6)
                if choice == 0:
                    key = rng.choice(string_keys)
                    value = f"v{rng.randint(0, 1_000_000)}"
                    send_raw(sock, cmd("SET", key, value))
                    expected = b"+OK\r\n"
                    model_strings[key] = value
                elif choice == 1:
                    key = rng.choice(string_keys)
                    send_raw(sock, cmd("GET", key))
                    if key in model_strings:
                        v = model_strings[key].encode()
                        expected = f"${len(v)}\r\n".encode() + v + b"\r\n"
                    else:
                        expected = b"$-1\r\n"
                elif choice == 2:
                    key = rng.choice(string_keys)
                    send_raw(sock, cmd("APPEND", key, "-x"))
                    new_val = model_strings.get(key, "") + "-x"
                    model_strings[key] = new_val
                    expected = f":{len(new_val)}\r\n".encode()
                elif choice == 3:
                    key = rng.choice(string_keys)
                    send_raw(sock, cmd("STRLEN", key))
                    expected = f":{len(model_strings.get(key, ''))}\r\n".encode()
                elif choice == 4:
                    key = rng.choice(string_keys)
                    existed = key in model_strings
                    send_raw(sock, cmd("DEL", key))
                    model_strings.pop(key, None)
                    expected = f":{1 if existed else 0}\r\n".encode()
                elif choice == 5:
                    key = rng.choice(string_keys)
                    send_raw(sock, cmd("EXISTS", key))
                    expected = f":{1 if key in model_strings else 0}\r\n".encode()
                else:
                    key = rng.choice(counter_keys)
                    delta = rng.choice([1, -1, 5, -5])
                    if delta == 1:
                        send_raw(sock, cmd("INCR", key))
                    elif delta == -1:
                        send_raw(sock, cmd("DECR", key))
                    else:
                        send_raw(sock, cmd("INCRBY", key, str(delta)))
                    model_counters[key] = model_counters.get(key, 0) + delta
                    expected = f":{model_counters[key]}\r\n".encode()

                actual = reader.read_one()
                if actual != expected:
                    errors.append(f"client {client_id} op {op_index}: expected {expected!r}, got {actual!r}")
                    return
    except Exception as e:  # noqa: BLE001 -- reported via `errors`, this runs on a worker thread
        errors.append(f"client {client_id} raised {e!r}")


def incr_worker(host, port, key, count, errors):
    try:
        with socket.create_connection((host, port), timeout=10.0) as sock:
            reader = RespReader(sock)
            for _ in range(count):
                send_raw(sock, cmd("INCR", key))
                reply = reader.read_one()
                if not reply.startswith(b":"):
                    errors.append(f"unexpected INCR reply: {reply!r}")
                    return
    except Exception as e:  # noqa: BLE001
        errors.append(repr(e))


def pipelined_worker(host, port, client_id, errors, disconnect_mid: bool):
    n = 50
    payload = b"".join(cmd("SET", f"pipe:{client_id}:{i}", "v") for i in range(n))
    try:
        sock = socket.create_connection((host, port), timeout=5.0)
        try:
            if disconnect_mid:
                send_raw(sock, payload[: len(payload) // 2])
                return
            send_raw(sock, payload)
            reader = RespReader(sock)
            for _ in range(n):
                reply = reader.read_one()
                if reply != b"+OK\r\n":
                    errors.append(f"client {client_id}: unexpected reply {reply!r}")
                    return
        finally:
            sock.close()
    except Exception as e:  # noqa: BLE001
        if not disconnect_mid:
            errors.append(f"client {client_id} raised {e!r}")


def unpipelined_worker(host, port, client_id, errors):
    try:
        with socket.create_connection((host, port), timeout=5.0) as sock:
            reader = RespReader(sock)
            for i in range(50):
                send_raw(sock, cmd("SET", f"solo:{client_id}:{i}", "v"))
                reply = reader.read_one()
                if reply != b"+OK\r\n":
                    errors.append(f"client {client_id}: unexpected reply {reply!r}")
                    return
    except Exception as e:  # noqa: BLE001
        errors.append(f"client {client_id} raised {e!r}")


def ttl_writer(host, port, client_id, duration_s, errors):
    try:
        with socket.create_connection((host, port), timeout=5.0) as sock:
            reader = RespReader(sock)
            deadline = time.monotonic() + duration_s
            i = 0
            while time.monotonic() < deadline:
                key = f"ttlw:{client_id}:{i}"
                send_raw(sock, cmd("SET", key, "v", "PX", "100"))
                reply = reader.read_one()
                if reply != b"+OK\r\n":
                    errors.append(f"client {client_id}: SET failed: {reply!r}")
                    return
                send_raw(sock, cmd("GET", key))
                reply = reader.read_one()
                if not reply.startswith(b"$"):
                    errors.append(f"client {client_id}: GET failed: {reply!r}")
                    return
                i += 1
    except Exception as e:  # noqa: BLE001
        errors.append(f"client {client_id} raised {e!r}")


def run_all(threads, join_timeout: float):
    start = time.monotonic()
    for t in threads:
        t.start()
    for t in threads:
        t.join(timeout=join_timeout)
    return time.monotonic() - start


class ConcurrencyTest(unittest.TestCase):
    def test_200_clients_each_see_exactly_their_own_model(self):
        with start_server() as server:
            errors = []
            num_clients = 200
            ops_per_client = 1000
            threads = [
                threading.Thread(
                    target=model_client_worker,
                    args=("127.0.0.1", server.port, i, ops_per_client, 1000 + i, errors),
                )
                for i in range(num_clients)
            ]
            elapsed = run_all(threads, join_timeout=120.0)
            print(f"\n  {num_clients} clients x {ops_per_client} ops in {elapsed:.2f}s")

            for t in threads:
                self.assertFalse(t.is_alive(), "a client thread did not finish in time")
            self.assertEqual(errors, [])
            self.assertIsNone(server.process.poll())

    def test_shared_counter_incr_is_atomic_under_concurrency(self):
        with start_server() as server:
            errors = []
            num_clients = 100
            incrs_per_client = 1000
            threads = [
                threading.Thread(
                    target=incr_worker, args=("127.0.0.1", server.port, "shared:counter", incrs_per_client, errors)
                )
                for _ in range(num_clients)
            ]
            run_all(threads, join_timeout=60.0)

            for t in threads:
                self.assertFalse(t.is_alive())
            self.assertEqual(errors, [])

            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("GET", "shared:counter"))
                expected = str(num_clients * incrs_per_client).encode()
                self.assertEqual(reader.read_one(), f"${len(expected)}\r\n".encode() + expected + b"\r\n")

    def test_mixed_pipelined_unpipelined_and_disconnecting_clients(self):
        with start_server() as server:
            errors = []
            threads = []
            for i in range(10):
                threads.append(
                    threading.Thread(target=pipelined_worker, args=("127.0.0.1", server.port, f"p{i}", errors, False))
                )
            for i in range(10):
                threads.append(threading.Thread(target=unpipelined_worker, args=("127.0.0.1", server.port, f"u{i}", errors)))
            for i in range(10):
                threads.append(
                    threading.Thread(target=pipelined_worker, args=("127.0.0.1", server.port, f"d{i}", errors, True))
                )
            run_all(threads, join_timeout=30.0)

            for t in threads:
                self.assertFalse(t.is_alive())
            self.assertEqual(errors, [])

            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("PING"))
                self.assertEqual(reader.read_one(), b"+PONG\r\n")
            self.assertIsNone(server.process.poll())

    def test_concurrent_ttl_workload_converges_with_no_errors(self):
        with start_server() as server:
            errors = []
            threads = [
                threading.Thread(target=ttl_writer, args=("127.0.0.1", server.port, i, 1.5, errors)) for i in range(20)
            ]
            run_all(threads, join_timeout=15.0)

            for t in threads:
                self.assertFalse(t.is_alive())
            self.assertEqual(errors, [])

            with server.connect() as sock:
                reader = RespReader(sock)
                deadline = time.monotonic() + 3.0
                size = dbsize(reader, sock)
                while size > 0 and time.monotonic() < deadline:
                    time.sleep(0.05)
                    size = dbsize(reader, sock)
                self.assertEqual(size, 0, "DBSIZE never converged to 0 after the concurrent TTL workload")

    def test_server_stays_alive(self):
        with start_server() as server:
            with server.connect() as sock:
                reader = RespReader(sock)
                send_raw(sock, cmd("PING"))
                self.assertEqual(reader.read_one(), b"+PONG\r\n")
            self.assertIsNone(server.process.poll())


class RedisBenchmarkCorrectnessTest(unittest.TestCase):
    """Correctness smoke check only -- no pinning, no trial discipline,
    numbers here are never recorded as a benchmark result."""

    @unittest.skipUnless(shutil.which("redis-benchmark"), "redis-benchmark not installed")
    def test_full_command_set_completes_with_zero_errors(self):
        with start_server() as server:
            result = subprocess.run(
                [
                    "redis-benchmark", "-p", str(server.port),
                    "-t", "set,get,incr,lpush,rpush,lpop,rpop,sadd,hset,spop,zadd,zpopmin",
                    "-n", "200000", "-c", "50", "-q",
                ],
                capture_output=True,
                text=True,
                timeout=180.0,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertNotIn("error", result.stdout.lower(), result.stdout)
            self.assertNotIn("error", result.stderr.lower(), result.stderr)
            for command in ("SET", "GET", "INCR", "LPUSH", "RPUSH", "LPOP", "RPOP",
                             "SADD", "HSET", "SPOP", "ZADD", "ZPOPMIN"):
                self.assertIn(f"{command}: ", result.stdout, f"no summary line for {command}")
            self.assertIsNone(server.process.poll())


if __name__ == "__main__":
    unittest.main()
