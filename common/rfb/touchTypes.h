#pragma once

#define VNC_TOUCH_BEGIN 0
#define VNC_TOUCH_UPDATE 1
#define VNC_TOUCH_END 2
#define VNC_MAX_TOUCHES 16
#define VNC_MAX_TEXT_FIELDS 64

typedef struct {
    int x, y, w, h;
} TextFieldRect;

typedef struct {
    int focused;
    int tapped;
    unsigned int touchId;
    TextFieldRect caret; /* insertion-point extents, screen px */
    TextFieldRect field; /* whole input widget extents, screen px */
} TapFocusInfo;

typedef struct {
    int count;
    TextFieldRect rects[VNC_MAX_TEXT_FIELDS];
} TextInputFields;

#ifdef __cplusplus
#include <cstddef>

namespace rfb {
    constexpr int touchEventBegin = VNC_TOUCH_BEGIN;
    constexpr int touchEventUpdate = VNC_TOUCH_UPDATE;
    constexpr int touchEventEnd = VNC_TOUCH_END;
    constexpr size_t maxTouches = VNC_MAX_TOUCHES;
    constexpr size_t maxTextFields = VNC_MAX_TEXT_FIELDS;
} // namespace rfb
#endif
