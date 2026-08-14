#pragma once

#include <cstddef>
#include <string>
#include <vector>

#include "erikslund/http/contracts.hpp"

#ifndef ERIKSLUND_HTTP_TEST_CONTRACTS_OBSERVED
#define ERIKSLUND_HTTP_TEST_CONTRACTS_OBSERVED 0
#endif

namespace erikslund::http::test {

inline constexpr size_t kMaxRecordedViolations = 32;

struct ContractViolation {
    std::string comment;
    std::string file;
    unsigned line = 0;

    int kind = 0;
    int semantic = 0;
    bool is_terminating = false;

    [[nodiscard]] std::string describe() const;
};

[[nodiscard]] constexpr bool contract_violations_are_observable() noexcept {
    return ERIKSLUND_HTTP_CONTRACTS_ACTIVE != 0 && ERIKSLUND_HTTP_TEST_CONTRACTS_OBSERVED != 0;
}

// Counts test-TU contract violations, not contracts compiled into the library.

[[nodiscard]] size_t contract_violation_count() noexcept;

[[nodiscard]] size_t acknowledged_contract_violation_count() noexcept;

[[nodiscard]] std::vector<ContractViolation> recorded_contract_violations();

[[nodiscard]] std::string contract_violation_report();

// Acknowledges expected process-wide violations; use only in single-threaded tests.
class ContractViolationProbe {
public:
    ContractViolationProbe() noexcept;
    ~ContractViolationProbe();
    ContractViolationProbe(const ContractViolationProbe&) =
        delete("a probe owns one window on a process-wide counter");
    ContractViolationProbe& operator=(const ContractViolationProbe&) =
        delete("a probe owns one window on a process-wide counter");

    [[nodiscard]] size_t count() const noexcept;

    [[nodiscard]] std::string last() const;

private:
    size_t baseline_ = 0;
};

} // namespace erikslund::http::test
