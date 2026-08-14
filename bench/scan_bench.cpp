
// Evidence for the scalar-versus-std::simd decision in PERFORMANCE.md.

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>
#include <version>

#if __has_include(<simd>) && defined(__glibcxx_simd) && defined(__SSE2__)
#include <simd>
#define ERIKSLUND_HTTP_BENCH_SIMD 1
#else
#define ERIKSLUND_HTTP_BENCH_SIMD 0
#endif

#include "erikslund/http/request.hpp"

#ifndef ERIKSLUND_HTTP_BENCH_LABEL
#define ERIKSLUND_HTTP_BENCH_LABEL "unlabelled"
#endif

namespace {

using erikslund::http::parse_request;
using erikslund::http::ParsedRequest;
using erikslund::http::ParseError;
using erikslund::http::RequestLimits;

constexpr std::string_view kHeaderBlockTerminator = "\r\n\r\n";
constexpr size_t kTerminatorBytes = 4;
constexpr size_t kNoTerminator = std::string_view::npos;

constexpr char kHorizontalTab = '\t';
constexpr char kCarriageReturn = '\r';
constexpr char kLineFeed = '\n';
constexpr unsigned char kSpaceByte = 0x20;
constexpr unsigned char kDeleteByte = 0x7F;

constexpr char kControlRangeMask = static_cast<char>(0xE0);

constexpr std::array<size_t, 3> kBlockSizes{200, 2'048, 16'384};

constexpr size_t kBytesPerMebibyte = 1'048'576;
constexpr size_t kBytesPerTrial = 32 * kBytesPerMebibyte;

constexpr size_t kTrialCount = 9;
constexpr size_t kWarmupIterations = 512;

constexpr double kNanosecondsPerSecond = 1'000'000'000.0;
constexpr double kBytesPerGibibyte = 1'073'741'824.0;

struct ScanResult {
    size_t terminator_offset = kNoTerminator;
    bool has_illegal_control = false;
};

[[nodiscard]] constexpr size_t fold(const ScanResult& result) noexcept {
    return result.terminator_offset + (result.has_illegal_control ? 1 : 0);
}

[[nodiscard]] constexpr bool is_illegal_control(char character) noexcept {
    const auto value = static_cast<unsigned char>(character);
    if (value == kDeleteByte)
        return true;
    if (value >= kSpaceByte)
        return false;
    return character != kHorizontalTab && character != kCarriageReturn && character != kLineFeed;
}

[[nodiscard]] ScanResult scan_bytewise(std::string_view block) noexcept {
    ScanResult result;
    for (size_t index = 0; index < block.size(); ++index) {
        const char character = block[index];
        if (character == kCarriageReturn && index + kTerminatorBytes <= block.size() &&
            block.compare(index, kTerminatorBytes, kHeaderBlockTerminator) == 0) {
            result.terminator_offset = index;
            return result;
        }
        result.has_illegal_control = result.has_illegal_control || is_illegal_control(character);
    }
    return result;
}

[[nodiscard]] ScanResult scan_library(std::string_view block) noexcept {
    ScanResult result;
    result.terminator_offset = block.find(kHeaderBlockTerminator);
    const size_t validated =
        result.terminator_offset == kNoTerminator ? block.size() : result.terminator_offset;
    for (size_t index = 0; index < validated; ++index)
        result.has_illegal_control =
            result.has_illegal_control || is_illegal_control(block[index]);
    return result;
}

[[nodiscard]] ScanResult find_only(std::string_view block) noexcept {
    return ScanResult{block.find(kHeaderBlockTerminator), false};
}

#if ERIKSLUND_HTTP_BENCH_SIMD

using CharVector = std::simd::vec<char>;
constexpr size_t kVectorBytes = static_cast<size_t>(CharVector::size());

[[nodiscard]] CharVector load_at(std::string_view block, size_t offset) noexcept {
    return std::simd::unchecked_load<CharVector>(
        std::span<const char>(block.data() + offset, kVectorBytes));
}

[[nodiscard]] ScanResult scan_simd(std::string_view block) noexcept {
    if (block.size() <= kVectorBytes)
        return scan_bytewise(block);

    const CharVector control_range(kControlRangeMask);
    const CharVector zero(char{0});
    const CharVector horizontal_tab(kHorizontalTab);
    const CharVector carriage_return(kCarriageReturn);
    const CharVector line_feed(kLineFeed);
    const CharVector delete_byte(static_cast<char>(kDeleteByte));

    ScanResult result;
    const size_t vector_limit = block.size() - kVectorBytes;
    size_t index = 0;
    while (index < vector_limit) {
        const CharVector chunk = load_at(block, index);
        const CharVector shifted = load_at(block, index + 1);

        const auto control = ((chunk & control_range) == zero) && (chunk != horizontal_tab) &&
                             (chunk != carriage_return) && (chunk != line_feed);
        const auto illegal = control || (chunk == delete_byte);
        // In a valid field block, "\n\r" uniquely identifies the middle of the terminator.
        const auto candidates = (chunk == line_feed) && (shifted == carriage_return);

        if (std::simd::any_of(candidates)) {
            const size_t candidate_end = std::min(index + kVectorBytes, block.size() - 1);
            for (size_t position = index; position < candidate_end; ++position) {
                if (block[position] != kLineFeed || block[position + 1] != kCarriageReturn)
                    continue;
                if (position < 1 || position + kTerminatorBytes - 1 > block.size())
                    continue;
                const size_t terminator = position - 1;
                if (block.compare(terminator, kTerminatorBytes, kHeaderBlockTerminator) != 0)
                    continue;
                for (size_t byte = index; byte < terminator; ++byte)
                    result.has_illegal_control =
                        result.has_illegal_control || is_illegal_control(block[byte]);
                result.terminator_offset = terminator;
                return result;
            }
        }

        result.has_illegal_control = result.has_illegal_control || std::simd::any_of(illegal);
        index += kVectorBytes;
    }

    const size_t tail_begin = index >= kTerminatorBytes - 1 ? index - (kTerminatorBytes - 1) : 0;
    const ScanResult tail = scan_bytewise(block.substr(tail_begin));
    if (tail.terminator_offset != kNoTerminator)
        result.terminator_offset = tail_begin + tail.terminator_offset;
    result.has_illegal_control = result.has_illegal_control || tail.has_illegal_control;
    return result;
}

[[nodiscard]] ScanResult simd_find_only(std::string_view block) noexcept {
    if (block.size() <= kVectorBytes)
        return find_only(block);

    const CharVector carriage_return(kCarriageReturn);
    const CharVector line_feed(kLineFeed);

    const size_t vector_limit = block.size() - kVectorBytes;
    size_t index = 0;
    while (index < vector_limit) {
        const auto candidates = (load_at(block, index) == line_feed) &&
                                (load_at(block, index + 1) == carriage_return);
        if (std::simd::any_of(candidates)) {
            const size_t candidate_end = std::min(index + kVectorBytes, block.size() - 1);
            for (size_t position = index; position < candidate_end; ++position) {
                if (block[position] != kLineFeed || block[position + 1] != kCarriageReturn)
                    continue;
                if (position < 1 || position + kTerminatorBytes - 1 > block.size())
                    continue;
                if (block.compare(position - 1, kTerminatorBytes, kHeaderBlockTerminator) == 0)
                    return ScanResult{position - 1, false};
            }
        }
        index += kVectorBytes;
    }

    const size_t tail_begin = index >= kTerminatorBytes - 1 ? index - (kTerminatorBytes - 1) : 0;
    const size_t tail = block.substr(tail_begin).find(kHeaderBlockTerminator);
    return ScanResult{tail == kNoTerminator ? kNoTerminator : tail_begin + tail, false};
}

#endif

constexpr std::array<std::string_view, 6> kRealisticFields{
    "User-Agent: Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) "
    "Chrome/140.0.0.0 Safari/537.36\r\n",
    "Accept: text/html,application/xhtml+xml,application/xml;q=0.9,image/avif,*/*;q=0.8\r\n",
    "Accept-Language: en-GB,en;q=0.9,sv-SE;q=0.8\r\n",
    "Accept-Encoding: gzip, deflate\r\n",
    "Cookie: session=8f2c1ab4d9e7c05318bd; theme=dark; locale=en-GB; consent=necessary\r\n",
    "Sec-Fetch-Dest: document\r\n",
};

constexpr std::string_view kRequestLine = "GET /status HTTP/1.1\r\nHost: erikslund.local\r\n";
constexpr std::string_view kPaddingFieldName = "X-Erikslund-Padding: ";
constexpr char kPaddingCharacter = 'p';
constexpr size_t kMinimumPaddingFieldBytes = kPaddingFieldName.size() + 2;

constexpr size_t kRealisticFieldBudget = 20;
constexpr size_t kPaddingFieldBudget = 20;

[[nodiscard]] std::string request_of_exactly(size_t total_bytes) {
    std::string block(kRequestLine);
    for (size_t emitted = 0; emitted < kRealisticFieldBudget; ++emitted) {
        const std::string_view field = kRealisticFields[emitted % kRealisticFields.size()];
        if (block.size() + field.size() + kTerminatorBytes > total_bytes)
            break;
        block += field;
    }

    size_t remaining = total_bytes - block.size() - kTerminatorBytes;
    for (size_t field = 0; field < kPaddingFieldBudget; ++field) {
        if (remaining < kMinimumPaddingFieldBytes)
            break;
        const size_t fields_left = kPaddingFieldBudget - field;
        size_t share = remaining / fields_left;
        if (fields_left == 1 || share < kMinimumPaddingFieldBytes)
            share = remaining;
        block += kPaddingFieldName;
        block.append(share - kMinimumPaddingFieldBytes, kPaddingCharacter);
        block += "\r\n";
        remaining -= share;
    }
    block += kHeaderBlockTerminator;
    return block;
}

struct Measurement {
    double nanoseconds_per_call = 0.0;
    size_t checksum = 0;
};

template <class Operation>
[[nodiscard]] Measurement measure(Operation operation, size_t iterations) {
    for (size_t warmup = 0; warmup < kWarmupIterations; ++warmup)
        static_cast<void>(operation());

    Measurement best{};
    for (size_t trial = 0; trial < kTrialCount; ++trial) {
        size_t sink = 0;
        const auto started = std::chrono::steady_clock::now();
        for (size_t iteration = 0; iteration < iterations; ++iteration)
            sink += operation();
        const auto elapsed = std::chrono::steady_clock::now() - started;

        const double per_call =
            std::chrono::duration<double, std::nano>(elapsed).count() /
            static_cast<double>(iterations);
        if (trial == 0 || per_call < best.nanoseconds_per_call) {
            best.nanoseconds_per_call = per_call;
            best.checksum = sink;
        }
    }
    return best;
}

[[nodiscard]] double gibibytes_per_second(size_t block_bytes, double nanoseconds) {
    if (nanoseconds <= 0.0)
        return 0.0;
    return (static_cast<double>(block_bytes) / kBytesPerGibibyte) /
           (nanoseconds / kNanosecondsPerSecond);
}

void report(std::string_view name, size_t block_bytes, const Measurement& measurement,
            double baseline_nanoseconds) {
    std::println("  {:<10} {:>10.1f} {:>9.2f} {:>10.2f}x", name,
                 measurement.nanoseconds_per_call,
                 gibibytes_per_second(block_bytes, measurement.nanoseconds_per_call),
                 baseline_nanoseconds / measurement.nanoseconds_per_call);
}

} // namespace

int main() {
    std::println("erikslund-http header-block scan -- {}", ERIKSLUND_HTTP_BENCH_LABEL);
    std::println("compiler: {}", __VERSION__);
#if ERIKSLUND_HTTP_BENCH_SIMD
    std::println("std::simd: compiled in, vec<char> width {} bytes", kVectorBytes);
#else
    std::println("std::simd: NOT AVAILABLE -- <simd>, __glibcxx_simd or __SSE2__ is missing");
#endif
    std::println("{} trials per figure, fastest reported, {} MiB scanned per trial", kTrialCount,
                 kBytesPerTrial / kBytesPerMebibyte);

    const RequestLimits limits;
    for (const size_t requested : kBlockSizes) {
        const std::string block = request_of_exactly(requested);
        const size_t iterations = std::max<size_t>(1, kBytesPerTrial / block.size());
        const std::string_view view(block);
        const std::expected<ParsedRequest, ParseError> parse_outcome = parse_request(view, limits);

        std::println("");
        const size_t field_count =
            parse_outcome.has_value() ? parse_outcome->request.headers().size() : size_t{0};
        std::println("block {} B, {} header fields, terminator at {}, {} iterations per trial",
                     block.size(), field_count, block.find(kHeaderBlockTerminator), iterations);
        std::println("  parse_request: {}",
                     parse_outcome.has_value()
                         ? "accepted, so the reference figure below is a whole parse"
                         : "REFUSED -- the reference figure is not a whole parse");
        std::println("  {:<10} {:>10} {:>9} {:>11}", "impl", "ns/scan", "GiB/s", "vs incumbent");

        const Measurement scalar_find = measure([view] { return fold(find_only(view)); },
                                                iterations);
        report("find", block.size(), scalar_find, scalar_find.nanoseconds_per_call);
#if ERIKSLUND_HTTP_BENCH_SIMD
        const Measurement simd_find = measure([view] { return fold(simd_find_only(view)); },
                                              iterations);
        report("find-simd", block.size(), simd_find, scalar_find.nanoseconds_per_call);
        if (simd_find.checksum != scalar_find.checksum)
            std::println("  MISMATCH: the two terminator searches disagreed ({}, {})",
                         scalar_find.checksum, simd_find.checksum);
#endif

        const Measurement library = measure([view] { return fold(scan_library(view)); },
                                            iterations);
        const Measurement bytewise = measure([view] { return fold(scan_bytewise(view)); },
                                             iterations);
        report("library", block.size(), library, library.nanoseconds_per_call);
        report("bytewise", block.size(), bytewise, library.nanoseconds_per_call);
#if ERIKSLUND_HTTP_BENCH_SIMD
        const Measurement simd = measure([view] { return fold(scan_simd(view)); }, iterations);
        report("simd", block.size(), simd, library.nanoseconds_per_call);
        if (simd.checksum != library.checksum || bytewise.checksum != library.checksum)
            std::println("  MISMATCH: the three fused scans did not agree ({}, {}, {})",
                         library.checksum, bytewise.checksum, simd.checksum);
#else
        if (bytewise.checksum != library.checksum)
            std::println("  MISMATCH: the two fused scans did not agree ({}, {})",
                         library.checksum, bytewise.checksum);
#endif

        const Measurement parsed = measure(
            [view, &limits] {
                const std::expected<ParsedRequest, ParseError> outcome =
                    parse_request(view, limits);
                return outcome.has_value() ? outcome->consumed_bytes : size_t{0};
            },
            iterations);
        std::println("  {:<10} {:>10.1f}   (parse_request over the same bytes)", "parse",
                     parsed.nanoseconds_per_call);
    }
    return 0;
}
