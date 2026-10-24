#pragma once

#include "WuCrypto.h"
#include "WuString.h"

constexpr int32_t kMaxStunIdentifierLength = 128;
constexpr int32_t kStunTransactionIdLength = 12;
constexpr uint32_t kStunCookie = 0x2112a442;
constexpr uint16_t kStunXorMagic = 0x2112;
constexpr int32_t kStunHeaderLength = 20;
constexpr int32_t kStunAlignment = 4;
constexpr int32_t kStunAttributeHeaderLength = 4;
constexpr int32_t kMinIceUfragLength = 4;
constexpr int32_t kMinStunUsernameLength = 2 * kMinIceUfragLength + 1;

struct StunUserIdentifier {
  uint8_t identifier[kMaxStunIdentifierLength];
  int32_t length;
};

enum StunAddressFamily { Stun_IPV4 = 0x01, Stun_IPV6 = 0x02 };

enum StunType { Stun_BindingRequest = 0x0001, Stun_SuccessResponse = 0x0101 };

enum StunAttributeType {
  StunAttrib_User = 0x06,
  StunAttrib_MessageIntegrity = 0x08,
  StunAttrib_XorMappedAddress = 0x20,
  StunAttrib_Fingerprint = 0x8028
};

struct StunAddress {
  uint8_t family;
  uint16_t port;

  union {
    uint32_t ipv4;
    uint8_t ipv6[16];
  } address;
};

inline bool StunUserIdentifierEqual(const StunUserIdentifier* a,
                                    const StunUserIdentifier* b) {
  return MemEqual(a->identifier, a->length, b->identifier, b->length);
}

struct StunPacket {
  uint16_t type;
  uint16_t length;
  uint32_t cookie;
  uint8_t transactionId[kStunTransactionIdLength];

  StunUserIdentifier remoteUser;
  StunUserIdentifier serverUser;

  StunAddress xorMappedAddress;

  int32_t integrityOffset{0};
  uint8_t integrity[kSHA1Length];
};

bool ParseStun(const uint8_t* src, int32_t len, StunPacket* packet);

bool VerifyStunIntegrity(const uint8_t *msg, int32_t len, const StunPacket *packet, const uint8_t *password,
                         int32_t passwordLen);

size_t SerializeStunPacket(const StunPacket* packet, const uint8_t* password,
                            int32_t passwordLen, uint8_t* dest, int32_t len);
