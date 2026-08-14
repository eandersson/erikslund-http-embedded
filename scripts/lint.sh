#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

IMAGE=erikslund-http-build
LINT_VOLUME=erikslund-http-lint

# Keep Git Bash from rewriting the container side of volume mounts.
export MSYS_NO_PATHCONV=1
SOURCE_DIR=$(pwd -W 2>/dev/null || pwd)

echo "==> image"
docker build -t "$IMAGE" docker

run_gate() {
    local gate=$1
    shift
    docker run --rm \
        -v "$SOURCE_DIR:/src:ro" \
        -v "$LINT_VOLUME:/build" \
        -e TLS -e ZLIB -e JOBS -e BUILD_DIR \
        "$@" \
        --entrypoint "/usr/local/bin/$gate.sh" \
        "$IMAGE"
}

failed=()

echo "==> clang-tidy"
run_gate clang-tidy "$@" || failed+=("clang-tidy")

echo "==> cppcheck"
run_gate cppcheck "$@" || failed+=("cppcheck")

if [ "${#failed[@]}" -ne 0 ]; then
    echo "==> FAIL: ${#failed[@]} of 2 gates objected: ${failed[*]}"
    exit 1
fi
echo "==> OK: clang-tidy and cppcheck both clean"
