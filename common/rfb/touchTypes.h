#pragma once

/* Wire values matching rfb::touchEvent* in common/rfb/touchTypes.h. */
#define VNC_TOUCH_BEGIN 0
#define VNC_TOUCH_UPDATE 1
#define VNC_TOUCH_END 2

#ifdef __cplusplus
namespace rfb {
    constexpr int touchEventBegin = VNC_TOUCH_BEGIN;
    constexpr int touchEventUpdate = VNC_TOUCH_UPDATE;
    constexpr int touchEventEnd = VNC_TOUCH_END;
}
#endif
