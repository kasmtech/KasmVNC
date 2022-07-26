#include "WuCrypto.h"
#include <assert.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include "WuRng.h"

WuSHA1Digest WuSHA1(const uint8_t* src, size_t len, const void* key,
                    size_t keyLen) {
  WuSHA1Digest digest;
  HMAC(EVP_sha1(), key, keyLen, src, len, digest.bytes, NULL);

  return digest;
}

WuCert::WuCert() : key(EVP_PKEY_new()), x509(X509_new()) {
  RSA* rsa = RSA_new();
  BIGNUM* n = BN_new();
  BN_set_word(n, RSA_F4);

  if (!RAND_status()) {
    uint64_t seed = WuRandomU64();
    RAND_seed(&seed, sizeof(seed));
  }

  RSA_generate_key_ex(rsa, 2048, n, NULL);
  EVP_PKEY_assign_RSA(key, rsa);

  BIGNUM* serial = BN_new();
  X509_NAME* name = X509_NAME_new();
  X509_set_pubkey(x509, key);
  BN_pseudo_rand(serial, 64, 0, 0);

  X509_set_version(x509, 0L);
  X509_NAME_add_entry_by_NID(name, NID_commonName, MBSTRING_UTF8,
                             (unsigned char*)"wusocket", -1, -1, 0);
  X509_set_subject_name(x509, name);
  X509_set_issuer_name(x509, name);
  X509_gmtime_adj(X509_get_notBefore(x509), 0);
  X509_gmtime_adj(X509_get_notAfter(x509), 365 * 24 * 3600);
  X509_sign(x509, key, EVP_sha1());

  unsigned int len = 32;
  uint8_t buf[32] = {0};
  X509_digest(x509, EVP_sha256(), buf, &len);

  assert(len == 32);
  for (unsigned int i = 0; i < len; i++) {
    if (i < 31) {
      snprintf(fingerprint + i * 3, 4, "%02X:", buf[i]);
    } else {
      snprintf(fingerprint + i * 3, 3, "%02X", buf[i]);
    }
  }

  fingerprint[95] = '\0';

  BN_free(n);
  BN_free(serial);
  X509_NAME_free(name);
}

WuCert::~WuCert() {
  EVP_PKEY_free(key);
  X509_free(x509);
}
