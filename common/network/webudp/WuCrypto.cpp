#include "WuCrypto.h"
#include <memory>
#include <openssl/bn.h>
#include <openssl/hmac.h>
#include <openssl/rsa.h>

WuSHA1Digest WuSHA1(const uint8_t* src, size_t len, const void* key,
                    size_t keyLen) {
  WuSHA1Digest digest;
  HMAC(EVP_sha1(), key, keyLen, src, len, digest.bytes, NULL);

  return digest;
}

WuCert::WuCert() : key(nullptr), x509(nullptr), fingerprint{} {
  std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> context(
      EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
  if (!context || EVP_PKEY_keygen_init(context.get()) <= 0 ||
      EVP_PKEY_CTX_set_rsa_keygen_bits(context.get(), 2048) <= 0)
    return;

  EVP_PKEY* generatedKey = nullptr;
  const int keyResult = EVP_PKEY_keygen(context.get(), &generatedKey);
  std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> privateKey(
      generatedKey, EVP_PKEY_free);
  if (keyResult <= 0 || !privateKey)
    return;

  std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new(), X509_free);
  if (!certificate)
    return;

  std::unique_ptr<BIGNUM, decltype(&BN_free)> serial(BN_new(), BN_free);
  if (!serial || BN_rand(serial.get(), 64, 0, 0) != 1 ||
      !BN_to_ASN1_INTEGER(serial.get(), X509_get_serialNumber(certificate.get())))
    return;

  X509_NAME* name = X509_get_subject_name(certificate.get());
  if (!name || X509_set_version(certificate.get(), 0L) != 1 ||
      X509_set_pubkey(certificate.get(), privateKey.get()) != 1 ||
      X509_NAME_add_entry_by_NID(name, NID_commonName, MBSTRING_UTF8,
                               (const unsigned char*)"wusocket", -1, -1, 0) != 1 ||
      X509_set_issuer_name(certificate.get(), name) != 1 ||
      !X509_gmtime_adj(X509_getm_notBefore(certificate.get()), 0) ||
      !X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 365 * 24 * 3600) ||
      X509_sign(certificate.get(), privateKey.get(), EVP_sha256()) <= 0)
    return;

  unsigned int len = 0;
  uint8_t buf[32] = {0};
  if (X509_digest(certificate.get(), EVP_sha256(), buf, &len) != 1 ||
      len != sizeof(buf))
    return;

  for (unsigned int i = 0; i < len; i++) {
    if (i < 31) {
      snprintf(fingerprint + i * 3, 4, "%02X:", buf[i]);
    } else {
      snprintf(fingerprint + i * 3, 3, "%02X", buf[i]);
    }
  }

  fingerprint[95] = '\0';

  key = privateKey.release();
  x509 = certificate.release();
}

WuCert::~WuCert() {
  EVP_PKEY_free(key);
  X509_free(x509);
}
