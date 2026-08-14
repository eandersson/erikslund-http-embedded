
function(erikslund_http_add_example name)
    add_executable(${name} ${CMAKE_CURRENT_SOURCE_DIR}/${name}.cpp)

    target_link_libraries(${name} PRIVATE erikslund::http)

    target_compile_options(${name} PRIVATE -Wall -Wextra -Wshadow -Wno-maybe-uninitialized)
endfunction()

erikslund_http_add_example(status_server)

erikslund_http_add_example(minimal)
