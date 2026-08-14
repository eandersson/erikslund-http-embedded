
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <pthread.h>
#include <unistd.h>

#include "erikslund/http/metrics.hpp"
#include "erikslund/http/observability.hpp"
#include "erikslund/http/request.hpp"
#include "erikslund/http/response.hpp"
#include "erikslund/http/router.hpp"
#include "erikslund/http/server.hpp"
#include "erikslund/http/status_page.hpp"
#include "erikslund/http/text.hpp"
#include "erikslund/http/tls.hpp"

namespace {

using erikslund::http::format_duration;
using erikslund::http::Listener;
using erikslund::http::MetricsRegistry;
using erikslund::http::mount_observability;
using erikslund::http::ObservabilityOptions;
using erikslund::http::Request;
using erikslund::http::Response;
using erikslund::http::Router;
using erikslund::http::Server;
using erikslund::http::ServerError;
using erikslund::http::ServerOptions;
using erikslund::http::State;
using erikslund::http::StatusPage;
using erikslund::http::tls_available;

constexpr std::string_view kServiceName = "status-server";
constexpr std::string_view kServiceVersion = "0.1.2";

constexpr uint16_t kPlaintextPort = 7'777;
constexpr uint16_t kTlsPort = 7'778;

constexpr std::string_view kLoopbackBindAddress = "127.0.0.1";

constexpr std::string_view kBindAddressVariable = "STATUS_SERVER_BIND";
constexpr std::string_view kAllowCidrsVariable = "STATUS_SERVER_ALLOW_CIDRS";
constexpr std::string_view kDrainFileVariable = "STATUS_SERVER_DRAIN_FILE";

constexpr std::string_view kGreetingPath = "/hello";
constexpr std::string_view kGreetingBody = "hello from erikslund-http\n";

constexpr std::string_view kReadyHeadline = "READY";
constexpr std::string_view kDrainingHeadline = "DRAINING";

constexpr size_t kTlsArgumentCount = 3;

[[nodiscard]] std::string environment_or(std::string_view name, std::string_view fallback) {
    const std::string key(name);
    const char* const value = std::getenv(key.c_str());
    if (value == nullptr || *value == '\0')
        return std::string(fallback);
    return std::string(value);
}

[[nodiscard]] std::vector<std::string> split_on_commas(std::string_view list) {
    std::vector<std::string> entries;
    while (!list.empty()) {
        const size_t separator = list.find(',');
        const std::string_view entry = list.substr(0, separator);
        const size_t first = entry.find_first_not_of(" \t");
        if (first != std::string_view::npos)
            entries.emplace_back(entry.substr(first, entry.find_last_not_of(" \t") - first + 1));
        if (separator == std::string_view::npos)
            break;
        list.remove_prefix(separator + 1);
    }
    return entries;
}

[[nodiscard]] std::string join_with_commas(const std::vector<std::string>& entries) {
    std::string joined;
    for (const std::string& entry : entries) {
        if (!joined.empty())
            joined += ", ";
        joined += entry;
    }
    return joined;
}

struct Deployment {
    std::chrono::steady_clock::time_point started_at = std::chrono::steady_clock::now();
    std::string bind_address = std::string(kLoopbackBindAddress);
    std::vector<std::string> allow_cidrs{};
    std::string drain_file{};
    bool tls_enabled = false;
};

[[nodiscard]] bool is_in_rotation(const Deployment& deployment) {
    if (deployment.drain_file.empty())
        return true;
    std::error_code lookup_failed;
    return !std::filesystem::exists(deployment.drain_file, lookup_failed);
}

[[nodiscard]] StatusPage render_status(const Deployment& deployment) {
    const auto uptime = std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::steady_clock::now() - deployment.started_at);
    const bool in_rotation = is_in_rotation(deployment);

    StatusPage page{std::string(kServiceName), std::string(kServiceVersion)};
    page.pid(static_cast<int>(::getpid()));
    page.state(std::string(in_rotation ? kReadyHeadline : kDrainingHeadline),
               in_rotation ? State::Ok : State::Bad);
    page.row("uptime", format_duration(uptime));
    page.section("listeners");
    page.row("plaintext", std::format("http://{}:{}/", deployment.bind_address, kPlaintextPort));
    if (deployment.tls_enabled)
        page.row("tls", std::format("https://{}:{}/", deployment.bind_address, kTlsPort),
                 State::Ok);
    else
        page.row("tls", "not configured", State::Warn);
    if (!deployment.allow_cidrs.empty())
        page.row("allow_cidrs", join_with_commas(deployment.allow_cidrs), State::Ok);
    page.link("/metrics", "/metrics");
    page.link("/healthz", "/healthz");
    return page;
}

[[nodiscard]] sigset_t block_shutdown_signals() {
    sigset_t blocked{};
    ::sigemptyset(&blocked);
    ::sigaddset(&blocked, SIGINT);
    ::sigaddset(&blocked, SIGTERM);
    ::pthread_sigmask(SIG_BLOCK, &blocked, nullptr);
    return blocked;
}

void wait_for_shutdown_signal(const sigset_t& blocked) {
    int received = 0;
    while (::sigwait(&blocked, &received) == EINTR) {
    }
}

} // namespace

int main(int argc, char** argv) {
    const std::span<char*> arguments(argv, static_cast<size_t>(argc));
    const sigset_t blocked = block_shutdown_signals();

    Deployment deployment;
    deployment.bind_address = environment_or(kBindAddressVariable, kLoopbackBindAddress);
    deployment.allow_cidrs = split_on_commas(environment_or(kAllowCidrsVariable, ""));
    deployment.drain_file = environment_or(kDrainFileVariable, "");

    MetricsRegistry registry{std::string("status_server")};

    registry.build_info(std::string(kServiceVersion));

    Router router;
    router.get(kGreetingPath, [](const Request&) -> Response {
        return Response::text(std::string(kGreetingBody)).etag_from_body();
    });

    ObservabilityOptions observability;
    observability.service_name = std::string(kServiceName);
    observability.version = std::string(kServiceVersion);
    observability.metrics = &registry;
    observability.healthy = [&deployment] { return is_in_rotation(deployment); };
    observability.status_page = [&deployment] { return render_status(deployment); };
    mount_observability(router, std::move(observability));

    ServerOptions options;
    options.allow_cidrs = deployment.allow_cidrs;
    Listener plaintext;
    plaintext.bind_address = deployment.bind_address;
    plaintext.port = kPlaintextPort;
    options.listeners.push_back(std::move(plaintext));

    if (arguments.size() >= kTlsArgumentCount && tls_available()) {
        Listener secure;
        secure.bind_address = deployment.bind_address;
        secure.port = kTlsPort;
        secure.tls.enabled = true;
        secure.tls.certificate_chain_file = arguments[1];
        secure.tls.private_key_file = arguments[2];
        options.listeners.push_back(std::move(secure));
        deployment.tls_enabled = true;
    } else if (arguments.size() >= kTlsArgumentCount) {
        std::println("this build was compiled without TLS; serving plaintext only");
    }

    Server server(std::move(router), std::move(options));
    server.install_metrics(registry);

    try {
        server.start();
    } catch (const ServerError& error) {
        std::println(stderr, "{} could not start: {}", kServiceName, error.what());
        return EXIT_FAILURE;
    }

    std::println("status page   http://{}:{}/", deployment.bind_address, server.port(0));
    if (deployment.tls_enabled)
        std::println("status page   https://{}:{}/", deployment.bind_address, server.port(1));
    std::println("metrics       http://{}:{}/metrics", deployment.bind_address, server.port(0));
    std::println("health        http://{}:{}/healthz", deployment.bind_address, server.port(0));
    if (!deployment.allow_cidrs.empty())
        std::println("allow cidrs   {}", join_with_commas(deployment.allow_cidrs));
    if (!deployment.drain_file.empty())
        std::println("drain switch  {}", deployment.drain_file);
    std::println("press Ctrl-C to stop");

    wait_for_shutdown_signal(blocked);

    std::println("stopping");
    server.stop();
    server.wait();
    return EXIT_SUCCESS;
}
