#include "erikslund/http/server.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <csignal>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <expected>
#include <format>
#include <latch>
#include <memory>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "erikslund/http/cidr.hpp"
#include "erikslund/http/contracts.hpp"
#include "internal/connection.hpp"
#include "internal/reactor.hpp"
#include "internal/server_metrics.hpp"
#include "internal/server_state.hpp"
#include "internal/socket.hpp"
#include "internal/tls_context.hpp"

namespace erikslund::http {
namespace {

constexpr unsigned char kFirstPrintableAscii = 0x20;
constexpr unsigned char kAsciiDelete = 0x7F;

[[nodiscard]] constexpr std::string_view level_name(LogLevel level) noexcept {
    switch (level) {
    case LogLevel::Debug:
        return "debug";
    case LogLevel::Info:
        return "info";
    case LogLevel::Warning:
        return "warn";
    case LogLevel::Error:
        return "error";
    }
    return "info";
}

void ignore_sigpipe_once() noexcept {
    static const bool applied = [] {
        struct sigaction current{};
        if (::sigaction(SIGPIPE, nullptr, &current) != 0)
            return false;
        if (current.sa_handler != SIG_DFL)
            return true;
        struct sigaction ignored{};
        ignored.sa_handler = SIG_IGN;
        ::sigemptyset(&ignored.sa_mask);
        return ::sigaction(SIGPIPE, &ignored, nullptr) == 0;
    }();
    static_cast<void>(applied);
}

[[nodiscard]] unsigned resolve_worker_count(unsigned configured) noexcept {
    if (configured > 0)
        return configured;
    const unsigned detected = std::thread::hardware_concurrency();
    return std::clamp(detected == 0 ? kMinAutoWorkerThreads : detected, kMinAutoWorkerThreads,
                      kMaxAutoWorkerThreads);
}

[[nodiscard]] bool has_control_character(std::string_view text) noexcept {
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < kFirstPrintableAscii || byte == kAsciiDelete)
            return true;
    }
    return false;
}

void validate_server_options(const ServerOptions& options) {
    if (options.worker_threads > kMaxWorkerThreads)
        throw ServerError(std::format("worker_threads exceeds the {} worker ceiling",
                                      kMaxWorkerThreads));
    if (options.max_connections == 0)
        throw ServerError("max_connections must be at least 1");
    if (options.max_connections_per_source == 0)
        throw ServerError("max_connections_per_source must be at least 1");
    if (options.max_requests_per_connection == 0)
        throw ServerError("max_requests_per_connection must be at least 1");
    if (options.listen_backlog <= 0)
        throw ServerError("listen_backlog must be at least 1");

    const std::array timeouts{
        std::pair{"handshake_timeout", options.handshake_timeout},
        std::pair{"header_timeout", options.header_timeout},
        std::pair{"body_timeout", options.body_timeout},
        std::pair{"write_timeout", options.write_timeout},
        std::pair{"keep_alive_idle", options.keep_alive_idle},
        std::pair{"request_deadline", options.request_deadline},
        std::pair{"stream_idle_timeout", options.stream_idle_timeout},
    };
    for (const auto& [name, timeout] : timeouts)
        if (timeout <= std::chrono::milliseconds::zero())
            throw ServerError(std::format("{} must be at least 1 ms", name));

    if (options.limits.max_header_count == 0 ||
        options.limits.max_header_count > kMaxParsedHeaders)
        throw ServerError(std::format("max_header_count must be between 1 and {}",
                                      kMaxParsedHeaders));
    if (options.limits.max_request_line_bytes == 0 ||
        options.limits.max_header_block_bytes == 0 || options.limits.max_target_bytes == 0)
        throw ServerError("request line, header block, and target limits must be nonzero");
    if (options.limits.max_target_bytes > options.limits.max_request_line_bytes)
        throw ServerError("max_target_bytes cannot exceed max_request_line_bytes");
    if (has_control_character(options.server_header))
        throw ServerError("server_header cannot contain control characters");

    std::vector<std::string> endpoints;
    endpoints.reserve(options.listeners.size());
    for (size_t index = 0; index < options.listeners.size(); ++index) {
        const Listener& listener = options.listeners[index];
        if (!listener.unix_socket_path.empty() && listener.port == 0)
            throw ServerError(std::format("listeners[{}] combines a Unix socket with port 0",
                                          index));
        if (listener.unix_socket_path.empty() && listener.bind_address.empty())
            throw ServerError(std::format("listeners[{}] has no bind address", index));
        if (listener.unix_socket_path.empty() && listener.port == 0)
            continue;

        std::string endpoint = listener.unix_socket_path.empty()
                                   ? std::format("{}:{}", listener.bind_address, listener.port)
                                   : std::format("unix:{}", listener.unix_socket_path);
        if (std::ranges::contains(endpoints, endpoint))
            throw ServerError(std::format("listeners[{}] repeats endpoint {}", index, endpoint));
        endpoints.push_back(std::move(endpoint));
    }
}

[[nodiscard]] std::string listener_url(const internal::ListenerState& listener) {
    if (listener.is_unix)
        return std::format("unix:{}", listener.config.unix_socket_path);

    const std::string_view scheme = listener.config.tls.enabled ? "https" : "http";
    std::string host = listener.config.bind_address.empty() ? std::string(kDefaultBindAddress)
                                                            : listener.config.bind_address;
    if (host.find(':') != std::string::npos)
        host = std::format("[{}]", host);
    return std::format("{}://{}:{}/", scheme, host, listener.resolved_port);
}

} // namespace

LogSink stderr_log_sink() {
    return [](LogLevel level, std::string_view message) noexcept {
        const std::string_view name = level_name(level);
        ::flockfile(stderr);
        std::fputc('[', stderr);
        std::fwrite(name.data(), 1, name.size(), stderr);
        std::fputs("] ", stderr);
        std::fwrite(message.data(), 1, message.size(), stderr);
        std::fputc('\n', stderr);
        ::funlockfile(stderr);
    };
}

ServerOptions ServerOptions::on_port(uint16_t port) {
    ServerOptions options;
    Listener listener;
    listener.port = port;
    options.listeners.push_back(std::move(listener));
    return options;
}

ServerOptions ServerOptions::on_port_tls(uint16_t port, TlsOptions tls) {
    ServerOptions options;
    Listener listener;
    listener.port = port;
    listener.tls = std::move(tls);
    listener.tls.enabled = true;
    options.listeners.push_back(std::move(listener));
    return options;
}

Server::Server(Router router, ServerOptions options)
    : router_(std::move(router)), options_(std::move(options)) {}

Server::~Server() {
    stop();
}

void Server::install_metrics(MetricsRegistry& registry) {
    if (started_)
        throw ServerError("install_metrics() must be called before start()");

    server_metrics_ = std::make_unique<internal::ServerMetrics>(registry);

    const auto started_at = std::chrono::steady_clock::now();
    registry.gauge_fn("uptime_seconds", "Seconds since the server registered its metrics",
                      [started_at] {
                          const auto elapsed = std::chrono::steady_clock::now() - started_at;
                          return std::chrono::duration<double>(elapsed).count();
                      });

    registry.library_build_info(std::string(kLibraryName), std::string(kVersion));
}

void Server::start() {
    if (started_)
        throw ServerError("Server::start() called twice");

    ignore_sigpipe_once();

    if (!options_.log)
        options_.log = stderr_log_sink();
    if (options_.listeners.empty())
        options_.listeners.push_back(Listener{});
    validate_server_options(options_);
    state_ = std::make_unique<internal::ServerState>(
        options_.max_connections, options_.max_connections_per_source, options_.log);

    const unsigned worker_count = resolve_worker_count(options_.worker_threads);

    std::expected<CidrAllowList, CidrError> parsed_allow_list =
        CidrAllowList::parse(options_.allow_cidrs);
    if (!parsed_allow_list.has_value())
        throw ServerError("allow_cidrs contains an entry that is not a valid address or prefix");

    const auto allow_list = std::make_shared<CidrAllowList>(std::move(*parsed_allow_list));

    try {
        for (const Listener& configured : options_.listeners) {
            auto state = std::make_unique<internal::ListenerState>();
            state->config = configured;
            state->is_unix = !configured.unix_socket_path.empty();

            if (configured.tls.enabled)
                state->tls.store(
                    internal::TlsContext::create(configured.tls, state_->logger()));

            const bool per_reactor = options_.reuse_port && !state->is_unix &&
                                     internal::reuse_port_supported();
            state->per_reactor_sockets = per_reactor;

            const size_t socket_count = per_reactor ? worker_count : 1U;
            uint16_t effective_port = configured.port;
            for (size_t index = 0; index < socket_count; ++index) {
                internal::UniqueFd bound;
                if (state->is_unix) {
                    internal::BoundUnixListener unix_bound = internal::bind_unix_listener(
                        configured.unix_socket_path, options_.listen_backlog);
                    state->unix_socket_identity = unix_bound.identity;
                    bound = std::move(unix_bound.fd);
                } else {
                    bound = internal::bind_tcp_listener(configured.bind_address, effective_port,
                                                        options_.listen_backlog, per_reactor);
                }
                if (index == 0 && !state->is_unix) {
                    effective_port = internal::resolved_port_of(bound.get());
                    state->resolved_port = effective_port;
                }
                state->sockets.push_back(std::move(bound));
            }

            listeners_.push_back(std::move(state));
        }

        for (unsigned index = 0; index < worker_count; ++index)
            reactors_.push_back(std::make_unique<internal::Reactor>(
                index, router_, options_, *allow_list, server_metrics_.get(), *state_));

        for (const auto& listener : listeners_) {
            for (unsigned index = 0; index < worker_count; ++index) {
                const bool exclusive = !listener->per_reactor_sockets;
                const int listen_fd = listener->per_reactor_sockets
                                          ? listener->sockets[index].get()
                                          : listener->sockets.front().get();
                reactors_[index]->add_listener(*listener, listen_fd, exclusive);
            }
        }
    } catch (...) {
        reactors_.clear();
        listeners_.clear();
        throw;
    }

    std::vector<std::pair<LogLevel, std::string>> startup_messages;
    startup_messages.reserve(listeners_.size() * 2);
    for (const auto& listener : listeners_) {
        const std::string url = listener_url(*listener);
        const std::string_view accept_model = listener->per_reactor_sockets
                                                  ? "SO_REUSEPORT socket per reactor"
                                                  : "one shared socket, EPOLLEXCLUSIVE";
        startup_messages.emplace_back(
            LogLevel::Info,
            std::format("listening on {} ({} workers, {})", url, worker_count, accept_model));

        if (!options_.warn_on_public_bind || listener->is_unix)
            continue;
        if (listener->config.tls.enabled || !options_.allow_cidrs.empty())
            continue;
        if (internal::bind_address_is_local(listener->config.bind_address))
            continue;

        startup_messages.emplace_back(
            LogLevel::Warning,
            std::format("{} is reachable from the network with neither TLS nor an allow list; "
                        "the status page and /metrics are unauthenticated",
                        url));
    }

    const auto ready = std::make_shared<std::latch>(static_cast<std::ptrdiff_t>(worker_count));
    const std::stop_token server_token = state_->stop_token();
    started_ = true;

    unsigned spawned = 0;
    try {
        for (unsigned index = 0; index < worker_count; ++index) {
            workers_.emplace_back([this, index, ready, allow_list,
                                   server_token](const std::stop_token& thread_token) {
                const std::stop_callback forward(thread_token, [this] { this->stop(); });
                reactors_[index]->run(server_token, *ready);
                static_cast<void>(allow_list);
            });
            ++spawned;
        }
    } catch (...) {
        for (unsigned index = spawned; index < worker_count; ++index)
            ready->count_down();
        state_->request_stop();
        throw;
    }

    ready->wait();
    if (state_->failure() != nullptr)
        wait();

    for (const auto& [level, message] : startup_messages)
        state_->logger().write(level, message);
}

void Server::wait() {
    for (std::jthread& worker : workers_)
        if (worker.joinable())
            worker.join();

    for (const auto& listener : listeners_)
        listener->release_unix_path();
    reactors_.clear();
    listeners_.clear();

    if (state_ == nullptr)
        return;
    if (const std::exception_ptr failure = state_->failure(); failure != nullptr)
        std::rethrow_exception(failure);
}

void Server::run(const std::stop_token& stop_token) {
    if (!started_)
        start();
    const std::stop_callback stop_bridge(stop_token, [this] { this->stop(); });
    wait();
}

void Server::stop() noexcept {
    if (state_ != nullptr)
        state_->request_stop();
    for (const auto& reactor : reactors_)
        if (reactor)
            reactor->wake();
}

uint16_t Server::port() const noexcept {
    return listeners_.empty() ? 0 : listeners_.front()->resolved_port;
}

uint16_t Server::port(size_t listener_index) const {
    if (listener_index >= listeners_.size())
        throw ServerError(std::format("listener index {} is out of range; {} configured",
                                      listener_index, listeners_.size()));
    return listeners_[listener_index]->resolved_port;
}

void Server::reload_tls() {
    struct ReloadedContext {
        internal::ListenerState* listener = nullptr;
        std::shared_ptr<internal::TlsContext> context;
        std::string url;
    };
    std::vector<ReloadedContext> loaded;
    for (const auto& listener : listeners_) {
        if (!listener->config.tls.enabled)
            continue;
        loaded.push_back(ReloadedContext{
            listener.get(), internal::TlsContext::create(listener->config.tls, state_->logger()),
            listener_url(*listener)});
    }

    for (auto& [listener, context, url] : loaded) {
        listener->tls.store(std::move(context));
        state_->logger().writef(LogLevel::Info, "reloaded the certificate for {}", url);
    }
}

} // namespace erikslund::http
