
add_executable(erikslund_http_write_config_schema
               ${CMAKE_CURRENT_SOURCE_DIR}/config_schema.cpp
               ${CMAKE_CURRENT_SOURCE_DIR}/write_config_schema.cpp)

target_link_libraries(erikslund_http_write_config_schema PRIVATE erikslund::http)

target_include_directories(erikslund_http_write_config_schema PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

target_compile_options(erikslund_http_write_config_schema PRIVATE
                       -Wall -Wextra -Wshadow -Wno-maybe-uninitialized)
