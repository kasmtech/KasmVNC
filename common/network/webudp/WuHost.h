#pragma once
#include <stdint.h>
#include "Wu.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct WuHost WuHost;

int32_t WuHostCreate(const char* hostAddr, uint16_t port, int32_t maxClients,
                     WuHost** host);
void WuHostDestroy(WuHost* host);
/*
 * Timeout:
 *  -1 = Block until an event
 *   0 = Return immediately
 *  >0 = Block for N milliseconds
 * Returns 1 if an event was received, 0 otherwise.
 */
int32_t WuHostServe(WuHost* host, WuEvent* evt, int timeout);
void WuHostRemoveClient(WuHost* wu, WuClient* client);
int32_t WuHostSendText(WuHost* host, WuClient* client, const char* text,
                       int32_t length);
int32_t WuHostSendBinary(WuHost* host, WuClient* client, const uint8_t* data,
                         int32_t length);
void WuHostSetErrorCallback(WuHost* host, WuErrorFn callback);
void WuHostSetDebugCallback(WuHost* host, WuErrorFn callback);
WuClient* WuHostFindClient(const WuHost* host, WuAddress address);

void WuGotHttp(WuHost *host, const char msg[], const uint32_t msglen,
               char resp[]);

#ifdef __cplusplus
}
#endif
