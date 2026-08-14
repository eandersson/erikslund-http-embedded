#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace erikslund::http {

enum class State : uint8_t { Ok, Warn, Bad };

[[nodiscard]] constexpr std::string_view state_class(State state) noexcept {
    switch (state) {
    case State::Ok:
        return "ok";
    case State::Warn:
        return "warn";
    case State::Bad:
        return "bad";
    }
    return "ok";
}

inline constexpr int kDefaultRefreshSeconds = 5;

inline constexpr std::string_view kDefaultEventsPath = "/events";

inline constexpr std::string_view kStatusPageStyle =
    R"CSS(body{font-family:system-ui,sans-serif;margin:2rem auto;max-width:46rem;color:#222}
h1{font-size:1.4rem;margin-bottom:.2rem} small{color:#888;font-weight:400}
table{border-collapse:collapse;width:100%;margin-top:1rem}
td{padding:.3rem .8rem;border-bottom:1px solid #e5e5e5;vertical-align:top}
td:first-child{color:#777;width:14rem} .ok{color:#0a7d28} .bad{color:#c0392b}
.warn{color:#b8860b} a{color:#2563eb;text-decoration:none})CSS";

// CSP hash for the exact inline live-update script emitted by render().
[[nodiscard]] std::string_view live_update_script_csp_source() noexcept;

// Escapes all text and exposes no raw-HTML API. Assign fluent chains to values, not references.
class StatusPage {
public:
    StatusPage(std::string service_name, std::string version);

    auto&& pid(this auto&& self, int process_id) {
        self.set_pid(process_id);
        return std::forward<decltype(self)>(self);
    }

    // Zero disables meta refresh.
    auto&& refresh_seconds(this auto&& self, int seconds) {
        self.set_refresh_seconds(seconds);
        return std::forward<decltype(self)>(self);
    }

    auto&& state(this auto&& self, std::string headline, State level) {
        self.set_state(std::move(headline), level);
        return std::forward<decltype(self)>(self);
    }

    auto&& row(this auto&& self, std::string_view label, std::string_view value) {
        self.add_row(label, value, State::Ok, false);
        return std::forward<decltype(self)>(self);
    }

    auto&& row(this auto&& self, std::string_view label, std::string_view value, State level) {
        self.add_row(label, value, level, true);
        return std::forward<decltype(self)>(self);
    }

    auto&& section(this auto&& self, std::string_view heading) {
        self.add_section(heading);
        return std::forward<decltype(self)>(self);
    }

    // Unsafe URLs render as inert text.
    auto&& link(this auto&& self, std::string_view href, std::string_view text) {
        self.add_link(href, text);
        return std::forward<decltype(self)>(self);
    }

    auto&& live_updates(this auto&& self, bool enabled) {
        self.set_live_updates(enabled);
        return std::forward<decltype(self)>(self);
    }

    [[nodiscard]] std::string render() const;

private:
    enum class ElementKind : uint8_t { Row, Section };

    struct Element {
        ElementKind kind = ElementKind::Row;
        std::string label;
        std::string value;
        State level = State::Ok;
        bool show_level_color = false;
    };

    struct Link {
        std::string href;
        std::string text;
    };

    void set_pid(int process_id);
    void set_refresh_seconds(int seconds);
    void set_state(std::string headline, State level);
    void add_row(std::string_view label, std::string_view value, State level,
                 bool show_level_color);
    void add_section(std::string_view heading);
    void add_link(std::string_view href, std::string_view text);
    void set_live_updates(bool enabled);

    std::string service_name_;
    std::string version_;
    std::string state_headline_;
    State state_level_ = State::Ok;
    std::vector<Element> elements_;
    std::vector<Link> links_;
    int process_id_ = 0;
    int refresh_seconds_ = kDefaultRefreshSeconds;
    bool live_updates_ = false;
};

} // namespace erikslund::http
