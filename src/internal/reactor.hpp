#pragma once

#include <atomic>
#include <cstdint>
#include <flat_map>
#include <latch>
#include <memory>
#include <mutex>
#include <span>
#include <stop_token>
#include <vector>

#include "erikslund/http/cidr.hpp"
#include "erikslund/http/response.hpp"
#include "erikslund/http/router.hpp"
#include "erikslund/http/server.hpp"
#include "internal/connection.hpp"
#include "internal/server_state.hpp"
#include "internal/socket.hpp"
#include "internal/unique_fd.hpp"

namespace erikslund::http::internal {

inline constexpr std::chrono::milliseconds kMaxEpollWait{500};

inline constexpr size_t kEpollBatchSize = 64;

// The only cross-thread reactor state; shared ownership keeps late notifications safe.
struct WakeState {
    UniqueFd event_fd;

    std::mutex mutex;

    std::vector<uint64_t> pending_streams;

    std::atomic<bool> alive{true};

    void notify_stream(uint64_t stream_id) noexcept;

    void wake() noexcept;
};

// Carries a never-reused stream id, not a reusable descriptor or Connection pointer.
class ReactorNotifier final : public StreamNotifier {
public:
    ReactorNotifier(std::shared_ptr<WakeState> wake, uint64_t stream_id) noexcept
        : wake_(std::move(wake)), stream_id_(stream_id) {}

    void notify() noexcept override;

private:
    std::shared_ptr<WakeState> wake_;
    uint64_t stream_id_ = 0;
};

// One worker owns this reactor and every Connection in its table.
class Reactor {
public:
    Reactor(unsigned index, const Router& router, const ServerOptions& options,
            const CidrAllowList& allow_list, ServerMetrics* metrics, ServerState& server_state);
    ~Reactor();
    Reactor(const Reactor&) = delete("a Reactor owns an epoll instance and live connections");
    Reactor& operator=(const Reactor&) = delete("a Reactor owns an epoll instance");

    void add_listener(ListenerState& listener, int listen_fd, bool exclusive);

    // Signals ready immediately before entering the noexcept event loop.
    void run(const std::stop_token& stop, std::latch& ready) noexcept;

    void wake() noexcept;

    [[nodiscard]] std::shared_ptr<StreamNotifier> make_notifier(int connection_fd);

    [[nodiscard]] unsigned index() const noexcept { return index_; }
    [[nodiscard]] size_t connection_count() const noexcept { return connections_.size(); }

private:
    struct ListenerBinding {
        ListenerState* listener = nullptr;
        int listen_fd = -1;
    };

    void accept_ready(const ListenerBinding& binding);

    // Rejected peers are closed without a response.
    void admit(UniqueFd fd, PeerAddress peer, ListenerState& listener);

    void handle_connection_event(int fd, uint32_t events);

    void scan_deadlines();

    void drain_stream_wakeups();

    void arm(int fd, EpollInterest interest);
    void close_connection(int fd);

    unsigned index_ = 0;
    UniqueFd epoll_fd_;
    std::shared_ptr<WakeState> wake_;

    std::vector<ListenerBinding> listeners_;

    std::flat_map<int, std::unique_ptr<Connection>> connections_;

    // Stream id to connection descriptor. Missing ids are stale notifications.
    std::flat_map<uint64_t, int> streaming_;

    // Never reused during this reactor's lifetime.
    uint64_t next_stream_id_ = 1;

    const Router* router_ = nullptr;
    const ServerOptions* options_ = nullptr;
    const CidrAllowList* allow_list_ = nullptr;
    ServerMetrics* metrics_ = nullptr;
    ServerState* server_state_ = nullptr;
    Logger* logger_ = nullptr;
};

} // namespace erikslund::http::internal
