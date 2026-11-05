"""Peer failures: each maps to a defined response, and the proxy keeps serving."""

import resource
import socket
import struct
import time

import pytest

from conftest import canned, read_all, send_request, status_of

GET = b"GET / HTTP/1.1\r\nHost: a\r\n\r\n"


def closed_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def test_backend_down_gives_502_and_proxy_keeps_serving(start_proxy):
    proxy = start_proxy(closed_port())
    assert status_of(send_request(proxy, GET)) == 502
    assert status_of(send_request(proxy, GET)) == 502
    assert proxy.process.poll() is None


def test_backend_that_never_answers_gives_504(make_backend, start_proxy):
    def handler(backend, conn):
        backend.read_request(conn)
        time.sleep(2)

    proxy = start_proxy(make_backend(handler).port, timeouts={"response_header_ms": 300})
    started = time.monotonic()
    assert status_of(send_request(proxy, GET)) == 504
    assert time.monotonic() - started < 1.5


def test_backend_closing_without_answering_gives_502(make_backend, start_proxy):
    def handler(backend, conn):
        backend.read_request(conn)

    assert status_of(send_request(start_proxy(make_backend(handler).port), GET)) == 502


@pytest.mark.parametrize("payload", [
    pytest.param(b"not http\r\n\r\n", id="garbage"),
    pytest.param(b"HTTP/1.1 200 OK\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\n", id="double-cl"),
    pytest.param(b"HTTP/1.1 101 Switching Protocols\r\nUpgrade: x\r\n\r\n", id="unrequested-101"),
])
def test_invalid_upstream_response_gives_502(make_backend, start_proxy, payload):
    assert status_of(send_request(start_proxy(make_backend(canned(payload)).port), GET)) == 502


def test_truncated_body_ends_in_reset_not_clean_close(make_backend, start_proxy):
    """A clean FIN would let close-delimited clients accept a partial response."""
    backend = make_backend(canned(b"HTTP/1.1 200 OK\r\nContent-Length: 100\r\n\r\npartial"))
    proxy = start_proxy(backend.port)
    with proxy.connect() as sock:
        sock.sendall(GET)
        data = b""
        with pytest.raises(ConnectionResetError):
            while True:
                chunk = sock.recv(4096)
                assert chunk, "expected a reset, got a clean close"
                data += chunk
    assert proxy.wait_for_access_logs(1)[0]["outcome"] == "upstream_aborted"


def test_client_vanishing_mid_body_leaves_proxy_healthy(make_backend, start_proxy):
    backend = make_backend()
    proxy = start_proxy(backend.port)
    with proxy.connect() as sock:
        sock.sendall(b"POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 100\r\n\r\npartial")
    assert proxy.wait_for_access_logs(1)[0]["outcome"] == "client_aborted"
    assert status_of(send_request(proxy, GET)) == 200


def test_client_resetting_mid_response_leaves_proxy_healthy(make_backend, start_proxy):
    body = b"x" * (16 * 1024 * 1024)
    backend = make_backend(canned(b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n" % len(body) + body))
    proxy = start_proxy(backend.port)
    with proxy.connect() as sock:
        sock.sendall(GET)
        sock.recv(1024)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
    assert proxy.wait_for_access_logs(1)[0]["outcome"] == "client_aborted"
    assert proxy.process.poll() is None


def test_stalled_upload_gets_408(make_backend, start_proxy):
    def handler(backend, conn):
        while conn.recv(65536):
            pass

    proxy = start_proxy(make_backend(handler).port, timeouts={"idle_ms": 300})
    with proxy.connect() as sock:
        sock.sendall(b"POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 10\r\n\r\nab")
        assert status_of(read_all(sock)) == 408


def test_stalled_response_is_reset(make_backend, start_proxy):
    def handler(backend, conn):
        backend.read_request(conn)
        conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: 10\r\n\r\nab")
        time.sleep(2)

    proxy = start_proxy(make_backend(handler).port, timeouts={"idle_ms": 300})
    with proxy.connect() as sock:
        sock.sendall(GET)
        with pytest.raises(ConnectionResetError):
            while sock.recv(4096):
                pass
    assert proxy.wait_for_access_logs(1)[0]["outcome"] == "timed_out"


def test_descriptor_exhaustion_at_accept_backs_off_and_recovers(make_backend, start_proxy):
    proxy = start_proxy(make_backend().port)
    limits = resource.prlimit(proxy.process.pid, resource.RLIMIT_NOFILE)
    resource.prlimit(proxy.process.pid, resource.RLIMIT_NOFILE, (proxy.open_fds(), limits[1]))

    sock = proxy.connect()  # Completed by the kernel, but the proxy cannot accept it.
    sock.sendall(GET)
    time.sleep(0.5)
    failures = len(proxy.events("server.accept_failed"))
    assert 1 <= failures <= 10  # 100 ms backoff, so about 5; a spin would log thousands.

    resource.prlimit(proxy.process.pid, resource.RLIMIT_NOFILE, limits)
    assert status_of(read_all(sock)) == 200
    sock.close()


def test_descriptor_exhaustion_at_connect_gives_502(make_backend, start_proxy):
    backend = make_backend()
    proxy = start_proxy(backend.port)
    limits = resource.prlimit(proxy.process.pid, resource.RLIMIT_NOFILE)
    # Room for the accepted client socket, but not the upstream socket.
    resource.prlimit(proxy.process.pid, resource.RLIMIT_NOFILE, (proxy.open_fds() + 1, limits[1]))

    assert status_of(send_request(proxy, GET)) == 502
    assert backend.connections == 0
    assert "open files" in proxy.wait_for_access_logs(1)[0]["detail"]
