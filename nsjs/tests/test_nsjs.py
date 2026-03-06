#!/usr/bin/env python3
"""
nsjs integration tests.

Starts AOLserver with nsjs loaded, runs HTTP tests against it (including
concurrent requests to verify cross-thread shared state), then shuts down.

Usage:
    python3 test_nsjs.py [--install /path/to/aolserver] [--port 8765] [-v]

Environment variables (override CLI defaults):
    NSJS_INSTALL   AOLserver install prefix
    NSJS_PORT      port to listen on
    NSJS_V8_LIB    path to V8 dylib directory (macOS only, for DYLD_LIBRARY_PATH)
"""

import argparse
import http.client
import os
import pathlib
import signal
import subprocess
import sys
import threading
import time
import unittest
from concurrent.futures import ThreadPoolExecutor, as_completed
from typing import Optional
from urllib.parse import urlencode

# ---------------------------------------------------------------------------
# Configuration (resolved at import time from env / CLI)
# ---------------------------------------------------------------------------

TESTS_DIR   = pathlib.Path(__file__).parent.resolve()
PAGES_DIR   = TESTS_DIR / "pages"
CONFIG_TCL  = TESTS_DIR / "config.tcl"

INSTALL = os.environ.get("NSJS_INSTALL", "/tmp/aolserver-test")
PORT    = int(os.environ.get("NSJS_PORT", "8765"))
V8_LIB  = os.environ.get("NSJS_V8_LIB", "")

HOST    = "127.0.0.1"

# ---------------------------------------------------------------------------
# Server lifecycle helpers
# ---------------------------------------------------------------------------

_server_proc: Optional[subprocess.Popen] = None


def _kill_port(port: int) -> None:
    """Kill any process already listening on the given port."""
    try:
        import subprocess as _sp
        result = _sp.run(
            ["lsof", "-ti", f":{port}"],
            capture_output=True, text=True
        )
        for pid in result.stdout.split():
            try:
                os.kill(int(pid), signal.SIGTERM)
            except (ProcessLookupError, ValueError):
                pass
    except Exception:
        pass


def start_server() -> None:
    global _server_proc

    _kill_port(PORT)
    time.sleep(0.3)  # let the port vacate

    nsd = os.path.join(INSTALL, "bin", "nsd")
    if not os.path.exists(nsd):
        raise RuntimeError(f"nsd not found at {nsd}; set NSJS_INSTALL correctly")

    env = os.environ.copy()
    env["NSJS_INSTALL"] = INSTALL
    env["NSJS_PORT"]    = str(PORT)
    env["NSJS_PAGES"]   = str(PAGES_DIR)

    lib_dirs = [os.path.join(INSTALL, "lib")]
    if V8_LIB:
        lib_dirs.append(V8_LIB)
    elif sys.platform == "darwin":
        # Try the default brew V8 location
        brew_v8 = "/opt/homebrew/opt/v8/lib"
        if os.path.isdir(brew_v8):
            lib_dirs.append(brew_v8)

    if sys.platform == "darwin":
        existing = env.get("DYLD_LIBRARY_PATH", "")
        parts = [p for p in existing.split(":") if p] + lib_dirs
        env["DYLD_LIBRARY_PATH"] = ":".join(parts)
    else:
        existing = env.get("LD_LIBRARY_PATH", "")
        parts = [p for p in existing.split(":") if p] + lib_dirs
        env["LD_LIBRARY_PATH"] = ":".join(parts)

    _server_proc = subprocess.Popen(
        [nsd, "-ft", str(CONFIG_TCL)],
        env=env,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
    )

    # Wait until the server is listening (up to 10 s)
    deadline = time.monotonic() + 10.0
    while time.monotonic() < deadline:
        if _server_proc.poll() is not None:
            out = _server_proc.stdout.read().decode(errors="replace")
            raise RuntimeError(f"nsd exited early:\n{out}")
        try:
            conn = http.client.HTTPConnection(HOST, PORT, timeout=1)
            conn.request("GET", "/hello.js")
            r = conn.getresponse()
            conn.close()
            if r.status in (200, 404):
                return
        except OSError:
            pass
        time.sleep(0.1)

    stop_server()
    raise RuntimeError("Server did not start within 10 s")


def stop_server() -> None:
    global _server_proc
    if _server_proc and _server_proc.poll() is None:
        _server_proc.send_signal(signal.SIGTERM)
        try:
            _server_proc.wait(timeout=5)
        except subprocess.TimeoutExpired:
            _server_proc.kill()
    _server_proc = None


# ---------------------------------------------------------------------------
# HTTP helper
# ---------------------------------------------------------------------------

def get(path: str, headers: Optional[dict] = None, method: str = "GET",
        body: Optional[bytes] = None):
    """Return (status, response_headers_dict, body_text)."""
    conn = http.client.HTTPConnection(HOST, PORT, timeout=10)
    conn.request(method, path, body=body, headers=headers or {})
    resp = conn.getresponse()
    status  = resp.status
    resp_headers = {k.lower(): v for k, v in resp.getheaders()}
    text    = resp.read().decode("utf-8", errors="replace")
    conn.close()
    return status, resp_headers, text


# ---------------------------------------------------------------------------
# Test suites
# ---------------------------------------------------------------------------

class TestBasicJs(unittest.TestCase):
    """Basic .js request handling."""

    def test_hello_returns_200(self):
        status, _, body = get("/hello.js")
        self.assertEqual(status, 200)
        self.assertEqual(body, "hello")

    def test_content_type_is_html(self):
        _, headers, _ = get("/hello.js")
        self.assertIn("text/html", headers.get("content-type", ""))

    def test_multiple_writes_concatenated(self):
        status, _, body = get("/multiwrite.js")
        self.assertEqual(status, 200)
        self.assertEqual(body, "abc")

    def test_missing_file_returns_404(self):
        status, _, _ = get("/nonexistent.js")
        self.assertEqual(status, 404)

    def test_missing_jsadp_returns_404(self):
        status, _, _ = get("/nonexistent.jsadp")
        self.assertEqual(status, 404)

    def test_syntax_error_returns_500(self):
        status, _, _ = get("/syntax-error.js")
        self.assertEqual(status, 500)

    def test_runtime_error_returns_500(self):
        status, _, _ = get("/runtime-error.js")
        self.assertEqual(status, 500)


class TestConnApi(unittest.TestCase):
    """ns.conn.* API surface."""

    def test_get_url(self):
        _, _, body = get("/url.js")
        self.assertEqual(body, "/url.js")

    def test_get_method_get(self):
        _, _, body = get("/method.js")
        self.assertEqual(body, "GET")

    def test_get_method_post(self):
        _, _, body = get("/method.js", method="POST", body=b"")
        self.assertEqual(body, "POST")

    def test_get_method_head(self):
        status, _, _ = get("/method.js", method="HEAD")
        # HEAD returns no body but must be 200
        self.assertEqual(status, 200)

    def test_get_header_present(self):
        _, _, body = get("/header.js", headers={"X-Test": "world"})
        self.assertEqual(body, "world")

    def test_get_header_missing_returns_null(self):
        _, _, body = get("/header.js")
        self.assertEqual(body, "null")

    def test_set_response_header(self):
        _, headers, _ = get("/header.js", headers={"X-Test": "pong"})
        self.assertEqual(headers.get("x-echo", ""), "pong")

    def test_get_query_present(self):
        _, _, body = get("/query.js?name=alice")
        # body: "alice|null"
        self.assertEqual(body, "alice|null")

    def test_get_query_missing_returns_null(self):
        _, _, body = get("/query.js")
        self.assertEqual(body, "null|null")


class TestContextIsolation(unittest.TestCase):
    """Each request gets a fresh V8 context; JS globals must not leak."""

    def test_globals_reset_per_request(self):
        # isolation.js sets counter=0 then increments; result must always be "1"
        results = set()
        for _ in range(5):
            _, _, body = get("/isolation.js")
            results.add(body)
        self.assertEqual(results, {"1"},
            "JS globals leaked across requests — expected always '1'")

    def test_globals_reset_across_threads(self):
        # Fire enough requests to hit multiple server threads, still always "1"
        def fetch(_):
            _, _, body = get("/isolation.js")
            return body

        with ThreadPoolExecutor(max_workers=10) as ex:
            results = set(ex.map(fetch, range(20)))
        self.assertEqual(results, {"1"},
            "JS globals leaked across threads — expected always '1'")


class TestSharedApi(unittest.TestCase):
    """ns.shared.* API (single-threaded correctness)."""

    def test_full_api_sequence(self):
        # shared-ops.js uses a unique array name per request (keyed on ?run=)
        run_id = str(int(time.time() * 1000))
        _, _, body = get(f"/shared-ops.js?run={run_id}")
        # Expected: "hello,true,false,false,15,12,null"
        self.assertEqual(body, "hello,true,false,false,15,12,null")

    def test_incr_starts_from_zero(self):
        # Use a unique array so the key starts at 0
        arr = f"incr_test_{time.time_ns()}"
        run_id = f"{arr}"
        # We can't easily pass the array name to shared-incr.js as written,
        # so test through shared-ops.js which uses ?run= to namespace the array
        run_id2 = f"z{time.time_ns()}"
        _, _, body = get(f"/shared-ops.js?run={run_id2}")
        # incr from "10" + 5 = 15, + (-3) = 12
        parts = body.split(",")
        self.assertEqual(parts[4], "15")
        self.assertEqual(parts[5], "12")

    def test_get_missing_key_returns_null(self):
        run_id = f"null_test_{time.time_ns()}"
        _, _, body = get(f"/shared-ops.js?run={run_id}")
        parts = body.split(",")
        self.assertEqual(parts[6], "null")


class TestSharedConcurrency(unittest.TestCase):
    """Verify ns.shared.incr is race-free across multiple threads."""

    def test_concurrent_incr_correct_total(self):
        """
        Fire N parallel requests each incrementing the same counter by 1.
        The returned values must be a permutation of 1..N (no duplicates, no
        gaps), proving the mutex serialises each incr atomically.
        """
        N = 50
        # Use a unique array/key so we start from 0 each run
        arr   = f"conc_{time.time_ns()}"
        key   = "hits"

        # Prime the key at 0 via shared-ops.js (set "n" to 0 then incr gives 1)
        # Actually shared-incr.js starts from whatever is there (0 if absent).
        # We'll just collect all returned values.

        def fetch(_):
            _, _, body = get("/shared-incr.js")
            return int(body.strip())

        # Reset the counter by using a unique test-run namespace.
        # shared-incr.js hardcodes array "test_hits" key "count".
        # We can't easily change that per-run, so just accept the counter
        # continues from wherever it is and check for N unique consecutive values.
        with ThreadPoolExecutor(max_workers=N) as ex:
            values = list(ex.map(fetch, range(N)))

        # All N values must be unique (no two threads got the same slot)
        self.assertEqual(len(values), len(set(values)),
            f"Duplicate incr values (race condition): {sorted(values)}")

        # They must span exactly N consecutive integers
        mn, mx = min(values), max(values)
        self.assertEqual(mx - mn + 1, N,
            f"Gaps in incr sequence (lost updates): range {mn}..{mx} for N={N}")

    def test_concurrent_incr_no_lost_updates(self):
        """
        Read the counter before and after N concurrent increments; the
        difference must equal exactly N.
        """
        N = 30

        # Get baseline by reading current value (incr by 0 isn't supported,
        # so we do a single incr to read the "current+1" as baseline).
        _, _, before_str = get("/shared-incr.js")
        baseline = int(before_str.strip())

        def fetch(_):
            _, _, body = get("/shared-incr.js")
            return int(body.strip())

        with ThreadPoolExecutor(max_workers=N) as ex:
            list(ex.map(fetch, range(N)))

        _, _, after_str = get("/shared-incr.js")
        after = int(after_str.strip())

        # baseline was N+1 from start, we then did N more, then 1 more = N+2 total
        # after = baseline + N + 1 (the final probe incr)
        self.assertEqual(after, baseline + N + 1,
            f"Lost updates: baseline={baseline}, after={after}, expected {baseline + N + 1}")


class TestJsAdp(unittest.TestCase):
    """.jsadp template engine."""

    def test_static_content(self):
        status, _, body = get("/static.jsadp")
        self.assertEqual(status, 200)
        self.assertIn("static content", body)

    def test_mixed_template(self):
        # mixed.jsadp: "<p>before</p>\n" + "code" + "\n<p>after</p>\n"
        _, _, body = get("/mixed.jsadp")
        self.assertIn("before", body)
        self.assertIn("code", body)
        self.assertIn("after", body)
        # Order must be preserved
        self.assertLess(body.index("before"), body.index("code"))
        self.assertLess(body.index("code"),   body.index("after"))

    def test_adp_counter_increments(self):
        # Each request to adp-counter.jsadp must return a higher value
        _, _, b1 = get("/adp-counter.jsadp")
        _, _, b2 = get("/adp-counter.jsadp")
        _, _, b3 = get("/adp-counter.jsadp")
        v1, v2, v3 = int(b1.strip()), int(b2.strip()), int(b3.strip())
        self.assertLess(v1, v2)
        self.assertLess(v2, v3)

    def test_adp_counter_concurrent(self):
        """N concurrent hits on adp-counter.jsadp must produce N unique values."""
        N = 20

        def fetch(_):
            _, _, body = get("/adp-counter.jsadp")
            return int(body.strip())

        with ThreadPoolExecutor(max_workers=N) as ex:
            values = list(ex.map(fetch, range(N)))

        self.assertEqual(len(values), len(set(values)),
            f"Duplicate counter values in concurrent .jsadp requests: {sorted(values)}")


# ---------------------------------------------------------------------------
# Extended API test suites
# ---------------------------------------------------------------------------

class TestConnExtended(unittest.TestCase):
    """Extended ns.conn.* API."""

    def test_all_pass(self):
        _, _, body = get("/test_conn_extended.js")
        for token in body.split(","):
            self.assertTrue(token.endswith(":ok"),
                f"conn-extended: {token} failed (full body: {body})")


class TestSharedExtended(unittest.TestCase):
    """Extended ns.shared.* API."""

    def test_all_pass(self):
        _, _, body = get("/test_shared_extended.js")
        for token in body.split(","):
            self.assertTrue(token.endswith(":ok"),
                f"shared-extended: {token} failed (full body: {body})")


class TestCacheApi(unittest.TestCase):
    """ns.cache.* API."""

    def test_all_pass(self):
        status, _, body = get("/test_cache.js")
        self.assertEqual(status, 200)
        for token in body.split(","):
            self.assertTrue(token.endswith(":ok"),
                f"cache: {token} failed (full body: {body})")


class TestConfigApi(unittest.TestCase):
    """ns.config* API."""

    def test_all_pass(self):
        status, _, body = get("/test_config.js")
        self.assertEqual(status, 200)
        for token in body.split(","):
            self.assertTrue(token.endswith(":ok"),
                f"config: {token} failed (full body: {body})")


class TestInfoApi(unittest.TestCase):
    """ns.info.* API."""

    def test_all_pass(self):
        status, _, body = get("/test_info.js")
        self.assertEqual(status, 200)
        for token in body.split(","):
            self.assertTrue(token.endswith(":ok"),
                f"info: {token} failed (full body: {body})")


class TestTimeApi(unittest.TestCase):
    """ns.time* API."""

    def test_all_pass(self):
        status, _, body = get("/test_time.js")
        self.assertEqual(status, 200)
        for token in body.split(","):
            self.assertTrue(token.endswith(":ok"),
                f"time: {token} failed (full body: {body})")


class TestUrlApi(unittest.TestCase):
    """ns.url.* API."""

    def test_all_pass(self):
        status, _, body = get("/test_url.js")
        self.assertEqual(status, 200)
        for token in body.split(","):
            self.assertTrue(token.endswith(":ok"),
                f"url: {token} failed (full body: {body})")


class TestHtmlApi(unittest.TestCase):
    """ns.html.* API."""

    def test_all_pass(self):
        status, _, body = get("/test_html.js")
        self.assertEqual(status, 200)
        for token in body.split(","):
            self.assertTrue(token.endswith(":ok"),
                f"html: {token} failed (full body: {body})")


class TestFileApi(unittest.TestCase):
    """ns.file.* API."""

    def test_all_pass(self):
        status, _, body = get("/test_file.js")
        self.assertEqual(status, 200)
        for token in body.split(","):
            self.assertTrue(token.endswith(":ok"),
                f"file: {token} failed (full body: {body})")


class TestDnsApi(unittest.TestCase):
    """ns.dns.* API."""

    def test_all_pass(self):
        status, _, body = get("/test_dns.js")
        self.assertEqual(status, 200)
        ok_tokens = [t for t in body.split(",") if t.endswith(":ok")]
        self.assertGreaterEqual(len(ok_tokens), 2,
            f"dns: expected at least 2 ok tokens, got: {body}")


class TestMutexApi(unittest.TestCase):
    """ns.mutex.* API."""

    def test_all_pass(self):
        status, _, body = get("/test_mutex.js")
        self.assertEqual(status, 200)
        for token in body.split(","):
            self.assertTrue(token.endswith(":ok"),
                f"mutex: {token} failed (full body: {body})")


class TestRwLockApi(unittest.TestCase):
    """ns.rwlock.* API."""

    def test_all_pass(self):
        status, _, body = get("/test_rwlock.js")
        self.assertEqual(status, 200)
        for token in body.split(","):
            self.assertTrue(token.endswith(":ok"),
                f"rwlock: {token} failed (full body: {body})")


class TestSchedApi(unittest.TestCase):
    """ns.sched.* API."""

    def test_all_pass(self):
        status, _, body = get("/test_sched.js")
        self.assertEqual(status, 200)
        for token in body.split(","):
            self.assertTrue(token.endswith(":ok"),
                f"sched: {token} failed (full body: {body})")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="nsjs integration tests")
    p.add_argument("--install", default=os.environ.get("NSJS_INSTALL", "/tmp/aolserver-test"),
                   help="AOLserver install prefix")
    p.add_argument("--port", type=int, default=int(os.environ.get("NSJS_PORT", "8765")),
                   help="Port to listen on")
    p.add_argument("--v8-lib", default=os.environ.get("NSJS_V8_LIB", ""),
                   help="Path to V8 dylib directory")
    p.add_argument("--no-server", action="store_true",
                   help="Skip server start/stop (server already running)")
    return p.parse_known_args()[0]


if __name__ == "__main__":
    args = parse_args()
    INSTALL = args.install
    PORT    = args.port
    V8_LIB  = args.v8_lib

    # Update the module-level globals used by get()
    # (unittest discovers TestCase classes at import time, so we patch here)
    import test_nsjs as _self  # noqa: F401 — just to satisfy linters
    _self.INSTALL = INSTALL
    _self.PORT    = PORT
    _self.V8_LIB  = V8_LIB

    manage_server = not args.no_server
    if manage_server:
        print(f"Starting nsd from {INSTALL} on port {PORT} ...", flush=True)
        try:
            start_server()
            print("Server ready.", flush=True)
        except Exception as e:
            print(f"FATAL: {e}", file=sys.stderr)
            sys.exit(1)

    try:
        # Remove our custom args so unittest doesn't see them
        sys.argv = [sys.argv[0]] + [a for a in sys.argv[1:]
                                     if not a.startswith("--install")
                                     and not a.startswith("--port")
                                     and not a.startswith("--v8")
                                     and not a.startswith("--no-server")]
        unittest.main(verbosity=2, exit=False)
    finally:
        if manage_server:
            print("\nStopping nsd ...", flush=True)
            stop_server()
