"""Shared helpers for gredis integration tests."""

import contextlib
import os
import resource
import socket
import subprocess
import time
from pathlib import Path
from typing import Optional, Union

REPO_ROOT = Path(__file__).resolve().parents[2]

_SANITIZER_MARKERS = ("AddressSanitizer", "ThreadSanitizer", "runtime error:")


def find_server_binary() -> Path:
    """Locate the built gredis-server binary.

    Honors GREDIS_SERVER_BIN if set, otherwise checks the usual build dirs.
    """
    env_path = os.environ.get("GREDIS_SERVER_BIN")
    if env_path:
        p = Path(env_path)
        if p.is_file():
            return p
        raise FileNotFoundError(f"GREDIS_SERVER_BIN={env_path} does not exist")

    candidates = [
        REPO_ROOT / "build" / "gredis-server",
        REPO_ROOT / "build-release" / "gredis-server",
        REPO_ROOT / "build-tsan" / "gredis-server",
    ]
    for c in candidates:
        if c.is_file():
            return c

    raise FileNotFoundError(
        "gredis-server binary not found. Build it first (see README.md), "
        "or set GREDIS_SERVER_BIN to its path. Looked in: "
        + ", ".join(str(c) for c in candidates)
    )


def run_server_once(*args: str, timeout: float = 5.0) -> subprocess.CompletedProcess:
    """Run gredis-server to completion and return the result.

    Only for invocations that exit on their own (--help, bad flags, bind
    failure). Use start_server() for a server meant to keep running.
    """
    binary = find_server_binary()
    return subprocess.run(
        [str(binary), *args],
        capture_output=True,
        text=True,
        timeout=timeout,
    )


def pick_free_port() -> int:
    """Binds to port 0 to let the OS pick, then releases it. Small race
    before the server actually binds it, but good enough for tests.
    """
    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


def _sanitizer_report(text: str) -> bool:
    return any(marker in text for marker in _SANITIZER_MARKERS)


def cmd(*args: Union[str, bytes]) -> bytes:
    """Encodes a command as a RESP2 array of bulk strings. Binary-safe:
    a bytes argument is used as-is, so tests can embed NUL/CR/LF."""
    encoded = [a.encode() if isinstance(a, str) else a for a in args]
    parts = [f"*{len(encoded)}\r\n".encode()]
    for a in encoded:
        parts.append(f"${len(a)}\r\n".encode() + a + b"\r\n")
    return b"".join(parts)


def send_raw(sock: socket.socket, data: bytes) -> None:
    """Wrapper around sendall(), named for tests that send deliberately
    malformed/non-command bytes."""
    sock.sendall(data)


class RespReader:
    """Minimal RESP2 reply reader. Buffers leftover bytes between calls
    so read_one() can read a pipelined batch one reply at a time."""

    def __init__(self, sock: socket.socket):
        self.sock = sock
        self.buf = bytearray()

    def _fill_until(self, n: int) -> None:
        while len(self.buf) < n:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("connection closed while reading a reply")
            self.buf.extend(chunk)

    def _read_line(self) -> bytes:
        while True:
            idx = self.buf.find(b"\r\n")
            if idx != -1:
                line = bytes(self.buf[: idx + 2])
                del self.buf[: idx + 2]
                return line
            chunk = self.sock.recv(65536)
            if not chunk:
                raise ConnectionError("connection closed while reading a reply line")
            self.buf.extend(chunk)

    def read_one(self) -> bytes:
        """Reads one RESP value, returns its raw wire bytes (type prefix
        and all) so tests can assert on exact framing, not just the value."""
        line = self._read_line()
        prefix = line[0:1]
        if prefix in (b"+", b"-", b":"):
            return line
        if prefix == b"$":
            n = int(line[1:-2])
            if n == -1:
                return line
            self._fill_until(n + 2)
            payload = bytes(self.buf[: n + 2])
            del self.buf[: n + 2]
            return line + payload
        if prefix == b"*":
            n = int(line[1:-2])
            if n == -1:
                return line
            out = line
            for _ in range(n):
                out += self.read_one()
            return out
        raise ValueError(f"unexpected RESP type byte: {prefix!r}")


def read_reply(sock: socket.socket) -> bytes:
    """Reads one RESP2 reply. Makes a fresh RespReader each call, so it
    discards leftover buffered bytes -- use RespReader directly for more
    than one reply off the same socket."""
    return RespReader(sock).read_one()


class ServerProcess:
    """A running gredis-server subprocess."""

    def __init__(self, process: subprocess.Popen, port: int):
        self.process = process
        self.port = port
        self.stdout = None
        self.stderr = None
        self.was_force_killed = False

    def connect(self, timeout: float = 5.0) -> socket.socket:
        return socket.create_connection(("127.0.0.1", self.port), timeout=timeout)

    def stop(self, timeout: float = 5.0) -> None:
        """Sends SIGTERM, falls back to SIGKILL if it doesn't exit in time
        (tracked in was_force_killed so assert_clean_shutdown knows not to
        expect a real exit code). Safe to call more than once."""
        self.was_force_killed = False
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=timeout)
            except subprocess.TimeoutExpired:
                self.was_force_killed = True
                self.process.kill()
                self.process.wait(timeout=timeout)
        if self.stdout is None:
            try:
                self.stdout, self.stderr = self.process.communicate(timeout=timeout)
            except subprocess.TimeoutExpired:
                self.stdout, self.stderr = "", ""

    def assert_clean_shutdown(self) -> None:
        """Raises if stderr shows a sanitizer report, or a graceful
        SIGTERM didn't exit 0."""
        text = self.stderr or ""
        if _sanitizer_report(text):
            raise AssertionError(f"sanitizer report from gredis-server:\n{text}")
        if not self.was_force_killed and self.process.returncode != 0:
            raise AssertionError(
                f"gredis-server did not exit 0 after SIGTERM (exit code {self.process.returncode})"
            )


@contextlib.contextmanager
def start_server(*extra_args: str, port: Optional[int] = None, ready_timeout: float = 5.0,
                  rlimit_nofile: Optional[int] = None):
    """Starts gredis-server, waits for a real PING/+PONG, yields a
    ServerProcess. Always stops it on the way out and checks stderr for
    sanitizer reports.

    `rlimit_nofile`, if given, applies RLIMIT_NOFILE to just the server
    child (via preexec_fn), not the test runner itself.
    """
    binary = find_server_binary()
    if port is None:
        port = pick_free_port()

    env = os.environ.copy()
    env.setdefault("ASAN_OPTIONS", "detect_leaks=1:abort_on_error=1")
    env.setdefault("UBSAN_OPTIONS", "print_stacktrace=1")

    preexec_fn = None
    if rlimit_nofile is not None:
        def preexec_fn():
            resource.setrlimit(resource.RLIMIT_NOFILE, (rlimit_nofile, rlimit_nofile))

    proc = subprocess.Popen(
        [str(binary), "--port", str(port), *extra_args],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        env=env,
        preexec_fn=preexec_fn,
    )
    server = ServerProcess(proc, port)

    deadline = time.monotonic() + ready_timeout
    ready = False
    while time.monotonic() < deadline:
        if proc.poll() is not None:
            break  # died before becoming ready; report below
        try:
            with socket.create_connection(("127.0.0.1", port), timeout=0.2) as sock:
                sock.settimeout(0.2)
                sock.sendall(cmd("PING"))
                if sock.recv(64) == b"+PONG\r\n":
                    ready = True
                    break
        except OSError:
            time.sleep(0.05)

    if not ready:
        server.stop()
        raise RuntimeError(
            f"gredis-server never became ready on port {port} "
            f"(exit code {proc.returncode})\nstdout: {server.stdout}\nstderr: {server.stderr}"
        )

    try:
        yield server
    finally:
        server.stop()
        server.assert_clean_shutdown()
