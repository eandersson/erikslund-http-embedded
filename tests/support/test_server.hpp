#pragma once
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "erikslund/http/router.hpp"
#include "erikslund/http/server.hpp"
#include "support/http_client.hpp"

namespace erikslund::http::test {

class CapturedLog {
public:
    CapturedLog() = default;
    CapturedLog(const CapturedLog&) = delete("a Server's log sink captures this object's address");
    CapturedLog& operator=(const CapturedLog&) =
        delete("a Server's log sink captures this object's address");

    [[nodiscard]] LogSink sink() {
        return [this](LogLevel level, std::string_view message) {
            const std::scoped_lock guard(mutex_);
            lines_.emplace_back(level, std::string(message));
        };
    }

    [[nodiscard]] std::vector<std::string> lines() const {
        const std::scoped_lock guard(mutex_);
        std::vector<std::string> copied;
        copied.reserve(lines_.size());
        for (const auto& [level, message] : lines_)
            copied.push_back(message);
        return copied;
    }

    [[nodiscard]] bool contains(std::string_view needle) const {
        const std::scoped_lock guard(mutex_);
        for (const auto& [level, message] : lines_)
            if (message.find(needle) != std::string::npos)
                return true;
        return false;
    }

    [[nodiscard]] bool contains(LogLevel level, std::string_view needle) const {
        const std::scoped_lock guard(mutex_);
        for (const auto& [line_level, message] : lines_)
            if (line_level == level && message.find(needle) != std::string::npos)
                return true;
        return false;
    }

    [[nodiscard]] size_t size() const {
        const std::scoped_lock guard(mutex_);
        return lines_.size();
    }

    void clear() {
        const std::scoped_lock guard(mutex_);
        lines_.clear();
    }

private:
    mutable std::mutex mutex_;
    std::vector<std::pair<LogLevel, std::string>> lines_;
};

[[nodiscard]] inline ServerOptions loopback_options() {
    ServerOptions options;
    Listener listener;
    listener.bind_address = kLoopbackIpv4;
    listener.port = 0;
    options.listeners.push_back(std::move(listener));
    options.worker_threads = 1;
    return options;
}

class TestServer {
public:
    explicit TestServer(Router router, ServerOptions options = loopback_options())
        : server_(std::move(router), with_captured_log(std::move(options), log_)) {}

    ~TestServer() = default;
    TestServer(const TestServer&) = delete("a TestServer owns listening sockets and threads");
    TestServer& operator=(const TestServer&) = delete("a TestServer owns listening sockets");
    TestServer(TestServer&&) = delete("Server itself is pinned; reactor threads point at it");
    TestServer& operator=(TestServer&&) = delete("Server itself is pinned");

    void start() { server_.start(); }
    [[nodiscard]] uint16_t port() const noexcept { return server_.port(); }
    [[nodiscard]] uint16_t port(size_t listener_index) const {
        return server_.port(listener_index);
    }
    [[nodiscard]] Server& server() noexcept { return server_; }
    [[nodiscard]] const CapturedLog& log() const noexcept { return log_; }
    [[nodiscard]] CapturedLog& log() noexcept { return log_; }

private:
    [[nodiscard]] static ServerOptions with_captured_log(ServerOptions options,
                                                         CapturedLog& target) {
        options.log = target.sink();
        return options;
    }

    // Destroyed in reverse order: Server joins every writer before the captured log disappears.
    CapturedLog log_;
    Server server_;
};

[[nodiscard]] inline std::unique_ptr<TestServer> started_test_server(
    Router router, ServerOptions options = loopback_options()) {
    auto fixture = std::make_unique<TestServer>(std::move(router), std::move(options));
    fixture->start();
    return fixture;
}

} // namespace erikslund::http::test
