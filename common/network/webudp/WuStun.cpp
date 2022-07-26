#include "WuStun.h"
#include <arpa/inet.h>
#include <string.h>
#include "CRC32.h"
#include "WuCrypto.h"

const int32_t kStunHeaderLength = 20;
const int32_t kStunAlignment = 4;

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

  int32_t maxOffset = int32_t(packet->length) - 1;
  int32_t payloadOffset = 0;
  while (payloadOffset < maxOffset) {
    int32_t remain = len - kStunHeaderLength - payloadOffset;
    if (remain >= 4) {
      uint16_t payloadType = 0;
      uint16_t payloadLength = 0;

      payloadOffset += ReadScalarSwapped(src + payloadOffset, &payloadType);
      payloadOffset += ReadScalarSwapped(src + payloadOffset, &payloadLength);
      remain -= 4;

      int32_t paddedLength =
          payloadLength + PadSize(payloadLength, kStunAlignment);

      if (payloadType == StunAttrib_User) {
        // fragment = min 4 chars
        // username = fragment:fragment (at least 9 characters)
        if (paddedLength <= remain && payloadLength >= 9) {
          const char* uname = (const char*)src + payloadOffset;
          int32_t colonIndex = FindTokenIndex(uname, payloadLength, ':');
          if (colonIndex >= 4) {
            int32_t serverUserLength = colonIndex;
            int32_t remoteUserLength = payloadLength - colonIndex - 1;
            if (serverUserLength > kMaxStunIdentifierLength ||
                remoteUserLength > kMaxStunIdentifierLength) {
              return false;
            } else {
              packet->serverUser.length = serverUserLength;
              packet->remoteUser.length = remoteUserLength;
              memcpy(packet->serverUser.identifier, uname, serverUserLength);
              memcpy(packet->remoteUser.identifier, uname + colonIndex + 1,
                     remoteUserLength);
              return true;
            }

          } else {
            return false;
          }
        } else {
          // Actual length > reported length
          return false;
        }
      }

      payloadOffset += paddedLength;
    } else {
      return false;
    }
  }

  return true;
}

int32_t SerializeStunPacket(const StunPacket* packet, const uint8_t* password,
                            int32_t passwordLen, uint8_t* dest, int32_t len) {
  memset(dest, 0, len);
  int32_t offset = WriteScalar(dest, htons(Stun_SuccessResponse));
  // X-MAPPED-ADDRESS (ip4) + MESSAGE-INTEGRITY SHA1
  int32_t contentLength = 12 + 24;
  int32_t contentLengthIntegrity = contentLength + 8;
  const int32_t contentLengthOffset = offset;
  offset += WriteScalar(dest + offset, htons(contentLength));
  offset += WriteScalar(dest + offset, htonl(kStunCookie));

  for (int32_t i = 0; i < 12; i++) {
    dest[i + offset] = packet->transactionId[i];
  }

  offset += 12;

  // xor mapped address attribute ipv4
  offset += WriteScalar(dest + offset, htons(StunAttrib_XorMappedAddress));
  offset += WriteScalar(dest + offset, htons(8));
  offset += WriteScalar(dest + offset, uint8_t(0));  // reserved
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
  uint32_t crc = StunCRC32(dest, offset) ^ 0x5354554e;

  offset += WriteScalar(dest + offset, htons(StunAttrib_Fingerprint));
  offset += WriteScalar(dest + offset, htons(4));
  offset += WriteScalar(dest + offset, htonl(crc));

  return offset;
}
