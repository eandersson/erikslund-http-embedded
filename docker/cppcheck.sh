#!/usr/bin/env bash
set -uo pipefail

: "${BUILD_DIR:=/build/lint-cmake}"
: "${JOBS:=$(nproc)}"
: "${TLS:=ON}"
: "${ZLIB:=ON}"

# Configure first so cppcheck sees generated switches and every optional path.
if ! cmake -S /src -B "$BUILD_DIR" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DERIKSLUND_HTTP_TLS="$TLS" \
        -DERIKSLUND_HTTP_ZLIB="$ZLIB" \
        -DERIKSLUND_HTTP_REFLECTION=OFF \
        -DERIKSLUND_HTTP_BUILD_TESTS=OFF \
        -DERIKSLUND_HTTP_BUILD_EXAMPLES=OFF >/dev/null; then
    echo "==> FAIL: cmake configure failed; cppcheck has no generated build_config.hpp to read"
    exit 1
fi

GENERATED_INCLUDE="$BUILD_DIR/generated/include"
if [ ! -f "$GENERATED_INCLUDE/erikslund/http/build_config.hpp" ]; then
    echo "==> FAIL: $GENERATED_INCLUDE/erikslund/http/build_config.hpp is missing"
    exit 1
fi

cd /src
echo "==> $(cppcheck --version) on src/ (warning,performance,portability; exhaustive)"

cppcheck \
    --enable=warning,performance,portability \
    --check-level=exhaustive \
    --std=c++26 \
    --language=c++ \
    --inline-suppr \
    --suppress=missingInclude \
    --suppress=missingIncludeSystem \
    --suppress=normalCheckLevelMaxBranches \
    --suppress=checkersReport \
    --error-exitcode=1 \
    --quiet \
    -j "$JOBS" \
    -I include \
    -I src \
    -I "$GENERATED_INCLUDE" \
    src/
status=$?

if [ "$status" -eq 0 ]; then
    echo "==> OK: cppcheck clean"
else
    echo "==> FAIL: cppcheck reported findings (exit $status)"
fi
exit "$status"
