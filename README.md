# AOLserver (refactor)

Upstream-style notes remain in [README](README). This document covers **this tree**: CMake builds, TLS **HTTP/2**, optional **HTTP/3 (QUIC)**, and how to verify behavior. For a short HTTP/2/3 pointer and historical context, see [http23.md](http23.md) — **operational truth is here** (http23.md defers to this file for current numbers and procedures).

## Building

From the repository root (with dependencies fetched by CMake as configured in this project):

```sh
cmake -S . -B build
cmake --build build -j8
```

**HTTP/3 (experimental)** requires **OpenSSL 3.5+** (bundled by this project’s deps), **ngtcp2**, and **nghttp3**, and forces **`-DNS_WITH_HTTP2=ON`** and **`-DNS_WITH_SSL=ON`**:

```sh
cmake -S . -B build-h3 -DNS_WITH_HTTP3=ON
cmake --build build-h3 -j8
```

### Reproducing GitHub Actions locally (Docker, Ubuntu 24.04)

The **conformance** workflow (`.github/workflows/h2spec.yml`) matches **`docker/Dockerfile.ci`** (Ubuntu **24.04**: CMake, Ninja, `libssl-dev`, `zlib1g-dev`, **`tcl`**, **`python3`**, etc.). With Docker installed:

```sh
chmod +x docker/run-ci-build.sh
./docker/run-ci-build.sh              # configure + build → build-docker-ci/ (gitignored)
./docker/run-ci-build.sh --shell       # shell in the image; repo mounted at /workspace
./docker/run-ci-build.sh --h2spec      # build + full h2spec (same env as CI)
./docker/run-ci-build.sh --conformance # h2spec + HTTP/1 + HTTP/3 build + h3spec (long-running)
```

Override **`BUILD_DIR`**, **`IMAGE`**, or **`DOCKER_PLATFORM`** if needed. **`DOCKER_PLATFORM=linux/amd64`** matches **GitHub-hosted** `ubuntu-latest` (x86_64); on Apple Silicon, omit it for a faster native **arm64** compile, or set it to reproduce CI byte-for-byte. The **`--h2spec`** / **`--conformance`** paths default to **`linux/amd64`** (the workflow installs **`h2spec_linux_amd64`** / **`h3spec-linux-x86_64`**). The script raises undersized **`H2SPEC_TIMEOUT`** values (below **180s**) to **300s** so **`h2spec -o`** does not flake under Docker Desktop **qemu** (full **h2spec** can take **30+** minutes emulated). Use **`--shell`** to run **`cmake`** / **`ninja`** by hand and capture full compiler errors.

## Running `nsd` from the build tree (macOS)

The dynamic loader must find `libnsd`, `libnsthread`, and bundled Tcl/OpenSSL/nghttp2 libraries:

```sh
tests/h2test/generate-tls-certs.sh   # self-signed cert.pem + key.pem (gitignored; OpenSSL CLI)
export DYLD_LIBRARY_PATH="$PWD/build/nsd:$PWD/build/nsthread:$PWD/deps-install/lib"
export NS_TCL_LIBRARY="$PWD/deps-install/lib/tcl8.6"
./build/nsd/nsd -f -t tests/h2test/minimal.tcl
```

`tests/h2test/run-h2spec.sh --start-nsd` and `tests/h3test/run-h3spec.sh --start-nsd` run the generator automatically. Manual runs (e.g. `stress-parallel-h2.sh` after starting `nsd` by hand) need the PEMs present first.

Give the process **~10 seconds** after “listening” before hitting TLS: reader threads are created on demand, and the first connection may arrive before the reader pool is ready. Avoid firing several parallel TLS clients in the first second after startup; an intermittent crash has been observed under that pattern and needs further isolation.

## HTTP/2 (TLS, ALPN `h2`)

HTTP/2 is implemented with **nghttp2** over the existing **nsssl** OpenSSL driver.

Notable fixes in this codebase:

- **Server connection preface:** The first outbound frame is a non-ACK **SETTINGS** frame (RFC 7540). With no `h2_*` SETTINGS parameters in the driver module config, `nghttp2_submit_settings(..., NULL, 0)` is used (library/RFC defaults). Optional parameters (see below) submit an explicit SETTINGS set instead. Without any server preface, nghttp2 could emit only a **SETTINGS ACK** first, which breaks clients (e.g. curl).
- **Input draining:** `nghttp2_session_mem_recv` may consume only a prefix of the supplied buffer; the remainder must be fed in the same read cycle (loop until the SSL chunk is fully consumed). Outbound `nghttp2_session_send` is **not** invoked between steps of that inner loop: flushing after the first frame in a chunk but before the rest is consumed could abort the feed with `E_RECV` and drop the remainder of the TLS plaintext (e.g. PRIORITY then PING in one read).
- **TLS EOF:** `nsssl` maps `SSL_read` returning 0 with `SSL_ERROR_ZERO_RETURN`, or `SSL_ERROR_SYSCALL` with `errno == 0` (TCP FIN/RST without `close_notify`), to `NS_DRIVER_RECV_TLS_EOF` (`-2`); the driver treats that like a clean close (`E_CLOSE`) instead of `E_RECV`. If `SSL_has_pending` / `SSL_pending` still report data, that SYSCALL/0 case is **not** treated as EOF (seen on Darwin with TLS 1.3) so HTTP/2 input is not dropped spuriously. If there is no OpenSSL “pending” data but a **TCP** `recv(..., MSG_PEEK)` shows ciphertext waiting, SYSCALL/0 is also **not** treated as EOF (more follow-on TLS records on the wire).
- **OpenSSL read drain:** After a successful `SSL_read`, `DriverRecv` loops on `SSL_has_pending(ssl)` so additional decrypted application data in the SSL buffer is returned in the same driver call (avoids waiting on `poll()` when the TCP queue is already empty).
- **Outbound flush before read:** In `SOCK_READWAIT`, the driver calls `NsHttp2TrySend` for every HTTP/2 socket with a session, not only when `POLLOUT` appears in `revents`, so pending SETTINGS/PING responses are not starved on stacks that omit `POLLOUT` until a write is attempted.
- **Handshake concurrency:** `sslInitLock` in `nsssl` is not held across `SslAcceptNonBlocking` (which may poll for a long time), so other connections are not blocked for the whole server TLS handshake.
- **Driver / reader:** HTTP/2 TLS sockets returned from the reader in `SOCK_READWAIT` (e.g. after `SSL_ERROR_WANT_READ`) are re-queued on the driver poll list instead of the pre-queue path that closes non-`SOCK_PREQUE` sockets.
- **Reader thread:** When `h2NeedDriverPoll` is set, control returns to the driver with an explicit `goto` after one `SockRead` iteration so the connection is not spun in the reader with extra `SSL_read` calls. Without that flag, HTTP/2 still allows up to **`h2readburst`** `SockRead` calls per driver wakeup (default **16**) so SETTINGS can complete in one batch; beyond that, the socket returns to the driver to avoid `POLLIN`/`SSL_read==0` spin (h2spec PING, etc.).
- **`NsHttp2TrySend` / TLS:** If `nghttp2_session_want_write` remains true after `nghttp2_session_send` (e.g. `SSL_write` stopped with `WANT_READ`), the implementation issues a non-blocking `DriverRecv` and feeds the result with `mem_recv` before retrying send, so SETTINGS ACK and follow-on client frames are not stuck behind a half-flushed TLS write.
- **Eager session + zero-length TLS read:** If the first post-handshake `DriverRecv` returns 0 bytes on HTTP/2, the driver still calls `NsHttp2EnsureSession` and `NsHttp2TrySend` (server SETTINGS preface), then performs up to four extra non-blocking `DriverRecv` attempts with `NsHttp2TrySend` before each, so the client preface often consumed in the same reader turn (SETTINGS ACK not starved). Accepted client sockets use **TCP_NODELAY** to reduce small-frame stalls with some clients.
- **Debug tracing:** Set **`AOLSERVER_H2_DEBUG`** to any non-empty value other than `0` to print **`SockClose`** and **`NsHttp2Feed`** diagnostics to **stderr** (bypasses normal server logging). Useful to correlate unexpected closes with feed outcomes (`rv=2` from `NsHttp2Feed` is mapped to `E_RECV` in the driver when `NsHttp2TrySend` fails).

### HTTP/2 configuration (`ns/server/<server>/module/nsssl`)

Optional parameters on the **nsssl** driver module (same section as `recvwait`, `address`, etc.):

| Parameter | Purpose |
|-----------|---------|
| `h2readburst` | Max HTTP/2 `SockRead` iterations per driver wakeup (default **16**, range 1–65535). |
| `h2_header_table_size` | `SETTINGS_HEADER_TABLE_SIZE`. |
| `h2_enable_push` | `SETTINGS_ENABLE_PUSH` (`0` / `1` / boolean strings). |
| `h2_max_concurrent_streams` | `SETTINGS_MAX_CONCURRENT_STREAMS`. |
| `h2_initial_window_size` | `SETTINGS_INITIAL_WINDOW_SIZE` (0 … 2³¹−1). |
| `h2_max_frame_size` | `SETTINGS_MAX_FRAME_SIZE` (16384–16777215; invalid values are logged and ignored). |
| `h2_max_header_list_size` | `SETTINGS_MAX_HEADER_LIST_SIZE`. |
| `h2_max_deflate_dynamic_table_size` | nghttp2 HPACK encoder dynamic table size (`nghttp2_option_set_max_deflate_dynamic_table_size`). |
| `h2_max_send_header_block_length` | Max compressed response header block (`nghttp2_option_set_max_send_header_block_length`). |

If **no** `h2_*` SETTINGS keys are set, the server preface uses an empty SETTINGS payload (defaults). If **any** SETTINGS key is set, only those keys are sent in the initial SETTINGS frame (omit parameters you want left at defaults).

### HTTP/2 observability

Counters are maintained **both** process-wide (sum of all drivers) and **per driver** (TLS `h2` traffic accepted by that driver, e.g. `s1/nsssl`). RST/GOAWAY/PING **sent** counts come from nghttp2’s **frame-send** callback; **received** counts from **frame-recv**. `defer_*` tracks deferred stream dispatch (request completed in the reader, queued for the driver). `bytes_fed` is application plaintext bytes passed to `mem_recv`; `trysend_drain_reads` counts `DriverRecv`+`mem_recv` cycles inside `NsHttp2TrySend` while `want_write` stays true.

- **Tcl:** `ns_http2 stats` — global dict of all counters below.
- **Tcl:** `ns_http2 stats -driver <fullname>` — same dict for one driver (e.g. `s1/nsssl`).
- **Tcl:** `ns_driver query <fullname>` — includes an `http2` sublist for **that driver** (not global).
- **nsjs:** `ns.http2.stats()` — global object; `ns.http2.stats("s1/nsssl")` — per-driver when built with nghttp2. Used by **`pages/stats-api.js`**, **`pages/stats.jsadp`**, and the dashboard **Drivers** tab.

Field names: `feed_ok`, `feed_mem_recv_err`, `trysend_recoveries`, `sessions_created`, `sessions_destroyed`, `streams_dispatched`, `rst_stream_sent`, `rst_stream_recv`, `session_send_fail`, `bytes_sent`, `bytes_fed`, `ping_recv`, `ping_sent`, `ping_ack_sent`, `goaway_recv`, `goaway_sent`, `defer_appends`, `defer_max_depth`, `trysend_drain_reads`.

### CI regression (conformance workflow)

On push/PR to `main` or `master`, **GitHub Actions** (`.github/workflows/h2spec.yml`, workflow name **conformance**) runs:

1. **`tls-h2spec-http1`** — CMake **`-DNS_WITH_V8=OFF -DNS_USE_SYSTEM_OPENSSL=ON`**, **`openssl`**, **`libssl-dev`**, **`zlib1g-dev`**, **`tcl`** (for **`tests/new/http.test`**), **`ninja`**, etc.; **`h2spec`** and **`tests/h2test/run-h2spec.sh --start-nsd`** (120s per-case timeout); then **`tests/h1test/run-http1-tests.sh --start-nsd`** (**`curl`** HTTP/1.1 and HTTP/1.0 smoke plus legacy **`tests/new/http.test`** against plain **nssock**). The h2 job avoids building **`openssl_ep`** on the runner.

2. **`tls-h3spec`** — separate job: **`-DNS_WITH_HTTP3=ON`** (bundled OpenSSL 3.5+ for QUIC; no **`NS_USE_SYSTEM_OPENSSL`**), **`python3`** for port selection, **`h3spec`** [v0.1.13](https://github.com/kazu-yamamoto/h3spec/releases/tag/v0.1.13) **`h3spec-linux-x86_64`**, and **`tests/h3test/run-h3spec.sh --start-nsd`**.

Local builds still use the bundled OpenSSL 3.5 tree by default; **`-DNS_USE_SYSTEM_OPENSSL=ON`** is optional when HTTP/3 is off. Adjust branches in the workflow file if your default branch differs.

### Debugging with AddressSanitizer / lldb

For intermittent crashes (e.g. many parallel TLS clients right after startup), rebuild with ASan and reproduce under **lldb** with `ASAN_OPTIONS=abort_on_error=1`:

```sh
cmake -S . -B build-asan \
  -DCMAKE_C_FLAGS="-fsanitize=address -g -O1" \
  -DCMAKE_CXX_FLAGS="-fsanitize=address -g -O1" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=address" \
  -DCMAKE_SHARED_LINKER_FLAGS="-fsanitize=address"
cmake --build build-asan -j8
```

Session teardown takes **`h2Lock`** before **`nghttp2_session_del`**. If ASan reports a use-after-free, capture stacks from **both** the driver and connection worker threads.

**Load reproducer (optional):** with **nsd** already listening on HTTPS, run **`tests/h2test/stress-parallel-h2.sh`** (or `tests/h2test/stress-parallel-h2.sh 200`) to fire many parallel **`curl --http2`** requests. To catch faults interactively:

```sh
lldb -- ./build/nsd/nsd -f -t tests/h2test/minimal.tcl
# (lldb) run
# wait for listen, then in another terminal: tests/h2test/stress-parallel-h2.sh
# (lldb) bt all
```

### Quick manual checks

```sh
# After nsd is up (~10 s after start on a quiet config)
curl -sk --http2 --max-time 10 https://127.0.0.1:8443/ -w "http=%{http_version} code=%{http_code}\n"
```

### h2spec (TLS)

With [h2spec](https://github.com/summerwind/h2spec) installed (e.g. `brew install h2spec`):

```sh
h2spec -t -k -h 127.0.0.1 -p 8443 -o 10
```

Full **h2spec** against `tests/h2test/minimal.tcl` has been run at **145 passed, 0 failed, 1 skipped** (146 tests, TLS `-t -k`). Repeat with `tests/h2test/run-h2spec.sh` when changing the HTTP/2 stack. If plain HTTP **8080** is busy, set **`NSSOCK_PORT`** (1024–65535); if the TLS port (**8443**) is busy, set **`H2SPEC_TLS_PORT`** and point **`h2spec -p`** at the same value; use **`NSD_BUILD_DIR`** when the `nsd` binary lives under e.g. **`build-h3`** so loaded modules match.

**Shutdown smoke test (SIGTERM, log checks):** `tests/test-nsd-shutdown.sh --h2` or `--h3` starts `minimal.tcl`, waits for the listener, sends **SIGTERM**, and asserts **`driver: stopped: nsssl`**, **`driver: stopped: nssock`**, and **`nsmain: … exiting`** appear in captured stderr. For HTTP/3, **`NSD_SHUTDOWN_H3_PORT`** pins the QUIC/TLS port; otherwise **`H3SPEC_PORT` inherited from the environment is ignored** so a stale port cannot be mistaken for this run (use **`NSD_SHUTDOWN_H3_PORT`** if you need a fixed port). Optional: **`NSD_SHUTDOWN_PRETERM_SLEEP`** (default **2**), **`NSD_SHUTDOWN_WAIT_SEC`** (default **45**); **`stdbuf -oL -eL`** is used when available so logs flush under redirection. Notable implementation details: **`NsHttp2TrySend`** performs **`DriverRecv` + `mem_recv`** while **`nghttp2_session_want_write`** stays true after **`session_send`** (OpenSSL may need more inbound records before **`SSL_write`** can finish). The driver **`SockReadLine`** path for HTTP/2 must preserve the logical **`Tcl_DString`** length when growing the recv buffer (a prior bug forced **`iov_len`** ~0 and broke **generic/2/1**). **`curl`** over HTTP/2 to `/` should report `2 200`.

The helper script `tests/h2test/h2_ping_client.py` sets **`ENABLE_PUSH` to 0** before the client preface: the **hyper-h2** defaults use `ENABLE_PUSH = 1` for clients, which violates RFC 7540 and is not a fair stand-in for h2spec until corrected. It also assigns each `conn.data_to_send()` result to a variable before `tls.sendall(...)`; a single expression `tls.sendall(conn.data_to_send())` has been observed to stall the SETTINGS exchange with CPython + hyper-h2 (buffer lifetime / evaluation order).

```sh
export AOLSERVER_H2_DEBUG=1
# run nsd; stderr shows SockClose / NsHttp2Feed lines
```

## HTTP/3 (QUIC, ALPN `h3`)

When built with **`-DNS_WITH_HTTP3=ON`**, **nsssl** can open a **UDP** listener (default: same port as TCP TLS, overridable) using **ngtcp2** + **nghttp3** and OpenSSL’s QUIC APIs (**libngtcp2_crypto_ossl**). TCP **ALPN** remains **`h2`** / **`http/1.1`** only; **`h3`** is negotiated on QUIC.

### HTTP/3 configuration (`ns/server/<server>/module/nsssl`)

| Parameter | Purpose |
|-----------|---------|
| `h3` | `1` / `true` to enable QUIC for this driver (requires `certificate` and `key`, same as TLS). |
| `h3_udp_port` | UDP bind port (default: same as `port`, e.g. **8443**). |
| `h3_max_streams_bidi` | Initial max client bidi streams (ngtcp2 transport). |
| `h3_max_idle_timeout_ms` | Idle timeout in milliseconds. |
| `h3_max_tx_udp_payload_size` | `max_tx_udp_payload_size` (1200–65535). |

### HTTP/3 observability

- **Tcl:** `ns_http3 stats` and `ns_http3 stats -driver <fullname>`.
- **Tcl:** `ns_driver query <fullname>` includes an **`http3`** sublist when HTTP/3 is enabled.
- **nsjs:** `ns.http3.stats()` / `ns.http3.stats("s1/nsssl")`; **stats-api** JSON field **`http3`**; dashboard **Drivers** tab.
### h3spec (QUIC)

Install [h3spec](https://github.com/kazu-yamamoto/h3spec/releases) (place the binary at **`tests/h3test/bin/h3spec`** if you want the script to find it automatically, or ensure **`h3spec`** is on your **`PATH`**). The path **`tests/h3test/bin/h3spec`** is gitignored so you can drop the binary there locally without committing it. Then build **nsd** with **`-DNS_WITH_HTTP3=ON`**, then:

```sh
export NSD_BUILD_DIR="$PWD/build-h3"   # or your CMake build dir with HTTP/3
export DYLD_LIBRARY_PATH="$NSD_BUILD_DIR/nsd:$NSD_BUILD_DIR/nsthread:$PWD/deps-install/lib"
export NS_TCL_LIBRARY="$PWD/deps-install/lib/tcl8.6"
./tests/h3test/run-h3spec.sh --start-nsd
```

With **`--start-nsd`**, the script **chooses a free TCP+UDP port** (default scan from **`38443`**, overridable with **`H3SPEC_PORT_BASE`**) via **`python3`**, exports **`H3SPEC_PORT`**, and **`tests/h3test/minimal.tcl`** binds nsssl TLS + QUIC there. It also picks a free **plain HTTP** port and exports **`NSSOCK_PORT`** so nssock does not collide with a busy **`8080`**. Nothing else on the machine is killed. Override with **`H3SPEC_PORT=8443`** (or any free port) if you want a fixed port. Without **`python3`**, it uses **`H3SPEC_PORT_BASE`** alone—set **`H3SPEC_PORT`** if that port is busy.

The script runs **`h3spec -n <host> <port> …`** by default (**`-n`** skips TLS certificate validation for the self-signed **`minimal.tcl`** cert). Set **`H3SPEC_VERIFY_TLS=1`** to validate the server certificate. Same port is used for UDP QUIC and TCP TLS. It waits for **TCP** listen with progress on **stderr**. Env: **`H3SPEC_STARTUP_MAXWAIT`**, **`H3SPEC_STARTUP_EXTRA_SLEEP`**, **`NSD_SHUTDOWN_WAIT_SEC`** (after **`h3spec`** exits, **`SIGTERM`** then **`SIGKILL`** to **`nsd`** so teardown cannot hang forever—default **45** seconds). If **`NSD_BUILD_DIR`** is unset, **`run-h3spec.sh`** picks **`build-h3`** when present, else **`build`**, and prepends **`tests/h3test/bin`** to **`PATH`** when **`h3spec`** is there. The same **`NSD_SHUTDOWN_WAIT_SEC`** behavior applies to **`tests/h2test/run-h2spec.sh --start-nsd`**.

**IDE / agent runs:** Full h3spec can take many minutes. Use **`--match '…one test…'`** and **`-t 3000`** for quick checks, or run the full suite in a normal terminal.

**ngtcp2 patch (bundled deps):** Stock ngtcp2 1.21.0 discards server Initial packets in UDP datagrams smaller than 1200 bytes before TLS runs. h3spec’s TLS 8.2 cases use a minimal ClientHello (short datagram), so the tree applies **`cmake/patches/ngtcp2-1.21.0-allow-undersized-initial.patch`** via **`cmake/patches/apply-ngtcp2-undersized-initial.sh`** during the **`ngtcp2_ep`** ExternalProject step. The QUIC stack also enforces **`quic_transport_parameters`** (extension type 57) in ClientHello via OpenSSL’s **`SSL_CTX_set_client_hello_cb`** so missing-extension failures surface as TLS alert 109 without breaking transport-parameter decode errors (e.g. missing **`initial_source_connection_id`**).
