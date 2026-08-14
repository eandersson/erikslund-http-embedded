#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

IMAGE=erikslund-http-build
BUILD_VOLUME=erikslund-http-build

# Keep Git Bash from rewriting the container side of volume mounts.
export MSYS_NO_PATHCONV=1
SOURCE_DIR=$(pwd -W 2>/dev/null || pwd)

echo "==> image"
docker build -t "$IMAGE" docker

echo "==> build"
docker run --rm \
    -v "$SOURCE_DIR:/src:ro" \
    -v "$BUILD_VOLUME:/build" \
    -e RUN_TESTS=0 \
    -e BUILD_TYPE -e CLEAN -e BUILD_DIR \
    -e TLS -e ZLIB -e REFLECTION \
    "$@" \
    "$IMAGE"
