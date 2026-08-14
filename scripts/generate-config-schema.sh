#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

: "${OUTPUT:=conf/http.schema.json}"

IMAGE=erikslund-http-build
BUILD_VOLUME=erikslund-http-build

GENERATOR=/build/cmake/tools/erikslund_http_write_config_schema

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
    "$@" \
    "$IMAGE"

# Replace the output only after generation succeeds; OUTPUT may be /dev/stdout.
schema_temp=$(mktemp)
trap 'rm -f "$schema_temp"' EXIT

echo "==> generate"
docker run --rm \
    -v "$BUILD_VOLUME:/build" \
    --entrypoint "$GENERATOR" \
    "$@" \
    "$IMAGE" > "$schema_temp"

if [ ! -s "$schema_temp" ]; then
    echo "==> FAIL: the generator produced no output; $OUTPUT left untouched" >&2
    exit 1
fi

cat "$schema_temp" > "$OUTPUT"
echo "==> OK: $OUTPUT ($(wc -c < "$schema_temp") bytes)"
