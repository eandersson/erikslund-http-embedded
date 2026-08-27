# CMake 3.25 is the first release that accepts CXX_STANDARD 26.
cmake_minimum_required(VERSION 3.31)
project(erikslund_http VERSION 0.1.2 LANGUAGES CXX)

list(APPEND CMAKE_MODULE_PATH ${CMAKE_CURRENT_SOURCE_DIR}/cmake)
include(CMakePackageConfigHelpers)
include(CheckPIESupported)
include(GNUInstallDirs)
include(FetchContent)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(ERIKSLUND_HTTP_TOP_LEVEL ON)
else()
    set(ERIKSLUND_HTTP_TOP_LEVEL OFF)
endif()

set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

# Do not choose a build type when consumed through add_subdirectory().
if(ERIKSLUND_HTTP_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Debug CACHE STRING "" FORCE)
endif()

# AUTO disables a missing dependency; explicit ON treats it as an error.
set(ERIKSLUND_HTTP_TLS "AUTO" CACHE STRING "TLS 1.3 via OpenSSL 3 (AUTO/ON/OFF)")
set_property(CACHE ERIKSLUND_HTTP_TLS PROPERTY STRINGS AUTO ON OFF)

set(ERIKSLUND_HTTP_ZLIB "AUTO" CACHE STRING "gzip response encoding via zlib (AUTO/ON/OFF)")
set_property(CACHE ERIKSLUND_HTTP_ZLIB PROPERTY STRINGS AUTO ON OFF)

option(ERIKSLUND_HTTP_CONTRACTS "P2900 contracts (-fcontracts)" ON)

set(ERIKSLUND_HTTP_CONTRACT_SEMANTIC "AUTO" CACHE STRING
    "P2900 evaluation semantic (AUTO/ignore/observe/enforce/quick_enforce)")
set_property(CACHE ERIKSLUND_HTTP_CONTRACT_SEMANTIC PROPERTY STRINGS
             AUTO ignore observe enforce quick_enforce)

option(ERIKSLUND_HTTP_REFLECTION "P2996 reflection (-freflection), enables reflect.hpp" ON)

option(ERIKSLUND_HTTP_STACKTRACE "std::stacktrace in the jthread catch handlers" ON)

check_pie_supported(OUTPUT_VARIABLE ERIKSLUND_HTTP_PIE_ERROR LANGUAGES CXX)
if(NOT CMAKE_CXX_LINK_PIE_SUPPORTED)
    message(FATAL_ERROR "PIE support is required: ${ERIKSLUND_HTTP_PIE_ERROR}")
endif()

option(ERIKSLUND_HTTP_MODULE "Additionally build the erikslund.http C++ module" OFF)

option(NATIVE_ARCH "Tune for the build machine's CPU (-march=native -mtune=native)" OFF)

option(ERIKSLUND_HTTP_BUILD_BENCH "Build the header-block scan benchmark" OFF)

option(ERIKSLUND_HTTP_BUILD_TESTS "Build the doctest suite" ${ERIKSLUND_HTTP_TOP_LEVEL})
option(ERIKSLUND_HTTP_BUILD_EXAMPLES "Build the examples" ${ERIKSLUND_HTTP_TOP_LEVEL})

option(ERIKSLUND_HTTP_BUILD_TOOLS "Build the configuration-schema generator" ${ERIKSLUND_HTTP_TOP_LEVEL})
option(ERIKSLUND_HTTP_INSTALL "Generate install and package-config rules" ${ERIKSLUND_HTTP_TOP_LEVEL})

function(erikslund_http_validate_tristate name value)
    if(NOT "${value}" STREQUAL "AUTO" AND NOT "${value}" STREQUAL "ON" AND
       NOT "${value}" STREQUAL "OFF")
        message(FATAL_ERROR
            "${name}=${value} is invalid; expected exactly one of AUTO, ON, or OFF.")
    endif()
endfunction()

erikslund_http_validate_tristate(ERIKSLUND_HTTP_TLS "${ERIKSLUND_HTTP_TLS}")
erikslund_http_validate_tristate(ERIKSLUND_HTTP_ZLIB "${ERIKSLUND_HTTP_ZLIB}")

function(erikslund_http_resolve_tristate value found human_name out_enabled out_reason)
    if("${value}" STREQUAL "OFF")
        set(${out_enabled} 0 PARENT_SCOPE)
        set(${out_reason} "disabled" PARENT_SCOPE)
    elseif(found)
        set(${out_enabled} 1 PARENT_SCOPE)
        set(${out_reason} "found" PARENT_SCOPE)
    elseif("${value}" STREQUAL "ON")
        message(FATAL_ERROR
            "ERIKSLUND_HTTP_${human_name}=ON but the dependency was not found. "
            "Install it, or set the option to AUTO to degrade silently.")
    else()
        set(${out_enabled} 0 PARENT_SCOPE)
        set(${out_reason} "not found" PARENT_SCOPE)
    endif()
endfunction()

find_package(Threads REQUIRED)

set(ERIKSLUND_HTTP_OPENSSL_VERSION "")
set(_openssl_found OFF)
if(NOT ERIKSLUND_HTTP_TLS STREQUAL "OFF")
    find_package(OpenSSL 3.0 QUIET COMPONENTS SSL Crypto)
    if(OpenSSL_FOUND)
        set(_openssl_found ON)
        set(ERIKSLUND_HTTP_OPENSSL_VERSION "${OPENSSL_VERSION}")
    endif()
endif()
erikslund_http_resolve_tristate("${ERIKSLUND_HTTP_TLS}" ${_openssl_found} "TLS"
                                ERIKSLUND_HTTP_TLS_01 ERIKSLUND_HTTP_TLS_REASON)

# Prefer an installed package, then headers, then the pinned source release.
set(ERIKSLUND_HTTP_GLAZE_VERSION "")
set(_glaze_found OFF)
set(ERIKSLUND_HTTP_GLAZE_VIA "none")
find_package(glaze CONFIG QUIET)
if(glaze_FOUND)
    set(_glaze_found ON)
    set(ERIKSLUND_HTTP_GLAZE_VIA "package")
    set(ERIKSLUND_HTTP_GLAZE_VERSION "${glaze_VERSION}")
else()
    find_path(ERIKSLUND_HTTP_GLAZE_INCLUDE_DIR NAMES glaze/glaze.hpp)
    if(ERIKSLUND_HTTP_GLAZE_INCLUDE_DIR)
        set(_glaze_found ON)
        set(ERIKSLUND_HTTP_GLAZE_VIA "headers")
        set(ERIKSLUND_HTTP_GLAZE_VERSION "headers at ${ERIKSLUND_HTTP_GLAZE_INCLUDE_DIR}")
    else()
        FetchContent_Declare(
            glaze
            URL https://codeload.github.com/stephenberry/glaze/tar.gz/refs/tags/v8.2.0
            URL_HASH SHA256=dcc1f107cfa3be4094d306bc44b32907ae1331a0557809c16332e643678fe1df
            DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
        FetchContent_MakeAvailable(glaze)
        if(TARGET glaze::glaze)
            set(_glaze_found ON)
            set(ERIKSLUND_HTTP_GLAZE_VIA "fetched")
            set(ERIKSLUND_HTTP_GLAZE_VERSION "8.2.0 (fetched)")
        endif()
    endif()
endif()
if(NOT _glaze_found)
    message(FATAL_ERROR "Glaze is required but could not be resolved")
endif()

set(_zlib_found OFF)
if(NOT ERIKSLUND_HTTP_ZLIB STREQUAL "OFF")
    find_package(ZLIB QUIET)
    if(ZLIB_FOUND)
        set(_zlib_found ON)
    endif()
endif()
erikslund_http_resolve_tristate("${ERIKSLUND_HTTP_ZLIB}" ${_zlib_found} "ZLIB"
                                ERIKSLUND_HTTP_ZLIB_01 ERIKSLUND_HTTP_ZLIB_REASON)

if(ERIKSLUND_HTTP_GLAZE_VIA STREQUAL "package")
    set(ERIKSLUND_HTTP_GLAZE_FIND_DEPENDENCY 1)
else()
    set(ERIKSLUND_HTTP_GLAZE_FIND_DEPENDENCY 0)
endif()

if(ERIKSLUND_HTTP_CONTRACTS)
    set(ERIKSLUND_HTTP_CONTRACTS_01 1)
else()
    set(ERIKSLUND_HTTP_CONTRACTS_01 0)
endif()
if(ERIKSLUND_HTTP_REFLECTION)
    set(ERIKSLUND_HTTP_REFLECTION_01 1)
else()
    set(ERIKSLUND_HTTP_REFLECTION_01 0)
endif()
if(ERIKSLUND_HTTP_STACKTRACE)
    set(ERIKSLUND_HTTP_STACKTRACE_01 1)
else()
    set(ERIKSLUND_HTTP_STACKTRACE_01 0)
endif()

if(ERIKSLUND_HTTP_MODULE)
    message(FATAL_ERROR
        "ERIKSLUND_HTTP_MODULE=ON is not supported by this toolchain. Module scanning requires the "
        "Ninja generator (the Make generator refuses it), and CMake cannot discover `import std` "
        "support for GCC. Build the header path; see docs/ for the module plan.")
endif()

set(ERIKSLUND_HTTP_GENERATED_INCLUDE ${CMAKE_CURRENT_BINARY_DIR}/generated/include)
# A generated header gives library and consumer code the same feature switches.
configure_file(
    ${CMAKE_CURRENT_SOURCE_DIR}/include/erikslund/http/build_config.hpp.in
    ${ERIKSLUND_HTTP_GENERATED_INCLUDE}/erikslund/http/build_config.hpp
    @ONLY)

file(GLOB_RECURSE ERIKSLUND_HTTP_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/src/*.cpp)
if(NOT ERIKSLUND_HTTP_SOURCES)
    message(FATAL_ERROR
        "No implementation sources found under src/. An empty static archive would link and then "
        "fail at every call site, so the build refuses to produce one.")
endif()

add_library(erikslund_http STATIC ${ERIKSLUND_HTTP_SOURCES})
add_library(erikslund::http ALIAS erikslund_http)

target_include_directories(
    erikslund_http
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
           $<BUILD_INTERFACE:${ERIKSLUND_HTTP_GENERATED_INCLUDE}>
           $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(erikslund_http PUBLIC Threads::Threads)
if(ERIKSLUND_HTTP_STACKTRACE)
    target_link_libraries(erikslund_http PUBLIC stdc++exp)
endif()

# Public headers require C++26 too.
target_compile_features(erikslund_http PUBLIC cxx_std_26)

# GCC 16 misdiagnoses std::optional in nested designated initializers.
target_compile_options(erikslund_http PRIVATE -Wall -Wextra -Wshadow -Wno-maybe-uninitialized)

target_compile_options(
    erikslund_http PRIVATE
    $<$<CONFIG:Release>:-O3;-flto;-fno-plt;-fno-semantic-interposition;-funroll-loops;-fomit-frame-pointer;-falign-functions=32;-falign-loops=32;-ffunction-sections;-fdata-sections;-pipe>)
target_link_options(
    erikslund_http INTERFACE
    $<$<CONFIG:Release>:-flto=auto;-Wl,-O3;-Wl,--as-needed;-Wl,--gc-sections>)
if(NATIVE_ARCH)
    target_compile_options(erikslund_http PRIVATE -march=native -mtune=native)
endif()

target_compile_options(erikslund_http PRIVATE -fhardened -ftrivial-auto-var-init=zero)
target_link_options(erikslund_http INTERFACE -Wl,-z,relro -Wl,-z,now)

# _FORTIFY_SOURCE requires optimization; keep the level private to this library.
target_compile_options(
    erikslund_http PRIVATE
    $<$<OR:$<CONFIG:Debug>,$<STREQUAL:$<CONFIG>,>>:-Og>)

if(ERIKSLUND_HTTP_CONTRACTS)
    # Headers need contract syntax, but the library alone owns its evaluation policy.
    target_compile_options(erikslund_http PUBLIC -fcontracts)

    set(ERIKSLUND_HTTP_CONTRACT_SEMANTICS AUTO ignore observe enforce quick_enforce)
    if(NOT ERIKSLUND_HTTP_CONTRACT_SEMANTIC IN_LIST ERIKSLUND_HTTP_CONTRACT_SEMANTICS)
        message(FATAL_ERROR
            "ERIKSLUND_HTTP_CONTRACT_SEMANTIC=${ERIKSLUND_HTTP_CONTRACT_SEMANTIC} is not one of "
            "${ERIKSLUND_HTTP_CONTRACT_SEMANTICS}. Refusing here, because GCC would reject the "
            "flag with a message naming neither the option nor what set it.")
    endif()

    if(ERIKSLUND_HTTP_CONTRACT_SEMANTIC STREQUAL "AUTO")
        # Debug builds stop at bugs; deployed builds report them without terminating.
        set(_semantic_debug enforce)
        set(_semantic_optimised observe)
        set(_optimised_configuration
            $<OR:$<CONFIG:Release>,$<CONFIG:RelWithDebInfo>,$<CONFIG:MinSizeRel>>)
        set(_resolved_contract_semantic
            $<IF:${_optimised_configuration},${_semantic_optimised},${_semantic_debug}>)

        if(CMAKE_CONFIGURATION_TYPES)
            set(ERIKSLUND_HTTP_CONTRACT_SEMANTIC_SUMMARY
                "per configuration: ${_semantic_debug} for Debug, "
                "${_semantic_optimised} for the optimised configurations")
        elseif(CMAKE_BUILD_TYPE MATCHES "^(Release|RelWithDebInfo|MinSizeRel)$")
            set(ERIKSLUND_HTTP_CONTRACT_SEMANTIC_SUMMARY
                "${_semantic_optimised} (AUTO, a daemon must not abort)")
        else()
            set(ERIKSLUND_HTTP_CONTRACT_SEMANTIC_SUMMARY
                "${_semantic_debug} (AUTO, a violation should be loud here)")
        endif()
    else()
        set(_resolved_contract_semantic ${ERIKSLUND_HTTP_CONTRACT_SEMANTIC})
        set(ERIKSLUND_HTTP_CONTRACT_SEMANTIC_SUMMARY
            "${ERIKSLUND_HTTP_CONTRACT_SEMANTIC} (explicit, every configuration)")
    endif()

    target_compile_options(erikslund_http PRIVATE
                           -fcontract-evaluation-semantic=${_resolved_contract_semantic})
else()
    set(ERIKSLUND_HTTP_CONTRACT_SEMANTIC_SUMMARY "n/a (contracts off)")
endif()
if(ERIKSLUND_HTTP_REFLECTION)
    # The pinned clang-tidy cannot parse translation units that include <meta>.
    target_compile_options(erikslund_http PUBLIC -freflection)
endif()

if(ERIKSLUND_HTTP_TLS_01)
    target_link_libraries(erikslund_http PRIVATE OpenSSL::SSL OpenSSL::Crypto)
endif()
if(ERIKSLUND_HTTP_ZLIB_01)
    target_link_libraries(erikslund_http PRIVATE ZLIB::ZLIB)
endif()
if(ERIKSLUND_HTTP_GLAZE_VIA STREQUAL "package")
    target_link_libraries(erikslund_http PUBLIC glaze::glaze)
elseif(ERIKSLUND_HTTP_GLAZE_VIA STREQUAL "headers")
    target_include_directories(erikslund_http SYSTEM PUBLIC
                               $<BUILD_INTERFACE:${ERIKSLUND_HTTP_GLAZE_INCLUDE_DIR}>)
else()
    target_link_libraries(erikslund_http PUBLIC $<BUILD_INTERFACE:glaze::glaze>)
endif()

if(ERIKSLUND_HTTP_BUILD_TESTS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tests/CMakeLists.txt)
    enable_testing()
    add_subdirectory(tests)
endif()
if(ERIKSLUND_HTTP_BUILD_EXAMPLES AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/examples/CMakeLists.txt)
    add_subdirectory(examples)
endif()
if(ERIKSLUND_HTTP_BUILD_TESTS AND ERIKSLUND_HTTP_BUILD_EXAMPLES)
    find_program(ERIKSLUND_HTTP_READELF NAMES readelf llvm-readelf REQUIRED)
    add_test(
        NAME executable_hardening
        COMMAND ${CMAKE_COMMAND}
                -DREAD_ELF=${ERIKSLUND_HTTP_READELF}
                -DBINARY=$<TARGET_FILE:status_server>
                -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/VerifyElfHardening.cmake)
endif()
if(ERIKSLUND_HTTP_BUILD_TOOLS AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tools/CMakeLists.txt)
    add_subdirectory(tools)
endif()
if(ERIKSLUND_HTTP_BUILD_BENCH AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/bench/CMakeLists.txt)
    add_subdirectory(bench)
endif()

if(ERIKSLUND_HTTP_INSTALL)
    # Preserve the erikslund::http alias in the installed package.
    set_target_properties(erikslund_http PROPERTIES EXPORT_NAME http)

    install(TARGETS erikslund_http
            EXPORT erikslund-http-targets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR})

    install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
            FILES_MATCHING PATTERN "*.hpp")
    install(FILES ${ERIKSLUND_HTTP_GENERATED_INCLUDE}/erikslund/http/build_config.hpp
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/erikslund/http)

    install(EXPORT erikslund-http-targets
            FILE erikslund-http-targets.cmake
            NAMESPACE erikslund::
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/erikslund-http)

    configure_package_config_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/erikslund-http-config.cmake.in
        ${CMAKE_CURRENT_BINARY_DIR}/erikslund-http-config.cmake
        INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/erikslund-http)

    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/erikslund-http-config-version.cmake
        VERSION ${PROJECT_VERSION}
        COMPATIBILITY SameMajorVersion)

    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/erikslund-http-config.cmake
                  ${CMAKE_CURRENT_BINARY_DIR}/erikslund-http-config-version.cmake
            DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/erikslund-http)

    if(NOT ERIKSLUND_HTTP_GLAZE_FIND_DEPENDENCY)
        # An installed consumer must find every promised public header.
        include(CheckCXXSourceCompiles)
        block()
            set(CMAKE_REQUIRED_INCLUDES "")
            set(CMAKE_REQUIRED_QUIET ON)
            check_cxx_source_compiles(
                "#if !__has_include(<glaze/glaze.hpp>)
                 #error Glaze is not on the default include path
                 #endif
                 int main() {}"
                ERIKSLUND_HTTP_GLAZE_ON_DEFAULT_INCLUDE_PATH)
        endblock()
        if(NOT ERIKSLUND_HTTP_GLAZE_ON_DEFAULT_INCLUDE_PATH)
            message(FATAL_ERROR
                "Glaze was resolved from ${ERIKSLUND_HTTP_GLAZE_VIA} rather than from a package, "
                "and <glaze/glaze.hpp> is not on the compiler's default include path. This build "
                "cannot install a usable package. Install Glaze's CMake package, put its headers "
                "on the default search path, or configure with ERIKSLUND_HTTP_INSTALL=OFF.")
        endif()
    endif()
endif()

if(ERIKSLUND_HTTP_TLS_01)
    set(_summary_tls "ON (OpenSSL ${ERIKSLUND_HTTP_OPENSSL_VERSION})")
else()
    set(_summary_tls "OFF (${ERIKSLUND_HTTP_TLS_REASON})")
endif()
set(_summary_glaze "required (Glaze ${ERIKSLUND_HTTP_GLAZE_VERSION})")
if(ERIKSLUND_HTTP_ZLIB_01)
    set(_summary_zlib "ON")
else()
    set(_summary_zlib "OFF (${ERIKSLUND_HTTP_ZLIB_REASON})")
endif()

message(STATUS "erikslund-http ${PROJECT_VERSION}")
message(STATUS "  TLS ................ ${_summary_tls}")
message(STATUS "  Glaze .............. ${_summary_glaze}")
message(STATUS "  zlib ............... ${_summary_zlib}")
message(STATUS "  contracts .......... ${ERIKSLUND_HTTP_CONTRACTS} (propagated INTERFACE when ON)")
message(STATUS "  contract semantic .. ${ERIKSLUND_HTTP_CONTRACT_SEMANTIC_SUMMARY}")
message(STATUS "  reflection ......... ${ERIKSLUND_HTTP_REFLECTION} (reflect.hpp cannot be linted)")
message(STATUS "  stacktrace ......... ${ERIKSLUND_HTTP_STACKTRACE}")
message(STATUS "  hardening .......... required (PIE, RELRO, immediate binding)")
message(STATUS "  build type ......... ${CMAKE_BUILD_TYPE}")
