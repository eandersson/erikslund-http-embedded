
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <ios>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include <doctest/doctest.h>

#include "erikslund/http/config.hpp"
#include "erikslund/http/request.hpp"
#include "erikslund/http/server.hpp"
#include "erikslund/http/tls.hpp"

namespace erikslund::http {

namespace {

[[nodiscard]] std::string rejection_reason(const ServerConfig& config) {
    try {
        static_cast<void>(to_options(config));
    } catch (const ServerError& refusal) {
        return refusal.what();
    }
    return {};
}

class TemporaryDirectory {
public:
    TemporaryDirectory() : path_(unique_path()) {
        std::error_code failure;
        std::filesystem::create_directories(path_, failure);
        REQUIRE_MESSAGE(!failure, "the suite needs a writable temporary directory");
    }

    ~TemporaryDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TemporaryDirectory(const TemporaryDirectory&) = delete("owns a directory on disk");
    TemporaryDirectory& operator=(const TemporaryDirectory&) = delete("owns a directory on disk");

    [[nodiscard]] std::filesystem::path write(std::string_view file_name,
                                              std::string_view text) const {
        const std::filesystem::path file = path_ / file_name;
        std::ofstream stream(file, std::ios::binary | std::ios::trunc);
        REQUIRE(stream.good());
        stream.write(text.data(), static_cast<std::streamsize>(text.size()));
        stream.close();
        REQUIRE(std::filesystem::exists(file));
        return file;
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    [[nodiscard]] static std::filesystem::path unique_path() {
        static int sequence = 0;
        ++sequence;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        return std::filesystem::temp_directory_path() /
               std::format("erikslund-http-config-{}-{}", stamp, sequence);
    }

    std::filesystem::path path_;
};

[[nodiscard]] std::filesystem::path example_config_path() {
    std::vector<std::filesystem::path> candidates;
    candidates.push_back(std::filesystem::path(__FILE__).parent_path().parent_path() / "conf" /
                         "http.example.yml");
#ifdef ERIKSLUND_HTTP_TEST_SOURCE_DIR
    candidates.push_back(std::filesystem::path(ERIKSLUND_HTTP_TEST_SOURCE_DIR) / "conf" /
                         "http.example.yml");
#endif
    candidates.emplace_back("/src/conf/http.example.yml");
    candidates.emplace_back("conf/http.example.yml");
    candidates.emplace_back("../conf/http.example.yml");
    candidates.emplace_back("../../conf/http.example.yml");

    for (const std::filesystem::path& candidate : candidates) {
        std::error_code ignored;
        if (std::filesystem::is_regular_file(candidate, ignored))
            return candidate;
    }
    return {};
}

void check_tls_options_match(const TlsOptions& left, const TlsOptions& right) {
    CHECK(left.enabled == right.enabled);
    CHECK(left.certificate_chain_file == right.certificate_chain_file);
    CHECK(left.private_key_file == right.private_key_file);
    CHECK(left.certificate_chain_pem == right.certificate_chain_pem);
    CHECK(left.private_key_pem == right.private_key_pem);
    CHECK(left.minimum_version == right.minimum_version);
    CHECK(left.client_ca_file == right.client_ca_file);
    CHECK(left.require_client_certificate == right.require_client_certificate);
    CHECK(left.alpn_protocols == right.alpn_protocols);
    CHECK(left.group_list == right.group_list);
    CHECK(left.session_tickets == right.session_tickets);
    CHECK(left.early_data == right.early_data);
    CHECK(left.kernel_tls == right.kernel_tls);
    CHECK(left.strict_transport_security == right.strict_transport_security);
    CHECK(left.hsts_max_age_seconds == right.hsts_max_age_seconds);
}

void check_options_match(const ServerOptions& left, const ServerOptions& right) {
    REQUIRE(left.listeners.size() == right.listeners.size());
    for (size_t index = 0; index < left.listeners.size(); ++index) {
        CHECK(left.listeners[index].bind_address == right.listeners[index].bind_address);
        CHECK(left.listeners[index].port == right.listeners[index].port);
        CHECK(left.listeners[index].unix_socket_path == right.listeners[index].unix_socket_path);
        check_tls_options_match(left.listeners[index].tls, right.listeners[index].tls);
    }

    CHECK(left.worker_threads == right.worker_threads);
    CHECK(left.limits.max_request_line_bytes == right.limits.max_request_line_bytes);
    CHECK(left.limits.max_header_block_bytes == right.limits.max_header_block_bytes);
    CHECK(left.limits.max_header_count == right.limits.max_header_count);
    CHECK(left.limits.max_target_bytes == right.limits.max_target_bytes);
    CHECK(left.limits.max_body_bytes == right.limits.max_body_bytes);
    CHECK(left.max_connections == right.max_connections);
    CHECK(left.max_connections_per_source == right.max_connections_per_source);
    CHECK(left.max_requests_per_connection == right.max_requests_per_connection);
    CHECK(left.handshake_timeout == right.handshake_timeout);
    CHECK(left.header_timeout == right.header_timeout);
    CHECK(left.body_timeout == right.body_timeout);
    CHECK(left.write_timeout == right.write_timeout);
    CHECK(left.keep_alive_idle == right.keep_alive_idle);
    CHECK(left.request_deadline == right.request_deadline);
    CHECK(left.listen_backlog == right.listen_backlog);
    CHECK(left.reuse_port == right.reuse_port);
    CHECK(left.tcp_nodelay == right.tcp_nodelay);
    CHECK(left.warn_on_public_bind == right.warn_on_public_bind);
    CHECK(left.allow_cidrs == right.allow_cidrs);
    CHECK(left.server_header == right.server_header);
}

constexpr std::string_view kEquivalentYaml = R"YAML(listeners:
  - bind_address: "127.0.0.1"
    port: 8080
    unix_socket_path: ""
    tls:
      enabled: false
      certificate_chain_file: ""
      private_key_file: ""
      minimum_version: "1.2"
      client_ca_file: ""
      require_client_certificate: false
      alpn_protocols: "http/1.1"
      group_list: "X25519MLKEM768"
      session_tickets: false
      early_data: true
      kernel_tls: false
      strict_transport_security: true
worker_threads: 3
limits:
  max_request_line_bytes: 4096
  max_header_block_bytes: 8192
  max_header_count: 32
  max_target_bytes: 1024
  max_body_bytes: 2048
max_connections: 64
max_connections_per_source: 16
max_requests_per_connection: 20
handshake_timeout_ms: 1000
header_timeout_ms: 1500
body_timeout_ms: 2000
write_timeout_ms: 2500
keep_alive_idle_ms: 3000
request_deadline_ms: 3500
listen_backlog: 32
reuse_port: false
tcp_nodelay: false
warn_on_public_bind: false
allow_cidrs:
  - "127.0.0.1/32"
  - "::1/128"
server_header: "erikslund-http-test"
)YAML";

constexpr std::string_view kEquivalentJson = R"JSON({
  "listeners": [
    {
      "bind_address": "127.0.0.1",
      "port": 8080,
      "unix_socket_path": "",
      "tls": {
        "enabled": false,
        "certificate_chain_file": "",
        "private_key_file": "",
        "minimum_version": "1.2",
        "client_ca_file": "",
        "require_client_certificate": false,
        "alpn_protocols": "http/1.1",
        "group_list": "X25519MLKEM768",
        "session_tickets": false,
        "early_data": true,
        "kernel_tls": false,
        "strict_transport_security": true
      }
    }
  ],
  "worker_threads": 3,
  "limits": {
    "max_request_line_bytes": 4096,
    "max_header_block_bytes": 8192,
    "max_header_count": 32,
    "max_target_bytes": 1024,
    "max_body_bytes": 2048
  },
  "max_connections": 64,
  "max_connections_per_source": 16,
  "max_requests_per_connection": 20,
  "handshake_timeout_ms": 1000,
  "header_timeout_ms": 1500,
  "body_timeout_ms": 2000,
  "write_timeout_ms": 2500,
  "keep_alive_idle_ms": 3000,
  "request_deadline_ms": 3500,
  "listen_backlog": 32,
  "reuse_port": false,
  "tcp_nodelay": false,
  "warn_on_public_bind": false,
  "allow_cidrs": ["127.0.0.1/32", "::1/128"],
  "server_header": "erikslund-http-test"
})JSON";

} // namespace

TEST_CASE("every config error carries its own explanatory sentence") {
    constexpr std::array<ConfigError, 6> kEveryError{
        ConfigError::NotFound,      ConfigError::Unreadable,    ConfigError::UnknownFormat,
        ConfigError::MalformedYaml, ConfigError::MalformedJson, ConfigError::InvalidValue};

    for (const ConfigError error : kEveryError)
        CHECK_FALSE(config_error_message(error).empty());

    for (size_t left = 0; left < kEveryError.size(); ++left)
        for (size_t right = left + 1; right < kEveryError.size(); ++right)
            CHECK(config_error_message(kEveryError[left]) !=
                  config_error_message(kEveryError[right]));
}

TEST_CASE("a default constructed configuration converts without complaint") {
    CHECK(rejection_reason(ServerConfig{}).empty());

    const ServerOptions options = to_options(ServerConfig{});
    CHECK(options.listeners.empty());
    CHECK(options.server_header == kDefaultServerHeader);
    CHECK(options.limits.max_header_count == kMaxParsedHeaders);
}

TEST_CASE("to_options refuses a unix socket listener that also asks for an ephemeral port") {
    ServerConfig config;
    ListenerConfig listener;
    listener.unix_socket_path = "/run/erikslund/http.sock";
    listener.port = 0;
    config.listeners.push_back(listener);

    const std::string reason = rejection_reason(config);
    REQUIRE_FALSE(reason.empty());
    CHECK(reason.find("unix_socket_path") != std::string::npos);
    CHECK(reason.find("port") != std::string::npos);
    CHECK_THROWS_AS(static_cast<void>(to_options(config)), ServerError);
}

TEST_CASE("to_options accepts a unix socket listener that leaves the port alone") {
    ServerConfig config;
    ListenerConfig listener;
    listener.unix_socket_path = "/run/erikslund/http.sock";
    config.listeners.push_back(listener);

    CHECK(rejection_reason(config).empty());
}

TEST_CASE("to_options refuses a tls listener with no certificate material") {
    ServerConfig missing_chain;
    ListenerConfig chainless;
    chainless.tls.enabled = true;
    chainless.tls.private_key_file = "/etc/erikslund/tls/privkey.pem";
    missing_chain.listeners.push_back(chainless);

    const std::string chain_reason = rejection_reason(missing_chain);
    REQUIRE_FALSE(chain_reason.empty());
    CHECK(chain_reason.find("certificate_chain_file") != std::string::npos);

    ServerConfig missing_key;
    ListenerConfig keyless;
    keyless.tls.enabled = true;
    keyless.tls.certificate_chain_file = "/etc/erikslund/tls/fullchain.pem";
    missing_key.listeners.push_back(keyless);

    const std::string key_reason = rejection_reason(missing_key);
    REQUIRE_FALSE(key_reason.empty());
    CHECK(key_reason.find("private_key_file") != std::string::npos);

    ServerConfig complete;
    ListenerConfig configured;
    configured.tls.enabled = true;
    configured.tls.certificate_chain_file = "/etc/erikslund/tls/fullchain.pem";
    configured.tls.private_key_file = "/etc/erikslund/tls/privkey.pem";
    complete.listeners.push_back(configured);

    REQUIRE(rejection_reason(complete).empty());
    const ServerOptions options = to_options(complete);
    REQUIRE(options.listeners.size() == 1);
    CHECK(options.listeners[0].tls.certificate_chain_pem.empty());
    CHECK(options.listeners[0].tls.private_key_pem.empty());
}

TEST_CASE("to_options refuses mutual tls without a ca bundle to verify against") {
    ServerConfig config;
    ListenerConfig listener;
    listener.tls.enabled = true;
    listener.tls.certificate_chain_file = "/etc/erikslund/tls/fullchain.pem";
    listener.tls.private_key_file = "/etc/erikslund/tls/privkey.pem";
    listener.tls.require_client_certificate = true;
    config.listeners.push_back(listener);

    const std::string reason = rejection_reason(config);
    REQUIRE_FALSE(reason.empty());
    CHECK(reason.find("client_ca_file") != std::string::npos);
}

TEST_CASE("to_options refuses a malformed cidr entry and names the entry") {
    ServerConfig config;
    config.allow_cidrs = {"10.0.0.0/8", "10.0.0.0/999", "::1/128"};

    const std::string reason = rejection_reason(config);
    REQUIRE_FALSE(reason.empty());
    CHECK(reason.find("10.0.0.0/999") != std::string::npos);
    CHECK(reason.find("allow_cidrs") != std::string::npos);
}

TEST_CASE("to_options refuses a cidr entry that is not an address at all") {
    ServerConfig config;
    config.allow_cidrs = {"not-an-address/24"};
    CHECK_FALSE(rejection_reason(config).empty());

    ServerConfig two_slashes;
    two_slashes.allow_cidrs = {"10.0.0.0/8/8"};
    CHECK_FALSE(rejection_reason(two_slashes).empty());
}

TEST_CASE("to_options refuses a timeout of zero because there is no disabled sentinel") {
    ServerConfig config;
    config.header_timeout_ms = 0;
    const std::string reason = rejection_reason(config);
    REQUIRE_FALSE(reason.empty());
    CHECK(reason.find("header_timeout_ms") != std::string::npos);
}

TEST_CASE("to_options refuses a per-source connection limit of zero") {
    ServerConfig config;
    config.max_connections_per_source = 0;
    const std::string reason = rejection_reason(config);
    REQUIRE_FALSE(reason.empty());
    CHECK(reason.find("max_connections_per_source") != std::string::npos);
}

TEST_CASE("to_options refuses a header count above the parser's compile-time capacity") {
    ServerConfig config;
    config.limits.max_header_count = kMaxParsedHeaders + 1;
    const std::string reason = rejection_reason(config);
    REQUIRE_FALSE(reason.empty());
    CHECK(reason.find("max_header_count") != std::string::npos);
}

TEST_CASE("to_options refuses a target cap the request line can never reach") {
    ServerConfig config;
    config.limits.max_request_line_bytes = 1'024;
    config.limits.max_target_bytes = 2'048;
    const std::string reason = rejection_reason(config);
    REQUIRE_FALSE(reason.empty());
    CHECK(reason.find("max_target_bytes") != std::string::npos);
}

TEST_CASE("to_options refuses two listeners that would silently share one endpoint") {
    ServerConfig config;
    ListenerConfig first;
    first.bind_address = "127.0.0.1";
    first.port = 8'080;
    ListenerConfig second = first;
    config.listeners.push_back(first);
    config.listeners.push_back(second);

    const std::string reason = rejection_reason(config);
    REQUIRE_FALSE(reason.empty());
    CHECK(reason.find("127.0.0.1:8080") != std::string::npos);
}

TEST_CASE("to_options refuses a server header that would split every response") {
    ServerConfig config;
    config.server_header = "erikslund\r\nX-Injected: yes";
    const std::string reason = rejection_reason(config);
    REQUIRE_FALSE(reason.empty());
    CHECK(reason.find("server_header") != std::string::npos);
}

TEST_CASE("to_options carries every timeout across as milliseconds") {
    ServerConfig config;
    config.handshake_timeout_ms = 1'100;
    config.header_timeout_ms = 1'200;
    config.body_timeout_ms = 1'300;
    config.write_timeout_ms = 1'400;
    config.keep_alive_idle_ms = 1'500;
    config.request_deadline_ms = 1'600;

    const ServerOptions options = to_options(config);
    CHECK(options.handshake_timeout == std::chrono::milliseconds{1'100});
    CHECK(options.header_timeout == std::chrono::milliseconds{1'200});
    CHECK(options.body_timeout == std::chrono::milliseconds{1'300});
    CHECK(options.write_timeout == std::chrono::milliseconds{1'400});
    CHECK(options.keep_alive_idle == std::chrono::milliseconds{1'500});
    CHECK(options.request_deadline == std::chrono::milliseconds{1'600});
}

TEST_CASE("to_options maps the minimum version string onto the tls enumerator") {
    ServerConfig thirteen;
    ListenerConfig modern;
    modern.tls.minimum_version = "1.3";
    thirteen.listeners.push_back(modern);
    const ServerOptions modern_options = to_options(thirteen);
    REQUIRE(modern_options.listeners.at(0).tls.minimum_version == TlsVersion::Tls13);

    ServerConfig twelve;
    ListenerConfig legacy;
    legacy.tls.minimum_version = "1.2";
    twelve.listeners.push_back(legacy);
    const ServerOptions legacy_options = to_options(twelve);
    REQUIRE(legacy_options.listeners.at(0).tls.minimum_version == TlsVersion::Tls12);

    ServerConfig nonsense;
    ListenerConfig mistyped;
    mistyped.tls.minimum_version = "1.4";
    nonsense.listeners.push_back(mistyped);
    const std::string reason = rejection_reason(nonsense);
    REQUIRE_FALSE(reason.empty());
    CHECK(reason.find("minimum_version") != std::string::npos);
}

TEST_CASE("to_options refuses an alpn protocol this server cannot speak") {
    ServerConfig config;
    ListenerConfig listener;
    listener.tls.alpn_protocols = "h2,http/1.1";
    config.listeners.push_back(listener);

    const std::string reason = rejection_reason(config);
    REQUIRE_FALSE(reason.empty());
    CHECK(reason.find("h2") != std::string::npos);
}

TEST_CASE("a yaml config and the equivalent json config produce identical server options") {
    const TemporaryDirectory directory;
    const auto yaml_file = directory.write("http.yml", kEquivalentYaml);
    const auto json_file = directory.write("http.json", kEquivalentJson);

    const auto from_yaml = load_config(yaml_file);
    REQUIRE_MESSAGE(from_yaml.has_value(), "the yaml fixture must load");
    const auto from_json = load_config(json_file);
    REQUIRE_MESSAGE(from_json.has_value(), "the json fixture must load");

    check_options_match(to_options(*from_yaml), to_options(*from_json));

    const ServerOptions options = to_options(*from_yaml);
    REQUIRE(options.listeners.size() == 1);
    CHECK(options.listeners[0].bind_address == "127.0.0.1");
    CHECK(options.listeners[0].port == 8'080);
    CHECK(options.listeners[0].tls.minimum_version == TlsVersion::Tls12);
    CHECK(options.listeners[0].tls.group_list == "X25519MLKEM768");
    CHECK_FALSE(options.listeners[0].tls.session_tickets);
    CHECK(options.listeners[0].tls.early_data);
    CHECK(options.worker_threads == 3);
    CHECK(options.limits.max_header_count == 32);
    CHECK(options.max_connections == 64);
    CHECK(options.max_connections_per_source == 16);
    CHECK(options.request_deadline == std::chrono::milliseconds{3'500});
    CHECK(options.listen_backlog == 32);
    CHECK_FALSE(options.reuse_port);
    CHECK(options.allow_cidrs == std::vector<std::string>{"127.0.0.1/32", "::1/128"});
    CHECK(options.server_header == "erikslund-http-test");
}

TEST_CASE("a file that is not there is reported as not found") {
    const TemporaryDirectory directory;
    const auto loaded = load_config(directory.path() / "absent.yml");
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error() == ConfigError::NotFound);
}

TEST_CASE("a directory in place of a file is reported as unreadable rather than not found") {
    const TemporaryDirectory directory;
    const auto loaded = load_config(directory.path());
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error() == ConfigError::Unreadable);
}

TEST_CASE("the reader is chosen by extension and an unknown one is refused rather than sniffed") {
    const TemporaryDirectory directory;
    const auto loaded = load_config(directory.write("http.conf", kEquivalentYaml));
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error() == ConfigError::UnknownFormat);

    const auto shouting = load_config(directory.write("http.YAML", kEquivalentYaml));
    CHECK(shouting.has_value());
}

TEST_CASE("a yaml file that is not well-formed is reported as malformed yaml") {
    const TemporaryDirectory directory;
    const auto loaded =
        load_config(directory.write("broken.yml", "server_header: \"unterminated\nport: 1\n"));
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error() == ConfigError::MalformedYaml);
}

TEST_CASE("a json file that is not well-formed is reported as malformed json") {
    const TemporaryDirectory directory;
    const auto loaded = load_config(directory.write("broken.json", R"({"worker_threads": 2)"));
    REQUIRE_FALSE(loaded.has_value());
    CHECK(loaded.error() == ConfigError::MalformedJson);
}

TEST_CASE("an unknown key is refused rather than silently ignored") {
    const TemporaryDirectory directory;

    const auto yaml = load_config(directory.write("typo.yml", "worker_thread: 2\n"));
    REQUIRE_MESSAGE(!yaml.has_value(), "a misspelled yaml key must not be accepted");
    CHECK(yaml.error() == ConfigError::MalformedYaml);

    const auto json = load_config(directory.write("typo.json", R"({"worker_thread": 2})"));
    REQUIRE_MESSAGE(!json.has_value(), "a misspelled json key must not be accepted");
    CHECK(json.error() == ConfigError::MalformedJson);

    const auto nested = load_config(
        directory.write("nested.yml", "listeners:\n  - bind_addres: \"::1\"\n"));
    CHECK_FALSE(nested.has_value());
}

TEST_CASE("a wrong typed value is refused rather than coerced") {
    const TemporaryDirectory directory;

    const auto yaml = load_config(directory.write("typed.yml", "worker_threads: three\n"));
    REQUIRE_MESSAGE(!yaml.has_value(), "a word where a count belongs must not be read as zero");
    CHECK(yaml.error() == ConfigError::MalformedYaml);

    const auto json = load_config(directory.write("typed.json", R"({"worker_threads": "three"})"));
    REQUIRE(!json.has_value());
    CHECK(json.error() == ConfigError::MalformedJson);

    const auto boolean =
        load_config(directory.write("typed-bool.json", R"({"reuse_port": "yes"})"));
    CHECK_FALSE(boolean.has_value());
}

TEST_CASE("a value the server cannot honour is reported as an invalid value") {
    const TemporaryDirectory directory;

    const auto zero_timeout =
        load_config(directory.write("timeout.yml", "header_timeout_ms: 0\n"));
    REQUIRE_FALSE(zero_timeout.has_value());
    CHECK(zero_timeout.error() == ConfigError::InvalidValue);

    const auto bad_cidr =
        load_config(directory.write("cidr.yml", "allow_cidrs:\n  - \"10.0.0.0/999\"\n"));
    REQUIRE_FALSE(bad_cidr.has_value());
    CHECK(bad_cidr.error() == ConfigError::InvalidValue);

    const auto unix_with_ephemeral_port = load_config(directory.write(
        "unix.yml", "listeners:\n  - unix_socket_path: \"/run/http.sock\"\n    port: 0\n"));
    REQUIRE_FALSE(unix_with_ephemeral_port.has_value());
    CHECK(unix_with_ephemeral_port.error() == ConfigError::InvalidValue);

    const auto tls_without_certificate = load_config(directory.write(
        "tls.yml", "listeners:\n  - bind_address: \"::\"\n    tls:\n      enabled: true\n"));
    REQUIRE_FALSE(tls_without_certificate.has_value());
    CHECK(tls_without_certificate.error() == ConfigError::InvalidValue);
}

TEST_CASE("an omitted key keeps its default so commenting a line out is always safe") {
    const TemporaryDirectory directory;
    const auto loaded = load_config(directory.write("sparse.yml", "worker_threads: 2\n"));
    REQUIRE(loaded.has_value());

    CHECK(loaded->worker_threads == 2);
    CHECK(loaded->listeners.empty());
    CHECK(loaded->max_connections == kDefaultMaxConnections);
    CHECK(loaded->max_connections_per_source == kDefaultMaxConnectionsPerSource);
    CHECK(loaded->limits.max_header_count == kMaxParsedHeaders);
    CHECK(loaded->server_header == kDefaultServerHeader);
    CHECK(loaded->reuse_port);
}

TEST_CASE("dump_config writes a document the loader reads back unchanged") {
    const TemporaryDirectory directory;
    const auto original = load_config(directory.write("http.yml", kEquivalentYaml));
    REQUIRE(original.has_value());

    const auto as_yaml = dump_config(*original, "yaml");
    REQUIRE(as_yaml.has_value());
    const auto reloaded_yaml = load_config(directory.write("dumped.yml", *as_yaml));
    REQUIRE_MESSAGE(reloaded_yaml.has_value(),
                    "the dump must be loadable by the loader that produced it");
    check_options_match(to_options(*original), to_options(*reloaded_yaml));

    const auto as_json = dump_config(*original, "json");
    REQUIRE(as_json.has_value());
    const auto reloaded_json = load_config(directory.write("dumped.json", *as_json));
    REQUIRE(reloaded_json.has_value());
    check_options_match(to_options(*original), to_options(*reloaded_json));
}

TEST_CASE("dump_config prints a configuration the loader would refuse") {
    ServerConfig unservable;
    unservable.header_timeout_ms = 0;
    const auto dumped = dump_config(unservable, "yaml");
    REQUIRE(dumped.has_value());
    CHECK(dumped->find("header_timeout_ms") != std::string::npos);
}

TEST_CASE("the committed example configuration parses") {
    const std::filesystem::path example = example_config_path();
    REQUIRE_MESSAGE(!example.empty(), "conf/http.example.yml was not found from any known root");

    const auto loaded = load_config(example);
    REQUIRE_MESSAGE(loaded.has_value(), "the committed example configuration must load");
    CHECK(rejection_reason(*loaded).empty());
}

TEST_CASE("the committed example configuration states the library defaults") {
    const std::filesystem::path example = example_config_path();
    REQUIRE_FALSE(example.empty());
    const auto loaded = load_config(example);
    REQUIRE(loaded.has_value());

    const ServerConfig defaults;
    const ListenerConfig default_listener;

    REQUIRE_MESSAGE(loaded->listeners.size() == 1,
                    "the example spells out exactly the one listener an absent list implies");
    CHECK(loaded->listeners[0].bind_address == default_listener.bind_address);
    CHECK(loaded->listeners[0].port == default_listener.port);
    CHECK(loaded->listeners[0].unix_socket_path == default_listener.unix_socket_path);
    CHECK(loaded->listeners[0].tls.enabled == default_listener.tls.enabled);
    CHECK(loaded->listeners[0].tls.certificate_chain_file ==
          default_listener.tls.certificate_chain_file);
    CHECK(loaded->listeners[0].tls.private_key_file == default_listener.tls.private_key_file);
    CHECK(loaded->listeners[0].tls.minimum_version == default_listener.tls.minimum_version);
    CHECK(loaded->listeners[0].tls.client_ca_file == default_listener.tls.client_ca_file);
    CHECK(loaded->listeners[0].tls.require_client_certificate ==
          default_listener.tls.require_client_certificate);
    CHECK(loaded->listeners[0].tls.alpn_protocols == default_listener.tls.alpn_protocols);
    CHECK(loaded->listeners[0].tls.group_list == default_listener.tls.group_list);
    CHECK(loaded->listeners[0].tls.session_tickets == default_listener.tls.session_tickets);
    CHECK(loaded->listeners[0].tls.early_data == default_listener.tls.early_data);
    CHECK(loaded->listeners[0].tls.kernel_tls == default_listener.tls.kernel_tls);
    CHECK(loaded->listeners[0].tls.strict_transport_security ==
          default_listener.tls.strict_transport_security);

    CHECK(loaded->worker_threads == defaults.worker_threads);
    CHECK(loaded->limits.max_request_line_bytes == defaults.limits.max_request_line_bytes);
    CHECK(loaded->limits.max_header_block_bytes == defaults.limits.max_header_block_bytes);
    CHECK(loaded->limits.max_header_count == defaults.limits.max_header_count);
    CHECK(loaded->limits.max_target_bytes == defaults.limits.max_target_bytes);
    CHECK(loaded->limits.max_body_bytes == defaults.limits.max_body_bytes);
    CHECK(loaded->max_connections == defaults.max_connections);
    CHECK(loaded->max_connections_per_source == defaults.max_connections_per_source);
    CHECK(loaded->max_requests_per_connection == defaults.max_requests_per_connection);
    CHECK(loaded->handshake_timeout_ms == defaults.handshake_timeout_ms);
    CHECK(loaded->header_timeout_ms == defaults.header_timeout_ms);
    CHECK(loaded->body_timeout_ms == defaults.body_timeout_ms);
    CHECK(loaded->write_timeout_ms == defaults.write_timeout_ms);
    CHECK(loaded->keep_alive_idle_ms == defaults.keep_alive_idle_ms);
    CHECK(loaded->request_deadline_ms == defaults.request_deadline_ms);
    CHECK(loaded->listen_backlog == defaults.listen_backlog);
    CHECK(loaded->reuse_port == defaults.reuse_port);
    CHECK(loaded->tcp_nodelay == defaults.tcp_nodelay);
    CHECK(loaded->warn_on_public_bind == defaults.warn_on_public_bind);
    CHECK(loaded->allow_cidrs == defaults.allow_cidrs);
    CHECK(loaded->server_header == defaults.server_header);
}

} // namespace erikslund::http
