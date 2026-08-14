#pragma once

#include <sys/socket.h>
#include <sys/uio.h>

#include <chrono>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "erikslund/http/peer_address.hpp"
#include "erikslund/http/server.hpp"
#include "internal/tls_context.hpp"
#include "internal/unique_fd.hpp"

namespace erikslund::http::internal {

// Prevents persistent accept errors from becoming a hot loop.
inline constexpr std::chrono::milliseconds kAcceptErrorBackoff{50};

struct UnixSocketIdentity {
    uint64_t device = 0;
    uint64_t inode = 0;
};

struct BoundUnixListener {
    UniqueFd fd;
    UnixSocketIdentity identity;
};

// Server-owned shared listener state. Everything except tls is immutable after start().
struct ListenerState {
    ListenerState() = default;
    ~ListenerState();
    ListenerState(const ListenerState&) = delete("owns listening sockets and a Unix pathname");
    ListenerState& operator=(const ListenerState&) = delete("owns listening sockets");

    Listener config;

    TlsContextSlot tls;

    uint16_t resolved_port = 0;

    bool per_reactor_sockets = false;

    bool is_unix = false;

    // Prevents cleanup from unlinking a replacement socket.
    std::optional<UnixSocketIdentity> unix_socket_identity;

    std::vector<UniqueFd> sockets;

    void release_unix_path() noexcept;
};

enum class AcceptStatus : uint8_t {
    Accepted,
    WouldBlock,
    TransientError,
    FatalError,
};

struct AcceptResult {
    AcceptStatus status = AcceptStatus::FatalError;
    UniqueFd fd;
    PeerAddress peer;
};

[[nodiscard]] AcceptResult accept_connection(int listen_fd) noexcept;

[[nodiscard]] bool set_nonblocking(int fd) noexcept;
[[nodiscard]] bool set_tcp_nodelay(int fd) noexcept;

[[nodiscard]] bool reuse_port_supported() noexcept;

// Binds dual-stack for "::". Failures throw ServerError.
[[nodiscard]] UniqueFd bind_tcp_listener(std::string_view bind_address, uint16_t port, int backlog,
                                         bool reuse_port);

// Reclaims only stale socket paths and returns their identity for safe cleanup.
[[nodiscard]] BoundUnixListener bind_unix_listener(std::string_view path, int backlog);

[[nodiscard]] uint16_t resolved_port_of(int fd);

[[nodiscard]] PeerAddress peer_address_from(const sockaddr_storage& address,
                                            socklen_t length) noexcept;

[[nodiscard]] std::optional<PeerAddress> peer_address_of(int fd) noexcept;

[[nodiscard]] bool bind_address_is_local(std::string_view bind_address) noexcept;

// Retries EINTR and otherwise preserves errno.
[[nodiscard]] ssize_t write_vectors(int fd, std::span<const iovec> vectors) noexcept;

[[nodiscard]] UniqueFd make_event_fd();

// Thread-safe cross-thread reactor wakeup.
void signal_event_fd(int event_fd) noexcept;

void drain_event_fd(int event_fd) noexcept;

} // namespace erikslund::http::internal
