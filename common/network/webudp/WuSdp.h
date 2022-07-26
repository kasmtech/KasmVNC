#pragma once
#include <stddef.h>
#include <stdint.h>

struct WuArena;

struct IceField {
  const char* value;
  int32_t length;
};

struct ICESdpFields {
  IceField ufrag;
  IceField password;
  IceField mid;
};

bool ParseSdp(const char* sdp, size_t len, ICESdpFields* fields);

const char* GenerateSDP(WuArena* arena, const char* certFingerprint,
                        const char* serverIp, uint16_t serverPort,
                        const char* ufrag, int32_t ufragLen, const char* pass,
                        int32_t passLen, const ICESdpFields* remote,
                        int* outLength);
