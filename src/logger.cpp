#include "internal/logger.hpp"

#include <cstdio>

namespace erikslund::http::internal {

void Logger::write(LogLevel level, std::string_view message) const noexcept {
    try {
        if (sink_)
            sink_(level, message);
    } catch (...) {
        std::fputs("[error] erikslund-http log sink threw\n", stderr);
    }
}

void Logger::write_peer(LogLevel level, std::string_view message) noexcept {
    bool should_log = false;
    bool report_suppression = false;
    try {
        const auto moment = std::chrono::steady_clock::now();
        const std::scoped_lock guard(peer_mutex_);
        if (peer_window_started_ == std::chrono::steady_clock::time_point{} ||
            moment - peer_window_started_ >= kPeerLogWindow) {
            peer_window_started_ = moment;
            peer_messages_ = 0;
            peer_suppression_reported_ = false;
        }

        if (peer_messages_ < kPeerLogBurst) {
            ++peer_messages_;
            should_log = true;
        } else if (!peer_suppression_reported_) {
            peer_suppression_reported_ = true;
            report_suppression = true;
        }
    } catch (...) {
        return;
    }

    if (should_log)
        write(level, message);
    else if (report_suppression)
        write(LogLevel::Warning, "further peer-triggered log messages are suppressed for 10s");
}

} // namespace erikslund::http::internal
