#!/usr/bin/env bash
# Reproduce the GitHub Actions h2spec job locally using Docker (Ubuntu 24.04).
#
# Usage (from repo root):
#   ./docker/run-ci-build.sh              # configure + ninja build (default build dir: build-docker-ci)
#   ./docker/run-ci-build.sh --shell      # interactive bash in the image with repo mounted
#   ./docker/run-ci-build.sh --h2spec     # build then download h2spec and run run-h2spec.sh --start-nsd
#
# Env:
#   IMAGE          image tag (default: aolserver-ci-ubuntu24)
#   BUILD_DIR      out-of-tree build dir under repo root (default: build-docker-ci)
#   DOCKER_PLATFORM  optional `docker --platform` (e.g. linux/amd64 to match GitHub-hosted runners).
#                    Default is empty (host architecture). For `--h2spec`, defaults to linux/amd64
#                    because the workflow uses h2spec_linux_amd64.

set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
IMAGE="${IMAGE:-aolserver-ci-ubuntu24}"
BUILD_DIR="${BUILD_DIR:-build-docker-ci}"

MODE="${1:-build}"
if [[ "$MODE" == --* ]]; then
  shift || true
fi

# GitHub Actions uses x86_64; h2spec tarball is amd64. Default to that for --h2spec only.
if [[ "${DOCKER_PLATFORM:-}" ]]; then
  PLATFORM="$DOCKER_PLATFORM"
elif [[ "${MODE:-}" == "--h2spec" ]]; then
  PLATFORM="linux/amd64"
else
  PLATFORM=""
fi

PLATFORM_ARGS=()
if [[ -n "$PLATFORM" ]]; then
  PLATFORM_ARGS=(--platform "$PLATFORM")
fi

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
      -e LD_LIBRARY_PATH="/workspace/$BUILD_DIR/nsd:/workspace/$BUILD_DIR/nsthread:/workspace/$BUILD_DIR/deps/install/lib" \
      -e NS_TCL_LIBRARY="/workspace/$BUILD_DIR/deps/install/lib/tcl8.6" \
      -e H2SPEC_TIMEOUT="${H2SPEC_TIMEOUT:-120}" \
      -e H2SPEC_STARTUP_SLEEP="${H2SPEC_STARTUP_SLEEP:-15}" \
      -e BUILD_DIR="$BUILD_DIR" \
      "$IMAGE" \
      bash -c "
        set -eo pipefail
        curl -fsSL https://github.com/summerwind/h2spec/releases/download/v2.6.0/h2spec_linux_amd64.tar.gz | tar xz
        install -m755 h2spec /usr/local/bin/
        export NSD_BIN=\"/workspace/$BUILD_DIR/nsd/nsd\"
        tests/h2test/run-h2spec.sh --start-nsd
      "
    ;;
  *)
    run_build
    ;;
esac
