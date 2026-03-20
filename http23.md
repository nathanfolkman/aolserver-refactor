# HTTP/2 and HTTP/3 notes

**Authoritative status, build instructions, h2spec commands, configuration keys, metrics, and ASan notes are in the root [README.md](README.md).** This file is kept for historical design context only.

## Snapshot (see README for updates)

- **HTTP/2:** TLS + ALPN `h2` via **nsssl**, framing with **nghttp2**. Full **h2spec** (TLS) against `tests/h2test/minimal.tcl` is expected to report **145 passed, 0 failed, 1 skipped** (146 tests) when run with a sufficient timeout (e.g. `H2SPEC_TIMEOUT=120` and `tests/h2test/run-h2spec.sh --start-nsd`).
- **Automation:** `tests/h2test/run-h2spec.sh`; optional GitHub Actions workflow `.github/workflows/h2spec.yml`.
- **HTTP/3:** Not implemented (requires QUIC + HTTP/3 stack integration).

## Older handoff text (obsolete)

Earlier drafts in this file referenced **62 h2spec failures** and partial fixes. That era predates the current green h2spec run and the driver/http2 hardening described in **README.md**. Ignore numbered failure counts here unless they match **README.md**.
