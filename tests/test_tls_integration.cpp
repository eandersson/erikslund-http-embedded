
#include "erikslund/http/build_config.hpp"

#if ERIKSLUND_HTTP_TLS

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <fstream>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <openssl/asn1.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <doctest/doctest.h>

#include "erikslund/http/request.hpp"
#include "erikslund/http/response.hpp"
#include "erikslund/http/router.hpp"
#include "erikslund/http/server.hpp"
#include "erikslund/http/tls.hpp"
#include "support/test_client.hpp"

namespace erikslund::http {
namespace {

using test::HttpResponse;
using test::loopback_options;
using test::simple_request;
using test::started_test_server;
using test::TestClient;
using test::TlsClientOptions;

constexpr std::string_view kHelloBody = "hello, world\n";
constexpr std::string_view kSecureBody = "secure\n";
constexpr std::string_view kPlainBody = "plain\n";

constexpr std::string_view kHttp11Protocol = "http/1.1";
constexpr std::string_view kHttp2Protocol = "h2";

constexpr int kOkStatus = 200;

constexpr std::string_view kFirstCommonName = "erikslund-http-first";
constexpr std::string_view kSecondCommonName = "erikslund-http-second";


constexpr std::string_view kCommonNameType = "CN";
constexpr std::string_view kOrganizationalUnitType = "OU";

constexpr std::string_view kPlainClientCommonName = "erikslund-http-client";
constexpr std::string_view kGuestCommonName = "guest";
constexpr std::string_view kAdminCommonName = "admin";
constexpr std::string_view kOperationsUnit = "ops";

constexpr std::string_view kCraftedCommonName = "guest,CN=admin";
constexpr std::string_view kImpersonatedRelativeName = "CN=admin";

constexpr std::string_view kCraftedOnelineCommonName = "guest/CN=admin";
constexpr std::string_view kImpersonatedOnelineRelativeName = "/CN=admin";

constexpr std::string_view kHostileCommonName =
    R"( guest,CN=admin+OU=ops="quoted"\<tag>;#hash )";

constexpr std::string_view kNulCommonName{"admin\0.example", sizeof("admin\0.example") - 1};

constexpr std::string_view kIntermediateCommonNamePrefix = "erikslund-http-intermediate";

constexpr std::string_view kCraftedIdentity = R"(CN=guest\,CN\3Dadmin)";
constexpr std::string_view kComposedIdentity = "CN=admin,CN=guest";
constexpr std::string_view kHostileIdentity =
    R"(CN=\ guest\,CN\3Dadmin\+OU\3Dops\3D\"quoted\"\\\<tag\>\;#hash\ )";
constexpr std::string_view kNulIdentity = R"(CN=admin\00.example)";

constexpr std::string_view kNoIdentityBody = "(none)";

constexpr char kLowestPrintableAscii = ' ';
constexpr char kHighestPrintableAscii = '~';

constexpr long kCertificateLifetimeSeconds = 31'536'000;
constexpr long kCertificateNotBeforeOffsetSeconds = 0;
constexpr long kCertificateSerialNumber = 1;

constexpr long kX509Version3 = 2;
constexpr size_t kSubjectTextBytes = 256;

constexpr size_t kTlsRecordHeaderBytes = 5;
constexpr std::array<unsigned char, kTlsRecordHeaderBytes> kTlsRecordHeader{
    0x16, 0x03, 0x01, 0x02, 0x00};
constexpr size_t kTlsRecordFillerBytes = 512;
constexpr char kTlsRecordFillerByte = 'A';

constexpr std::chrono::milliseconds kProtocolConfusionHeaderTimeout{800};
constexpr std::chrono::milliseconds kProtocolConfusionBound{4'000};

constexpr size_t kPlaintextListenerIndex = 0;
constexpr size_t kSecureListenerIndex = 1;
constexpr size_t kPinningListenerIndex = 0;
constexpr size_t kUnpinnedListenerIndex = 1;


struct EvpPkeyDeleter {
    void operator()(EVP_PKEY* key) const noexcept { EVP_PKEY_free(key); }
};
using EvpPkeyPtr = std::unique_ptr<EVP_PKEY, EvpPkeyDeleter>;

struct X509Deleter {
    void operator()(X509* certificate) const noexcept { X509_free(certificate); }
};
using X509Ptr = std::unique_ptr<X509, X509Deleter>;

struct BioDeleter {
    void operator()(BIO* bio) const noexcept { BIO_free(bio); }
};
using BioPtr = std::unique_ptr<BIO, BioDeleter>;

struct X509ExtensionDeleter {
    void operator()(X509_EXTENSION* extension) const noexcept { X509_EXTENSION_free(extension); }
};
using X509ExtensionPtr = std::unique_ptr<X509_EXTENSION, X509ExtensionDeleter>;

struct SslCtxDeleter {
    void operator()(SSL_CTX* context) const noexcept { SSL_CTX_free(context); }
};
using SslCtxPtr = std::unique_ptr<SSL_CTX, SslCtxDeleter>;

struct SslDeleter {
    void operator()(SSL* session) const noexcept { SSL_free(session); }
};
using SslPtr = std::unique_ptr<SSL, SslDeleter>;

struct CertificateMaterial {
    std::string certificate_pem;
    std::string private_key_pem;

    [[nodiscard]] bool valid() const noexcept {
        return !certificate_pem.empty() && !private_key_pem.empty();
    }
};

[[nodiscard]] bool add_extension(X509* certificate, int nid, const char* value) {
    X509V3_CTX context{};
    X509V3_set_ctx_nodb(&context);
    X509V3_set_ctx(&context, certificate, certificate, nullptr, nullptr, 0);
    const X509ExtensionPtr extension(X509V3_EXT_conf_nid(nullptr, &context, nid, value));
    if (!extension)
        return false;
    return X509_add_ext(certificate, extension.get(), -1) == 1;
}

[[nodiscard]] std::string read_bio(BIO* bio) {
    char* data = nullptr;
    const long length = BIO_get_mem_data(bio, &data);
    if (data == nullptr || length <= 0)
        return {};
    return std::string(data, static_cast<size_t>(length));
}

[[nodiscard]] CertificateMaterial mint_self_signed_certificate(std::string_view common_name) {
    CertificateMaterial material;

    const EvpPkeyPtr key(EVP_EC_gen("P-256"));
    if (!key)
        return material;

    const X509Ptr certificate(X509_new());
    if (!certificate)
        return material;

    if (X509_set_version(certificate.get(), kX509Version3) != 1)
        return material;
    if (ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), kCertificateSerialNumber) != 1)
        return material;
    if (X509_gmtime_adj(X509_getm_notBefore(certificate.get()),
                        kCertificateNotBeforeOffsetSeconds) == nullptr)
        return material;
    if (X509_gmtime_adj(X509_getm_notAfter(certificate.get()), kCertificateLifetimeSeconds) ==
        nullptr)
        return material;
    if (X509_set_pubkey(certificate.get(), key.get()) != 1)
        return material;

    X509_NAME* const subject = X509_get_subject_name(certificate.get());
    const std::string name_text(common_name);
    if (X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
                                   reinterpret_cast<const unsigned char*>(name_text.c_str()), -1,
                                   -1, 0) != 1)
        return material;
    if (X509_set_issuer_name(certificate.get(), subject) != 1)
        return material;

    if (!add_extension(certificate.get(), NID_basic_constraints, "critical,CA:TRUE"))
        return material;
    if (!add_extension(certificate.get(), NID_subject_alt_name, "DNS:localhost,IP:127.0.0.1"))
        return material;

    if (X509_sign(certificate.get(), key.get(), EVP_sha256()) == 0)
        return material;

    const BioPtr certificate_bio(BIO_new(BIO_s_mem()));
    const BioPtr key_bio(BIO_new(BIO_s_mem()));
    if (!certificate_bio || !key_bio)
        return material;
    if (PEM_write_bio_X509(certificate_bio.get(), certificate.get()) != 1)
        return material;
    if (PEM_write_bio_PrivateKey(key_bio.get(), key.get(), nullptr, nullptr, 0, nullptr,
                                 nullptr) != 1)
        return material;

    material.certificate_pem = read_bio(certificate_bio.get());
    material.private_key_pem = read_bio(key_bio.get());
    ERR_clear_error();
    return material;
}


struct RelativeName {
    std::string_view type;
    std::string_view value;
    bool joins_previous = false;
};

enum class CertificateRole : uint8_t { Leaf, Authority };

[[nodiscard]] X509Ptr read_certificate_pem(std::string_view pem) {
    const BioPtr source(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
    if (!source)
        return nullptr;
    return X509Ptr(PEM_read_bio_X509(source.get(), nullptr, nullptr, nullptr));
}

[[nodiscard]] EvpPkeyPtr read_private_key_pem(std::string_view pem) {
    const BioPtr source(BIO_new_mem_buf(pem.data(), static_cast<int>(pem.size())));
    if (!source)
        return nullptr;
    return EvpPkeyPtr(PEM_read_bio_PrivateKey(source.get(), nullptr, nullptr, nullptr));
}

[[nodiscard]] CertificateMaterial mint_issued_certificate(std::span<const RelativeName> subject,
                                                          const CertificateMaterial& issuer,
                                                          CertificateRole role) {
    CertificateMaterial material;
    const X509Ptr issuer_certificate = read_certificate_pem(issuer.certificate_pem);
    const EvpPkeyPtr issuer_key = read_private_key_pem(issuer.private_key_pem);
    if (!issuer_certificate || !issuer_key)
        return material;

    const EvpPkeyPtr key(EVP_EC_gen("P-256"));
    const X509Ptr certificate(X509_new());
    if (!key || !certificate)
        return material;

    if (X509_set_version(certificate.get(), kX509Version3) != 1)
        return material;
    if (ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), kCertificateSerialNumber) != 1)
        return material;
    if (X509_gmtime_adj(X509_getm_notBefore(certificate.get()),
                        kCertificateNotBeforeOffsetSeconds) == nullptr)
        return material;
    if (X509_gmtime_adj(X509_getm_notAfter(certificate.get()), kCertificateLifetimeSeconds) ==
        nullptr)
        return material;
    if (X509_set_pubkey(certificate.get(), key.get()) != 1)
        return material;

    X509_NAME* const name = X509_get_subject_name(certificate.get());
    for (const RelativeName& entry : subject) {
        const std::string type(entry.type);
        const int set = entry.joins_previous ? -1 : 0;
        if (X509_NAME_add_entry_by_txt(name, type.c_str(), MBSTRING_ASC,
                                       reinterpret_cast<const unsigned char*>(entry.value.data()),
                                       static_cast<int>(entry.value.size()), -1, set) != 1)
            return material;
    }
    if (X509_set_issuer_name(certificate.get(),
                             X509_get_subject_name(issuer_certificate.get())) != 1)
        return material;

    const char* const constraints =
        role == CertificateRole::Authority ? "critical,CA:TRUE" : "critical,CA:FALSE";
    if (!add_extension(certificate.get(), NID_basic_constraints, constraints))
        return material;
    if (role == CertificateRole::Leaf &&
        !add_extension(certificate.get(), NID_ext_key_usage, "clientAuth"))
        return material;

    if (X509_sign(certificate.get(), issuer_key.get(), EVP_sha256()) == 0)
        return material;

    const BioPtr certificate_bio(BIO_new(BIO_s_mem()));
    const BioPtr key_bio(BIO_new(BIO_s_mem()));
    if (!certificate_bio || !key_bio)
        return material;
    if (PEM_write_bio_X509(certificate_bio.get(), certificate.get()) != 1)
        return material;
    if (PEM_write_bio_PrivateKey(key_bio.get(), key.get(), nullptr, nullptr, 0, nullptr,
                                 nullptr) != 1)
        return material;

    material.certificate_pem = read_bio(certificate_bio.get());
    material.private_key_pem = read_bio(key_bio.get());
    ERR_clear_error();
    return material;
}

[[nodiscard]] CertificateMaterial mint_client_certificate(std::span<const RelativeName> subject,
                                                          const CertificateMaterial& issuer) {
    return mint_issued_certificate(subject, issuer, CertificateRole::Leaf);
}

[[nodiscard]] CertificateMaterial mint_client_behind_intermediates(const CertificateMaterial& root,
                                                                   size_t intermediate_count) {
    CertificateMaterial issuer = root;
    std::vector<std::string> intermediates;
    for (size_t level = 0; level < intermediate_count; ++level) {
        const std::string common_name =
            std::format("{}-{}", kIntermediateCommonNamePrefix, level);
        const std::array<RelativeName, 1> subject{RelativeName{kCommonNameType, common_name}};
        issuer = mint_issued_certificate(subject, issuer, CertificateRole::Authority);
        if (!issuer.valid())
            return {};
        intermediates.push_back(issuer.certificate_pem);
    }

    const std::array<RelativeName, 1> leaf{RelativeName{kCommonNameType, kPlainClientCommonName}};
    CertificateMaterial client = mint_client_certificate(leaf, issuer);
    if (!client.valid())
        return {};
    for (size_t level = intermediates.size(); level > 0; --level)
        client.certificate_pem += intermediates[level - 1];
    return client;
}

class TemporaryPemFile {
public:
    TemporaryPemFile(std::string_view label, std::string_view contents) {
        static std::atomic<unsigned> sequence{0};
        const std::filesystem::path target =
            std::filesystem::temp_directory_path() /
            std::format("erikslund-http-test-{}-{}-{}.pem", label, ::getpid(),
                        sequence.fetch_add(1));
        path_ = target.string();
        if (!write(contents))
            path_.clear();
    }

    ~TemporaryPemFile() {
        if (path_.empty())
            return;
        std::error_code ignored;
        static_cast<void>(std::filesystem::remove(path_, ignored));
    }

    TemporaryPemFile(const TemporaryPemFile&) = delete;
    TemporaryPemFile& operator=(const TemporaryPemFile&) = delete;
    TemporaryPemFile(TemporaryPemFile&&) = delete;
    TemporaryPemFile& operator=(TemporaryPemFile&&) = delete;

    [[nodiscard]] const std::string& path() const noexcept { return path_; }
    [[nodiscard]] bool valid() const noexcept { return !path_.empty(); }

    [[nodiscard]] bool write(std::string_view contents) const {
        std::ofstream out(path_, std::ios::binary | std::ios::trunc);
        if (!out)
            return false;
        out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
        out.close();
        return static_cast<bool>(out);
    }

private:
    std::string path_;
};

[[nodiscard]] std::string presented_certificate_subject(uint16_t port) {
    const SslCtxPtr context(SSL_CTX_new(TLS_client_method()));
    if (!context)
        return {};
    SSL_CTX_set_verify(context.get(), SSL_VERIFY_NONE, nullptr);

    const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0)
        return {};

    std::string subject;
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = ::htons(port);
    if (::inet_pton(AF_INET, test::kLoopbackIpv4, &address.sin_addr) == 1 &&
        ::connect(fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0) {
        const SslPtr session(SSL_new(context.get()));
        if (session && SSL_set_fd(session.get(), fd) == 1 && SSL_connect(session.get()) == 1) {
            const X509Ptr peer(SSL_get1_peer_certificate(session.get()));
            std::array<char, kSubjectTextBytes> text{};
            if (peer && X509_NAME_oneline(X509_get_subject_name(peer.get()), text.data(),
                                          static_cast<int>(text.size())) != nullptr)
                subject = text.data();
        }
    }
    ::close(fd);
    ERR_clear_error();
    return subject;
}


[[nodiscard]] Router make_test_router() {
    Router router;
    router.get("/hello", [](const Request&) { return Response::text(std::string(kHelloBody)); });
    router.get("/secure", [](const Request& request) {
        return Response::text(request.is_secure() ? std::string(kSecureBody)
                                                  : std::string(kPlainBody));
    });
    return router;
}

[[nodiscard]] Listener plaintext_listener() {
    Listener listener;
    listener.bind_address = test::kLoopbackIpv4;
    listener.port = 0;
    return listener;
}

[[nodiscard]] Listener secure_listener() {
    Listener listener = plaintext_listener();
    listener.tls.enabled = true;
    listener.tls.certificate_chain_file = ERIKSLUND_HTTP_TEST_CERTIFICATE_FILE;
    listener.tls.private_key_file = ERIKSLUND_HTTP_TEST_PRIVATE_KEY_FILE;
    return listener;
}

[[nodiscard]] Listener secure_listener_from(const TemporaryPemFile& certificate,
                                            const TemporaryPemFile& private_key) {
    Listener listener = plaintext_listener();
    listener.tls.enabled = true;
    listener.tls.certificate_chain_file = certificate.path();
    listener.tls.private_key_file = private_key.path();
    return listener;
}

[[nodiscard]] ServerOptions options_with(Listener listener) {
    ServerOptions options = loopback_options();
    options.listeners.clear();
    options.listeners.push_back(std::move(listener));
    return options;
}

[[nodiscard]] TlsClientOptions offering(std::string_view protocol) {
    TlsClientOptions tls_options;
    tls_options.alpn_protocol = std::string(protocol);
    return tls_options;
}

[[nodiscard]] Router make_identity_router() {
    Router router;
    router.get("/identity", [](const Request& request) {
        const std::optional<std::string_view> identity = request.client_certificate_subject();
        return Response::text(identity.has_value() ? std::string(*identity)
                                                   : std::string(kNoIdentityBody));
    });
    return router;
}

[[nodiscard]] std::optional<std::string> identity_seen_by_handler(
    const CertificateMaterial& authority, const CertificateMaterial& client) {
    const TemporaryPemFile authority_certificate("ca-cert", authority.certificate_pem);
    const TemporaryPemFile authority_key("ca-key", authority.private_key_pem);
    const TemporaryPemFile client_certificate("client-cert", client.certificate_pem);
    const TemporaryPemFile client_key("client-key", client.private_key_pem);
    if (!authority_certificate.valid() || !authority_key.valid() ||
        !client_certificate.valid() || !client_key.valid())
        return std::nullopt;

    Listener mutual = secure_listener_from(authority_certificate, authority_key);
    mutual.tls.client_ca_file = authority_certificate.path();
    mutual.tls.require_client_certificate = true;
    const auto fixture =
        started_test_server(make_identity_router(), options_with(std::move(mutual)));

    TlsClientOptions tls_options = offering(kHttp11Protocol);
    tls_options.certificate_file = client_certificate.path();
    tls_options.private_key_file = client_key.path();

    TestClient authenticated;
    if (!authenticated.connect_tls(fixture->port(), tls_options))
        return std::nullopt;
    const std::optional<HttpResponse> response =
        authenticated.request(simple_request("GET", "/identity"));
    if (!response.has_value() || !response->complete || response->status_code != kOkStatus)
        return std::nullopt;
    return response->body;
}

[[nodiscard]] std::optional<std::string> identity_of_subject(
    std::span<const RelativeName> subject) {
    const CertificateMaterial authority = mint_self_signed_certificate(kFirstCommonName);
    if (!authority.valid())
        return std::nullopt;
    const CertificateMaterial client = mint_client_certificate(subject, authority);
    if (!client.valid())
        return std::nullopt;
    return identity_seen_by_handler(authority, client);
}

[[nodiscard]] bool is_printable_ascii(std::string_view text) noexcept {
    for (const char character : text)
        if (character < kLowestPrintableAscii || character > kHighestPrintableAscii)
            return false;
    return true;
}

} // namespace


TEST_CASE("completes_a_tls_1_3_handshake_and_serves_a_200") {
    const auto fixture = started_test_server(make_test_router(), options_with(secure_listener()));

    TlsClientOptions tls_options = offering(kHttp11Protocol);
    tls_options.minimum_version = TLS1_3_VERSION;
    tls_options.maximum_version = TLS1_3_VERSION;

    TestClient client;
    REQUIRE_MESSAGE(client.connect_tls(fixture->port(), tls_options),
                    std::format("a TLS 1.3 only client was refused: {}", client.tls_error()));

    const std::optional<HttpResponse> response = client.request(simple_request("GET", "/hello"));
    REQUIRE(response.has_value());
    REQUIRE(response->complete);
    CHECK(response->status_code == kOkStatus);
    CHECK(response->body == kHelloBody);
}

TEST_CASE("refuses_a_client_that_cannot_speak_tls_1_3") {
    const auto fixture = started_test_server(make_test_router(), options_with(secure_listener()));

    TlsClientOptions tls_options = offering(kHttp11Protocol);
    tls_options.maximum_version = TLS1_2_VERSION;

    TestClient client;
    CHECK_MESSAGE(!client.connect_tls(fixture->port(), tls_options),
                  "the configured floor is TLS 1.3, so a client capped at 1.2 must be refused");

    TestClient recovered;
    CHECK(recovered.connect_tls(fixture->port(), offering(kHttp11Protocol)));
}

TEST_CASE("negotiates_the_http_1_1_alpn_protocol") {
    const auto fixture = started_test_server(make_test_router(), options_with(secure_listener()));

    TestClient client;
    REQUIRE(client.connect_tls(fixture->port(), offering(kHttp11Protocol)));
    CHECK(client.negotiated_alpn() == kHttp11Protocol);
}

TEST_CASE("serves_a_client_that_offers_no_alpn_at_all") {
    const auto fixture = started_test_server(make_test_router(), options_with(secure_listener()));

    TestClient client;
    REQUIRE(client.connect_tls(fixture->port(), TlsClientOptions{}));
    CHECK_MESSAGE(client.negotiated_alpn().empty(),
                  "a client that offered nothing must not be told a protocol was negotiated");
    const std::optional<HttpResponse> response = client.request(simple_request("GET", "/hello"));
    REQUIRE(response.has_value());
    CHECK(response->status_code == kOkStatus);
}

TEST_CASE("refuses_the_handshake_of_a_client_that_offers_only_h2") {
    const auto fixture = started_test_server(make_test_router(), options_with(secure_listener()));

    TestClient client;
    CHECK_MESSAGE(!client.connect_tls(fixture->port(), offering(kHttp2Protocol)),
                  "a client offering only h2 must be refused, not admitted without ALPN");
    CHECK_MESSAGE(client.negotiated_alpn().empty(),
                  std::format("the listener settled on \"{}\" for a client that never offered it",
                              client.negotiated_alpn()));

    TestClient recovered;
    CHECK(recovered.connect_tls(fixture->port(), offering(kHttp11Protocol)));
}


TEST_CASE("disconnects_a_plaintext_client_that_talks_to_the_tls_port") {
    const auto fixture = started_test_server(make_test_router(), options_with(secure_listener()));

    TestClient client;
    REQUIRE(client.connect(fixture->port()));
    REQUIRE(client.send_raw(simple_request("GET", "/hello")));

    const std::string received = client.read_until_closed();
    CHECK_MESSAGE(client.saw_end_of_stream(),
                  "cleartext on an https port must be dropped, not left hanging");
    CHECK_MESSAGE(received.find("HTTP/") == std::string::npos,
                  "a TLS listener must never answer in cleartext, whatever was asked of it");
    CHECK_MESSAGE(fixture->log().contains(LogLevel::Error, "tls: SSL_accept failed"),
                  "TLS diagnostics must use the server's configured log sink");

    TestClient recovered;
    CHECK(recovered.connect_tls(fixture->port(), offering(kHttp11Protocol)));
}

TEST_CASE("does_not_leave_a_tls_client_hanging_on_the_plaintext_port") {
    ServerOptions options = options_with(plaintext_listener());
    options.header_timeout = kProtocolConfusionHeaderTimeout;
    const auto fixture = started_test_server(make_test_router(), std::move(options));

    TestClient client;
    REQUIRE(client.connect(fixture->port()));
    std::string first_flight(reinterpret_cast<const char*>(kTlsRecordHeader.data()),
                             kTlsRecordHeader.size());
    first_flight.append(kTlsRecordFillerBytes, kTlsRecordFillerByte);
    REQUIRE(client.send_raw(first_flight));

    const auto began = std::chrono::steady_clock::now();
    const std::string received = client.read_until_closed(kProtocolConfusionBound);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - began);

    CHECK_MESSAGE(client.saw_end_of_stream(),
                  std::format("a TLS client on the cleartext port was still connected after {} ms",
                              elapsed.count()));
    if (!received.empty())
        CHECK_MESSAGE(std::string_view(received).starts_with("HTTP/1.1 4"),
                      std::format("a handshake that is not HTTP deserves a 4xx, not:\n{}",
                                  received));
}


TEST_CASE("reports_is_secure_as_true_on_the_tls_listener_and_false_on_the_plaintext_one") {
    ServerOptions options = options_with(plaintext_listener());
    options.listeners.push_back(secure_listener());
    const auto fixture = started_test_server(make_test_router(), std::move(options));

    TestClient cleartext;
    REQUIRE(cleartext.connect(fixture->port(kPlaintextListenerIndex)));
    const std::optional<HttpResponse> over_cleartext =
        cleartext.request(simple_request("GET", "/secure"));
    REQUIRE(over_cleartext.has_value());
    CHECK(over_cleartext->body == kPlainBody);

    TestClient secure;
    REQUIRE(secure.connect_tls(fixture->port(kSecureListenerIndex), offering(kHttp11Protocol)));
    const std::optional<HttpResponse> over_tls = secure.request(simple_request("GET", "/secure"));
    REQUIRE(over_tls.has_value());
    CHECK_MESSAGE(over_tls->body == kSecureBody,
                  "a handler on a TLS listener has to be able to tell that it is on one");
}

TEST_CASE("sends_strict_transport_security_on_the_tls_listener_only") {
    ServerOptions options = options_with(plaintext_listener());
    Listener pinning = secure_listener();
    pinning.tls.strict_transport_security = true;
    options.listeners.push_back(std::move(pinning));
    const auto fixture = started_test_server(make_test_router(), std::move(options));

    TestClient cleartext;
    REQUIRE(cleartext.connect(fixture->port(kPlaintextListenerIndex)));
    const std::optional<HttpResponse> over_cleartext =
        cleartext.request(simple_request("GET", "/hello"));
    REQUIRE(over_cleartext.has_value());
    CHECK_MESSAGE(!over_cleartext->has_header("Strict-Transport-Security"),
                  "HSTS over cleartext is a promise the listener carrying it cannot keep");

    TestClient secure;
    REQUIRE(secure.connect_tls(fixture->port(kSecureListenerIndex), offering(kHttp11Protocol)));
    const std::optional<HttpResponse> over_tls = secure.request(simple_request("GET", "/hello"));
    REQUIRE(over_tls.has_value());
    CHECK(over_tls->header_value("Strict-Transport-Security") ==
          std::format("max-age={}", kDefaultHstsMaxAgeSeconds));
}

TEST_CASE("sends_strict_transport_security_only_on_the_listener_that_enabled_it") {
    Listener pinning = secure_listener();
    pinning.tls.strict_transport_security = true;
    ServerOptions options = options_with(std::move(pinning));
    options.listeners.push_back(secure_listener());
    const auto fixture = started_test_server(make_test_router(), std::move(options));

    TestClient pinned;
    REQUIRE(pinned.connect_tls(fixture->port(kPinningListenerIndex), offering(kHttp11Protocol)));
    const std::optional<HttpResponse> from_pinning =
        pinned.request(simple_request("GET", "/hello"));
    REQUIRE(from_pinning.has_value());
    CHECK(from_pinning->has_header("Strict-Transport-Security"));

    TestClient unpinned;
    REQUIRE(unpinned.connect_tls(fixture->port(kUnpinnedListenerIndex),
                                 offering(kHttp11Protocol)));
    const std::optional<HttpResponse> from_unpinning =
        unpinned.request(simple_request("GET", "/hello"));
    REQUIRE(from_unpinning.has_value());
    CHECK_MESSAGE(!from_unpinning->has_header("Strict-Transport-Security"),
                  "HSTS belongs to the listener that enabled it, not to every TLS response the "
                  "process happens to write");
}


TEST_CASE("swaps_the_certificate_on_reload_tls_while_an_open_connection_keeps_the_old_one") {
    const CertificateMaterial first = mint_self_signed_certificate(kFirstCommonName);
    const CertificateMaterial second = mint_self_signed_certificate(kSecondCommonName);
    REQUIRE(first.valid());
    REQUIRE(second.valid());

    const TemporaryPemFile certificate("cert", first.certificate_pem);
    const TemporaryPemFile private_key("key", first.private_key_pem);
    REQUIRE(certificate.valid());
    REQUIRE(private_key.valid());

    const auto fixture = started_test_server(
        make_test_router(), options_with(secure_listener_from(certificate, private_key)));

    CHECK(presented_certificate_subject(fixture->port()).find(kFirstCommonName) !=
          std::string::npos);

    TestClient established;
    REQUIRE(established.connect_tls(fixture->port(), offering(kHttp11Protocol)));
    REQUIRE(established.request(simple_request("GET", "/hello")).has_value());

    REQUIRE(certificate.write(second.certificate_pem));
    REQUIRE(private_key.write(second.private_key_pem));
    fixture->server().reload_tls();

    const std::optional<HttpResponse> after_reload =
        established.request(simple_request("GET", "/hello"));
    REQUIRE_MESSAGE(after_reload.has_value(),
                    "the connection established before the reload has to keep working on the "
                    "context it started with");
    CHECK(after_reload->status_code == kOkStatus);

    const std::string renewed = presented_certificate_subject(fixture->port());
    CHECK_MESSAGE(renewed.find(kSecondCommonName) != std::string::npos,
                  std::format("a connection made after the reload was served the old certificate: "
                              "subject is \"{}\"",
                              renewed));
}

TEST_CASE("refuses_a_client_with_no_certificate_on_a_listener_that_requires_one") {
    const CertificateMaterial material = mint_self_signed_certificate(kFirstCommonName);
    REQUIRE(material.valid());
    const TemporaryPemFile certificate("ca-cert", material.certificate_pem);
    const TemporaryPemFile private_key("ca-key", material.private_key_pem);
    REQUIRE(certificate.valid());
    REQUIRE(private_key.valid());

    Listener mutual = secure_listener_from(certificate, private_key);
    mutual.tls.client_ca_file = certificate.path();
    mutual.tls.require_client_certificate = true;
    const auto fixture = started_test_server(make_test_router(), options_with(std::move(mutual)));

    TestClient anonymous;
    bool answered = false;
    if (anonymous.connect_tls(fixture->port(), offering(kHttp11Protocol))) {
        const std::optional<HttpResponse> response =
            anonymous.request(simple_request("GET", "/hello"));
        answered = response.has_value() && response->complete &&
                   response->status_code == kOkStatus;
    }
    CHECK_MESSAGE(!answered,
                  "a listener requiring a client certificate served a client that had none");
}


TEST_CASE("reports_a_verified_client_certificate_subject_as_an_rfc_2253_distinguished_name") {
    const std::array<RelativeName, 1> subject{
        RelativeName{kCommonNameType, kPlainClientCommonName}};
    const std::optional<std::string> identity = identity_of_subject(subject);
    REQUIRE(identity.has_value());
    CHECK(*identity == std::format("{}={}", kCommonNameType, kPlainClientCommonName));
    CHECK(client_identity_has_attribute(*identity, kCommonNameType, kPlainClientCommonName));
}

TEST_CASE("a_common_name_holding_a_relative_name_separator_cannot_impersonate_a_composed_subject") {
    const std::array<RelativeName, 1> crafted{RelativeName{kCommonNameType, kCraftedCommonName}};
    const std::array<RelativeName, 2> composed{RelativeName{kCommonNameType, kGuestCommonName},
                                               RelativeName{kCommonNameType, kAdminCommonName}};

    const std::optional<std::string> crafted_identity = identity_of_subject(crafted);
    const std::optional<std::string> composed_identity = identity_of_subject(composed);
    REQUIRE(crafted_identity.has_value());
    REQUIRE(composed_identity.has_value());

    CHECK(*crafted_identity == kCraftedIdentity);
    CHECK(*composed_identity == kComposedIdentity);
    CHECK_MESSAGE(*crafted_identity != *composed_identity,
                  "two different subjects produced the same identity string");

    CHECK_MESSAGE(crafted_identity->find(kImpersonatedRelativeName) == std::string::npos,
                  std::format("a crafted common name produced the identity \"{}\", which contains "
                              "the relative name \"{}\" the certificate does not carry",
                              *crafted_identity, kImpersonatedRelativeName));
    CHECK(composed_identity->find(kImpersonatedRelativeName) != std::string::npos);

    CHECK_FALSE(client_identity_has_attribute(*crafted_identity, kCommonNameType,
                                              kAdminCommonName));
    CHECK(client_identity_has_attribute(*composed_identity, kCommonNameType, kAdminCommonName));
    CHECK(client_identity_has_attribute(*composed_identity, kCommonNameType, kGuestCommonName));

    CHECK(client_identity_has_attribute(*crafted_identity, kCommonNameType, kCraftedCommonName));
}

TEST_CASE("a_common_name_holding_the_legacy_subject_separator_cannot_impersonate_a_relative_name") {
    const std::array<RelativeName, 1> crafted{
        RelativeName{kCommonNameType, kCraftedOnelineCommonName}};
    const std::optional<std::string> identity = identity_of_subject(crafted);
    REQUIRE(identity.has_value());

    CHECK_MESSAGE(identity->find(kImpersonatedOnelineRelativeName) == std::string::npos,
                  std::format("a crafted common name produced the identity \"{}\", which contains "
                              "the relative name \"{}\" the certificate does not carry",
                              *identity, kImpersonatedOnelineRelativeName));
    CHECK_FALSE(client_identity_has_attribute(*identity, kCommonNameType, kAdminCommonName));
    CHECK(client_identity_has_attribute(*identity, kCommonNameType, kCraftedOnelineCommonName));
}

TEST_CASE("escapes_every_rfc_2253_special_character_in_a_client_certificate_common_name") {
    const std::array<RelativeName, 1> subject{RelativeName{kCommonNameType, kHostileCommonName}};
    const std::optional<std::string> identity = identity_of_subject(subject);
    REQUIRE(identity.has_value());

    CHECK(*identity == kHostileIdentity);
    CHECK_MESSAGE(is_printable_ascii(*identity),
                  "the identity has to be safe to put in a log line and a header value, so no byte "
                  "in it may be a control character or sit above ASCII");

    CHECK(client_identity_has_attribute(*identity, kCommonNameType, kHostileCommonName));
    CHECK_FALSE(client_identity_has_attribute(*identity, kCommonNameType, kAdminCommonName));
    CHECK_FALSE(client_identity_has_attribute(*identity, kOrganizationalUnitType, kOperationsUnit));
}

TEST_CASE("distinguishes_a_multi_valued_relative_name_from_two_separate_ones") {
    const std::array<RelativeName, 2> joined{
        RelativeName{kCommonNameType, kAdminCommonName},
        RelativeName{kOrganizationalUnitType, kOperationsUnit, true}};
    const std::array<RelativeName, 2> separate{
        RelativeName{kCommonNameType, kAdminCommonName},
        RelativeName{kOrganizationalUnitType, kOperationsUnit}};

    const std::optional<std::string> joined_identity = identity_of_subject(joined);
    const std::optional<std::string> separate_identity = identity_of_subject(separate);
    REQUIRE(joined_identity.has_value());
    REQUIRE(separate_identity.has_value());

    CHECK(*joined_identity != *separate_identity);
    CHECK(joined_identity->find(kRelativeNameValueJoiner) != std::string::npos);
    CHECK(joined_identity->find(kRelativeNameSeparator) == std::string::npos);
    CHECK(separate_identity->find(kRelativeNameSeparator) != std::string::npos);

    for (const std::string& identity : {*joined_identity, *separate_identity}) {
        CHECK(client_identity_has_attribute(identity, kCommonNameType, kAdminCommonName));
        CHECK(client_identity_has_attribute(identity, kOrganizationalUnitType, kOperationsUnit));
        CHECK_FALSE(client_identity_has_attribute(identity, kCommonNameType, kOperationsUnit));
    }
}

TEST_CASE("escapes_a_nul_inside_a_common_name_rather_than_letting_it_end_the_name") {
    const std::array<RelativeName, 1> subject{RelativeName{kCommonNameType, kNulCommonName}};
    const std::optional<std::string> identity = identity_of_subject(subject);
    REQUIRE(identity.has_value());

    CHECK(*identity == kNulIdentity);
    CHECK(is_printable_ascii(*identity));
    CHECK_FALSE(client_identity_has_attribute(*identity, kCommonNameType, kAdminCommonName));
    CHECK(client_identity_has_attribute(*identity, kCommonNameType, kNulCommonName));
}

TEST_CASE("reports_no_identity_for_a_tls_listener_that_verifies_no_client_certificate") {
    const auto fixture =
        started_test_server(make_identity_router(), options_with(secure_listener()));

    TestClient client;
    REQUIRE(client.connect_tls(fixture->port(), offering(kHttp11Protocol)));
    const std::optional<HttpResponse> response = client.request(simple_request("GET", "/identity"));
    REQUIRE(response.has_value());
    CHECK_MESSAGE(response->body == kNoIdentityBody,
                  "a listener with no client CA verifies nothing, so it has no identity to report "
                  "-- and SSL_get_verify_result answers X509_V_OK there precisely because nothing "
                  "was checked");
}

TEST_CASE("serves_a_client_chain_within_the_verification_depth_and_refuses_one_past_it") {
    const CertificateMaterial authority = mint_self_signed_certificate(kFirstCommonName);
    REQUIRE(authority.valid());

    const auto within = static_cast<size_t>(kMaxClientCertificateChainDepth);
    const CertificateMaterial shallow = mint_client_behind_intermediates(authority, within);
    REQUIRE(shallow.valid());
    CHECK_MESSAGE(identity_seen_by_handler(authority, shallow).has_value(),
                  std::format("a chain with {} intermediate authorities is inside the configured "
                              "depth and has to verify",
                              within));

    const CertificateMaterial deep = mint_client_behind_intermediates(authority, within + 1);
    REQUIRE(deep.valid());
    CHECK_MESSAGE(!identity_seen_by_handler(authority, deep).has_value(),
                  std::format("a chain with {} intermediate authorities is past the configured "
                              "depth, and every level past it is another signature an "
                              "unauthenticated peer can make the server verify",
                              within + 1));
}

} // namespace erikslund::http

#endif // ERIKSLUND_HTTP_TLS
