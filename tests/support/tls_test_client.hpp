#pragma once
#include "erikslund/http/build_config.hpp"

#if ERIKSLUND_HTTP_TLS

#include <array>
#include <cerrno>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <string>
#include <string_view>
#include <utility>

#include <sys/socket.h>

#include <openssl/err.h>
#include <openssl/ssl.h>

namespace erikslund::http::test {

struct TlsClientOptions {
    std::string alpn_protocol{};
    std::string server_name = "localhost";
    int minimum_version = 0;
    int maximum_version = 0;
    std::string certificate_file{};
    std::string private_key_file{};
};

enum class TlsReadOutcome : uint8_t { Data, WouldBlock, Closed };

class TlsTestClient {
public:
    TlsTestClient() = default;
    ~TlsTestClient() { close(); }
    TlsTestClient(const TlsTestClient&) = delete("an SSL session is unique");
    TlsTestClient& operator=(const TlsTestClient&) = delete("an SSL session is unique");
    TlsTestClient(TlsTestClient&& other) noexcept
        : context_(std::exchange(other.context_, nullptr)),
          ssl_(std::exchange(other.ssl_, nullptr)), alpn_(std::move(other.alpn_)),
          error_(std::move(other.error_)) {}
    TlsTestClient& operator=(TlsTestClient&& other) noexcept {
        if (this != &other) {
            close();
            context_ = std::exchange(other.context_, nullptr);
            ssl_ = std::exchange(other.ssl_, nullptr);
            alpn_ = std::move(other.alpn_);
            error_ = std::move(other.error_);
        }
        return *this;
    }

    template <class ApplyDeadline>
    [[nodiscard]] bool connect(int fd, const TlsClientOptions& options,
                               std::chrono::steady_clock::time_point deadline,
                               ApplyDeadline apply_deadline) {
        close();
        error_.clear();
        alpn_.clear();
        context_ = SSL_CTX_new(TLS_client_method());
        if (context_ == nullptr)
            return fail("SSL_CTX_new");
        SSL_CTX_set_verify(context_, SSL_VERIFY_NONE, nullptr);
        if (options.minimum_version != 0 &&
            SSL_CTX_set_min_proto_version(context_, options.minimum_version) != 1)
            return fail("SSL_CTX_set_min_proto_version");
        if (options.maximum_version != 0 &&
            SSL_CTX_set_max_proto_version(context_, options.maximum_version) != 1)
            return fail("SSL_CTX_set_max_proto_version");
        if (!options.certificate_file.empty()) {
            if (SSL_CTX_use_certificate_chain_file(context_, options.certificate_file.c_str()) != 1)
                return fail("SSL_CTX_use_certificate_chain_file");
            if (SSL_CTX_use_PrivateKey_file(context_, options.private_key_file.c_str(),
                                            SSL_FILETYPE_PEM) != 1)
                return fail("SSL_CTX_use_PrivateKey_file");
        }
        ssl_ = SSL_new(context_);
        if (ssl_ == nullptr)
            return fail("SSL_new");
        if (!options.alpn_protocol.empty()) {
            constexpr size_t kMaxAlpnNameBytes = 255;
            if (options.alpn_protocol.size() > kMaxAlpnNameBytes)
                return fail("the ALPN name is longer than one length byte can describe");
            std::string wire(1, static_cast<char>(options.alpn_protocol.size()));
            wire += options.alpn_protocol;
            if (SSL_set_alpn_protos(ssl_, reinterpret_cast<const unsigned char*>(wire.data()),
                                    static_cast<unsigned int>(wire.size())) != 0)
                return fail("SSL_set_alpn_protos");
        }
        if (!options.server_name.empty())
            static_cast<void>(SSL_set_tlsext_host_name(ssl_, options.server_name.c_str()));
        if (SSL_set_fd(ssl_, fd) != 1)
            return fail("SSL_set_fd");

        while (true) {
            if (!apply_deadline(SO_RCVTIMEO, deadline) ||
                !apply_deadline(SO_SNDTIMEO, deadline)) {
                error_ = "the TLS handshake did not finish before the deadline";
                return false;
            }
            ERR_clear_error();
            const int result = SSL_connect(ssl_);
            if (result == 1) {
                const unsigned char* protocol = nullptr;
                unsigned int protocol_length = 0;
                SSL_get0_alpn_selected(ssl_, &protocol, &protocol_length);
                if (protocol != nullptr && protocol_length > 0)
                    alpn_.assign(reinterpret_cast<const char*>(protocol), protocol_length);
                return true;
            }
            const int reason = SSL_get_error(ssl_, result);
            if (reason == SSL_ERROR_WANT_READ || reason == SSL_ERROR_WANT_WRITE)
                continue;
            return fail("SSL_connect");
        }
    }

    [[nodiscard]] TlsReadOutcome read(char* out, size_t capacity, size_t& received) noexcept {
        received = 0;
        ERR_clear_error();
        if (SSL_read_ex(ssl_, out, capacity, &received) == 1)
            return TlsReadOutcome::Data;
        const int reason = SSL_get_error(ssl_, 0);
        if (reason == SSL_ERROR_WANT_READ || reason == SSL_ERROR_WANT_WRITE ||
            (reason == SSL_ERROR_SYSCALL && (errno == EAGAIN || errno == EWOULDBLOCK)))
            return TlsReadOutcome::WouldBlock;
        capture_error("SSL_read_ex");
        return TlsReadOutcome::Closed;
    }

    [[nodiscard]] bool write(const char* data, size_t length, size_t& written) noexcept {
        written = 0;
        ERR_clear_error();
        if (SSL_write_ex(ssl_, data, length, &written) == 1)
            return true;
        capture_error("SSL_write_ex");
        return false;
    }

    void close() noexcept {
        if (ssl_ != nullptr) {
            static_cast<void>(SSL_shutdown(ssl_));
            SSL_free(ssl_);
            ssl_ = nullptr;
        }
        if (context_ != nullptr) {
            SSL_CTX_free(context_);
            context_ = nullptr;
        }
    }

    [[nodiscard]] bool is_active() const noexcept { return ssl_ != nullptr; }
    [[nodiscard]] std::string_view negotiated_alpn() const noexcept { return alpn_; }
    [[nodiscard]] std::string_view error() const noexcept { return error_; }

private:
    void capture_error(std::string_view operation) noexcept {
        std::string detail;
        for (unsigned long code = ERR_get_error(); code != 0; code = ERR_get_error()) {
            std::array<char, 256> text{};
            ERR_error_string_n(code, text.data(), text.size());
            try {
                if (!detail.empty())
                    detail += "; ";
                detail += text.data();
            } catch (const std::exception&) {
                detail.clear();
            }
        }
        try {
            error_ = detail.empty() ? std::string(operation) : std::string(operation) + ": " + detail;
        } catch (const std::exception&) {
            error_.clear();
        }
    }
    [[nodiscard]] bool fail(std::string_view operation) {
        capture_error(operation);
        return false;
    }

    SSL_CTX* context_ = nullptr;
    SSL* ssl_ = nullptr;
    std::string alpn_;
    std::string error_;
};

} // namespace erikslund::http::test

#endif
