#!/usr/bin/env bash
set -euo pipefail

readonly PLAIN_ORIGIN="http://status-server:7777"
readonly TLS_ORIGIN="https://status-server:7778"
readonly TLS_HOST="status-server"
readonly TLS_PORT=7778
readonly RAW_REQUEST_HOST="status-server"
readonly RAW_REQUEST_PORT=7777
readonly ALLOWLISTED_ORIGIN="http://status-allowlisted:7777"
readonly RESTRICTED_ORIGIN="http://status-restricted:7777"
readonly PROMETHEUS_ORIGIN="http://prometheus:9090"

readonly PROMETHEUS_JOB="status-server"

readonly CERTIFICATE_FILE=/tls/server.crt
readonly DRAIN_FILE=/control/drain

readonly REQUEST_TIMEOUT_SECONDS=5
readonly READY_TIMEOUT_SECONDS=90
readonly POLL_INTERVAL_SECONDS=1

readonly SCRAPE_ADVANCE_TIMEOUT_SECONDS=30

readonly KEEP_ALIVE_REQUEST_COUNT=5

readonly EXPECTED_METRIC_NAMES=(
    status_server_build_info
    status_server_library_build_info
    status_server_requests_total
    status_server_connections_accepted_total
    status_server_connections_active
    status_server_connections_rejected_total
    status_server_bytes_sent_total
    status_server_request_duration_seconds_sum
    status_server_tls_handshakes_total
    status_server_tls_handshake_failures_total
    status_server_uptime_seconds
)

readonly EXPECTED_PAGE_FRAGMENTS=(
    '<title>status-server v0.1.2</title>'
    '<strong>READY</strong>'
    '<tr><td>uptime</td>'
    '<strong>listeners</strong>'
    '<tr><td>plaintext</td>'
    '<tr><td>tls</td>'
    '<a href="/metrics">/metrics</a>'
    '<a href="/healthz">/healthz</a>'
)

ok() {
    printf 'ok: %s\n' "$1"
}

fail() {
    printf 'FAIL: %s\n' "$1" >&2
    exit 1
}

assert_contains() {
    local haystack="$1" needle="$2" sentence="$3"
    if [[ "$haystack" != *"$needle"* ]]; then
        printf '%s\n' "$haystack" | head -n 40 >&2
        fail "${sentence} -- the response does not contain: ${needle}"
    fi
}

assert_absent() {
    local haystack="$1" needle="$2" sentence="$3"
    if [[ "$haystack" == *"$needle"* ]]; then
        printf '%s\n' "$haystack" | head -n 40 >&2
        fail "${sentence} -- the response contains what it must not: ${needle}"
    fi
}

assert_equals() {
    local actual="$1" expected="$2" sentence="$3"
    if [ "$actual" != "$expected" ]; then
        fail "${sentence} -- expected \"${expected}\", got \"${actual}\""
    fi
}

http_body() {
    local url="$1"
    shift
    curl --silent --show-error --fail --max-time "$REQUEST_TIMEOUT_SECONDS" "$@" "$url"
}

http_headers() {
    local url="$1"
    shift
    curl --silent --show-error --fail --max-time "$REQUEST_TIMEOUT_SECONDS" \
        --output /dev/null --dump-header - "$@" "$url"
}

http_status() {
    local url="$1"
    shift
    local code=""
    code=$(curl --silent --output /dev/null --max-time "$REQUEST_TIMEOUT_SECONDS" \
        --write-out '%{http_code}' "$@" "$url" 2>/dev/null) || true
    printf '%s' "${code:-000}"
}

header_value() {
    local transcript="$1" field="$2"
    printf '%s\n' "$transcript" \
        | grep -i "^${field}:" \
        | sed "s/^[^:]*:[[:space:]]*//" \
        | tr -d '\r' \
        | head -n 1
}

elide_uptime() {
    sed 's|<tr><td>uptime</td><td>[^<]*</td></tr>|<tr><td>uptime</td><td>ELIDED</td></tr>|'
}

prometheus_target_field() {
    local field="$1"
    http_body "${PROMETHEUS_ORIGIN}/api/v1/targets?state=active" \
        | jq -r --arg job "$PROMETHEUS_JOB" --arg field "$field" \
            '.data.activeTargets[] | select(.labels.job == $job) | .[$field]'
}

check_wait_healthy() {
    local deadline=$((SECONDS + READY_TIMEOUT_SECONDS))
    while [ "$SECONDS" -lt "$deadline" ]; do
        if [ "$(http_status "${PLAIN_ORIGIN}/healthz")" = "200" ]; then
            ok "the service answers /healthz with 200 over the network"
            return 0
        fi
        sleep "$POLL_INTERVAL_SECONDS"
    done
    fail "the service did not answer /healthz with 200 within ${READY_TIMEOUT_SECONDS}s"
}

check_prometheus_ready() {
    local deadline=$((SECONDS + READY_TIMEOUT_SECONDS))
    while [ "$SECONDS" -lt "$deadline" ]; do
        if [ "$(http_status "${PROMETHEUS_ORIGIN}/-/ready")" = "200" ]; then
            ok "Prometheus is up and accepting queries"
            return 0
        fi
        sleep "$POLL_INTERVAL_SECONDS"
    done
    fail "Prometheus did not become ready within ${READY_TIMEOUT_SECONDS}s"
}

check_status_page() {
    local headers page
    headers=$(http_headers "${PLAIN_ORIGIN}/")
    page=$(http_body "${PLAIN_ORIGIN}/")

    assert_contains "${headers,,}" 'content-type: text/html; charset=utf-8' \
        "the status page is served as HTML"
    local fragment
    for fragment in "${EXPECTED_PAGE_FRAGMENTS[@]}"; do
        assert_contains "$page" "$fragment" "the status page carries its operator rows"
    done

    local alias_page
    alias_page=$(http_body "${PLAIN_ORIGIN}/status" | elide_uptime)
    assert_equals "$alias_page" "$(printf '%s' "$page" | elide_uptime)" \
        "/status renders the same page as the root"

    ok "the status page renders with every expected operator row, and /status mirrors it"
}

check_health_ok() {
    local status body
    status=$(http_status "${PLAIN_ORIGIN}/healthz")
    assert_equals "$status" "200" "/healthz answers 200 while the service is in rotation"
    body=$(http_body "${PLAIN_ORIGIN}/healthz")
    assert_equals "$body" "ok" "/healthz answers with the documented probe body"
    ok "/healthz answers 200 ok while the service is in rotation"
}

check_health_degraded() {
    local status body page
    status=$(http_status "${PLAIN_ORIGIN}/healthz")
    assert_equals "$status" "503" "/healthz answers 503 once the service is drained"

    body=$(curl --silent --max-time "$REQUEST_TIMEOUT_SECONDS" "${PLAIN_ORIGIN}/healthz")
    assert_equals "$body" "degraded" "a degraded probe names itself in the body"

    assert_equals "$(http_status "${PLAIN_ORIGIN}/health")" "503" \
        "both health spellings agree that the service is out of rotation"

    page=$(http_body "${PLAIN_ORIGIN}/")
    assert_contains "$page" '<strong>DRAINING</strong>' \
        "the status page shows an operator that the service is out of rotation"
    ok "/healthz answers 503 degraded and the page reads DRAINING once the service is drained"
}

check_drain_on() {
    : >"$DRAIN_FILE"
    ok "the drain switch is set at ${DRAIN_FILE}"
}

check_drain_off() {
    rm -f "$DRAIN_FILE"
    ok "the drain switch is cleared"
}

check_keep_alive() {
    local -a transfers=()
    local request
    for ((request = 0; request < KEEP_ALIVE_REQUEST_COUNT; request++)); do
        transfers+=(--output /dev/null "${PLAIN_ORIGIN}/hello")
    done

    local transcript
    transcript=$(curl --silent --show-error --fail --max-time "$REQUEST_TIMEOUT_SECONDS" \
        --write-out '%{http_code} %{num_connects}\n' "${transfers[@]}")

    local answered connects
    answered=$(printf '%s\n' "$transcript" \
        | awk '$1 == "200" { total += 1 } END { print total + 0 }')
    connects=$(printf '%s\n' "$transcript" | awk '{ total += $2 } END { print total + 0 }')

    assert_equals "$answered" "$KEEP_ALIVE_REQUEST_COUNT" \
        "every keep-alive request is answered with 200"
    assert_equals "$connects" "1" \
        "${KEEP_ALIVE_REQUEST_COUNT} requests travel over one connection"
    ok "${KEEP_ALIVE_REQUEST_COUNT} requests are served over a single HTTP/1.1 connection"
}

check_conditional_get() {
    local first_headers etag
    first_headers=$(http_headers "${PLAIN_ORIGIN}/hello")
    etag=$(header_value "$first_headers" etag) || true
    if [ -z "$etag" ]; then
        fail "the first fetch published no validator, so no conditional GET is possible"
    fi

    local transcript status size validator
    transcript=$(curl --silent --show-error --max-time "$REQUEST_TIMEOUT_SECONDS" \
        --output /dev/null --dump-header - --header "If-None-Match: ${etag}" \
        --write-out 'HTTP_CODE %{http_code}\nSIZE %{size_download}\n' "${PLAIN_ORIGIN}/hello")

    status=$(printf '%s\n' "$transcript" | awk '$1 == "HTTP_CODE" { print $2 }')
    size=$(printf '%s\n' "$transcript" | awk '$1 == "SIZE" { print $2 }')
    validator=$(header_value "$transcript" etag) || true

    assert_equals "$status" "304" "a second fetch quoting the validator is answered 304"
    assert_equals "$size" "0" "a 304 carries no body"
    assert_equals "$validator" "$etag" \
        "the validator travels with the 304 so a cache can refresh its own record"
    ok "a conditional GET returns 304 with no body and the same validator (${etag})"
}

check_tls_handshake() {
    local transcript
    transcript=$(printf '' | openssl s_client -connect "${TLS_HOST}:${TLS_PORT}" \
        -servername "$TLS_HOST" -CAfile "$CERTIFICATE_FILE" -alpn http/1.1 2>&1) || true

    assert_contains "$transcript" 'ALPN protocol: http/1.1' "ALPN selects the one protocol served"
    assert_contains "$transcript" 'Protocol: TLSv1.3' "the listener negotiates TLS 1.3"
    assert_contains "$transcript" 'Verification: OK' \
        "the served chain verifies against the certificate the stack minted"
    assert_contains "$transcript" 'Negotiated TLS1.3 group: X25519MLKEM768' \
        "the handshake keeps the post-quantum hybrid group"

    local status version
    status=$(http_status "${TLS_ORIGIN}/healthz" --cacert "$CERTIFICATE_FILE")
    assert_equals "$status" "200" "the TLS listener serves the health probe"
    version=$(curl --silent --show-error --fail --max-time "$REQUEST_TIMEOUT_SECONDS" \
        --cacert "$CERTIFICATE_FILE" --output /dev/null --write-out '%{http_version}' \
        "${TLS_ORIGIN}/healthz")
    assert_equals "$version" "1.1" "the protocol spoken over TLS is the one ALPN selected"

    ok "7778 negotiates TLS 1.3 with X25519MLKEM768, ALPN http/1.1, and a verifying chain"
}

check_alpn_h2_refused() {
    local transcript
    transcript=$(printf '' | openssl s_client -connect "${TLS_HOST}:${TLS_PORT}" \
        -servername "$TLS_HOST" -CAfile "$CERTIFICATE_FILE" -alpn h2 2>&1) || true

    assert_absent "$transcript" 'ALPN protocol: h2' \
        "a client offering only h2 must not be told it got h2"
    assert_contains "${transcript,,}" 'no application protocol' \
        "a client offering only h2 is refused with the ALPN alert rather than left hanging"
    ok "a client offering only h2 is refused with no_application_protocol, never given h2"
}

check_listeners_agree() {
    local plain_hello tls_hello plain_page tls_page
    plain_hello=$(http_body "${PLAIN_ORIGIN}/hello")
    tls_hello=$(http_body "${TLS_ORIGIN}/hello" --cacert "$CERTIFICATE_FILE")
    assert_equals "$tls_hello" "$plain_hello" "both listeners serve the same route table"

    plain_page=$(http_body "${PLAIN_ORIGIN}/" | elide_uptime)
    tls_page=$(http_body "${TLS_ORIGIN}/" --cacert "$CERTIFICATE_FILE" | elide_uptime)
    assert_equals "$tls_page" "$plain_page" "both listeners render the same status page"

    ok "the plaintext and TLS listeners serve byte-identical content"
}

check_cidr_allows_listed_peer() {
    local status
    status=$(http_status "${ALLOWLISTED_ORIGIN}/healthz")
    assert_equals "$status" "200" "a peer inside the allowlist is served"
    ok "a CIDR-restricted listener serves a peer named by its allowlist"
}

check_cidr_refuses_outside_peer() {
    local status curl_status=0
    curl --silent --output /dev/null --max-time "$REQUEST_TIMEOUT_SECONDS" \
        "${RESTRICTED_ORIGIN}/healthz" || curl_status=$?
    status=$(http_status "${RESTRICTED_ORIGIN}/healthz")

    assert_equals "$status" "000" "a peer outside the allowlist receives no HTTP response at all"
    case "$curl_status" in
    52 | 56)
        ok "a CIDR-restricted listener closes a peer outside its allowlist without a response"
        ;;
    *)
        fail "a refused peer must see a closed connection, but curl exited ${curl_status}"
        ;;
    esac
}

check_prometheus_scraping() {
    local deadline=$((SECONDS + READY_TIMEOUT_SECONDS))
    local health="" last_error="" scrape_url="" names="" metric missing
    while [ "$SECONDS" -lt "$deadline" ]; do
        health=$(prometheus_target_field health 2>/dev/null) || health=""
        last_error=$(prometheus_target_field lastError 2>/dev/null) || last_error=""
        scrape_url=$(prometheus_target_field scrapeUrl 2>/dev/null) || scrape_url=""
        names=$(http_body "${PROMETHEUS_ORIGIN}/api/v1/label/__name__/values" 2>/dev/null) \
            || names=""

        missing=""
        for metric in "${EXPECTED_METRIC_NAMES[@]}"; do
            if ! printf '%s' "$names" | jq -e --arg name "$metric" \
                '.data | index($name) != null' >/dev/null 2>&1; then
                missing="$metric"
                break
            fi
        done

        if [ "$health" = "up" ] \
            && [ -z "$last_error" ] \
            && [ "$scrape_url" = "http://status-server:7777/metrics" ] \
            && [ -z "$missing" ]; then
            ok "the real Prometheus scrapes /metrics and indexes every expected series name"
            return 0
        fi
        sleep "$POLL_INTERVAL_SECONDS"
    done

    printf 'Prometheus target: health=%q error=%q url=%q missing_metric=%q\n' \
        "$health" "$last_error" "$scrape_url" "$missing" >&2
    fail "Prometheus did not produce a healthy, complete scrape within ${READY_TIMEOUT_SECONDS}s"
}

check_prometheus_scrape_advances() {
    local before after
    before=$(prometheus_target_field lastScrape 2>/dev/null) || before=""
    if [ -z "$before" ]; then
        fail "Prometheus has no record of ever scraping the target"
    fi

    local deadline=$((SECONDS + SCRAPE_ADVANCE_TIMEOUT_SECONDS))
    while [ "$SECONDS" -lt "$deadline" ]; do
        after=$(prometheus_target_field lastScrape 2>/dev/null) || after=""
        if [ -n "$after" ] && [ "$after" != "$before" ]; then
            ok "Prometheus is actively scraping (last scrape moved to ${after})"
            return 0
        fi
        sleep "$POLL_INTERVAL_SECONDS"
    done
    fail "Prometheus has not scraped again within ${SCRAPE_ADVANCE_TIMEOUT_SECONDS}s of ${before}"
}

report_uptime_seconds() {
    http_body "${PLAIN_ORIGIN}/metrics" \
        | awk '$1 == "status_server_uptime_seconds" { print $2; found = 1 }
               END { if (!found) exit 1 }'
}

check_uptime_below() {
    local ceiling="$1" uptime
    uptime=$(report_uptime_seconds)
    if ! awk -v value="$uptime" -v ceiling="$ceiling" 'BEGIN { exit !(value < ceiling) }'; then
        fail "uptime is ${uptime}s, not below ${ceiling}s -- the process was never replaced"
    fi
    ok "the restarted service is a fresh process (${uptime}s of uptime, was ${ceiling}s)"
}

raw_exchange() {
    local authority_fields="$1" target="${2:-/healthz}" answer=""
    exec 3<>"/dev/tcp/${RAW_REQUEST_HOST}/${RAW_REQUEST_PORT}" || {
        printf 'no connection'
        return 0
    }
    printf '%b' "GET ${target} HTTP/1.1\r\n${authority_fields}Connection: close\r\n\r\n" >&3
    answer=$(timeout "$REQUEST_TIMEOUT_SECONDS" head -n 1 <&3 | tr -d '\r') \
        || answer="no status line within ${REQUEST_TIMEOUT_SECONDS}s"
    exec 3>&-
    printf '%s' "$answer"
}

check_host_required() {
    local answer

    answer=$(raw_exchange "Host: ${RAW_REQUEST_HOST}\r\n")
    assert_equals "$answer" "HTTP/1.1 200 OK" \
        "a hand-written request naming one authority is served like any other"

    local -a refusals=(
        "|/healthz|no Host at all on HTTP/1.1"
        "Host: ${RAW_REQUEST_HOST}\r\nHost: evil.example\r\n|/healthz|a second Host"
        "Host: \r\n|/healthz|an empty Host"
        "Host: a b\r\n|/healthz|a Host carrying a space"
        "Host: trusted@evil.example\r\n|/healthz|a Host carrying userinfo"
        "Host: ${RAW_REQUEST_HOST}\r\n|http://evil.example/healthz|a target contradicting Host"
    )
    local entry fields target description
    for entry in "${refusals[@]}"; do
        fields="${entry%%|*}"
        description="${entry##*|}"
        target="${entry#*|}"
        target="${target%%|*}"
        answer=$(raw_exchange "$fields" "$target")
        assert_equals "$answer" "HTTP/1.1 400 Bad Request" \
            "the server answered ${description} with something other than 400"
    done

    ok "a request names exactly one authority or it is refused: ${#refusals[@]} shapes, one control"
}

usage() {
    cat >&2 <<'USAGE'
usage: e2e-checks.sh <check> [argument]

  wait-healthy                 block until /healthz answers 200
  prometheus-ready             block until Prometheus accepts queries
  status-page                  the page renders with every operator row, /status mirrors it
  health-ok                    /healthz is 200 "ok" while in rotation
  drain-on                     set the drain switch
  health-degraded              /healthz is 503 "degraded" and the page reads DRAINING
  drain-off                    clear the drain switch
  keep-alive                   several requests over one connection
  conditional-get              a validator-quoting second fetch is answered 304
  host-required                hand-written requests naming no or two authorities are refused
  tls-handshake                TLS 1.3, ALPN http/1.1, verifying chain, PQ hybrid group
  alpn-h2-refused              a client offering only h2 is refused, never given h2
  listeners-agree              plaintext and TLS serve byte-identical content
  cidr-allows-listed-peer      an allowlisted peer is served
  cidr-refuses-outside-peer    a peer outside the allowlist is closed without a response
  prometheus-scraping          the target is up and every series name is indexed
  prometheus-scrape-advances   the last scrape moves, so scraping is live
  uptime-seconds               print the service's uptime gauge (asserts nothing)
  expect-uptime-below N        the process restarted, so its uptime is below N seconds
USAGE
}

main() {
    local subcommand="${1:-}"
    if [ "$#" -gt 0 ]; then
        shift
    fi

    case "$subcommand" in
    wait-healthy) check_wait_healthy ;;
    prometheus-ready) check_prometheus_ready ;;
    status-page) check_status_page ;;
    health-ok) check_health_ok ;;
    drain-on) check_drain_on ;;
    health-degraded) check_health_degraded ;;
    drain-off) check_drain_off ;;
    keep-alive) check_keep_alive ;;
    conditional-get) check_conditional_get ;;
    host-required) check_host_required ;;
    tls-handshake) check_tls_handshake ;;
    alpn-h2-refused) check_alpn_h2_refused ;;
    listeners-agree) check_listeners_agree ;;
    cidr-allows-listed-peer) check_cidr_allows_listed_peer ;;
    cidr-refuses-outside-peer) check_cidr_refuses_outside_peer ;;
    prometheus-scraping) check_prometheus_scraping ;;
    prometheus-scrape-advances) check_prometheus_scrape_advances ;;
    uptime-seconds) report_uptime_seconds ;;
    expect-uptime-below)
        check_uptime_below "${1:?expect-uptime-below needs a ceiling in seconds}"
        ;;
    "" | -h | --help | help)
        usage
        exit 2
        ;;
    *)
        printf 'FAIL: unknown check "%s"\n' "$subcommand" >&2
        usage
        exit 2
        ;;
    esac
}

main "$@"
