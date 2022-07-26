#include "WuHost.h"

int32_t WuHostCreate(const char*, const char*, int32_t, WuHost** host) {
  *host = NULL;
  return WU_OK;
}
int32_t WuHostServe(WuHost*, WuEvent*, int) { return 0; }
void WuHostRemoveClient(WuHost*, WuClient*) {}
int32_t WuHostSendText(WuHost*, WuClient*, const char*, int32_t) { return 0; }
int32_t WuHostSendBinary(WuHost*, WuClient*, const uint8_t*, int32_t) { return 0; }
void WuHostSetErrorCallback(WuHost*, WuErrorFn) {}
