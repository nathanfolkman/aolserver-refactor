#!/usr/bin/env bash
# Run h2spec against a TLS HTTP/2 AOLserver started with minimal.tcl.
# CI: .github/workflows/h2spec.yml runs this with --start-nsd after a CMake build.
# Usage:
#   ./run-h2spec.sh              # expects nsd already listening on 127.0.0.1:8443
#   ./run-h2spec.sh --start-nsd  # starts build/nsd in background, runs h2spec, stops nsd
# Env when nsd is already running (from repo root; use your build dir instead of build/ if needed):
#   macOS: export DYLD_LIBRARY_PATH="$PWD/build/nsd:$PWD/build/nsthread:$PWD/deps-install/lib"
#   Linux: export LD_LIBRARY_PATH="$PWD/build/nsd:$PWD/build/nsthread:$PWD/deps-install/lib"
#   export NS_TCL_LIBRARY="$PWD/deps-install/lib/tcl8.6"
#
# Optional: H2SPEC_ARGS="generic/2/1" to run a single case; JUNIT=path for -j report.

set -eo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HOST="${H2SPEC_HOST:-127.0.0.1}"
PORT="${H2SPEC_PORT:-8443}"
TIMEOUT="${H2SPEC_TIMEOUT:-10}"
NSD_BIN="${NSD_BIN:-$ROOT/build/nsd/nsd}"
CONFIG="${NSD_CONFIG:-$ROOT/tests/h2test/minimal.tcl}"
JUNIT="${JUNIT:-}"
H2SPEC_ARGS="${H2SPEC_ARGS:-}"

START_NSD=0
if [[ "${1:-}" == "--start-nsd" ]]; then
  START_NSD=1
  shift
fi

extra=()
if [[ -n "$JUNIT" ]]; then
  extra+=(--junit-report "$JUNIT")
fi
# shellcheck disable=2206
specs=( $H2SPEC_ARGS )

h2cmd=(h2spec -t -k -h "$HOST" -p "$PORT" -o "$TIMEOUT")
[[ ${#extra[@]} -gt 0 ]] && h2cmd+=("${extra[@]}")
h2cmd+=("$@")
if [[ ${#specs[@]} -gt 0 ]]; then
  h2cmd=(h2spec "${specs[@]}" -t -k -h "$HOST" -p "$PORT" -o "$TIMEOUT")
  [[ ${#extra[@]} -gt 0 ]] && h2cmd+=("${extra[@]}")
  h2cmd+=("$@")
fi

cleanup() {
  if [[ -n "${NSD_PID:-}" ]] && kill -0 "$NSD_PID" 2>/dev/null; then
    kill "$NSD_PID" 2>/dev/null || true
    wait "$NSD_PID" 2>/dev/null || true
  fi
}
trap cleanup EXIT

if [[ "$START_NSD" -eq 1 ]]; then
  if [[ ! -x "$NSD_BIN" ]]; then
    echo "run-h2spec: nsd not found at $NSD_BIN (build first)" >&2
    exit 2
  fi
  "$ROOT/tests/h2test/generate-tls-certs.sh"
  nsd_root="$(cd "$(dirname "$NSD_BIN")/.." && pwd)"
  repo_root="$(cd "$nsd_root/.." && pwd)"
  _deps_lib="$repo_root/deps-install/lib"
  _libs="$nsd_root/nsd:$nsd_root/nsthread:$_deps_lib"
  if [[ "$(uname -s)" == "Darwin" ]]; then
    export DYLD_LIBRARY_PATH="${DYLD_LIBRARY_PATH:-$_libs}"
  else
    export LD_LIBRARY_PATH="${LD_LIBRARY_PATH:-$_libs}"
  fi
  export NS_TCL_LIBRARY="${NS_TCL_LIBRARY:-$_deps_lib/tcl8.6}"
  export NSD_BUILD_DIR="${NSD_BUILD_DIR:-$nsd_root}"
  # Bundled Tcl/OpenSSL live under <repo>/deps-install (sibling of the build dir).
  # Docker (and any root shell): nsd refuses to run as uid 0 without -u (GitHub Actions is non-root).
  nsd_args=(-f -t "$CONFIG")
  if [[ "$(id -u)" == "0" ]]; then
    nsd_args+=( -u nobody )
  fi
  ( cd "$nsd_root" && exec "$NSD_BIN" "${nsd_args[@]}" ) &
  NSD_PID=$!
  echo "run-h2spec: nsd pid=$NSD_PID, waiting ${H2SPEC_STARTUP_SLEEP:-12}s for readers..."
  sleep "${H2SPEC_STARTUP_SLEEP:-12}"
fi

echo "run-h2spec: ${h2cmd[*]}"
"${h2cmd[@]}"
rc=$?
exit "$rc"
