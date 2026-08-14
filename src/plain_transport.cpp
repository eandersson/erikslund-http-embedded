#include "internal/transport.hpp"

#include <cerrno>
#include <cstddef>
#include <span>

#include <sys/socket.h>
#include <sys/uio.h>

#include "erikslund/http/contracts.hpp"
#include "internal/socket.hpp"

namespace erikslund::http::internal {
namespace {

[[nodiscard]] bool errno_means_would_block(int value) noexcept {
    return value == EAGAIN || value == EWOULDBLOCK;
}

} // namespace

TransportResult PlainTransport::handshake() noexcept {
    return TransportResult::ok(0);
}

TransportResult PlainTransport::read(std::span<char> out) noexcept {
    if (out.empty())
        return TransportResult::ok(0);
    while (true) {
        const ssize_t received = ::recv(fd_, out.data(), out.size(), 0);
        if (received > 0)
            return TransportResult::ok(static_cast<size_t>(received));
        if (received == 0)
            return TransportResult::closed();
        if (errno == EINTR)
            continue;
        if (errno_means_would_block(errno))
            return TransportResult::want_read();
        return TransportResult::error();
    }
}

TransportResult PlainTransport::write(std::span<const char> in) noexcept {
    if (in.empty())
        return TransportResult::ok(0);
    while (true) {
        const ssize_t sent = ::send(fd_, in.data(), in.size(), MSG_NOSIGNAL);
        if (sent >= 0)
            return TransportResult::ok(static_cast<size_t>(sent));
        if (errno == EINTR)
            continue;
        if (errno_means_would_block(errno))
            return TransportResult::want_write();
        return TransportResult::error();
    }
}

TransportResult PlainTransport::writev(std::span<const iovec> vectors) noexcept {
    ERIKSLUND_HTTP_ASSERT(vectors.size() <= kMaxWriteVectors);
    if (vectors.empty())
        return TransportResult::ok(0);
    const ssize_t sent = write_vectors(fd_, vectors);
    if (sent >= 0)
        return TransportResult::ok(static_cast<size_t>(sent));
    if (errno_means_would_block(errno))
        return TransportResult::want_write();
    return TransportResult::error();
}

TransportResult PlainTransport::shutdown() noexcept {
    if (::shutdown(fd_, SHUT_WR) == 0 || errno == ENOTCONN)
        return TransportResult::ok(0);
    if (errno_means_would_block(errno))
        return TransportResult::want_write();
    return TransportResult::error();
}

} // namespace erikslund::http::internal
