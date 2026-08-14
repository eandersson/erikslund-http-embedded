#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."

readonly COMPOSE_FILE=e2e/docker-compose.yml
readonly TOOLCHAIN_CONTEXT=docker
readonly TOOLCHAIN_IMAGE=erikslund-http-build
readonly LOG_DIRECTORY=e2e/logs

readonly SERVICE_LOG_LINES=40

readonly HEALTH_TIMEOUT_SECONDS=120
readonly POLL_INTERVAL_SECONDS=1

readonly HEALTHCHECKED_SERVICES=(status-server status-allowlisted status-restricted)

compose() {
    docker compose --file "$COMPOSE_FILE" "$@"
}

tester() {
    docker compose --progress quiet --file "$COMPOSE_FILE" run --rm --no-deps tester "$@"
}

step() {
    printf '\n==> %s\n' "$1"
}

retain_logs() {
    local stamp archive service
    stamp=$(date -u +%Y%m%dT%H%M%SZ)
    archive="${LOG_DIRECTORY}/${stamp}"
    mkdir -p "$archive"

    compose ps --all >"${archive}/containers.txt" 2>&1 || true
    for service in $(compose config --services); do
        compose logs --no-color --timestamps "$service" >"${archive}/${service}.log" 2>&1 || true
    done

    printf '\n==> FAILED. Full logs archived under %s\n' "$archive"
    printf '==> last %d lines from status-server:\n' "$SERVICE_LOG_LINES"
    compose logs --no-color --tail "$SERVICE_LOG_LINES" status-server || true
}

finish() {
    local status=$?
    if [ "$status" -ne 0 ]; then
        retain_logs
    fi
    printf '\n==> tearing the stack down\n'
    compose down --volumes --remove-orphans --timeout 5 >/dev/null 2>&1 || true
    exit "$status"
}

wait_for_health() {
    local service="$1"
    local deadline=$((SECONDS + HEALTH_TIMEOUT_SECONDS))
    local container health
    while [ "$SECONDS" -lt "$deadline" ]; do
        container=$(compose ps --quiet "$service" 2>/dev/null | head -n 1)
        if [ -n "$container" ]; then
            health=$(docker inspect --format '{{.State.Health.Status}}' "$container" 2>/dev/null) \
                || health=""
            if [ "$health" = "healthy" ]; then
                printf 'ok: %s reports healthy to docker\n' "$service"
                return 0
            fi
        fi
        sleep "$POLL_INTERVAL_SECONDS"
    done
    printf 'FAIL: %s never reported healthy within %ds\n' "$service" "$HEALTH_TIMEOUT_SECONDS" >&2
    return 1
}

wait_for_stack() {
    local service
    for service in "${HEALTHCHECKED_SERVICES[@]}"; do
        wait_for_health "$service"
    done
    tester prometheus-ready
}

bring_the_stack_up() {
    step "toolchain image"
    docker build --tag "$TOOLCHAIN_IMAGE" "$TOOLCHAIN_CONTEXT"

    step "E2E images (the Release build of examples/status_server, plus the tester)"
    compose build

    step "bringing the stack up"
    compose up --detach

    step "waiting for the stack to report healthy"
    wait_for_stack
}

run_checklist() {
    step "the status page renders the operator rows"
    tester status-page

    step "HTTP/1.1 keep-alive and the conditional GET an auto-refreshing page performs"
    tester keep-alive
    tester conditional-get

    step "the authority rule, against requests no correct client will build"
    tester host-required

    step "TLS 1.3 on 7778, and a client offering only h2"
    tester tls-handshake
    tester alpn-h2-refused

    step "the plaintext and TLS listeners serve the same content"
    tester listeners-agree

    step "the CIDR allowlist discriminates between peers"
    tester cidr-allows-listed-peer
    tester cidr-refuses-outside-peer

    step "a real Prometheus scrapes /metrics"
    tester prometheus-scraping
    tester prometheus-scrape-advances

    step "the health probe follows the service out of rotation and back"
    tester health-ok
    tester drain-on
    tester health-degraded
    tester drain-off
    tester health-ok

    step "RECOVERY: restarting the service container"
    local uptime_before_restart
    uptime_before_restart=$(tester uptime-seconds | tr -d '\r')
    printf 'the running process reports %ss of uptime\n' "$uptime_before_restart"

    compose restart status-server
    wait_for_health status-server
    tester wait-healthy
    tester expect-uptime-below "$uptime_before_restart"

    step "RECOVERY: the stack came back without intervention"
    tester status-page
    tester tls-handshake
    tester prometheus-scraping
    tester prometheus-scrape-advances
}

print_usage() {
    printf '%s\n' \
        'Usage: scripts/e2e.sh [check [arguments...]]' \
        '' \
        'Without a check, runs the full stack checklist.' \
        'With a check, starts the stack and runs only that tester command.'
}

main() {
    if [ "${1:-}" = "--help" ] || [ "${1:-}" = "-h" ]; then
        print_usage
        docker run --rm erikslund-http-e2e-tester:latest --help || true
        return 0
    fi

    trap finish EXIT
    bring_the_stack_up

    if [ "$#" -gt 0 ]; then
        step "running the named check only"
        tester "$@"
    else
        run_checklist
    fi

    printf '\n==> E2E PASSED\n'
}

main "$@"
