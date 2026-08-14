
#include <chrono>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#include <doctest/doctest.h>

#include "erikslund/http/request.hpp"
#include "erikslund/http/response.hpp"
#include "erikslund/http/router.hpp"
#include "erikslund/http/server.hpp"
#include "internal/unique_fd.hpp"
#include "support/test_client.hpp"

namespace erikslund::http {
namespace {

using test::HttpResponse;
using test::simple_request;
using test::started_test_server;
using test::TestClient;
using test::TestServer;

constexpr std::string_view kHelloBody = "hello over unix\n";
constexpr std::string_view kMarkerBytes = "this file belongs to another process\n";
constexpr std::string_view kSocketFileName = "http.sock";
constexpr int kOkStatus = 200;

class TemporaryUnixDirectory {
public:
    TemporaryUnixDirectory() : path_(unique_path()) {
        std::error_code failure;
        std::filesystem::create_directories(path_, failure);
        REQUIRE_MESSAGE(!failure, "the Unix-listener suite needs a writable temporary directory");
    }

    ~TemporaryUnixDirectory() {
        std::error_code ignored;
        std::filesystem::remove_all(path_, ignored);
    }

    TemporaryUnixDirectory(const TemporaryUnixDirectory&) = delete("owns a directory on disk");
    TemporaryUnixDirectory& operator=(const TemporaryUnixDirectory&) =
        delete("owns a directory on disk");

    [[nodiscard]] std::filesystem::path socket_path() const {
        return path_ / kSocketFileName;
    }

    [[nodiscard]] bool write_marker() const {
        std::ofstream file(socket_path(), std::ios::binary | std::ios::trunc);
        file << kMarkerBytes;
        return file.good();
    }

private:
    [[nodiscard]] static std::filesystem::path unique_path() {
        static unsigned sequence = 0;
        ++sequence;
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        return std::filesystem::temp_directory_path() /
               std::format("erikslund-http-unix-{}-{}-{}", ::getpid(), stamp, sequence);
    }

    std::filesystem::path path_;
};

[[nodiscard]] Router make_test_router() {
    Router router;
    router.get("/hello", [](const Request&) { return Response::text(std::string(kHelloBody)); });
    return router;
}

[[nodiscard]] ServerOptions unix_options(const std::filesystem::path& path) {
    ServerOptions options;
    Listener listener;
    listener.unix_socket_path = path.string();
    options.listeners.push_back(std::move(listener));
    options.worker_threads = 1;
    options.warn_on_public_bind = false;
    return options;
}

[[nodiscard]] bool leave_stale_socket(const std::filesystem::path& path) {
    const std::string text = path.string();
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (text.empty() || text.size() >= sizeof(address.sun_path))
        return false;
    std::memcpy(address.sun_path, text.data(), text.size());

    internal::UniqueFd socket(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!socket)
        return false;
    const auto length =
        static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + text.size() + 1);
    return ::bind(socket.get(), reinterpret_cast<const sockaddr*>(&address), length) == 0;
}

[[nodiscard]] bool serves_a_request(const std::filesystem::path& path) {
    TestClient client;
    if (!client.connect_unix(path.string()))
        return false;
    const std::optional<HttpResponse> response =
        client.request(simple_request("GET", "/hello"));
    return response.has_value() && response->complete && response->status_code == kOkStatus &&
           response->body == kHelloBody;
}

} // namespace

TEST_SUITE("unix socket adversarial") {

TEST_CASE("refuses_to_replace_a_regular_file_with_a_unix_listener") {
    const TemporaryUnixDirectory directory;
    const std::filesystem::path path = directory.socket_path();
    REQUIRE(directory.write_marker());

    TestServer fixture(make_test_router(), unix_options(path));
    CHECK_THROWS_AS(fixture.start(), ServerError);

    std::error_code failure;
    CHECK(std::filesystem::is_regular_file(path, failure));
    CHECK_FALSE(failure);
    CHECK(std::filesystem::file_size(path, failure) == kMarkerBytes.size());
    CHECK_FALSE(failure);
}

TEST_CASE("refuses_to_take_over_a_live_unix_listener_and_leaves_it_reachable") {
    const TemporaryUnixDirectory directory;
    const std::filesystem::path path = directory.socket_path();
    const auto owner = started_test_server(make_test_router(), unix_options(path));

    TestServer contender(make_test_router(), unix_options(path));
    CHECK_THROWS_AS(contender.start(), ServerError);
    CHECK_MESSAGE(serves_a_request(path),
                  "probing a live pathname must not disrupt the process that owns it");

    owner->server().stop();
    owner->server().wait();
    CHECK_MESSAGE(!std::filesystem::exists(path),
                  "a clean shutdown must remove the socket pathname it created");
}

TEST_CASE("recovers_a_stale_unix_socket_left_by_a_crashed_process") {
    const TemporaryUnixDirectory directory;
    const std::filesystem::path path = directory.socket_path();
    REQUIRE(leave_stale_socket(path));
    REQUIRE(std::filesystem::exists(path));

    const auto fixture = started_test_server(make_test_router(), unix_options(path));
    CHECK(serves_a_request(path));
}

TEST_CASE("shutdown_never_unlinks_a_path_that_replaced_the_owned_unix_socket") {
    const TemporaryUnixDirectory directory;
    const std::filesystem::path path = directory.socket_path();
    const auto fixture = started_test_server(make_test_router(), unix_options(path));

    std::error_code failure;
    REQUIRE(std::filesystem::remove(path, failure));
    REQUIRE_FALSE(failure);
    REQUIRE(directory.write_marker());

    fixture->server().stop();
    fixture->server().wait();
    CHECK_MESSAGE(std::filesystem::is_regular_file(path, failure),
                  "shutdown may remove only the inode bind created");
    CHECK_FALSE(failure);
    CHECK(std::filesystem::file_size(path, failure) == kMarkerBytes.size());
    CHECK_FALSE(failure);
}

} // TEST_SUITE("unix socket adversarial")

} // namespace erikslund::http
