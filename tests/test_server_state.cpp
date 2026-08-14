#include <doctest/doctest.h>

#include <array>
#include <cstdint>
#include <exception>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "erikslund/http/peer_address.hpp"
#include "erikslund/http/server.hpp"
#include "internal/logger.hpp"
#include "internal/server_state.hpp"

namespace erikslund::http::internal {
namespace {

[[nodiscard]] PeerAddress peer(uint8_t last_byte) {
    PeerAddress address;
    address.bytes.back() = last_byte;
    return address;
}

} // namespace

TEST_CASE("the process-wide admission limit is shared by every source") {
    ServerState state(2, 2, {});
    const auto first = state.admit(peer(1));
    const auto second = state.admit(peer(2));
    const auto refused = state.admit(peer(3));

    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == AdmissionRejection::GlobalLimit);
}

TEST_CASE("one source cannot consume more than its configured connection share") {
    ServerState state(4, 1, {});
    const PeerAddress noisy_source = peer(1);
    const auto first = state.admit(noisy_source);
    const auto refused = state.admit(noisy_source);
    const auto other_source = state.admit(peer(2));

    REQUIRE(first.has_value());
    REQUIRE_FALSE(refused.has_value());
    CHECK(refused.error() == AdmissionRejection::SourceLimit);
    CHECK(other_source.has_value());
}

TEST_CASE("destroying a connection admission returns both of its slots") {
    ServerState state(1, 1, {});
    {
        const auto admitted = state.admit(peer(1));
        REQUIRE(admitted.has_value());
    }

    CHECK(state.admit(peer(1)).has_value());
}

TEST_CASE("a reactor failure stops the server and remains reportable") {
    ServerState state(1, 1, {});
    state.fail(std::make_exception_ptr(std::runtime_error("reactor failed")));

    CHECK(state.stop_token().stop_requested());
    REQUIRE(state.failure() != nullptr);
    CHECK_THROWS_WITH_AS(std::rethrow_exception(state.failure()), "reactor failed",
                         std::runtime_error);
}

TEST_CASE("a throwing application log sink cannot escape the logger") {
    Logger logger([](LogLevel, std::string_view) { throw std::runtime_error("sink failed"); });
    CHECK_NOTHROW(logger.write(LogLevel::Info, "message"));
    CHECK_NOTHROW(logger.write_peer(LogLevel::Error, "peer message"));
}

TEST_CASE("peer-triggered diagnostics are bounded within one logging window") {
    std::vector<std::string> messages;
    Logger logger([&messages](LogLevel, std::string_view message) {
        messages.emplace_back(message);
    });

    for (unsigned index = 0; index < kPeerLogBurst * 2; ++index)
        logger.write_peer(LogLevel::Error, "peer failure");

    REQUIRE(messages.size() == kPeerLogBurst + 1);
    CHECK(messages.back().find("suppressed") != std::string::npos);
}

} // namespace erikslund::http::internal
