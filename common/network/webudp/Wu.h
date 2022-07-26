#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WU_OK 0
#define WU_ERROR 1
#define WU_OUT_OF_MEMORY 2

typedef struct WuClient WuClient;
typedef struct Wu Wu;
typedef void (*WuErrorFn)(const char* err, void* userData);
typedef void (*WuWriteFn)(const uint8_t* data, size_t length,
                          const WuClient* client, void* userData);

typedef enum {
  WuEvent_BinaryData,
  WuEvent_ClientJoin,
  WuEvent_ClientLeave,
  WuEvent_TextData
} WuEventType;

typedef enum {
  WuSDPStatus_Success,
  WuSDPStatus_InvalidSDP,
  WuSDPStatus_MaxClients,
  WuSDPStatus_Error
} WuSDPStatus;

typedef struct {
  WuEventType type;
  WuClient* client;
  const uint8_t* data;
  int32_t length;
} WuEvent;

typedef struct {
  WuSDPStatus status;
  WuClient* client;
  const char* sdp;
  int32_t sdpLength;
} SDPResult;

typedef struct {
  uint32_t host;
  uint16_t port;
} WuAddress;

int32_t WuCreate(const char* host, uint16_t port, int maxClients, Wu** wu);
void WuDestroy(Wu* wu);
int32_t WuUpdate(Wu* wu, WuEvent* evt);
int32_t WuSendText(Wu* wu, WuClient* client, const char* text, int32_t length);
int32_t WuSendBinary(Wu* wu, WuClient* client, const uint8_t* data,
                     int32_t length);
void WuReportError(Wu* wu, const char* error);
void WuReportDebug(Wu* wu, const char* error);
void WuRemoveClient(Wu* wu, WuClient* client);
void WuClientSetUserData(WuClient* client, void* user);
void* WuClientGetUserData(const WuClient* client);
SDPResult WuExchangeSDP(Wu* wu, const char* sdp, int32_t length);
void WuHandleUDP(Wu* wu, const WuAddress* remote, const uint8_t* data,
                 int32_t length);
void WuSetUDPWriteFunction(Wu* wu, WuWriteFn write);
void WuSetUserData(Wu* wu, void* userData);
void WuSetErrorCallback(Wu* wu, WuErrorFn callback);
void WuSetDebugCallback(Wu* wu, WuErrorFn callback);
WuAddress WuClientGetAddress(const WuClient* client);
WuClient* WuFindClient(const Wu* wu, WuAddress address);

#ifdef __cplusplus
}
#endif
