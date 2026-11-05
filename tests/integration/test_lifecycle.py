"""Process behaviour: startup validation, logging and signals."""

import json
import signal
import socket
import subprocess
import time
from pathlib import Path

import pytest

from conftest import BINARY, TIMEOUT, send_request, status_of

REPO_ROOT = Path(__file__).resolve().parents[2]


def run(config_text, tmp_path, *args):
    path = tmp_path / "config.yaml"
    path.write_text(config_text)
    return subprocess.run([BINARY, "--config", str(path), *args], capture_output=True, text=True,
                          timeout=TIMEOUT)


def test_check_config_accepts_the_shipped_example():
    result = subprocess.run([BINARY, "--config", "config/proxy.yaml", "--check-config"],
                            capture_output=True, text=True, timeout=TIMEOUT, cwd=REPO_ROOT)
    assert result.returncode == 0, result.stderr


def test_invalid_config_exits_2_with_diagnostics_and_never_listens(tmp_path):
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]

    result = run(f"listener: {{port: {port}}}\nupstream: {{host: h}}\ntypo: 1\n", tmp_path)

    assert result.returncode == 2
    assert "upstream.port" in result.stderr
    assert "typo" in result.stderr
    assert result.stdout == ""
    with socket.socket() as sock:
        assert sock.connect_ex(("127.0.0.1", port)) != 0


def test_listen_failure_exits_1_with_a_json_error(tmp_path):
    with socket.socket() as holder:
        holder.bind(("127.0.0.1", 0))
        holder.listen(1)
        port = holder.getsockname()[1]
        result = run(f"listener: {{port: {port}}}\nupstream: {{host: 127.0.0.1, port: 1}}\n",
                     tmp_path)
    assert result.returncode == 1
    assert json.loads(result.stdout.splitlines()[-1])["event"] == "server.fatal"


def test_version_flag():
    result = subprocess.run([BINARY, "--version"], capture_output=True, text=True, timeout=TIMEOUT)
    assert result.returncode == 0
    assert result.stdout.startswith("fluxgate ")


def test_missing_config_argument_is_a_usage_error():
    assert subprocess.run([BINARY], capture_output=True, timeout=TIMEOUT).returncode == 2


@pytest.mark.parametrize("signum", [signal.SIGTERM, signal.SIGINT])
def test_signals_stop_an_idle_server_cleanly(make_backend, start_proxy, signum):
    proxy = start_proxy(make_backend().port)
    proxy.process.send_signal(signum)
    assert proxy.process.wait(timeout=TIMEOUT) == 0
    assert proxy.events("server.stopped")


def test_sigterm_during_a_session_exits_promptly(make_backend, start_proxy):
    proxy = start_proxy(make_backend().port)
    with proxy.connect() as sock:
        sock.sendall(b"GET / HTTP/1.1\r\n")
        time.sleep(0.1)  # The partial request keeps a session open.
        proxy.process.send_signal(signal.SIGTERM)
        assert proxy.process.wait(timeout=TIMEOUT) == 0
    assert proxy.wait_for_access_logs(1)[0]["outcome"] == "shutdown"


def test_access_log_records_the_exchange(make_backend, start_proxy):
    proxy = start_proxy(make_backend().port)
    assert status_of(send_request(proxy, b"GET /p HTTP/1.1\r\nHost: a\r\n\r\n")) == 200
    entry = proxy.wait_for_access_logs(1)[0]
    assert entry["level"] == "info"
    assert "ts" in entry
    assert (entry["method"], entry["path"], entry["status"]) == ("GET", "/p", 200)
    assert entry["outcome"] == "completed"
    assert entry["bytes_in"] > 0 and entry["bytes_out"] > 0
    assert entry["client"] == "127.0.0.1"
