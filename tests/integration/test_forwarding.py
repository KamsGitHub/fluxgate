"""The forwarding path: correct bytes, however TCP chooses to deliver them."""

import hashlib
import socket
import threading
import time

from conftest import canned, read_all, send_request, status_of

GET = b"GET /hello?x=1 HTTP/1.1\r\nHost: example.test\r\n\r\n"


def test_forwards_request_and_response(make_backend, start_proxy):
    backend = make_backend()
    proxy = start_proxy(backend.port)
    response = send_request(proxy, GET)

    assert response.startswith(b"HTTP/1.1 200 OK\r\n")
    assert response.endswith(b"\r\n\r\nok")
    head = backend.requests[0]["head"]
    assert head.startswith("GET /hello?x=1 HTTP/1.1\r\n")
    assert "Host: example.test\r\n" in head


def test_adds_proxy_headers_and_rewrites_connection(make_backend, start_proxy):
    backend = make_backend()
    proxy = start_proxy(backend.port)
    response = send_request(proxy, GET)

    head = backend.requests[0]["head"]
    assert "X-Forwarded-For: 127.0.0.1\r\n" in head
    assert "Via: 1.1 fluxgate" in head
    assert "Connection: close\r\n" in head
    assert b"Connection: close\r\n" in response
    assert b"keep-alive" not in response


def test_request_delivered_one_byte_at_a_time(make_backend, start_proxy):
    backend = make_backend()
    proxy = start_proxy(backend.port)
    wire = b"POST /f HTTP/1.1\r\nHost: a\r\nContent-Length: 5\r\n\r\nhello"
    with proxy.connect() as sock:
        for i in range(len(wire)):
            sock.send(wire[i:i + 1])
            time.sleep(0.001)
        response = read_all(sock)

    assert status_of(response) == 200
    assert backend.requests[0]["body"] == b"hello"


def test_response_delivered_one_byte_at_a_time(make_backend, start_proxy):
    payload = b"HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"

    def handler(backend, conn):
        backend.read_request(conn)
        conn.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        for i in range(len(payload)):
            conn.send(payload[i:i + 1])
            time.sleep(0.001)

    proxy = start_proxy(make_backend(handler).port)
    assert send_request(proxy, GET).endswith(b"\r\n\r\nhello")


def test_large_request_body_arrives_intact(make_backend, start_proxy):
    backend = make_backend()
    proxy = start_proxy(backend.port, limits={"max_request_body_bytes": 64 * 1024 * 1024})
    body = bytes(range(256)) * 80_000  # 20 MiB
    wire = b"POST /u HTTP/1.1\r\nHost: a\r\nContent-Length: %d\r\n\r\n" % len(body)
    assert status_of(send_request(proxy, wire + body)) == 200
    assert hashlib.sha256(backend.requests[0]["body"]).digest() == hashlib.sha256(body).digest()


def test_large_response_survives_slow_small_window_reader(make_backend, start_proxy):
    """Forces partial writes on the real socket, and checks memory stays bounded."""
    body = bytes(range(256)) * 160_000  # 40 MiB

    def handler(backend, conn):
        backend.read_request(conn)
        conn.sendall(b"HTTP/1.1 200 OK\r\nContent-Length: %d\r\n\r\n" % len(body))
        conn.sendall(body)

    proxy = start_proxy(make_backend(handler).port, timeouts={"exchange_ms": 60000})
    baseline = proxy.rss_kib()

    sock = socket.socket()
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 4096)
    sock.settimeout(30)
    sock.connect(("127.0.0.1", proxy.port))
    sock.sendall(GET)
    time.sleep(0.5)  # Lets every kernel buffer on the path fill while nothing reads.
    peak = proxy.rss_kib()

    data = bytearray()  # bytes += would be quadratic, and outlast the backend's timeout.
    while True:
        chunk = sock.recv(1 << 20)
        if not chunk:
            break
        data += chunk
    sock.close()

    assert bytes(data).split(b"\r\n\r\n", 1)[1] == body
    # Buffered in the proxy: two 64 KiB buffers, not the 40 MiB in flight.
    assert peak - baseline < 8 * 1024


def test_chunked_response_is_relayed_verbatim(make_backend, start_proxy):
    chunks = b"5\r\nhello\r\n6\r\n world\r\n0\r\nX-Trailer: 1\r\n\r\n"
    backend = make_backend(canned(b"HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n" + chunks))
    response = send_request(start_proxy(backend.port), GET)
    assert b"Transfer-Encoding: chunked\r\n" in response
    assert response.endswith(b"\r\n\r\n" + chunks)


def test_close_delimited_response(make_backend, start_proxy):
    backend = make_backend(canned(b"HTTP/1.0 200 OK\r\n\r\nuntil close"))
    assert send_request(start_proxy(backend.port), GET).endswith(b"\r\n\r\nuntil close")


def test_head_does_not_wait_for_a_body(make_backend, start_proxy):
    backend = make_backend(canned(b"HTTP/1.1 200 OK\r\nContent-Length: 500\r\n\r\n"))
    proxy = start_proxy(backend.port)
    started = time.monotonic()
    response = send_request(proxy, b"HEAD / HTTP/1.1\r\nHost: a\r\n\r\n")
    assert b"Content-Length: 500\r\n" in response
    assert time.monotonic() - started < 1


def test_no_content_response(make_backend, start_proxy):
    backend = make_backend(canned(b"HTTP/1.1 204 No Content\r\n\r\n"))
    assert status_of(send_request(start_proxy(backend.port), GET)) == 204


def test_expect_continue_is_answered_by_the_proxy(make_backend, start_proxy):
    backend = make_backend()
    proxy = start_proxy(backend.port)
    with proxy.connect() as sock:
        sock.sendall(b"POST / HTTP/1.1\r\nHost: a\r\nExpect: 100-continue\r\n"
                     b"Content-Length: 4\r\n\r\n")
        assert sock.recv(4096) == b"HTTP/1.1 100 Continue\r\n\r\n"
        sock.sendall(b"body")
        assert status_of(read_all(sock)) == 200
    assert backend.requests[0]["body"] == b"body"
    assert "expect" not in backend.requests[0]["head"].lower()


def test_early_response_during_large_upload(make_backend, start_proxy):
    """The upstream refuses without reading the body, so both directions are in play."""

    def handler(backend, conn):
        data = b""
        while b"\r\n\r\n" not in data:
            data += conn.recv(4096)
        conn.sendall(b"HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\n\r\n")
        time.sleep(1)

    proxy = start_proxy(make_backend(handler).port,
                        limits={"max_request_body_bytes": 64 * 1024 * 1024})
    body = b"x" * (32 * 1024 * 1024)
    sock = proxy.connect()

    def upload():
        try:
            sock.sendall(b"POST / HTTP/1.1\r\nHost: a\r\nContent-Length: %d\r\n\r\n" % len(body))
            sock.sendall(body)
        except OSError:
            pass  # The proxy stops reading once it has delivered the response.

    uploader = threading.Thread(target=upload, daemon=True)
    uploader.start()
    assert status_of(sock.recv(4096)) == 403
    sock.close()
    uploader.join(timeout=5)


def test_half_closed_client_still_gets_its_response(make_backend, start_proxy):
    proxy = start_proxy(make_backend().port)
    with proxy.connect() as sock:
        sock.sendall(GET)
        sock.shutdown(socket.SHUT_WR)
        assert status_of(read_all(sock)) == 200


def test_many_sequential_requests_leak_no_descriptors(make_backend, start_proxy):
    proxy = start_proxy(make_backend().port)
    send_request(proxy, GET)
    before = proxy.open_fds()
    for _ in range(300):
        assert status_of(send_request(proxy, GET)) == 200
    proxy.wait_for_access_logs(301)
    assert proxy.open_fds() == before


def test_concurrent_clients_are_queued_and_all_served(make_backend, start_proxy):
    """Serial today, but the backlog must hold them all rather than drop any."""
    proxy = start_proxy(make_backend().port)
    results = []

    def client():
        results.append(status_of(send_request(proxy, GET)))

    threads = [threading.Thread(target=client) for _ in range(20)]
    for thread in threads:
        thread.start()
    for thread in threads:
        thread.join(timeout=15)
    assert results == [200] * 20
