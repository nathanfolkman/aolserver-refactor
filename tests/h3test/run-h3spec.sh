#!/usr/bin/env bash
# Run h3spec against QUIC UDP on the same host/port as tests/h3test/minimal.tcl.
# Install h3spec from: https://github.com/kazu-yamamoto/h3spec/releases
#
# Usage:
#   ./run-h3spec.sh              # expects nsd already running with h3=1
#   ./run-h3spec.sh --start-nsd  # picks a free TCP+UDP port, exports H3SPEC_PORT, starts nsd
#
# Env:
#   Linux: set LD_LIBRARY_PATH to build/nsd:build/nsthread:build/deps/install/lib (run-h3spec.sh sets
#   defaults from NSD_BUILD_DIR when --start-nsd). macOS: DYLD_LIBRARY_PATH.
#   H3SPEC_HOST           default 127.0.0.1
#   H3SPEC_PORT           force port (must match running nsd if not --start-nsd)
#   H3SPEC_PORT_BASE      first port to try when auto-picking (default 38443)
#   NSSOCK_PORT           set by --start-nsd for plain HTTP (minimal.tcl); avoids 8080 clashes
#   NSD_BIN, NSD_CONFIG, NSD_BUILD_DIR
#   H3SPEC_STARTUP_MAXWAIT, H3SPEC_STARTUP_EXTRA_SLEEP
#   H3SPEC_VERIFY_TLS=1     validate server cert (default: pass h3spec -n for self-signed minimal.tcl)
#
# With --start-nsd, a free port is chosen (no killing other processes). If python3
# is missing, H3SPEC_PORT_BASE is used as a single guess (set H3SPEC_PORT if it clashes).

set -eo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
HOST="${H3SPEC_HOST:-127.0.0.1}"
# PORT set after optional auto-pick when --start-nsd
# Prefer vendored h3spec binary when present (see README / releases).
case ":$PATH:" in
  *:"$ROOT/tests/h3test/bin":*) ;;
  *) PATH="$ROOT/tests/h3test/bin:$PATH" ;;
esac
export PATH
if [[ -z "${NSD_BUILD_DIR:-}" ]]; then
  if [[ -x "$ROOT/build-h3/nsd/nsd" ]]; then
    NSD_BUILD_DIR="$ROOT/build-h3"
  else
    NSD_BUILD_DIR="$ROOT/build"
  fi
fi
NSD_BIN="${NSD_BIN:-$NSD_BUILD_DIR/nsd/nsd}"
export NSD_BUILD_DIR
CONFIG="${NSD_CONFIG:-$ROOT/tests/h3test/minimal.tcl}"

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

# Pick a port where both TCP and UDP listen can succeed on HOST (nsssl needs both).
pick_free_quic_port() {
  local bind_host="$1"
  local base="${2:-38443}"
  if command -v python3 >/dev/null 2>&1; then
    python3 - "$bind_host" "$base" <<'PY'
import socket
import sys

host = sys.argv[1]
base = int(sys.argv[2])
for port in range(base, min(base + 512, 65536)):
    ok = True
    for kind in (socket.SOCK_STREAM, socket.SOCK_DGRAM):
        s = socket.socket(socket.AF_INET, kind)
        try:
            s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            s.bind((host, port))
        except OSError:
            ok = False
            break
        finally:
            s.close()
    if ok:
        print(port)
        sys.exit(0)
sys.exit(1)
PY
    return
  fi
  echo "$base"
}

# TCP-only free port (nssock); avoid clashing with QUIC/TLS port.
pick_free_tcp_port() {
  local bind_host="$1"
  local avoid="${2:-0}"
  if command -v python3 >/dev/null 2>&1; then
    python3 - "$bind_host" "$avoid" <<'PY'
import socket
import sys

host = sys.argv[1]
avoid = int(sys.argv[2])
for port in range(28000, min(28000 + 512, 65536)):
    if port == avoid:
        continue
    s = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
    try:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind((host, port))
    except OSError:
        continue
    finally:
        s.close()
    print(port)
    sys.exit(0)
sys.exit(1)
PY
    return
  fi
  echo "28080"
}

# Wait until nsssl accepts TCP on HOST:PORT (same as minimal.tcl TLS listen).
wait_nsd_tcp() {
  local maxwait="${H3SPEC_STARTUP_MAXWAIT:-25}"
  local step=0.25
  local steps=$(( maxwait * 4 ))
  local i
  echo "run-h3spec: waiting for TCP ${HOST}:${PORT} (max ${maxwait}s, ${step}s steps)..." >&2
  for ((i = 0; i < steps; i++)); do
    if (echo >/dev/tcp/"$HOST"/"$PORT") 2>/dev/null; then
      echo "run-h3spec: nsd listening on ${HOST}:${PORT}" >&2
      return 0
    fi
    if command -v nc >/dev/null 2>&1; then
      if nc -z -G 1 "$HOST" "$PORT" 2>/dev/null || nc -z -w1 "$HOST" "$PORT" 2>/dev/null; then
        echo "run-h3spec: nsd listening on ${HOST}:${PORT} (nc)" >&2
        return 0
      fi
    fi
    if (( i % 4 == 0 && i > 0 )); then
      echo "run-h3spec: still waiting for nsd... $((i / 4))s / ${maxwait}s" >&2
    fi
    sleep "$step"
  done
  echo "run-h3spec: timeout: no TCP listener on ${HOST}:${PORT} (nsd crashed or wrong port?)" >&2
  return 1
}

if [[ "$START_NSD" -eq 1 ]]; then
  if [[ -n "${H3SPEC_PORT:-}" ]]; then
    PORT="$H3SPEC_PORT"
    echo "run-h3spec: using H3SPEC_PORT=${PORT} (fixed)" >&2
  else
    # Jitter reduces collisions when several runs start together (parallel agents).
    BASE="${H3SPEC_PORT_BASE:-38443}"
    BASE=$((BASE + RANDOM % 400))
    if (( BASE > 65000 )); then BASE=$((H3SPEC_PORT_BASE + RANDOM % 200)); fi
    if command -v python3 >/dev/null 2>&1; then
      if ! PORT="$(pick_free_quic_port "$HOST" "$BASE")"; then
	echo "run-h3spec: no free TCP+UDP port from ${BASE}..$((BASE + 511)) (install python3?)" >&2
	exit 2
      fi
      echo "run-h3spec: picked free QUIC/TLS port ${PORT} (scan from ${BASE})" >&2
    else
      PORT="$BASE"
      echo "run-h3spec: no python3 — using port ${PORT} (H3SPEC_PORT_BASE); set H3SPEC_PORT if bind fails" >&2
    fi
  fi
  export H3SPEC_PORT="$PORT"
  if NSSOCK_PORT="$(pick_free_tcp_port "$HOST" "$PORT")"; then
    export NSSOCK_PORT
    echo "run-h3spec: nssock TCP port ${NSSOCK_PORT} (NSSOCK_PORT)" >&2
  else
    export NSSOCK_PORT="${NSSOCK_PORT:-28080}"
    echo "run-h3spec: warning: could not pick NSSOCK_PORT; using ${NSSOCK_PORT}" >&2
  fi

  if [[ ! -x "$NSD_BIN" ]]; then
    echo "run-h3spec: nsd not found at $NSD_BIN (build with -DNS_WITH_HTTP3=ON)" >&2
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
  nsd_args=(-f -t "$CONFIG")
  if [[ "$(id -u)" == "0" ]]; then
    nsd_args+=( -u nobody )
  fi
  echo "run-h3spec: starting nsd: $NSD_BIN" >&2
  ( cd "$nsd_root" && exec "$NSD_BIN" "${nsd_args[@]}" ) &
  NSD_PID=$!
  echo "run-h3spec: nsd pid=$NSD_PID" >&2
  wait_nsd_tcp || exit 2
  if ! kill -0 "$NSD_PID" 2>/dev/null; then
    echo "run-h3spec: nsd process exited (nsssl bind failed or fatal error?); another app may own TCP ${PORT}" >&2
    exit 2
  fi
  if [[ "${H3SPEC_STARTUP_EXTRA_SLEEP:-0}" != "0" ]]; then
    echo "run-h3spec: extra sleep ${H3SPEC_STARTUP_EXTRA_SLEEP}s (H3SPEC_STARTUP_EXTRA_SLEEP)" >&2
    sleep "${H3SPEC_STARTUP_EXTRA_SLEEP}"
  fi
else
  PORT="${H3SPEC_PORT:-8443}"
fi

if ! command -v h3spec >/dev/null 2>&1; then
  echo "run-h3spec: h3spec not in PATH (see https://github.com/kazu-yamamoto/h3spec/releases)" >&2
  exit 2
fi

H3SPEC_TLS_ARGS=()
if [[ "${H3SPEC_VERIFY_TLS:-}" != "1" ]]; then
  H3SPEC_TLS_ARGS=( -n )
fi
echo "run-h3spec: launching h3spec $HOST $PORT $* (h3spec -n unless H3SPEC_VERIFY_TLS=1)" >&2
h3spec "${H3SPEC_TLS_ARGS[@]}" "$HOST" "$PORT" "$@"
exit $?
