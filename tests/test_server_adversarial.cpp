
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <dirent.h>
#include <netinet/in.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <unistd.h>

#include <doctest/doctest.h>

#include "erikslund/http/metrics.hpp"
#include "erikslund/http/request.hpp"
#include "erikslund/http/response.hpp"
#include "erikslund/http/router.hpp"
#include "erikslund/http/server.hpp"
#include "erikslund/http/sse.hpp"
#include "support/test_client.hpp"

namespace erikslund::http {
namespace {

using test::BodyExpectation;
using test::CapturedLog;
using test::HttpResponse;
using test::loopback_options;
using test::simple_request;
using test::started_test_server;
using test::TestClient;
using test::TestServer;
using test::wait_until;

constexpr std::string_view kHelloBody = "hello, world\n";
constexpr std::string_view kOctetStreamContentType = "application/octet-stream";

constexpr int kOkStatus = 200;
constexpr int kBadRequestStatus = 400;
constexpr int kContentTooLargeStatus = 413;
constexpr int kHeaderFieldsTooLargeStatus = 431;

constexpr size_t kLargeBodyBytes = 1'048'576;

constexpr std::chrono::milliseconds kSlowlorisByteInterval{100};
constexpr std::chrono::milliseconds kSlowlorisRequestDeadline{700};

constexpr std::chrono::milliseconds kSlowlorisDisconnectBound{4'000};

constexpr std::chrono::milliseconds kSlowlorisEarliestDisconnect{300};

constexpr std::chrono::milliseconds kRelaxedPhaseTimeout{30'000};

constexpr size_t kSmallBodyLimitBytes = 1'024;
constexpr size_t kSmallHeaderBlockLimitBytes = 2'048;
constexpr size_t kOversizedDeclaredLength = 65'536;
constexpr size_t kHeaderPaddingFieldCount = 64;
constexpr size_t kHeaderPaddingValueBytes = 96;

constexpr size_t kTightRequestLineLimitBytes = 1'024;
constexpr size_t kTightHeaderBlockLimitBytes = 1'024;

constexpr size_t kBodyOverTheHeaderBudgetBytes = 65'536;

constexpr size_t kBinaryGarbageBytes = 512;

constexpr size_t kAbandonedConnectionCount = 300;
constexpr size_t kAbandonedShapeCount = 4;
constexpr size_t kWarmupRequestCount = 4;

constexpr std::chrono::milliseconds kSipTimeout{50};

constexpr size_t kReservedDescriptorCount = 64;
constexpr size_t kDescriptorsPerConnection = 2;
constexpr size_t kFallbackDescriptorBudget = 1'024;
constexpr size_t kDesiredConcurrentConnections = 300;
constexpr size_t kMinimumConcurrentConnections = 64;

constexpr size_t kDescriptorDriftTolerance = 16;

constexpr double kSettledConnectionGauge = 0.5;
constexpr std::chrono::milliseconds kSettleBudget{10'000};
constexpr std::string_view kMetricsPrefix = "erikslund_http";
constexpr std::string_view kAcceptedMetricName = "connections_accepted";
constexpr std::string_view kActiveMetricName = "connections_active";
constexpr std::string_view kMetricsHelpText = "read back by the descriptor-leak case";

constexpr std::chrono::milliseconds kBulkReadBudget{30'000};
constexpr std::chrono::milliseconds kOverCapReadBudget{15'000};

constexpr unsigned kSingleWorker = 1;
constexpr unsigned kSeveralWorkers = 4;
constexpr unsigned kTinyConnectionCap = 4;
constexpr size_t kOverCapConnectionCount = 32;
constexpr size_t kConnectionCapHeadroomFactor = 2;
constexpr size_t kConnectionFloodThreadCount = 4;
constexpr std::chrono::milliseconds kConnectionFloodWarmup{100};

constexpr double kBusyLoopCoreFraction = 0.25;

constexpr std::chrono::milliseconds kCpuSampleWindow{1'000};

constexpr std::chrono::milliseconds kSettleBeforeSample{250};

constexpr long kMicrosecondsPerSecond = 1'000'000;

constexpr int kStalledPeerReceiveBufferBytes = 2'048;

constexpr size_t kUnabsorbableBodyBytes = 16'777'216;
constexpr std::string_view kStallingRoute = "/stalling";

constexpr std::chrono::milliseconds kUnhurriedWriteTimeout{20'000};

constexpr std::chrono::milliseconds kBriefStreamIdleTimeout{400};

constexpr int kStreamReclaimIdleBudgets = 12;

constexpr int kStreamSurvivalIdleBudgets = 8;

constexpr std::chrono::milliseconds kStreamPublishInterval{100};

constexpr std::string_view kStreamRoute = "/events";
constexpr std::string_view kEventStreamContentType = "text/event-stream";
constexpr std::string_view kTickEvent = "tick";
constexpr std::string_view kTickPayload = "1";

constexpr std::string_view kStalledStreamRoute = "/events-stalled";
constexpr std::string_view kHeadPaddingField = "X-Pad";

constexpr std::chrono::milliseconds kBriefWriteTimeout{300};

constexpr std::chrono::milliseconds kStreamNotificationInterval{50};
constexpr int kNotifiedStreamReclaimWriteBudgets = 10;

constexpr size_t kSubscriberCeiling = 2;

constexpr size_t kHeadRequestCount = 8;


[[nodiscard]] size_t open_descriptor_count() {
    DIR* const directory = ::opendir("/proc/self/fd");
    if (directory == nullptr)
        return 0;

    size_t count = 0;
    while (const dirent* const entry = ::readdir(directory)) {
        const std::string_view name(entry->d_name);
        if (name == "." || name == "..")
            continue;
        ++count;
    }
    ::closedir(directory);
    return count;
}

[[nodiscard]] size_t affordable_connection_count() {
    rlimit limit{};
    if (::getrlimit(RLIMIT_NOFILE, &limit) != 0)
        return (kFallbackDescriptorBudget - kReservedDescriptorCount) / kDescriptorsPerConnection;
    if (limit.rlim_cur == RLIM_INFINITY)
        return kDesiredConcurrentConnections;
    const auto budget = static_cast<size_t>(limit.rlim_cur);
    if (budget <= kReservedDescriptorCount)
        return 0;
    return (budget - kReservedDescriptorCount) / kDescriptorsPerConnection;
}


[[nodiscard]] const std::string& large_body() {
    static const std::string body(kLargeBodyBytes, 'x');
    return body;
}

[[nodiscard]] const std::string& unabsorbable_body() {
    static const std::string body(kUnabsorbableBodyBytes, 'x');
    return body;
}

void flood_listener(const std::stop_token& stop, uint16_t port) {
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(port);
    static_cast<void>(::inet_pton(AF_INET, "127.0.0.1", &address.sin_addr));

    while (!stop.stop_requested()) {
        const int connection = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (connection < 0)
            continue;
        static_cast<void>(::connect(connection, reinterpret_cast<const sockaddr*>(&address),
                                    sizeof(address)));
        ::close(connection);
    }
}

[[nodiscard]] Response padded_head(Response response) {
    response.header(std::string(kHeadPaddingField), unabsorbable_body());
    return response;
}

[[nodiscard]] Router make_test_router() {
    Router router;
    router.get("/hello", [](const Request&) { return Response::text(std::string(kHelloBody)); });
    router.get("/large", [](const Request&) {
        return Response::borrowed(large_body(), kOctetStreamContentType);
    });
    router.get(std::string(kStallingRoute), [](const Request&) {
        return Response::borrowed(unabsorbable_body(), kOctetStreamContentType);
    });
    router.post("/echo",
                [](const Request& request) { return Response::text(std::string(request.body())); });
    return router;
}

[[nodiscard]] bool logged_anything_at(const CapturedLog& log, LogLevel level) {
    return log.contains(level, std::string_view{});
}

[[nodiscard]] std::string joined_log(const CapturedLog& log) {
    std::string text;
    for (const std::string& line : log.lines()) {
        text.append(line);
        text.push_back('\n');
    }
    return text;
}

[[nodiscard]] bool serves_a_plain_request(uint16_t port) {
    TestClient client;
    if (!client.connect(port))
        return false;
    const std::optional<HttpResponse> response = client.request(simple_request("GET", "/hello"));
    return response.has_value() && response->complete && response->status_code == kOkStatus &&
           response->body == kHelloBody;
}

[[nodiscard]] bool eventually_serves_a_plain_request(uint16_t port) {
    return wait_until([port] { return serves_a_plain_request(port); });
}

[[nodiscard]] std::string binary_garbage(size_t length) {
    std::string bytes;
    bytes.reserve(length);
    unsigned value = 1;
    while (bytes.size() < length) {
        const auto candidate = static_cast<unsigned char>(value % 256U);
        ++value;
        if (candidate == '\r' || candidate == '\n' || candidate == ' ' || candidate == 0)
            continue;
        bytes.push_back(static_cast<char>(candidate));
    }
    return bytes;
}


[[nodiscard]] double process_cpu_seconds() {
    rusage usage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0)
        return 0.0;
    const auto seconds = [](const timeval& moment) {
        return static_cast<double>(moment.tv_sec) +
               static_cast<double>(moment.tv_usec) / static_cast<double>(kMicrosecondsPerSecond);
    };
    return seconds(usage.ru_utime) + seconds(usage.ru_stime);
}

[[nodiscard]] double core_fraction_over(std::chrono::milliseconds window) {
    const double before = process_cpu_seconds();
    std::this_thread::sleep_for(window);
    const double spent = process_cpu_seconds() - before;
    return spent / std::chrono::duration<double>(window).count();
}

class StalledPeer {
public:
    StalledPeer() = default;
    ~StalledPeer() { disconnect(); }
    StalledPeer(const StalledPeer&) = delete("a peer owns its descriptor");
    StalledPeer& operator=(const StalledPeer&) = delete("a peer owns its descriptor");

    [[nodiscard]] bool connect(uint16_t port) {
        fd_ = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd_ < 0)
            return false;
        const int capacity = kStalledPeerReceiveBufferBytes;
        static_cast<void>(::setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &capacity, sizeof(capacity)));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        if (::inet_pton(AF_INET, test::kLoopbackIpv4, &address.sin_addr) != 1)
            return false;
        return ::connect(fd_, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0;
    }

    [[nodiscard]] bool send(std::string_view bytes) const {
        return fd_ >= 0 &&
               ::send(fd_, bytes.data(), bytes.size(), 0) == static_cast<ssize_t>(bytes.size());
    }

    [[nodiscard]] bool half_close() const { return fd_ >= 0 && ::shutdown(fd_, SHUT_WR) == 0; }

    void disconnect() noexcept {
        if (fd_ >= 0)
            ::close(fd_);
        fd_ = -1;
    }

private:
    int fd_ = -1;
};

class SilentStream final : public StreamSource {
public:
    [[nodiscard]] std::string_view content_type() const noexcept override {
        return kEventStreamContentType;
    }

    [[nodiscard]] Pull pull(std::string&) override { return Pull::Idle; }

    void on_attached(std::shared_ptr<StreamNotifier>) override {
        attached_.store(true, std::memory_order_release);
    }

    void on_detached() noexcept override { detached_.store(true, std::memory_order_release); }

    [[nodiscard]] bool attached() const noexcept {
        return attached_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool detached() const noexcept {
        return detached_.load(std::memory_order_acquire);
    }

private:
    std::atomic<bool> attached_{false};
    std::atomic<bool> detached_{false};
};

class CountedStream final : public StreamSource {
public:
    CountedStream(std::atomic<int>& outstanding, std::atomic<int>& attached) noexcept
        : outstanding_(outstanding), attached_(attached) {
        outstanding_.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] std::string_view content_type() const noexcept override {
        return kEventStreamContentType;
    }

    [[nodiscard]] Pull pull(std::string&) override { return Pull::Idle; }

    void on_attached(std::shared_ptr<StreamNotifier>) override {
        attached_.fetch_add(1, std::memory_order_relaxed);
    }

    void on_detached() noexcept override {
        if (!released_.exchange(true, std::memory_order_acq_rel))
            outstanding_.fetch_sub(1, std::memory_order_relaxed);
    }

private:
    std::atomic<int>& outstanding_;
    std::atomic<int>& attached_;
    std::atomic<bool> released_{false};
};

class NotifyingFloodStream final : public StreamSource {
public:
    [[nodiscard]] std::string_view content_type() const noexcept override {
        return kEventStreamContentType;
    }

    [[nodiscard]] Pull pull(std::string& out) override {
        out += unabsorbable_body();
        return Pull::Wrote;
    }

    void on_attached(std::shared_ptr<StreamNotifier> notifier) override {
        const std::scoped_lock guard(mutex_);
        notifier_ = std::move(notifier);
        attached_.store(true, std::memory_order_release);
    }

    void on_detached() noexcept override {
        {
            const std::scoped_lock guard(mutex_);
            notifier_.reset();
        }
        detached_.store(true, std::memory_order_release);
    }

    void notify() noexcept {
        std::shared_ptr<StreamNotifier> notifier;
        {
            const std::scoped_lock guard(mutex_);
            notifier = notifier_;
        }
        if (notifier)
            notifier->notify();
    }

    [[nodiscard]] bool attached() const noexcept {
        return attached_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool detached() const noexcept {
        return detached_.load(std::memory_order_acquire);
    }

private:
    std::mutex mutex_;
    std::shared_ptr<StreamNotifier> notifier_;
    std::atomic<bool> attached_{false};
    std::atomic<bool> detached_{false};
};

} // namespace


TEST_CASE("closes_a_connection_that_sends_nothing_without_a_response_or_a_logged_error") {
    const auto fixture = started_test_server(make_test_router());

    TestClient client;
    REQUIRE(client.connect(fixture->port()));
    REQUIRE(client.half_close());

    const std::string received = client.read_until_closed();
    CHECK_MESSAGE(received.empty(),
                  "answering a peer that said nothing with a 400 only fills the log");
    CHECK_MESSAGE(client.saw_end_of_stream(),
                  "the descriptor must be reclaimed, not held to its timeout");
    CHECK_MESSAGE(!logged_anything_at(fixture->log(), LogLevel::Warning),
                  std::format("an empty connection is ordinary traffic, not a fault:\n{}",
                              joined_log(fixture->log())));
    CHECK(serves_a_plain_request(fixture->port()));
}

TEST_CASE("disconnects_a_slowloris_client_on_the_request_deadline_not_the_header_timeout") {
    ServerOptions options = loopback_options();
    options.request_deadline = kSlowlorisRequestDeadline;
    options.handshake_timeout = kRelaxedPhaseTimeout;
    options.header_timeout = kRelaxedPhaseTimeout;
    options.body_timeout = kRelaxedPhaseTimeout;
    options.keep_alive_idle = kRelaxedPhaseTimeout;
    const auto fixture = started_test_server(make_test_router(), std::move(options));

    TestClient client;
    REQUIRE(client.connect(fixture->port()));

    const std::string probe = simple_request("GET", "/hello");
    const auto began = std::chrono::steady_clock::now();
    const size_t accepted = client.send_slowly(probe, kSlowlorisByteInterval);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - began);

    REQUIRE_MESSAGE(accepted < probe.size(),
                    "a client dribbling one byte per 100 ms held its worker for the whole request");
    CHECK_MESSAGE(elapsed < kSlowlorisDisconnectBound,
                  std::format("the dribbling client survived {} ms, well past its {} ms budget",
                              elapsed.count(), kSlowlorisRequestDeadline.count()));
    CHECK_MESSAGE(elapsed >= kSlowlorisEarliestDisconnect,
                  std::format("the connection died after {} ms, far too early to be the request "
                              "deadline doing it",
                              elapsed.count()));

    const std::string received = client.read_until_closed();
    CHECK_MESSAGE(client.saw_end_of_stream(),
                  "an expired request must have its descriptor reclaimed");
    if (!received.empty())
        CHECK_MESSAGE(std::string_view(received).starts_with("HTTP/1.1 408"),
                      std::format("the only answer a timed-out request may get is 408, not:\n{}",
                                  received));
    CHECK(serves_a_plain_request(fixture->port()));
}

TEST_CASE("a_partial_pipelined_request_starts_its_deadline_before_another_byte_arrives") {
    ServerOptions options = loopback_options();
    options.request_deadline = kSlowlorisRequestDeadline;
    options.handshake_timeout = kRelaxedPhaseTimeout;
    options.header_timeout = kRelaxedPhaseTimeout;
    options.body_timeout = kRelaxedPhaseTimeout;
    options.keep_alive_idle = kRelaxedPhaseTimeout;
    const auto fixture = started_test_server(make_test_router(), std::move(options));

    TestClient client;
    REQUIRE(client.connect(fixture->port()));
    REQUIRE(client.send_raw(simple_request("GET", "/hello") + "G"));

    const std::optional<HttpResponse> first = client.read_response();
    REQUIRE(first.has_value());
    CHECK(first->status_code == kOkStatus);
    CHECK(first->body == kHelloBody);

    const std::string received = client.read_until_closed(kSlowlorisDisconnectBound);
    CHECK_MESSAGE(client.saw_end_of_stream(),
                  "a buffered request prefix must not inherit the keep-alive idle budget");
    if (!received.empty())
        CHECK_MESSAGE(std::string_view(received).starts_with("HTTP/1.1 408"),
                      std::format("the only answer an expired pipelined request may get is 408, "
                                  "not:\n{}",
                                  received));
    CHECK(serves_a_plain_request(fixture->port()));
}

TEST_CASE("answers_a_body_larger_than_the_configured_limit_with_413_and_closes") {
    ServerOptions options = loopback_options();
    options.limits.max_body_bytes = kSmallBodyLimitBytes;
    const auto fixture = started_test_server(make_test_router(), std::move(options));

    TestClient client;
    REQUIRE(client.connect(fixture->port()));
    const std::optional<HttpResponse> response = client.request(simple_request(
        "POST", "/echo", std::format("Content-Length: {}\r\n", kOversizedDeclaredLength)));
    REQUIRE(response.has_value());
    CHECK(response->status_code == kContentTooLargeStatus);
    CHECK(response->header_value("Connection") == "close");
    CHECK_MESSAGE(client.wait_for_close(),
                  "a refused request leaves the connection in an unknown framing state and must "
                  "end it");
    CHECK(serves_a_plain_request(fixture->port()));
}

TEST_CASE("answers_a_header_block_larger_than_the_configured_limit_with_431_and_closes") {
    ServerOptions options = loopback_options();
    options.limits.max_header_block_bytes = kSmallHeaderBlockLimitBytes;
    const auto fixture = started_test_server(make_test_router(), std::move(options));

    std::string padding;
    for (size_t index = 0; index < kHeaderPaddingFieldCount; ++index)
        padding += std::format("X-Padding-{}: {}\r\n", index,
                               std::string(kHeaderPaddingValueBytes, 'p'));

    TestClient client;
    REQUIRE(client.connect(fixture->port()));
    const std::optional<HttpResponse> response =
        client.request(simple_request("GET", "/hello", padding));
    REQUIRE(response.has_value());
    CHECK_MESSAGE(response->status_code == kHeaderFieldsTooLargeStatus,
                  std::format("an over-budget header block answers 431, not {}",
                              response->status_code));
    CHECK_MESSAGE(client.wait_for_close(),
                  "a refused header block must end the connection, not invite a retry on it");
    CHECK(serves_a_plain_request(fixture->port()));
}

TEST_CASE("names_the_limit_the_peer_actually_exceeded_when_a_body_arrives_with_its_headers") {
    ServerOptions options = loopback_options();
    options.limits.max_request_line_bytes = kTightRequestLineLimitBytes;
    options.limits.max_header_block_bytes = kTightHeaderBlockLimitBytes;
    options.limits.max_target_bytes = kTightRequestLineLimitBytes;
    const size_t body_limit = options.limits.max_body_bytes;
    const auto fixture = started_test_server(make_test_router(), std::move(options));

    const std::string body(kBodyOverTheHeaderBudgetBytes, 'b');
    TestClient client;
    REQUIRE(client.connect(fixture->port()));
    const std::optional<HttpResponse> response =
        client.request(simple_request("POST", "/echo", {}, body));
    REQUIRE(response.has_value());

    CHECK_MESSAGE(response->status_code != kHeaderFieldsTooLargeStatus,
                  std::format("a {}-byte body under a {}-byte body limit was answered 431, naming "
                              "a header limit the request never came near",
                              body.size(), body_limit));
    CHECK(response->status_code == kOkStatus);
    CHECK(response->body == body);
}

TEST_CASE("answers_a_garbage_request_line_with_400") {
    const auto fixture = started_test_server(make_test_router());

    TestClient client;
    REQUIRE(client.connect(fixture->port()));
    const std::optional<HttpResponse> response = client.request("@@@ NOT A REQUEST @@@\r\n\r\n");
    REQUIRE(response.has_value());
    CHECK(response->status_code == kBadRequestStatus);
    CHECK(response->header_value("Connection") == "close");
    CHECK(serves_a_plain_request(fixture->port()));
}

TEST_CASE("answers_binary_garbage_with_400_and_keeps_serving") {
    const auto fixture = started_test_server(make_test_router());

    TestClient client;
    REQUIRE(client.connect(fixture->port()));
    const std::optional<HttpResponse> response =
        client.request(binary_garbage(kBinaryGarbageBytes) + std::string(test::kHeaderTerminator));
    REQUIRE_MESSAGE(response.has_value(),
                    "binary garbage must be answered, not silently swallowed");
    CHECK(response->status_code == kBadRequestStatus);
    CHECK_MESSAGE(!logged_anything_at(fixture->log(), LogLevel::Error),
                  std::format("a malformed request is a client error, not a server one:\n{}",
                              joined_log(fixture->log())));
    CHECK(serves_a_plain_request(fixture->port()));
}


TEST_CASE("survives_a_client_that_disconnects_in_the_middle_of_sending_a_request") {
    const auto fixture = started_test_server(make_test_router());

    {
        TestClient client;
        REQUIRE(client.connect(fixture->port()));
        REQUIRE(client.send_raw("GET /hel"));
    }

    CHECK_MESSAGE(eventually_serves_a_plain_request(fixture->port()),
                  "a truncated request must cost its own connection and nothing more");
}

TEST_CASE("survives_a_client_that_disconnects_in_the_middle_of_receiving_a_response") {
    const auto fixture = started_test_server(make_test_router());

    {
        TestClient client;
        REQUIRE(client.connect(fixture->port()));
        REQUIRE(client.send_raw(simple_request("GET", "/large")));
    }

    CHECK_MESSAGE(eventually_serves_a_plain_request(fixture->port()),
                  "a peer that walks away mid-response must not disturb the reactor serving the "
                  "rest of its connections");
}

TEST_CASE("holds_its_descriptor_count_steady_across_hundreds_of_abandoned_connections") {
    MetricsRegistry registry{std::string(kMetricsPrefix)};
    TestServer fixture(make_test_router());
    fixture.server().install_metrics(registry);
    fixture.start();
    const uint16_t port = fixture.port();

    const Counter& accepted =
        registry.counter(std::string(kAcceptedMetricName), std::string(kMetricsHelpText));
    const Gauge& active =
        registry.gauge(std::string(kActiveMetricName), std::string(kMetricsHelpText));

    for (size_t index = 0; index < kWarmupRequestCount; ++index)
        REQUIRE(serves_a_plain_request(port));

    const size_t baseline = open_descriptor_count();
    REQUIRE_MESSAGE(baseline > 0, "/proc/self/fd is unreadable, so nothing here can be measured");
    const uint64_t expected_accepted = accepted.value() + kAbandonedConnectionCount;

    for (size_t index = 0; index < kAbandonedConnectionCount; ++index) {
        TestClient client;
        REQUIRE(client.connect(port));
        switch (index % kAbandonedShapeCount) {
        case 0:
            break;
        case 1:
            static_cast<void>(client.send_raw("GET /hello HTTP/1.1\r\nHost: 127.0.0.1"));
            break;
        case 2:
            static_cast<void>(client.send_raw(simple_request("GET", "/large")));
            break;
        default:
            static_cast<void>(client.send_raw(simple_request("GET", "/large")));
            static_cast<void>(client.read_some(kSipTimeout));
            break;
        }
    }

    static_cast<void>(wait_until(
        [&] {
            return accepted.value() >= expected_accepted &&
                   active.value() < kSettledConnectionGauge &&
                   open_descriptor_count() <= baseline + kDescriptorDriftTolerance;
        },
        kSettleBudget));

    CHECK_MESSAGE(accepted.value() >= expected_accepted,
                  std::format("only {} of the {} abandoned connections were ever accepted, so the "
                              "descriptor comparison proves nothing",
                              accepted.value(), expected_accepted));
    CHECK_MESSAGE(active.value() < kSettledConnectionGauge,
                  std::format("{} connections are still in a reactor's table after every peer "
                              "walked away",
                              active.value()));

    const size_t settled = open_descriptor_count();
    CHECK_MESSAGE(settled <= baseline + kDescriptorDriftTolerance,
                  std::format("{} connections were abandoned and the process went from {} open "
                              "descriptors to {} -- the connection table is not letting go",
                              kAbandonedConnectionCount, baseline, settled));
    CHECK(serves_a_plain_request(port));
}


TEST_CASE("serves_every_one_of_several_hundred_simultaneous_connections") {
    const size_t connection_count =
        std::min(kDesiredConcurrentConnections, affordable_connection_count());
    REQUIRE_MESSAGE(connection_count >= kMinimumConcurrentConnections,
                    std::format("the descriptor limit only affords {} connections, too few for "
                                "this case to mean anything",
                                connection_count));

    ServerOptions options = loopback_options();
    options.max_connections =
        static_cast<unsigned>(kDesiredConcurrentConnections * kConnectionCapHeadroomFactor);
    options.max_connections_per_source = options.max_connections;
    options.listen_backlog =
        static_cast<int>(kDesiredConcurrentConnections * kConnectionCapHeadroomFactor);
    options.header_timeout = kRelaxedPhaseTimeout;
    options.keep_alive_idle = kRelaxedPhaseTimeout;
    const auto fixture = started_test_server(make_test_router(), std::move(options));

    std::vector<TestClient> clients(connection_count);
    for (size_t index = 0; index < connection_count; ++index)
        REQUIRE_MESSAGE(clients[index].connect(fixture->port()),
                        std::format("connection {} was refused", index));

    for (TestClient& client : clients)
        REQUIRE(client.send_raw(simple_request("GET", "/hello")));

    const auto phase_deadline = std::chrono::steady_clock::now() + kBulkReadBudget;
    size_t served = 0;
    for (TestClient& client : clients) {
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            phase_deadline - std::chrono::steady_clock::now());
        const std::optional<HttpResponse> response =
            client.read_response(std::max(left, test::kMinimumSocketTimeout));
        if (response.has_value() && response->complete && response->status_code == kOkStatus &&
            response->body == kHelloBody)
            ++served;
    }

    CHECK_MESSAGE(served == connection_count,
                  std::format("{} of {} simultaneous connections went unserved",
                              connection_count - served, connection_count));
}

TEST_CASE("the process-wide connection cap holds across several reactors and returns its slots") {
    ServerOptions options = loopback_options();
    options.worker_threads = kSeveralWorkers;
    options.max_connections = kTinyConnectionCap;
    options.max_connections_per_source = kTinyConnectionCap;
    options.header_timeout = kRelaxedPhaseTimeout;
    const auto fixture = started_test_server(make_test_router(), std::move(options));

    std::vector<TestClient> clients(kOverCapConnectionCount);
    for (size_t index = 0; index < kOverCapConnectionCount; ++index)
        REQUIRE(clients[index].connect(fixture->port()));

    const auto phase_deadline = std::chrono::steady_clock::now() + kOverCapReadBudget;
    size_t served = 0;
    size_t refused = 0;
    for (TestClient& client : clients) {
        static_cast<void>(client.send_raw(simple_request("GET", "/hello")));
        const auto left = std::chrono::duration_cast<std::chrono::milliseconds>(
            phase_deadline - std::chrono::steady_clock::now());
        const std::optional<HttpResponse> response =
            client.read_response(std::max(left, test::kMinimumSocketTimeout));
        if (response.has_value() && response->complete && response->status_code == kOkStatus)
            ++served;
        else
            ++refused;
    }

    CHECK_MESSAGE(served >= 1, "the connections inside the cap still have to be served");
    CHECK_MESSAGE(served <= kTinyConnectionCap,
                  std::format("{} connections were served against a cap of {}", served,
                              kTinyConnectionCap));
    CHECK(served + refused == kOverCapConnectionCount);

    clients.clear();
    CHECK_MESSAGE(eventually_serves_a_plain_request(fixture->port()),
                  "slots have to come back once the peers holding them are gone");
}

TEST_CASE("a_sustained_connection_flood_cannot_starve_an_established_connection") {
    ServerOptions options = loopback_options();
    options.worker_threads = kSingleWorker;
    const auto fixture = started_test_server(make_test_router(), std::move(options));

    TestClient established;
    REQUIRE(established.connect(fixture->port()));
    const std::optional<HttpResponse> before =
        established.request(simple_request("GET", "/hello"));
    REQUIRE(before.has_value());
    REQUIRE(before->status_code == kOkStatus);

    std::vector<std::jthread> flooders;
    flooders.reserve(kConnectionFloodThreadCount);
    for (size_t index = 0; index < kConnectionFloodThreadCount; ++index)
        flooders.emplace_back(
            [port = fixture->port()](const std::stop_token& stop) { flood_listener(stop, port); });
    std::this_thread::sleep_for(kConnectionFloodWarmup);

    const std::optional<HttpResponse> during =
        established.request(simple_request("GET", "/hello"));
    for (std::jthread& flooder : flooders)
        flooder.request_stop();

    REQUIRE_MESSAGE(during.has_value(), "the accept queue starved an established connection");
    CHECK(during->status_code == kOkStatus);
    CHECK(during->body == kHelloBody);
}


TEST_CASE("a_peer_that_half_closes_while_a_response_is_stalled_costs_the_reactor_no_cpu") {
    const auto measure = [](bool half_close) {
        ServerOptions options = loopback_options();
        options.write_timeout = kUnhurriedWriteTimeout;
        const auto fixture = started_test_server(make_test_router(), std::move(options));

        StalledPeer peer;
        REQUIRE(peer.connect(fixture->port()));
        REQUIRE(peer.send(simple_request("GET", kStallingRoute)));
        if (half_close)
            REQUIRE(peer.half_close());

        std::this_thread::sleep_for(kSettleBeforeSample);
        return core_fraction_over(kCpuSampleWindow);
    };

    const double write_side_open = measure(false);
    const double write_side_shut = measure(true);

    REQUIRE_MESSAGE(write_side_open < kBusyLoopCoreFraction,
                    std::format("the control burned {:.3f} of a core, so the measurement itself is "
                                "unsound and the comparison below would prove nothing",
                                write_side_open));
    CHECK_MESSAGE(write_side_shut < kBusyLoopCoreFraction,
                  std::format("a peer that stopped reading and shut its write side left the "
                              "reactor burning {:.3f} of a core, against {:.3f} for the same "
                              "stalled response with the peer's write side still open",
                              write_side_shut, write_side_open));
}

TEST_CASE("a_peer_that_half_closes_after_a_complete_request_still_receives_its_response") {
    const auto fixture = started_test_server(make_test_router());

    TestClient client;
    REQUIRE(client.connect(fixture->port()));
    REQUIRE(client.send_raw(simple_request("GET", "/hello")));
    REQUIRE(client.half_close());

    const std::optional<HttpResponse> response = client.read_response();
    REQUIRE_MESSAGE(response.has_value(),
                    "a peer that half-closed after a complete request got no answer at all");
    CHECK(response->status_code == kOkStatus);
    CHECK(response->body == kHelloBody);
}

TEST_CASE("a_stream_whose_peer_vanished_costs_the_reactor_no_cpu") {
    auto source = std::make_shared<SilentStream>();
    Router router = make_test_router();
    router.get(std::string(kStreamRoute),
               [source](const Request&) { return Response::stream(source); });
    const auto fixture = started_test_server(std::move(router));

    {
        TestClient client;
        REQUIRE(client.connect(fixture->port()));
        REQUIRE(client.send_raw(simple_request("GET", kStreamRoute)));
        const std::optional<HttpResponse> head = client.read_head();
        REQUIRE_MESSAGE(head.has_value(),
                        "the stream never opened, so there is nothing to abandon");
        REQUIRE(head->status_code == kOkStatus);
    }

    std::this_thread::sleep_for(kSettleBeforeSample);
    const double burned = core_fraction_over(kCpuSampleWindow);
    REQUIRE_MESSAGE(!source->detached(),
                    "the connection was reclaimed inside the window, so the number below is not a "
                    "measurement of what holding an abandoned stream costs");
    CHECK_MESSAGE(burned < kBusyLoopCoreFraction,
                  std::format("an abandoned stream left the reactor burning {:.3f} of a core, and "
                              "with no heartbeat to fail there is nothing to end it",
                              burned));
}

TEST_CASE("an_abandoned_event_stream_is_reclaimed_when_no_heartbeat_can_reclaim_it") {
    SseChannel channel(SseOptions{.heartbeat_interval = std::chrono::seconds::zero(),
                                  .retry_hint = std::chrono::seconds::zero()});
    Router router = make_test_router();
    router.get(std::string(kStreamRoute), channel.handler());

    ServerOptions options = loopback_options();
    options.stream_idle_timeout = kBriefStreamIdleTimeout;
    const auto fixture = started_test_server(std::move(router), std::move(options));

    {
        TestClient client;
        REQUIRE(client.connect(fixture->port()));
        REQUIRE(client.send_raw(simple_request("GET", kStreamRoute)));
        REQUIRE(client.read_head().has_value());
        REQUIRE(channel.subscriber_count() == 1);
    }

    const bool reclaimed = wait_until([&channel] { return channel.subscriber_count() == 0; },
                                      kBriefStreamIdleTimeout * kStreamReclaimIdleBudgets);
    CHECK_MESSAGE(reclaimed,
                  "an abandoned subscriber holds a descriptor, a connection slot and one of the "
                  "channel's subscriber slots, and only a deadline can take them back");
}

TEST_CASE("a_stream_that_keeps_writing_outlives_the_budget_that_reclaims_an_abandoned_one") {
    SseChannel channel(SseOptions{.heartbeat_interval = std::chrono::seconds::zero(),
                                  .retry_hint = std::chrono::seconds::zero()});
    Router router = make_test_router();
    router.get(std::string(kStreamRoute), channel.handler());

    ServerOptions options = loopback_options();
    options.stream_idle_timeout = kBriefStreamIdleTimeout;
    const auto fixture = started_test_server(std::move(router), std::move(options));

    TestClient client;
    REQUIRE(client.connect(fixture->port()));
    REQUIRE(client.send_raw(simple_request("GET", kStreamRoute)));
    REQUIRE(client.read_head().has_value());
    REQUIRE(channel.subscriber_count() == 1);

    const auto until =
        std::chrono::steady_clock::now() + kBriefStreamIdleTimeout * kStreamSurvivalIdleBudgets;
    while (std::chrono::steady_clock::now() < until) {
        channel.publish(kTickEvent, kTickPayload);
        std::this_thread::sleep_for(kStreamPublishInterval);
        static_cast<void>(client.read_some(test::kMinimumSocketTimeout));
    }

    CHECK_MESSAGE(channel.subscriber_count() == 1,
                  "a subscriber that received an event every tenth of a second was disconnected "
                  "anyway, so its writes are not refreshing the budget that reclaims it");
}

TEST_CASE("notifications_cannot_extend_a_stalled_stream_without_write_progress") {
    auto source = std::make_shared<NotifyingFloodStream>();
    Router router = make_test_router();
    router.get(std::string(kStreamRoute),
               [source](const Request&) { return Response::stream(source); });

    ServerOptions options = loopback_options();
    options.write_timeout = kBriefWriteTimeout;
    const auto fixture = started_test_server(std::move(router), std::move(options));

    StalledPeer peer;
    REQUIRE(peer.connect(fixture->port()));
    REQUIRE(peer.send(simple_request("GET", kStreamRoute)));
    REQUIRE_MESSAGE(wait_until([&source] { return source->attached(); }),
                    "the stream never attached, so no stalled write can be exercised");

    std::jthread producer([source](const std::stop_token& stop) {
        while (!stop.stop_requested()) {
            source->notify();
            std::this_thread::sleep_for(kStreamNotificationInterval);
        }
    });

    const bool reclaimed = wait_until(
        [&source] { return source->detached(); },
        kBriefWriteTimeout * kNotifiedStreamReclaimWriteBudgets);
    producer.request_stop();

    CHECK_MESSAGE(reclaimed,
                  "notifications kept renewing a stalled peer's write deadline without any bytes "
                  "leaving the socket");
}

TEST_CASE("a_subscriber_that_vanishes_before_its_stream_attaches_gives_every_resource_back") {
    MetricsRegistry registry{std::string(kMetricsPrefix)};
    SseChannel channel(SseOptions{.max_subscribers = kSubscriberCeiling,
                                  .heartbeat_interval = std::chrono::seconds::zero(),
                                  .retry_hint = std::chrono::seconds::zero()});

    Router router = make_test_router();
    router.get(std::string(kStreamRoute), channel.handler());
    router.get(std::string(kStalledStreamRoute),
               [subscribe = channel.handler()](const Request& request) {
                   return padded_head(subscribe(request));
               });

    ServerOptions options = loopback_options();
    options.write_timeout = kBriefWriteTimeout;
    TestServer fixture(std::move(router), std::move(options));
    fixture.server().install_metrics(registry);
    fixture.start();
    const uint16_t port = fixture.port();

    const Gauge& active =
        registry.gauge(std::string(kActiveMetricName), std::string(kMetricsHelpText));

    REQUIRE(serves_a_plain_request(port));
    const size_t baseline = open_descriptor_count();
    REQUIRE_MESSAGE(baseline > 0, "/proc/self/fd is unreadable, so nothing here can be measured");

    for (size_t round = 0; round < kSubscriberCeiling; ++round) {
        const size_t before_round = channel.subscriber_count();
        StalledPeer peer;
        REQUIRE(peer.connect(port));
        REQUIRE(peer.send(simple_request("GET", kStalledStreamRoute)));
        REQUIRE_MESSAGE(
            wait_until([&] { return channel.subscriber_count() > before_round; }),
            "the channel never accepted the subscription, so nothing is being abandoned");
        peer.disconnect();
        REQUIRE_MESSAGE(
            wait_until([&active] { return active.value() < kSettledConnectionGauge; },
                       kSettleBudget),
            "the connection itself was never reclaimed, so this case cannot say anything about "
            "what it was holding when it went");
    }

    const bool released = wait_until([&channel] { return channel.subscriber_count() == 0; },
                                     kSettleBudget);
    CHECK_MESSAGE(released,
                  std::format("{} subscriptions were abandoned before their streams attached and "
                              "the channel still counts {} of them",
                              kSubscriberCeiling, channel.subscriber_count()));

    const size_t settled = open_descriptor_count();
    CHECK_MESSAGE(settled <= baseline + kDescriptorDriftTolerance,
                  std::format("the process went from {} open descriptors to {}", baseline,
                              settled));
    CHECK_MESSAGE(active.value() < kSettledConnectionGauge,
                  std::format("{} connections are still counted after every peer walked away",
                              active.value()));

    TestClient subscriber;
    REQUIRE(subscriber.connect(port));
    REQUIRE(subscriber.send_raw(simple_request("GET", kStreamRoute)));
    const std::optional<HttpResponse> head = subscriber.read_head();
    REQUIRE(head.has_value());
    CHECK_MESSAGE(head->status_code == kOkStatus,
                  std::format("a real subscriber was answered {} because {} slots are held by "
                              "peers that never even attached",
                              head->status_code, kSubscriberCeiling));
    CHECK(head->header_value("Content-Type") == kEventStreamContentType);
    CHECK(serves_a_plain_request(port));
}

TEST_CASE("a_source_handed_over_and_never_attached_is_still_told_the_connection_is_gone") {
    auto source = std::make_shared<SilentStream>();
    Router router = make_test_router();
    router.get(std::string(kStalledStreamRoute),
               [source](const Request&) { return padded_head(Response::stream(source)); });

    ServerOptions options = loopback_options();
    options.write_timeout = kBriefWriteTimeout;
    const auto fixture = started_test_server(std::move(router), std::move(options));

    {
        StalledPeer peer;
        REQUIRE(peer.connect(fixture->port()));
        REQUIRE(peer.send(simple_request("GET", kStalledStreamRoute)));
        std::this_thread::sleep_for(kSettleBeforeSample);
        REQUIRE_MESSAGE(!source->attached(),
                        "the head fitted after all, so what follows measures the ordinary attach "
                        "path rather than the window before it");
    }

    CHECK_MESSAGE(wait_until([&source] { return source->detached(); }, kSettleBudget),
                  "a source the connection accepted and never attached was dropped without being "
                  "told, so whatever produced it counts the subscription for ever");
    CHECK_MESSAGE(!source->attached(),
                  "a connection that never wrote its head must not have attached the source");
}

TEST_CASE("repeated_head_requests_to_a_streaming_route_leave_no_source_attached_or_unreleased") {
    std::atomic<int> outstanding{0};
    std::atomic<int> attached{0};

    Router router = make_test_router();
    router.get(std::string(kStreamRoute), [&outstanding, &attached](const Request&) {
        return Response::stream(std::make_shared<CountedStream>(outstanding, attached));
    });
    const auto fixture = started_test_server(std::move(router));

    for (size_t index = 0; index < kHeadRequestCount; ++index) {
        TestClient client;
        REQUIRE(client.connect(fixture->port()));
        const std::optional<HttpResponse> answer =
            client.request(simple_request("HEAD", kStreamRoute), test::kDefaultResponseTimeout,
                           BodyExpectation::HeadRequest);
        REQUIRE(answer.has_value());
        CHECK(answer->status_code == kOkStatus);
        CHECK_MESSAGE(answer->body.empty(), "a HEAD carries no body, streaming route or not");
    }

    CHECK_MESSAGE(attached.load() == 0,
                  std::format("{} of {} HEAD requests were enrolled in a stream that can never "
                              "send them anything",
                              attached.load(), kHeadRequestCount));
    CHECK_MESSAGE(wait_until([&outstanding] { return outstanding.load() == 0; }, kSettleBudget),
                  std::format("{} sources are still held by connections that answered a HEAD",
                              outstanding.load()));
    CHECK(serves_a_plain_request(fixture->port()));
}

} // namespace erikslund::http
