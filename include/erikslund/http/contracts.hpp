#pragma once

#include "erikslund/http/build_config.hpp"

// Contracts express programmer errors only. Validate every request, configuration value, and other
// externally controlled input with ordinary runtime code; a contract violation never throws and
// may terminate the process.
//
// Use the shims after the parameter list. Name postcondition results, and make value parameters
// referenced by a postcondition const. Runtime branches paired with contracts must remain correct
// under the observe semantic.
//
// The build flag is authoritative: GCC 16 defines __cpp_contracts even without -fcontracts, while
// clang-tidy and cppcheck cannot parse the syntax.

#if ERIKSLUND_HTTP_CONTRACTS && !defined(ERIKSLUND_HTTP_NO_CONTRACTS) && defined(__GNUC__) && \
    !defined(__clang__) && !defined(CPPCHECK) && __GNUC__ >= 16

#define ERIKSLUND_HTTP_PRE(...) pre(__VA_ARGS__)
#define ERIKSLUND_HTTP_POST(...) post(__VA_ARGS__)
#define ERIKSLUND_HTTP_ASSERT(...) contract_assert(__VA_ARGS__)
#define ERIKSLUND_HTTP_CONTRACTS_ACTIVE 1

#else

#define ERIKSLUND_HTTP_PRE(...)
#define ERIKSLUND_HTTP_POST(...)
#define ERIKSLUND_HTTP_ASSERT(...) static_cast<void>(0)
#define ERIKSLUND_HTTP_CONTRACTS_ACTIVE 0

#endif
