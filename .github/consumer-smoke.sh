#!/usr/bin/env bash
# Exercise all documented integration paths without supplying hidden usage requirements.
set -euo pipefail

: "${CONSUMER_DIR:=/build/consumer}"
: "${LIBRARY_DIR:=/src}"
: "${SMOKE_PATHS:=subdirectory fetchcontent install}"
: "${BUILD_TYPE:=}"
: "${TLS:=}"
: "${GLAZE:=}"
: "${ZLIB:=}"
: "${EXPERIMENTAL:=}"

# Separate ports avoid TIME_WAIT collisions between paths.
SUBDIRECTORY_PORT=8080
FETCHCONTENT_PORT=8081
INSTALL_PORT=8082

READINESS_TIMEOUT_SECONDS=15
REQUEST_TIMEOUT_SECONDS=10

library_options=()
for option_name in TLS GLAZE ZLIB EXPERIMENTAL; do
    option_value=${!option_name}
    if [ -n "$option_value" ]; then
        library_options+=("-DERIKSLUND_HTTP_$option_name=$option_value")
    fi
done

consumer_pid=""
consumer_log=""

cleanup() {
    local status=$?
    exec 9>&- || true
    if [ -n "$consumer_pid" ] && kill -0 "$consumer_pid" 2>/dev/null; then
        kill -KILL "$consumer_pid" 2>/dev/null || true
        wait "$consumer_pid" 2>/dev/null || true
    fi
    if [ "$status" -ne 0 ] && [ -n "$consumer_log" ] && [ -f "$consumer_log" ]; then
        echo "==> the consumer said:"
        sed 's/^/      /' "$consumer_log"
    fi
}
trap cleanup EXIT

write_consumer_main() {
    local source_dir=$1
    local port=$2

    cat >"$source_dir/main.cpp" <<CPP_END
#include <cstdio>
#include <print>

#include "erikslund/http/http.hpp"

int main() {
    using namespace erikslund::http;

    Router router;
    router.get("/", [](const Request&) { return Response::text("consumed\n"); });

    ServerOptions options = ServerOptions::on_port($port);
    options.listeners[0].bind_address = "127.0.0.1";

    Server server(std::move(router), std::move(options));
    server.start();
    std::println("consumer listening on {}", server.port());

    // The caller closes stdin to stop the server.
    static_cast<void>(std::getchar());

    server.stop();
    server.wait();
}
CPP_END
}

serve_one_request() {
    local binary=$1
    local port=$2

    if [ ! -x "$binary" ]; then
        echo "==> FAIL: the consumer build produced no executable at $binary"
        exit 1
    fi

    echo "==> run it and ask it for a page"
    local run_dir
    run_dir=$(dirname "$binary")
    consumer_log=$run_dir/consumer.log

    # A FIFO lets this script close stdin after the request.
    local stdin_fifo=$run_dir/stdin
    rm -f "$stdin_fifo"
    mkfifo "$stdin_fifo"
    "$binary" <"$stdin_fifo" >"$consumer_log" 2>&1 &
    consumer_pid=$!
    exec 9>"$stdin_fifo"

    local ready=0
    local deadline=$((SECONDS + READINESS_TIMEOUT_SECONDS))
    while [ "$SECONDS" -lt "$deadline" ]; do
        if ! kill -0 "$consumer_pid" 2>/dev/null; then
            echo "==> FAIL: the consumer exited before it was ready"
            exit 1
        fi
        if curl -sS -o /dev/null --max-time "$REQUEST_TIMEOUT_SECONDS" \
                "http://127.0.0.1:$port/"; then
            ready=1
            break
        fi
        sleep 0.2
    done
    if [ "$ready" -ne 1 ]; then
        echo "==> FAIL: nothing answered on 127.0.0.1:$port within ${READINESS_TIMEOUT_SECONDS}s"
        exit 1
    fi

    local body
    body=$(curl -sS --max-time "$REQUEST_TIMEOUT_SECONDS" "http://127.0.0.1:$port/")
    if [ "$body" != "consumed" ]; then
        echo "==> FAIL: the consumer's own route answered [$body], not [consumed]"
        exit 1
    fi

    echo "==> stop it by closing its stdin"
    exec 9>&-
    local shutdown_status=0
    wait "$consumer_pid" || shutdown_status=$?
    consumer_pid=""
    if [ "$shutdown_status" -ne 0 ]; then
        echo "==> FAIL: the consumer exited $shutdown_status after its stdin closed"
        exit 1
    fi
    # Do not report a successful path's log if a later path fails.
    consumer_log=""
}

smoke_subdirectory() {
    local root=$CONSUMER_DIR/subdirectory
    local source_dir=$root/source
    local build_dir=$root/build

    echo
    echo "======== path 1 of 3: a vendored subtree, added with add_subdirectory ========"
    mkdir -p "$source_dir"

    # Deliberately declares no dialect, flags, or transitive dependencies.
    cat >"$source_dir/CMakeLists.txt" <<CMAKE_END
cmake_minimum_required(VERSION 3.25)
project(erikslund_http_consumer LANGUAGES CXX)

add_subdirectory($LIBRARY_DIR erikslund-http)

add_executable(my_service main.cpp)
target_link_libraries(my_service PRIVATE erikslund::http)
CMAKE_END

    write_consumer_main "$source_dir" "$SUBDIRECTORY_PORT"

    echo "==> configure"
    cmake -S "$source_dir" -B "$build_dir" "${library_options[@]}"

    echo "==> build"
    cmake --build "$build_dir" -j"$(nproc)"

    serve_one_request "$build_dir/my_service" "$SUBDIRECTORY_PORT"
    echo "==> OK: a project that only says add_subdirectory built it, ran it and was served by it"
}

smoke_fetchcontent() {
    local root=$CONSUMER_DIR/fetchcontent
    local source_dir=$root/source
    local build_dir=$root/build
    local archive=$root/erikslund-http.tar.gz

    echo
    echo "======== path 2 of 3: FetchContent ========"
    mkdir -p "$source_dir"

    # A local archive tests the working tree without network access.
    tar -czf "$archive" -C "$LIBRARY_DIR" .

    cat >"$source_dir/CMakeLists.txt" <<CMAKE_END
cmake_minimum_required(VERSION 3.25)
project(erikslund_http_consumer LANGUAGES CXX)

include(FetchContent)
FetchContent_Declare(
    erikslund_http
    URL $archive)
FetchContent_MakeAvailable(erikslund_http)

add_executable(my_service main.cpp)
target_link_libraries(my_service PRIVATE erikslund::http)
CMAKE_END

    write_consumer_main "$source_dir" "$FETCHCONTENT_PORT"

    echo "==> configure"
    cmake -S "$source_dir" -B "$build_dir" "${library_options[@]}"

    echo "==> build"
    cmake --build "$build_dir" -j"$(nproc)"

    serve_one_request "$build_dir/my_service" "$FETCHCONTENT_PORT"
    echo "==> OK: a project that only says FetchContent built it, ran it and was served by it"
}

smoke_install() {
    local root=$CONSUMER_DIR/install
    local library_build_dir=$root/library-build
    local prefix=$root/prefix
    local source_dir=$root/source
    local build_dir=$root/build

    echo
    echo "======== path 3 of 3: cmake --install, then find_package ========"
    mkdir -p "$source_dir"

    local build_type_option=()
    if [ -n "$BUILD_TYPE" ]; then
        build_type_option=(-DCMAKE_BUILD_TYPE="$BUILD_TYPE")
    fi

    echo "==> configure and build the library for installation"
    cmake -S "$LIBRARY_DIR" -B "$library_build_dir" \
        "${build_type_option[@]}" \
        "${library_options[@]}" \
        -DERIKSLUND_HTTP_BUILD_TESTS=OFF \
        -DERIKSLUND_HTTP_BUILD_EXAMPLES=OFF \
        -DERIKSLUND_HTTP_BUILD_TOOLS=OFF
    cmake --build "$library_build_dir" -j"$(nproc)"

    echo "==> install into $prefix"
    cmake --install "$library_build_dir" --prefix "$prefix"

    local config_file
    config_file=$(find "$prefix" -name erikslund-http-config.cmake -print -quit)
    if [ -z "$config_file" ]; then
        echo "==> FAIL: cmake --install wrote no erikslund-http-config.cmake anywhere under $prefix"
        exit 1
    fi
    local package_dir
    package_dir=$(dirname "$config_file")
    local targets_file=$package_dir/erikslund-http-targets.cmake

    echo "==> check what landed in the prefix"
    local archive_file
    archive_file=$(find "$prefix" -name liberikslund_http.a -print -quit)
    if [ -z "$archive_file" ]; then
        echo "==> FAIL: the installed tree carries no liberikslund_http.a"
        exit 1
    fi
    local required_file
    for required_file in \
        "$prefix/include/erikslund/http/http.hpp" \
        "$prefix/include/erikslund/http/build_config.hpp" \
        "$package_dir/erikslund-http-config-version.cmake" \
        "$targets_file"; do
        if [ ! -f "$required_file" ]; then
            echo "==> FAIL: the installed tree is missing $required_file"
            exit 1
        fi
    done

    if ! grep -q "add_library(erikslund::http STATIC IMPORTED)" "$targets_file"; then
        echo "==> FAIL: the installed package exports no target called erikslund::http, which is"
        echo "          the name all three documented integration paths promise. It exports:"
        grep -n "IMPORTED)" "$targets_file" | sed 's/^/      /'
        exit 1
    fi

    if ! grep -q "cxx_std_26" "$targets_file"; then
        echo "==> FAIL: the exported target does not carry cxx_std_26 as a usage requirement"
        exit 1
    fi

    if grep -q "set(erikslund-http_CONTRACTS 1)" "$config_file" \
       && ! grep -q -- "-fcontracts" "$targets_file"; then
        echo "==> FAIL: this build enabled contracts, but the exported target passes no -fcontracts"
        exit 1
    fi

    cat >"$source_dir/CMakeLists.txt" <<CMAKE_END
cmake_minimum_required(VERSION 3.25)
project(erikslund_http_consumer LANGUAGES CXX)

find_package(erikslund-http 0.1 CONFIG REQUIRED)

add_executable(my_service main.cpp)
target_link_libraries(my_service PRIVATE erikslund::http)
CMAKE_END

    write_consumer_main "$source_dir" "$INSTALL_PORT"

    echo "==> configure the consumer against the installed package"
    cmake -S "$source_dir" -B "$build_dir" -DCMAKE_PREFIX_PATH="$prefix"

    echo "==> build"
    cmake --build "$build_dir" -j"$(nproc)"

    serve_one_request "$build_dir/my_service" "$INSTALL_PORT"
    echo "==> OK: a project that only says find_package built it, ran it and was served by it"
}

echo "==> writing throwaway consumer projects into $CONSUMER_DIR"
echo "==> library under test: $LIBRARY_DIR"
echo "==> options forced on it: ${library_options[*]:-none, so the library resolves its own}"
echo "==> paths: $SMOKE_PATHS"
rm -rf "$CONSUMER_DIR"
mkdir -p "$CONSUMER_DIR"

for smoke_path in $SMOKE_PATHS; do
    case $smoke_path in
        subdirectory) smoke_subdirectory ;;
        fetchcontent) smoke_fetchcontent ;;
        install) smoke_install ;;
        *)
            echo "==> FAIL: SMOKE_PATHS names [$smoke_path], which is not one of the three paths"
            exit 1
            ;;
    esac
done

echo
echo "==> OK: every documented way of adding this library built a server that answered a request"
