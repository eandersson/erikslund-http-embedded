#pragma once
// Register routes before Server::start(). Dispatch is lock-free and concurrent; mutation after
// start is a data race.

#include <cstdint>
#include <flat_map>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "erikslund/http/assets.hpp"
#include "erikslund/http/method.hpp"
#include "erikslund/http/request.hpp"
#include "erikslund/http/response.hpp"

namespace erikslund::http {

// Handlers may own move-only state and must be callable through a const Router.
using Handler = std::move_only_function<Response(const Request&) const>;

// A response short-circuits; nullopt continues. Runs in registration order before routing.
using Middleware = std::move_only_function<std::optional<Response>(const Request&) const>;

namespace router_detail {

enum class RouteSegmentKind : uint8_t { Literal, Parameter, Wildcard, Invalid };

[[nodiscard]] constexpr bool is_reachable_segment(std::string_view segment,
                                                  bool is_final_segment) noexcept {
    if (segment.empty())
        return is_final_segment;
    return segment != "." && segment != "..";
}

[[nodiscard]] constexpr RouteSegmentKind classify_segment(std::string_view segment,
                                                          bool is_final_segment) noexcept {
    if (!is_reachable_segment(segment, is_final_segment))
        return RouteSegmentKind::Invalid;
    if (segment == "*")
        return is_final_segment ? RouteSegmentKind::Wildcard : RouteSegmentKind::Invalid;
    if (segment.contains('*'))
        return RouteSegmentKind::Invalid;

    const bool contains_brace = segment.contains('{') || segment.contains('}');
    if (!contains_brace)
        return RouteSegmentKind::Literal;
    if (segment.size() < 3 || !segment.starts_with('{') || !segment.ends_with('}'))
        return RouteSegmentKind::Invalid;

    const std::string_view name = segment.substr(1, segment.size() - 2);
    if (name.empty() || name.contains('{') || name.contains('}'))
        return RouteSegmentKind::Invalid;
    return RouteSegmentKind::Parameter;
}

} // namespace router_detail

// Supports exact paths, whole-segment {parameters}, and a trailing wildcard.
[[nodiscard]] constexpr bool is_valid_route_pattern(std::string_view pattern) noexcept {
    if (pattern.empty() || pattern.front() != '/')
        return false;

    size_t parameter_count = 0;
    size_t cursor = 1;
    while (cursor <= pattern.size()) {
        const size_t slash = pattern.find('/', cursor);
        const bool is_final = slash == std::string_view::npos;
        const size_t length = is_final ? std::string_view::npos : slash - cursor;
        switch (router_detail::classify_segment(pattern.substr(cursor, length), is_final)) {
        case router_detail::RouteSegmentKind::Parameter:
            ++parameter_count;
            if (parameter_count > kMaxPathParameters)
                return false;
            break;
        case router_detail::RouteSegmentKind::Wildcard:
        case router_detail::RouteSegmentKind::Literal:
            break;
        case router_detail::RouteSegmentKind::Invalid:
            return false;
        }
        if (is_final)
            break;
        cursor = slash + 1;
    }
    return true;
}

static_assert(is_valid_route_pattern("/metrics"));
static_assert(is_valid_route_pattern("/stats/client/{address}"));
static_assert(is_valid_route_pattern("/assets/*"));
static_assert(!is_valid_route_pattern("/assets/*/icon"));
static_assert(!is_valid_route_pattern("/stats/{}"));
static_assert(!is_valid_route_pattern("/asset*"));
static_assert(!is_valid_route_pattern("/user-{id}"));
static_assert(!is_valid_route_pattern("metrics"));

static_assert(is_valid_route_pattern("/"));
static_assert(is_valid_route_pattern("/stats/"));

static_assert(!is_valid_route_pattern("//"));
static_assert(!is_valid_route_pattern("/a//b"));
static_assert(!is_valid_route_pattern("/a/./b"));
static_assert(!is_valid_route_pattern("/a/."));
static_assert(!is_valid_route_pattern("/a/../b"));
static_assert(!is_valid_route_pattern("/.."));

class Router {
public:
    Router();
    ~Router();
    Router(const Router&) = delete("handlers are move-only; move the Router into the Server");
    Router& operator=(const Router&) = delete("handlers are move-only");
    Router(Router&&) noexcept;
    Router& operator=(Router&&) noexcept;

    // Throws ServerError for an invalid pattern.
    Router& route(Method methods, std::string_view pattern, Handler handler);

    // Registers GET and HEAD together.
    Router& get(std::string_view pattern, Handler handler);
    Router& post(std::string_view pattern, Handler handler);
    Router& put(std::string_view pattern, Handler handler);
    Router& patch(std::string_view pattern, Handler handler);
    Router& del(std::string_view pattern, Handler handler);
    Router& options(std::string_view pattern, Handler handler);

    // Assets must outlive the Router. Explicitly refused encodings produce 406.
    Router& mount(std::string_view prefix, const AssetBundle& bundle);

    Router& use(Middleware middleware);

    // Without a fallback, unmatched paths return 404.
    Router& fallback(Handler handler);

    // Defined in json.hpp to keep Glaze out of this header.
    template <class Producer>
    Router& json_get(std::string_view pattern, Producer producer);

    // Resolves middleware, exact routes, dynamic routes, mounts, then fallback. Method mismatches
    // return 405 with Allow.
    [[nodiscard]] Response dispatch(Request& request) const;

    // Returns every registered pattern, sorted.
    [[nodiscard]] std::vector<std::string> paths() const;

private:
    struct Segment {
        std::string literal;
        std::string parameter_name;
        bool is_parameter = false;
    };

    struct Route {
        Method methods = Method::None;
        std::string pattern;
        std::vector<Segment> segments;
        bool has_wildcard = false;
        Handler handler;
    };

    struct Mount {
        std::string prefix;
        AssetBundle bundle;
    };

    std::vector<Route> routes_;

    // Literal pattern to route indices.
    std::flat_map<std::string, std::vector<uint32_t>> exact_index_;

    // Registration order is match precedence.
    std::vector<uint32_t> dynamic_routes_;

    std::vector<Mount> mounts_;
    std::vector<Middleware> middleware_;
    Handler fallback_;
};

} // namespace erikslund::http
