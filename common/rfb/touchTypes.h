#pragma once

/* Wire values matching rfb::touchEvent* in common/rfb/touchTypes.h. */
#define VNC_TOUCH_BEGIN 0
#define VNC_TOUCH_UPDATE 1
#define VNC_TOUCH_END 2
#define VNC_MAX_TOUCHES 16

typedef struct {
    int focused;
    int tapped;
    unsigned int touchId;
    int caretX, caretY, caretW, caretH;
    int fieldX, fieldY, fieldW, fieldH;
} TapFocusInfo;

#ifdef __cplusplus
#include <cstddef>

namespace rfb {
    constexpr int touchEventBegin = VNC_TOUCH_BEGIN;
    constexpr int touchEventUpdate = VNC_TOUCH_UPDATE;
    constexpr int touchEventEnd = VNC_TOUCH_END;
    constexpr size_t maxTouches = VNC_MAX_TOUCHES;
}
#endif
