#pragma once


#include <filesystem>
#include <string_view>
#include <system_error>
#include <vector>

namespace erikslund::http::test {

[[nodiscard]] inline std::filesystem::path conf_file_path(std::string_view file_name) {
    std::vector<std::filesystem::path> candidates;
    candidates.push_back(std::filesystem::path(__FILE__).parent_path().parent_path().parent_path() /
                         "conf" / file_name);
#ifdef ERIKSLUND_HTTP_TEST_SOURCE_DIR
    candidates.push_back(std::filesystem::path(ERIKSLUND_HTTP_TEST_SOURCE_DIR) / "conf" /
                         file_name);
#endif
    candidates.push_back(std::filesystem::path("/src/conf") / file_name);
    candidates.push_back(std::filesystem::path("conf") / file_name);
    candidates.push_back(std::filesystem::path("../conf") / file_name);
    candidates.push_back(std::filesystem::path("../../conf") / file_name);

    for (const std::filesystem::path& candidate : candidates) {
        std::error_code ignored;
        if (std::filesystem::is_regular_file(candidate, ignored))
            return candidate;
    }
    return {};
}

} // namespace erikslund::http::test
