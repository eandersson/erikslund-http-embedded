#pragma once

#include <chrono>
#include <cstdint>
#include <inplace_vector>
#include <memory>
#include <optional>
#include <string>

#include "erikslund/http/cidr.hpp"
#include "erikslund/http/peer_address.hpp"
#include "erikslund/http/request.hpp"
#include "erikslund/http/response.hpp"
#include "erikslund/http/router.hpp"
#include "erikslund/http/server.hpp"
#include "erikslund/http/status.hpp"
#include "internal/buffer.hpp"
#include "internal/request_parser.hpp"
#include "internal/server_state.hpp"
#include "internal/transport.hpp"
#include "internal/unique_fd.hpp"

namespace erikslund::http::internal {

class Reactor;
class ServerMetrics;

enum class ConnectionState : uint8_t {
    Handshaking,
    ReadingHeaders,
    ReadingBody,
    Writing,
    Streaming,
    Closing,
};

enum class EpollInterest : uint8_t { Done, Quiescent, Read, Write, ReadWrite };

// Owned and accessed exclusively by one Reactor. Cross-thread stream notifications reach the
// reactor's eventfd and never touch this object.
class Connection {
public:
    Connection(UniqueFd fd, PeerAddress peer, Transport transport, Reactor& reactor,
               const Router& router, const ServerOptions& options, const Listener& listener,
               ServerMetrics* metrics, ConnectionAdmission admission);
    ~Connection();
    Connection(const Connection&) = delete("a Connection owns its descriptor and buffers");
    Connection& operator=(const Connection&) = delete("a Connection owns its descriptor");

    // Drives until blocked and returns the next epoll interest or Done.
    [[nodiscard]] EpollInterest step(uint32_t epoll_events);

    [[nodiscard]] EpollInterest on_deadline_expired();

    [[nodiscard]] std::chrono::steady_clock::time_point deadline() const noexcept {
        return deadline_;
    }

    [[nodiscard]] ConnectionState state() const noexcept { return state_; }
    [[nodiscard]] int fd() const noexcept { return fd_.get(); }
    [[nodiscard]] const PeerAddress& peer() const noexcept { return peer_; }

    void begin_close() noexcept;

    [[nodiscard]] EpollInterest on_stream_notified();

    [[nodiscard]] bool is_streaming() const noexcept { return state_ == ConnectionState::Streaming; }

private:
    [[nodiscard]] EpollInterest drive_handshake();
    [[nodiscard]] EpollInterest drive_read_headers();
    [[nodiscard]] EpollInterest drive_read_body();
    [[nodiscard]] EpollInterest drive_write();
    [[nodiscard]] EpollInterest drive_stream();
    [[nodiscard]] EpollInterest drive_close();

    void adopt_parsed_request(ParsedRequest parsed);
    void dispatch_and_serialize();

    // Sends nothing when the peer has not sent any bytes.
    void serialize_error(Status status);

    void enter_state(ConnectionState next) noexcept;
    void arm_deadline(std::chrono::milliseconds budget) noexcept;

    // Idempotent.
    void release_stream_source() noexcept;

    [[nodiscard]] std::chrono::milliseconds stream_liveness_budget() const noexcept;

    UniqueFd fd_;
    PeerAddress peer_;
    Transport transport_;
    ConnectionAdmission admission_;

    // Non-owning; all outlive this connection.
    Reactor* reactor_ = nullptr;
    const Router* router_ = nullptr;
    const ServerOptions* options_ = nullptr;
    const Listener* listener_ = nullptr;
    ServerMetrics* metrics_ = nullptr;

    Buffer read_buffer_;
    Buffer write_buffer_;

    RequestParser request_parser_;
    Request request_;

    // Keeps borrowed response views valid through the write.
    std::optional<Response> pending_response_;

    // Stable storage referenced by write_vectors_ during compressed writes.
    std::string compressed_body_;

    std::inplace_vector<iovec, kMaxWriteVectors> write_vectors_;
    size_t write_offset_ = 0;

    // Possession begins before attachment so every exit path can release the source.
    std::shared_ptr<StreamSource> stream_;
    std::shared_ptr<StreamNotifier> notifier_;

    std::chrono::steady_clock::time_point deadline_{};
    std::chrono::steady_clock::time_point request_deadline_{};

    unsigned requests_served_ = 0;
    ConnectionState state_ = ConnectionState::Handshaking;

    bool saw_any_bytes_ = false;
    bool stream_waiting_for_write_progress_ = false;
};

} // namespace erikslund::http::internal
