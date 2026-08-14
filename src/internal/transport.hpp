#pragma once

#include <sys/uio.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>

#include "erikslund/http/build_config.hpp"
#include "erikslund/http/contracts.hpp"

// OpenSSL's native tag keeps this header usable without OpenSSL includes.
struct ssl_st;

namespace erikslund::http::internal {

class Logger;

// TLS reads may need write readiness and writes may need read readiness. Retry WantRead/WantWrite
// with identical arguments; OpenSSL requires a stable write buffer. EINTR stays internal, and
// partial transfers return Ok with their byte count. Each transport belongs to one reactor thread.

enum class TransportStatus : uint8_t { Ok, WantRead, WantWrite, Closed, Error };

struct TransportResult {
    TransportStatus status = TransportStatus::Error;
    size_t bytes = 0;

    [[nodiscard]] static constexpr TransportResult ok(size_t transferred) noexcept {
        return TransportResult{TransportStatus::Ok, transferred};
    }
    [[nodiscard]] static constexpr TransportResult want_read() noexcept {
        return TransportResult{TransportStatus::WantRead, 0};
    }
    [[nodiscard]] static constexpr TransportResult want_write() noexcept {
        return TransportResult{TransportStatus::WantWrite, 0};
    }
    [[nodiscard]] static constexpr TransportResult closed() noexcept {
        return TransportResult{TransportStatus::Closed, 0};
    }
    [[nodiscard]] static constexpr TransportResult error() noexcept {
        return TransportResult{TransportStatus::Error, 0};
    }

    [[nodiscard]] constexpr bool is_blocked() const noexcept {
        return status == TransportStatus::WantRead || status == TransportStatus::WantWrite;
    }
    [[nodiscard]] constexpr bool is_terminal() const noexcept {
        return status == TransportStatus::Closed || status == TransportStatus::Error;
    }
};

inline constexpr size_t kMaxWriteVectors = 4;

class PlainTransport {
public:
    PlainTransport() = default;
    explicit PlainTransport(int fd) noexcept : fd_(fd) {}

    [[nodiscard]] TransportResult handshake() noexcept;

    [[nodiscard]] TransportResult read(std::span<char> out) noexcept;
    [[nodiscard]] TransportResult write(std::span<const char> in) noexcept;
    [[nodiscard]] TransportResult writev(std::span<const iovec> vectors) noexcept;

    [[nodiscard]] TransportResult shutdown() noexcept;

    [[nodiscard]] bool is_secure() const noexcept { return false; }
    [[nodiscard]] std::string_view alpn_selected() const noexcept { return {}; }
    [[nodiscard]] std::optional<std::string> peer_certificate_subject() const { return std::nullopt; }

    [[nodiscard]] int fd() const noexcept { return fd_; }

private:
    // Owned by the enclosing Connection.
    int fd_ = -1;
};

// Declared in no-TLS builds so connection.cpp needs no conditional code.
class TlsTransport {
public:
    TlsTransport() = default;

    // Takes ownership of ssl; the Connection owns fd.
    TlsTransport(int fd, ssl_st* ssl, Logger& logger) noexcept
        : fd_(fd), ssl_(ssl), logger_(&logger) {}

    ~TlsTransport();
    TlsTransport(const TlsTransport&) = delete("an SSL object has one owner");
    TlsTransport& operator=(const TlsTransport&) = delete("an SSL object has one owner");
    TlsTransport(TlsTransport&& other) noexcept;
    TlsTransport& operator=(TlsTransport&& other) noexcept;

    [[nodiscard]] TransportResult handshake() noexcept;

    [[nodiscard]] TransportResult read(std::span<char> out) noexcept;
    [[nodiscard]] TransportResult write(std::span<const char> in) noexcept;

    // May stop between vectors; bytes identifies the retry offset.
    [[nodiscard]] TransportResult writev(std::span<const iovec> vectors) noexcept;

    [[nodiscard]] TransportResult shutdown() noexcept;

    [[nodiscard]] bool is_secure() const noexcept { return true; }

    [[nodiscard]] std::string_view alpn_selected() const noexcept;

    // Verified, printable RFC 2253 subject. Compare complete attributes; never authorize by
    // substring.
    [[nodiscard]] std::optional<std::string> peer_certificate_subject() const;

    [[nodiscard]] int fd() const noexcept { return fd_; }
    [[nodiscard]] bool has_ssl() const noexcept { return ssl_ != nullptr; }

private:
    int fd_ = -1;
    ssl_st* ssl_ = nullptr;
    Logger* logger_ = nullptr;
    // Owns the storage viewed by alpn_selected().
    std::string alpn_;
};

// Value storage avoids per-connection allocation and collapses to PlainTransport without TLS.
#if ERIKSLUND_HTTP_TLS
using Transport = std::variant<PlainTransport, TlsTransport>;
#else
using Transport = std::variant<PlainTransport>;
#endif

[[nodiscard]] TransportResult transport_handshake(Transport& transport) noexcept;
[[nodiscard]] TransportResult transport_read(Transport& transport, std::span<char> out) noexcept;
[[nodiscard]] TransportResult transport_write(Transport& transport,
                                              std::span<const char> in) noexcept;
[[nodiscard]] TransportResult transport_writev(Transport& transport,
                                               std::span<const iovec> vectors) noexcept;
[[nodiscard]] TransportResult transport_shutdown(Transport& transport) noexcept;
[[nodiscard]] bool transport_is_secure(const Transport& transport) noexcept;
[[nodiscard]] std::string_view transport_alpn_selected(const Transport& transport) noexcept;
[[nodiscard]] std::optional<std::string> transport_peer_certificate_subject(
    const Transport& transport);
[[nodiscard]] int transport_fd(const Transport& transport) noexcept;

} // namespace erikslund::http::internal
