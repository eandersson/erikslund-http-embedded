#include "internal/reactor.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <exception>
#include <format>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "erikslund/http/build_config.hpp"

#if ERIKSLUND_HTTP_STACKTRACE
#include <stacktrace>
#endif

#include <sys/epoll.h>

#include "erikslund/http/contracts.hpp"
#include "internal/tls_context.hpp"
#include "internal/transport.hpp"
#include "internal/server_metrics.hpp"

// Each worker owns an epoll instance and, when supported, SO_REUSEPORT listener sockets. Shared
// listeners use EPOLLEXCLUSIVE to avoid waking every worker.

namespace erikslund::http::internal {
namespace {

constexpr size_t kErrorTextBytes = 256;
constexpr size_t kMaxAcceptsPerTurn = 64;

[[nodiscard]] std::string errno_text(int error_number) {
    std::array<char, kErrorTextBytes> storage{};
    const char* text = ::strerror_r(error_number, storage.data(), storage.size());
    return text != nullptr ? std::string(text) : std::string("unknown error");
}

void log_stacktrace(Logger& logger) noexcept {
#if ERIKSLUND_HTTP_STACKTRACE
    try {
        logger.write(LogLevel::Error, std::to_string(std::stacktrace::current()));
    } catch (...) {
        logger.write(LogLevel::Error, "stacktrace formatting failed");
    }
#else
    static_cast<void>(logger);
#endif
}

// Never request EPOLLRDHUP: it remains level-triggered after a half-close and would spin. EOF still
// arrives through read readiness; terminal ERR/HUP are reported without being requested.
[[nodiscard]] uint32_t epoll_mask_for(EpollInterest interest) noexcept {
    switch (interest) {
    case EpollInterest::Read:
        return static_cast<uint32_t>(EPOLLIN);
    case EpollInterest::Write:
        return static_cast<uint32_t>(EPOLLOUT);
    case EpollInterest::ReadWrite:
        return static_cast<uint32_t>(EPOLLIN) | static_cast<uint32_t>(EPOLLOUT);
    case EpollInterest::Quiescent:
    case EpollInterest::Done:
        return 0;
    }
    return 0;
}

} // namespace

void WakeState::notify_stream(uint64_t stream_id) noexcept {
    if (!alive.load(std::memory_order_acquire))
        return;
    try {
        const std::scoped_lock guard(mutex);
        // One pending entry per stream bounds a hot publisher while the reactor is busy.
        if (std::ranges::find(pending_streams, stream_id) == pending_streams.end())
            pending_streams.push_back(stream_id);
    } catch (const std::exception&) {
        return;
    }
    signal_event_fd(event_fd.get());
}

void WakeState::wake() noexcept {
    signal_event_fd(event_fd.get());
}

void ReactorNotifier::notify() noexcept {
    if (wake_)
        wake_->notify_stream(stream_id_);
}

Reactor::Reactor(unsigned index, const Router& router, const ServerOptions& options,
                  const CidrAllowList& allow_list, ServerMetrics* metrics,
                  ServerState& server_state)
    : index_(index), epoll_fd_(::epoll_create1(EPOLL_CLOEXEC)),
      wake_(std::make_shared<WakeState>()), router_(&router), options_(&options),
      allow_list_(&allow_list), metrics_(metrics), server_state_(&server_state),
      logger_(&server_state.logger()) {
    if (!epoll_fd_)
        throw ServerError(std::format("epoll_create1() failed: {}", errno_text(errno)));

    wake_->event_fd = make_event_fd();

    epoll_event registration{};
    registration.events = static_cast<uint32_t>(EPOLLIN);
    registration.data.fd = wake_->event_fd.get();
    if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, wake_->event_fd.get(), &registration) != 0)
        throw ServerError(std::format("registering the wakeup eventfd failed: {}",
                                      errno_text(errno)));
}

Reactor::~Reactor() {
    wake_->alive.store(false, std::memory_order_release);

    // Deregister descriptors before their owning objects close them.
    for (const auto& [connection_fd, connection] : connections_)
        ::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_DEL, connection_fd, nullptr);

    if (metrics_ != nullptr && !connections_.empty())
        metrics_->connections_active_changed(-static_cast<int64_t>(connections_.size()));

    connections_.clear();
    streaming_.clear();
}

void Reactor::add_listener(ListenerState& listener, int listen_fd, bool exclusive) {
    epoll_event registration{};
    registration.events = static_cast<uint32_t>(EPOLLIN);
    if (exclusive) {
        registration.events |= static_cast<uint32_t>(EPOLLEXCLUSIVE);
    }
    registration.data.fd = listen_fd;
    if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, listen_fd, &registration) != 0)
        throw ServerError(std::format("registering a listening socket on reactor {} failed: {}",
                                      index_, errno_text(errno)));

    listeners_.push_back(ListenerBinding{&listener, listen_fd});
}

void Reactor::run(const std::stop_token& stop, std::latch& ready) noexcept {
    struct ReadyGuard {
        std::latch* pending = nullptr;
        ~ReadyGuard() {
            if (pending != nullptr)
                pending->count_down();
        }
        void release() noexcept {
            if (pending != nullptr) {
                pending->count_down();
                pending = nullptr;
            }
        }
    } ready_guard{&ready};

    try {
        const std::stop_callback stop_wake(stop, [this] { wake(); });

        ready_guard.release();

        std::array<epoll_event, kEpollBatchSize> events{};
        while (!stop.stop_requested()) {
            const int count =
                ::epoll_wait(epoll_fd_.get(), events.data(), static_cast<int>(events.size()),
                             static_cast<int>(kMaxEpollWait.count()));
            if (count < 0) {
                if (errno == EINTR)
                    continue;
                throw ServerError(std::format("epoll_wait() failed: {}", errno_text(errno)));
            }

            for (int index = 0; index < count; ++index) {
                const int ready_fd = events[static_cast<size_t>(index)].data.fd;
                if (ready_fd == wake_->event_fd.get())
                    continue;
                if (std::ranges::any_of(listeners_, [ready_fd](const ListenerBinding& binding) {
                        return binding.listen_fd == ready_fd;
                    }))
                    continue;
                handle_connection_event(ready_fd, events[static_cast<size_t>(index)].events);
            }

            for (int index = 0; index < count; ++index) {
                const int ready_fd = events[static_cast<size_t>(index)].data.fd;
                if (ready_fd == wake_->event_fd.get()) {
                    drain_event_fd(ready_fd);
                    drain_stream_wakeups();
                    continue;
                }
                const auto binding =
                    std::ranges::find_if(listeners_, [ready_fd](const ListenerBinding& entry) {
                        return entry.listen_fd == ready_fd;
                    });
                if (binding != listeners_.end())
                    accept_ready(*binding);
            }

            // Scan after every wait because continuous traffic may prevent timeout returns.
            scan_deadlines();
        }
    } catch (const std::exception& error) {
        logger_->writef(LogLevel::Error, "reactor {} stopped: {}", index_, error.what());
        log_stacktrace(*logger_);
        server_state_->fail(std::current_exception());
    } catch (...) {
        logger_->writef(LogLevel::Error, "reactor {} stopped on an unknown exception", index_);
        log_stacktrace(*logger_);
        server_state_->fail(std::current_exception());
    }

    wake_->alive.store(false, std::memory_order_release);

    for (const auto& [connection_fd, connection] : connections_)
        connection->begin_close();
}

void Reactor::wake() noexcept {
    wake_->wake();
}

std::shared_ptr<StreamNotifier> Reactor::make_notifier(int connection_fd) {
    const uint64_t stream_id = next_stream_id_++;
    streaming_.insert_or_assign(stream_id, connection_fd);
    return std::make_shared<ReactorNotifier>(wake_, stream_id);
}

void Reactor::accept_ready(const ListenerBinding& binding) {
    ERIKSLUND_HTTP_ASSERT(binding.listener != nullptr);

    // retire_listener() may erase binding's storage.
    const int listen_fd = binding.listen_fd;
    ListenerState& listener = *binding.listener;

    for (size_t accepted_count = 0; accepted_count < kMaxAcceptsPerTurn; ++accepted_count) {
        AcceptResult accepted = accept_connection(listen_fd);
        switch (accepted.status) {
        case AcceptStatus::Accepted:
            admit(std::move(accepted.fd), accepted.peer, listener);
            break;

        case AcceptStatus::WouldBlock:
            return;

        case AcceptStatus::TransientError:
            // Persistent accept errors remain readable; back off to avoid a hot loop.
            logger_->writef(LogLevel::Warning,
                            "reactor {} backing off after an accept failure", index_);
            std::this_thread::sleep_for(kAcceptErrorBackoff);
            return;

        case AcceptStatus::FatalError: {
            const int accept_error = errno;
            throw ServerError(std::format("accept() failed on reactor {}: {}", index_,
                                          errno_text(accept_error)));
        }
        }
    }
}

void Reactor::admit(UniqueFd fd, PeerAddress peer, ListenerState& listener) {
    if (metrics_ != nullptr)
        metrics_->connection_accepted();

    if (!peer.is_unix && !allow_list_->empty() && !allow_list_->allows(peer)) {
        if (metrics_ != nullptr)
            metrics_->connection_rejected(ConnectionRejection::Cidr);
        return;
    }

    std::expected<ConnectionAdmission, AdmissionRejection> admission =
        server_state_->admit(peer);
    if (!admission.has_value()) {
        if (metrics_ != nullptr)
            metrics_->connection_rejected(admission.error() == AdmissionRejection::GlobalLimit
                                              ? ConnectionRejection::Limit
                                              : ConnectionRejection::SourceLimit);
        return;
    }

    if (options_->tcp_nodelay && !listener.is_unix)
        static_cast<void>(set_tcp_nodelay(fd.get()));

    const int connection_fd = fd.get();
    Transport transport{PlainTransport(connection_fd)};

#if ERIKSLUND_HTTP_TLS
    if (!listener.tls.empty()) {
        const std::shared_ptr<TlsContext> context = listener.tls.load();
        std::optional<TlsTransport> secure;
        if (context)
            secure = context->make_transport(connection_fd);
        if (!secure.has_value()) {
            if (metrics_ != nullptr)
                metrics_->connection_rejected(ConnectionRejection::Tls);
            return;
        }
        transport = Transport{std::move(*secure)};
    }
#endif

    auto connection = std::make_unique<Connection>(std::move(fd), peer, std::move(transport), *this,
                                                   *router_, *options_, listener.config, metrics_,
                                                   std::move(*admission));

    epoll_event registration{};
    registration.events = epoll_mask_for(EpollInterest::Read);
    registration.data.fd = connection_fd;
    if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_ADD, connection_fd, &registration) != 0) {
        logger_->write_peerf(LogLevel::Warning,
                             "reactor {} could not register an accepted connection: {}", index_,
                             errno_text(errno));
        return;
    }

    connections_.insert_or_assign(connection_fd, std::move(connection));
    if (metrics_ != nullptr)
        metrics_->connections_active_changed(1);
}

void Reactor::handle_connection_event(int fd, uint32_t events) {
    const auto entry = connections_.find(fd);
    if (entry == connections_.end())
        return;

    try {
        const EpollInterest interest = entry->second->step(events);
        if (interest != EpollInterest::Done) {
            arm(fd, interest);
            return;
        }
    } catch (const std::exception& error) {
        logger_->write_peerf(LogLevel::Error, "connection {} failed: {}", fd, error.what());
    } catch (...) {
        logger_->write_peerf(LogLevel::Error,
                             "connection {} failed on an unknown exception", fd);
    }

    if (connections_.contains(fd))
        close_connection(fd);
}

void Reactor::scan_deadlines() {
    const auto moment = std::chrono::steady_clock::now();

    std::vector<int> expired;
    for (const auto& [connection_fd, connection] : connections_)
        if (connection->deadline() <= moment)
            expired.push_back(connection_fd);

    for (const int connection_fd : expired) {
        const auto entry = connections_.find(connection_fd);
        if (entry == connections_.end())
            continue;
        const EpollInterest interest = entry->second->on_deadline_expired();
        if (interest == EpollInterest::Done)
            close_connection(connection_fd);
        else
            arm(connection_fd, interest);
    }
}

void Reactor::drain_stream_wakeups() {
    std::vector<uint64_t> pending;
    {
        const std::scoped_lock guard(wake_->mutex);
        pending.swap(wake_->pending_streams);
    }

    for (const uint64_t stream_id : pending) {
        const auto mapping = streaming_.find(stream_id);
        if (mapping == streaming_.end())
            continue;
        const int connection_fd = mapping->second;
        const auto entry = connections_.find(connection_fd);
        if (entry == connections_.end())
            continue;

        const EpollInterest interest = entry->second->on_stream_notified();
        if (interest == EpollInterest::Done)
            close_connection(connection_fd);
        else
            arm(connection_fd, interest);
    }
}

void Reactor::arm(int fd, EpollInterest interest) {
    ERIKSLUND_HTTP_ASSERT(interest != EpollInterest::Done);

    epoll_event registration{};
    // An empty mask stays registered for implicit ERR/HUP delivery.
    registration.events = epoll_mask_for(interest);
    registration.data.fd = fd;
    // Always update the kernel instead of maintaining a second cached mask.
    if (::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_MOD, fd, &registration) != 0) {
        logger_->write_peerf(LogLevel::Debug,
                             "reactor {} could not re-arm a connection: {}", index_,
                             errno_text(errno));
        close_connection(fd);
    }
}

void Reactor::close_connection(int fd) {
    const auto entry = connections_.find(fd);
    if (entry == connections_.end())
        return;

    // Remove from epoll before destruction closes the descriptor.
    ::epoll_ctl(epoll_fd_.get(), EPOLL_CTL_DEL, fd, nullptr);

    for (auto mapping = streaming_.begin(); mapping != streaming_.end();) {
        if (mapping->second == fd)
            mapping = streaming_.erase(mapping);
        else
            ++mapping;
    }

    connections_.erase(entry);
    if (metrics_ != nullptr)
        metrics_->connections_active_changed(-1);
}

} // namespace erikslund::http::internal
