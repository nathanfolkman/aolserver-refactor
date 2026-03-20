#!/usr/bin/env bash
# Explicit shutdown test for AOLserver with HTTP/2 (h2test) or HTTP/3 (h3test) configs.
# Verifies SIGTERM produces a clean exit and expected log lines (driver + nsmain exiting).
#
# Usage (from repo root):
#   ./tests/test-nsd-shutdown.sh --h2
#   ./tests/test-nsd-shutdown.sh --h3
#
# Env (optional):
#   NSD_BIN, NSD_BUILD_DIR, NSSOCK_PORT, NSD_SHUTDOWN_H3_PORT (h3; avoids stale env H3SPEC_PORT)
#   H2SPEC_TLS_PORT (h2, default 8443)
#   NSD_SHUTDOWN_WAIT_SEC        max seconds to wait after SIGTERM (default 45)
#   NSD_SHUTDOWN_PRETERM_SLEEP   seconds to wait before SIGTERM so logs flush (default 2)
#   DYLD_LIBRARY_PATH, NS_TCL_LIBRARY  (set automatically if unset)

set -eo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
MODE=""
H2_TLS_PORT="${H2SPEC_TLS_PORT:-8443}"
WAIT_SEC="${NSD_SHUTDOWN_WAIT_SEC:-45}"
PRETERM_SLEEP="${NSD_SHUTDOWN_PRETERM_SLEEP:-2}"

usage() {
  echo "Usage: $0 --h2 | --h3" >&2
  exit 2
}

pick_free_quic_port() {
  local bind_host="$1"
  local base="${2:-38443}"
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
}

pick_free_tcp_port_avoid() {
  local bind_host="$1"
  local avoid="${2:-0}"
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
}

wait_tcp() {
  local host="$1" port="$2" maxwait="${3:-30}"
  local step=0.25
  local steps=$(( maxwait * 4 ))
  local i
  for ((i = 0; i < steps; i++)); do
    if command -v nc >/dev/null 2>&1; then
      if nc -z -G 1 "$host" "$port" 2>/dev/null || nc -z -w1 "$host" "$port" 2>/dev/null; then
        return 0
      fi
    elif (echo >/dev/tcp/"$host"/"$port") 2>/dev/null; then
      return 0
    fi
    sleep "$step"
  done
  return 1
}

wait_pid_gone() {
  local pid=$1
  local max=$2
  local i=0
  while kill -0 "$pid" 2>/dev/null && (( i < max * 2 )); do
    sleep 0.5
    ((i++)) || true
  done
  ! kill -0 "$pid" 2>/dev/null
}

if [[ "${1:-}" == "--h2" ]]; then
  MODE=h2
elif [[ "${1:-}" == "--h3" ]]; then
  MODE=h3
else
  usage
fi

LOG="$(mktemp -t nsd-shutdown-log.XXXXXX)"
cleanup() {
  rm -f "$LOG"
}
trap cleanup EXIT

if [[ "$MODE" == h2 ]]; then
  CONFIG="$ROOT/tests/h2test/minimal.tcl"
  if [[ -z "${NSD_BUILD_DIR:-}" ]]; then
    NSD_BUILD_DIR="$ROOT/build"
  fi
  NSD_BIN="${NSD_BIN:-$NSD_BUILD_DIR/nsd/nsd}"
  export NSD_BUILD_DIR
  export DYLD_LIBRARY_PATH="${DYLD_LIBRARY_PATH:-$NSD_BUILD_DIR/nsd:$NSD_BUILD_DIR/nsthread:$NSD_BUILD_DIR/deps/install/lib}"
  export NS_TCL_LIBRARY="${NS_TCL_LIBRARY:-$NSD_BUILD_DIR/deps/install/lib/tcl8.6}"
  # Optional: override TLS port in config via env (tests/h2test/minimal.tcl must support H2SPEC_TLS_PORT)
  export H2SPEC_TLS_PORT="$H2_TLS_PORT"
else
  CONFIG="$ROOT/tests/h3test/minimal.tcl"
  if [[ -z "${NSD_BUILD_DIR:-}" ]]; then
    if [[ -x "$ROOT/build-h3/nsd/nsd" ]]; then
      NSD_BUILD_DIR="$ROOT/build-h3"
    else
      NSD_BUILD_DIR="$ROOT/build"
    fi
  fi
  NSD_BIN="${NSD_BIN:-$NSD_BUILD_DIR/nsd/nsd}"
  export NSD_BUILD_DIR
  export DYLD_LIBRARY_PATH="${DYLD_LIBRARY_PATH:-$NSD_BUILD_DIR/nsd:$NSD_BUILD_DIR/nsthread:$NSD_BUILD_DIR/deps/install/lib}"
  export NS_TCL_LIBRARY="${NS_TCL_LIBRARY:-$NSD_BUILD_DIR/deps/install/lib/tcl8.6}"
  # Ignore inherited H3SPEC_PORT from the shell (e.g. after run-h3spec.sh) unless
  # NSD_SHUTDOWN_H3_PORT is set for this invocation.
  if [[ -n "${NSD_SHUTDOWN_H3_PORT:-}" ]]; then
    H3SPEC_PORT="$NSD_SHUTDOWN_H3_PORT"
  else
    unset H3SPEC_PORT
  fi
  if [[ -z "${H3SPEC_PORT:-}" ]]; then
    # Wide range avoids collisions with stale listeners in the 38xxx band.
    BASE=$((45000 + RANDOM % 2000))
    H3SPEC_PORT="$(pick_free_quic_port 127.0.0.1 "$BASE")" || {
      echo "test-nsd-shutdown: could not pick free QUIC/TLS port (python3?)" >&2
      exit 2
    }
  fi
  export H3SPEC_PORT
  export NSSOCK_PORT="${NSSOCK_PORT:-$(pick_free_tcp_port_avoid 127.0.0.1 "$H3SPEC_PORT")}"
fi

if [[ ! -x "$NSD_BIN" ]]; then
  echo "test-nsd-shutdown: nsd not found at $NSD_BIN" >&2
  exit 2
fi

"$ROOT/tests/h2test/generate-tls-certs.sh"

echo "test-nsd-shutdown: mode=$MODE nsd=$NSD_BIN config=$CONFIG log=$LOG" >&2
if [[ "$MODE" == h3 ]]; then
  echo "test-nsd-shutdown: H3SPEC_PORT=$H3SPEC_PORT NSSOCK_PORT=$NSSOCK_PORT" >&2
fi

# Line-buffer stderr when possible (helps log checks when redirecting to a file).
NSD_CMD=("$NSD_BIN" -f -t "$CONFIG")
if command -v stdbuf >/dev/null 2>&1; then
  NSD_CMD=(stdbuf -oL -eL "${NSD_CMD[@]}")
fi
"${NSD_CMD[@]}" >>"$LOG" 2>&1 &
NSD_PID=$!

verify_listener_pid() {
  local port="$1" want="$2"
  if ! command -v lsof >/dev/null 2>&1; then
    return 0
  fi
  local p
  p="$(lsof -nP -iTCP:"${port}" -sTCP:LISTEN 2>/dev/null | awk 'NR==2 {print $2}')"
  if [[ -z "$p" ]]; then
    echo "test-nsd-shutdown: no TCP listener on 127.0.0.1:${port} (race?)" >&2
    return 1
  fi
  if [[ "$p" != "$want" ]]; then
    echo "test-nsd-shutdown: TCP ${port} owned by pid ${p}, expected our nsd ${want} (stale server?)" >&2
    return 1
  fi
  return 0
}

if [[ "$MODE" == h2 ]]; then
  if ! wait_tcp 127.0.0.1 "$H2_TLS_PORT" 30; then
    echo "test-nsd-shutdown: timeout waiting for TLS on 127.0.0.1:${H2_TLS_PORT}" >&2
    kill "$NSD_PID" 2>/dev/null || true
    wait "$NSD_PID" 2>/dev/null || true
    cat "$LOG" >&2
    exit 2
  fi
else
  if ! wait_tcp 127.0.0.1 "$H3SPEC_PORT" 30; then
    echo "test-nsd-shutdown: timeout waiting for TLS on 127.0.0.1:${H3SPEC_PORT}" >&2
    kill "$NSD_PID" 2>/dev/null || true
    wait "$NSD_PID" 2>/dev/null || true
    cat "$LOG" >&2
    exit 2
  fi
fi

if ! kill -0 "$NSD_PID" 2>/dev/null; then
  echo "test-nsd-shutdown: nsd (pid ${NSD_PID}) exited before readiness check" >&2
  cat "$LOG" >&2
  exit 2
fi

if [[ "$MODE" == h2 ]]; then
  verify_listener_pid "$H2_TLS_PORT" "$NSD_PID" || { cat "$LOG" >&2; exit 2; }
  if command -v curl >/dev/null 2>&1; then
    curl -sk --http2 --max-time 5 "https://127.0.0.1:${H2_TLS_PORT}/" -o /dev/null || true
  fi
else
  verify_listener_pid "$H3SPEC_PORT" "$NSD_PID" || { cat "$LOG" >&2; exit 2; }
fi

echo "test-nsd-shutdown: waiting ${PRETERM_SLEEP}s before SIGTERM (log flush)" >&2
sleep "$PRETERM_SLEEP"

echo "test-nsd-shutdown: sending SIGTERM to pid $NSD_PID (wait up to ${WAIT_SEC}s)" >&2
kill -TERM "$NSD_PID" 2>/dev/null || true

if ! wait_pid_gone "$NSD_PID" "$WAIT_SEC"; then
  echo "test-nsd-shutdown: ERROR: nsd still running after ${WAIT_SEC}s; sending SIGKILL" >&2
  kill -9 "$NSD_PID" 2>/dev/null || true
  wait "$NSD_PID" 2>/dev/null || true
  cat "$LOG" >&2
  exit 1
fi

wait "$NSD_PID" 2>/dev/null || true

fail=0
if ! grep -q 'driver: stopped: nsssl' "$LOG"; then
  echo "test-nsd-shutdown: missing log line: driver: stopped: nsssl" >&2
  fail=1
fi
if ! grep -q 'driver: stopped: nssock' "$LOG"; then
  echo "test-nsd-shutdown: missing log line: driver: stopped: nssock" >&2
  fail=1
fi
if ! grep -q 'nsmain:.*exiting' "$LOG"; then
  echo "test-nsd-shutdown: missing log line: nsmain: ... exiting" >&2
  fail=1
fi

if [[ "$fail" -ne 0 ]]; then
  echo "test-nsd-shutdown: --- nsd log ---" >&2
  cat "$LOG" >&2
  exit 1
fi

echo "test-nsd-shutdown: OK (clean shutdown, expected log lines present)" >&2
exit 0
