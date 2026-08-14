
set(ERIKSLUND_HTTP_TEST_CERTIFICATE_DAYS 3650)

set(ERIKSLUND_HTTP_TEST_CERTIFICATE_CURVE prime256v1)

set(ERIKSLUND_HTTP_TEST_CERTIFICATE_SAN
    "subjectAltName=DNS:localhost,IP:127.0.0.1,IP:0:0:0:0:0:0:0:1")

function(erikslund_http_add_test_certificates target)
    find_program(ERIKSLUND_HTTP_OPENSSL_COMMAND NAMES openssl)
    if(NOT ERIKSLUND_HTTP_OPENSSL_COMMAND)
        message(FATAL_ERROR
            "The TLS suites need the openssl command-line tool to generate a test certificate, and "
            "it was not found on PATH. Install it (Debian: apt-get install openssl), or configure "
            "with -DERIKSLUND_HTTP_TLS=OFF to build the suite without its TLS tier.")
    endif()

    set(certificate_dir ${CMAKE_CURRENT_BINARY_DIR}/test-certificates)
    file(MAKE_DIRECTORY ${certificate_dir})

    set(certificate_file ${certificate_dir}/server-certificate.pem)
    set(private_key_file ${certificate_dir}/server-key.pem)
    set(mismatched_key_file ${certificate_dir}/unrelated-key.pem)

    add_custom_command(
        OUTPUT ${certificate_file} ${private_key_file}
        COMMAND ${ERIKSLUND_HTTP_OPENSSL_COMMAND} req -x509 -noenc
                -newkey ec -pkeyopt ec_paramgen_curve:${ERIKSLUND_HTTP_TEST_CERTIFICATE_CURVE}
                -keyout ${private_key_file}
                -out ${certificate_file}
                -days ${ERIKSLUND_HTTP_TEST_CERTIFICATE_DAYS}
                -subj "/CN=localhost"
                -addext ${ERIKSLUND_HTTP_TEST_CERTIFICATE_SAN}
                -addext "basicConstraints=critical,CA:FALSE"
                -addext "keyUsage=critical,digitalSignature,keyEncipherment"
                -addext "extendedKeyUsage=serverAuth"
        COMMENT "openssl: self-signed test certificate for localhost, 127.0.0.1 and ::1"
        VERBATIM)

    add_custom_command(
        OUTPUT ${mismatched_key_file}
        COMMAND ${ERIKSLUND_HTTP_OPENSSL_COMMAND} genpkey -algorithm EC
                -pkeyopt ec_paramgen_curve:${ERIKSLUND_HTTP_TEST_CERTIFICATE_CURVE}
                -out ${mismatched_key_file}
        COMMENT "openssl: an unrelated key, for the certificate/key mismatch case"
        VERBATIM)

    add_custom_target(erikslund_http_test_certificates
                      DEPENDS ${certificate_file} ${private_key_file} ${mismatched_key_file})
    add_dependencies(${target} erikslund_http_test_certificates)

    target_compile_definitions(
        ${target} PRIVATE
        ERIKSLUND_HTTP_TEST_CERTIFICATE_FILE="${certificate_file}"
        ERIKSLUND_HTTP_TEST_PRIVATE_KEY_FILE="${private_key_file}"
        ERIKSLUND_HTTP_TEST_MISMATCHED_KEY_FILE="${mismatched_key_file}"
        ERIKSLUND_HTTP_TEST_CERTIFICATE_DIR="${certificate_dir}")
endfunction()
