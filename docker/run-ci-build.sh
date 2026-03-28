#!/usr/bin/env bash
# Reproduce GitHub Actions conformance jobs locally using Docker (Ubuntu 24.04).
#
# Usage (from repo root):
#   ./docker/run-ci-build.sh              # configure + ninja build (default build dir: build-docker-ci)
#   ./docker/run-ci-build.sh --shell      # interactive bash in the image with repo mounted
#   ./docker/run-ci-build.sh --h2spec     # build, h2spec, then HTTP/1 tests (set H2SPEC_SKIP_HTTP1=1 for h2spec only)
#   ./docker/run-ci-build.sh --conformance  # full CI: h2spec + HTTP/1 (curl+Tcl) + HTTP/3 build + h3spec
#
# Env:
#   IMAGE          image tag (default: aolserver-ci-ubuntu24)
#   BUILD_DIR      out-of-tree build dir under repo root (default: build-docker-ci)
#   BUILD_DIR_H3   HTTP/3 build dir (default: build-docker-ci-h3)
#   DOCKER_PLATFORM  optional `docker --platform` (e.g. linux/amd64 to match GitHub-hosted runners).
#                    Default is empty (host architecture). For `--h2spec` and `--conformance`, defaults
#                    to linux/amd64 because h2spec/h3spec binaries are x86_64.
#   H2SPEC_TIMEOUT   per-case h2spec -o; values under 180s are raised to 300s for Docker (qemu/amd64).
#   NSSOCK_PORT      plain-HTTP nssock port (default 18080; matches CI .github/workflows/h2spec.yml).
#   H2SPEC_ARGS      optional section filter, e.g. generic/2/2 (passed to run-h2spec.sh).
#   AOLSERVER_H2_FEED_LOG  set to 1 for nghttp2 mem_recv failure lines on stderr (nsd/http2.c).
#   H2SPEC_SKIP_HTTP1     set to 1 to run only h2spec in --h2spec mode (default: also run HTTP/1 like CI).

set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="${IMAGE:-aolserver-ci-ubuntu24}"
BUILD_DIR="${BUILD_DIR:-build-docker-ci}"
BUILD_DIR_H3="${BUILD_DIR_H3:-build-docker-ci-h3}"

MODE="${1:-build}"
if [[ "$MODE" == --* ]]; then
  shift || true
fi

# GitHub Actions uses x86_64; h2spec/h3spec binaries are amd64. Default for --h2spec / --conformance.
if [[ "${DOCKER_PLATFORM:-}" ]]; then
  PLATFORM="$DOCKER_PLATFORM"
elif [[ "${MODE:-}" == "--h2spec" || "${MODE:-}" == "--conformance" ]]; then
  PLATFORM="linux/amd64"
else
  PLATFORM=""
fi

PLATFORM_ARGS=()
if [[ -n "$PLATFORM" ]]; then
  PLATFORM_ARGS=(--platform "$PLATFORM")
fi

# linux/amd64 under emulation is slow; ignore undersized host H2SPEC_TIMEOUT (e.g. 120).
h2spec_docker_timeout() {
  local t="${H2SPEC_TIMEOUT:-300}"
  case "$t" in
    ''|*[!0-9]*) echo 300; return ;;
  esac
  if [[ "$t" -lt 180 ]]; then
    echo 300
  else
    echo "$t"
  fi
}

docker build "${PLATFORM_ARGS[@]}" -t "$IMAGE" -f "$ROOT/docker/Dockerfile.ci" "$ROOT/docker"

run_build() {
  docker run --rm "${PLATFORM_ARGS[@]}" \
    -v "$ROOT:/workspace" \
    -w /workspace \
    "$IMAGE" \
    bash -c "
      set -eo pipefail
      cmake -S . -B '$BUILD_DIR' -G Ninja -DNS_WITH_V8=OFF -DNS_USE_SYSTEM_OPENSSL=ON
      cmake --build '$BUILD_DIR' -j\"\$(nproc)\"
      echo \"OK: built in /workspace/$BUILD_DIR\"
    "
}

case "${MODE:-}" in
  --shell)
    exec docker run --rm -it "${PLATFORM_ARGS[@]}" -v "$ROOT:/workspace" -w /workspace "$IMAGE" bash
    ;;
  --h2spec)
    run_build
    docker run --rm "${PLATFORM_ARGS[@]}" \
      -v "$ROOT:/workspace" \
      -w /workspace \
      -e LD_LIBRARY_PATH="/workspace/$BUILD_DIR/nsd:/workspace/$BUILD_DIR/nsthread:/workspace/deps-install/lib" \
      -e NS_TCL_LIBRARY="/workspace/deps-install/lib/tcl8.6" \
      -e H2SPEC_TIMEOUT="$(h2spec_docker_timeout)" \
      -e H2SPEC_STARTUP_SLEEP="${H2SPEC_STARTUP_SLEEP:-15}" \
      -e HTTP1_STARTUP_SLEEP="${HTTP1_STARTUP_SLEEP:-15}" \
      -e NSSOCK_PORT="${NSSOCK_PORT:-18080}" \
      -e H2SPEC_ARGS="${H2SPEC_ARGS:-}" \
      -e H2SPEC_SKIP_HTTP1="${H2SPEC_SKIP_HTTP1:-}" \
      -e AOLSERVER_H2_FEED_LOG="${AOLSERVER_H2_FEED_LOG:-}" \
      -e JUNIT="${JUNIT:-/workspace/h2spec-docker-junit.xml}" \
      -e BUILD_DIR="$BUILD_DIR" \
      "$IMAGE" \
      bash -c "
        set -eo pipefail
        curl -fsSL https://github.com/summerwind/h2spec/releases/download/v2.6.0/h2spec_linux_amd64.tar.gz | tar xz
        install -m755 h2spec /usr/local/bin/
        export NSD_BIN=\"/workspace/$BUILD_DIR/nsd/nsd\"
        rc_all=0
        if ! tests/h2test/run-h2spec.sh --start-nsd 2>&1 | tee /workspace/h2spec-docker.log; then
          echo 'h2spec: FAILED' >&2
          rc_all=1
        fi
        if [[ \"\${H2SPEC_SKIP_HTTP1:-}\" != \"1\" ]]; then
          echo '=== HTTP/1 (curl + Tcl) ===' >&2
          chmod +x tests/h1test/run-http1-tests.sh
          if ! tests/h1test/run-http1-tests.sh --start-nsd; then
            echo 'HTTP/1 tests: FAILED' >&2
            rc_all=1
          fi
        fi
        exit \"\$rc_all\"
      "
    ;;
  --conformance)
    run_build
    CONF_RC=0
    docker run --rm "${PLATFORM_ARGS[@]}" \
      -v "$ROOT:/workspace" \
      -w /workspace \
      -e LD_LIBRARY_PATH="/workspace/$BUILD_DIR/nsd:/workspace/$BUILD_DIR/nsthread:/workspace/deps-install/lib" \
      -e NS_TCL_LIBRARY="/workspace/deps-install/lib/tcl8.6" \
      -e H2SPEC_TIMEOUT="$(h2spec_docker_timeout)" \
      -e H2SPEC_STARTUP_SLEEP="${H2SPEC_STARTUP_SLEEP:-15}" \
      -e HTTP1_STARTUP_SLEEP="${HTTP1_STARTUP_SLEEP:-15}" \
      -e BUILD_DIR="$BUILD_DIR" \
      "$IMAGE" \
      bash -c "
        set -eo pipefail
        curl -fsSL https://github.com/summerwind/h2spec/releases/download/v2.6.0/h2spec_linux_amd64.tar.gz | tar xz
        install -m755 h2spec /usr/local/bin/
        export NSD_BIN=\"/workspace/$BUILD_DIR/nsd/nsd\"
        rc_all=0
        echo '=== h2spec ==='
        if ! tests/h2test/run-h2spec.sh --start-nsd; then
          echo 'h2spec: FAILED' >&2
          rc_all=1
        else
          echo 'h2spec: OK'
        fi
        echo '=== HTTP/1 (curl + Tcl) ==='
        chmod +x tests/h1test/run-http1-tests.sh
        if ! tests/h1test/run-http1-tests.sh --start-nsd; then
          echo 'HTTP/1 tests: FAILED' >&2
          rc_all=1
        else
          echo 'HTTP/1 tests: OK'
        fi
        exit \"\$rc_all\"
      " || CONF_RC=1
    echo "=== Configure + build HTTP/3 (bundled OpenSSL) ==="
    docker run --rm "${PLATFORM_ARGS[@]}" \
      -v "$ROOT:/workspace" \
      -w /workspace \
      "$IMAGE" \
      bash -c "
        set -eo pipefail
        cmake -S . -B '$BUILD_DIR_H3' -G Ninja -DNS_WITH_V8=OFF -DNS_WITH_HTTP3=ON
        cmake --build '$BUILD_DIR_H3' -j\"\$(nproc)\"
      " || CONF_RC=1
    docker run --rm "${PLATFORM_ARGS[@]}" \
      -v "$ROOT:/workspace" \
      -w /workspace \
      -e LD_LIBRARY_PATH="/workspace/$BUILD_DIR_H3/nsd:/workspace/$BUILD_DIR_H3/nsthread:/workspace/deps-install/lib" \
      -e NS_TCL_LIBRARY="/workspace/deps-install/lib/tcl8.6" \
      -e NSD_BUILD_DIR="/workspace/$BUILD_DIR_H3" \
      -e H3SPEC_STARTUP_EXTRA_SLEEP="${H3SPEC_STARTUP_EXTRA_SLEEP:-15}" \
      "$IMAGE" \
      bash -c "
        set -eo pipefail
        curl -fsSL https://github.com/kazu-yamamoto/h3spec/releases/download/v0.1.13/h3spec-linux-x86_64 -o /tmp/h3spec
        chmod +x /tmp/h3spec
        install -m755 /tmp/h3spec /usr/local/bin/
        export NSD_BIN=\"/workspace/$BUILD_DIR_H3/nsd/nsd\"
        echo '=== h3spec ==='
        tests/h3test/run-h3spec.sh --start-nsd
      " || CONF_RC=1
    if [[ "$CONF_RC" -eq 0 ]]; then
      echo "OK: conformance (h2spec + HTTP/1 + HTTP/3 build + h3spec) — all steps passed"
    else
      echo "FAIL: one or more conformance steps failed (see logs above)" >&2
    fi
    exit "$CONF_RC"
    ;;
  *)
    run_build
    ;;
esac
