#pragma once

#include <algorithm>
#include <array>
#include <cerrno>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>

#include "erikslund/http/build_config.hpp"
#include "support/http_response.hpp"
#include "support/tls_test_client.hpp"

namespace erikslund::http::test {

inline constexpr const char* kLoopbackIpv4 = "127.0.0.1";

inline constexpr std::chrono::milliseconds kDefaultConnectTimeout{2'000};
inline constexpr std::chrono::milliseconds kDefaultResponseTimeout{5'000};

inline constexpr std::chrono::milliseconds kMinimumSocketTimeout{1};

inline constexpr size_t kReceiveChunkBytes = 4'096;
inline constexpr long kMillisecondsPerSecond = 1'000;
inline constexpr long kMicrosecondsPerMillisecond = 1'000;

inline constexpr std::chrono::milliseconds kDefaultPollInterval{5};

template <class Predicate>
[[nodiscard]] bool wait_until(Predicate predicate,
                              std::chrono::milliseconds timeout = kDefaultResponseTimeout,
                              std::chrono::milliseconds poll_interval = kDefaultPollInterval) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        if (predicate())
            return true;
        if (std::chrono::steady_clock::now() >= deadline)
            return false;
        std::this_thread::sleep_for(poll_interval);
    }
}

class TestClient {
public:
    TestClient() = default;

    ~TestClient() { close(); }

    TestClient(const TestClient&) = delete("a client owns its socket and its SSL object");
    TestClient& operator=(const TestClient&) = delete("a client owns its socket");

    TestClient(TestClient&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)),
          inbox_(std::move(other.inbox_)),
          eof_(std::exchange(other.eof_, false))
#if ERIKSLUND_HTTP_TLS
          ,
          tls_(std::move(other.tls_))
#endif
    {
        other.inbox_.clear();
    }

    TestClient& operator=(TestClient&& other) noexcept {
        if (this == &other)
            return *this;
        close();
        fd_ = std::exchange(other.fd_, -1);
        inbox_ = std::move(other.inbox_);
        other.inbox_.clear();
        eof_ = std::exchange(other.eof_, false);
#if ERIKSLUND_HTTP_TLS
        tls_ = std::move(other.tls_);
#endif
        return *this;
    }

    [[nodiscard]] bool connect(uint16_t port,
                               std::chrono::milliseconds timeout = kDefaultConnectTimeout) {
        close();
        fd_ = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd_ < 0)
            return false;

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(port);
        if (::inet_pton(AF_INET, kLoopbackIpv4, &address.sin_addr) != 1) {
            close();
            return false;
        }
        const auto address_length = static_cast<socklen_t>(sizeof(address));
        if (!finish_connect(reinterpret_cast<const sockaddr*>(&address), address_length, timeout)) {
            close();
            return false;
        }

        const int enable = 1;
        static_cast<void>(::setsockopt(fd_, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable)));
        apply_default_timeouts();
        return true;
    }

    [[nodiscard]] bool connect_unix(std::string_view path,
                                    std::chrono::milliseconds timeout = kDefaultConnectTimeout) {
        close();
        sockaddr_un address{};
        address.sun_family = AF_UNIX;
        if (path.empty() || path.size() >= sizeof(address.sun_path))
            return false;
        std::memcpy(address.sun_path, path.data(), path.size());

        fd_ = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd_ < 0)
            return false;
        const auto length =
            static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
        if (!finish_connect(reinterpret_cast<const sockaddr*>(&address), length, timeout)) {
            close();
            return false;
        }
        apply_default_timeouts();
        return true;
    }

#if ERIKSLUND_HTTP_TLS
    [[nodiscard]] bool connect_tls(uint16_t port, const TlsClientOptions& tls_options = {},
                                   std::chrono::milliseconds timeout = kDefaultConnectTimeout) {
        if (!connect(port, timeout))
            return false;
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        const bool connected = tls_.connect(
            fd_, tls_options, deadline,
            [this](int option, std::chrono::steady_clock::time_point when) {
                return apply_deadline(option, when);
            });
        if (!connected) {
            tls_.close();
            if (fd_ >= 0) {
                ::close(fd_);
                fd_ = -1;
            }
        }
        return connected;
    }

    [[nodiscard]] std::string_view negotiated_alpn() const noexcept {
        return tls_.negotiated_alpn();
    }

    [[nodiscard]] std::string_view tls_error() const noexcept { return tls_.error(); }

    [[nodiscard]] bool is_tls() const noexcept { return tls_.is_active(); }
#endif

    [[nodiscard]] bool is_open() const noexcept { return fd_ >= 0; }

    [[nodiscard]] bool saw_end_of_stream() const noexcept { return eof_; }

    [[nodiscard]] std::string_view buffered() const noexcept { return inbox_; }

    [[nodiscard]] bool send_raw(std::string_view bytes,
                                std::chrono::milliseconds timeout = kDefaultResponseTimeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        size_t sent = 0;
        while (sent < bytes.size()) {
            if (!apply_deadline(SO_SNDTIMEO, deadline))
                return false;
            size_t written = 0;
            if (!raw_write(bytes.data() + sent, bytes.size() - sent, written))
                return false;
            sent += written;
        }
        return true;
    }

    [[nodiscard]] size_t send_slowly(std::string_view bytes,
                                     std::chrono::milliseconds delay_between_bytes,
                                     std::chrono::milliseconds per_byte_timeout =
                                         kDefaultResponseTimeout) {
        size_t sent = 0;
        for (const char byte : bytes) {
            if (!apply_deadline(SO_SNDTIMEO, std::chrono::steady_clock::now() + per_byte_timeout))
                return sent;
            size_t written = 0;
            if (!raw_write(&byte, 1, written) || written != 1)
                return sent;
            ++sent;
            std::this_thread::sleep_for(delay_between_bytes);
        }
        return sent;
    }

    [[nodiscard]] bool half_close() noexcept {
        return fd_ >= 0 && ::shutdown(fd_, SHUT_WR) == 0;
    }

    [[nodiscard]] std::optional<HttpResponse> read_response(
        std::chrono::milliseconds timeout = kDefaultResponseTimeout,
        BodyExpectation expectation = BodyExpectation::FromHeaders) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::optional<HttpResponse> response = read_head_until(deadline);
        if (!response.has_value())
            return std::nullopt;
        read_body_until(deadline, expectation, *response);
        return response;
    }

    [[nodiscard]] std::optional<HttpResponse> read_head(
        std::chrono::milliseconds timeout = kDefaultResponseTimeout) {
        return read_head_until(std::chrono::steady_clock::now() + timeout);
    }

    [[nodiscard]] std::string read_some(
        std::chrono::milliseconds timeout = kDefaultResponseTimeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        if (inbox_.empty())
            static_cast<void>(pump(deadline));
        std::string taken = std::move(inbox_);
        inbox_.clear();
        return taken;
    }

    [[nodiscard]] std::string read_until_closed(
        std::chrono::milliseconds timeout = kDefaultResponseTimeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (pump(deadline) == ReadOutcome::Data)
            continue;
        std::string taken = std::move(inbox_);
        inbox_.clear();
        return taken;
    }

    [[nodiscard]] bool wait_for_close(
        std::chrono::milliseconds timeout = kDefaultResponseTimeout) {
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        while (true) {
            if (eof_)
                return true;
            const ReadOutcome outcome = pump(deadline);
            if (outcome == ReadOutcome::Closed)
                return true;
            if (outcome != ReadOutcome::Data)
                return false;
        }
    }

    [[nodiscard]] std::optional<HttpResponse> request(
        std::string_view raw_request, std::chrono::milliseconds timeout = kDefaultResponseTimeout,
        BodyExpectation expectation = BodyExpectation::FromHeaders) {
        if (!send_raw(raw_request, timeout))
            return std::nullopt;
        return read_response(timeout, expectation);
    }

    void close() noexcept {
#if ERIKSLUND_HTTP_TLS
        tls_.close();
#endif
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
        inbox_.clear();
        eof_ = false;
    }

private:
    enum class ReadOutcome : uint8_t { Data, WouldBlock, Closed, Failed };

    void apply_default_timeouts() noexcept {
        const auto deadline = std::chrono::steady_clock::now() + kDefaultResponseTimeout;
        static_cast<void>(apply_deadline(SO_RCVTIMEO, deadline));
        static_cast<void>(apply_deadline(SO_SNDTIMEO, deadline));
    }

    [[nodiscard]] bool apply_deadline(int option,
                                      std::chrono::steady_clock::time_point deadline) noexcept {
        if (fd_ < 0)
            return false;
        auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining <= std::chrono::milliseconds::zero())
            return false;
        remaining = std::max(remaining, kMinimumSocketTimeout);

        timeval budget{};
        budget.tv_sec = static_cast<time_t>(remaining.count() / kMillisecondsPerSecond);
        budget.tv_usec = static_cast<suseconds_t>((remaining.count() % kMillisecondsPerSecond) *
                                                  kMicrosecondsPerMillisecond);
        return ::setsockopt(fd_, SOL_SOCKET, option, &budget, sizeof(budget)) == 0;
    }

    [[nodiscard]] bool finish_connect(const sockaddr* address, socklen_t length,
                                      std::chrono::milliseconds timeout) noexcept {
        const int flags = ::fcntl(fd_, F_GETFL, 0);
        if (flags < 0)
            return false;
        if (::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) != 0)
            return false;

        if (::connect(fd_, address, length) != 0) {
            if (errno != EINPROGRESS)
                return false;
            pollfd waiter{};
            waiter.fd = fd_;
            waiter.events = POLLOUT;
            int ready = 0;
            while ((ready = ::poll(&waiter, 1, static_cast<int>(timeout.count()))) < 0) {
                if (errno != EINTR)
                    return false;
            }
            if (ready != 1)
                return false;
            int pending_error = 0;
            socklen_t error_length = sizeof(pending_error);
            if (::getsockopt(fd_, SOL_SOCKET, SO_ERROR, &pending_error, &error_length) != 0 ||
                pending_error != 0)
                return false;
        }
        return ::fcntl(fd_, F_SETFL, flags) == 0;
    }

    [[nodiscard]] ReadOutcome raw_read(char* out, size_t capacity, size_t& received) noexcept {
        received = 0;
#if ERIKSLUND_HTTP_TLS
        if (tls_.is_active()) {
            switch (tls_.read(out, capacity, received)) {
            case TlsReadOutcome::Data:
                return ReadOutcome::Data;
            case TlsReadOutcome::WouldBlock:
                return ReadOutcome::WouldBlock;
            case TlsReadOutcome::Closed:
                return ReadOutcome::Closed;
            }
        }
#endif
        while (true) {
            const ssize_t got = ::recv(fd_, out, capacity, 0);
            if (got > 0) {
                received = static_cast<size_t>(got);
                return ReadOutcome::Data;
            }
            if (got == 0)
                return ReadOutcome::Closed;
            if (errno == EINTR)
                continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK)
                return ReadOutcome::WouldBlock;
            if (errno == ECONNRESET)
                return ReadOutcome::Closed;
            return ReadOutcome::Failed;
        }
    }

    [[nodiscard]] bool raw_write(const char* data, size_t length, size_t& written) noexcept {
        written = 0;
        if (length == 0)
            return true;
#if ERIKSLUND_HTTP_TLS
        if (tls_.is_active())
            return tls_.write(data, length, written);
#endif
        while (true) {
            // Expected closed-peer writes must not terminate the runner with SIGPIPE.
            const ssize_t sent = ::send(fd_, data, length, MSG_NOSIGNAL);
            if (sent > 0) {
                written = static_cast<size_t>(sent);
                return true;
            }
            if (sent == 0)
                return false;
            if (errno == EINTR)
                continue;
            return false;
        }
    }

    [[nodiscard]] ReadOutcome pump(std::chrono::steady_clock::time_point deadline) {
        if (eof_)
            return ReadOutcome::Closed;
        if (!apply_deadline(SO_RCVTIMEO, deadline))
            return ReadOutcome::WouldBlock;

        std::array<char, kReceiveChunkBytes> chunk{};
        size_t received = 0;
        const ReadOutcome outcome = raw_read(chunk.data(), chunk.size(), received);
        if (outcome == ReadOutcome::Data)
            inbox_.append(chunk.data(), received);
        else if (outcome == ReadOutcome::Closed)
            eof_ = true;
        return outcome;
    }

    [[nodiscard]] std::optional<HttpResponse> read_head_until(
        std::chrono::steady_clock::time_point deadline) {
        size_t searched = 0;
        size_t terminator = inbox_.find(kHeaderTerminator);
        while (terminator == std::string::npos) {
            searched = inbox_.size() >= kHeaderTerminator.size()
                           ? inbox_.size() - (kHeaderTerminator.size() - 1)
                           : 0;
            if (pump(deadline) != ReadOutcome::Data)
                return std::nullopt;
            terminator = inbox_.find(kHeaderTerminator, searched);
        }

        const size_t head_bytes = terminator + kHeaderTerminator.size();
        HttpResponse response;
        if (!parse_response_head(std::string_view(inbox_).substr(0, head_bytes), response))
            return std::nullopt;
        response.raw_head = inbox_.substr(0, head_bytes);
        inbox_.erase(0, head_bytes);
        return response;
    }

    void read_body_until(std::chrono::steady_clock::time_point deadline,
                         BodyExpectation expectation, HttpResponse& response) {
        if (expectation == BodyExpectation::HeadRequest ||
            response.status_code == kNoContentStatus ||
            response.status_code == kNotModifiedStatus) {
            response.complete = true;
            return;
        }

        const std::optional<std::string_view> declared = response.header("Content-Length");
        if (!declared.has_value()) {
            while (pump(deadline) == ReadOutcome::Data)
                continue;
            response.complete = eof_;
            response.body = std::move(inbox_);
            inbox_.clear();
            return;
        }

        size_t expected = 0;
        const std::from_chars_result parsed =
            std::from_chars(declared->data(), declared->data() + declared->size(), expected);
        if (parsed.ec != std::errc{} || parsed.ptr != declared->data() + declared->size()) {
            response.complete = false;
            response.body = std::move(inbox_);
            inbox_.clear();
            return;
        }

        while (inbox_.size() < expected && pump(deadline) == ReadOutcome::Data)
            continue;
        const size_t available = std::min(expected, inbox_.size());
        response.body = inbox_.substr(0, available);
        inbox_.erase(0, available);
        response.complete = available == expected;
    }

    int fd_ = -1;
    std::string inbox_;
    bool eof_ = false;

#if ERIKSLUND_HTTP_TLS
    TlsTestClient tls_;
#endif
};

[[nodiscard]] inline std::string simple_request(std::string_view method, std::string_view target,
                                                std::string_view extra_headers = {},
                                                std::string_view body = {}) {
    std::string request;
    request += method;
    request += ' ';
    request += target;
    request += " HTTP/1.1\r\nHost: ";
    request += kLoopbackIpv4;
    request += "\r\n";
    request += extra_headers;
    if (!body.empty()) {
        request += "Content-Length: ";
        request += std::to_string(body.size());
        request += "\r\n";
    }
    request += "\r\n";
    request += body;
    return request;
}

} // namespace erikslund::http::test
