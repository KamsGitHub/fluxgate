# Security

FluxGate is not ready for untrusted production traffic. This document says what is
protected today, how that is tested, and what is known to be missing.

## Trust boundaries

| Party | Trust | Rationale |
| --- | --- | --- |
| Clients | Untrusted | All bytes may be hostile, including their timing |
| Upstream | Semi-trusted | Operator-configured, but responses are still validated, as it may be faulty or compromised |
| Configuration file | Trusted | Written by the operator |
| Host | Trusted | Other local users and root are out of scope |

## Threat model

The attacker can open connections to the listener and send arbitrary bytes at any speed.
The goals considered are: desynchronising framing between FluxGate and an upstream,
reaching unintended destinations, forging request provenance, corrupting logs, leaking
sensitive data into logs, and exhausting resources.

## Mitigations

Every entry below has automated tests, in `tests/unit` and `tests/integration/test_security.py`.

| Threat | Mitigation |
| --- | --- |
| Request smuggling by conflicting framing | llhttp strict mode with no lenient flags; `Content-Length` with `Transfer-Encoding`, or repeated `Content-Length`, gets 400 |
| Smuggling through `Transfer-Encoding` | All request `Transfer-Encoding` is refused with 501 until Milestone 3 |
| Smuggling through malformed syntax | Bare CR or LF, whitespace before a colon, obsolete line folding and NUL bytes are all rejected |
| Trailing bytes treated as a new request | Only bytes the parser has placed inside the message are forwarded; each connection carries one request |
| Framing headers deleted through `Connection` | `Content-Length`, `Host` and `Transfer-Encoding` cannot be removed that way |
| Response desynchronisation | Upstream connections are never reused; bytes past the end of a response are dropped; ambiguous responses get 502 |
| Open proxy behaviour | The upstream is fixed by configuration; `CONNECT` and absolute-form targets are refused |
| Forged provenance | Client `Forwarded`, `X-Forwarded-*` and `X-Real-IP` are removed, including underscore variants |
| Slowloris | An absolute head deadline that progress does not extend |
| Slow request bodies and responses | Idle timeout and an absolute exchange cap |
| Memory exhaustion | Fixed buffers, head size and count limits, and a declared body limit checked before connecting |
| Log injection | Everything outside printable ASCII is escaped in JSON output |
| Secrets in logs | Query strings are not logged; neither are header values |
| Silent truncation | An upstream failure mid-response ends in RST rather than a clean close |
| Descriptor exhaustion in `accept` | Logged, with a 100 ms backoff to prevent spinning |

Three injected faults were confirmed to be caught by the suite when this was written:
relaying past the message end, losing the write position, and skipping lingering close.

## Known gaps

**Availability is the main weakness today.** Connections are served one at a time, so a
single client can hold the server until a timeout expires, and a few clients in rotation
can keep it busy indefinitely. With defaults that is up to 10 seconds per silent connection.
Milestone 2 removes this. Do not expose the current build to untrusted clients.

| Gap | Effect | Addressed in |
| --- | --- | --- |
| No concurrency | See above | M2 |
| No per-client or global connection limits | The backlog is the only limit | M2, M6 |
| No TLS | Traffic and `X-Forwarded-Proto` are plaintext-only | M6 |
| No metrics or request IDs | Attacks are visible only in access logs | M5 |
| Upstream resolved once, first address only | No DNS failover | M4 |
| Client FIN not seen while waiting for an upstream | A disconnected client's upstream work runs until a write fails or a deadline passes; this can't be told apart from a legitimate half-close | M3 |
| Log writes are synchronous | A blocked stdout pipe stalls the proxy | M5 |
| `Host` value is forwarded, not matched against an allowlist | Backends must not trust it blindly | M4 routing |
| Response headers are not filtered for sensitive content | Upstream `Server` and similar pass through | Not scheduled |

## Behaviours worth knowing

llhttp's strict mode accepts multiple spaces between the method and the request target,
which RFC 9112 forbids. This is not a smuggling route here, as the start line is rebuilt
from parsed fields and the upstream never sees the original bytes. A test records this
behaviour, so a dependency update that changes it will be noticed.

Underscored headers other than the provenance and hop-by-hop names are forwarded unchanged.
