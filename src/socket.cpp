#include "internal/socket.hpp"

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstring>
#include <format>
#include <optional>
#include <string>
#include <string_view>

#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include "erikslund/http/contracts.hpp"
#include "erikslund/http/peer_address.hpp"
#include "erikslund/http/server.hpp"

namespace erikslund::http::internal {
namespace {

constexpr size_t kIpv4MappedMarkerOffset = 10;
constexpr size_t kIpv4AddressOffset = 12;
constexpr size_t kIpv4ByteCount = 4;
constexpr uint8_t kIpv4MappedMarker = 0xFF;

constexpr mode_t kUnixSocketMode = 0660;

constexpr uint64_t kEventFdTicket = 1;

// Mutable batch used to resume a partial writev without changing the caller's vectors.
constexpr size_t kWriteBatchCapacity = 8;

constexpr size_t kErrorTextBytes = 256;

[[nodiscard]] std::string errno_text(int error_number) {
    std::array<char, kErrorTextBytes> storage{};
    const char* text = ::strerror_r(error_number, storage.data(), storage.size());
    return text != nullptr ? std::string(text) : std::string("unknown error");
}

[[nodiscard]] bool is_ipv4_mapped(const std::array<uint8_t, kIpv6ByteCount>& bytes) noexcept {
    for (size_t index = 0; index < kIpv4MappedMarkerOffset; ++index)
        if (bytes[index] != 0)
            return false;
    return bytes[kIpv4MappedMarkerOffset] == kIpv4MappedMarker &&
           bytes[kIpv4MappedMarkerOffset + 1] == kIpv4MappedMarker;
}

[[nodiscard]] bool is_wildcard_address(std::string_view bind_address) noexcept {
    return bind_address.empty() || bind_address == "::" || bind_address == "0.0.0.0" ||
           bind_address == "*";
}

// Bind addresses are literals so DNS cannot change the selected interface at startup.
[[nodiscard]] std::optional<PeerAddress> parse_literal_address(std::string_view text) noexcept {
    std::array<char, INET6_ADDRSTRLEN + 1> literal{};
    if (text.empty() || text.size() >= literal.size())
        return std::nullopt;
    std::memcpy(literal.data(), text.data(), text.size());

    in_addr address_v4{};
    if (::inet_pton(AF_INET, literal.data(), &address_v4) == 1) {
        PeerAddress peer{};
        peer.is_v4 = true;
        peer.bytes[kIpv4MappedMarkerOffset] = kIpv4MappedMarker;
        peer.bytes[kIpv4MappedMarkerOffset + 1] = kIpv4MappedMarker;
        std::memcpy(peer.bytes.data() + kIpv4AddressOffset, &address_v4.s_addr, kIpv4ByteCount);
        return peer;
    }

    in6_addr address_v6{};
    if (::inet_pton(AF_INET6, literal.data(), &address_v6) == 1) {
        PeerAddress peer{};
        std::memcpy(peer.bytes.data(), address_v6.s6_addr, kIpv6ByteCount);
        peer.is_v4 = is_ipv4_mapped(peer.bytes);
        return peer;
    }
    return std::nullopt;
}

[[nodiscard]] int fill_bind_address(std::string_view bind_address, uint16_t port,
                                     sockaddr_storage& storage, socklen_t& length) {
    if (bind_address == "0.0.0.0") {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(port);
        std::memcpy(&storage, &address, sizeof(address));
        length = sizeof(address);
        return AF_INET;
    }

    if (is_wildcard_address(bind_address)) {
        sockaddr_in6 address{};
        address.sin6_family = AF_INET6;
        address.sin6_addr = in6addr_any;
        address.sin6_port = htons(port);
        std::memcpy(&storage, &address, sizeof(address));
        length = sizeof(address);
        return AF_INET6;
    }

    std::array<char, INET6_ADDRSTRLEN + 1> literal{};
    if (bind_address.size() >= literal.size())
        throw ServerError(std::format("bind address '{}' is not an address literal", bind_address));
    std::memcpy(literal.data(), bind_address.data(), bind_address.size());

    sockaddr_in address_v4{};
    if (::inet_pton(AF_INET, literal.data(), &address_v4.sin_addr) == 1) {
        address_v4.sin_family = AF_INET;
        address_v4.sin_port = htons(port);
        std::memcpy(&storage, &address_v4, sizeof(address_v4));
        length = sizeof(address_v4);
        return AF_INET;
    }

    sockaddr_in6 address_v6{};
    if (::inet_pton(AF_INET6, literal.data(), &address_v6.sin6_addr) == 1) {
        address_v6.sin6_family = AF_INET6;
        address_v6.sin6_port = htons(port);
        std::memcpy(&storage, &address_v6, sizeof(address_v6));
        length = sizeof(address_v6);
        return AF_INET6;
    }

    throw ServerError(std::format(
        "bind address '{}' is neither an IPv4 nor an IPv6 literal; host names are not resolved "
        "because the interface a daemon listens on must not depend on DNS",
        bind_address));
}

[[nodiscard]] UnixSocketIdentity unix_socket_identity(const struct stat& status) noexcept {
    return UnixSocketIdentity{static_cast<uint64_t>(status.st_dev),
                              static_cast<uint64_t>(status.st_ino)};
}

[[nodiscard]] bool same_unix_socket(const struct stat& status,
                                    const UnixSocketIdentity& identity) noexcept {
    return S_ISSOCK(status.st_mode) && static_cast<uint64_t>(status.st_dev) == identity.device &&
           static_cast<uint64_t>(status.st_ino) == identity.inode;
}

void unlink_owned_unix_socket(const char* path, const UnixSocketIdentity& identity) noexcept {
    struct stat current{};
    if (::lstat(path, &current) != 0 || !same_unix_socket(current, identity))
        return;
    static_cast<void>(::unlink(path));
}

void remove_stale_unix_socket(const sockaddr_un& address, socklen_t length,
                              std::string_view display_path) {
    struct stat before{};
    if (::lstat(address.sun_path, &before) != 0) {
        if (errno == ENOENT)
            return;
        throw ServerError(std::format("inspecting unix socket '{}' failed: {}", display_path,
                                      errno_text(errno)));
    }
    if (!S_ISSOCK(before.st_mode))
        throw ServerError(std::format(
            "unix socket path '{}' already exists and is not a socket", display_path));

    UniqueFd probe(::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!probe)
        throw ServerError(std::format("probing unix socket '{}' failed: {}", display_path,
                                      errno_text(errno)));

    if (::connect(probe.get(), reinterpret_cast<const sockaddr*>(&address), length) == 0)
        throw ServerError(std::format("unix socket '{}' already has a live listener", display_path));

    const int connect_error = errno;
    if (connect_error == ENOENT)
        return;
    if (connect_error != ECONNREFUSED)
        throw ServerError(std::format("unix socket '{}' may still be active: {}", display_path,
                                      errno_text(connect_error)));

    const UnixSocketIdentity expected = unix_socket_identity(before);
    struct stat after{};
    if (::lstat(address.sun_path, &after) != 0) {
        if (errno == ENOENT)
            return;
        throw ServerError(std::format("rechecking unix socket '{}' failed: {}", display_path,
                                      errno_text(errno)));
    }
    if (!same_unix_socket(after, expected))
        throw ServerError(
            std::format("unix socket '{}' changed while it was being probed", display_path));
    if (::unlink(address.sun_path) != 0 && errno != ENOENT)
        throw ServerError(std::format("removing stale socket '{}' failed: {}", display_path,
                                      errno_text(errno)));
}

} // namespace

ListenerState::~ListenerState() {
    release_unix_path();
}

void ListenerState::release_unix_path() noexcept {
    if (!unix_socket_identity.has_value() || config.unix_socket_path.empty())
        return;
    unlink_owned_unix_socket(config.unix_socket_path.c_str(), *unix_socket_identity);
    unix_socket_identity.reset();
}

AcceptResult accept_connection(int listen_fd) noexcept {
    AcceptResult result;
    sockaddr_storage address{};
    socklen_t length = sizeof(address);

    // Set both flags atomically with accept to avoid fork inheritance races.
    const int accepted = ::accept4(listen_fd, reinterpret_cast<sockaddr*>(&address), &length,
                                   SOCK_NONBLOCK | SOCK_CLOEXEC);
    if (accepted >= 0) {
        result.status = AcceptStatus::Accepted;
        result.fd = UniqueFd(accepted);
        result.peer = peer_address_from(address, length);
        return result;
    }

    switch (errno) {
    case EAGAIN:
#if EWOULDBLOCK != EAGAIN
    case EWOULDBLOCK:
#endif
        result.status = AcceptStatus::WouldBlock;
        return result;
    case EINTR:
    case ECONNABORTED:
    case EPROTO:
    case EMFILE:
    case ENFILE:
    case ENOBUFS:
    case ENOMEM:
    case EPERM:
        result.status = AcceptStatus::TransientError;
        return result;
    default:
        result.status = AcceptStatus::FatalError;
        return result;
    }
}

bool set_nonblocking(int fd) noexcept {
    const int flags = ::fcntl(fd, F_GETFL, 0);
    if (flags < 0)
        return false;
    if ((flags & O_NONBLOCK) != 0)
        return true;
    return ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) == 0;
}

bool set_tcp_nodelay(int fd) noexcept {
    const int enable = 1;
    return ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &enable, sizeof(enable)) == 0;
}

bool reuse_port_supported() noexcept {
#ifdef SO_REUSEPORT
    static const bool supported = [] {
        const UniqueFd probe(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
        if (!probe)
            return false;
        const int enable = 1;
        return ::setsockopt(probe.get(), SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(enable)) == 0;
    }();
    return supported;
#else
    return false;
#endif
}

UniqueFd bind_tcp_listener(std::string_view bind_address, uint16_t port, int backlog,
                           bool reuse_port) {
    sockaddr_storage storage{};
    socklen_t length = 0;
    const int family = fill_bind_address(bind_address, port, storage, length);

    UniqueFd listener(::socket(family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!listener)
        throw ServerError(std::format("socket() for {}:{} failed: {}", bind_address, port,
                                      errno_text(errno)));

    const int enable = 1;
    if (::setsockopt(listener.get(), SOL_SOCKET, SO_REUSEADDR, &enable, sizeof(enable)) != 0)
        throw ServerError(std::format("SO_REUSEADDR on {}:{} failed: {}", bind_address, port,
                                      errno_text(errno)));

#ifdef SO_REUSEPORT
    // A policy change after the probe falls back to the shared EPOLLEXCLUSIVE listener.
    if (reuse_port)
        static_cast<void>(
            ::setsockopt(listener.get(), SOL_SOCKET, SO_REUSEPORT, &enable, sizeof(enable)));
#endif

    if (family == AF_INET6 && is_wildcard_address(bind_address)) {
        const int v6only = 0;
        if (::setsockopt(listener.get(), IPPROTO_IPV6, IPV6_V6ONLY, &v6only, sizeof(v6only)) != 0)
            throw ServerError(std::format("clearing IPV6_V6ONLY on {}:{} failed: {}", bind_address,
                                          port, errno_text(errno)));
    }

    if (::bind(listener.get(), reinterpret_cast<const sockaddr*>(&storage), length) != 0)
        throw ServerError(
            std::format("bind {}:{} failed: {}", bind_address, port, errno_text(errno)));

    if (::listen(listener.get(), backlog) != 0)
        throw ServerError(
            std::format("listen on {}:{} failed: {}", bind_address, port, errno_text(errno)));

    return listener;
}

BoundUnixListener bind_unix_listener(std::string_view path, int backlog) {
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    if (path.empty() || path.size() >= sizeof(address.sun_path) || path.contains('\0'))
        throw ServerError(std::format("unix socket path '{}' must be 1..{} characters", path,
                                      sizeof(address.sun_path) - 1));
    std::memcpy(address.sun_path, path.data(), path.size());

    UniqueFd listener(::socket(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0));
    if (!listener)
        throw ServerError(std::format("socket(AF_UNIX) for '{}' failed: {}", path,
                                      errno_text(errno)));

    const socklen_t length =
        static_cast<socklen_t>(offsetof(sockaddr_un, sun_path) + path.size() + 1);
    if (::bind(listener.get(), reinterpret_cast<const sockaddr*>(&address), length) != 0) {
        const int bind_error = errno;
        if (bind_error != EADDRINUSE)
            throw ServerError(std::format("bind '{}' failed: {}", path, errno_text(bind_error)));
        remove_stale_unix_socket(address, length, path);
        if (::bind(listener.get(), reinterpret_cast<const sockaddr*>(&address), length) != 0)
            throw ServerError(std::format("bind '{}' after stale cleanup failed: {}", path,
                                          errno_text(errno)));
    }

    struct stat bound_status{};
    if (::lstat(address.sun_path, &bound_status) != 0) {
        const int inspect_error = errno;
        throw ServerError(std::format("inspecting bound unix socket '{}' failed: {}", path,
                                      errno_text(inspect_error)));
    }
    if (!S_ISSOCK(bound_status.st_mode))
        throw ServerError(
            std::format("bound unix socket '{}' was replaced before it could listen", path));
    const UnixSocketIdentity identity = unix_socket_identity(bound_status);

    if (::chmod(address.sun_path, kUnixSocketMode) != 0) {
        const int chmod_error = errno;
        unlink_owned_unix_socket(address.sun_path, identity);
        throw ServerError(std::format("chmod '{}' failed: {}", path, errno_text(chmod_error)));
    }

    if (::listen(listener.get(), backlog) != 0) {
        const int listen_error = errno;
        unlink_owned_unix_socket(address.sun_path, identity);
        throw ServerError(std::format("listen on '{}' failed: {}", path,
                                      errno_text(listen_error)));
    }

    return BoundUnixListener{std::move(listener), identity};
}

uint16_t resolved_port_of(int fd) {
    sockaddr_storage storage{};
    socklen_t length = sizeof(storage);
    if (::getsockname(fd, reinterpret_cast<sockaddr*>(&storage), &length) != 0)
        throw ServerError(std::format("getsockname() failed: {}", errno_text(errno)));

    if (storage.ss_family == AF_INET) {
        sockaddr_in address{};
        std::memcpy(&address, &storage, sizeof(address));
        return ntohs(address.sin_port);
    }
    if (storage.ss_family == AF_INET6) {
        sockaddr_in6 address{};
        std::memcpy(&address, &storage, sizeof(address));
        return ntohs(address.sin6_port);
    }
    return 0;
}

PeerAddress peer_address_from(const sockaddr_storage& address, socklen_t length) noexcept {
    static_cast<void>(length);
    PeerAddress peer{};

    switch (address.ss_family) {
    case AF_INET: {
        sockaddr_in source{};
        std::memcpy(&source, &address, sizeof(source));
        peer.is_v4 = true;
        peer.port = ntohs(source.sin_port);
        peer.bytes[kIpv4MappedMarkerOffset] = kIpv4MappedMarker;
        peer.bytes[kIpv4MappedMarkerOffset + 1] = kIpv4MappedMarker;
        std::memcpy(peer.bytes.data() + kIpv4AddressOffset, &source.sin_addr.s_addr,
                    kIpv4ByteCount);
        return peer;
    }
    case AF_INET6: {
        sockaddr_in6 source{};
        std::memcpy(&source, &address, sizeof(source));
        peer.port = ntohs(source.sin6_port);
        std::memcpy(peer.bytes.data(), source.sin6_addr.s6_addr, kIpv6ByteCount);
        peer.is_v4 = is_ipv4_mapped(peer.bytes);
        return peer;
    }
    case AF_UNIX:
        peer.is_unix = true;
        return peer;
    default:
        return peer;
    }
}

std::optional<PeerAddress> peer_address_of(int fd) noexcept {
    sockaddr_storage storage{};
    socklen_t length = sizeof(storage);
    if (::getpeername(fd, reinterpret_cast<sockaddr*>(&storage), &length) != 0)
        return std::nullopt;
    return peer_address_from(storage, length);
}

bool bind_address_is_local(std::string_view bind_address) noexcept {
    if (is_wildcard_address(bind_address))
        return false;
    if (bind_address == "localhost")
        return true;
    const std::optional<PeerAddress> address = parse_literal_address(bind_address);
    return address.has_value() && address->is_loopback();
}

ssize_t write_vectors(int fd, std::span<const iovec> vectors) noexcept {
    size_t total_written = 0;

    while (true) {
        std::array<iovec, kWriteBatchCapacity> batch{};
        size_t batch_size = 0;
        size_t batch_bytes = 0;
        size_t skipped = total_written;

        for (const iovec& entry : vectors) {
            if (skipped >= entry.iov_len) {
                skipped -= entry.iov_len;
                continue;
            }
            if (batch_size == batch.size())
                break;
            iovec piece = entry;
            piece.iov_base = static_cast<char*>(piece.iov_base) + skipped;
            piece.iov_len -= skipped;
            skipped = 0;
            batch_bytes += piece.iov_len;
            batch[batch_size] = piece;
            ++batch_size;
        }

        if (batch_size == 0)
            return static_cast<ssize_t>(total_written);

        const ssize_t written = ::writev(fd, batch.data(), static_cast<int>(batch_size));
        if (written < 0) {
            if (errno == EINTR)
                continue;
            if (total_written > 0)
                return static_cast<ssize_t>(total_written);
            return -1;
        }

        total_written += static_cast<size_t>(written);
        if (static_cast<size_t>(written) < batch_bytes)
            return static_cast<ssize_t>(total_written);
    }
}

UniqueFd make_event_fd() {
    UniqueFd descriptor(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    if (!descriptor)
        throw ServerError(std::format("eventfd() failed: {}", errno_text(errno)));
    return descriptor;
}

void signal_event_fd(int event_fd) noexcept {
    if (event_fd < 0)
        return;
    const uint64_t ticket = kEventFdTicket;
    while (::write(event_fd, &ticket, sizeof(ticket)) < 0) {
        if (errno == EINTR)
            continue;
        // A saturated eventfd already has a pending wakeup.
        break;
    }
}

void drain_event_fd(int event_fd) noexcept {
    ERIKSLUND_HTTP_ASSERT(event_fd >= 0);
    uint64_t counter = 0;
    while (true) {
        const ssize_t received = ::read(event_fd, &counter, sizeof(counter));
        if (received > 0)
            continue;
        if (received < 0 && errno == EINTR)
            continue;
        return;
    }
}

} // namespace erikslund::http::internal
