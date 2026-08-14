#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace erikslund::http {

// Compile-time SHA-256 for the fixed CSP script only; not a general security primitive.
inline constexpr size_t kSha256DigestBytes = 32;
inline constexpr size_t kSha256BlockBytes = 64;
inline constexpr size_t kSha256Rounds = 64;
inline constexpr size_t kSha256StateWords = 8;

inline constexpr size_t kSha256LengthBytes = 8;
inline constexpr uint8_t kSha256PaddingMarker = 0x80;

inline constexpr size_t kBytesPerWord = 4;
inline constexpr size_t kBitsPerByte = 8;
inline constexpr size_t kWordBits = 32;

using Sha256Digest = std::array<uint8_t, kSha256DigestBytes>;

// FIPS 180-4 section 4.2.2.
inline constexpr std::array<uint32_t, kSha256Rounds> kSha256RoundConstants = {
    0x428a2f98U, 0x71374491U, 0xb5c0fbcfU, 0xe9b5dba5U, 0x3956c25bU, 0x59f111f1U, 0x923f82a4U,
    0xab1c5ed5U, 0xd807aa98U, 0x12835b01U, 0x243185beU, 0x550c7dc3U, 0x72be5d74U, 0x80deb1feU,
    0x9bdc06a7U, 0xc19bf174U, 0xe49b69c1U, 0xefbe4786U, 0x0fc19dc6U, 0x240ca1ccU, 0x2de92c6fU,
    0x4a7484aaU, 0x5cb0a9dcU, 0x76f988daU, 0x983e5152U, 0xa831c66dU, 0xb00327c8U, 0xbf597fc7U,
    0xc6e00bf3U, 0xd5a79147U, 0x06ca6351U, 0x14292967U, 0x27b70a85U, 0x2e1b2138U, 0x4d2c6dfcU,
    0x53380d13U, 0x650a7354U, 0x766a0abbU, 0x81c2c92eU, 0x92722c85U, 0xa2bfe8a1U, 0xa81a664bU,
    0xc24b8b70U, 0xc76c51a3U, 0xd192e819U, 0xd6990624U, 0xf40e3585U, 0x106aa070U, 0x19a4c116U,
    0x1e376c08U, 0x2748774cU, 0x34b0bcb5U, 0x391c0cb3U, 0x4ed8aa4aU, 0x5b9cca4fU, 0x682e6ff3U,
    0x748f82eeU, 0x78a5636fU, 0x84c87814U, 0x8cc70208U, 0x90befffaU, 0xa4506cebU, 0xbef9a3f7U,
    0xc67178f2U};

// FIPS 180-4 section 5.3.3.
inline constexpr std::array<uint32_t, kSha256StateWords> kSha256InitialState = {
    0x6a09e667U, 0xbb67ae85U, 0x3c6ef372U, 0xa54ff53aU,
    0x510e527fU, 0x9b05688cU, 0x1f83d9abU, 0x5be0cd19U};

[[nodiscard]] constexpr uint32_t rotate_right(uint32_t value, unsigned bits) noexcept {
    return (value >> bits) | (value << (kWordBits - bits));
}

[[nodiscard]] constexpr uint8_t padded_message_byte(std::string_view message, size_t padded_bytes,
                                                    size_t offset) noexcept {
    if (offset < message.size())
        return static_cast<uint8_t>(message[offset]);
    if (offset == message.size())
        return kSha256PaddingMarker;
    if (offset < padded_bytes - kSha256LengthBytes)
        return 0;
    const uint64_t message_bits = static_cast<uint64_t>(message.size()) * kBitsPerByte;
    const unsigned shift = static_cast<unsigned>((padded_bytes - 1 - offset) * kBitsPerByte);
    return uint8_t((message_bits >> shift) & 0xFFU);
}

[[nodiscard]] constexpr Sha256Digest sha256(std::string_view message) noexcept {
    const size_t minimum = message.size() + 1 + kSha256LengthBytes;
    const size_t padded_bytes =
        ((minimum + kSha256BlockBytes - 1) / kSha256BlockBytes) * kSha256BlockBytes;

    std::array<uint32_t, kSha256StateWords> state = kSha256InitialState;
    for (size_t block = 0; block < padded_bytes; block += kSha256BlockBytes) {
        std::array<uint32_t, kSha256Rounds> schedule{};
        for (size_t word = 0; word < kSha256BlockBytes / kBytesPerWord; ++word) {
            uint32_t packed = 0;
            for (size_t byte_in_word = 0; byte_in_word < kBytesPerWord; ++byte_in_word)
                packed = (packed << kBitsPerByte) |
                         padded_message_byte(message, padded_bytes,
                                             block + word * kBytesPerWord + byte_in_word);
            schedule[word] = packed;
        }
        for (size_t index = kSha256BlockBytes / kBytesPerWord; index < kSha256Rounds; ++index) {
            const uint32_t previous = schedule[index - 15];
            const uint32_t recent = schedule[index - 2];
            const uint32_t spread_low =
                rotate_right(previous, 7) ^ rotate_right(previous, 18) ^ (previous >> 3);
            const uint32_t spread_high =
                rotate_right(recent, 17) ^ rotate_right(recent, 19) ^ (recent >> 10);
            schedule[index] = schedule[index - 16] + spread_low + schedule[index - 7] + spread_high;
        }

        // FIPS 180-4 names these working variables a through h.
        uint32_t working_a = state[0];
        uint32_t working_b = state[1];
        uint32_t working_c = state[2];
        uint32_t working_d = state[3];
        uint32_t working_e = state[4];
        uint32_t working_f = state[5];
        uint32_t working_g = state[6];
        uint32_t working_h = state[7];
        for (size_t round = 0; round < kSha256Rounds; ++round) {
            const uint32_t sigma_e =
                rotate_right(working_e, 6) ^ rotate_right(working_e, 11) ^
                rotate_right(working_e, 25);
            const uint32_t choose = (working_e & working_f) ^ (~working_e & working_g);
            const uint32_t first_temporary = working_h + sigma_e + choose +
                                             kSha256RoundConstants[round] + schedule[round];
            const uint32_t sigma_a =
                rotate_right(working_a, 2) ^ rotate_right(working_a, 13) ^
                rotate_right(working_a, 22);
            const uint32_t majority = (working_a & working_b) ^ (working_a & working_c) ^
                                      (working_b & working_c);
            const uint32_t second_temporary = sigma_a + majority;

            working_h = working_g;
            working_g = working_f;
            working_f = working_e;
            working_e = working_d + first_temporary;
            working_d = working_c;
            working_c = working_b;
            working_b = working_a;
            working_a = first_temporary + second_temporary;
        }

        state[0] += working_a;
        state[1] += working_b;
        state[2] += working_c;
        state[3] += working_d;
        state[4] += working_e;
        state[5] += working_f;
        state[6] += working_g;
        state[7] += working_h;
    }

    Sha256Digest digest{};
    for (size_t word = 0; word < kSha256StateWords; ++word)
        for (size_t byte_in_word = 0; byte_in_word < kBytesPerWord; ++byte_in_word) {
            const unsigned shift =
                static_cast<unsigned>((kBytesPerWord - 1 - byte_in_word) * kBitsPerByte);
            digest[word * kBytesPerWord + byte_in_word] =
                uint8_t((state[word] >> shift) & 0xFFU);
        }
    return digest;
}

inline constexpr std::string_view kBase64Alphabet =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

inline constexpr size_t kBase64BytesPerGroup = 3;
inline constexpr size_t kBase64CharactersPerGroup = 4;
inline constexpr size_t kBase64Bits = 6;
inline constexpr size_t kBase64DigestLength = 44;
inline constexpr char kBase64Padding = '=';

// view() borrows this storage.
struct Base64Digest {
    std::array<char, kBase64DigestLength> characters{};

    [[nodiscard]] constexpr std::string_view view() const noexcept {
        return std::string_view(characters.data(), characters.size());
    }
};

static_assert(kSha256DigestBytes % kBase64BytesPerGroup == 2);

[[nodiscard]] constexpr Base64Digest base64_of_digest(const Sha256Digest& digest) noexcept {
    Base64Digest encoded{};
    size_t written = 0;
    const auto emit = [&encoded, &written](uint32_t group, size_t sextet_index) {
        const unsigned shift = static_cast<unsigned>(sextet_index * kBase64Bits);
        encoded.characters[written++] = kBase64Alphabet[(group >> shift) & 0x3FU];
    };

    size_t index = 0;
    for (; index + kBase64BytesPerGroup <= kSha256DigestBytes; index += kBase64BytesPerGroup) {
        const uint32_t group = (uint32_t(digest[index]) << (2 * kBitsPerByte)) |
                               (uint32_t(digest[index + 1]) << kBitsPerByte) |
                               uint32_t(digest[index + 2]);
        emit(group, 3);
        emit(group, 2);
        emit(group, 1);
        emit(group, 0);
    }
    const uint32_t tail = (uint32_t(digest[index]) << (2 * kBitsPerByte)) |
                          (uint32_t(digest[index + 1]) << kBitsPerByte);
    emit(tail, 3);
    emit(tail, 2);
    emit(tail, 1);
    encoded.characters[written] = kBase64Padding;
    return encoded;
}

[[nodiscard]] constexpr bool sha256_base64_equals(std::string_view message,
                                                  std::string_view expected) noexcept {
    return base64_of_digest(sha256(message)).view() == expected;
}

static_assert(sha256_base64_equals("", "47DEQpj8HBSa+/TImW+5JCeuQeRkm5NMpJWZG3hSuFU="),
              "the digest of the empty message");
static_assert(sha256_base64_equals("abc", "ungWv48Bz+pBQUDeXa4iI7ADYaOWF3qctBD/YfIAFa0="),
              "FIPS 180-4 one-block example");
static_assert(sha256_base64_equals(
                  "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq",
                  "JI1qYdIGOLjlwCaTDD5gOaM85Flk/yFn9uzt1BnbBsE="),
              "FIPS 180-4 two-block example, which is the case that exercises the block loop");

} // namespace erikslund::http
