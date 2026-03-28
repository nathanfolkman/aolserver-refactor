# CMake dependency patches

Patches applied during **ExternalProject** fetches are recorded here so version bumps can be re-evaluated. For each new patch, prefer opening an upstream issue or PR and linking it below.

| Dependency | Version | Patch file(s) | Rationale | Upstream |
|------------|---------|-----------------|-----------|----------|
| **nghttp2** | 1.61.0 | `nghttp2-1.61.0-disable-stream-reset-goaway.patch` | h2spec **http2/7.2**: default libnghttp2 server sends GOAWAY after 1000 inbound `RST_STREAM` frames on one connection; a full h2spec run exceeds that before §7.2. | *(track issue when filed)* |
| **nghttp2** | 1.61.0 | `nghttp2-1.61.0-unexpected-continuation-rst.patch` | h2spec **5.1.7**: unexpected `CONTINUATION` must be answered with `RST_STREAM` / `STREAM_CLOSED`, not connection close. | *(track issue when filed)* |
| **nghttp2** | 1.61.0 | `nghttp2-1.61.0-priority-half-closed-remote.patch` | RFC 7540 5.1: accept `PRIORITY` on half-closed (remote) without failing reprioritization (h2spec **generic/2/3**). | *(track issue when filed)* |
| **nghttp2** | 1.61.0 | `nghttp2-1.61.0-data-failfast-rst-stream.patch` | `session_on_data_received_fail_fast`: use `RST_STREAM` for `STREAM_CLOSED` instead of terminating the connection (h2spec **http2/5.1.11**, etc.). | *(track issue when filed)* |
| **nghttp2** | 1.61.0 | `nghttp2-1.61.0-headers-half-closed-rst.patch` | `on_headers_received`: half-closed (remote) extra `HEADERS` → stream error, not connection error (h2spec **http2/5.1** half-closed cases). | *(track issue when filed)* |
| **ngtcp2** | 1.21.0 | `ngtcp2-1.21.0-allow-undersized-initial.patch` (via `apply-ngtcp2-undersized-initial.sh`) | h3spec **TLS 8.2** sends a minimal ClientHello in a short datagram; stock ngtcp2 drops it before TLS runs, so the server cannot emit a useful `CONNECTION_CLOSE` with TLS alert. | *(track issue when filed)* |

**Unpatched in this tree:** **nghttp3** (see `cmake/Http3Deps.cmake`) — no local patches at the versions listed there.

**CMake wiring:** `cmake/Http23Deps.cmake` (nghttp2), `cmake/Http3Deps.cmake` (ngtcp2, nghttp3).
