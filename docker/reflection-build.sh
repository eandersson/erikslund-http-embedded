#!/usr/bin/env bash
set -euo pipefail

: "${BUILD_DIR:=/build/reflection-cmake}"

REFLECTION_TEST_CASE='add_rows_from derives one row label per member'

export BUILD_DIR
export REFLECTION=ON
# Keep a dedicated assertion that the reflection suites are present.
/usr/local/bin/build-entrypoint.sh "$@"

TEST_BINARY="$BUILD_DIR/tests/erikslund_http_tests"
if [ ! -x "$TEST_BINARY" ]; then
    echo "==> FAIL: $TEST_BINARY was not built, so nothing instantiated reflect.hpp"
    exit 1
fi

echo "==> verify the reflection suites were compiled in"
# Capture first to avoid a pipefail/SIGPIPE false result from grep -q.
listing=$("$TEST_BINARY" --list-test-cases)
if ! grep -qF "$REFLECTION_TEST_CASE" <<<"$listing"; then
    echo "==> FAIL: the reflection test cases are absent from a REFLECTION=ON build."
    echo "      Check that the option reaches test_reflect.cpp and its suites are registered."
    exit 1
fi

echo "==> OK: reflect.hpp compiled and its suites ran"
