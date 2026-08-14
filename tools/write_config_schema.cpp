
#include <cstdio>
#include <print>

#include "config_schema.hpp"

int main() {
    std::print("{}", erikslund::http::tools::config_json_schema());
    return 0;
}
