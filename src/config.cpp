#include "erikslund/http/config.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdio>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <glaze/glaze.hpp>
#include <glaze/yaml.hpp>

#include "erikslund/http/cidr.hpp"
#include "erikslund/http/contracts.hpp"
#include "erikslund/http/request.hpp"
#include "erikslund/http/server.hpp"
#include "erikslund/http/text.hpp"
#include "erikslund/http/tls.hpp"

namespace erikslund::http {

namespace {

constexpr std::string_view kTls13Name = "1.3";
constexpr std::string_view kTls12Name = "1.2";

constexpr std::string_view kSupportedAlpnProtocol = "http/1.1";

constexpr std::string_view kOptionalWhitespace = " \t";

constexpr unsigned char kFirstPrintableAscii = 0x20;
constexpr unsigned char kAsciiDelete = 0x7F;

[[nodiscard]] constexpr std::string_view trim_optional_whitespace(std::string_view text) noexcept {
    const size_t first = text.find_first_not_of(kOptionalWhitespace);
    if (first == std::string_view::npos)
        return {};
    return text.substr(first, text.find_last_not_of(kOptionalWhitespace) - first + 1);
}

[[nodiscard]] std::vector<std::string_view> comma_separated(std::string_view list) {
    std::vector<std::string_view> tokens;
    size_t start = 0;
    while (true) {
        const size_t comma = list.find(',', start);
        const size_t end = comma == std::string_view::npos ? list.size() : comma;
        const std::string_view token = trim_optional_whitespace(list.substr(start, end - start));
        if (!token.empty())
            tokens.push_back(token);
        if (comma == std::string_view::npos)
            break;
        start = comma + 1;
    }
    return tokens;
}

[[nodiscard]] bool has_control_character(std::string_view text) noexcept {
    for (const char character : text) {
        const auto byte = static_cast<unsigned char>(character);
        if (byte < kFirstPrintableAscii || byte == kAsciiDelete)
            return true;
    }
    return false;
}

[[nodiscard]] std::string_view cidr_error_text(CidrError error) noexcept {
    switch (error) {
    case CidrError::MalformedEntry:
        return "expected ADDRESS or ADDRESS/PREFIX with exactly one '/'";
    case CidrError::BadPrefixLength:
        return "the prefix length must be 0..32 for IPv4 or 0..128 for IPv6";
    case CidrError::BadAddress:
        return "the address part parses as neither IPv4 nor IPv6";
    }
    return "the entry could not be parsed";
}

[[nodiscard]] std::optional<std::string> problem_with_tls(const TlsConfig& tls,
                                                          size_t listener_index) {
    if (tls.minimum_version != kTls13Name && tls.minimum_version != kTls12Name)
        return std::format(
            "listeners[{}].tls.minimum_version is \"{}\"; the accepted values are \"1.3\" and "
            "\"1.2\"",
            listener_index, tls.minimum_version);

    for (const std::string_view protocol : comma_separated(tls.alpn_protocols)) {
        if (protocol != kSupportedAlpnProtocol)
            return std::format(
                "listeners[{}].tls.alpn_protocols offers \"{}\", which this server cannot speak; "
                "a client that accepts the offer then waits on a protocol that never answers",
                listener_index, protocol);
    }

    if (!tls.enabled)
        return std::nullopt;

    if (tls.certificate_chain_file.empty())
        return std::format(
            "listeners[{}].tls.enabled is true but certificate_chain_file is empty", listener_index);
    if (tls.private_key_file.empty())
        return std::format("listeners[{}].tls.enabled is true but private_key_file is empty",
                           listener_index);
    if (tls.require_client_certificate && tls.client_ca_file.empty())
        return std::format(
            "listeners[{}].tls.require_client_certificate needs a client_ca_file to verify against",
            listener_index);
    return std::nullopt;
}

struct NamedTimeout {
    std::string_view key;
    uint32_t milliseconds;
};

struct NamedByteLimit {
    std::string_view key;
    size_t bytes;
};

[[nodiscard]] std::optional<std::string> first_problem_with(const ServerConfig& config) {
    if (config.worker_threads > kMaxWorkerThreads)
        return std::format("worker_threads is {}; the ceiling is {} (0 means one reactor per core, "
                           "clamped to {})",
                           config.worker_threads, kMaxWorkerThreads,
                           kMaxAutoWorkerThreads);
    if (config.max_connections == 0)
        return "max_connections is 0, which accepts a connection only to close it again; there is "
               "no unlimited sentinel";
    if (config.max_connections_per_source == 0)
        return "max_connections_per_source is 0, which refuses every TCP source; there is no "
               "unlimited sentinel";
    if (config.max_requests_per_connection == 0)
        return "max_requests_per_connection is 0, which closes a connection before answering it; "
               "there is no unlimited sentinel";
    if (config.listen_backlog <= 0)
        return std::format("listen_backlog is {}; it must be at least 1", config.listen_backlog);

    const std::array timeouts{
        NamedTimeout{"handshake_timeout_ms", config.handshake_timeout_ms},
        NamedTimeout{"header_timeout_ms", config.header_timeout_ms},
        NamedTimeout{"body_timeout_ms", config.body_timeout_ms},
        NamedTimeout{"write_timeout_ms", config.write_timeout_ms},
        NamedTimeout{"keep_alive_idle_ms", config.keep_alive_idle_ms},
        NamedTimeout{"request_deadline_ms", config.request_deadline_ms},
        NamedTimeout{"stream_idle_timeout_ms", config.stream_idle_timeout_ms},
    };
    for (const NamedTimeout& timeout : timeouts) {
        if (timeout.milliseconds == 0)
            return std::format("{} is 0; every timeout must be at least 1 ms, because a listener "
                               "with no timeout is a slowloris target",
                               timeout.key);
    }

    if (config.limits.max_header_count == 0 || config.limits.max_header_count > kMaxParsedHeaders)
        return std::format("limits.max_header_count is {}; it must be between 1 and {}, the "
                           "compile-time capacity of the parser's header table",
                           config.limits.max_header_count, kMaxParsedHeaders);

    const std::array byte_limits{
        NamedByteLimit{"limits.max_request_line_bytes", config.limits.max_request_line_bytes},
        NamedByteLimit{"limits.max_header_block_bytes", config.limits.max_header_block_bytes},
        NamedByteLimit{"limits.max_target_bytes", config.limits.max_target_bytes},
    };
    for (const NamedByteLimit& limit : byte_limits) {
        if (limit.bytes == 0)
            return std::format("{} is 0, which rejects every request", limit.key);
    }

    if (config.limits.max_target_bytes > config.limits.max_request_line_bytes)
        return std::format("limits.max_target_bytes ({}) exceeds limits.max_request_line_bytes "
                           "({}), and the target is part of the request line",
                           config.limits.max_target_bytes, config.limits.max_request_line_bytes);

    std::vector<std::string> endpoints;
    endpoints.reserve(config.listeners.size());
    for (size_t index = 0; index < config.listeners.size(); ++index) {
        const ListenerConfig& listener = config.listeners[index];

        if (!listener.unix_socket_path.empty() && listener.port == 0)
            return std::format(
                "listeners[{}] sets both unix_socket_path and port 0; port 0 means \"bind an "
                "ephemeral port and read it back with Server::port()\", which an AF_UNIX listener "
                "can never do, so the port would stay 0 and anything waiting to learn it would "
                "wait forever",
                index);
        if (listener.unix_socket_path.empty() && listener.bind_address.empty())
            return std::format("listeners[{}] has neither a bind_address nor a unix_socket_path",
                               index);

        if (auto tls_problem = problem_with_tls(listener.tls, index))
            return tls_problem;

        if (listener.port == 0 && listener.unix_socket_path.empty())
            continue;
        std::string endpoint =
            listener.unix_socket_path.empty()
                ? std::format("{}:{}", listener.bind_address, listener.port)
                : std::format("unix:{}", listener.unix_socket_path);
        if (std::ranges::contains(endpoints, endpoint))
            return std::format("listeners[{}] repeats the endpoint {}; with reuse_port the "
                               "duplicate binds successfully and silently takes a share of the "
                               "traffic instead of failing",
                               index, endpoint);
        endpoints.push_back(std::move(endpoint));
    }

    for (const std::string& entry : config.allow_cidrs) {
        const std::array<std::string, 1> single{entry};
        const auto parsed = CidrAllowList::parse(single);
        if (!parsed)
            return std::format("allow_cidrs entry \"{}\" is invalid: {}", entry,
                               cidr_error_text(parsed.error()));
    }

    if (has_control_character(config.server_header))
        return "server_header contains a control character; a CR or an LF there would split every "
               "response this server writes";

    return std::nullopt;
}

[[nodiscard]] TlsOptions to_tls_options(const TlsConfig& config) {
    TlsOptions tls;
    tls.enabled = config.enabled;
    tls.certificate_chain_file = config.certificate_chain_file;
    tls.private_key_file = config.private_key_file;
    tls.minimum_version =
        config.minimum_version == kTls12Name ? TlsVersion::Tls12 : TlsVersion::Tls13;
    tls.client_ca_file = config.client_ca_file;
    tls.require_client_certificate = config.require_client_certificate;
    tls.alpn_protocols = config.alpn_protocols;
    tls.group_list = config.group_list;
    tls.session_tickets = config.session_tickets;
    tls.early_data = config.early_data;
    tls.kernel_tls = config.kernel_tls;
    tls.strict_transport_security = config.strict_transport_security;
    return tls;
}

} // namespace

std::string_view config_error_message(ConfigError error) noexcept {
    switch (error) {
    case ConfigError::NotFound:
        return "the configuration file does not exist";
    case ConfigError::Unreadable:
        return "the configuration file exists but could not be read";
    case ConfigError::UnknownFormat:
        return "the configuration file extension is neither .yml/.yaml nor .json";
    case ConfigError::MalformedYaml:
        return "the configuration file is not well-formed YAML";
    case ConfigError::MalformedJson:
        return "the configuration file is not well-formed JSON";
    case ConfigError::InvalidValue:
        return "the configuration contains a value this server cannot honour";
    }
    return "the configuration could not be loaded";
}

namespace {

constexpr size_t kMaxConfigFileBytes = 1'048'576;

enum class ConfigFormat : uint8_t { Yaml, Json };

[[nodiscard]] std::string lowercased(std::string_view text) {
    std::string lowered(text);
    for (char& character : lowered)
        character = ascii_to_lower(character);
    return lowered;
}

[[nodiscard]] std::optional<ConfigFormat> format_for_extension(std::string_view extension) {
    const std::string lowered = lowercased(extension);
    if (lowered == ".yml" || lowered == ".yaml")
        return ConfigFormat::Yaml;
    if (lowered == ".json")
        return ConfigFormat::Json;
    return std::nullopt;
}

void log_config_problem(std::string_view subject, std::string_view detail) noexcept {
    try {
        const LogSink sink = stderr_log_sink();
        sink(LogLevel::Error, std::format("configuration {}: {}", subject, detail));
    } catch (...) {
        std::fputs("[error] configuration error could not be logged\n", stderr);
    }
}

[[nodiscard]] std::expected<std::string, std::string> read_whole_file(
    const std::filesystem::path& path)
    ERIKSLUND_HTTP_POST(result: !result.has_value() || result->size() <= kMaxConfigFileBytes) {
    std::error_code size_error;
    const auto size = std::filesystem::file_size(path, size_error);
    if (size_error)
        return std::unexpected(
            std::format("the file size could not be determined ({})", size_error.message()));
    if (size > kMaxConfigFileBytes)
        return std::unexpected(std::format("{} bytes exceeds the {}-byte configuration ceiling",
                                           size, kMaxConfigFileBytes));

    std::ifstream stream(path, std::ios::binary);
    if (!stream)
        return std::unexpected("the file could not be opened for reading");

    std::string text(static_cast<size_t>(size), '\0');
    stream.read(text.data(), static_cast<std::streamsize>(size));
    if (stream.bad())
        return std::unexpected("the file could not be read to the end");
    text.resize(static_cast<size_t>(stream.gcount()));
    return text;
}

} // namespace

std::expected<ServerConfig, ConfigError> load_config(const std::filesystem::path& path) {
    const std::string subject = path.string();

    std::error_code status_error;
    const std::filesystem::file_status status = std::filesystem::status(path, status_error);
    if (status_error || !std::filesystem::exists(status)) {
        log_config_problem(subject, "no such file");
        return std::unexpected(ConfigError::NotFound);
    }
    if (!std::filesystem::is_regular_file(status)) {
        log_config_problem(subject, "not a regular file");
        return std::unexpected(ConfigError::Unreadable);
    }

    const auto format = format_for_extension(path.extension().string());
    if (!format) {
        log_config_problem(subject,
                           "unrecognized extension; expected .yml, .yaml or .json. The reader is "
                           "chosen by extension, never by sniffing the contents");
        return std::unexpected(ConfigError::UnknownFormat);
    }

    const auto text = read_whole_file(path);
    if (!text) {
        log_config_problem(subject, text.error());
        return std::unexpected(ConfigError::Unreadable);
    }

    ServerConfig config;
    glz::error_ctx parse_error{};
    if (*format == ConfigFormat::Yaml)
        parse_error = glz::read_yaml(config, *text);
    else
        parse_error = glz::read_json(config, *text);
    if (parse_error) {
        log_config_problem(subject, glz::format_error(parse_error, *text));
        return std::unexpected(*format == ConfigFormat::Yaml ? ConfigError::MalformedYaml
                                                             : ConfigError::MalformedJson);
    }

    if (const auto problem = first_problem_with(config)) {
        log_config_problem(subject, *problem);
        return std::unexpected(ConfigError::InvalidValue);
    }
    return config;
}

std::expected<std::string, ConfigError> dump_config(const ServerConfig& config,
                                                    std::string_view format) {
    const std::string lowered = lowercased(format);
    const bool as_yaml =
        lowered == "yaml" || lowered == "yml" || lowered == ".yaml" || lowered == ".yml";

    if (as_yaml) {
        auto written = glz::write_yaml(config);
        if (!written) {
            log_config_problem("dump", "the configuration could not be written as YAML");
            return std::unexpected(ConfigError::MalformedYaml);
        }
        return std::move(*written);
    }

    std::string buffer;
    if (glz::write<glz::opts{.prettify = true}>(config, buffer)) {
        log_config_problem("dump", "the configuration could not be written as JSON");
        return std::unexpected(ConfigError::MalformedJson);
    }
    return buffer;
}

ServerOptions to_options(const ServerConfig& config) {
    if (const auto problem = first_problem_with(config))
        throw ServerError("invalid configuration: " + *problem);

    ServerOptions options;

    options.listeners.reserve(config.listeners.size());
    for (const ListenerConfig& source : config.listeners) {
        Listener listener;
        listener.bind_address = source.bind_address;
        listener.port = source.port;
        listener.unix_socket_path = source.unix_socket_path;
        listener.tls = to_tls_options(source.tls);
        options.listeners.push_back(std::move(listener));
    }

    options.worker_threads = config.worker_threads;

    options.limits.max_request_line_bytes = config.limits.max_request_line_bytes;
    options.limits.max_header_block_bytes = config.limits.max_header_block_bytes;
    options.limits.max_header_count = config.limits.max_header_count;
    options.limits.max_target_bytes = config.limits.max_target_bytes;
    options.limits.max_body_bytes = config.limits.max_body_bytes;

    options.max_connections = config.max_connections;
    options.max_connections_per_source = config.max_connections_per_source;
    options.max_requests_per_connection = config.max_requests_per_connection;

    options.handshake_timeout = std::chrono::milliseconds{config.handshake_timeout_ms};
    options.header_timeout = std::chrono::milliseconds{config.header_timeout_ms};
    options.body_timeout = std::chrono::milliseconds{config.body_timeout_ms};
    options.write_timeout = std::chrono::milliseconds{config.write_timeout_ms};
    options.keep_alive_idle = std::chrono::milliseconds{config.keep_alive_idle_ms};
    options.request_deadline = std::chrono::milliseconds{config.request_deadline_ms};
    options.stream_idle_timeout = std::chrono::milliseconds{config.stream_idle_timeout_ms};

    options.listen_backlog = config.listen_backlog;
    options.reuse_port = config.reuse_port;
    options.tcp_nodelay = config.tcp_nodelay;
    options.warn_on_public_bind = config.warn_on_public_bind;
    options.allow_cidrs = config.allow_cidrs;
    options.server_header = config.server_header;

    return options;
}

} // namespace erikslund::http
