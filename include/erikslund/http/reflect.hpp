#pragma once
// P2996 reflection for status rows and gauges. Include directly: clang-tidy cannot parse <meta>,
// so this header is deliberately absent from http.hpp.

#include <format>
#include <string>
#include <string_view>
#include <type_traits>

#include "erikslund/http/build_config.hpp"
#include "erikslund/http/metrics.hpp"
#include "erikslund/http/status_page.hpp"

#if ERIKSLUND_HTTP_REFLECTION
#include <meta>
#endif

namespace erikslund::http {

#if ERIKSLUND_HTTP_REFLECTION

namespace reflect_detail {

[[nodiscard]] inline std::string humanize_identifier(std::string_view identifier) {
    std::string label(identifier);
    for (char& character : label)
        if (character == '_')
            character = ' ';
    return label;
}

template <class Aggregate>
consteval auto declared_members() {
    return std::define_static_array(
        std::meta::nonstatic_data_members_of(^^Aggregate, std::meta::access_context::current()));
}

} // namespace reflect_detail

// Adds one row per formattable member in declaration order.
template <class Snapshot>
StatusPage& add_rows_from(StatusPage& page, const Snapshot& snapshot) {
    // Expansion statements require a static constant range.
    static constexpr auto members = reflect_detail::declared_members<Snapshot>();
    template for (constexpr auto member : members) {
        page.row(reflect_detail::humanize_identifier(std::meta::identifier_of(member)),
                 std::format("{}", snapshot.[:member:]));
    }
    return page;
}

// Registers numeric members as gauges. The producer runs once per member while scraping.
template <class Snapshot, class Producer>
void register_gauges_from(MetricsRegistry& registry, std::string_view help, Producer producer) {
    static constexpr auto members = reflect_detail::declared_members<Snapshot>();
    template for (constexpr auto member : members) {
        using MemberType = [:std::meta::type_of(member):];
        if constexpr (std::is_arithmetic_v<MemberType>) {
            registry.gauge_fn(std::string(std::meta::identifier_of(member)), std::string(help),
                              [producer] {
                                  return static_cast<double>(producer().[:member:]);
                              });
        }
    }
}

#endif // ERIKSLUND_HTTP_REFLECTION

} // namespace erikslund::http
