
#define DOCTEST_CONFIG_IMPLEMENT
#include <doctest/doctest.h>

#include <cstddef>
#include <cstdio>
#include <print>
#include <string>

#include "support/contract_violations.hpp"

#include "support/test_client.hpp"

namespace {

constexpr int kContractViolationExitCode = 70;

[[nodiscard]] bool report_unacknowledged_contract_violations() {
    using erikslund::http::test::acknowledged_contract_violation_count;
    using erikslund::http::test::contract_violation_count;
    using erikslund::http::test::contract_violation_report;

    const size_t observed = contract_violation_count();
    const size_t acknowledged = acknowledged_contract_violation_count();
    if (observed <= acknowledged)
        return false;

    std::println(stderr, "");
    std::println(stderr, "[contracts] {} contract violation(s) fired, {} were expected by a test.",
                 observed, acknowledged);
    const std::string report = contract_violation_report();
    if (!report.empty())
        std::print(stderr, "{}", report);
    std::println(stderr,
                 "[contracts] A violated contract means an invariant was already broken before the "
                 "assertion that did or did not notice. Fix the caller, or claim the violation "
                 "with a ContractViolationProbe in the test that provokes it.");
    return true;
}

} // namespace

int main(int argc, char** argv) {
    doctest::Context context;
    context.applyCommandLine(argc, argv);

    const int failures = context.run();

    if (context.shouldExit())
        return failures;

    const bool unacknowledged = report_unacknowledged_contract_violations();
    if (failures != 0)
        return failures;
    return unacknowledged ? kContractViolationExitCode : 0;
}
