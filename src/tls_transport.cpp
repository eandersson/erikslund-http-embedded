#include "internal/transport.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <sys/uio.h>

#if ERIKSLUND_HTTP_TLS
#include <openssl/bio.h>
#include <openssl/crypto.h>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/sslerr.h>
#include <openssl/x509.h>
#endif

#include "erikslund/http/contracts.hpp"
#include "erikslund/http/server.hpp"
#include "internal/logger.hpp"

namespace erikslund::http::internal {
#if ERIKSLUND_HTTP_TLS

namespace {

inline constexpr size_t kOpenSslErrorTextBytes = 256;

inline constexpr size_t kMaxLoggedOpenSslErrors = 4;

void log_tls_error(Logger* logger, std::string_view message) noexcept {
    if (logger != nullptr)
        logger->write_peer(LogLevel::Error, message);
}

// Drain the thread-local OpenSSL queue so stale errors cannot poison another connection.
void log_openssl_failure(Logger* logger, std::string_view operation,
                         std::string_view context) noexcept {
    try {
        std::string detail;
        size_t appended = 0;
        for (unsigned long code = ERR_get_error(); code != 0; code = ERR_get_error()) {
            if (appended >= kMaxLoggedOpenSslErrors)
                continue;
            std::array<char, kOpenSslErrorTextBytes> text{};
            ERR_error_string_n(code, text.data(), text.size());
            if (!detail.empty())
                detail += "; ";
            detail += text.data();
            ++appended;
        }

        std::string message = std::format("tls: {} failed", operation);
        if (!context.empty())
            message += std::format(" ({})", context);
        if (!detail.empty())
            message += std::format(": {}", detail);
        log_tls_error(logger, message);
        // Queue cleanup below is mandatory even when logging throws.
    } catch (...) {
        log_tls_error(logger, "tls: failure details could not be formatted");
    }
    ERR_clear_error();
}

enum class StalledKind : uint8_t { None, Write, Writev };

// SSL_write_ex retries require the same address and length. Store that state in SSL ex_data so it
// follows the SSL object's lifetime and moves. SSL_read_ex has no equivalent buffer rule.
struct StalledWrite {
    StalledKind kind = StalledKind::None;
    const void* buffer = nullptr;
    size_t length = 0;

    // Per-connection storage keeps a coalesced write's retry address stable.
    std::vector<char> scratch;
};

void reclaim_stalled_write([[maybe_unused]] void* parent, void* pointer,
                           [[maybe_unused]] CRYPTO_EX_DATA* ex_data,
                           [[maybe_unused]] int index, [[maybe_unused]] long argument_length,
                           [[maybe_unused]] void* argument) {
    const std::unique_ptr<StalledWrite> reclaimed{static_cast<StalledWrite*>(pointer)};
}

// Claim the process-wide ex_data slot on first use.
[[nodiscard]] int stalled_write_index() noexcept {
    static const int index =
        CRYPTO_get_ex_new_index(CRYPTO_EX_INDEX_SSL, 0, nullptr, nullptr, nullptr,
                                &reclaim_stalled_write);
    return index;
}

[[nodiscard]] StalledWrite* stalled_write_for(SSL* ssl) noexcept {
    const int index = stalled_write_index();
    if (index < 0)
        return nullptr;
    if (void* const existing = SSL_get_ex_data(ssl, index); existing != nullptr)
        return static_cast<StalledWrite*>(existing);
    try {
        auto created = std::make_unique<StalledWrite>();
        if (SSL_set_ex_data(ssl, index, created.get()) != 1)
            return nullptr;
        return created.release();
    } catch (const std::exception&) {
        return nullptr;
    }
}

[[nodiscard]] bool matches_stalled_write(const StalledWrite& pending, StalledKind kind,
                                         const void* buffer, size_t length) noexcept {
    if (pending.kind == StalledKind::None)
        return true;
    return pending.kind == kind && pending.buffer == buffer && pending.length == length;
}

void record_write_outcome(StalledWrite& pending, const TransportResult& outcome, StalledKind kind,
                          const void* buffer, size_t length) noexcept {
    if (outcome.is_blocked()) {
        pending.kind = kind;
        pending.buffer = buffer;
        pending.length = length;
        return;
    }
    pending.kind = StalledKind::None;
    pending.buffer = nullptr;
    pending.length = 0;
}

// Leaves the OpenSSL error queue empty on every path.
[[nodiscard]] TransportResult classify_ssl_failure(SSL* ssl, int result, int call_errno,
                                                   std::string_view operation,
                                                   Logger* logger) noexcept {
    switch (SSL_get_error(ssl, result)) {
    case SSL_ERROR_WANT_READ:
        return TransportResult::want_read();
    case SSL_ERROR_WANT_WRITE:
        return TransportResult::want_write();
    case SSL_ERROR_ZERO_RETURN:
        ERR_clear_error();
        return TransportResult::closed();
    case SSL_ERROR_SYSCALL:
        if (call_errno == 0) {
            // Browsers commonly close without close_notify when navigating away.
            ERR_clear_error();
            return TransportResult::closed();
        }
        // Retryable BIO conditions arrive as WANT_READ or WANT_WRITE, not SYSCALL.
        log_openssl_failure(logger, operation, std::generic_category().message(call_errno));
        return TransportResult::error();
    case SSL_ERROR_SSL:
#ifdef SSL_R_UNEXPECTED_EOF_WHILE_READING
        // Treat unexpected EOF as disconnect here without weakening handshake validation globally.
        if (ERR_GET_REASON(ERR_peek_error()) == SSL_R_UNEXPECTED_EOF_WHILE_READING) {
            ERR_clear_error();
            return TransportResult::closed();
        }
#endif
        log_openssl_failure(logger, operation, {});
        return TransportResult::error();
    default:
        log_openssl_failure(logger, operation, {});
        return TransportResult::error();
    }
}

[[nodiscard]] TransportResult write_through_ssl(SSL* ssl, StalledWrite* pending, StalledKind kind,
                                                const char* data, size_t length,
                                                Logger* logger) noexcept {
    if (pending != nullptr && !matches_stalled_write(*pending, kind, data, length)) {
        // Fail this connection instead of triggering BAD_WRITE_RETRY or a process-wide contract.
        log_tls_error(logger,
                      "tls: SSL_write_ex retry presented a different buffer than the stalled call");
        return TransportResult::error();
    }

    // SSL_get_error requires a clean thread-local error queue before the call.
    ERR_clear_error();
    size_t written = 0;
    const int result = SSL_write_ex(ssl, data, length, &written);
    const int call_errno = errno;

    const TransportResult outcome =
        result == 1 ? TransportResult::ok(written)
                    : classify_ssl_failure(ssl, result, call_errno, "SSL_write_ex", logger);
    if (pending != nullptr)
        record_write_outcome(*pending, outcome, kind, data, length);
    return outcome;
}

[[nodiscard]] bool coalesce_into_scratch(std::vector<char>& scratch,
                                         std::span<const iovec> vectors, size_t total) noexcept {
    try {
        scratch.clear();
        scratch.reserve(total);
        for (const iovec& vector : vectors) {
            const char* const begin = static_cast<const char*>(vector.iov_base);
            scratch.insert(scratch.end(), begin, begin + vector.iov_len);
        }
    } catch (const std::exception&) {
        return false;
    }
    return true;
}

void assign_negotiated_alpn(SSL* ssl, std::string& out) noexcept {
    const unsigned char* protocol = nullptr;
    unsigned int protocol_length = 0;
    SSL_get0_alpn_selected(ssl, &protocol, &protocol_length);
    try {
        if (protocol == nullptr || protocol_length == 0)
            out.clear();
        else
            out.assign(reinterpret_cast<const char*>(protocol), protocol_length);
    } catch (const std::exception&) {
        out.clear();
    }
}

struct X509Deleter {
    void operator()(X509* certificate) const noexcept { X509_free(certificate); }
};
using X509Ptr = std::unique_ptr<X509, X509Deleter>;

struct BioDeleter {
    void operator()(BIO* bio) const noexcept { BIO_free(bio); }
};
using BioPtr = std::unique_ptr<BIO, BioDeleter>;

// Clears the thread-local error queue on entry and exit, including allocation failure.
struct ErrorQueueDrain {
    ErrorQueueDrain() noexcept { ERR_clear_error(); }
    ~ErrorQueueDrain() noexcept { ERR_clear_error(); }
    ErrorQueueDrain(const ErrorQueueDrain&) = delete("one scope owns the queue");
    ErrorQueueDrain& operator=(const ErrorQueueDrain&) = delete("one scope owns the queue");
};

inline constexpr int kNoIndent = 0;

// RFC 2253 hex form for '='.
inline constexpr std::string_view kEqualsHexEscape = "\\3D";

inline constexpr char kRelativeNameAssignment = '=';
inline constexpr char kRelativeNameSeparator = ',';
inline constexpr char kRelativeNameValueJoiner = '+';
inline constexpr char kRelativeNameEscape = '\\';

// Escape '=' inside values so only real type/value separators retain that spelling.
[[nodiscard]] std::string escape_equals_inside_values(std::string_view printed) {
    std::string escaped;
    escaped.reserve(printed.size());
    bool inside_value = false;
    for (size_t index = 0; index < printed.size(); ++index) {
        const char character = printed[index];
        if (character == kRelativeNameEscape) {
            escaped.push_back(character);
            if (index + 1 < printed.size())
                escaped.push_back(printed[index + 1]);
            ++index;
            continue;
        }
        if (character == kRelativeNameAssignment) {
            if (inside_value) {
                escaped += kEqualsHexEscape;
                continue;
            }
            inside_value = true;
        } else if (character == kRelativeNameSeparator || character == kRelativeNameValueJoiner) {
            inside_value = false;
        }
        escaped.push_back(character);
    }
    return escaped;
}

// RFC 2253 is reversible; ESC_CTRL and ESC_MSB keep the result printable and safe to log.
[[nodiscard]] std::optional<std::string> rfc2253_subject(X509_NAME* subject) {
    const BioPtr sink(BIO_new(BIO_s_mem()));
    if (!sink)
        return std::nullopt;
    if (X509_NAME_print_ex(sink.get(), subject, kNoIndent, XN_FLAG_RFC2253) < 0)
        return std::nullopt;

    char* text = nullptr;
    const long length = BIO_get_mem_data(sink.get(), &text);
    if (length < 0)
        return std::nullopt;
    if (length == 0)
        return std::string();
    if (text == nullptr)
        return std::nullopt;
    return escape_equals_inside_values(std::string_view(text, static_cast<size_t>(length)));
}

} // namespace


TlsTransport::~TlsTransport() {
    SSL_free(ssl_);
}

TlsTransport::TlsTransport(TlsTransport&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)),
      ssl_(std::exchange(other.ssl_, nullptr)),
      logger_(std::exchange(other.logger_, nullptr)),
      alpn_(std::move(other.alpn_)) {}

TlsTransport& TlsTransport::operator=(TlsTransport&& other) noexcept {
    if (this != &other) {
        SSL_free(ssl_);
        fd_ = std::exchange(other.fd_, -1);
        ssl_ = std::exchange(other.ssl_, nullptr);
        logger_ = std::exchange(other.logger_, nullptr);
        alpn_ = std::move(other.alpn_);
    }
    return *this;
}

TransportResult TlsTransport::handshake() noexcept {
    if (ssl_ == nullptr)
        return TransportResult::error();

    ERR_clear_error();
    const int result = SSL_accept(ssl_);
    const int call_errno = errno;
    if (result == 1) {
        assign_negotiated_alpn(ssl_, alpn_);
        return TransportResult::ok(0);
    }
    return classify_ssl_failure(ssl_, result, call_errno, "SSL_accept", logger_);
}

TransportResult TlsTransport::read(std::span<char> out) noexcept {
    if (ssl_ == nullptr)
        return TransportResult::error();
    if (out.empty())
        return TransportResult::ok(0);

    ERR_clear_error();
    size_t received = 0;
    const int result = SSL_read_ex(ssl_, out.data(), out.size(), &received);
    const int call_errno = errno;
    if (result == 1)
        return TransportResult::ok(received);
    return classify_ssl_failure(ssl_, result, call_errno, "SSL_read_ex", logger_);
}

TransportResult TlsTransport::write(std::span<const char> in) noexcept {
    if (ssl_ == nullptr)
        return TransportResult::error();
    if (in.empty())
        return TransportResult::ok(0);

    return write_through_ssl(ssl_, stalled_write_for(ssl_), StalledKind::Write, in.data(),
                             in.size(), logger_);
}

TransportResult TlsTransport::writev(std::span<const iovec> vectors) noexcept {
    if (ssl_ == nullptr)
        return TransportResult::error();
    ERIKSLUND_HTTP_ASSERT(vectors.size() <= kMaxWriteVectors);

    size_t total = 0;
    for (const iovec& vector : vectors)
        total += vector.iov_len;
    if (total == 0)
        return TransportResult::ok(0);

    StalledWrite* const pending = stalled_write_for(ssl_);
    if (pending == nullptr) {
        log_tls_error(logger_, "tls: no memory for this connection's pending-write state");
        return TransportResult::error();
    }

    // Coalesce one TLS record. Do not refill scratch during retries; reserve() may move it.
    const bool is_retry = pending->kind == StalledKind::Writev && pending->length == total;
    if (!is_retry && !coalesce_into_scratch(pending->scratch, vectors, total)) {
        log_tls_error(logger_, "tls: no memory for the record coalescing buffer");
        return TransportResult::error();
    }

    return write_through_ssl(ssl_, pending, StalledKind::Writev, pending->scratch.data(), total,
                             logger_);
}

TransportResult TlsTransport::shutdown() noexcept {
    if (ssl_ == nullptr)
        return TransportResult::ok(0);

    ERR_clear_error();
    const int result = SSL_shutdown(ssl_);
    const int call_errno = errno;

    // Send close_notify once; RFC 8446 does not require waiting for the peer's alert.
    if (result >= 0)
        return TransportResult::ok(0);
    return classify_ssl_failure(ssl_, result, call_errno, "SSL_shutdown", logger_);
}

std::string_view TlsTransport::alpn_selected() const noexcept {
    return alpn_;
}

std::optional<std::string> TlsTransport::peer_certificate_subject() const {
    if (ssl_ == nullptr)
        return std::nullopt;

    const ErrorQueueDrain drain;

    // X509_V_OK under SSL_VERIFY_NONE means no verification occurred.
    if ((SSL_get_verify_mode(ssl_) & SSL_VERIFY_PEER) == 0)
        return std::nullopt;
    if (SSL_get_verify_result(ssl_) != X509_V_OK)
        return std::nullopt;

    const X509Ptr certificate(SSL_get1_peer_certificate(ssl_));
    if (!certificate)
        return std::nullopt;

    X509_NAME* const subject = X509_get_subject_name(certificate.get());
    if (subject == nullptr)
        return std::nullopt;
    return rfc2253_subject(subject);
}

#else // ERIKSLUND_HTTP_TLS


TlsTransport::~TlsTransport() = default;

TlsTransport::TlsTransport(TlsTransport&& other) noexcept
    : fd_(std::exchange(other.fd_, -1)),
      ssl_(std::exchange(other.ssl_, nullptr)),
      logger_(std::exchange(other.logger_, nullptr)),
      alpn_(std::move(other.alpn_)) {}

TlsTransport& TlsTransport::operator=(TlsTransport&& other) noexcept {
    if (this != &other) {
        fd_ = std::exchange(other.fd_, -1);
        ssl_ = std::exchange(other.ssl_, nullptr);
        logger_ = std::exchange(other.logger_, nullptr);
        alpn_ = std::move(other.alpn_);
    }
    return *this;
}

TransportResult TlsTransport::handshake() noexcept {
    return TransportResult::error();
}

TransportResult TlsTransport::read(std::span<char>) noexcept {
    return TransportResult::error();
}

TransportResult TlsTransport::write(std::span<const char>) noexcept {
    return TransportResult::error();
}

TransportResult TlsTransport::writev(std::span<const iovec>) noexcept {
    return TransportResult::error();
}

TransportResult TlsTransport::shutdown() noexcept {
    return TransportResult::ok(0);
}

std::string_view TlsTransport::alpn_selected() const noexcept {
    return alpn_;
}

std::optional<std::string> TlsTransport::peer_certificate_subject() const {
    return std::nullopt;
}

#endif // ERIKSLUND_HTTP_TLS

} // namespace erikslund::http::internal
