# Design decisions

Each entry records a decision, why it was made, and what was rejected.

## Sans-IO session driven by an external event source

**Decision:** `ProxySession` expresses all waiting as data, and has no loop of its own.

**Why:** Milestone 1 needs forwarding before Milestone 2 brings epoll. Blocking code would
have been rewritten entirely, and could not have been full duplex.

**Rejected:** Blocking `read`/`write` per connection, which deadlocks when an upstream
answers early. A per-direction thread pair, which contradicts the project's threading model.

## One connection at a time in Milestone 1

**Decision:** `Server` serves connections serially.

**Why:** Concurrency is Milestone 2's scope. Faking it with threads now would add ownership
and shutdown problems that the epoll design removes.

**Cost:** One slow client delays all others. It is bounded by the timeouts but is a real
denial-of-service weakness until Milestone 2; see [security.md](security.md).

## llhttp from the first milestone

**Decision:** Use llhttp at once, although parser integration is formally Milestone 3.

**Why:** A hand-written parser would have been thrown away, and it is the most dangerous
code in a proxy to get subtly wrong. llhttp is the parser behind Node.js.

**Rejected:** A minimal parser for Milestone 1.

## Report boundaries; relay raw bytes

**Decision:** The parser finds message boundaries using `HPE_PAUSED`, and the proxy forwards
the original bytes. Bodies are not decoded and re-encoded.

**Why:** Relaying needs no copying, and the proxy cannot introduce re-framing errors.
Transfer-Encoding and Trailer are kept on responses, because they still describe the bytes.

**Cost:** Chunked framing is passed through unmodified, so a future HTTP/1.0 client would
need decoding. HTTP/1.0 is refused today.

## Rendering heads rather than editing them in place

**Decision:** Heads are parsed into fields and rebuilt.

**Why:** The upstream only ever receives a canonical serialisation, so any oddity the parser
tolerates cannot reach it. Extra spaces in the request line are one such case.

**Cost:** Allocations per request, and header-name case is preserved but whitespace is not.

## `Stream` as a virtual interface

**Decision:** Session I/O goes through an abstract class.

**Why:** It enables deterministic fault injection in tests, and is where TLS will plug in. A
virtual call beside a syscall has no measurable cost.

**Rejected:** Templating the session on its stream type, which would move all session code
into headers for no measurable gain.

## Conan 2 rather than vcpkg

**Decision:** Conan 2, with `conan.lock` committed.

**Why:** The lockfile pins recipe revisions, which gives a reproducible graph with no
registry baseline to manage. Prebuilt binaries shorten CI. Sitting in a Python virtualenv
alongside CMake, Ninja and the clang tools, it needs no system packages or root access.

**Rejected:** vcpkg, which is a solid alternative, but needs a separate bootstrap and more
often builds from source. System packages, whose versions vary by distribution.

## Absorbing `Expect: 100-continue` at the proxy

**Decision:** FluxGate answers `100 Continue` itself and strips `Expect`.

**Why:** Clients would otherwise stall for their own timeout. It also removes a per-exchange
protocol step that pooled connections would have to track. Envoy behaves the same way.

**Cost:** The upstream loses the chance to refuse before the body is sent.

## Lingering close

**Decision:** After responding, half-close and drain the client, bounded by time and bytes.

**Why:** Closing a socket with unread data sends RST, which can destroy a response the client
has not read yet. This matters most for a 413 sent during an upload.

**Cost:** A client that never closes holds the serial server for up to one second.

## Client provenance headers are discarded

**Decision:** `Forwarded`, `X-Forwarded-*` and `X-Real-IP` from clients are removed, including
underscore variants, and FluxGate's own are added.

**Why:** FluxGate has no trusted downstream, so any such value is forged by definition. A
trusted-proxy list could relax this later, as a deliberate configuration choice.

## Upstream connections closed after each exchange

**Decision:** `Connection: close` goes to the upstream, and the connection is not reused.

**Why:** This removes response-queue desynchronisation entirely until pooling (Milestone 4)
has been built with the framing guarantees it needs.

## Level-triggered readiness

**Decision:** The session derives interest from its state, and treats readiness as a hint.

**Why:** A full buffer requires the proxy to deliberately stop reading, which level-triggered
semantics express directly. Milestone 2 will re-examine this with measurements.
