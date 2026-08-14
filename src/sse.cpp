#include "erikslund/http/sse.hpp"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <deque>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "erikslund/http/contracts.hpp"
#include "erikslund/http/method.hpp"
#include "erikslund/http/request.hpp"
#include "erikslund/http/response.hpp"
#include "erikslund/http/router.hpp"
#include "erikslund/http/server.hpp"
#include "erikslund/http/status.hpp"

namespace erikslund::http {

namespace {

constexpr std::string_view kEventStreamContentType = "text/event-stream";

constexpr std::string_view kEventFieldPrefix = "event: ";
constexpr std::string_view kDataFieldPrefix = "data: ";
constexpr std::string_view kRetryFieldPrefix = "retry: ";

constexpr std::string_view kHeartbeatFrame = ": heartbeat\n\n";

constexpr std::string_view kSubscriberLimitBody = "sse: subscriber limit reached\n";
constexpr std::string_view kChannelClosedBody = "sse: channel closed\n";

constexpr std::chrono::seconds kHeartbeatTickInterval{1};

constexpr unsigned kOverflowsBeforeDisconnect = 4;

constexpr int64_t kMillisecondsPerSecond = 1'000;

using SharedFrame = std::shared_ptr<const std::string>;

void append_data_lines(std::string& frame, std::string_view payload) {
    size_t line_start = 0;
    while (true) {
        const size_t break_at = payload.find_first_of("\r\n", line_start);
        const size_t line_end = (break_at == std::string_view::npos) ? payload.size() : break_at;
        frame += kDataFieldPrefix;
        frame += payload.substr(line_start, line_end - line_start);
        frame += '\n';
        if (break_at == std::string_view::npos)
            return;
        line_start = break_at + 1;
        if (payload[break_at] == '\r' && line_start < payload.size() && payload[line_start] == '\n')
            ++line_start;
    }
}

void append_event_name(std::string& frame, std::string_view event) {
    frame += kEventFieldPrefix;
    for (const char character : event)
        frame += (character == '\r' || character == '\n') ? ' ' : character;
    frame += '\n';
}

[[nodiscard]] bool add_frame_bytes(size_t& frame_size, size_t bytes, size_t limit) noexcept {
    if (bytes > limit - frame_size)
        return false;
    frame_size += bytes;
    return true;
}

[[nodiscard]] std::optional<size_t> event_frame_size(std::string_view event, std::string_view data,
                                                     size_t limit) noexcept {
    size_t frame_size = 0;
    if (!event.empty() &&
        (!add_frame_bytes(frame_size, kEventFieldPrefix.size(), limit) ||
         !add_frame_bytes(frame_size, event.size(), limit) ||
         !add_frame_bytes(frame_size, 1, limit)))
        return std::nullopt;

    size_t line_start = 0;
    while (true) {
        const size_t break_at = data.find_first_of("\r\n", line_start);
        const size_t line_end = break_at == std::string_view::npos ? data.size() : break_at;
        if (!add_frame_bytes(frame_size, kDataFieldPrefix.size(), limit) ||
            !add_frame_bytes(frame_size, line_end - line_start, limit) ||
            !add_frame_bytes(frame_size, 1, limit))
            return std::nullopt;
        if (break_at == std::string_view::npos)
            break;
        line_start = break_at + 1;
        if (data[break_at] == '\r' && line_start < data.size() && data[line_start] == '\n')
            ++line_start;
    }
    if (!add_frame_bytes(frame_size, 1, limit))
        return std::nullopt;
    return frame_size;
}

[[nodiscard]] SharedFrame build_event_frame(std::string_view event, std::string_view data,
                                            size_t frame_size) {
    std::string frame;
    frame.reserve(frame_size);
    if (!event.empty())
        append_event_name(frame, event);
    append_data_lines(frame, data);
    frame += '\n';
    return std::make_shared<const std::string>(std::move(frame));
}

[[nodiscard]] SharedFrame build_retry_frame(std::chrono::seconds retry_hint) {
    std::string frame;
    frame += kRetryFieldPrefix;
    frame += std::to_string(retry_hint.count() * kMillisecondsPerSecond);
    frame += "\n\n";
    return std::make_shared<const std::string>(std::move(frame));
}

} // namespace

class SseSubscriber final : public StreamSource {
public:
    explicit SseSubscriber(const SseOptions& options)
        : max_queued_bytes_(options.max_queued_bytes_per_subscriber),
          heartbeat_interval_(options.heartbeat_interval),
          last_write_(std::chrono::steady_clock::now()) {
        if (options.retry_hint > std::chrono::seconds::zero()) {
            SharedFrame retry = build_retry_frame(options.retry_hint);
            if (retry->size() <= max_queued_bytes_)
                push_back_locked(std::move(retry));
        }
    }

    [[nodiscard]] std::string_view content_type() const noexcept override {
        return kEventStreamContentType;
    }

    [[nodiscard]] Pull pull(std::string& out) override {
        std::lock_guard lock(mutex_);
        ERIKSLUND_HTTP_ASSERT(!detached_);
        if (queue_.empty())
            return closed_ ? Pull::Finished : Pull::Idle;
        for (const SharedFrame& frame : queue_)
            out += *frame;
        queue_.clear();
        queued_bytes_ = 0;
        consecutive_overflows_ = 0;
        last_write_ = std::chrono::steady_clock::now();
        return Pull::Wrote;
    }

    void on_attached(std::shared_ptr<StreamNotifier> notifier) override {
        std::lock_guard lock(mutex_);
        notifier_ = std::move(notifier);
    }

    void on_detached() noexcept override {
        std::lock_guard lock(mutex_);
        detached_ = true;
        closed_ = true;
        notifier_.reset();
        queue_.clear();
        queued_bytes_ = 0;
    }

    void enqueue(const SharedFrame& frame) {
        std::shared_ptr<StreamNotifier> notifier;
        {
            std::lock_guard lock(mutex_);
            if (closed_)
                return;
            if (frame->size() > max_queued_bytes_) {
                close_locked();
                notifier = notifier_;
            } else {
                bool dropped = false;
                while (!queue_.empty() && frame->size() > max_queued_bytes_ - queued_bytes_) {
                    queued_bytes_ -= queue_.front()->size();
                    queue_.pop_front();
                    dropped = true;
                }
                push_back_locked(frame);
                if (dropped && ++consecutive_overflows_ >= kOverflowsBeforeDisconnect)
                    close_locked();
                notifier = notifier_;
            }
        }
        if (notifier)
            notifier->notify();
    }

    void begin_close() noexcept {
        std::shared_ptr<StreamNotifier> notifier;
        {
            std::lock_guard lock(mutex_);
            closed_ = true;
            notifier = notifier_;
        }
        if (notifier)
            notifier->notify();
    }

    [[nodiscard]] bool alive() const noexcept {
        std::lock_guard lock(mutex_);
        return !closed_ && !detached_;
    }

    void maybe_heartbeat(std::chrono::steady_clock::time_point now) {
        std::shared_ptr<StreamNotifier> notifier;
        {
            std::lock_guard lock(mutex_);
            if (closed_ || detached_ || heartbeat_interval_ <= std::chrono::seconds::zero())
                return;
            if (now - last_write_ < heartbeat_interval_)
                return;
            if (!queue_.empty())
                return;
            if (kHeartbeatFrame.size() > max_queued_bytes_) {
                close_locked();
            } else {
                push_back_locked(std::make_shared<const std::string>(kHeartbeatFrame));
            }
            last_write_ = now;
            notifier = notifier_;
        }
        if (notifier)
            notifier->notify();
    }

private:
    void push_back_locked(SharedFrame frame) {
        queued_bytes_ += frame->size();
        queue_.push_back(std::move(frame));
    }

    void close_locked() noexcept {
        closed_ = true;
        queue_.clear();
        queued_bytes_ = 0;
    }

    mutable std::mutex mutex_;
    std::deque<SharedFrame> queue_;
    size_t queued_bytes_ = 0;
    size_t max_queued_bytes_ = 0;
    std::chrono::seconds heartbeat_interval_{};
    std::shared_ptr<StreamNotifier> notifier_;
    std::chrono::steady_clock::time_point last_write_{};
    unsigned consecutive_overflows_ = 0;
    bool closed_ = false;
    bool detached_ = false;
};

namespace {

void prune_dead(std::vector<std::shared_ptr<SseSubscriber>>& subscribers) {
    std::erase_if(subscribers, [](const std::shared_ptr<SseSubscriber>& subscriber) {
        return !subscriber->alive();
    });
}

class HeartbeatPump {
public:
    HeartbeatPump(const HeartbeatPump&) = delete("one ticker serves every channel in the process");
    HeartbeatPump& operator=(const HeartbeatPump&) =
        delete("one ticker serves every channel in the process");

    static HeartbeatPump& instance() {
        static HeartbeatPump pump;
        return pump;
    }

    void add(std::weak_ptr<SseSubscriber> subscriber) {
        {
            std::lock_guard lock(mutex_);
            subscribers_.push_back(std::move(subscriber));
        }
        idle_.notify_all();
    }

private:
    HeartbeatPump() : thread_([this](const std::stop_token& stop) { run(stop); }) {}

    void run(const std::stop_token& stop) noexcept {
        try {
            std::unique_lock lock(mutex_);
            while (!stop.stop_requested()) {
                if (subscribers_.empty()) {
                    idle_.wait(lock, stop, [this] { return !subscribers_.empty(); });
                    continue;
                }
                idle_.wait_for(lock, stop, kHeartbeatTickInterval, [] { return false; });
                if (stop.stop_requested())
                    return;
                tick_locked();
            }
        } catch (const std::exception& e) {
            std::fputs("erikslund-http: sse heartbeat stopped: ", stderr);
            std::fputs(e.what(), stderr);
            std::fputc('\n', stderr);
        } catch (...) {
            std::fputs("erikslund-http: sse heartbeat stopped\n", stderr);
        }
    }

    void tick_locked() {
        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        std::erase_if(subscribers_, [now](const std::weak_ptr<SseSubscriber>& weak) {
            const std::shared_ptr<SseSubscriber> subscriber = weak.lock();
            if (!subscriber || !subscriber->alive())
                return true;
            subscriber->maybe_heartbeat(now);
            return false;
        });
    }

    std::mutex mutex_;
    std::condition_variable_any idle_;

    std::vector<std::weak_ptr<SseSubscriber>> subscribers_;

    std::jthread thread_;
};

} // namespace

class SseChannel::State {
public:
    explicit State(SseOptions options) : options_(options) {}

    [[nodiscard]] Response subscribe(const Request& request) {
        if (request.method() == Method::Head) {
            Response probe = Response::empty(Status::Ok);
            probe.header("Content-Type", std::string(kEventStreamContentType));
            return probe;
        }

        auto subscriber = std::make_shared<SseSubscriber>(options_);
        {
            std::lock_guard lock(mutex_);
            if (!accepting_)
                return Response::text(std::string(kChannelClosedBody), Status::ServiceUnavailable);
            prune_dead(subscribers_);
            if (subscribers_.size() >= options_.max_subscribers) {
                Response busy =
                    Response::text(std::string(kSubscriberLimitBody), Status::ServiceUnavailable);
                busy.header("Retry-After", std::to_string(options_.retry_hint.count()));
                return busy;
            }
            subscribers_.push_back(subscriber);
        }
        HeartbeatPump::instance().add(subscriber);

        Response response = Response::stream(std::move(subscriber));
        response.header("X-Accel-Buffering", "no");
        return response;
    }

    void publish(std::string_view event, std::string_view data) {
        const std::optional<size_t> frame_size =
            event_frame_size(event, data, options_.max_event_bytes);
        if (!frame_size)
            throw ServerError("encoded SSE event exceeds max_event_bytes");
        const SharedFrame frame = build_event_frame(event, data, *frame_size);

        std::lock_guard lock(mutex_);
        prune_dead(subscribers_);
        for (const std::shared_ptr<SseSubscriber>& subscriber : subscribers_)
            subscriber->enqueue(frame);
    }

    [[nodiscard]] size_t subscriber_count() const noexcept {
        std::lock_guard lock(mutex_);
        size_t live = 0;
        for (const std::shared_ptr<SseSubscriber>& subscriber : subscribers_)
            if (subscriber->alive())
                ++live;
        return live;
    }

    void close_all() noexcept {
        std::lock_guard lock(mutex_);
        close_subscribers();
    }

    void shutdown() noexcept {
        std::lock_guard lock(mutex_);
        accepting_ = false;
        close_subscribers();
    }

private:
    void close_subscribers() noexcept {
        for (const std::shared_ptr<SseSubscriber>& subscriber : subscribers_)
            subscriber->begin_close();
        subscribers_.clear();
    }

    SseOptions options_;
    mutable std::mutex mutex_;
    std::vector<std::shared_ptr<SseSubscriber>> subscribers_;
    bool accepting_ = true;
};

SseChannel::SseChannel(SseOptions options) : state_(std::make_shared<State>(options)) {}

SseChannel::~SseChannel() {
    state_->shutdown();
}

Handler SseChannel::handler() {
    const std::shared_ptr<State> state = state_;
    return [state](const Request& request) { return state->subscribe(request); };
}

void SseChannel::publish(std::string_view event, std::string_view data) {
    state_->publish(event, data);
}

void SseChannel::publish_json(std::string_view event, std::string_view json_body) {
    publish(event, json_body);
}

size_t SseChannel::subscriber_count() const noexcept {
    return state_->subscriber_count();
}

void SseChannel::close_all() noexcept {
    state_->close_all();
}

} // namespace erikslund::http
