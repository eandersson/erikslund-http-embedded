
foreach(optimisation_level IN ITEMS 2 3)
    set(bench_target scan_bench_o${optimisation_level})
    add_executable(${bench_target} ${CMAKE_CURRENT_SOURCE_DIR}/scan_bench.cpp)

    target_link_libraries(${bench_target} PRIVATE erikslund::http)

    target_compile_options(${bench_target} PRIVATE
                           -Wall -Wextra -Wshadow -Wno-maybe-uninitialized
                           -O${optimisation_level})
    target_compile_definitions(${bench_target} PRIVATE
                               ERIKSLUND_HTTP_BENCH_LABEL="-O${optimisation_level}")
endforeach()
