#include "WuStun.h"
#include <arpa/inet.h>
#include <string.h>
#include "CRC32.h"
#include <openssl/crypto.h>
#include "WuBufferOp.h"
#include "WuCrypto.h"

bool ParseStun(const uint8_t* src, int32_t len, StunPacket* packet) {
  if (len < kStunHeaderLength || src[0] != 0 || src[1] != 1) {
    return false;
  }

  src += ReadScalarSwapped(src, &packet->type);

  if (packet->type != Stun_BindingRequest) {
    return false;
  }

  src += ReadScalarSwapped(src, &packet->length);

  if (packet->length < 4 || packet->length > len - kStunHeaderLength) {
    // Need at least 1 attribute
    return false;
  }

  src += ReadScalarSwapped(src, &packet->cookie);

  for (int32_t i = 0; i < kStunTransactionIdLength; i++) {
    packet->transactionId[i] = src[i];
  }

  src += kStunTransactionIdLength;

  bool has_user = false;

  int32_t payloadOffset = 0;
  while (payloadOffset + kStunAttributeHeaderLength <= packet->length) {
    const int32_t attr_start = payloadOffset;
    uint16_t payloadType = 0;
    uint16_t payloadLength = 0;

    payloadOffset += ReadScalarSwapped(src + payloadOffset, &payloadType);
    payloadOffset += ReadScalarSwapped(src + payloadOffset, &payloadLength);

    const int32_t paddedLength =
        payloadLength + PadSize(payloadLength, kStunAlignment);

    if (paddedLength > packet->length - payloadOffset)
      return false;

    if (payloadType == StunAttrib_User) {
      // fragment = min 4 chars
      // username = fragment:fragment (at least 9 characters)
      if (payloadLength < kMinStunUsernameLength)
        return false;

      const char* uname = reinterpret_cast<const char*>(src + payloadOffset);
      const int32_t colonIndex = FindTokenIndex(uname, payloadLength, ':');
      if (colonIndex < kMinIceUfragLength)
        return false;

      const int32_t serverUserLength = colonIndex;
      const int32_t remoteUserLength = payloadLength - colonIndex - 1;
      if (serverUserLength > kMaxStunIdentifierLength || remoteUserLength > kMaxStunIdentifierLength)
        return false;

      packet->serverUser.length = serverUserLength;
      packet->remoteUser.length = remoteUserLength;
      memcpy(packet->serverUser.identifier, uname, serverUserLength);
      memcpy(packet->remoteUser.identifier, uname + colonIndex + 1, remoteUserLength);

      has_user = true;
    } else if (payloadType == StunAttrib_MessageIntegrity) {
      if (payloadLength != kSHA1Length)
        return false;

      memcpy(packet->integrity, src + payloadOffset, kSHA1Length);
      packet->integrityOffset = attr_start + kStunHeaderLength;

      break;
    }

    payloadOffset += paddedLength;
  }

  return has_user && packet->integrityOffset;
}

bool VerifyStunIntegrity(const uint8_t *msg, int32_t len, const StunPacket *packet, const uint8_t *password, int32_t passwordLen) {
  if (!packet->integrityOffset)
    return false;

  const int32_t mac_end = packet->integrityOffset + kStunAttributeHeaderLength + static_cast<int32_t>(kSHA1Length);
  if (packet->integrityOffset < kStunHeaderLength || mac_end > len)
    return false;

  // RFC 5389 15.4: the HMAC covers the message up to (excluding) the
  // MESSAGE-INTEGRITY attribute, with the header length field adjusted to
  // point at the end of that attribute.
  constexpr int32_t buf_size = 512;

  if (packet->integrityOffset > buf_size)
    return false;

  uint8_t buf[buf_size];

  memcpy(buf, msg, packet->integrityOffset);
  const uint16_t adjusted = htons(mac_end - kStunHeaderLength);
  memcpy(buf + 2, &adjusted, sizeof(adjusted));

  auto [bytes] = WuSHA1(buf, packet->integrityOffset, password, passwordLen);

  return CRYPTO_memcmp(bytes, packet->integrity, kSHA1Length) == 0;
}

size_t SerializeStunPacket(const StunPacket* packet, const uint8_t* password,
                            int32_t passwordLen, uint8_t* dest, int32_t len) {
  memset(dest, 0, len);
  size_t offset = WriteScalar(dest, htons(Stun_SuccessResponse));
  // X-MAPPED-ADDRESS (ip4) + MESSAGE-INTEGRITY SHA1
  int32_t contentLength = 12 + 24;
  int32_t contentLengthIntegrity = contentLength + 8;
  const size_t contentLengthOffset = offset;
  offset += WriteScalar(dest + offset, htons(contentLength));
  offset += WriteScalar(dest + offset, htonl(kStunCookie));

  for (int32_t i = 0; i < 12; i++) {
    dest[i + offset] = packet->transactionId[i];
  }

  offset += 12;

  // xor mapped address attribute ipv4
  offset += WriteScalar(dest + offset, htons(StunAttrib_XorMappedAddress));
  offset += WriteScalar(dest + offset, htons(8));
  offset += WriteScalar(dest + offset, 0);  // reserved
  offset += WriteScalar(dest + offset, packet->xorMappedAddress.family);
  offset += WriteScalar(dest + offset, packet->xorMappedAddress.port);
  offset += WriteScalar(dest + offset, packet->xorMappedAddress.address.ipv4);

  WuSHA1Digest digest = WuSHA1(dest, offset, password, passwordLen);

  offset += WriteScalar(dest + offset, htons(StunAttrib_MessageIntegrity));
  offset += WriteScalar(dest + offset, htons(20));

  for (int32_t i = 0; i < 20; i++) {
    dest[i + offset] = digest.bytes[i];
  }

  offset += 20;

  WriteScalar(dest + contentLengthOffset, htons(contentLengthIntegrity));
  const uint32_t crc = StunCRC32(dest, offset) ^ 0x5354554e;

  offset += WriteScalar(dest + offset, htons(StunAttrib_Fingerprint));
  offset += WriteScalar(dest + offset, htons(4));
  offset += WriteScalar(dest + offset, htonl(crc));

  return offset;
}
