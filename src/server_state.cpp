#include "internal/server_state.hpp"

#include <utility>

namespace erikslund::http::internal {

ConnectionAdmission::ConnectionAdmission(ServerState& state,
                                         std::array<uint8_t, kIpv6ByteCount> source,
                                         bool source_counted) noexcept
    : state_(&state), source_(source), source_counted_(source_counted) {}

ConnectionAdmission::~ConnectionAdmission() {
    release();
}

ConnectionAdmission::ConnectionAdmission(ConnectionAdmission&& other) noexcept
    : state_(std::exchange(other.state_, nullptr)), source_(other.source_),
      source_counted_(other.source_counted_) {}

ConnectionAdmission& ConnectionAdmission::operator=(ConnectionAdmission&& other) noexcept {
    if (this == &other)
        return *this;
    release();
    state_ = std::exchange(other.state_, nullptr);
    source_ = other.source_;
    source_counted_ = other.source_counted_;
    return *this;
}

void ConnectionAdmission::release() noexcept {
    if (state_ == nullptr)
        return;
    state_->release(source_, source_counted_);
    state_ = nullptr;
}

std::expected<ConnectionAdmission, AdmissionRejection> ServerState::admit(
    const PeerAddress& peer) {
    const bool count_source = !peer.is_unix;
    const std::scoped_lock guard(admission_mutex_);
    if (active_connections_ >= max_connections_)
        return std::unexpected(AdmissionRejection::GlobalLimit);

    if (count_source) {
        const auto entry = source_connections_.find(peer.bytes);
        const unsigned source_count = entry == source_connections_.end() ? 0U : entry->second;
        if (source_count >= max_connections_per_source_)
            return std::unexpected(AdmissionRejection::SourceLimit);
        source_connections_.insert_or_assign(peer.bytes, source_count + 1);
    }

    ++active_connections_;
    return ConnectionAdmission(*this, peer.bytes, count_source);
}

void ServerState::release(const std::array<uint8_t, kIpv6ByteCount>& source,
                          bool source_counted) noexcept {
    try {
        const std::scoped_lock guard(admission_mutex_);
        if (active_connections_ > 0)
            --active_connections_;
        if (!source_counted)
            return;

        const auto entry = source_connections_.find(source);
        if (entry == source_connections_.end())
            return;
        if (entry->second <= 1)
            source_connections_.erase(entry);
        else
            --entry->second;
    } catch (...) {
        logger_.write(LogLevel::Error, "connection admission accounting failed during release");
    }
}

void ServerState::fail(std::exception_ptr failure) noexcept {
    try {
        const std::scoped_lock guard(failure_mutex_);
        if (failure_ == nullptr)
            failure_ = std::move(failure);
    } catch (...) {
        logger_.write(LogLevel::Error, "could not retain the reactor failure");
    }
    request_stop();
}

std::exception_ptr ServerState::failure() const noexcept {
    try {
        const std::scoped_lock guard(failure_mutex_);
        return failure_;
    } catch (...) {
        return nullptr;
    }
}

} // namespace erikslund::http::internal
