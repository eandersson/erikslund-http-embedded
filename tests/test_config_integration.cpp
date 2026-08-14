

#include "erikslund/http/build_config.hpp"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include <doctest/doctest.h>

#include "erikslund/http/config.hpp"
#include "erikslund/http/request.hpp"
#include "erikslund/http/response.hpp"
#include "erikslund/http/router.hpp"
#include "erikslund/http/server.hpp"
#include "erikslund/http/tls.hpp"
#include "support/conf_files.hpp"
#include "support/test_client.hpp"

namespace erikslund::http {
namespace {

using test::conf_file_path;
using test::HttpResponse;
using test::simple_request;
using test::started_test_server;
using test::TestClient;

constexpr std::string_view kPlaintextExample = "http.example.yml";
constexpr std::string_view kTlsExample = "http.tls.example.yml";

constexpr std::string_view kProbeRoute = "/config-probe";
constexpr std::string_view kProbeBody = "served from the committed example\n";

constexpr int kOkStatus = 200;

constexpr uint16_t kDocumentedPlaintextPort = 7'777;
constexpr uint16_t kDocumentedTlsPort = 7'778;
constexpr size_t kTlsExampleListenerCount = 2;

[[nodiscard]] Router probe_router() {
    Router router;
    router.get(kProbeRoute, [](const Request&) { return Response::text(std::string(kProbeBody)); });
    return router;
}

[[nodiscard]] ServerOptions options_from_example(std::string_view file_name) {
    const std::filesystem::path example = conf_file_path(file_name);
    REQUIRE_MESSAGE(!example.empty(), std::format("conf/{} was not found from any known root",
                                                  file_name));

    const auto loaded = load_config(example);
    REQUIRE_MESSAGE(loaded.has_value(),
                    std::format("conf/{} must load: {}", file_name,
                                loaded.has_value()
                                    ? std::string_view{}
                                    : config_error_message(loaded.error())));

    return to_options(*loaded);
}

void retarget_to_loopback(Listener& listener) {
    listener.bind_address = test::kLoopbackIpv4;
    listener.port = 0;
}

} // namespace

TEST_CASE("the committed plaintext example serves a request end to end") {
    ServerOptions options = options_from_example(kPlaintextExample);

    REQUIRE_MESSAGE(options.listeners.size() == 1,
                    "the reference example documents exactly one listener");
    retarget_to_loopback(options.listeners.front());

    const auto fixture = started_test_server(probe_router(), std::move(options));

    TestClient client;
    REQUIRE(client.connect(fixture->port()));
    const std::optional<HttpResponse> response = client.request(simple_request("GET", kProbeRoute));
    REQUIRE(response.has_value());
    REQUIRE(response->complete);
    CHECK(response->status_code == kOkStatus);
    CHECK(response->body == kProbeBody);

    CHECK(response->header_value("Server") == kDefaultServerHeader);
}

TEST_CASE("the committed plaintext example needs no allowlist exemption to be reached") {
    const std::filesystem::path example = conf_file_path(kPlaintextExample);
    REQUIRE_FALSE(example.empty());
    const auto loaded = load_config(example);
    REQUIRE(loaded.has_value());
    CHECK(loaded->allow_cidrs.empty());
}

TEST_CASE("the TLS example describes the two listeners its own header promises") {
    const ServerOptions options = options_from_example(kTlsExample);

    REQUIRE(options.listeners.size() == kTlsExampleListenerCount);

    CHECK_FALSE(options.listeners[0].tls.enabled);
    CHECK(options.listeners[0].port == kDocumentedPlaintextPort);

    CHECK(options.listeners[1].tls.enabled);
    CHECK(options.listeners[1].port == kDocumentedTlsPort);
    CHECK(options.listeners[1].tls.minimum_version == TlsVersion::Tls13);

    CHECK(options.allow_cidrs.size() == 3);
    CHECK(options.allow_cidrs[0] == "10.0.1.0/24");
    CHECK(options.allow_cidrs[1] == "127.0.0.1/32");
    CHECK(options.allow_cidrs[2] == "::1/128");

    const ServerOptions defaults;
    CHECK(options.header_timeout == defaults.header_timeout);
    CHECK(options.limits.max_body_bytes == defaults.limits.max_body_bytes);
    CHECK(options.listeners[1].tls.group_list.empty());
    CHECK(options.listeners[1].tls.session_tickets);
    CHECK_FALSE(options.listeners[1].tls.early_data);
}

#if ERIKSLUND_HTTP_TLS

TEST_CASE("the committed TLS example serves both of its listeners end to end") {
    ServerOptions options = options_from_example(kTlsExample);
    REQUIRE(options.listeners.size() == kTlsExampleListenerCount);

    retarget_to_loopback(options.listeners[0]);
    retarget_to_loopback(options.listeners[1]);

    options.listeners[1].tls.certificate_chain_file = ERIKSLUND_HTTP_TEST_CERTIFICATE_FILE;
    options.listeners[1].tls.private_key_file = ERIKSLUND_HTTP_TEST_PRIVATE_KEY_FILE;

    const auto fixture = started_test_server(probe_router(), std::move(options));

    TestClient plaintext;
    REQUIRE(plaintext.connect(fixture->port(0)));
    const std::optional<HttpResponse> over_plaintext =
        plaintext.request(simple_request("GET", kProbeRoute));
    REQUIRE(over_plaintext.has_value());
    CHECK(over_plaintext->status_code == kOkStatus);
    CHECK(over_plaintext->body == kProbeBody);

    TestClient secure;
    REQUIRE_MESSAGE(secure.connect_tls(fixture->port(1)),
                    std::format("the example's TLS listener refused a handshake: {}",
                                secure.tls_error()));
    const std::optional<HttpResponse> over_tls = secure.request(simple_request("GET", kProbeRoute));
    REQUIRE(over_tls.has_value());
    CHECK(over_tls->status_code == kOkStatus);
    CHECK(over_tls->body == kProbeBody);

    CHECK_FALSE(over_tls->has_header("Strict-Transport-Security"));
}

#endif // ERIKSLUND_HTTP_TLS

} // namespace erikslund::http
