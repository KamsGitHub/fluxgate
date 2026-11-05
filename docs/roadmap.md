# Roadmap

Status of every planned capability. An entry moves to "Implemented and tested" only
when an automated test in this repository exercises it.

| Status | Meaning |
| --- | --- |
| Tested | Implemented and covered by passing automated tests |
| Unvalidated | Implemented, but not yet fully validated |
| Planned | Scheduled for the milestone shown |
| Unsupported | Deliberately refused or out of scope |

## Milestone 1: Foundation (complete)

| Capability | Status |
| --- | --- |
| C++20 CMake project with presets and strict warnings (`-Werror`) | Tested |
| Sanitizer builds: ASan with UBSan, and TSan | Tested |
| Reproducible dependencies (Conan 2 lockfile) | Tested |
| clang-format and clang-tidy, clean | Tested |
| RAII file descriptors and non-blocking TCP listener | Tested |
| Structured JSON logging and access logs | Tested |
| YAML configuration with validation before listening | Tested |
| Non-blocking, full-duplex forwarding to one backend | Tested |
| Fragmented input and partial writes | Tested |
| Backpressure and bounded per-connection memory | Tested |
| Strict framing with `Content-Length` requests | Tested |
| Response relaying: `Content-Length`, chunked, close-delimited | Tested |
| Head, connect, response, idle and exchange timeouts | Tested |
| Hop-by-hop and provenance header handling | Tested |
| Stop on SIGTERM and SIGINT, without draining | Tested |
| Docker image and Compose stack | Tested |
| GitHub Actions workflow | Tested |
| Soak and high-load behaviour | Unvalidated: Milestone 7 |

## Milestone 2: Event-driven networking

epoll event loop, multiple concurrent connections, connection and flow-control tests,
resource-lifecycle tests. `ProxySession` is built to be driven by it unchanged.

## Milestone 3: HTTP/1.1 correctness

Persistent connections, request chunked encoding, pipelining with response ordering,
black-box conformance and malformed-input suites.

## Milestone 4: Load balancing and reliability

Round-robin, weighted round-robin, least-active-connections, active and passive health
checks, connection pooling, retries only where they cannot duplicate non-idempotent
operations.

## Milestone 5: Observability and operations

Prometheus metrics on a separate management listener, request IDs, graceful shutdown
with a drain deadline, multi-worker event loops where benchmarks justify them.

## Milestone 6: Security and TLS

TLS termination, HTTPS upstreams with certificate and hostname verification, further
HTTP hardening, security-focused integration tests.

## Milestone 7: Performance engineering

Reproducible load tests, latency and throughput measurement, CPU and memory profiling,
regression benchmarks, and a like-for-like comparison with NGINX or Envoy.

## Unsupported

Refused today with an explicit status, unless a milestone above says otherwise.

| Behaviour | Response | Changes in |
| --- | --- | --- |
| Request `Transfer-Encoding` | 501 | M3 |
| HTTP versions other than 1.1 | 505 | Not scheduled |
| `CONNECT` | 501 | Not scheduled |
| Absolute-form and asterisk-form targets | 400 | Not scheduled |
| Protocol upgrades, including WebSocket | `Upgrade` removed; upstream 101 gets 502 | Not scheduled |
| HTTP/2 and HTTP/3 | Not implemented | Not scheduled |
| Pipelined requests | Dropped; connection closes after one response | M3 |
