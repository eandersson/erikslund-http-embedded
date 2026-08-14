#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "erikslund/http/build_config.hpp"
#include "erikslund/http/text.hpp"

namespace erikslund::http {

// Only advertise protocols this server can parse.
inline constexpr const char* kDefaultAlpnProtocols = "http/1.1";

inline constexpr const char* kDefaultGroupList = "X25519MLKEM768:X25519:P-256";

inline constexpr unsigned kDefaultHstsMaxAgeSeconds = 31'536'000;

// Bounds unauthenticated signature verification work during mTLS handshakes.
inline constexpr int kMaxClientCertificateChainDepth = 4;

enum class TlsVersion : uint8_t {
    Tls13,
    Tls12,
};

struct TlsOptions {
    // Enabling TLS in a build without OpenSSL makes Server::start() fail.
    bool enabled = false;

    // PEM chain starts with the leaf certificate.
    std::string certificate_chain_file{};
    std::string private_key_file{};

    // Non-empty PEM values take precedence over files.
    std::string certificate_chain_pem{};
    std::string private_key_pem{};

    TlsVersion minimum_version = TlsVersion::Tls13;

    std::string client_ca_file{};

    // When false, offered certificates are verified but may be absent.
    bool require_client_certificate = false;

    // Comma-separated; only http/1.1 is currently supported.
    std::string alpn_protocols = kDefaultAlpnProtocols;

    // Empty uses kDefaultGroupList.
    std::string group_list{};

    bool session_tickets = true;

    // Early data is replayable; enable only for replay-safe routes.
    bool early_data = false;

    // Ignored when unsupported.
    bool kernel_tls = true;

    // Adds Strict-Transport-Security to this listener's responses.
    bool strict_transport_security = false;
    unsigned hsts_max_age_seconds = kDefaultHstsMaxAgeSeconds;
};

[[nodiscard]] constexpr bool tls_available() noexcept {
    return kTlsSupported;
}

inline constexpr char kRelativeNameSeparator = ',';
inline constexpr char kRelativeNameValueJoiner = '+';
inline constexpr char kRelativeNameAssignment = '=';

inline constexpr char kRelativeNameEscape = '\\';
inline constexpr size_t kRelativeNameHexDigits = 2;
inline constexpr int kRelativeNameHexRadix = 16;
inline constexpr int kRelativeNameHexLetterOffset = 10;
inline constexpr int kNotARelativeNameHexDigit = -1;

// Matches a complete decoded RFC 2253 attribute, never a substring. Attribute names are
// case-insensitive; values are byte-exact. Malformed identities fail closed.
[[nodiscard]] constexpr bool client_identity_has_attribute(std::string_view identity,
                                                           std::string_view attribute,
                                                           std::string_view value) noexcept {
    const auto hex_digit_value = [](char character) noexcept -> int {
        if (character >= '0' && character <= '9')
            return character - '0';
        if (character >= 'a' && character <= 'f')
            return character - 'a' + kRelativeNameHexLetterOffset;
        if (character >= 'A' && character <= 'F')
            return character - 'A' + kRelativeNameHexLetterOffset;
        return kNotARelativeNameHexDigit;
    };

    size_t index = 0;
    while (index < identity.size()) {
        const size_t type_begin = index;
        while (index < identity.size() && identity[index] != kRelativeNameAssignment &&
               identity[index] != kRelativeNameSeparator &&
               identity[index] != kRelativeNameValueJoiner)
            ++index;
        if (index == identity.size() || identity[index] != kRelativeNameAssignment)
            return false;
        const bool same_type =
            equals_ignore_case(identity.substr(type_begin, index - type_begin), attribute);
        ++index;

        bool same_value = same_type;
        size_t compared = 0;
        while (index < identity.size() && identity[index] != kRelativeNameSeparator &&
               identity[index] != kRelativeNameValueJoiner) {
            char decoded = identity[index];
            ++index;
            if (decoded == kRelativeNameEscape && index < identity.size()) {
                const int high = hex_digit_value(identity[index]);
                const int low = index + 1 < identity.size() ? hex_digit_value(identity[index + 1])
                                                            : kNotARelativeNameHexDigit;
                if (high != kNotARelativeNameHexDigit && low != kNotARelativeNameHexDigit) {
                    decoded = static_cast<char>(high * kRelativeNameHexRadix + low);
                    index += kRelativeNameHexDigits;
                } else {
                    decoded = identity[index];
                    ++index;
                }
            }
            if (!same_value)
                continue;
            if (compared == value.size() || value[compared] != decoded)
                same_value = false;
            else
                ++compared;
        }
        if (same_value && compared == value.size())
            return true;
        if (index < identity.size())
            ++index;
    }
    return false;
}

} // namespace erikslund::http
