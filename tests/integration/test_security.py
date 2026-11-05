"""Hostile input: nothing malformed or smuggled may reach the upstream."""

import time

import pytest

from conftest import read_all, send_request, status_of

REJECTED = [
    pytest.param(b"POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 1\r\nContent-Length: 2\r\n\r\nx",
                 400, id="conflicting-content-length"),
    pytest.param(b"POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 1\r\n"
                 b"Transfer-Encoding: chunked\r\n\r\n0\r\n\r\n", 400, id="cl-then-te"),
    pytest.param(b"POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n"
                 b"Content-Length: 1\r\n\r\n0\r\n\r\n", 400, id="te-then-cl"),
    pytest.param(b"POST / HTTP/1.1\r\nHost: a\r\nContent-Length : 1\r\n\r\nx", 400,
                 id="space-before-colon"),
    pytest.param(b"GET / HTTP/1.1\r\nHost: a\r\n folded: x\r\n\r\n", 400, id="obs-fold"),
    pytest.param(b"GET / HTTP/1.1\nHost: a\n\n", 400, id="bare-lf"),
    pytest.param(b"GET / HTTP/1.1\r\nX: a\rContent-Length: 5\r\nHost: a\r\n\r\n", 400,
                 id="bare-cr"),
    pytest.param(b"GET / HTTP/1.1\r\n\r\n", 400, id="missing-host"),
    pytest.param(b"GET / HTTP/1.1\r\nHost: a\r\nHost: b\r\n\r\n", 400, id="duplicate-host"),
    pytest.param(b"GET http://evil.test/ HTTP/1.1\r\nHost: a\r\n\r\n", 400, id="absolute-form"),
    pytest.param(b"CONNECT evil.test:443 HTTP/1.1\r\nHost: evil.test\r\n\r\n", 501, id="connect"),
    pytest.param(b"POST / HTTP/1.1\r\nHost: a\r\nTransfer-Encoding: chunked\r\n\r\n0\r\n\r\n",
                 501, id="chunked-request"),
    pytest.param(b"GET / HTTP/1.0\r\nHost: a\r\n\r\n", 505, id="http-1.0"),
    pytest.param(b"GET / HTTP/1.1\r\nHost: a\r\nExpect: magic\r\n\r\n", 417, id="bad-expect"),
    pytest.param(b"not http\r\n\r\n", 400, id="garbage"),
]


@pytest.mark.parametrize("wire, status", REJECTED)
def test_rejected_without_contacting_upstream(make_backend, start_proxy, wire, status):
    backend = make_backend()
    proxy = start_proxy(backend.port)
    assert status_of(send_request(proxy, wire)) == status
    assert backend.connections == 0


def test_oversized_head_gets_431(make_backend, start_proxy):
    backend = make_backend()
    proxy = start_proxy(backend.port, limits={"max_header_bytes": 1024})
    wire = b"GET / HTTP/1.1\r\nHost: a\r\nX-Pad: " + b"a" * 4096 + b"\r\n\r\n"
    assert status_of(send_request(proxy, wire)) == 431
    assert backend.connections == 0


def test_too_many_headers_get_431(make_backend, start_proxy):
    proxy = start_proxy(make_backend().port, limits={"max_headers": 5})
    wire = b"GET / HTTP/1.1\r\nHost: a\r\n" + b"X: y\r\n" * 10 + b"\r\n"
    assert status_of(send_request(proxy, wire)) == 431


def test_oversized_body_is_refused_while_client_is_still_sending(make_backend, start_proxy):
    """The 413 must survive the unread upload: this is what lingering close is for."""
    backend = make_backend()
    proxy = start_proxy(backend.port, limits={"max_request_body_bytes": 1024})
    body = b"x" * (256 * 1024)
    with proxy.connect() as sock:
        sock.sendall(b"POST / HTTP/1.1\r\nHost: a\r\nContent-Length: %d\r\n\r\n" % len(body))
        try:
            sock.sendall(body)
        except OSError:
            pass
        assert status_of(sock.recv(4096)) == 413
    assert backend.connections == 0


def test_bytes_after_the_body_never_reach_the_upstream(make_backend, start_proxy):
    backend = make_backend()
    proxy = start_proxy(backend.port)
    send_request(proxy, b"POST / HTTP/1.1\r\nHost: a\r\nContent-Length: 2\r\n\r\nhi"
                        b"GET /smuggled HTTP/1.1\r\nHost: a\r\n\r\n")
    assert len(backend.requests) == 1
    assert backend.requests[0]["body"] == b"hi"
    assert "smuggled" not in backend.requests[0]["head"]


def test_connection_header_cannot_strip_content_length(make_backend, start_proxy):
    backend = make_backend()
    proxy = start_proxy(backend.port)
    send_request(proxy, b"POST / HTTP/1.1\r\nHost: a\r\nConnection: Content-Length\r\n"
                        b"Content-Length: 2\r\n\r\nhi")
    assert "Content-Length: 2\r\n" in backend.requests[0]["head"]
    assert backend.requests[0]["body"] == b"hi"


def test_forged_provenance_headers_are_replaced(make_backend, start_proxy):
    backend = make_backend()
    proxy = start_proxy(backend.port)
    send_request(proxy, b"GET / HTTP/1.1\r\nHost: a\r\nX-Forwarded-For: 6.6.6.6\r\n"
                        b"X_Forwarded_For: 6.6.6.6\r\nForwarded: for=6.6.6.6\r\n\r\n")
    head = backend.requests[0]["head"]
    assert "6.6.6.6" not in head
    assert "X-Forwarded-For: 127.0.0.1\r\n" in head


def test_slowloris_is_cut_off_at_the_head_deadline(make_backend, start_proxy):
    backend = make_backend()
    proxy = start_proxy(backend.port, timeouts={"request_header_ms": 400})
    started = time.monotonic()
    with proxy.connect() as sock:
        sock.sendall(b"GET / HTTP/1.1\r\nHost: a\r\n")
        try:
            for _ in range(100):  # Steady trickling must not extend the deadline.
                sock.sendall(b"X")
                time.sleep(0.02)
        except OSError:
            pass
        response = read_all(sock)
    assert status_of(response) == 408
    assert time.monotonic() - started < 3
    assert backend.connections == 0


def test_silent_client_does_not_wedge_the_server(make_backend, start_proxy):
    proxy = start_proxy(make_backend().port, timeouts={"request_header_ms": 200})
    idle = proxy.connect()
    # This client queues behind the silent one, and must be served once it times out.
    assert status_of(send_request(proxy, b"GET / HTTP/1.1\r\nHost: a\r\n\r\n")) == 200
    idle.close()


def test_query_strings_stay_out_of_the_access_log(make_backend, start_proxy):
    proxy = start_proxy(make_backend().port)
    send_request(proxy, b"GET /login?token=hunter2 HTTP/1.1\r\nHost: a\r\n\r\n")
    entry = proxy.wait_for_access_logs(1)[0]
    assert entry["path"] == "/login"
    assert "hunter2" not in str(proxy.lines)


def test_hostile_target_cannot_break_log_format(make_backend, start_proxy):
    proxy = start_proxy(make_backend().port)
    send_request(proxy, b'GET /"},{"event":"forged HTTP/1.1\r\nHost: a\r\n\r\n')
    proxy.wait_for_access_logs(1)  # conftest parses every line as JSON.
    assert not proxy.events("forged")
