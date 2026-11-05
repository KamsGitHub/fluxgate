"""Fixtures that run the real fluxgate binary against scripted raw-socket peers."""

import json
import os
import socket
import subprocess
import threading
import time
from pathlib import Path

import pytest

BINARY = Path(os.environ.get("FLUXGATE_BIN", "build/debug/fluxgate"))
TIMEOUT = 15


class Proxy:
    def __init__(self, process, lines):
        self.process = process
        self.lines = lines
        self.port = self._wait_for_port()

    def _wait_for_port(self):
        deadline = time.monotonic() + TIMEOUT
        while time.monotonic() < deadline:
            for line in list(self.lines):
                if line.get("event") == "server.listening":
                    return line["port"]
            if self.process.poll() is not None:
                raise RuntimeError("fluxgate exited during startup")
            time.sleep(0.005)
        raise RuntimeError("fluxgate did not start")

    def events(self, name):
        return [line for line in self.lines if line.get("event") == name]

    def wait_for_access_logs(self, count):
        deadline = time.monotonic() + TIMEOUT
        while time.monotonic() < deadline:
            if len(self.events("access")) >= count:
                return self.events("access")
            time.sleep(0.005)
        raise AssertionError(f"saw {len(self.events('access'))} access logs, wanted {count}")

    def open_fds(self):
        return len(os.listdir(f"/proc/{self.process.pid}/fd"))

    def rss_kib(self):
        for line in Path(f"/proc/{self.process.pid}/status").read_text().splitlines():
            if line.startswith("VmRSS:"):
                return int(line.split()[1])
        raise AssertionError("no VmRSS")

    def connect(self):
        sock = socket.create_connection(("127.0.0.1", self.port), timeout=TIMEOUT)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        return sock


class Backend:
    """Accepts connections and runs `handler(conn)` for each, on its own thread."""

    def __init__(self, handler):
        self.handler = handler
        self.listener = socket.socket()
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(64)
        self.port = self.listener.getsockname()[1]
        self.requests = []
        self.connections = 0
        self._stop = False
        self._threads = []
        self._acceptor = threading.Thread(target=self._accept_loop, daemon=True)
        self._acceptor.start()

    def _accept_loop(self):
        self.listener.settimeout(0.05)
        while not self._stop:
            try:
                conn, _ = self.listener.accept()
            except TimeoutError:
                continue
            except OSError:
                return
            self.connections += 1
            conn.settimeout(TIMEOUT)
            thread = threading.Thread(target=self._run, args=(conn,), daemon=True)
            self._threads.append(thread)
            thread.start()

    def _run(self, conn):
        with conn:
            try:
                self.handler(self, conn)
            except OSError:
                pass

    def read_request(self, conn):
        """Reads one Content-Length framed request and records it."""
        data = b""
        while b"\r\n\r\n" not in data:
            chunk = conn.recv(65536)
            if not chunk:
                return None
            data += chunk
        head, _, body = data.partition(b"\r\n\r\n")
        length = 0
        for line in head.split(b"\r\n")[1:]:
            name, _, value = line.partition(b":")
            if name.lower() == b"content-length":
                length = int(value)
        while len(body) < length:
            chunk = conn.recv(65536)
            if not chunk:
                break
            body += chunk
        request = {"head": head.decode("latin-1"), "body": body}
        self.requests.append(request)
        return request

    def close(self):
        self._stop = True
        self._acceptor.join(timeout=2)
        self.listener.close()


def canned(response):
    """A handler that answers every request with fixed bytes."""

    def handler(backend, conn):
        if backend.read_request(conn) is not None:
            conn.sendall(response)

    return handler


OK = b"HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: keep-alive\r\n\r\nok"


@pytest.fixture
def make_backend():
    backends = []

    def make(handler=None):
        backend = Backend(handler or canned(OK))
        backends.append(backend)
        return backend

    yield make
    for backend in backends:
        backend.close()


def render_config(upstream_port, overrides):
    sections = {
        "listener": {"address": "127.0.0.1", "port": 0},
        "upstream": {"host": "127.0.0.1", "port": upstream_port},
        "timeouts": {"request_header_ms": 2000, "connect_ms": 1000,
                     "response_header_ms": 3000, "idle_ms": 3000, "exchange_ms": 10000},
    }
    for name, values in overrides.items():
        sections.setdefault(name, {}).update(values)
    return json.dumps(sections)  # JSON is valid YAML.


@pytest.fixture
def start_proxy(tmp_path):
    processes = []

    def start(upstream_port, **overrides):
        config = tmp_path / f"proxy{len(processes)}.yaml"
        config.write_text(render_config(upstream_port, overrides))
        process = subprocess.Popen([BINARY, "--config", str(config)],
                                   stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        processes.append(process)
        lines = []

        def collect():
            for raw in process.stdout:
                lines.append(json.loads(raw))

        threading.Thread(target=collect, daemon=True).start()
        return Proxy(process, lines)

    yield start
    for process in processes:
        if process.poll() is None:
            process.terminate()
        # Under sanitizers a leak or error shows up as a nonzero exit, failing here.
        assert process.wait(timeout=TIMEOUT) == 0


def read_all(sock):
    data = b""
    while True:
        chunk = sock.recv(65536)
        if not chunk:
            return data
        data += chunk


def send_request(proxy, wire):
    with proxy.connect() as sock:
        sock.sendall(wire)
        return read_all(sock)


def status_of(response):
    return int(response.split(b" ", 2)[1])
