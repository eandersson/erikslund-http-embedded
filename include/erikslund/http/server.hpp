#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <stdexcept>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "erikslund/http/metrics.hpp"
#include "erikslund/http/request.hpp"
#include "erikslund/http/router.hpp"
#include "erikslund/http/tls.hpp"

namespace erikslund::http {

namespace internal {
class Reactor;
struct ListenerState;
class ServerMetrics;
class ServerState;
} // namespace internal

inline constexpr const char* kDefaultBindAddress = "::1";
inline constexpr const char* kDefaultServerHeader = "erikslund-http";
inline constexpr uint16_t kDefaultPort = 7777;

inline constexpr std::string_view kLibraryName = "erikslund-http";

inline constexpr unsigned kDefaultMaxConnections = 512;
inline constexpr unsigned kDefaultMaxConnectionsPerSource = 64;
inline constexpr unsigned kDefaultMaxRequestsPerConnection = 100;
inline constexpr int kDefaultListenBacklog = 128;

inline constexpr unsigned kMinAutoWorkerThreads = 1;
inline constexpr unsigned kMaxAutoWorkerThreads = 4;
inline constexpr unsigned kMaxWorkerThreads = 256;

inline constexpr std::chrono::milliseconds kDefaultHandshakeTimeout{5'000};
inline constexpr std::chrono::milliseconds kDefaultHeaderTimeout{5'000};
inline constexpr std::chrono::milliseconds kDefaultBodyTimeout{5'000};
inline constexpr std::chrono::milliseconds kDefaultWriteTimeout{5'000};
inline constexpr std::chrono::milliseconds kDefaultKeepAliveIdle{15'000};
inline constexpr std::chrono::milliseconds kDefaultStreamIdleTimeout{60'000};

// End-to-end request budget, independent of per-phase timeouts.
inline constexpr std::chrono::milliseconds kDefaultRequestDeadline{3'000};

enum class LogLevel : uint8_t { Debug, Info, Warning, Error };

using LogSink = std::function<void(LogLevel, std::string_view)>;

[[nodiscard]] LogSink stderr_log_sink();

struct Listener {
    // Explicit "::" binds are dual-stack.
    std::string bind_address = kDefaultBindAddress;

    // Zero selects an ephemeral port.
    uint16_t port = kDefaultPort;

    // Non-empty selects AF_UNIX and ignores bind_address and port.
    std::string unix_socket_path{};

    TlsOptions tls{};
};

struct ServerOptions {
    // Empty creates one plaintext listener with default settings.
    std::vector<Listener> listeners{};

    // Zero uses hardware_concurrency clamped to the automatic range.
    unsigned worker_threads = 0;

    RequestLimits limits{};

    // Process-wide across every reactor.
    unsigned max_connections = kDefaultMaxConnections;

    // Per TCP source address; Unix-domain peers use only the process-wide limit.
    unsigned max_connections_per_source = kDefaultMaxConnectionsPerSource;

    unsigned max_requests_per_connection = kDefaultMaxRequestsPerConnection;

    std::chrono::milliseconds handshake_timeout = kDefaultHandshakeTimeout;
    std::chrono::milliseconds header_timeout = kDefaultHeaderTimeout;
    std::chrono::milliseconds body_timeout = kDefaultBodyTimeout;
    std::chrono::milliseconds write_timeout = kDefaultWriteTimeout;
    std::chrono::milliseconds keep_alive_idle = kDefaultKeepAliveIdle;
    std::chrono::milliseconds request_deadline = kDefaultRequestDeadline;

    // Keep above the longest expected gap between stream writes.
    std::chrono::milliseconds stream_idle_timeout = kDefaultStreamIdleTimeout;

    int listen_backlog = kDefaultListenBacklog;

    // Falls back to one EPOLLEXCLUSIVE shared socket when disabled or unavailable.
    bool reuse_port = true;

    bool tcp_nodelay = true;

    // Warns when a routable listener has neither TLS nor a CIDR allowlist.
    bool warn_on_public_bind = true;

    // Empty allows every peer; rejected peers receive no response.
    std::vector<std::string> allow_cidrs{};

    // Empty omits the Server header.
    std::string server_header = kDefaultServerHeader;

    // Empty uses stderr_log_sink().
    LogSink log{};

    [[nodiscard]] static ServerOptions on_port(uint16_t port);
    [[nodiscard]] static ServerOptions on_port_tls(uint16_t port, TlsOptions tls);
};

class ServerError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class Server {
public:
    Server(Router router, ServerOptions options);
    ~Server();
    Server(const Server&) = delete("Server owns listening sockets and reactor threads");
    Server& operator=(const Server&) = delete("Server owns listening sockets and reactor threads");
    Server(Server&&) = delete("reactor threads hold a pointer to this Server");
    Server& operator=(Server&&) = delete("reactor threads hold a pointer to this Server");

    // Returns after every listener is accepting. Startup failures throw ServerError.
    void start();

    // Joins every worker and rethrows the first reactor failure.
    void wait();

    // Starts and waits until the token is signalled.
    void run(const std::stop_token& stop);

    // Idempotent and thread-safe. In-flight requests finish.
    void stop() noexcept;

    // Resolved port of the first listener.
    [[nodiscard]] uint16_t port() const noexcept;

    [[nodiscard]] uint16_t port(size_t listener_index) const;

    // Registers server and library metrics before start(). The registry must outlive the Server.
    void install_metrics(MetricsRegistry& registry);

    // Reloads TLS material atomically. Existing connections retain the old context; failures leave
    // it installed and throw ServerError.
    void reload_tls();

    [[nodiscard]] const ServerOptions& options() const noexcept { return options_; }

private:
    Router router_;
    ServerOptions options_;

    std::unique_ptr<internal::ServerMetrics> server_metrics_;

    // Shared admission, failure, stop, and logging state. Outlives listeners and reactors.
    std::unique_ptr<internal::ServerState> state_;

    std::vector<std::unique_ptr<internal::ListenerState>> listeners_;

    // reactor[index] is owned by workers[index].
    std::vector<std::unique_ptr<internal::Reactor>> reactors_;

    bool started_ = false;

    // Must remain last so threads join before the state they reference is destroyed.
    std::vector<std::jthread> workers_;
};

} // namespace erikslund::http
