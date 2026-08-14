#include "internal/connection.hpp"

#include <array>
#include <chrono>
#include <cstdint>
#include <exception>
#include <expected>
#include <inplace_vector>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <sys/epoll.h>
#include <sys/uio.h>

#include "erikslund/http/contracts.hpp"
#include "erikslund/http/method.hpp"
#include "erikslund/http/status.hpp"
#include "internal/reactor.hpp"
#include "internal/response_encoder.hpp"
#include "internal/server_metrics.hpp"
#include "internal/socket.hpp"

namespace erikslund::http::internal {
namespace {

constexpr size_t kReadChunkBytes = 4'096;

// Prevents a deep pipeline from monopolizing one reactor turn.
constexpr unsigned kMaxPipelinedRequestsPerStep = 8;

constexpr unsigned kCloseDrainPasses = 4;
constexpr size_t kCloseDrainChunkBytes = 1'024;

[[nodiscard]] std::chrono::steady_clock::time_point now() noexcept {
    return std::chrono::steady_clock::now();
}

[[nodiscard]] std::string& stream_scratch() {
    static thread_local std::string scratch;
    return scratch;
}

[[nodiscard]] constexpr size_t saturating_add(size_t left, size_t right) noexcept {
    const size_t available = std::numeric_limits<size_t>::max() - left;
    return right > available ? std::numeric_limits<size_t>::max() : left + right;
}

} // namespace

Connection::Connection(UniqueFd fd, PeerAddress peer, Transport transport, Reactor& reactor,
                       const Router& router, const ServerOptions& options, const Listener& listener,
                       ServerMetrics* metrics, ConnectionAdmission admission)
    : fd_(std::move(fd)), peer_(peer), transport_(std::move(transport)),
      admission_(std::move(admission)), reactor_(&reactor), router_(&router), options_(&options),
      listener_(&listener), metrics_(metrics), read_buffer_(kInitialBufferBytes),
      write_buffer_(kInitialBufferBytes) {
    request_.set_peer(peer_);
    request_.set_secure(transport_is_secure(transport_));
    enter_state(ConnectionState::Handshaking);
}

Connection::~Connection() {
    release_stream_source();
}

EpollInterest Connection::step(uint32_t epoll_events) {
    // ERR/HUP are terminal. RDHUP only closes the peer's write side, so a response may still leave.
    constexpr uint32_t kTerminalEvents =
        static_cast<uint32_t>(EPOLLERR) | static_cast<uint32_t>(EPOLLHUP);
    if ((epoll_events & kTerminalEvents) != 0)
        return EpollInterest::Done;

    switch (state_) {
    case ConnectionState::Handshaking:
        return drive_handshake();
    case ConnectionState::ReadingHeaders:
        return drive_read_headers();
    case ConnectionState::ReadingBody: {
        const EpollInterest interest = drive_read_body();
        // Buffered pipeline data needs no new read readiness.
        if (state_ == ConnectionState::ReadingHeaders && !read_buffer_.empty())
            return drive_read_headers();
        return interest;
    }
    case ConnectionState::Writing: {
        const EpollInterest interest = drive_write();
        if (state_ == ConnectionState::ReadingHeaders)
            return drive_read_headers();
        return interest;
    }
    case ConnectionState::Streaming:
        return drive_stream();
    case ConnectionState::Closing:
        return drive_close();
    }
    return EpollInterest::Done;
}

EpollInterest Connection::drive_handshake() {
    const TransportResult result = transport_handshake(transport_);
    switch (result.status) {
    case TransportStatus::Ok:
        if (transport_is_secure(transport_) && metrics_ != nullptr)
            metrics_->tls_handshake_completed();
        request_.set_secure(transport_is_secure(transport_));
        enter_state(ConnectionState::ReadingHeaders);
        return drive_read_headers();
    case TransportStatus::WantRead:
        return EpollInterest::Read;
    case TransportStatus::WantWrite:
        return EpollInterest::Write;
    case TransportStatus::Closed:
    case TransportStatus::Error:
        if (transport_is_secure(transport_) && metrics_ != nullptr)
            metrics_->tls_handshake_failed();
        return EpollInterest::Done;
    }
    return EpollInterest::Done;
}

EpollInterest Connection::drive_read_headers() {
    const RequestLimits& limits = options_->limits;

    for (unsigned pass = 0; pass < kMaxPipelinedRequestsPerStep; ++pass) {
        bool body_pending = false;

        while (true) {
            const std::string_view buffered = read_buffer_.readable();
            if (!buffered.empty()) {
                std::expected<ParsedRequest, ParseError> parsed =
                    request_parser_.parse(buffered, limits);
                if (parsed.has_value()) {
                    adopt_parsed_request(std::move(*parsed));
                    dispatch_and_serialize();
                    enter_state(ConnectionState::Writing);
                    break;
                }

                const ParseError error = parsed.error();
                if (error != ParseError::NeedsMoreData) {
                    if (metrics_ != nullptr)
                        metrics_->connection_rejected(ConnectionRejection::Parse);
                    serialize_error(status_for(error));
                    enter_state(ConnectionState::Writing);
                    break;
                }

                if (request_parser_.waiting_for_body()) {
                    enter_state(ConnectionState::ReadingBody);
                    body_pending = true;
                    break;
                }
            }

            const std::span<char> tail = read_buffer_.writable_tail(kReadChunkBytes);
            const TransportResult result = transport_read(transport_, tail);
            if (result.status == TransportStatus::WantRead)
                return EpollInterest::Read;
            if (result.status == TransportStatus::WantWrite)
                return EpollInterest::Write;
            if (result.is_terminal() || result.bytes == 0) {
                begin_close();
                return drive_close();
            }

            read_buffer_.commit(result.bytes);
            if (!saw_any_bytes_) {
                saw_any_bytes_ = true;
                // The first byte starts the absolute slowloris budget.
                request_deadline_ = now() + options_->request_deadline;
                arm_deadline(options_->header_timeout);
            }
        }

        const EpollInterest interest = body_pending ? drive_read_body() : drive_write();
        if (state_ != ConnectionState::ReadingHeaders)
            return interest;
    }

    // Writability yields immediately when buffered pipelines exhaust this turn's fairness budget.
    if (!read_buffer_.empty())
        return EpollInterest::ReadWrite;
    return EpollInterest::Read;
}

EpollInterest Connection::drive_read_body() {
    const RequestLimits& limits = options_->limits;

    while (true) {
        const std::string_view buffered = read_buffer_.readable();
        std::expected<ParsedRequest, ParseError> parsed = request_parser_.parse(buffered, limits);
        if (parsed.has_value()) {
            adopt_parsed_request(std::move(*parsed));
            dispatch_and_serialize();
            enter_state(ConnectionState::Writing);
            return drive_write();
        }

        const ParseError error = parsed.error();
        if (error != ParseError::NeedsMoreData) {
            if (metrics_ != nullptr)
                metrics_->connection_rejected(ConnectionRejection::Parse);
            serialize_error(status_for(error));
            enter_state(ConnectionState::Writing);
            return drive_write();
        }

        const std::span<char> tail = read_buffer_.writable_tail(kReadChunkBytes);
        const TransportResult result = transport_read(transport_, tail);
        if (result.status == TransportStatus::WantRead)
            return EpollInterest::Read;
        if (result.status == TransportStatus::WantWrite)
            return EpollInterest::Write;
        if (result.is_terminal() || result.bytes == 0) {
            begin_close();
            return drive_close();
        }
        read_buffer_.commit(result.bytes);
    }
}

void Connection::adopt_parsed_request(ParsedRequest parsed) {
    const size_t consumed = parsed.consumed_bytes;
    request_ = std::move(parsed.request);
    request_.set_peer(peer_);
    request_.set_secure(transport_is_secure(transport_));
    request_.set_received_at(now());
    read_buffer_.consume(consumed);
    if (std::optional<std::string> subject = transport_peer_certificate_subject(transport_))
        request_.set_client_certificate_subject(std::move(*subject));
}

EpollInterest Connection::drive_write() {
    size_t pending_bytes = 0;
    for (const iovec& entry : write_vectors_)
        pending_bytes += entry.iov_len;

    while (write_offset_ < pending_bytes) {
        // Keep buffer addresses stable across OpenSSL write retries.
        std::inplace_vector<iovec, kMaxWriteVectors> batch;
        size_t skipped = write_offset_;
        for (const iovec& entry : write_vectors_) {
            if (skipped >= entry.iov_len) {
                skipped -= entry.iov_len;
                continue;
            }
            iovec piece = entry;
            piece.iov_base = static_cast<char*>(piece.iov_base) + skipped;
            piece.iov_len -= skipped;
            skipped = 0;
            batch.push_back(piece);
        }
        if (batch.empty())
            break;

        const TransportResult result = transport_writev(transport_, std::span<const iovec>(batch));
        if (result.status == TransportStatus::WantWrite)
            return EpollInterest::Write;
        if (result.status == TransportStatus::WantRead)
            return EpollInterest::Read;
        if (result.is_terminal())
            return EpollInterest::Done;
        if (result.bytes == 0)
            return EpollInterest::Write;

        write_offset_ += result.bytes;
        if (metrics_ != nullptr)
            metrics_->bytes_written(result.bytes);
    }

    // Attach streams after their head is written; HEAD discards without attaching.
    const bool streaming = pending_response_.has_value() && pending_response_->is_stream();
    if (streaming && stream_ && request_.method() != Method::Head) {
        write_vectors_.clear();
        write_offset_ = 0;
        write_buffer_.clear();
        pending_response_.reset();
        notifier_ = reactor_->make_notifier(fd_.get());
        try {
            stream_->on_attached(notifier_);
        } catch (...) {
            begin_close();
            return drive_close();
        }
        enter_state(ConnectionState::Streaming);
        return drive_stream();
    }

    const Status answered =
        pending_response_.has_value() ? pending_response_->status() : Status::InternalServerError;
    if (metrics_ != nullptr) {
        std::optional<std::chrono::duration<double>> service_time;
        if (request_.received_at() != std::chrono::steady_clock::time_point{})
            service_time = now() - request_.received_at();
        metrics_->response_completed(answered, service_time);
    }

    ++requests_served_;
    release_stream_source();
    pending_response_.reset();
    write_vectors_.clear();
    write_offset_ = 0;
    write_buffer_.clear();

    // Error responses and streams converge here with keep_alive disabled.
    const bool keep_alive =
        !streaming && request_.keep_alive() &&
        requests_served_ < options_->max_requests_per_connection;
    if (!keep_alive) {
        begin_close();
        return drive_close();
    }

    request_.reset();
    const size_t retained_read_bytes = saturating_add(options_->limits.max_request_line_bytes,
                                                      options_->limits.max_header_block_bytes);
    read_buffer_.compact_and_reclaim(retained_read_bytes);
    saw_any_bytes_ = !read_buffer_.empty();
    request_deadline_ = saw_any_bytes_ ? now() + options_->request_deadline
                                       : std::chrono::steady_clock::time_point{};
    enter_state(ConnectionState::ReadingHeaders);
    return EpollInterest::Read;
}

EpollInterest Connection::drive_stream() {
    ERIKSLUND_HTTP_ASSERT(state_ == ConnectionState::Streaming);

    while (true) {
        size_t pending_bytes = 0;
        for (const iovec& entry : write_vectors_)
            pending_bytes += entry.iov_len;

        while (write_offset_ < pending_bytes) {
            std::inplace_vector<iovec, kMaxWriteVectors> batch;
            size_t skipped = write_offset_;
            for (const iovec& entry : write_vectors_) {
                if (skipped >= entry.iov_len) {
                    skipped -= entry.iov_len;
                    continue;
                }
                iovec piece = entry;
                piece.iov_base = static_cast<char*>(piece.iov_base) + skipped;
                piece.iov_len -= skipped;
                skipped = 0;
                batch.push_back(piece);
            }
            if (batch.empty())
                break;

            const TransportResult result =
                transport_writev(transport_, std::span<const iovec>(batch));
            if (result.is_terminal()) {
                begin_close();
                return drive_close();
            }
            if (result.is_blocked() || result.bytes == 0) {
                if (!stream_waiting_for_write_progress_) {
                    stream_waiting_for_write_progress_ = true;
                    arm_deadline(options_->write_timeout);
                }
                return result.status == TransportStatus::WantRead ? EpollInterest::Read
                                                                  : EpollInterest::Write;
            }
            write_offset_ += result.bytes;
            stream_waiting_for_write_progress_ = false;
            arm_deadline(options_->write_timeout);
            if (metrics_ != nullptr)
                metrics_->bytes_written(result.bytes);
        }

        if (pending_bytes > 0) {
            stream_waiting_for_write_progress_ = false;
            arm_deadline(stream_liveness_budget());
        }

        write_vectors_.clear();
        write_offset_ = 0;
        write_buffer_.clear();

        if (!stream_) {
            begin_close();
            return drive_close();
        }

        std::string& scratch = stream_scratch();
        scratch.clear();
        StreamSource::Pull outcome = StreamSource::Pull::Finished;
        try {
            outcome = stream_->pull(scratch);
        } catch (...) {
            outcome = StreamSource::Pull::Finished;
        }

        if (outcome == StreamSource::Pull::Finished) {
            if (!scratch.empty())
                write_buffer_.append(scratch);
            if (write_buffer_.empty()) {
                begin_close();
                return drive_close();
            }
            write_vectors_.push_back(
                iovec{const_cast<char*>(write_buffer_.read_pointer()), write_buffer_.size()});
            release_stream_source();
            continue;
        }

        if (outcome == StreamSource::Pull::Idle) {
            // Empty interest avoids spinning on a writable socket or persistent read EOF. A stream
            // notification or its liveness deadline wakes it.
            return EpollInterest::Quiescent;
        }

        // Treat a zero-byte write claim as idle.
        if (scratch.empty())
            return EpollInterest::Quiescent;
        write_buffer_.append(scratch);
        write_vectors_.push_back(
            iovec{const_cast<char*>(write_buffer_.read_pointer()), write_buffer_.size()});
    }
}

EpollInterest Connection::drive_close() {
    ERIKSLUND_HTTP_ASSERT(state_ == ConnectionState::Closing);

    std::array<char, kCloseDrainChunkBytes> scratch{};
    for (unsigned pass = 0; pass < kCloseDrainPasses; ++pass) {
        const TransportResult result = transport_read(transport_, std::span<char>(scratch));
        if (result.is_terminal())
            break;
        if (result.is_blocked() || result.bytes == 0)
            break;
    }
    return EpollInterest::Done;
}

void Connection::dispatch_and_serialize() {
    if (!pending_response_.has_value()) {
        try {
            pending_response_ = router_->dispatch(request_);
        } catch (const std::exception&) {
            // Application exceptions become generic 500 responses.
            pending_response_ =
                Response::text("Internal Server Error\n", Status::InternalServerError);
        } catch (...) {
            pending_response_ =
                Response::text("Internal Server Error\n", Status::InternalServerError);
        }

        apply_conditional_request(request_, *pending_response_);
    }

    if (pending_response_->is_stream())
        stream_ = pending_response_->stream_source();

    const Response& response = *pending_response_;
    const bool keep_alive = !response.is_stream() && request_.keep_alive() &&
                            requests_served_ + 1 < options_->max_requests_per_connection;
    const ResponseEncodingOptions encoding_options{
        .server_header = options_->server_header,
        .keep_alive = keep_alive,
        .secure = transport_is_secure(transport_),
        .strict_transport_security = listener_->tls.strict_transport_security,
        .hsts_max_age_seconds = listener_->tls.hsts_max_age_seconds,
    };
    const EncodedResponse encoded = encode_response(request_, response, encoding_options,
                                                    write_buffer_, compressed_body_);

    // Build vectors only after their backing storage has its final address.
    write_vectors_.clear();
    write_offset_ = 0;
    write_vectors_.push_back(
        iovec{const_cast<char*>(write_buffer_.read_pointer()), write_buffer_.size()});
    if (!encoded.suppress_body && !encoded.body.empty())
        write_vectors_.push_back(
            iovec{const_cast<char*>(encoded.body.data()), encoded.body.size()});

    // Once handling completes, the write timeout replaces the request budget.
    request_deadline_ = std::chrono::steady_clock::time_point{};
}

void Connection::serialize_error(Status status) {
    request_parser_.reset();
    request_.reset();
    request_.set_peer(peer_);
    request_.set_secure(transport_is_secure(transport_));
    request_.set_received_at(now());

    std::string body(reason_phrase(status));
    body.push_back('\n');
    pending_response_ = Response::text(std::move(body), status);
    dispatch_and_serialize();
}

void Connection::begin_close() noexcept {
    if (state_ == ConnectionState::Closing)
        return;
    release_stream_source();
    // Half-close to deliver FIN instead of RST. Do not stall shutdown for another TLS round trip.
    static_cast<void>(transport_shutdown(transport_));
    enter_state(ConnectionState::Closing);
}

EpollInterest Connection::on_stream_notified() {
    if (state_ != ConnectionState::Streaming)
        return state_ == ConnectionState::Closing ? EpollInterest::Done : EpollInterest::Read;
    return drive_stream();
}

EpollInterest Connection::on_deadline_expired() {
    switch (state_) {
    case ConnectionState::Handshaking:
        if (metrics_ != nullptr && transport_is_secure(transport_))
            metrics_->tls_handshake_failed();
        begin_close();
        return drive_close();

    case ConnectionState::ReadingHeaders:
    case ConnectionState::ReadingBody:
        if (!saw_any_bytes_) {
            begin_close();
            return drive_close();
        }
        serialize_error(Status::RequestTimeout);
        enter_state(ConnectionState::Writing);
        return drive_write();

    case ConnectionState::Writing:
    case ConnectionState::Streaming:
    case ConnectionState::Closing:
        begin_close();
        return drive_close();
    }
    return EpollInterest::Done;
}

void Connection::enter_state(ConnectionState next) noexcept {
    state_ = next;
    switch (next) {
    case ConnectionState::Handshaking:
        arm_deadline(options_->handshake_timeout);
        return;
    case ConnectionState::ReadingHeaders:
        arm_deadline(!saw_any_bytes_ && requests_served_ > 0 ? options_->keep_alive_idle
                                                             : options_->header_timeout);
        return;
    case ConnectionState::ReadingBody:
        arm_deadline(options_->body_timeout);
        return;
    case ConnectionState::Writing:
        arm_deadline(options_->write_timeout);
        return;
    case ConnectionState::Streaming:
        arm_deadline(stream_liveness_budget());
        return;
    case ConnectionState::Closing:
        arm_deadline(options_->write_timeout);
        return;
    }
}

void Connection::release_stream_source() noexcept {
    stream_waiting_for_write_progress_ = false;
    if (stream_) {
        stream_->on_detached();
        stream_.reset();
    }
    notifier_.reset();
}

void Connection::arm_deadline(std::chrono::milliseconds budget) noexcept {
    deadline_ = now() + budget;
    // The absolute request deadline clamps every phase.
    if (request_deadline_ != std::chrono::steady_clock::time_point{} &&
        state_ != ConnectionState::Streaming && deadline_ > request_deadline_)
        deadline_ = request_deadline_;
}

std::chrono::milliseconds Connection::stream_liveness_budget() const noexcept {
    return options_->stream_idle_timeout;
}

} // namespace erikslund::http::internal
