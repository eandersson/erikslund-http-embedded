#pragma once
// Glaze stays in config.cpp to avoid imposing its compile cost on consumers.

#include <cstdint>
#include <expected>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

#include "erikslund/http/build_config.hpp"
#include "erikslund/http/request.hpp"
#include "erikslund/http/server.hpp"

namespace erikslund::http {

struct TlsConfig {
    bool enabled = false;
    std::string certificate_chain_file{};
    std::string private_key_file{};

    // "1.3" or "1.2"; a string keeps serialized configuration readable.
    std::string minimum_version = "1.3";

    std::string client_ca_file{};
    bool require_client_certificate = false;
    std::string alpn_protocols = "http/1.1";

    // Empty uses the library default.
    std::string group_list{};

    bool session_tickets = true;
    bool early_data = false;
    bool kernel_tls = true;
    bool strict_transport_security = false;
};

struct ListenerConfig {
    std::string bind_address = kDefaultBindAddress;
    uint16_t port = kDefaultPort;
    std::string unix_socket_path{};
    TlsConfig tls{};
};

struct LimitsConfig {
    size_t max_request_line_bytes = kDefaultMaxRequestLineBytes;
    size_t max_header_block_bytes = kDefaultMaxHeaderBlockBytes;
    size_t max_header_count = kMaxParsedHeaders;
    size_t max_target_bytes = kDefaultMaxTargetBytes;
    size_t max_body_bytes = kDefaultMaxBodyBytes;
};

struct ServerConfig {
    std::vector<ListenerConfig> listeners{};
    unsigned worker_threads = 0;
    LimitsConfig limits{};
    unsigned max_connections = kDefaultMaxConnections;
    unsigned max_connections_per_source = kDefaultMaxConnectionsPerSource;
    unsigned max_requests_per_connection = kDefaultMaxRequestsPerConnection;
    uint32_t handshake_timeout_ms = 5'000;
    uint32_t header_timeout_ms = 5'000;
    uint32_t body_timeout_ms = 5'000;
    uint32_t write_timeout_ms = 5'000;
    uint32_t keep_alive_idle_ms = 15'000;
    uint32_t request_deadline_ms = 3'000;
    uint32_t stream_idle_timeout_ms = 60'000;
    int listen_backlog = kDefaultListenBacklog;
    bool reuse_port = true;
    bool tcp_nodelay = true;
    bool warn_on_public_bind = true;
    std::vector<std::string> allow_cidrs{};
    std::string server_header = kDefaultServerHeader;
};

enum class ConfigError : uint8_t {
    NotFound,
    Unreadable,
    UnknownFormat,
    MalformedYaml,
    MalformedJson,
    InvalidValue,
};

[[nodiscard]] std::string_view config_error_message(ConfigError error) noexcept;

// Chooses YAML or JSON from the file extension and validates all values.
[[nodiscard]] std::expected<ServerConfig, ConfigError> load_config(
    const std::filesystem::path& path);

[[nodiscard]] std::expected<std::string, ConfigError> dump_config(const ServerConfig& config,
                                                                  std::string_view format);

[[nodiscard]] ServerOptions to_options(const ServerConfig& config);

} // namespace erikslund::http
