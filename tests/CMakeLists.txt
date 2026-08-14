
set(ERIKSLUND_HTTP_TEST_TIMEOUT_SECONDS 600)

# CONFIGURE_DEPENDS prevents a new test file from being silently omitted.
file(GLOB ERIKSLUND_HTTP_TEST_SOURCES CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
if(NOT ERIKSLUND_HTTP_TEST_SOURCES)
    message(FATAL_ERROR
        "No test sources found under tests/. An executable with no doctest translation unit links "
        "and then reports zero assertions, which is indistinguishable from a suite that passed.")
endif()

find_path(ERIKSLUND_HTTP_DOCTEST_INCLUDE_DIR NAMES doctest/doctest.h)
if(NOT ERIKSLUND_HTTP_DOCTEST_INCLUDE_DIR)
    message(FATAL_ERROR
        "doctest/doctest.h was not found. Install it (Debian: apt-get install doctest-dev), or "
        "configure with -DERIKSLUND_HTTP_BUILD_TESTS=OFF.")
endif()

add_executable(erikslund_http_tests ${ERIKSLUND_HTTP_TEST_SOURCES})
target_link_libraries(erikslund_http_tests PRIVATE erikslund_http)

target_include_directories(erikslund_http_tests SYSTEM PRIVATE
                           ${ERIKSLUND_HTTP_DOCTEST_INCLUDE_DIR})

target_include_directories(
    erikslund_http_tests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    # Tests intentionally exercise private state-machine components.
    ${PROJECT_SOURCE_DIR}/src)

# Fixtures live in the read-only source mount, outside the build tree.
target_compile_definitions(erikslund_http_tests PRIVATE
                           ERIKSLUND_HTTP_TEST_SOURCE_DIR="${PROJECT_SOURCE_DIR}")

target_compile_options(erikslund_http_tests PRIVATE -Wall -Wextra -Wno-maybe-uninitialized)

# doctest macros legitimately shadow their enclosing case's locals.
target_compile_options(erikslund_http_tests PRIVATE -Wno-shadow)

if(ERIKSLUND_HTTP_CONTRACTS)
    # Record a violated test contract without terminating the remaining suites.
    target_compile_options(erikslund_http_tests PRIVATE -fcontract-evaluation-semantic=observe)

    target_compile_definitions(erikslund_http_tests PRIVATE
                               ERIKSLUND_HTTP_TEST_CONTRACTS_OBSERVED=1)
endif()

if(ERIKSLUND_HTTP_TLS_01)
    if(NOT TARGET OpenSSL::SSL)
        find_package(OpenSSL 3.0 REQUIRED COMPONENTS SSL Crypto)
    endif()
    target_link_libraries(erikslund_http_tests PRIVATE OpenSSL::SSL OpenSSL::Crypto)

    include(${CMAKE_CURRENT_SOURCE_DIR}/support/test_certificates.cmake)
    erikslund_http_add_test_certificates(erikslund_http_tests)
endif()

target_link_options(erikslund_http_tests PRIVATE -rdynamic)

add_test(NAME unit COMMAND erikslund_http_tests)
set_tests_properties(unit PROPERTIES TIMEOUT ${ERIKSLUND_HTTP_TEST_TIMEOUT_SECONDS})
