// Exercise the production DTLS context and its failure paths without an X server.
#include <cstdio>
#include <cstddef>
#include <cstring>
#include <memory>
#include <openssl/err.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include "network/webudp/Wu.cpp"

namespace {
enum class Failure { None, Context, KeyInit, Keygen, Certificate, SerialAllocation, SerialRandom, SerialAssignment, Sign, Digest, DigestLength };
Failure failure = Failure::None;
int failures = 0;
void check(const char* name, bool passed) {
  std::printf("%s: %s\n", passed ? "PASS" : "FAIL", name);
  if (!passed) {
    ++failures;
    ERR_print_errors_fp(stderr);
  }
}
}

extern "C" {
EVP_PKEY_CTX* __real_EVP_PKEY_CTX_new_id(int, ENGINE*);
EVP_PKEY_CTX* __wrap_EVP_PKEY_CTX_new_id(int id, ENGINE* engine) {
  return failure == Failure::Context ? nullptr : __real_EVP_PKEY_CTX_new_id(id, engine);
}
int __real_EVP_PKEY_keygen_init(EVP_PKEY_CTX*);
int __wrap_EVP_PKEY_keygen_init(EVP_PKEY_CTX* context) {
  return failure == Failure::KeyInit ? 0 : __real_EVP_PKEY_keygen_init(context);
}
int __real_EVP_PKEY_keygen(EVP_PKEY_CTX*, EVP_PKEY**);
int __wrap_EVP_PKEY_keygen(EVP_PKEY_CTX* context, EVP_PKEY** key) {
  // Return an allocated key even on the injected failure to exercise cleanup.
  const int result = __real_EVP_PKEY_keygen(context, key);
  return failure == Failure::Keygen ? 0 : result;
}
X509* __real_X509_new();
X509* __wrap_X509_new() {
  return failure == Failure::Certificate ? nullptr : __real_X509_new();
}
BIGNUM* __real_BN_new();
BIGNUM* __wrap_BN_new() {
  return failure == Failure::SerialAllocation ? nullptr : __real_BN_new();
}
int __real_BN_rand(BIGNUM*, int, int, int);
int __wrap_BN_rand(BIGNUM* value, int bits, int top, int bottom) {
  return failure == Failure::SerialRandom ? 0 : __real_BN_rand(value, bits, top, bottom);
}
ASN1_INTEGER* __real_BN_to_ASN1_INTEGER(const BIGNUM*, ASN1_INTEGER*);
ASN1_INTEGER* __wrap_BN_to_ASN1_INTEGER(const BIGNUM* value, ASN1_INTEGER* result) {
  return failure == Failure::SerialAssignment ? nullptr : __real_BN_to_ASN1_INTEGER(value, result);
}
int __real_X509_sign(X509*, EVP_PKEY*, const EVP_MD*);
int __wrap_X509_sign(X509* cert, EVP_PKEY* key, const EVP_MD* md) {
  return failure == Failure::Sign ? 0 : __real_X509_sign(cert, key, md);
}
int __real_X509_digest(const X509*, const EVP_MD*, unsigned char*, unsigned int*);
int __wrap_X509_digest(const X509* cert, const EVP_MD* md,
                       unsigned char* digest, unsigned int* len) {
  if (failure == Failure::Digest)
    return 0;
  const int result = __real_X509_digest(cert, md, digest, len);
  if (failure == Failure::DigestLength)
    *len = 1;
  return result;
}
}

static void certificate_test() {
  WuCert cert;
  check("certificate created", cert.isValid());
  if (!cert.isValid())
    return;
  check("RSA key has 2048 bits", EVP_PKEY_bits(cert.key) == 2048);
  check("certificate uses SHA256 with RSA",
        X509_get_signature_nid(cert.x509) == NID_sha256WithRSAEncryption);
  check("certificate signature verifies", X509_verify(cert.x509, cert.key) == 1);
  std::unique_ptr<BIGNUM, decltype(&BN_free)> serial(
      ASN1_INTEGER_to_BN(X509_get_serialNumber(cert.x509), nullptr), BN_free);
  check("certificate has a positive 64-bit serial",
        serial && !BN_is_negative(serial.get()) && BN_num_bits(serial.get()) == 64);
  WuCert other;
  check("independent certificates have distinct serials",
        other.isValid() && ASN1_INTEGER_cmp(X509_get_serialNumber(cert.x509),
                                           X509_get_serialNumber(other.x509)) != 0);
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int length = 0;
  char expected[96] = {};
  const bool hashed = X509_digest(cert.x509, EVP_sha256(), digest, &length) == 1;
  if (hashed && length == 32)
    for (unsigned i = 0; i < length; ++i)
      std::snprintf(expected + i * 3, i == length - 1 ? 3 : 4,
                    i == length - 1 ? "%02X" : "%02X:", digest[i]);
  check("SDP fingerprint matches certificate SHA256",
        hashed && length == 32 && std::strcmp(expected, cert.fingerprint) == 0);
}

static bool pump(BIO* source, BIO* destination) {
  unsigned char buffer[65536];
  while (BIO_ctrl_pending(source)) {
    const int length = BIO_read(source, buffer, sizeof(buffer));
    if (length <= 0 || BIO_write(destination, buffer, length) != length)
      return false;
  }
  return true;
}

static bool handshake_step(SSL* ssl) {
  if (SSL_is_init_finished(ssl))
    return true;
  const int result = SSL_do_handshake(ssl);
  if (result == 1)
    return true;
  const int error = SSL_get_error(ssl, result);
  return error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE;
}

static void dtls_test() {
  Wu* raw = nullptr;
  const int status = WuCreate("127.0.0.1", 6901, 1, &raw);
  std::unique_ptr<Wu, decltype(&WuDestroy)> wu(raw, WuDestroy);
  check("production WuCreate succeeds", status == WU_OK && wu);
  if (!wu)
    return;
  std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> clientContext(
      SSL_CTX_new(DTLS_client_method()), SSL_CTX_free);
  WuClient* serverClient = WuNewClient(wu.get());
  SSL* server = serverClient ? serverClient->ssl : nullptr;
  std::unique_ptr<SSL, decltype(&SSL_free)> client(
      clientContext ? SSL_new(clientContext.get()) : nullptr, SSL_free);
  check("DTLS peers allocated", server && client);
  if (!server || !client)
    return;
  for (SSL* ssl : {server, client.get()}) {
    SSL_set_bio(ssl, BIO_new(BIO_s_mem()), BIO_new(BIO_s_mem()));
    BIO_set_mem_eof_return(SSL_get_rbio(ssl), -1);
    BIO_set_mem_eof_return(SSL_get_wbio(ssl), -1);
    SSL_set_options(ssl, SSL_OP_NO_QUERY_MTU);
    SSL_set_mtu(ssl, 1200);
    SSL_set_min_proto_version(ssl, DTLS1_2_VERSION);
    SSL_set_max_proto_version(ssl, DTLS1_2_VERSION);
  }
  SSL_set_accept_state(server);
  SSL_set_connect_state(client.get());
  bool ok = true;
  for (unsigned attempt = 0; attempt < 32 && ok; ++attempt) {
    ok = handshake_step(client.get()) &&
         pump(SSL_get_wbio(client.get()), SSL_get_rbio(server)) &&
         handshake_step(server) &&
         pump(SSL_get_wbio(server), SSL_get_rbio(client.get()));
    if (SSL_is_init_finished(client.get()) && SSL_is_init_finished(server))
      break;
  }
  ok = ok && SSL_is_init_finished(client.get()) && SSL_is_init_finished(server);
  check("DTLS1.2 handshake using production certificate and context", ok);
  if (!ok)
    return;
  // WebRTC authenticates the self-signed certificate by the advertised fingerprint.
  X509* peer = SSL_get_peer_certificate(client.get());
  unsigned char digest[EVP_MAX_MD_SIZE];
  unsigned int length = 0;
  char fingerprint[96] = {};
  bool verified = peer && X509_digest(peer, EVP_sha256(), digest, &length) == 1 && length == 32;
  if (verified)
    for (unsigned i = 0; i < length; ++i)
      std::snprintf(fingerprint + i * 3, i == length - 1 ? 3 : 4,
                    i == length - 1 ? "%02X" : "%02X:", digest[i]);
  X509_free(peer);
  check("DTLS peer matches advertised SDP fingerprint",
        verified && std::strcmp(fingerprint, wu->certFingerprint) == 0);
  char received[32] = {};
  check("encrypted DTLS payload round trip",
        SSL_write(client.get(), "request", 7) == 7 &&
        pump(SSL_get_wbio(client.get()), SSL_get_rbio(server)) &&
        SSL_read(server, received, sizeof(received)) == 7 &&
        !std::memcmp(received, "request", 7) &&
        SSL_write(server, "response", 8) == 8 &&
        pump(SSL_get_wbio(server), SSL_get_rbio(client.get())) &&
        SSL_read(client.get(), received, sizeof(received)) == 8 &&
        !std::memcmp(received, "response", 8));
}

static void failure_tests() {
  const struct { Failure failure; const char* name; } cases[] = {
      {Failure::Context, "key context allocation failure"},
      {Failure::KeyInit, "key initialization failure"},
      {Failure::Keygen, "key generation failure with partial allocation"},
      {Failure::Certificate, "certificate allocation failure"},
      {Failure::SerialAllocation, "serial allocation failure"},
      {Failure::SerialRandom, "serial random generation failure"},
      {Failure::SerialAssignment, "serial assignment failure"},
      {Failure::Sign, "certificate signing failure"},
      {Failure::Digest, "certificate fingerprint failure"},
      {Failure::DigestLength, "unexpected fingerprint length"}};
  for (const auto& test : cases) {
    failure = test.failure;
    {
      WuCert cert;
      check(test.name, !cert.isValid() && !cert.key && !cert.x509 && !cert.fingerprint[0]);
    }
    Wu* wu = reinterpret_cast<Wu*>(1);
    check("failed certificate prevents successful WuCreate",
          WuCreate("127.0.0.1", 6901, 1, &wu) == WU_ERROR && !wu);
    failure = Failure::None;
    ERR_clear_error();
  }
  dtls_test(); // Recovery after all failures also exercises repeated teardown.
}

static void live_client_teardown_test() {
  Wu* wu = nullptr;
  const int status = WuCreate("127.0.0.1", 6901, 2, &wu);
  check("server created for live client teardown", status == WU_OK && wu);
  if (!wu)
    return;
  check("two live DTLS clients created", WuNewClient(wu) && WuNewClient(wu));
  WuDestroy(wu);
}

static void pool_alignment_test() {
  WuPool* pool = WuPoolCreate(3, 3);
  void* first = WuPoolAcquire(pool);
  void* second = WuPoolAcquire(pool);
  void* third = WuPoolAcquire(pool);
  check("pool aligns every payload even for odd block sizes",
        first && second && third &&
        reinterpret_cast<uintptr_t>(first) % alignof(std::max_align_t) == 0 &&
        reinterpret_cast<uintptr_t>(second) % alignof(std::max_align_t) == 0 &&
        reinterpret_cast<uintptr_t>(third) % alignof(std::max_align_t) == 0);
  WuPoolRelease(pool, second);
  check("aligned pool preserves release and reuse", WuPoolAcquire(pool) == second);
  WuPoolDestroy(pool);
}

int main() {
  std::printf("Runtime: %s\n", OpenSSL_version(OPENSSL_VERSION));
  certificate_test();
  dtls_test();
  failure_tests();
  live_client_teardown_test();
  pool_alignment_test();
  std::printf("Failures: %d\n", failures);
  return failures ? 1 : 0;
}
