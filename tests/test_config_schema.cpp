
#define ERIKSLUND_HTTP_JSON_SCHEMA 1

#include <filesystem>
#include <fstream>
#include <ios>
#include <iterator>
#include <string>
#include <string_view>

#include <doctest/doctest.h>

#include <glaze/glaze.hpp>
#include <glaze/json/minify.hpp>

#include "erikslund/http/config.hpp"
#include "erikslund/http/json.hpp"
#include "support/conf_files.hpp"

namespace erikslund::http {

namespace {

constexpr std::string_view kSchemaFileName = "http.schema.json";

constexpr std::string_view kRegenerate =
    "conf/http.schema.json is GENERATED from ServerConfig and must not be hand-edited. Run "
    "scripts/generate-config-schema.sh and commit the result.";

[[nodiscard]] std::string read_whole_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    REQUIRE_MESSAGE(stream.good(), "the committed schema must be readable");
    return std::string(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
}

} // namespace

TEST_CASE("the committed schema still describes the current ServerConfig") {
    const std::filesystem::path schema_file = test::conf_file_path(kSchemaFileName);
    REQUIRE_MESSAGE(!schema_file.empty(),
                    "conf/http.schema.json was not found from any known root");

    std::string committed = read_whole_file(schema_file);
    std::string generated = json_schema_for<ServerConfig>();
    REQUIRE_FALSE(generated.empty());

    CHECK_MESSAGE(glz::minify_json(committed) == glz::minify_json(generated), kRegenerate);
}

TEST_CASE("the committed schema refuses an unknown key the way the loader does") {
    const std::filesystem::path schema_file = test::conf_file_path(kSchemaFileName);
    REQUIRE_FALSE(schema_file.empty());
    const std::string committed = read_whole_file(schema_file);

    CHECK_MESSAGE(committed.find(R"("additionalProperties": false)") != std::string::npos,
                  kRegenerate);
}

TEST_CASE("the committed schema is a text file the way every other file in the tree is") {
    const std::filesystem::path schema_file = test::conf_file_path(kSchemaFileName);
    REQUIRE_FALSE(schema_file.empty());
    const std::string committed = read_whole_file(schema_file);

    REQUIRE_FALSE(committed.empty());
    CHECK(committed.back() == '\n');
    CHECK_MESSAGE(committed.find('\r') == std::string::npos,
                  "a CR would come from a Windows checkout; .gitattributes pins LF");
}

} // namespace erikslund::http
