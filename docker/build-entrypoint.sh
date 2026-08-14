#!/usr/bin/env bash
set -euo pipefail

: "${BUILD_TYPE:=Debug}"
: "${RUN_TESTS:=1}"
: "${CLEAN:=0}"
: "${BUILD_DIR:=/build/cmake}"
: "${TLS:=AUTO}"
: "${ZLIB:=OFF}"
: "${REFLECTION:=ON}"

if [ "$CLEAN" = "1" ]; then
    echo "==> CLEAN: removing $BUILD_DIR"
    rm -rf "$BUILD_DIR"
fi

echo "==> configure (BUILD_TYPE=$BUILD_TYPE TLS=$TLS ZLIB=$ZLIB REFLECTION=$REFLECTION)"
cmake -S /src -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE="$BUILD_TYPE" \
    -DERIKSLUND_HTTP_TLS="$TLS" \
    -DERIKSLUND_HTTP_ZLIB="$ZLIB" \
    -DERIKSLUND_HTTP_REFLECTION="$REFLECTION"

echo "==> build"
cmake --build "$BUILD_DIR" -j"$(nproc)"

if [ "$RUN_TESTS" = "1" ]; then
    if [ -f "$BUILD_DIR/CTestTestfile.cmake" ]; then
        echo "==> test"
        # Make dangling-view failures reproducible in allocator-backed tests.
        export MALLOC_PERTURB_=165
        ctest --test-dir "$BUILD_DIR" --output-on-failure
    else
        echo "==> test SKIPPED: no tests configured in this build tree"
    fi
fi

echo "==> BUILD OK"
