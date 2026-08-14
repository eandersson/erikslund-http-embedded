#pragma once

#include <functional>
#include <string>

#include "erikslund/http/build_config.hpp"
#include "erikslund/http/metrics.hpp"
#include "erikslund/http/router.hpp"
#include "erikslund/http/sse.hpp"
#include "erikslund/http/status_page.hpp"

namespace erikslund::http {

struct ObservabilityOptions {
    std::string service_name{};
    std::string version{};

    // Not owned. Null omits the metrics routes.
    MetricsRegistry* metrics = nullptr;

    // Empty means always healthy. Runs on a reactor thread.
    std::function<bool()> healthy{};

    // Called per request. Empty renders a minimal page.
    std::function<StatusPage()> status_page{};

    // Not owned. Non-null mounts /events.
    SseChannel* live = nullptr;
};

// Mounts /, /status, /health, /healthz, and /favicon.ico. Metrics and events routes are optional.
void mount_observability(Router& router, ObservabilityOptions options);

} // namespace erikslund::http
