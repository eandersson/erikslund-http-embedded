set(ERIKSLUND_HTTP_EMBED_ASSETS_MODULE "${CMAKE_CURRENT_LIST_FILE}")

set(ERIKSLUND_HTTP_EMBED_BYTES_PER_LINE 1024)

set(ERIKSLUND_HTTP_EMBED_ETAG_HEX_DIGITS 16)


function(_erikslund_http_hex_to_literal hex out_variable)
    math(EXPR chunk_hex_length "${ERIKSLUND_HTTP_EMBED_BYTES_PER_LINE} * 2")
    string(LENGTH "${hex}" hex_length)
    set(lines "")
    set(offset 0)
    while(offset LESS hex_length)
        math(EXPR remaining "${hex_length} - ${offset}")
        if(remaining LESS chunk_hex_length)
            set(take ${remaining})
        else()
            set(take ${chunk_hex_length})
        endif()
        string(SUBSTRING "${hex}" ${offset} ${take} chunk)
        string(REGEX REPLACE "(..)" "\\\\x\\1" escaped "${chunk}")
        string(APPEND lines "    \"${escaped}\"\n")
        math(EXPR offset "${offset} + ${take}")
    endwhile()
    set(${out_variable} "${lines}" PARENT_SCOPE)
endfunction()

function(_erikslund_http_escape_cxx_string text out_variable)
    string(REPLACE "\\" "\\\\" escaped "${text}")
    string(REPLACE "\"" "\\\"" escaped "${escaped}")
    set(${out_variable} "${escaped}" PARENT_SCOPE)
endfunction()

function(_erikslund_http_asset_etag path out_variable)
    file(MD5 "${path}" digest)
    string(SUBSTRING "${digest}" 0 ${ERIKSLUND_HTTP_EMBED_ETAG_HEX_DIGITS} short_digest)
    set(${out_variable} "W/\\\"${short_digest}\\\"" PARENT_SCOPE)
endfunction()

function(_erikslund_http_write_asset_source)
    file(STRINGS "${EMBED_MANIFEST}" manifest_entries)

    set(body "")
    set(table "")
    set(index 0)
    set(total_bytes 0)

    foreach(entry IN LISTS manifest_entries)
        string(REPLACE "\t" ";" parts "${entry}")
        list(GET parts 0 url_path)
        list(GET parts 1 content_encoding)
        list(GET parts 2 relative_path)
        if(content_encoding STREQUAL "identity")
            set(content_encoding "")
        endif()

        set(absolute_path "${EMBED_DIR}/${relative_path}")
        file(SIZE "${absolute_path}" byte_count)
        math(EXPR total_bytes "${total_bytes} + ${byte_count}")
        _erikslund_http_asset_etag("${absolute_path}" etag)
        _erikslund_http_escape_cxx_string("${url_path}" url_literal)

        string(APPEND body "\n// ${relative_path} -- ${byte_count} bytes\n")
        if(byte_count EQUAL 0)
            string(APPEND body "constexpr std::string_view kAssetBytes${index}{};\n")
        elseif(EMBED_USE_EMBED)
            string(APPEND body
"consteval auto embed_asset_${index}() {
    constexpr unsigned char raw[] = {
    };
    std::array<char, sizeof(raw)> characters{};
    for (std::size_t byte_index = 0; byte_index < sizeof(raw); ++byte_index)
        characters[byte_index] = static_cast<char>(raw[byte_index]);
    return characters;
}
constexpr auto kAssetData${index} = embed_asset_${index}();
constexpr std::string_view kAssetBytes${index}{kAssetData${index}.data(),
                                               kAssetData${index}.size()};
")
        else()
            file(READ "${absolute_path}" hex_dump HEX)
            _erikslund_http_hex_to_literal("${hex_dump}" literal_lines)
            string(APPEND body "constexpr std::string_view kAssetBytes${index}{\n")
            string(APPEND body "${literal_lines}")
            string(APPEND body "    , ${byte_count}};\n")
        endif()

        string(APPEND table
               "    Asset{\"${url_literal}\", content_type_for_extension(\"${url_literal}\"),\n")
        string(APPEND table
               "          kAssetBytes${index}, \"${etag}\", \"${content_encoding}\"},\n")
        math(EXPR index "${index} + 1")
    endforeach()

    if(EMBED_USE_EMBED)
        set(strategy "#embed directives")
    else()
        set(strategy "escaped byte literals")
    endif()

    file(WRITE "${EMBED_OUTPUT_HPP}"
"#pragma once
// Generated from ${EMBED_DIR}; do not edit.

#include <span>

#include \"erikslund/http/assets.hpp\"

namespace erikslund::http::generated {

// Views reference static storage.
[[nodiscard]] std::span<const Asset> ${EMBED_VARIABLE}() noexcept;

} // namespace erikslund::http::generated
")

    file(WRITE "${EMBED_OUTPUT_CPP}"
"// Generated from ${EMBED_DIR}; do not edit. ${index} assets, ${total_bytes} bytes via ${strategy}.
// #embed avoids parsing one integer literal per byte; consteval converts its unsigned bytes once.

#include \"${EMBED_VARIABLE}.hpp\"

#include <array>
#include <cstddef>
#include <span>
#include <string_view>

#include \"erikslund/http/assets.hpp\"

namespace erikslund::http::generated {
namespace {

${body}
// Sorted for AssetBundle::find().
constexpr std::array<Asset, ${index}> kAssets{{
${table}}};

} // namespace

std::span<const Asset> ${EMBED_VARIABLE}() noexcept {
    return kAssets;
}

} // namespace erikslund::http::generated
")
endfunction()


function(_erikslund_http_detect_embed out_variable)
    if(NOT DEFINED ERIKSLUND_HTTP_HAS_EMBED)
        set(probe_directory "${CMAKE_BINARY_DIR}/erikslund_http_embed")
        set(probe_resource "${probe_directory}/embed_probe.bin")
        file(WRITE "${probe_resource}" "erikslund")
        if(NOT CMAKE_CXX_STANDARD OR CMAKE_CXX_STANDARD LESS 26)
            set(CMAKE_CXX_STANDARD 26)
            set(CMAKE_CXX_STANDARD_REQUIRED ON)
        endif()
        include(CheckCXXSourceCompiles)
        check_cxx_source_compiles("
#ifndef __has_embed
#  error this compiler has no __has_embed
#endif
#if !__has_embed(\"${probe_resource}\")
#  error the probe resource was not found
#endif
static constexpr unsigned char kProbe[] = {
};
static_assert(sizeof(kProbe) == 9);
int main() { return 0; }
" ERIKSLUND_HTTP_HAS_EMBED)
    endif()
    set(${out_variable} "${ERIKSLUND_HTTP_HAS_EMBED}" PARENT_SCOPE)
endfunction()

function(erikslund_http_embed_assets)
    cmake_parse_arguments(PARSE_ARGV 0 arg "" "TARGET;DIR;VARIABLE;PREFIX" "")

    if(NOT arg_TARGET)
        message(FATAL_ERROR "erikslund_http_embed_assets: TARGET is required.")
    endif()
    if(NOT TARGET ${arg_TARGET})
        message(FATAL_ERROR
                "erikslund_http_embed_assets: TARGET '${arg_TARGET}' is not an existing target. "
                "Call this after the add_executable/add_library that creates it.")
    endif()
    if(NOT arg_VARIABLE MATCHES "^[A-Za-z_][A-Za-z0-9_]*$")
        message(FATAL_ERROR
                "erikslund_http_embed_assets: VARIABLE must be a C++ identifier, got "
                "'${arg_VARIABLE}'. It becomes a function name in the generated source.")
    endif()
    if(NOT arg_DIR)
        message(FATAL_ERROR "erikslund_http_embed_assets: DIR is required.")
    endif()
    get_filename_component(asset_directory "${arg_DIR}" ABSOLUTE)
    if(NOT IS_DIRECTORY "${asset_directory}")
        message(FATAL_ERROR "erikslund_http_embed_assets: DIR '${arg_DIR}' is not a directory.")
    endif()

    set(url_prefix "${arg_PREFIX}")
    if(NOT url_prefix)
        set(url_prefix "/assets")
    endif()
    if(NOT url_prefix MATCHES "^/")
        message(FATAL_ERROR
                "erikslund_http_embed_assets: PREFIX must start with '/', got '${url_prefix}'.")
    endif()
    string(REGEX REPLACE "/+$" "" url_prefix "${url_prefix}")

    file(GLOB_RECURSE relative_paths RELATIVE "${asset_directory}" CONFIGURE_DEPENDS
         "${asset_directory}/*")
    if(NOT relative_paths)
        message(FATAL_ERROR
                "erikslund_http_embed_assets: no files under '${asset_directory}'. An empty bundle "
                "is a mistyped path far more often than an intended state, so this is an error "
                "rather than an empty table that 404s at runtime.")
    endif()

    set(manifest_entries "")
    set(absolute_paths "")
    foreach(relative_path IN LISTS relative_paths)
        set(content_encoding "identity")
        set(url_path "${url_prefix}/${relative_path}")
        if(relative_path MATCHES "\\.gz$")
            string(REGEX REPLACE "\\.gz$" "" uncompressed "${relative_path}")
            if(NOT "${uncompressed}" IN_LIST relative_paths)
                set(content_encoding "gzip")
                set(url_path "${url_prefix}/${uncompressed}")
            endif()
        endif()
        list(APPEND manifest_entries "${url_path}\t${content_encoding}\t${relative_path}")
        list(APPEND absolute_paths "${asset_directory}/${relative_path}")
    endforeach()
    list(SORT manifest_entries)

    set(generated_root "${CMAKE_CURRENT_BINARY_DIR}/erikslund_http_embed/${arg_TARGET}")
    set(generated_cpp "${generated_root}/generated/${arg_VARIABLE}.cpp")
    set(generated_hpp "${generated_root}/generated/${arg_VARIABLE}.hpp")
    set(manifest "${generated_root}/${arg_VARIABLE}.manifest")

    string(REPLACE ";" "\n" manifest_text "${manifest_entries}")
    string(APPEND manifest_text "\n")
    set(existing_manifest "")
    if(EXISTS "${manifest}")
        file(READ "${manifest}" existing_manifest)
    endif()
    if(NOT existing_manifest STREQUAL manifest_text)
        file(WRITE "${manifest}" "${manifest_text}")
    endif()

    _erikslund_http_detect_embed(use_embed)
    if(NOT use_embed)
        set(use_embed 0)
    endif()

    list(LENGTH relative_paths asset_count)
    add_custom_command(
        OUTPUT "${generated_cpp}" "${generated_hpp}"
        COMMAND "${CMAKE_COMMAND}"
                -DERIKSLUND_HTTP_EMBED_GENERATE=1
                "-DEMBED_MANIFEST=${manifest}"
                "-DEMBED_DIR=${asset_directory}"
                "-DEMBED_VARIABLE=${arg_VARIABLE}"
                "-DEMBED_PREFIX=${url_prefix}"
                "-DEMBED_USE_EMBED=${use_embed}"
                "-DEMBED_OUTPUT_CPP=${generated_cpp}"
                "-DEMBED_OUTPUT_HPP=${generated_hpp}"
                -P "${ERIKSLUND_HTTP_EMBED_ASSETS_MODULE}"
        DEPENDS ${absolute_paths} "${manifest}" "${ERIKSLUND_HTTP_EMBED_ASSETS_MODULE}"
        COMMENT "erikslund-http: embedding ${asset_count} assets from ${asset_directory}"
        VERBATIM)

    target_sources(${arg_TARGET} PRIVATE "${generated_cpp}")
    target_include_directories(${arg_TARGET} PRIVATE "${generated_root}")

    add_custom_target(${arg_TARGET}_${arg_VARIABLE}_assets DEPENDS "${generated_cpp}"
                                                                   "${generated_hpp}")
    add_dependencies(${arg_TARGET} ${arg_TARGET}_${arg_VARIABLE}_assets)
endfunction()

if(CMAKE_SCRIPT_MODE_FILE AND ERIKSLUND_HTTP_EMBED_GENERATE)
    _erikslund_http_write_asset_source()
endif()
