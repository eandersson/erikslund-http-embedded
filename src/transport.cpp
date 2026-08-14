#include "internal/transport.hpp"

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>

#include <sys/uio.h>

namespace erikslund::http::internal {
namespace {

static_assert(std::is_nothrow_move_constructible_v<PlainTransport>);
static_assert(std::is_nothrow_move_constructible_v<TlsTransport>);

template <typename Visitor, typename TransportRef>
// NOLINTNEXTLINE(bugprone-exception-escape)
[[nodiscard]] decltype(auto) visit_transport(TransportRef&& transport, Visitor&& visitor) noexcept {
    return std::visit(std::forward<Visitor>(visitor), std::forward<TransportRef>(transport));
}

} // namespace

TransportResult transport_handshake(Transport& transport) noexcept {
    return visit_transport(transport, [](auto& active) { return active.handshake(); });
}

TransportResult transport_read(Transport& transport, std::span<char> out) noexcept {
    return visit_transport(transport, [out](auto& active) { return active.read(out); });
}

TransportResult transport_write(Transport& transport, std::span<const char> in) noexcept {
    return visit_transport(transport, [in](auto& active) { return active.write(in); });
}

TransportResult transport_writev(Transport& transport, std::span<const iovec> vectors) noexcept {
    return visit_transport(transport, [vectors](auto& active) { return active.writev(vectors); });
}

TransportResult transport_shutdown(Transport& transport) noexcept {
    return visit_transport(transport, [](auto& active) { return active.shutdown(); });
}

bool transport_is_secure(const Transport& transport) noexcept {
    return visit_transport(transport, [](const auto& active) { return active.is_secure(); });
}

std::string_view transport_alpn_selected(const Transport& transport) noexcept {
    return visit_transport(transport, [](const auto& active) { return active.alpn_selected(); });
}

std::optional<std::string> transport_peer_certificate_subject(const Transport& transport) {
    return visit_transport(transport,
                           [](const auto& active) { return active.peer_certificate_subject(); });
}

int transport_fd(const Transport& transport) noexcept {
    return visit_transport(transport, [](const auto& active) { return active.fd(); });
}

} // namespace erikslund::http::internal
