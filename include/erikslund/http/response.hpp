#pragma once

#include <chrono>
#include <cstdint>
#include <flat_map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>

#include "erikslund/http/status.hpp"

namespace erikslund::http {

// The only connection-adjacent API callable off the owning reactor thread. It only wakes that
// reactor and becomes a no-op after detach.
class StreamNotifier {
public:
    StreamNotifier() = default;
    virtual ~StreamNotifier() = default;
    StreamNotifier(const StreamNotifier&) = delete("a notifier is bound to one connection");
    StreamNotifier& operator=(const StreamNotifier&) = delete("a notifier is bound to one connection");

    virtual void notify() noexcept = 0;
};

class StreamSource {
public:
    enum class Pull : uint8_t {
        Wrote,
        Idle,
        Finished,
    };

    StreamSource() = default;
    virtual ~StreamSource() = default;
    StreamSource(const StreamSource&) = delete("a stream source is bound to one connection");
    StreamSource& operator=(const StreamSource&) = delete("a stream source is bound to one connection");

    // Read once before the first pull; invalid values are omitted.
    [[nodiscard]] virtual std::string_view content_type() const noexcept = 0;

    // Called serially on the owning reactor. Appends to out and never clears it.
    [[nodiscard]] virtual Pull pull(std::string& out) = 0;

    virtual void on_attached(std::shared_ptr<StreamNotifier> notifier) = 0;

    // Called once on the reactor thread, even when on_attached() was never called. The source must
    // stop using its notifier before returning.
    virtual void on_detached() noexcept = 0;
};

// One value per field name, with deterministic iteration order.
using HeaderMap = std::flat_map<std::string, std::string>;

// A borrowed body must not point into the Response's owned string.
using ResponseBody = std::variant<std::string, std::string_view>;

// Assign fluent chains to values, not references.
class Response {
public:
    Response();

    [[nodiscard]] static Response text(std::string body, Status status = Status::Ok);
    [[nodiscard]] static Response html(std::string body, Status status = Status::Ok);
    [[nodiscard]] static Response json(std::string body, Status status = Status::Ok);
    [[nodiscard]] static Response prometheus(std::string body);
    // Body must outlive the response.
    [[nodiscard]] static Response borrowed(std::string_view body, std::string_view content_type,
                                           Status status = Status::Ok);
    [[nodiscard]] static Response empty(Status status);
    // An empty location omits the Location header.
    [[nodiscard]] static Response redirect(std::string location, Status status = Status::Found);
    [[nodiscard]] static Response stream(std::shared_ptr<StreamSource> source);

    // Replaces an existing field. Invalid names or values are logged and ignored.
    auto&& header(this auto&& self, std::string name, std::string value) {
        self.set_header(std::move(name), std::move(value));
        return std::forward<decltype(self)>(self);
    }

    auto&& etag(this auto&& self, std::string value) {
        self.set_header("ETag", std::move(value));
        return std::forward<decltype(self)>(self);
    }

    auto&& etag_from_body(this auto&& self) {
        self.apply_etag_from_body();
        return std::forward<decltype(self)>(self);
    }

    // Negative durations become max-age=0.
    auto&& cache_for(this auto&& self, std::chrono::seconds max_age) {
        self.apply_cache_for(max_age);
        return std::forward<decltype(self)>(self);
    }

    auto&& no_store(this auto&& self) {
        self.set_header("Cache-Control", "no-store");
        return std::forward<decltype(self)>(self);
    }

    auto&& content_encoding(this auto&& self, std::string value) {
        self.set_header("Content-Encoding", std::move(value));
        return std::forward<decltype(self)>(self);
    }

    [[nodiscard]] std::string_view body() const noexcept;
    [[nodiscard]] Status status() const noexcept { return status_; }
    [[nodiscard]] const HeaderMap& headers() const noexcept { return headers_; }
    [[nodiscard]] std::optional<std::string_view> find_header(std::string_view name) const noexcept;
    [[nodiscard]] bool is_stream() const noexcept { return stream_ != nullptr; }
    [[nodiscard]] const std::shared_ptr<StreamSource>& stream_source() const noexcept {
        return stream_;
    }

private:
    void set_header(std::string name, std::string value);
    void apply_etag_from_body();
    void apply_cache_for(std::chrono::seconds max_age);

    Status status_ = Status::Ok;
    ResponseBody body_;
    HeaderMap headers_;
    std::shared_ptr<StreamSource> stream_;
};

} // namespace erikslund::http
