#!/usr/bin/env bash
set -euo pipefail

: "${BUILD_DIR:=/build/lint-cmake}"
: "${CONFIG_FILE:=/src/.clang-tidy}"
: "${LINT_DB_DIR:=/build/lint-db}"
: "${JOBS:=$(nproc)}"
# Clang 19 cannot build an AST while GCC 16-only flags are present.
DEFAULT_STRIP_FLAGS="-fcontracts -fcontract-evaluation-semantic= -freflection"
DEFAULT_STRIP_FLAGS="$DEFAULT_STRIP_FLAGS -fhardened -Wno-maybe-uninitialized"
: "${STRIP_FLAGS:=$DEFAULT_STRIP_FLAGS}"
: "${TLS:=ON}"
: "${ZLIB:=ON}"

echo "==> configure (TLS=$TLS ZLIB=$ZLIB)"
cmake -S /src -B "$BUILD_DIR" \
    -DCMAKE_BUILD_TYPE=Debug \
    -DERIKSLUND_HTTP_TLS="$TLS" \
    -DERIKSLUND_HTTP_ZLIB="$ZLIB" \
    -DERIKSLUND_HTTP_REFLECTION=OFF \
    -DERIKSLUND_HTTP_BUILD_TESTS=OFF \
    -DERIKSLUND_HTTP_BUILD_EXAMPLES=OFF >/dev/null

# Keep CMake's GCC database intact and give clang-tidy a compatible copy.
echo "==> rewrite compile commands, stripping:$STRIP_FLAGS"
mkdir -p "$LINT_DB_DIR"
strip_script=''
for flag in $STRIP_FLAGS; do
    escaped=$(printf '%s' "$flag" | sed 's/[][\\/$*.^]/\\&/g')
    case $flag in
    *=) strip_script="${strip_script}s/ ${escaped}[^ \"]*\\([ \"]\\)/\\1/g;" ;;
    *) strip_script="${strip_script}s/ ${escaped}\\([ \"]\\)/\\1/g;" ;;
    esac
done
sed "$strip_script" "$BUILD_DIR/compile_commands.json" >"$LINT_DB_DIR/compile_commands.json"

for flag in $STRIP_FLAGS; do
    if grep -qF -- "$flag" "$LINT_DB_DIR/compile_commands.json"; then
        echo "==> FAIL: $flag survived the rewrite -- clang cannot parse this database"
        exit 1
    fi
done

mapfile -t sources < <(find /src/src -name '*.cpp' | sort)
if [ "${#sources[@]}" -eq 0 ]; then
    echo "==> FAIL: no translation units found under /src/src"
    exit 1
fi

files=()
skipped=()
for source in "${sources[@]}"; do
    # The pinned clang frontend cannot parse reflection's <meta> header.
    if grep -qE 'erikslund/http/reflect\.hpp|include[[:space:]]*<meta>' "$source"; then
        skipped+=("$source")
        continue
    fi
    files+=("$source")
done

missing=()
# clang-tidy exits successfully when a source lacks a compile command.
for source in "${files[@]}"; do
    grep -qF "\"$source\"" "$LINT_DB_DIR/compile_commands.json" || missing+=("$source")
done
if [ "${#missing[@]}" -ne 0 ]; then
    echo "==> FAIL: ${#missing[@]} source(s) have no compile command and would be skipped silently:"
    printf '      %s\n' "${missing[@]}"
    echo "      re-run with CLEAN, or check the src/*.cpp glob in CMakeLists.txt"
    exit 1
fi

if [ "${#skipped[@]}" -ne 0 ]; then
    echo "==> note: ${#skipped[@]} reflection translation unit(s) excluded (<meta> is unparseable):"
    printf '%s\n' "${skipped[@]}" | sed 's#^/src/#      #'
fi

if [ ! -f "$CONFIG_FILE" ]; then
    echo "==> FAIL: $CONFIG_FILE not found -- the curated check list is what this gate enforces"
    exit 1
fi

echo "==> clang-tidy on ${#files[@]} translation units"

OUT=/tmp/tidy.d
rm -rf "$OUT"
mkdir -p "$OUT"
export LINT_DB_DIR CONFIG_FILE OUT
# Separate per-file output keeps parallel diagnostics readable.
printf '%s\0' "${files[@]}" | xargs -0 -P "$JOBS" -I{} sh -c '
    prefix="$OUT/$(echo "$1" | tr / _)"
    status=0
    clang-tidy -p "$LINT_DB_DIR" --config-file="$CONFIG_FILE" --quiet \
        --export-fixes="$prefix.yaml" "$1" >"$prefix.log" 2>&1 || status=$?
    printf "%s\t%s\n" "$status" "$1" >"$prefix.status"
' _ {}
cat "$OUT"/*.log > /tmp/clang-tidy.log

status_count=$(find "$OUT" -maxdepth 1 -type f -name '*.status' | wc -l)
if [ "$status_count" -ne "${#files[@]}" ]; then
    echo "==> FAIL: clang-tidy completed $status_count of ${#files[@]} translation units"
    exit 1
fi

notes=$(
    grep -hE ': (warning|error): .*\[clang-diagnostic-' /tmp/clang-tidy.log |
        sed 's#^/src/##' | sort -u || true
)
if [ -n "$notes" ]; then
    echo "==> note: clang front-end diagnostics (NOT gated -- GCC is the compiler):"
    echo "$notes" | sed 's/^/      /'
fi

unexpected_statuses=()
# GCC is authoritative; tolerate exits caused only by clang frontend diagnostics.
for status_file in "$OUT"/*.status; do
    IFS=$'\t' read -r status source < "$status_file"
    if [ "$status" -eq 0 ]; then
        continue
    fi

    fixes_file="${status_file%.status}.yaml"
    if [ "$status" -eq 1 ] && [ -f "$fixes_file" ] &&
            grep -qE '^  - DiagnosticName:[[:space:]]+clang-diagnostic-error' "$fixes_file" &&
            [ -z "$(grep -E '^  - DiagnosticName:' "$fixes_file" |
                        grep -vE 'DiagnosticName:[[:space:]]+clang-diagnostic-' || true)" ]; then
        continue
    fi
    unexpected_statuses+=("$status_file")
done

if [ "${#unexpected_statuses[@]}" -ne 0 ]; then
    echo "==> FAIL: clang-tidy did not complete normally:"
    for status_file in "${unexpected_statuses[@]}"; do
        IFS=$'\t' read -r status source < "$status_file"
        echo "      $source (exit $status)"
        tail -40 "${status_file%.status}.log" | sed 's/^/        /'
    done
    exit 1
fi

findings=$(
    grep -hE ': (warning|error): ' /tmp/clang-tidy.log |
        grep -vE '\[clang-diagnostic-' | sed 's#^/src/##' | sort -u || true
)
if [ -n "$findings" ]; then
    echo "==> FAIL: clang-tidy reported $(printf '%s\n' "$findings" | wc -l) finding(s):"
    printf '%s\n' "$findings" | sed 's/^/      /'
    exit 1
fi
echo "==> OK: clang-tidy clean"
