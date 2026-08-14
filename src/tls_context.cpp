#include "internal/tls_context.hpp"

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>

#if ERIKSLUND_HTTP_TLS
#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#endif

#include "erikslund/http/contracts.hpp"
#include "erikslund/http/server.hpp"
#include "erikslund/http/tls.hpp"
#include "internal/logger.hpp"
#include "internal/transport.hpp"

namespace erikslund::http::internal {

#if ERIKSLUND_HTTP_TLS

namespace {

inline constexpr size_t kOpenSslErrorTextBytes = 256;

inline constexpr size_t kMaxReportedOpenSslErrors = 4;

inline constexpr uint32_t kMaxEarlyDataBytes = 16'384;

// Bind resumed sessions to the client-certificate policy that originally verified them.
inline constexpr std::string_view kAnonymousSessionContext = "erikslund-http/anon";
inline constexpr std::string_view kMutualSessionContext = "erikslund-http/mtls";
static_assert(kAnonymousSessionContext.size() <= SSL_MAX_SID_CTX_LENGTH &&
                  kMutualSessionContext.size() <= SSL_MAX_SID_CTX_LENGTH,
              "a session id context longer than OpenSSL's limit is rejected at run time, and a "
              "listener that cannot set one would resume across verification policies");

// Drain the thread-local OpenSSL queue so stale errors cannot be blamed on a later operation.
[[nodiscard]] std::string drain_openssl_errors() {
    std::string detail;
    size_t appended = 0;
    for (unsigned long code = ERR_get_error(); code != 0; code = ERR_get_error()) {
        if (appended >= kMaxReportedOpenSslErrors)
            continue;
        std::array<char, kOpenSslErrorTextBytes> text{};
        ERR_error_string_n(code, text.data(), text.size());
        if (!detail.empty())
            detail += "; ";
        detail += text.data();
        ++appended;
    }
    if (detail.empty())
        detail = "no further detail from OpenSSL";
    return detail;
}

// Fail encrypted keys instead of blocking a daemon on OpenSSL's terminal prompt.
int refuse_passphrase([[maybe_unused]] char* buffer, [[maybe_unused]] int size,
                      [[maybe_unused]] int read_or_write, [[maybe_unused]] void* user_data) {
    return 0;
}

struct SslCtxDeleter {
    void operator()(SSL_CTX* context) const noexcept { SSL_CTX_free(context); }
};
using SslCtxPtr = std::unique_ptr<SSL_CTX, SslCtxDeleter>;

struct BioDeleter {
    void operator()(BIO* bio) const noexcept { BIO_free(bio); }
};
using BioPtr = std::unique_ptr<BIO, BioDeleter>;

struct X509Deleter {
    void operator()(X509* certificate) const noexcept { X509_free(certificate); }
};
using X509Ptr = std::unique_ptr<X509, X509Deleter>;

struct EvpPkeyDeleter {
    void operator()(EVP_PKEY* key) const noexcept { EVP_PKEY_free(key); }
};
using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>;

struct X509NameStackDeleter {
    void operator()(STACK_OF(X509_NAME)* names) const noexcept {
        sk_X509_NAME_pop_free(names, X509_NAME_free);
    }
};
using X509NameStackPtr = std::unique_ptr<STACK_OF(X509_NAME), X509NameStackDeleter>;

[[nodiscard]] std::string_view trim_ascii_spaces(std::string_view text) noexcept {
    const size_t first = text.find_first_not_of(" \t");
    if (first == std::string_view::npos)
        return {};
    return text.substr(first, text.find_last_not_of(" \t") - first + 1);
}

void reject_unsupported_alpn(const TlsOptions& options) {
    std::string_view remaining = options.alpn_protocols;
    while (!remaining.empty()) {
        const size_t separator = remaining.find(',');
        const std::string_view entry = trim_ascii_spaces(remaining.substr(0, separator));
        remaining = separator == std::string_view::npos ? std::string_view{}
                                                        : remaining.substr(separator + 1);
        if (entry.empty())
            continue;
        if (entry != kAlpnHttp11Name)
            throw ServerError(std::format(
                "TLS: alpn_protocols names \"{}\", which this server cannot speak. Only \"{}\" is "
                "implemented; see docs/http2-plan.md.",
                entry, kAlpnHttp11Name));
    }
}

int select_alpn_protocol([[maybe_unused]] SSL* ssl, const unsigned char** out,
                         unsigned char* out_length,
                         const unsigned char* client_protocols, unsigned int client_length,
                         [[maybe_unused]] void* argument) {
    unsigned char* selected = nullptr;
    const int outcome = SSL_select_next_proto(
        &selected, out_length, reinterpret_cast<const unsigned char*>(kAlpnHttp11Wire.data()),
        static_cast<unsigned int>(kAlpnHttp11Wire.size()), client_protocols, client_length);

    // NO_OVERLAP points at the client's first protocol; never advertise that as a match.
    if (outcome != OPENSSL_NPN_NEGOTIATED)
        return SSL_TLSEXT_ERR_ALERT_FATAL;
    *out = selected;
    return SSL_TLSEXT_ERR_OK;
}

void apply_protocol_floor(SSL_CTX* context, const TlsOptions& options) {
    const int floor_version =
        options.minimum_version == TlsVersion::Tls12 ? TLS1_2_VERSION : TLS1_3_VERSION;
    if (SSL_CTX_set_min_proto_version(context, floor_version) != 1)
        throw ServerError(std::format("TLS: cannot set the minimum protocol version to {:#06x}: {}",
                                      floor_version, drain_openssl_errors()));
}

void apply_hardening_options(SSL_CTX* context, const TlsOptions& options) {
    // Renegotiation amplifies asymmetric work; TLS compression enables CRIME.
    SSL_CTX_set_options(context, SSL_OP_NO_RENEGOTIATION | SSL_OP_CIPHER_SERVER_PREFERENCE |
                                     SSL_OP_NO_COMPRESSION);

    // Release record buffers while keep-alive connections idle.
    SSL_CTX_set_mode(context, SSL_MODE_RELEASE_BUFFERS);

    // Keep SSL_write_ex all-or-nothing to preserve the identical-buffer retry rule.

#ifdef SSL_OP_ENABLE_KTLS
    if (options.kernel_tls)
        SSL_CTX_set_options(context, SSL_OP_ENABLE_KTLS);
#else
    static_cast<void>(options);
#endif
}

void apply_group_list(SSL_CTX* context, const TlsOptions& options) {
    // Preserve OpenSSL 3.5's post-quantum-first preference to resist harvest-now-decrypt-later.
    const char* const groups =
        options.group_list.empty() ? kDefaultGroupList : options.group_list.c_str();
    if (SSL_CTX_set1_groups_list(context, groups) != 1)
        throw ServerError(std::format("TLS: the key-exchange group list \"{}\" was rejected: {}",
                                      groups, drain_openssl_errors()));
}

void apply_session_policy(SSL_CTX* context, const TlsOptions& options) {
    if (!options.session_tickets) {
        SSL_CTX_set_options(context, SSL_OP_NO_TICKET);
        if (SSL_CTX_set_num_tickets(context, 0) != 1)
            throw ServerError(
                std::format("TLS: cannot disable session tickets: {}", drain_openssl_errors()));
    }
    // A fresh SSL_CTX also rotates ticket keys.

    // Early data is replayable, so 0-RTT remains opt-in.
    const uint32_t early_data_bytes = options.early_data ? kMaxEarlyDataBytes : 0;
    if (SSL_CTX_set_max_early_data(context, early_data_bytes) != 1)
        throw ServerError(
            std::format("TLS: cannot set the early-data limit: {}", drain_openssl_errors()));
}

void use_chain_from_file(SSL_CTX* context, const std::string& path) {
    if (SSL_CTX_use_certificate_chain_file(context, path.c_str()) != 1)
        throw ServerError(std::format("TLS: cannot load the certificate chain from \"{}\": {}",
                                      path, drain_openssl_errors()));
}

void use_chain_from_pem(SSL_CTX* context, const std::string& pem) {
    const BioPtr source(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
    if (!source)
        throw ServerError(std::format("TLS: cannot wrap certificate_chain_pem for reading: {}",
                                      drain_openssl_errors()));

    const X509Ptr leaf(PEM_read_bio_X509_AUX(source.get(), nullptr, &refuse_passphrase, nullptr));
    if (!leaf)
        throw ServerError(std::format("TLS: certificate_chain_pem holds no leaf certificate: {}",
                                      drain_openssl_errors()));
    if (SSL_CTX_use_certificate(context, leaf.get()) != 1)
        throw ServerError(std::format("TLS: the leaf certificate in certificate_chain_pem was "
                                      "rejected: {}",
                                      drain_openssl_errors()));
    if (SSL_CTX_clear_chain_certs(context) != 1)
        throw ServerError(std::format("TLS: cannot reset the certificate chain: {}",
                                      drain_openssl_errors()));

    while (true) {
        X509Ptr intermediate(
            PEM_read_bio_X509(source.get(), nullptr, &refuse_passphrase, nullptr));
        if (!intermediate)
            break;
        if (SSL_CTX_add0_chain_cert(context, intermediate.get()) != 1)
            throw ServerError(std::format("TLS: an intermediate in certificate_chain_pem was "
                                          "rejected: {}",
                                          drain_openssl_errors()));
        std::ignore = intermediate.release();
    }

    const unsigned long last = ERR_peek_last_error();
    if (ERR_GET_LIB(last) == ERR_LIB_PEM && ERR_GET_REASON(last) == PEM_R_NO_START_LINE) {
        ERR_clear_error();
        return;
    }
    if (last != 0)
        throw ServerError(std::format("TLS: certificate_chain_pem is malformed after the leaf "
                                      "certificate: {}",
                                      drain_openssl_errors()));
}

void use_private_key_from_file(SSL_CTX* context, const std::string& path) {
    if (SSL_CTX_use_PrivateKey_file(context, path.c_str(), SSL_FILETYPE_PEM) != 1)
        throw ServerError(std::format("TLS: cannot load the private key from \"{}\": {}", path,
                                      drain_openssl_errors()));
}

void use_private_key_from_pem(SSL_CTX* context, const std::string& pem) {
    const BioPtr source(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
    if (!source)
        throw ServerError(std::format("TLS: cannot wrap private_key_pem for reading: {}",
                                      drain_openssl_errors()));

    const EvpPkeyPtr key(
        PEM_read_bio_PrivateKey(source.get(), nullptr, &refuse_passphrase, nullptr));
    if (!key)
        throw ServerError(std::format("TLS: private_key_pem could not be parsed -- an encrypted "
                                      "key is not usable here, because a daemon has no terminal to "
                                      "take a passphrase from: {}",
                                      drain_openssl_errors()));
    if (SSL_CTX_use_PrivateKey(context, key.get()) != 1)
        throw ServerError(
            std::format("TLS: the key in private_key_pem was rejected: {}",
                        drain_openssl_errors()));
}

void load_certificate_material(SSL_CTX* context, const TlsOptions& options) {
    SSL_CTX_set_default_passwd_cb(context, &refuse_passphrase);

    const bool chain_is_inline = !options.certificate_chain_pem.empty();
    const bool key_is_inline = !options.private_key_pem.empty();

    if (!chain_is_inline && options.certificate_chain_file.empty())
        throw ServerError("TLS: enabled but no certificate was configured. Set "
                          "certificate_chain_file or certificate_chain_pem.");
    if (!key_is_inline && options.private_key_file.empty())
        throw ServerError("TLS: enabled but no private key was configured. Set private_key_file or "
                          "private_key_pem.");

    if (chain_is_inline)
        use_chain_from_pem(context, options.certificate_chain_pem);
    else
        use_chain_from_file(context, options.certificate_chain_file);

    if (key_is_inline)
        use_private_key_from_pem(context, options.private_key_pem);
    else
        use_private_key_from_file(context, options.private_key_file);

    if (SSL_CTX_check_private_key(context) != 1)
        throw ServerError(std::format(
            "TLS: the private key ({}) does not match the certificate ({}): {}",
            key_is_inline ? std::string("private_key_pem") : options.private_key_file,
            chain_is_inline ? std::string("certificate_chain_pem")
                            : options.certificate_chain_file,
            drain_openssl_errors()));
}

void apply_session_id_context(SSL_CTX* context, bool verifies_client_certificates) {
    const std::string_view identifier =
        verifies_client_certificates ? kMutualSessionContext : kAnonymousSessionContext;
    if (SSL_CTX_set_session_id_context(context,
                                       reinterpret_cast<const unsigned char*>(identifier.data()),
                                       static_cast<unsigned int>(identifier.size())) != 1)
        throw ServerError(std::format("TLS: cannot set the session id context: {}",
                                      drain_openssl_errors()));
}

void configure_client_verification(SSL_CTX* context, const TlsOptions& options) {
    apply_session_id_context(context, !options.client_ca_file.empty());

    if (options.client_ca_file.empty()) {
        if (options.require_client_certificate)
            throw ServerError("TLS: require_client_certificate is set but client_ca_file is empty. "
                              "With no CA to verify against, every client certificate would fail "
                              "and the listener would refuse every connection.");
        SSL_CTX_set_verify(context, SSL_VERIFY_NONE, nullptr);
        return;
    }

    if (SSL_CTX_load_verify_locations(context, options.client_ca_file.c_str(), nullptr) != 1)
        throw ServerError(std::format("TLS: cannot load the client CA bundle \"{}\": {}",
                                      options.client_ca_file, drain_openssl_errors()));

    X509NameStackPtr accepted_names(SSL_load_client_CA_file(options.client_ca_file.c_str()));
    if (!accepted_names)
        throw ServerError(std::format("TLS: cannot read subject names from the client CA bundle "
                                      "\"{}\": {}",
                                      options.client_ca_file, drain_openssl_errors()));
    SSL_CTX_set_client_CA_list(context, accepted_names.release());

    // Bound unauthenticated signature-verification work.
    SSL_CTX_set_verify_depth(context, kMaxClientCertificateChainDepth);

    int verify_mode = SSL_VERIFY_PEER;
    if (options.require_client_certificate)
        // SSL_VERIFY_PEER alone still permits a missing certificate.
        verify_mode |= SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
    SSL_CTX_set_verify(context, verify_mode, nullptr);
}

[[nodiscard]] std::chrono::system_clock::time_point leaf_certificate_not_after(SSL_CTX* context) {
    X509* const leaf = SSL_CTX_get0_certificate(context);
    if (leaf == nullptr)
        return {};
    const ASN1_TIME* const not_after = X509_get0_notAfter(leaf);
    if (not_after == nullptr)
        return {};

    int day_difference = 0;
    int second_difference = 0;
    if (ASN1_TIME_diff(&day_difference, &second_difference, nullptr, not_after) != 1)
        return {};
    return std::chrono::time_point_cast<std::chrono::system_clock::duration>(
        std::chrono::system_clock::now() + std::chrono::days(day_difference) +
        std::chrono::seconds(second_difference));
}

void warn_when_certificate_already_expired(std::chrono::system_clock::time_point not_after,
                                           Logger& logger) {
    if (not_after == std::chrono::system_clock::time_point{})
        return;
    if (not_after > std::chrono::system_clock::now())
        return;
    logger.writef(LogLevel::Warning,
                  "tls: the configured certificate expired at {:%Y-%m-%d %H:%M:%S} UTC",
                  std::chrono::floor<std::chrono::seconds>(not_after));
}

} // namespace

#else // ERIKSLUND_HTTP_TLS

namespace {

inline constexpr const char* kNoTlsSupportMessage =
    "erikslund-http was built without TLS support; reconfigure with -DERIKSLUND_HTTP_TLS=ON";

} // namespace

#endif // ERIKSLUND_HTTP_TLS

TlsContext::~TlsContext() {
#if ERIKSLUND_HTTP_TLS
    SSL_CTX_free(context_);
#endif
}

// Build a new immutable context for reloads. In-flight handshakes retain the old one, and invalid
// replacement material never disturbs the active listener.
std::shared_ptr<TlsContext> TlsContext::create(const TlsOptions& options, Logger& logger) {
#if ERIKSLUND_HTTP_TLS
    ERR_clear_error();

    SslCtxPtr context(SSL_CTX_new(TLS_server_method()));
    if (!context)
        throw ServerError(std::format("TLS: SSL_CTX_new failed: {}", drain_openssl_errors()));

    reject_unsupported_alpn(options);
    apply_protocol_floor(context.get(), options);
    apply_hardening_options(context.get(), options);
    apply_group_list(context.get(), options);
    apply_session_policy(context.get(), options);
    SSL_CTX_set_alpn_select_cb(context.get(), &select_alpn_protocol, nullptr);
    load_certificate_material(context.get(), options);
    configure_client_verification(context.get(), options);

    const std::chrono::system_clock::time_point not_after =
        leaf_certificate_not_after(context.get());
    warn_when_certificate_already_expired(not_after, logger);

    std::unique_ptr<TlsContext> loaded(new TlsContext());
    loaded->not_after_ = not_after;
    loaded->requires_client_certificate_ = options.require_client_certificate;
    loaded->logger_ = &logger;
    loaded->context_ = context.release();

    // Do not leak setup errors into the first handshake on this thread.
    ERR_clear_error();
    return std::shared_ptr<TlsContext>(std::move(loaded));
#else
    static_cast<void>(options);
    static_cast<void>(logger);
    throw ServerError(kNoTlsSupportMessage);
#endif
}

std::optional<TlsTransport> TlsContext::make_transport(int fd) const {
#if ERIKSLUND_HTTP_TLS
    ERIKSLUND_HTTP_ASSERT(fd >= 0);
    if (context_ == nullptr)
        return std::nullopt;

    ERR_clear_error();
    SSL* const ssl = SSL_new(context_);
    if (ssl == nullptr) {
        logger_->write_peerf(LogLevel::Error, "tls: SSL_new failed: {}",
                             drain_openssl_errors());
        return std::nullopt;
    }

    // SSL_set_fd uses BIO_NOCLOSE; Connection remains the descriptor owner.
    if (SSL_set_fd(ssl, fd) != 1) {
        const std::string detail = drain_openssl_errors();
        SSL_free(ssl);
        logger_->write_peerf(LogLevel::Error, "tls: SSL_set_fd failed: {}", detail);
        return std::nullopt;
    }

    SSL_set_accept_state(ssl);
    return TlsTransport(fd, ssl, *logger_);
#else
    static_cast<void>(fd);
    return std::nullopt;
#endif
}

} // namespace erikslund::http::internal
