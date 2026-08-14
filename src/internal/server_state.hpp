#pragma once

#include <array>
#include <cstdint>
#include <exception>
#include <expected>
#include <map>
#include <mutex>
#include <stop_token>

#include "erikslund/http/peer_address.hpp"
#include "internal/logger.hpp"

namespace erikslund::http::internal {

enum class AdmissionRejection : uint8_t { GlobalLimit, SourceLimit };

class ServerState;

// Releases both the process-wide and per-source counts with the Connection that owns the slot.
class ConnectionAdmission {
public:
    ConnectionAdmission() = default;
    ~ConnectionAdmission();
    ConnectionAdmission(const ConnectionAdmission&) = delete("one connection owns one slot");
    ConnectionAdmission& operator=(const ConnectionAdmission&) = delete("slots are not shared");
    ConnectionAdmission(ConnectionAdmission&& other) noexcept;
    ConnectionAdmission& operator=(ConnectionAdmission&& other) noexcept;

private:
    friend class ServerState;

    ConnectionAdmission(ServerState& state, std::array<uint8_t, kIpv6ByteCount> source,
                        bool source_counted) noexcept;
    void release() noexcept;

    ServerState* state_ = nullptr;
    std::array<uint8_t, kIpv6ByteCount> source_{};
    bool source_counted_ = false;
};

class ServerState {
public:
    ServerState(unsigned max_connections, unsigned max_connections_per_source, LogSink log)
        : max_connections_(max_connections),
          max_connections_per_source_(max_connections_per_source), logger_(std::move(log)) {}

    [[nodiscard]] std::expected<ConnectionAdmission, AdmissionRejection> admit(
        const PeerAddress& peer);

    void fail(std::exception_ptr failure) noexcept;
    [[nodiscard]] std::exception_ptr failure() const noexcept;

    void request_stop() noexcept { stop_source_.request_stop(); }
    [[nodiscard]] std::stop_token stop_token() const noexcept { return stop_source_.get_token(); }

    [[nodiscard]] Logger& logger() noexcept { return logger_; }

private:
    friend class ConnectionAdmission;

    void release(const std::array<uint8_t, kIpv6ByteCount>& source,
                 bool source_counted) noexcept;

    const unsigned max_connections_;
    const unsigned max_connections_per_source_;

    std::mutex admission_mutex_;
    unsigned active_connections_ = 0;
    std::map<std::array<uint8_t, kIpv6ByteCount>, unsigned> source_connections_;

    std::stop_source stop_source_;

    mutable std::mutex failure_mutex_;
    std::exception_ptr failure_;

    Logger logger_;
};

} // namespace erikslund::http::internal
