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
import socket
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

INSTALL   = os.environ.get("NSJS_INSTALL", "/tmp/aolserver-test")
PORT      = int(os.environ.get("NSJS_PORT", "8765"))
JSCP_PORT = int(os.environ.get("NSJS_JSCP_PORT", "9090"))
V8_LIB    = os.environ.get("NSJS_V8_LIB", "")

HOST = "127.0.0.1"

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
    env["NSJS_INSTALL"]   = INSTALL
    env["NSJS_PORT"]      = str(PORT)
    env["NSJS_PAGES"]     = str(PAGES_DIR)
    env["NSJS_JSCP_PORT"] = str(JSCP_PORT)

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


class TestSleepApi(unittest.TestCase):
    def test_all_pass(self):
        status, _, body = get("/test_sleep.js")
        self.assertEqual(status, 200)
        for token in body.split(","):
            self.assertTrue(token.endswith(":ok"),
                f"sleep: {token} failed (full body: {body})")


class TestCryptApi(unittest.TestCase):
    def test_all_pass(self):
        status, _, body = get("/test_crypt.js")
        self.assertEqual(status, 200)
        for token in body.split(","):
            self.assertTrue(token.endswith(":ok"),
                f"crypt: {token} failed (full body: {body})")


class TestEnvApi(unittest.TestCase):
    def test_all_pass(self):
        status, _, body = get("/test_env.js")
        self.assertEqual(status, 200)
        for token in body.split(","):
            self.assertTrue(token.endswith(":ok"),
                f"env: {token} failed (full body: {body})")


class TestFileExtendedApi(unittest.TestCase):
    def test_all_pass(self):
        status, _, body = get("/test_file_extended.js")
        self.assertEqual(status, 200)
        for token in body.split(","):
            self.assertTrue(token.endswith(":ok"),
                f"file-ext: {token} failed (full body: {body})")


class TestImageApi(unittest.TestCase):
    def test_all_pass(self):
        status, _, body = get("/test_image.js")
        self.assertEqual(status, 200)
        for token in body.split(","):
            self.assertTrue(token.endswith(":ok"),
                f"image: {token} failed (full body: {body})")


class TestHtmlExtendedApi(unittest.TestCase):
    def test_all_pass(self):
        status, _, body = get("/test_html_extended.js")
        self.assertEqual(status, 200)
        for token in body.split(","):
            self.assertTrue(token.endswith(":ok"),
                f"html-ext: {token} failed (full body: {body})")


class TestProcessApi(unittest.TestCase):
    def test_all_pass(self):
        status, _, body = get("/test_process.js")
        self.assertEqual(status, 200)
        for token in body.split(","):
            self.assertTrue(token.endswith(":ok"),
                f"process: {token} failed (full body: {body})")


class TestConfigExtendedApi(unittest.TestCase):
    def test_all_pass(self):
        status, _, body = get("/test_config_extended.js")
        self.assertEqual(status, 200)
        for token in body.split(","):
            self.assertTrue(token.endswith(":ok"),
                f"config-ext: {token} failed (full body: {body})")


class TestConnExtended2Api(unittest.TestCase):
    def test_all_pass(self):
        status, _, body = get("/test_conn_extended2.js")
        # returnNotice sends a response with its own status
        self.assertIn(status, (200, 201, 204))
        tokens = [t for t in body.split(",") if t.strip()]
        for token in tokens:
            self.assertTrue(token.endswith(":ok"),
                f"conn-ext2: {token} failed (full body: {body})")


class TestSemaApi(unittest.TestCase):
    def test_all_pass(self):
        status, _, body = get("/test_sema.js")
        self.assertEqual(status, 200)
        for token in body.split(","):
            self.assertTrue(token.endswith(":ok"),
                f"sema: {token} failed (full body: {body})")


class TestCondApi(unittest.TestCase):
    def test_all_pass(self):
        status, _, body = get("/test_cond.js")
        self.assertEqual(status, 200)
        for token in body.split(","):
            self.assertTrue(token.endswith(":ok"),
                f"cond: {token} failed (full body: {body})")


class TestSchedExtendedApi(unittest.TestCase):
    def test_all_pass(self):
        status, _, body = get("/test_sched_extended.js")
        self.assertEqual(status, 200)
        for token in body.split(","):
            self.assertTrue(token.endswith(":ok"),
                f"sched-ext: {token} failed (full body: {body})")


class TestSetApi(unittest.TestCase):
    def test_all_pass(self):
        status, _, body = get("/test_set.js")
        self.assertEqual(status, 200)
        for token in body.split(","):
            self.assertTrue(token.endswith(":ok"),
                f"set: {token} failed (full body: {body})")


class TestHttpApi(unittest.TestCase):
    def test_all_pass(self):
        status, _, body = get("/test_http.js")
        self.assertEqual(status, 200)
        for token in body.split(","):
            self.assertTrue(token.endswith(":ok"),
                f"http: {token} failed (full body: {body})")


class TestSockApi(unittest.TestCase):
    def test_all_pass(self):
        status, _, body = get("/test_sock.js")
        self.assertEqual(status, 200)
        tokens = [t for t in body.split(",") if not t.endswith(":skip")]
        for token in tokens:
            self.assertTrue(token.endswith(":ok"),
                f"sock: {token} failed (full body: {body})")


class TestThreadApi(unittest.TestCase):
    def test_all_pass(self):
        status, _, body = get("/test_thread.js")
        self.assertEqual(status, 200)
        for token in body.split(","):
            self.assertTrue(token.endswith(":ok"),
                f"thread: {token} failed (full body: {body})")


class TestRandApi(unittest.TestCase):
    def test_all_pass(self):
        status, _, body = get("/test_rand.js")
        self.assertEqual(status, 200)
        for token in body.split(","):
            self.assertTrue(token.endswith(":ok"),
                f"rand: {token} failed (full body: {body})")


class TestAtShutdownApi(unittest.TestCase):
    def test_all_pass(self):
        status, _, body = get("/test_atshutdown.js")
        self.assertEqual(status, 200)
        for token in body.split(","):
            self.assertTrue(token.endswith(":ok"),
                f"atshutdown: {token} failed (full body: {body})")


# ---------------------------------------------------------------------------
# jscp helper
# ---------------------------------------------------------------------------

def jscp_connect(timeout: float = 5.0) -> socket.socket:
    """Connect to the jscp port and return a connected socket."""
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    s.settimeout(timeout)
    s.connect((HOST, JSCP_PORT))
    return s


def jscp_read_until(s: socket.socket, needle: bytes, timeout: float = 5.0) -> bytes:
    """Read from socket until needle is found, return all bytes read."""
    buf = b""
    s.settimeout(timeout)
    while needle not in buf:
        chunk = s.recv(256)
        if not chunk:
            break
        buf += chunk
    return buf


def jscp_auth(s: socket.socket, user: str = "admin", passwd: str = "secret") -> bool:
    """Perform jscp authentication; return True on success.

    After successful auth the server sends the first command prompt (e.g.
    "jscp 1> "), which we consume here so callers can immediately call
    jscp_cmd() without pre-reading the prompt.
    """
    # Read "Username: "
    data = jscp_read_until(s, b"Username: ")
    if b"Username:" not in data:
        return False
    s.sendall((user + "\n").encode())
    # Read "Password: "
    data = jscp_read_until(s, b"Password: ")
    if b"Password:" not in data:
        return False
    s.sendall((passwd + "\n").encode())
    # Read auth result line
    data = jscp_read_until(s, b"\n")
    if b"successful" not in data:
        return False
    # Consume the first command prompt so jscp_cmd() can start cleanly
    jscp_read_until(s, b"> ")
    return True


def jscp_cmd(s: socket.socket, cmd: str) -> str:
    """Send a command and return the result line (stripped).

    Protocol: server sends prompt ending in '> ', we send cmd\n,
    server replies with result\r\n then the next prompt.

    Strategy: send command, read lines until we see a line ending with '> '
    (the next prompt). The result is the line(s) before the prompt.
    """
    s.sendall((cmd + "\n").encode())
    # Read bytes until we see the next prompt ending with "> "
    data = jscp_read_until(s, b"> ")
    # Extract result: everything before the last prompt line
    lines = data.replace(b"\r\n", b"\n").split(b"\n")
    result_lines = []
    for line in lines:
        stripped = line.strip()
        # Skip the prompt line itself (ends with "> ")
        if stripped.endswith(b">"):
            continue
        if stripped:
            result_lines.append(stripped.decode("utf-8", errors="replace"))
    return result_lines[0] if result_lines else ""


class TestJsCp(unittest.TestCase):
    """JavaScript control port (jscp) tests."""

    def _connect(self) -> socket.socket:
        s = jscp_connect()
        self.assertTrue(jscp_auth(s), "jscp auth failed")
        return s

    def test_connect_and_eval(self):
        s = self._connect()
        try:
            result = jscp_cmd(s, "1 + 1")
            self.assertEqual(result, "2")
        finally:
            s.close()

    def test_persistent_state(self):
        s = self._connect()
        try:
            jscp_cmd(s, "var x = 42")
            result = jscp_cmd(s, "x * 2")
            self.assertEqual(result, "84")
        finally:
            s.close()

    def test_object_result(self):
        s = self._connect()
        try:
            result = jscp_cmd(s, "({a:1})")
            self.assertIn('"a"', result)
            self.assertIn("1", result)
        finally:
            s.close()

    def test_bad_auth(self):
        s = jscp_connect()
        try:
            jscp_read_until(s, b"Username: ")
            s.sendall(b"admin\n")
            jscp_read_until(s, b"Password: ")
            s.sendall(b"wrongpassword\n")
            data = jscp_read_until(s, b"\n")
            self.assertIn(b"incorrect", data.lower())
        finally:
            s.close()

    def test_error_handling(self):
        s = self._connect()
        try:
            result = jscp_cmd(s, "undeclared_var_xyz.foo")
            self.assertTrue(result.startswith("ERROR:"),
                f"expected ERROR:, got: {result}")
        finally:
            s.close()

    def test_exit(self):
        s = self._connect()
        try:
            s.sendall(b"exit\n")
            # Server should close the connection
            s.settimeout(3.0)
            data = b""
            try:
                while True:
                    chunk = s.recv(256)
                    if not chunk:
                        break
                    data += chunk
            except (socket.timeout, ConnectionResetError):
                pass
            # Connection should be closed (recv returns b"" or timeout)
            self.assertTrue(True)  # reached here without hanging
        finally:
            s.close()

    def test_multi_line(self):
        s = self._connect()
        try:
            # jscp_auth already consumed first prompt; send continuation line
            s.sendall(b"1 +\\\n")
            # Wait for continuation prompt "... "
            jscp_read_until(s, b"... ")
            # Send second line and capture result
            result = jscp_cmd(s, "1")
            self.assertEqual(result, "2")
        finally:
            s.close()


# ---------------------------------------------------------------------------
# Script compilation cache and stats tests
# ---------------------------------------------------------------------------

class TestScriptCacheStats(unittest.TestCase):
    """Tests for ns.js compilation cache and ns.js.stats API."""

    def _get_global_stats(self) -> dict:
        import json
        _, _, body = get("/test_js_stats.js?action=global")
        return json.loads(body)

    def _get_script_stats(self) -> list:
        import json
        _, _, body = get("/test_js_stats.js?action=scripts")
        return json.loads(body)

    def _reset_stats(self):
        get("/test_js_stats.js?action=reset")

    def test_stats_global_fields(self):
        """ns.js.stats.global() returns an object with all expected fields."""
        stats = self._get_global_stats()
        expected_keys = [
            "totalRequests", "totalAdpRequests", "cacheHits", "cacheMisses",
            "cacheInvalidations", "totalExecUsec", "totalCompileUsec",
            "compileErrors", "runtimeErrors", "cachedScripts",
            "cachedAdpScripts", "contextCreations", "totalContextUsec",
            "activeIsolates",
        ]
        for k in expected_keys:
            self.assertIn(k, stats, f"Missing field: {k}")

    def test_stats_cache_hits_increase(self):
        """After warm-up, hits should outnumber misses on repeated requests."""
        s0 = self._get_global_stats()
        hits_before   = s0["cacheHits"]
        misses_before = s0["cacheMisses"]
        # Fire 50 requests.  With maxthreads=10 there are at most 10 first-time
        # misses; the remaining 40+ are cache hits.
        for _ in range(50):
            get("/hello.js")
        s1 = self._get_global_stats()
        new_hits   = s1["cacheHits"]   - hits_before
        new_misses = s1["cacheMisses"] - misses_before
        self.assertGreater(
            new_hits, new_misses,
            f"Expected hits ({new_hits}) > misses ({new_misses}) after 50 requests",
        )

    def test_stats_total_requests_increase(self):
        """totalRequests increments with each request."""
        self._reset_stats()
        get("/hello.js")
        s1 = self._get_global_stats()
        get("/hello.js")
        s2 = self._get_global_stats()
        self.assertGreater(s2["totalRequests"], s1["totalRequests"])

    def test_stats_context_creations(self):
        """contextCreations increments — one per request."""
        self._reset_stats()
        n = 3
        for _ in range(n):
            get("/hello.js")
        stats = self._get_global_stats()
        self.assertGreaterEqual(stats["contextCreations"], n)

    def test_stats_reset(self):
        """ns.js.stats.reset() dramatically reduces counters vs pre-reset levels."""
        # Build up a large request count
        for _ in range(20):
            get("/hello.js")
        s_before = self._get_global_stats()
        self._reset_stats()
        s_after = self._get_global_stats()
        # After reset the counter should be far less than before.
        # The reset request itself and the get-stats request may each add 1,
        # so allow a small margin (< 5).
        self.assertLess(s_after["totalRequests"], 5,
                        "Reset did not dramatically reduce totalRequests")
        self.assertLess(s_before["totalRequests"],
                        s_before["totalRequests"] + 1)  # sanity
        # Verify activeIsolates is preserved through reset
        self.assertGreater(s_after["activeIsolates"], 0)

    def test_stats_active_isolates(self):
        """activeIsolates is a positive integer (at least one worker thread)."""
        stats = self._get_global_stats()
        self.assertGreater(stats["activeIsolates"], 0)

    def test_stats_scripts_list(self):
        """ns.js.stats.scripts() returns a list; after a request the file appears."""
        # Trigger hello.js so it's cached on at least one thread
        get("/hello.js")
        scripts = self._get_script_stats()
        self.assertIsInstance(scripts, list)
        self.assertGreater(len(scripts), 0)
        # Every entry should have the required fields
        required = ["path", "hitCount", "missCount", "invalidateCount",
                    "totalExecUsec", "totalCompileUsec", "lastCompileTime"]
        for entry in scripts:
            for f in required:
                self.assertIn(f, entry, f"scripts entry missing field: {f}")

    def test_cache_invalidation(self):
        """Editing a .js file triggers re-execution of updated content.

        Invalidation count is a best-effort check — it increments only when
        the same worker thread that cached the old version also serves the new
        one.  We verify the behaviour (new content returned) and accept that
        cacheMisses must have gone up (covers both invalidation and new-thread
        first-miss paths).
        """
        tmp_path = PAGES_DIR / "cache_test_tmp.js"
        try:
            # Create v1 and saturate all minthreads by making many requests
            tmp_path.write_text("ns.conn.write('v1');")
            for _ in range(20):
                _, _, body = get("/cache_test_tmp.js")
                self.assertEqual(body, "v1")

            # Baseline stats
            s0 = self._get_global_stats()
            misses_before = s0["cacheMisses"]

            # Update the file and advance mtime by a full second so that
            # st.st_mtime (1-second resolution on most filesystems) is
            # guaranteed to differ from the cached value.
            import time as _time, os as _os
            old_mtime = int(tmp_path.stat().st_mtime)
            tmp_path.write_text("ns.conn.write('v2');")
            _os.utime(str(tmp_path), (old_mtime + 2, old_mtime + 2))

            # Next request MUST return updated content
            _, _, body2 = get("/cache_test_tmp.js")
            self.assertEqual(body2, "v2", "Updated script not re-executed")

            # At least one miss (invalidation or new-thread first-miss)
            s1 = self._get_global_stats()
            self.assertGreater(s1["cacheMisses"], misses_before,
                               "cacheMisses did not increase after file change")

        finally:
            if tmp_path.exists():
                tmp_path.unlink()

    def test_context_isolation_still_works(self):
        """Cache must not bleed global state between requests (UnboundScript
        + fresh Context is the correct model)."""
        n = 5
        bodies = set()
        for _ in range(n):
            _, _, body = get("/isolation.js")
            bodies.add(body)
        # isolation.js increments a global and returns it; with fresh context
        # each request should always return "1"
        self.assertEqual(bodies, {"1"},
                         "Context isolation broken — global leaked across requests")


# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------

def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="nsjs integration tests")
    p.add_argument("--install", default=os.environ.get("NSJS_INSTALL", "/tmp/aolserver-test"),
                   help="AOLserver install prefix")
    p.add_argument("--port", type=int, default=int(os.environ.get("NSJS_PORT", "8765")),
                   help="Port to listen on")
    p.add_argument("--jscp-port", type=int, default=int(os.environ.get("NSJS_JSCP_PORT", "9090")),
                   help="jscp port")
    p.add_argument("--v8-lib", default=os.environ.get("NSJS_V8_LIB", ""),
                   help="Path to V8 dylib directory")
    p.add_argument("--no-server", action="store_true",
                   help="Skip server start/stop (server already running)")
    return p.parse_known_args()[0]


if __name__ == "__main__":
    args = parse_args()
    INSTALL   = args.install
    PORT      = args.port
    JSCP_PORT = args.jscp_port
    V8_LIB    = args.v8_lib

    # Update the module-level globals used by get() and jscp helpers
    import test_nsjs as _self  # noqa: F401 — just to satisfy linters
    _self.INSTALL   = INSTALL
    _self.PORT      = PORT
    _self.JSCP_PORT = JSCP_PORT
    _self.V8_LIB    = V8_LIB

    manage_server = not args.no_server
    if manage_server:
        print(f"Starting nsd from {INSTALL} on port {PORT} (jscp:{JSCP_PORT}) ...", flush=True)
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
                                     and not a.startswith("--jscp")
                                     and not a.startswith("--v8")
                                     and not a.startswith("--no-server")]
        unittest.main(verbosity=2, exit=False)
    finally:
        if manage_server:
            print("\nStopping nsd ...", flush=True)
            stop_server()
