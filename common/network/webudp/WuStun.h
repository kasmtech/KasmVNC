#pragma once

#include "WuBufferOp.h"
#include "WuString.h"

const int32_t kMaxStunIdentifierLength = 128;
const int32_t kStunTransactionIdLength = 12;
const uint32_t kStunCookie = 0x2112a442;
const uint16_t kStunXorMagic = 0x2112;

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
};

bool ParseStun(const uint8_t* src, int32_t len, StunPacket* packet);

int32_t SerializeStunPacket(const StunPacket* packet, const uint8_t* password,
                            int32_t passwordLen, uint8_t* dest, int32_t len);
