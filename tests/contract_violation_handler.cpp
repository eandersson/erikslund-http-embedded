
#include "support/contract_violations.hpp"

#include <atomic>
#include <cstddef>
#include <exception>
#include <format>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#if ERIKSLUND_HTTP_CONTRACTS_ACTIVE
#if !__has_include(<contracts>)
#error "ERIKSLUND_HTTP_CONTRACTS is on but <contracts> is absent; the replaceable violation \
handler cannot be defined. Either the toolchain is older than GCC 16 or -fcontracts was added \
without the library that carries std::contracts::contract_violation."
#endif
#include <contracts>
#endif

namespace erikslund::http::test {

void record_contract_violation(ContractViolation violation) noexcept;

namespace {

struct ViolationState {
    std::mutex mutex;
    std::vector<ContractViolation> recorded;
    std::atomic<size_t> observed{0};
    std::atomic<size_t> acknowledged{0};
};

ViolationState& state() {
    // Safe even when a contract fires during another translation unit's static initialization.
    static ViolationState shared;
    return shared;
}

} // namespace

void record_contract_violation(ContractViolation violation) noexcept {
    ViolationState& shared = state();
    shared.observed.fetch_add(1, std::memory_order_relaxed);
    try {
        const std::scoped_lock guard(shared.mutex);
        if (shared.recorded.size() < kMaxRecordedViolations)
            shared.recorded.push_back(std::move(violation));
    } catch (const std::exception&) {
    }
}

std::string ContractViolation::describe() const {
    return std::format("{}:{}: contract violated: {} (kind={} semantic={} terminating={})",
                       file.empty() ? "<unknown file>" : file, line,
                       comment.empty() ? "<no predicate text>" : comment, kind, semantic,
                       is_terminating);
}

size_t contract_violation_count() noexcept {
    return state().observed.load(std::memory_order_relaxed);
}

size_t acknowledged_contract_violation_count() noexcept {
    return state().acknowledged.load(std::memory_order_relaxed);
}

std::vector<ContractViolation> recorded_contract_violations() {
    ViolationState& shared = state();
    const std::scoped_lock guard(shared.mutex);
    return shared.recorded;
}

std::string contract_violation_report() {
    const std::vector<ContractViolation> violations = recorded_contract_violations();
    if (violations.empty())
        return {};

    std::string report;
    for (const ContractViolation& violation : violations) {
        report += "  ";
        report += violation.describe();
        report += "\n";
    }
    const size_t total = contract_violation_count();
    if (total > violations.size())
        report += std::format("  ... and {} further violation(s) whose text was not recorded\n",
                              total - violations.size());
    return report;
}

ContractViolationProbe::ContractViolationProbe() noexcept
    : baseline_(contract_violation_count()) {}

ContractViolationProbe::~ContractViolationProbe() {
    state().acknowledged.fetch_add(count(), std::memory_order_relaxed);
}

size_t ContractViolationProbe::count() const noexcept {
    const size_t observed = contract_violation_count();
    return observed > baseline_ ? observed - baseline_ : 0;
}

std::string ContractViolationProbe::last() const {
    const std::vector<ContractViolation> violations = recorded_contract_violations();
    if (violations.size() <= baseline_)
        return {};
    return violations.back().describe();
}

} // namespace erikslund::http::test

#if ERIKSLUND_HTTP_CONTRACTS_ACTIVE

// GCC finds this replaceable P2900 handler by its exact global name and signature.
void handle_contract_violation(const std::contracts::contract_violation& violation) {
    erikslund::http::test::ContractViolation record;
    try {
        const char* const comment = violation.comment();
        if (comment != nullptr)
            record.comment = comment;
        const char* const file = violation.location().file_name();
        if (file != nullptr)
            record.file = file;
        record.line = violation.location().line();
        record.kind = static_cast<int>(violation.kind());
        record.semantic = static_cast<int>(violation.semantic());
        record.is_terminating = violation.is_terminating();
    } catch (const std::exception&) {
    }
    erikslund::http::test::record_contract_violation(std::move(record));
}

#endif // ERIKSLUND_HTTP_CONTRACTS_ACTIVE
