if(NOT DEFINED READ_ELF OR NOT DEFINED BINARY)
    message(FATAL_ERROR "READ_ELF and BINARY are required")
endif()

execute_process(
    COMMAND "${READ_ELF}" -h "${BINARY}"
    RESULT_VARIABLE header_result
    OUTPUT_VARIABLE header)
if(NOT header_result EQUAL 0 OR NOT header MATCHES "Type:[ \t]+DYN")
    message(FATAL_ERROR "${BINARY} is not a position-independent executable")
endif()

execute_process(
    COMMAND "${READ_ELF}" -W -l "${BINARY}"
    RESULT_VARIABLE program_header_result
    OUTPUT_VARIABLE program_headers)
if(NOT program_header_result EQUAL 0 OR NOT program_headers MATCHES "GNU_RELRO")
    message(FATAL_ERROR "${BINARY} has no GNU_RELRO segment")
endif()
if(NOT program_headers MATCHES "GNU_STACK[^\r\n]*RW[ \t]")
    message(FATAL_ERROR "${BINARY} does not have a non-executable stack")
endif()

execute_process(
    COMMAND "${READ_ELF}" -d "${BINARY}"
    RESULT_VARIABLE dynamic_result
    OUTPUT_VARIABLE dynamic_section)
if(NOT dynamic_result EQUAL 0 OR NOT dynamic_section MATCHES "BIND_NOW")
    message(FATAL_ERROR "${BINARY} does not enable immediate binding")
endif()
