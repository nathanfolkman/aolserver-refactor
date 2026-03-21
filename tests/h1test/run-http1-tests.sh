#!/usr/bin/env bash
# HTTP/1 smoke + legacy Tcl conformance tests against plain nssock (tests/h2test/minimal.tcl).
# CI: .github/workflows runs this after h2spec in the same job (separate nsd start).
#
# Usage (repo root):
#   ./tests/h1test/run-http1-tests.sh --start-nsd
#
# Env:
#   NSD_BIN, NSD_CONFIG (default tests/h2test/minimal.tcl), NSD_BUILD_DIR, NSSOCK_PORT
#   HTTP1_STARTUP_SLEEP  seconds to wait after starting nsd (default 12)

set -eo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
NSD_BIN="${NSD_BIN:-$ROOT/build/nsd/nsd}"
CONFIG="${NSD_CONFIG:-$ROOT/tests/h2test/minimal.tcl}"
NSSOCK_PORT="${NSSOCK_PORT:-8080}"

START_NSD=0
if [[ "${1:-}" == "--start-nsd" ]]; then
  START_NSD=1
  shift
fi

cleanup() {
  if [[ -n "${NSD_PID:-}" ]] && kill -0 "$NSD_PID" 2>/dev/null; then
    kill "$NSD_PID" 2>/dev/null || true
    wait "$NSD_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

if [[ "$START_NSD" -ne 1 ]]; then
  echo "run-http1-tests: use --start-nsd (or extend script for attach-to-running)" >&2
  exit 2
fi

if [[ ! -x "$NSD_BIN" ]]; then
  echo "run-http1-tests: nsd not found at $NSD_BIN (build first)" >&2
  exit 2
fi

"$ROOT/tests/h2test/generate-tls-certs.sh"
nsd_root="$(cd "$(dirname "$NSD_BIN")/.." && pwd)"
_libs="$nsd_root/nsd:$nsd_root/nsthread:$nsd_root/deps/install/lib"
if [[ "$(uname -s)" == "Darwin" ]]; then
  export DYLD_LIBRARY_PATH="${DYLD_LIBRARY_PATH:-$_libs}"
else
  export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-$_libs}"
fi
export NS_TCL_LIBRARY="${NS_TCL_LIBRARY:-$nsd_root/deps/install/lib/tcl8.6}"
export NSD_BUILD_DIR="${NSD_BUILD_DIR:-$nsd_root}"
export NSSOCK_PORT

nsd_args=(-f -t "$CONFIG")
if [[ "$(id -u)" == "0" ]]; then
  nsd_args+=( -u nobody )
fi
( cd "$nsd_root" && exec "$NSD_BIN" "${nsd_args[@]}" ) &
NSD_PID=$!
echo "run-http1-tests: nsd pid=$NSD_PID, NSSOCK_PORT=$NSSOCK_PORT, waiting ${HTTP1_STARTUP_SLEEP:-12}s..." >&2
sleep "${HTTP1_STARTUP_SLEEP:-12}"

if ! kill -0 "$NSD_PID" 2>/dev/null; then
  echo "run-http1-tests: nsd exited before tests" >&2
  exit 2
fi

# --- curl: HTTP/1.1 and HTTP/1.0 status line sanity ---
echo "run-http1-tests: curl smoke http://127.0.0.1:${NSSOCK_PORT}/" >&2
code11=$(curl -sS -o /dev/null -w '%{http_code}' --http1.1 "http://127.0.0.1:${NSSOCK_PORT}/" || true)
if [[ ! "$code11" =~ ^(200|301|302|404)$ ]]; then
  echo "run-http1-tests: unexpected HTTP/1.1 status $code11 for GET /" >&2
  exit 1
fi
code10=$(curl -sS -o /dev/null -w '%{http_code}' --http1.0 "http://127.0.0.1:${NSSOCK_PORT}/" || true)
if [[ ! "$code10" =~ ^(200|301|302|404)$ ]]; then
  echo "run-http1-tests: unexpected HTTP/1.0 status $code10 for GET /" >&2
  exit 1
fi

# --- Tcl: tests/new/http.test (requires tclsh + tcltest, threaded Tcl) ---
if ! command -v tclsh >/dev/null 2>&1; then
  echo "run-http1-tests: tclsh not in PATH; install Tcl (e.g. apt install tcl)" >&2
  exit 2
fi
if ! tclsh -c 'package require tcltest' 2>/dev/null; then
  echo "run-http1-tests: tcltest not available (e.g. apt install tcl)" >&2
  exit 2
fi
if ! tclsh -c 'if {![info exists ::tcl_platform(threaded)] || !$::tcl_platform(threaded)} { exit 1 }'; then
  echo "run-http1-tests: need threaded tclsh for http.test" >&2
  exit 2
fi

export AOLSERVER_HTTP_TEST="127.0.0.1:${NSSOCK_PORT}"
export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-$_libs}"
export NS_TCL_LIBRARY="${NS_TCL_LIBRARY:-$nsd_root/deps/install/lib/tcl8.6}"
echo "run-http1-tests: tclsh tests/new/http.test (AOLSERVER_HTTP_TEST=$AOLSERVER_HTTP_TEST)" >&2
( cd "$ROOT/tests/new" && tclsh http.test )
exit $?
