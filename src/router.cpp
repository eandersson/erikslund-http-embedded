#include "erikslund/http/router.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <flat_map>
#include <format>
#include <functional>
#include <inplace_vector>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "erikslund/http/assets.hpp"
#include "erikslund/http/contracts.hpp"
#include "erikslund/http/method.hpp"
#include "erikslund/http/request.hpp"
#include "erikslund/http/response.hpp"
#include "erikslund/http/server.hpp"
#include "erikslund/http/status.hpp"
#include "internal/conditional_request.hpp"


namespace erikslund::http {
namespace {

constexpr std::string_view kNotFoundBody = "Not Found\n";
constexpr std::string_view kMethodNotAllowedBody = "Method Not Allowed\n";
constexpr std::string_view kNotAcceptableBody = "Not Acceptable\n";
constexpr std::string_view kInternalErrorBody = "Internal Server Error\n";
constexpr std::string_view kBadRequestBody = "Bad Request\n";

constexpr std::string_view kAcceptEncodingField = "Accept-Encoding";
constexpr std::string_view kAllowField = "Allow";
constexpr std::string_view kCacheControlField = "Cache-Control";
constexpr std::string_view kIfNoneMatchField = "If-None-Match";
constexpr std::string_view kVaryField = "Vary";

constexpr Method kGetAndHead = Method::Get | Method::Head;

constexpr int64_t kAssetMaxAgeSeconds = 31'536'000;

[[nodiscard]] std::string immutable_cache_control() {
    return std::format("public, max-age={}, immutable", kAssetMaxAgeSeconds);
}

[[nodiscard]] std::string allow_field_value(Method methods) {
    std::string value;
    for (const Method candidate : kAllMethods) {
        if (!contains(methods, candidate))
            continue;
        if (!value.empty())
            value += ", ";
        value += method_name(candidate);
    }
    return value;
}

[[nodiscard]] Response not_found_response() {
    return Response::text(std::string(kNotFoundBody), Status::NotFound);
}

[[nodiscard]] Response method_not_allowed_response(Method allowed) {
    Response response = Response::text(std::string(kMethodNotAllowedBody),
                                       Status::MethodNotAllowed);
    response.header(std::string(kAllowField), allow_field_value(allowed));
    return response;
}

[[nodiscard]] Response precompressed_asset_not_acceptable() {
    Response response =
        Response::text(std::string(kNotAcceptableBody), Status::NotAcceptable);
    response.header(std::string(kVaryField), std::string(kAcceptEncodingField));
    return response;
}

[[nodiscard]] Response internal_error_response() {
    return Response::text(std::string(kInternalErrorBody), Status::InternalServerError);
}

[[nodiscard]] Response bad_request_response() {
    return Response::text(std::string(kBadRequestBody), Status::BadRequest);
}

[[nodiscard]] Response invoke_handler(const Handler& handler, const Request& request) {
    try {
        return handler(request);
    } catch (...) {
        return internal_error_response();
    }
}

[[nodiscard]] std::optional<Response> invoke_middleware(const Middleware& middleware,
                                                        const Request& request) {
    try {
        return middleware(request);
    } catch (...) {
        return internal_error_response();
    }
}

[[nodiscard]] const std::vector<uint32_t>* find_exact(
    const std::flat_map<std::string, std::vector<uint32_t>>& index, std::string_view path) {
    const auto& patterns = index.keys();
    const auto found = std::lower_bound(patterns.begin(), patterns.end(), path,
                                        [](const std::string& left, std::string_view right) {
                                            return std::string_view(left) < right;
                                        });
    if (found == patterns.end() || std::string_view(*found) != path)
        return nullptr;
    const auto position = static_cast<size_t>(found - patterns.begin());
    return &index.values()[position];
}

[[nodiscard]] Response asset_response(const Asset& asset, const Request& request) {
    const bool is_precompressed = !asset.content_encoding.empty();
    if (is_precompressed && request.header(kAcceptEncodingField).has_value() &&
        !request.wants_gzip())
        return precompressed_asset_not_acceptable();

    const std::optional<std::string_view> if_none_match = request.header(kIfNoneMatchField);
    if (!asset.etag.empty() && if_none_match.has_value() &&
        internal::if_none_match_matches(*if_none_match, asset.etag)) {
        Response not_modified = Response::empty(Status::NotModified);
        not_modified.etag(std::string(asset.etag));
        not_modified.header(std::string(kCacheControlField), immutable_cache_control());
        if (is_precompressed)
            not_modified.header(std::string(kVaryField), std::string(kAcceptEncodingField));
        return not_modified;
    }

    Response response = Response::borrowed(asset.bytes, asset.content_type);
    if (!asset.etag.empty())
        response.etag(std::string(asset.etag));
    if (!asset.content_encoding.empty())
        response.content_encoding(std::string(asset.content_encoding));
    if (is_precompressed)
        response.header(std::string(kVaryField), std::string(kAcceptEncodingField));
    response.header(std::string(kCacheControlField), immutable_cache_control());
    return response;
}

} // namespace

Router::Router() = default;
Router::~Router() = default;

Router::Router(Router&& other) noexcept
    : routes_(std::move(other.routes_)), exact_index_(std::move(other.exact_index_)),
      dynamic_routes_(std::move(other.dynamic_routes_)), mounts_(std::move(other.mounts_)),
      middleware_(std::move(other.middleware_)), fallback_(std::move(other.fallback_)) {}

Router& Router::operator=(Router&& other) noexcept {
    if (this != &other) {
        routes_ = std::move(other.routes_);
        exact_index_ = std::move(other.exact_index_);
        dynamic_routes_ = std::move(other.dynamic_routes_);
        mounts_ = std::move(other.mounts_);
        middleware_ = std::move(other.middleware_);
        fallback_ = std::move(other.fallback_);
    }
    return *this;
}

Router& Router::route(Method methods, std::string_view pattern, Handler handler) {
    if (methods == Method::None)
        throw ServerError(std::format("route \"{}\" was registered for no method", pattern));
    if (!handler)
        throw ServerError(
            std::format("route \"{}\" was registered with an empty handler", pattern));
    if (!is_valid_route_pattern(pattern))
        throw ServerError(std::format(
            "route pattern \"{}\" is invalid: it must start with '/', every {{name}} capture must "
            "be closed and non-empty, '*' may appear only as the final character, and no segment "
            "may be empty or \".\" or \"..\" -- the request parser rejects those paths, so such a "
            "pattern could never be reached",
            pattern));

    Route compiled;
    compiled.methods = methods;
    compiled.pattern = std::string(pattern);
    compiled.handler = std::move(handler);

    size_t parameter_count = 0;
    size_t cursor = 1;
    while (cursor <= pattern.size()) {
        const size_t slash = pattern.find('/', cursor);
        const size_t length =
            (slash == std::string_view::npos) ? std::string_view::npos : slash - cursor;
        const std::string_view piece = pattern.substr(cursor, length);

        const bool is_final = slash == std::string_view::npos;
        switch (router_detail::classify_segment(piece, is_final)) {
        case router_detail::RouteSegmentKind::Wildcard:
            compiled.has_wildcard = true;
            break;
        case router_detail::RouteSegmentKind::Parameter: {
            const std::string_view capture_name = piece.substr(1, piece.size() - 2);
            Segment segment;
            segment.is_parameter = true;
            segment.parameter_name = std::string(capture_name);
            compiled.segments.push_back(std::move(segment));
            ++parameter_count;
            break;
        }
        case router_detail::RouteSegmentKind::Literal: {
            Segment segment;
            segment.literal = std::string(piece);
            compiled.segments.push_back(std::move(segment));
            break;
        }
        case router_detail::RouteSegmentKind::Invalid:
            ERIKSLUND_HTTP_ASSERT(false);
            break;
        }

        if (slash == std::string_view::npos)
            break;
        cursor = slash + 1;
    }

    if (parameter_count > kMaxPathParameters)
        throw ServerError(std::format(
            "route pattern \"{}\" declares {} captures, but the per-request parameter table holds "
            "{}",
            pattern, parameter_count, kMaxPathParameters));

    const bool is_dynamic = compiled.has_wildcard || parameter_count > 0;
    if (!is_dynamic)
        compiled.segments.clear();

    const auto reject_overlap = [&](Method existing_methods) {
        if ((existing_methods & methods) == Method::None)
            return;
        throw ServerError(std::format(
            "route \"{}\" is already registered for at least one of these methods; the second "
            "registration could never be reached",
            pattern));
    };
    if (is_dynamic) {
        for (const uint32_t index : dynamic_routes_) {
            const Route& existing = routes_[static_cast<size_t>(index)];
            if (existing.pattern == compiled.pattern)
                reject_overlap(existing.methods);
        }
    } else if (exact_index_.contains(compiled.pattern)) {
        for (const uint32_t index : exact_index_.at(compiled.pattern))
            reject_overlap(routes_[static_cast<size_t>(index)].methods);
    }

    const auto route_index = static_cast<uint32_t>(routes_.size());
    std::string exact_key = is_dynamic ? std::string{} : compiled.pattern;
    routes_.push_back(std::move(compiled));
    if (is_dynamic)
        dynamic_routes_.push_back(route_index);
    else
        exact_index_[std::move(exact_key)].push_back(route_index);
    return *this;
}

Router& Router::get(std::string_view pattern, Handler handler) {
    return route(kGetAndHead, pattern, std::move(handler));
}

Router& Router::post(std::string_view pattern, Handler handler) {
    return route(Method::Post, pattern, std::move(handler));
}

Router& Router::put(std::string_view pattern, Handler handler) {
    return route(Method::Put, pattern, std::move(handler));
}

Router& Router::patch(std::string_view pattern, Handler handler) {
    return route(Method::Patch, pattern, std::move(handler));
}

Router& Router::del(std::string_view pattern, Handler handler) {
    return route(Method::Delete, pattern, std::move(handler));
}

Router& Router::options(std::string_view pattern, Handler handler) {
    return route(Method::Options, pattern, std::move(handler));
}

Router& Router::mount(std::string_view prefix, const AssetBundle& bundle) {
    if (prefix.empty() || prefix.front() != '/')
        throw ServerError(std::format("mount prefix \"{}\" must start with '/'", prefix));
    if (prefix.contains('*') || prefix.contains('{') || prefix.contains('}'))
        throw ServerError(
            std::format("mount prefix \"{}\" is a literal path, not a route pattern", prefix));
    if (!is_valid_route_pattern(prefix))
        throw ServerError(std::format(
            "mount prefix \"{}\" holds a segment no request path can carry: no segment may be "
            "empty or \".\" or \"..\"",
            prefix));

    Mount asset_mount;
    asset_mount.prefix = std::string(prefix);
    if (!asset_mount.prefix.ends_with('/'))
        asset_mount.prefix += '/';
    asset_mount.bundle = bundle;
    mounts_.push_back(std::move(asset_mount));
    return *this;
}

Router& Router::use(Middleware middleware) {
    if (!middleware)
        throw ServerError("Router::use() was given an empty middleware");
    middleware_.push_back(std::move(middleware));
    return *this;
}

Router& Router::fallback(Handler handler) {
    if (!handler)
        throw ServerError("Router::fallback() was given an empty handler");
    if (fallback_)
        throw ServerError("Router::fallback() was already set; there is only one");
    fallback_ = std::move(handler);
    return *this;
}

Response Router::dispatch(Request& request) const {
    if (request.method() == Method::None)
        return bad_request_response();

    request.clear_path_parameters();

    for (const Middleware& middleware : middleware_) {
        std::optional<Response> short_circuit = invoke_middleware(middleware, request);
        if (short_circuit.has_value())
            return std::move(*short_circuit);
    }

    const std::string_view path = request.path();
    const Method method = request.method();

    Method allowed = Method::None;

    using Captures = std::inplace_vector<HeaderView, kMaxPathParameters>;
    const auto match = [path](const Route& candidate) -> std::optional<Captures> {
        Captures captures;
        std::string_view remaining = path;
        for (const Segment& segment : candidate.segments) {
            if (remaining.empty() || remaining.front() != '/')
                return std::nullopt;
            remaining.remove_prefix(1);
            const size_t slash = remaining.find('/');
            const std::string_view piece = remaining.substr(0, slash);
            if (segment.is_parameter) {
                if (piece.empty())
                    return std::nullopt;
                captures.push_back(HeaderView{segment.parameter_name, piece});
            } else if (piece != segment.literal) {
                return std::nullopt;
            }
            remaining = (slash == std::string_view::npos) ? std::string_view{}
                                                          : remaining.substr(slash);
        }
        if (candidate.has_wildcard)
            return !remaining.empty() && remaining.front() == '/' ? std::optional{captures}
                                                                   : std::nullopt;
        return remaining.empty() ? std::optional{captures} : std::nullopt;
    };

    const auto invoke_route = [&request](const Route& route, const Captures& captures) {
        for (const HeaderView& capture : captures)
            request.bind_path_parameter(capture.name, capture.value);
        return invoke_handler(route.handler, request);
    };

    if (const std::vector<uint32_t>* exact = find_exact(exact_index_, path)) {
        for (const uint32_t index : *exact) {
            const Route& candidate = routes_[static_cast<size_t>(index)];
            if (contains(candidate.methods, method))
                return invoke_route(candidate, Captures{});
            allowed |= candidate.methods;
        }
    }

    for (const uint32_t index : dynamic_routes_) {
        const Route& candidate = routes_[static_cast<size_t>(index)];
        const std::optional<Captures> captures = match(candidate);
        if (!captures.has_value())
            continue;
        if (contains(candidate.methods, method))
            return invoke_route(candidate, *captures);
        allowed |= candidate.methods;
    }

    for (const Mount& asset_mount : mounts_) {
        if (!path.starts_with(asset_mount.prefix))
            continue;
        const Asset* asset = asset_mount.bundle.find(path);
        const std::string_view relative = path.substr(asset_mount.prefix.size() - 1);
        if (asset == nullptr && relative != path)
            asset = asset_mount.bundle.find(relative);
        if (asset == nullptr)
            continue;
        if (!contains(kGetAndHead, method)) {
            allowed |= kGetAndHead;
            continue;
        }
        return asset_response(*asset, request);
    }

    if (allowed != Method::None)
        return method_not_allowed_response(allowed);
    if (fallback_)
        return invoke_handler(fallback_, request);
    return not_found_response();
}

std::vector<std::string> Router::paths() const {
    std::vector<std::string> patterns;
    patterns.reserve(routes_.size() + mounts_.size());
    for (const Route& entry : routes_)
        patterns.push_back(entry.pattern);
    for (const Mount& asset_mount : mounts_)
        patterns.push_back(asset_mount.prefix + '*');
    std::ranges::sort(patterns);
    const auto duplicates = std::ranges::unique(patterns);
    patterns.erase(duplicates.begin(), duplicates.end());
    return patterns;
}

} // namespace erikslund::http
