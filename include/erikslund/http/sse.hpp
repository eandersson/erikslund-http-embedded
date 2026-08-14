#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <string_view>

#include "erikslund/http/router.hpp"

namespace erikslund::http {

inline constexpr size_t kDefaultMaxSubscribers = 32;

// Over-budget subscribers are disconnected.
inline constexpr size_t kDefaultSubscriberQueueBytes = 65'536;
inline constexpr size_t kDefaultMaxSseEventBytes = 16'384;

// Zero disables heartbeats.
inline constexpr std::chrono::seconds kDefaultHeartbeatInterval{15};

inline constexpr std::chrono::seconds kDefaultRetryHint{5};

struct SseOptions {
    size_t max_subscribers = kDefaultMaxSubscribers;
    size_t max_queued_bytes_per_subscriber = kDefaultSubscriberQueueBytes;
    size_t max_event_bytes = kDefaultMaxSseEventBytes;
    std::chrono::seconds heartbeat_interval = kDefaultHeartbeatInterval;
    std::chrono::seconds retry_hint = kDefaultRetryHint;
};

// All public operations are thread-safe. Publishing wakes each subscriber's owning reactor; only
// that reactor touches the connection or socket. Use separate channels for separate audiences.
class SseChannel {
public:
    explicit SseChannel(SseOptions options = {});
    ~SseChannel();
    SseChannel(const SseChannel&) = delete("a channel owns live subscriber connections");
    SseChannel& operator=(const SseChannel&) = delete("a channel owns live subscriber connections");

    // Answers 503 after max_subscribers is reached.
    [[nodiscard]] Handler handler();

    // Empty event names are allowed; multiline payloads emit one data field per line.
    void publish(std::string_view event, std::string_view data);

    void publish_json(std::string_view event, std::string_view json_body);

    [[nodiscard]] size_t subscriber_count() const noexcept;

    void close_all() noexcept;

private:
    class State;
    std::shared_ptr<State> state_;
};

} // namespace erikslund::http
