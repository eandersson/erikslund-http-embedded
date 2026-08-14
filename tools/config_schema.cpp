#define ERIKSLUND_HTTP_JSON_SCHEMA 1

#include "config_schema.hpp"

#include <string>

#include <glaze/glaze.hpp>
#include <glaze/json/prettify.hpp>

#include "erikslund/http/config.hpp"
#include "erikslund/http/json.hpp"

namespace erikslund::http::tools {

std::string config_json_schema() {
    std::string schema = glz::prettify_json(json_schema_for<ServerConfig>());

    schema.push_back('\n');
    return schema;
}

} // namespace erikslund::http::tools
