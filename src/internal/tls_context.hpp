#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string_view>

#include "erikslund/http/build_config.hpp"
#include "erikslund/http/tls.hpp"
#include "internal/transport.hpp"

struct ssl_ctx_st;

namespace erikslund::http::internal {

class Logger;

// Length-prefixed ALPN wire format.
inline constexpr std::string_view kAlpnHttp11Wire{"\x08http/1.1", 9};

inline constexpr std::string_view kAlpnHttp11Name = "http/1.1";

// Immutable after creation and safe to share across reactor threads.
class TlsContext {
public:
    ~TlsContext();
    TlsContext(const TlsContext&) = delete("an SSL_CTX has one owner; share the shared_ptr");
    TlsContext& operator=(const TlsContext&) = delete("an SSL_CTX has one owner");

    // Loads, validates, and hardens a context. Failures throw ServerError.
    [[nodiscard]] static std::shared_ptr<TlsContext> create(const TlsOptions& options,
                                                            Logger& logger);

    [[nodiscard]] std::optional<TlsTransport> make_transport(int fd) const;

    [[nodiscard]] std::chrono::system_clock::time_point certificate_not_after() const noexcept {
        return not_after_;
    }

    [[nodiscard]] bool requires_client_certificate() const noexcept {
        return requires_client_certificate_;
    }

private:
    TlsContext() = default;

    ssl_ctx_st* context_ = nullptr;
    Logger* logger_ = nullptr;
    std::chrono::system_clock::time_point not_after_{};
    bool requires_client_certificate_ = false;
};

// Lock-free reload slot; shared ownership keeps old contexts alive for existing connections.
class TlsContextSlot {
public:
    TlsContextSlot() = default;
    explicit TlsContextSlot(std::shared_ptr<TlsContext> initial) : context_(std::move(initial)) {}

    [[nodiscard]] std::shared_ptr<TlsContext> load() const noexcept {
        return context_.load(std::memory_order_acquire);
    }

    void store(std::shared_ptr<TlsContext> context) noexcept {
        context_.store(std::move(context), std::memory_order_release);
    }

    [[nodiscard]] bool empty() const noexcept { return load() == nullptr; }

private:
    std::atomic<std::shared_ptr<TlsContext>> context_;
};

} // namespace erikslund::http::internal
