/* Copyright (C) 2026 Kasm.  All Rights Reserved.
 *
 * This is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This software is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/* This file must not include any X server headers - it mixes GLib,
 * D-Bus and Xlib client headers which clash with dix/os macros. All
 * interaction with the server side goes through vncAtSpi.h. */

#define _GNU_SOURCE /* pthread_timedjoin_np */

#include <X11/Xatom.h>
#include <X11/Xlib.h>
#include <atspi/atspi.h>
#include <dbus/dbus.h>

#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "vncAtSpi.h"

#include "os.h"

#ifndef ATSPI_DEBUG
#define ATSPI_DEBUG 0
#endif

#define ATSPI_TRACE(...)                                                                                               \
    do {                                                                                                               \
        if (ATSPI_DEBUG)                                                                                               \
            fprintf(stderr, __VA_ARGS__);                                                                              \
    } while (0)

/* Wrap a whole multi-statement debug-only block (fetch -> format -> free). */
#define ATSPI_DEBUG_BLOCK(...)                                                                                         \
    do {                                                                                                               \
        if (ATSPI_DEBUG) {                                                                                             \
            __VA_ARGS__                                                                                                \
        }                                                                                                              \
    } while (0)

static int notifypipe[2] = {-1, -1};
static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;

#define PENDING_MAX 8
static TapFocusInfo pending_states[PENDING_MAX];
static int pending_head = 0;
static int pending_count = 0;
static TapFocusInfo last_reported = {.focused = -1};

#define FIELDS_PENDING_MAX 4
static TextInputFields pending_fields[FIELDS_PENDING_MAX];
static int fields_head = 0;
static int fields_count = 0;
static char display_name[32];
static pthread_t atspi_tid;
static int thread_started = 0;
static int shutdown_requested = 0;
static pthread_mutex_t shutdown_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t shutdown_cond = PTHREAD_COND_INITIALIZER;

static volatile int atspi_ready = 0;

static unsigned int g_tap_touch_id = 0;
static double g_tap_touch_ms = 0.0;
static int g_tap_touch_pending = 0;
static int g_tap_touch_x = 0, g_tap_touch_y = 0; /* screen px, for spatial correlation */

static TextFieldRect g_focused_rect = {0};
static unsigned int g_focused_pid = 0;
static int g_focused_valid = 0;

static const AtspiRole INPUT_ROLES[] = {ATSPI_ROLE_ENTRY,
    ATSPI_ROLE_PASSWORD_TEXT,
    ATSPI_ROLE_TEXT,
    ATSPI_ROLE_SPIN_BUTTON,
    ATSPI_ROLE_TERMINAL,
    ATSPI_ROLE_DOCUMENT_TEXT,
    ATSPI_ROLE_LIST_BOX};

static Display *hit_dpy = NULL;

typedef struct {
    int x, y, w, h;
} HitRect;

typedef struct {
    int x, y;
    unsigned int touchId;
    double queued_ms;
} TapRequest;

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec * 1000.0 + (double) ts.tv_nsec / 1e6;
}

static void report_state_full(const TapFocusInfo *info, int force) {
    if (!force && memcmp(info, &last_reported, sizeof(*info)) == 0)
        return;
    const double t0 = now_ms();
    last_reported = *info;

    pthread_mutex_lock(&state_mutex);
    if (pending_count == PENDING_MAX) {
        pending_head = (pending_head + 1) % PENDING_MAX;
        pending_count--;
    }
    pending_states[(pending_head + pending_count) % PENDING_MAX] = *info;
    pending_count++;
    pthread_mutex_unlock(&state_mutex);

    if (write(notifypipe[1], "", 1) < 0) {
        /* pipe full or closed; the server thread will still pick the
         * state up on the next wakeup */
    }

    ATSPI_TRACE("vncAtSpi: report_state_full took %.3f ms\n", now_ms() - t0);
}

static void report_state(const TapFocusInfo *info) {
    report_state_full(info, 0);
}

static void report_fields(const TextInputFields *fields) {
    pthread_mutex_lock(&state_mutex);
    if (fields_count == FIELDS_PENDING_MAX) {
        ++fields_head;
        fields_head %= FIELDS_PENDING_MAX;
        fields_count--;
    }
    pending_fields[(fields_head + fields_count) % FIELDS_PENDING_MAX] = *fields;
    ++fields_count;
    pthread_mutex_unlock(&state_mutex);

    if (write(notifypipe[1], "", 1) < 0) {
        /* pipe full or closed; the server thread picks the list up on the
         * next wakeup */
    }
}

enum TextControlType
{
    TEXT_CONTROL_TYPE_NONE,
    TEXT_CONTROL_TYPE_EDITABLE,
    TEXT_CONTROL_TYPE_FOCUSABLE,
};

static const char *TextControl[] = {
    "no editable state, non-text role",
    "EDITABLE state",
    "TEXT_CONTROL_FOCUSABLE",
};

static enum TextControlType is_text_control(AtspiAccessible *acc) {
    gboolean editable = FALSE;
    gboolean focusable = FALSE;
    gboolean has_states = FALSE;

    AtspiStateSet *states = atspi_accessible_get_state_set(acc);
    if (states) {
        has_states = TRUE;
        editable = atspi_state_set_contains(states, ATSPI_STATE_EDITABLE);
        focusable = atspi_state_set_contains(states, ATSPI_STATE_FOCUSABLE);
        g_object_unref(states);
    }

    AtspiRole role = atspi_accessible_get_role(acc, NULL);
    gchar *role_name = atspi_accessible_get_role_name(acc, NULL);

    ATSPI_DEBUG_BLOCK(AtspiText *text_iface = atspi_accessible_get_text_iface(acc);
        gboolean has_text = text_iface != NULL;
        if (text_iface) g_object_unref(text_iface);

        ATSPI_TRACE("vncAtSpi:   checks: states=%s editable=%d focusable=%d role=%d(%s) text_iface=%d\n",
            has_states ? "yes" : "unavailable",
            editable,
            focusable,
            role,
            role_name ? role_name : "?",
            has_text););

    g_free(role_name);

    if (editable)
        return TEXT_CONTROL_TYPE_EDITABLE;

    for (size_t i = 0; i < sizeof(INPUT_ROLES) / sizeof(INPUT_ROLES[0]); i++) {
        if (role == INPUT_ROLES[i])
            return TEXT_CONTROL_TYPE_EDITABLE;
    }

    return TEXT_CONTROL_TYPE_NONE;
}

static void get_rects(AtspiAccessible *acc, TapFocusInfo *info) {
    AtspiText *text = atspi_accessible_get_text_iface(acc);
    if (text) {
        gint offset = atspi_text_get_caret_offset(text, NULL);
        AtspiRect *r = atspi_text_get_character_extents(text, offset, ATSPI_COORD_TYPE_SCREEN, NULL);
        if (r) {
            info->caret.x = r->x;
            info->caret.y = r->y;
            info->caret.w = r->width;
            info->caret.h = r->height;
            g_free(r);
        }
        g_object_unref(text);
    }

    AtspiComponent *comp = atspi_accessible_get_component_iface(acc);
    if (comp) {
        AtspiRect *r = atspi_component_get_extents(comp, ATSPI_COORD_TYPE_SCREEN, NULL);
        if (r) {
            info->field.x = r->x;
            info->field.y = r->y;
            info->field.w = r->width;
            info->field.h = r->height;
            g_free(r);
        }
        g_object_unref(comp);
    }
}

static void collect_text_fields(AtspiAccessible *node, TextInputFields *out, int depth, int *budget) {
    if (!node || depth <= 0 || *budget <= 0 || out->count >= VNC_MAX_TEXT_FIELDS)
        return;

    gint n = atspi_accessible_get_child_count(node, NULL);
    if (n > 128)
        n = 128;

    for (gint i = 0; i < n && *budget > 0 && out->count < VNC_MAX_TEXT_FIELDS; i++) {
        (*budget)--;
        AtspiAccessible *child = atspi_accessible_get_child_at_index(node, i, NULL);
        if (!child)
            continue;

        if (is_text_control(child) != TEXT_CONTROL_TYPE_NONE) {
            AtspiComponent *comp = atspi_accessible_get_component_iface(child);
            if (comp) {
                AtspiRect *r = atspi_component_get_extents(comp, ATSPI_COORD_TYPE_SCREEN, NULL);
                if (r) {
                    /* skip degenerate / off-screen decoys (see window_rect_at_point) */
                    if (r->width > 1 && r->height > 1 && r->x > -100000 && r->y > -100000) {
                        TextFieldRect *slot = &out->rects[out->count++];
                        slot->x = r->x;
                        slot->y = r->y;
                        slot->w = r->width;
                        slot->h = r->height;
                    }
                    g_free(r);
                }
                g_object_unref(comp);
            }
        }

        collect_text_fields(child, out, depth - 1, budget);
        g_object_unref(child);
    }
}

static void scan_window_text_fields(AtspiAccessible *win) {
    if (!win)
        return;

    TextInputFields fields = {0};
    int budget = 4000;
    collect_text_fields(win, &fields, 24, &budget);

    ATSPI_TRACE("vncAtSpi: window scan found %d text field(s)\n", fields.count);
    report_fields(&fields);
}

/* Walk up from a node to its top-level window (the child of the application)
 * and scan that for text fields. This is the reliable way to locate the window
 * the user is in - a tap's point-based window lookup misresolves to the
 * full-screen desktop frame for some toolkits (e.g. Chromium). */
static void scan_containing_window(AtspiAccessible *node) {
    if (!node)
        return;

    AtspiAccessible *win = g_object_ref(node);
    for (int i = 0; i < 64; i++) {
        AtspiAccessible *parent = atspi_accessible_get_parent(win, NULL);
        if (!parent)
            break;
        AtspiRole prole = atspi_accessible_get_role(parent, NULL);
        if (prole == ATSPI_ROLE_APPLICATION || prole == ATSPI_ROLE_DESKTOP_FRAME) {
            g_object_unref(parent); /* win is the top-level window */
            break;
        }
        g_object_unref(win);
        win = parent;
    }

    scan_window_text_fields(win);
    g_object_unref(win);
}

static AtspiAccessible *deep_search_at_point(AtspiAccessible *acc, int x, int y, int depth, int *budget) {
    if (depth <= 0 || *budget <= 0)
        return NULL;

    gint n = atspi_accessible_get_child_count(acc, NULL);
    if (n > 32)
        n = 32;
    for (gint i = 0; i < n && *budget > 0; i++) {
        (*budget)--;
        AtspiAccessible *child = atspi_accessible_get_child_at_index(acc, i, NULL);
        if (!child)
            continue;

        int usable = 0, contains = 0;
        AtspiComponent *comp = atspi_accessible_get_component_iface(child);
        if (comp) {
            AtspiRect *r = atspi_component_get_extents(comp, ATSPI_COORD_TYPE_SCREEN, NULL);
            if (r) {
                usable = r->width > 1 && r->height > 1 && r->x > -100000 && r->y > -100000;
                contains = usable && x >= r->x && x < r->x + r->width && y >= r->y && y < r->y + r->height;
                g_free(r);
            }
            g_object_unref(comp);
        }

        if (contains) {
            AtspiAccessible *deeper = deep_search_at_point(child, x, y, depth - 1, budget);
            if (deeper) {
                g_object_unref(child);
                return deeper;
            }
            return child;
        }
        if (!usable) {
            AtspiAccessible *deeper = deep_search_at_point(child, x, y, depth - 1, budget);
            if (deeper) {
                g_object_unref(child);
                return deeper;
            }
        }
        g_object_unref(child);
    }
    return NULL;
}

static AtspiAccessible *descend_at_point(AtspiAccessible *root, int x, int y) {
    AtspiAccessible *cur = g_object_ref(root);
    int budget = 200;

    for (int depth = 0; depth < 64; depth++) {
        AtspiAccessible *child = NULL;
        AtspiComponent *comp = atspi_accessible_get_component_iface(cur);
        if (comp) {
            child = atspi_component_get_accessible_at_point(comp, x, y, ATSPI_COORD_TYPE_SCREEN, NULL);
            g_object_unref(comp);
        }
        if (child == cur) {
            g_object_unref(child);
            child = NULL;
        }
        if (!child)
            child = deep_search_at_point(cur, x, y, 8, &budget);
        if (!child)
            break;
        g_object_unref(cur);
        cur = child;
    }
    return cur;
}

static int hit_x_error(Display *dpy, XErrorEvent *e) {
    (void) dpy;
    ATSPI_TRACE("vncAtSpi: ignoring X error %d (request %d) during hit test\n", e->error_code, e->request_code);
    return 0;
}

static int window_has_wm_state(Display *dpy, Window w) {
    static Atom wm_state = None;
    if (wm_state == None)
        wm_state = XInternAtom(dpy, "WM_STATE", False);

    Atom type = None;
    int format = 0;
    unsigned long nitems = 0, after = 0;
    unsigned char *data = NULL;

    if (XGetWindowProperty(dpy, w, wm_state, 0, 0, False, AnyPropertyType, &type, &format, &nitems, &after, &data) !=
        Success)
        return 0;
    if (data)
        XFree(data);
    return type != None;
}

/* The toplevel X window on top at (x, y), honouring stacking.
 *
 * XTranslateCoordinates hands back the child of the target that contains
 * the point, and it respects stacking order, so looping lands on whatever
 * is visually on top. Unlike the window manager's client list this also
 * sees override-redirect windows, which is what makes a tap on an open
 * menu resolve to the menu rather than to the window behind it.
 *
 * The first descendant carrying WM_STATE is the managed client window,
 * which is what the toolkit exposes over AT-SPI. Override-redirect popups
 * have no WM_STATE, so fall back to the outermost window under the root. */
static Window x_toplevel_at_point(Display *dpy, int x, int y) {
    const Window root = DefaultRootWindow(dpy);
    Window cur = root, outermost = None;

    for (int depth = 0; depth < 32; depth++) {
        Window child = None;
        int wx = 0, wy = 0;

        if (!XTranslateCoordinates(dpy, root, cur, x, y, &wx, &wy, &child) || child == None)
            break;

        if (outermost == None)
            outermost = child;
        if (window_has_wm_state(dpy, child))
            return child;
        cur = child;
    }
    return outermost;
}

static pid_t x_window_pid(Display *dpy, Window w) {
    static Atom pid_atom = None;
    if (pid_atom == None)
        pid_atom = XInternAtom(dpy, "_NET_WM_PID", False);

    Atom type = None;
    int format = 0;
    unsigned long nitems = 0, after = 0;
    unsigned char *data = NULL;
    pid_t pid = 0;

    if (XGetWindowProperty(dpy, w, pid_atom, 0, 1, False, XA_CARDINAL, &type, &format, &nitems, &after, &data) ==
        Success) {
        if (data && type == XA_CARDINAL && format == 32 && nitems >= 1)
            pid = (pid_t) * (unsigned long *) data;
        if (data)
            XFree(data);
    }
    return pid;
}

static int x_window_rect(Display *dpy, Window w, HitRect *out) {
    Window root_ret = None, child = None;
    int wx = 0, wy = 0, rx = 0, ry = 0;
    unsigned int width = 0, height = 0, border = 0, depth = 0;

    if (!XGetGeometry(dpy, w, &root_ret, &wx, &wy, &width, &height, &border, &depth))
        return 0;
    if (width < 1 || height < 1)
        return 0;
    if (!XTranslateCoordinates(dpy, w, root_ret, 0, 0, &rx, &ry, &child))
        return 0;

    out->x = rx;
    out->y = ry;
    out->w = (int) width;
    out->h = (int) height;
    return 1;
}

static long rect_area(const HitRect *r) {
    return (long) r->w * (long) r->h;
}

/* Intersection over union, 0.0 when the rects miss each other. */
static double rect_iou(const HitRect *a, const HitRect *b) {
    int x0 = a->x > b->x ? a->x : b->x;
    int y0 = a->y > b->y ? a->y : b->y;
    int x1 = (a->x + a->w) < (b->x + b->w) ? (a->x + a->w) : (b->x + b->w);
    int y1 = (a->y + a->h) < (b->y + b->h) ? (a->y + a->h) : (b->y + b->h);

    if (x1 <= x0 || y1 <= y0)
        return 0.0;

    double inter = (double) (x1 - x0) * (double) (y1 - y0);
    double uni = (double) rect_area(a) + (double) rect_area(b) - inter;
    return uni > 0.0 ? inter / uni : 0.0;
}

/* TRUE when the window's extents contain (x, y); *out gets the rect.
 * Degenerate rects are skipped - xfwm4 always exposes a 5x5 decoy window
 * parked at (-1000,-1000) that even carries the ACTIVE state. */
static int window_rect_at_point(AtspiAccessible *win, int x, int y, HitRect *out) {
    AtspiComponent *comp = atspi_accessible_get_component_iface(win);
    if (!comp)
        return 0;

    int inside = 0;
    AtspiRect *r = atspi_component_get_extents(comp, ATSPI_COORD_TYPE_SCREEN, NULL);
    if (r) {
        if (r->width > 1 && r->height > 1 && x >= r->x && x < r->x + r->width && y >= r->y && y < r->y + r->height) {
            inside = 1;
            out->x = r->x;
            out->y = r->y;
            out->w = r->width;
            out->h = r->height;
        }
        g_free(r);
    }
    g_object_unref(comp);
    return inside;
}

static AtspiAccessible *find_window_at_point(int x, int y) {
    AtspiAccessible *desktop = atspi_get_desktop(0);
    if (!desktop)
        return NULL;

    HitRect xrect = {0, 0, 0, 0};
    int have_xrect = 0;
    pid_t xpid = 0;

    if (hit_dpy) {
        Window w = x_toplevel_at_point(hit_dpy, x, y);
        if (w != None) {
            have_xrect = x_window_rect(hit_dpy, w, &xrect);
            xpid = x_window_pid(hit_dpy, w);
            ATSPI_TRACE("vncAtSpi: X topmost at (%d,%d): win=0x%lx rect=%dx%d+%d+%d pid=%ld\n",
                x,
                y,
                (unsigned long) w,
                xrect.w,
                xrect.h,
                xrect.x,
                xrect.y,
                (long) xpid);
        }
    }

    AtspiAccessible *best = NULL;
    double best_score = -1.0;
    long best_area = 0;
    int best_active = 0;

    gint napps = atspi_accessible_get_child_count(desktop, NULL);
    for (gint i = 0; i < napps; i++) {
        AtspiAccessible *app = atspi_accessible_get_child_at_index(desktop, i, NULL);
        if (!app)
            continue;

        int pid_match = 0;
        if (xpid != 0) {
            guint apppid = atspi_accessible_get_process_id(app, NULL);
            pid_match = (apppid != 0 && (pid_t) apppid == xpid);
        }

        gint nwins = atspi_accessible_get_child_count(app, NULL);
        for (gint j = 0; j < nwins; j++) {
            AtspiAccessible *win = atspi_accessible_get_child_at_index(app, j, NULL);
            if (!win)
                continue;

            HitRect wrect;
            if (window_rect_at_point(win, x, y, &wrect)) {
                int active = 0;
                AtspiStateSet *states = atspi_accessible_get_state_set(win);
                if (states) {
                    active = atspi_state_set_contains(states, ATSPI_STATE_ACTIVE);
                    g_object_unref(states);
                }

                double score = have_xrect ? rect_iou(&xrect, &wrect) : 0.0;
                if (pid_match)
                    score += 1.0;

                long area = rect_area(&wrect);
                int better = 0;
                if (best == NULL) {
                    better = 1;
                } else if (score > best_score + 1e-9) {
                    better = 1;
                } else if (score > best_score - 1e-9) {
                    if (area < best_area)
                        better = 1;
                    else if (area == best_area && active && !best_active)
                        better = 1;
                }

                if (better) {
                    if (best)
                        g_object_unref(best);
                    best = g_object_ref(win);
                    best_score = score;
                    best_area = area;
                    best_active = active;
                }
            }
            g_object_unref(win);
        }
        g_object_unref(app);
    }
    g_object_unref(desktop);
    return best;
}

static int google_editor_window(AtspiAccessible *win) {
    gchar *name = atspi_accessible_get_name(win, NULL);
    int match = name && (strstr(name, "Google Docs") || strstr(name, "Google Sheets") || strstr(name, "Google Slides"));
    g_free(name);
    return match;
}

static void hit_test_tap_impl(int x, int y, unsigned int touchId) {
    ATSPI_TRACE("vncAtSpi: hit test (tap) at (%d,%d)\n", x, y);

    TapFocusInfo info = {0};
    info.tapped = 1;
    info.touchId = touchId;

    AtspiAccessible *win = find_window_at_point(x, y);
    if (!win) {
        ATSPI_TRACE("vncAtSpi: tap (%d,%d): no window at that point\n", x, y);
        return;
    }

    ATSPI_DEBUG_BLOCK(gchar *wn = atspi_accessible_get_name(win, NULL);
        gchar *wr = atspi_accessible_get_role_name(win, NULL);
        guint wpid = atspi_accessible_get_process_id(win, NULL);
        ATSPI_TRACE("vncAtSpi: tap window=\"%s\" role=%s pid=%u\n", wn && wn[0] ? wn : "?", wr ? wr : "?", wpid);
        g_free(wn);
        g_free(wr););

    AtspiAccessible *target = descend_at_point(win, x, y);

    ATSPI_DEBUG_BLOCK(gchar *win_name = atspi_accessible_get_name(win, NULL);
        gchar *name = atspi_accessible_get_name(target, NULL);
        gchar *role = atspi_accessible_get_role_name(target, NULL);
        ATSPI_TRACE("vncAtSpi: tap (%d,%d) in window \"%s\" hit widget \"%s\" role=%s\n",
            x,
            y,
            win_name && win_name[0] ? win_name : "?",
            name && name[0] ? name : "?",
            role ? role : "?");
        g_free(role);
        g_free(name);
        g_free(win_name););

    if (target != win && is_text_control(target) != TEXT_CONTROL_TYPE_NONE) {
        info.focused = 1;
        get_rects(target, &info);
    } else if (g_focused_valid && atspi_accessible_get_process_id(win, NULL) == g_focused_pid &&
               x >= g_focused_rect.x && x < g_focused_rect.x + g_focused_rect.w && y >= g_focused_rect.y &&
               y < g_focused_rect.y + g_focused_rect.h) {
        info.focused = 1;
        info.field = g_focused_rect;
        ATSPI_TRACE("vncAtSpi: tap fell back to focused-field rect\n");
    } else if (target && google_editor_window(win) &&
               (atspi_accessible_get_role(target, NULL) == ATSPI_ROLE_CANVAS ||
                   atspi_accessible_get_role(target, NULL) == ATSPI_ROLE_SECTION)) {
        info.focused = 1;
        get_rects(target, &info);
        ATSPI_TRACE("vncAtSpi: Google editor canvas/section - forcing text input\n");
    }

    ATSPI_DEBUG_BLOCK(const char *reason = info.focused    ? "text control"
                                           : target == win ? "tap did not hit a child widget"
                                                           : "not a text control";
        ATSPI_TRACE("vncAtSpi:   tap verdict: %s (%s)",
            info.focused ? "TEXT INPUT -> show keyboard" : "not a text input",
            reason);
        if (info.focused)
            ATSPI_TRACE(" caret=%dx%d+%d+%d field=%dx%d+%d+%d",
            info.caret.w,
            info.caret.h,
            info.caret.x,
            info.caret.y,
            info.field.w,
            info.field.h,
            info.field.x,
            info.field.y);
        ATSPI_TRACE("\n"););

    g_object_unref(target);
    g_object_unref(win);

    report_state_full(&info, 1);
}

static void hit_test_tap(int x, int y, unsigned int touchId) {
    const double t0 = now_ms();
    hit_test_tap_impl(x, y, touchId);
    ATSPI_TRACE("vncAtSpi: hit_test_tap (%d,%d) took %.1f ms\n", x, y, now_ms() - t0);
}

static gboolean process_tap(gpointer data) {
    TapRequest *req = data;

    ATSPI_TRACE("vncAtSpi: tap request (%d,%d) waited %.1f ms in queue\n", req->x, req->y, now_ms() - req->queued_ms);

    if (atspi_ready)
        hit_test_tap(req->x, req->y, req->touchId);

    free(req);
    return G_SOURCE_REMOVE;
}

static void on_focus_event(AtspiEvent *event, void *data) {
    AtspiAccessible *obj = event->source;
    if (!obj) {
        g_boxed_free(ATSPI_TYPE_EVENT, event);
        return;
    }

    TapFocusInfo info = {0};

    ATSPI_DEBUG_BLOCK(gchar *name = atspi_accessible_get_name(obj, NULL);
        gchar *role = atspi_accessible_get_role_name(obj, NULL);
        gchar *app_name = NULL;
        AtspiAccessible *app = atspi_accessible_get_application(obj, NULL);
        if (app) {
            app_name = atspi_accessible_get_name(app, NULL);
            g_object_unref(app);
        }

        ATSPI_TRACE("vncAtSpi: event=%s detail1=%d app=\"%s\" widget=\"%s\" role=%s\n",
            event->type,
            event->detail1,
            app_name && app_name[0] ? app_name : "?",
            name && name[0] ? name : "?",
            role ? role : "?");

        g_free(app_name);
        g_free(role);
        g_free(name););

    /* "object:state-changed:focused" carries gain/loss in detail1;
     * legacy "focus:" events only fire on gain */
    int gained = strncmp(event->type, "focus", 5) == 0 || event->detail1;

    if (gained && is_text_control(obj) != TEXT_CONTROL_TYPE_NONE) {
        get_rects(obj, &info);
        g_focused_rect = info.field;
        g_focused_pid = atspi_accessible_get_process_id(obj, NULL);
        g_focused_valid = (info.field.w > 0 && info.field.h > 0);

        int tap_fresh = g_tap_touch_pending && (now_ms() - g_tap_touch_ms) < 1500.0;
        int tap_inside = tap_fresh && g_tap_touch_x >= info.field.x && g_tap_touch_x < info.field.x + info.field.w &&
                         g_tap_touch_y >= info.field.y && g_tap_touch_y < info.field.y + info.field.h;

        if (!tap_inside) {
            ATSPI_TRACE("vncAtSpi: focus gain not from a tap inside the field (fresh=%d) tap=(%d,%d) field=%dx%d+%d+%d "
                        "- ignoring\n",
                tap_fresh,
                g_tap_touch_x,
                g_tap_touch_y,
                info.field.w,
                info.field.h,
                info.field.x,
                info.field.y);
            g_boxed_free(ATSPI_TYPE_EVENT, event);
            return;
        }

        info.focused = 1;
        scan_containing_window(obj);

        info.tapped = 1;
        info.touchId = g_tap_touch_id;
        g_tap_touch_pending = 0;
    }

    ATSPI_TRACE("vncAtSpi: focus event type=%s gained=%d is_text_input=%d\n", event->type, gained, info.focused);

    ATSPI_DEBUG_BLOCK(const char *reason = info.focused ? "text control" : "focus lost or not a text control";
        ATSPI_TRACE(
            "vncAtSpi:   verdict: %s (%s)", info.focused ? "TEXT INPUT -> show keyboard" : "not a text input", reason);
        if (info.focused)
            ATSPI_TRACE(" caret=%dx%d+%d+%d field=%dx%d+%d+%d",
            info.caret.w,
            info.caret.h,
            info.caret.x,
            info.caret.y,
            info.field.w,
            info.field.h,
            info.field.x,
            info.field.y);
        ATSPI_TRACE("\n"););

    if (info.focused || !gained)
        report_state(&info);

    if (!gained)
        g_focused_valid = 0;

    g_boxed_free(ATSPI_TYPE_EVENT, event);
}

static int a11y_address_alive(const char *address) {
    DBusConnection *conn = dbus_connection_open_private(address, NULL);
    if (!conn)
        return 0;
    dbus_connection_close(conn);
    dbus_connection_unref(conn);
    return 1;
}

static DBusConnection *open_session_bus(void) {
    char busaddr[64];
    snprintf(busaddr, sizeof(busaddr), "unix:path=/run/user/%u/bus", (unsigned) getuid());

    DBusConnection *conn = dbus_connection_open_private(busaddr, NULL);
    if (!conn)
        return NULL;
    if (!dbus_bus_register(conn, NULL)) {
        dbus_connection_close(conn);
        dbus_connection_unref(conn);
        return NULL;
    }
    return conn;
}

/* Ask org.a11y.Bus on the user's session bus for the a11y bus address.
 * This works even after an X server restart, unlike the AT_SPI_BUS root
 * window property, which at-spi-bus-launcher only sets once at its own
 * startup. */
static char *a11y_address_from_session_bus(void) {
    DBusConnection *conn = open_session_bus();
    if (!conn)
        return NULL;

    char *address = NULL;
    DBusMessage *msg = dbus_message_new_method_call("org.a11y.Bus", "/org/a11y/bus", "org.a11y.Bus", "GetAddress");
    if (msg) {
        DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, 2000, NULL);
        if (reply) {
            const char *s = NULL;
            if (dbus_message_get_args(reply, NULL, DBUS_TYPE_STRING, &s, DBUS_TYPE_INVALID) && s && s[0])
                address = strdup(s);
            dbus_message_unref(reply);
        }
        dbus_message_unref(msg);
    }
    dbus_connection_close(conn);
    dbus_connection_unref(conn);
    return address;
}

static void enable_a11y_status_flags(void) {
    DBusConnection *conn = open_session_bus();
    if (!conn) {
        LogMessage(X_WARNING, "[atspi] cannot reach session bus to enable a11y status flags\n");
        return;
    }

    static const char *props[] = {"IsEnabled", "ScreenReaderEnabled"};
    for (size_t i = 0; i < sizeof(props) / sizeof(props[0]); i++) {
        DBusMessage *msg =
            dbus_message_new_method_call("org.a11y.Bus", "/org/a11y/bus", "org.freedesktop.DBus.Properties", "Set");
        if (!msg)
            continue;

        const char *status_iface = "org.a11y.Status";
        dbus_bool_t enabled = TRUE;
        DBusMessageIter iter, variant;
        dbus_message_iter_init_append(msg, &iter);
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &status_iface);
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &props[i]);
        dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT, DBUS_TYPE_BOOLEAN_AS_STRING, &variant);
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &enabled);
        dbus_message_iter_close_container(&iter, &variant);

        DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, 2000, NULL);
        ATSPI_TRACE("vncAtSpi: set org.a11y.Status.%s=true %s\n", props[i], reply ? "ok" : "FAILED");
        if (reply)
            dbus_message_unref(reply);
        dbus_message_unref(msg);
    }

    dbus_connection_close(conn);
    dbus_connection_unref(conn);
}

/* Fallback: the AT_SPI_BUS property on our own root window. */
static char *a11y_address_from_root_window(void) {
    Display *dpy = XOpenDisplay(display_name);
    if (!dpy)
        return NULL;

    Atom prop = XInternAtom(dpy, "AT_SPI_BUS", False);
    Atom actual_type;
    int actual_format;
    unsigned long nitems, bytes_left;
    unsigned char *data = NULL;
    char *address = NULL;

    if (XGetWindowProperty(dpy,
            DefaultRootWindow(dpy),
            prop,
            0,
            256,
            False,
            XA_STRING,
            &actual_type,
            &actual_format,
            &nitems,
            &bytes_left,
            &data) == Success &&
        actual_type == XA_STRING && data && nitems > 0)
        address = strndup((char *) data, nitems);

    if (data)
        XFree(data);
    XCloseDisplay(dpy);
    return address;
}

/* libatspi aborts the process (g_error) if it cannot reach the a11y
 * bus, so we replicate its discovery by hand and verify the address
 * answers before letting libatspi connect via AT_SPI_BUS_ADDRESS. */
static char *wait_for_a11y_bus(void) {
    pthread_mutex_lock(&shutdown_mutex);
    while (!shutdown_requested) {
        pthread_mutex_unlock(&shutdown_mutex);

        char *address = a11y_address_from_session_bus();
        const char *src = "session bus";
        if (!address) {
            address = a11y_address_from_root_window();
            src = "root window";
        }

        if (address) {
            const int alive = a11y_address_alive(address);
            ATSPI_TRACE("vncAtSpi: a11y bus via %s: %s (alive=%d)\n", src, address, alive);
            if (alive)
                return address;
            free(address);
        } else {
            ATSPI_TRACE("vncAtSpi: a11y bus address not found yet (no session bus, no root-window AT_SPI_BUS)\n");
        }

        pthread_mutex_lock(&shutdown_mutex);
        if (!shutdown_requested) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            ts.tv_sec += 2;
            pthread_cond_timedwait(&shutdown_cond, &shutdown_mutex, &ts);
        }
    }
    pthread_mutex_unlock(&shutdown_mutex);
    return NULL;
}

static gboolean quit_event_loop(gpointer data) {
    (void) data;
    atspi_event_quit();
    return G_SOURCE_REMOVE;
}

static void *atspi_thread(void *arg) {
    (void) arg;

    char *address = wait_for_a11y_bus();
    if (!address) /* shutdown requested before the bus appeared */
        return NULL;
    setenv("AT_SPI_BUS_ADDRESS", address, 1);
    LogMessage(X_INFO, "[atspi] connecting to a11y bus at %s\n", address);
    free(address);

    enable_a11y_status_flags();

    int ret = atspi_init();
    LogMessage(X_INFO, "[atspi] init=%d  desktop_count=%d\n", ret, atspi_get_desktop_count());

    if (ret < 0) {
        LogMessage(X_WARNING, "[atspi] init failed\n");

        return NULL;
    }

    AtspiEventListener *listener = atspi_event_listener_new(on_focus_event, NULL, NULL);

    GError *err = NULL;
    if (!atspi_event_listener_register(listener, "object:state-changed:focused", &err)) {
        LogMessage(X_WARNING, "[atspi] failed to register focus listener: %s\n", err ? err->message : "unknown");

        g_clear_error(&err);
        g_object_unref(listener);
        atspi_exit();
        return NULL;
    }
    if (!atspi_event_listener_register(listener, "focus:", &err)) {
        /* non-fatal: only legacy toolkits need it */
        g_clear_error(&err);
    }

    XSetErrorHandler(hit_x_error);
    hit_dpy = XOpenDisplay(display_name);
    if (!hit_dpy)
        LogMessage(X_WARNING,
            "[atspi] no X connection for stacking queries, "
            "falling back to geometry only\n");

    LogMessage(X_INFO, "[atspi] focus tracking active\n");
    atspi_ready = 1;
    /* a shutdown requested between init and here still quits: the idle
     * source added by vncAtSpiShutdown fires on the first iteration */
    atspi_event_main();

    atspi_ready = 0;
    if (hit_dpy) {
        XCloseDisplay(hit_dpy);
        hit_dpy = NULL;
    }
    atspi_event_listener_deregister(listener, "object:state-changed:focused", NULL);
    atspi_event_listener_deregister(listener, "focus:", NULL);
    g_object_unref(listener);
    atspi_exit();
    LogMessage(X_INFO, "[atspi] focus tracking shut down\n");
    return NULL;
}

int vncAtSpiInit(const char *displayNum) {
    if (notifypipe[0] != -1)
        return notifypipe[0];

    snprintf(display_name, sizeof(display_name), ":%s", displayNum ? displayNum : "0");

    if (pipe(notifypipe) < 0)
        return -1;
    fcntl(notifypipe[0], F_SETFL, fcntl(notifypipe[0], F_GETFL, 0) | O_NONBLOCK);
    fcntl(notifypipe[1], F_SETFL, fcntl(notifypipe[1], F_GETFL, 0) | O_NONBLOCK);

    if (pthread_create(&atspi_tid, NULL, atspi_thread, NULL) != 0) {
        close(notifypipe[0]);
        close(notifypipe[1]);
        notifypipe[0] = notifypipe[1] = -1;
        return -1;
    }
    thread_started = 1;

    return notifypipe[0];
}

void vncAtSpiShutdown(void) {
    if (!thread_started)
        return;

    pthread_mutex_lock(&shutdown_mutex);
    if (shutdown_requested) {
        pthread_mutex_unlock(&shutdown_mutex);
        return;
    }
    shutdown_requested = 1;
    pthread_cond_signal(&shutdown_cond);
    pthread_mutex_unlock(&shutdown_mutex);

    /* wakes the (default-context) event loop; if the loop is not
     * running yet, the idle source fires on its first iteration */
    g_idle_add(quit_event_loop, NULL);

    /* bounded join: don't let a stuck a11y bus hang server shutdown */
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += 1;
    if (pthread_timedjoin_np(atspi_tid, NULL, &deadline) != 0) {
        LogMessage(X_WARNING, "[atspi] shutdown timed out, abandoning thread\n");
        pthread_detach(atspi_tid);
    }
    thread_started = 0;

    if (notifypipe[0] != -1) {
        close(notifypipe[0]);
        close(notifypipe[1]);
        notifypipe[0] = notifypipe[1] = -1;
    }
}

int vncAtSpiGetFd(void) {
    return notifypipe[0];
}

void vncAtSpiRequestHitTest(int x, int y, unsigned int touchId) {
    if (!thread_started || !atspi_ready)
        return;

    TapRequest *req = malloc(sizeof(*req));
    if (!req)
        return;

    req->x = x;
    req->y = y;
    req->touchId = touchId;
    req->queued_ms = now_ms();

    g_tap_touch_id = touchId;
    g_tap_touch_ms = req->queued_ms;
    g_tap_touch_x = x;
    g_tap_touch_y = y;
    g_tap_touch_pending = 1;

    /* g_idle_add is thread-safe; it wakes the AT-SPI thread's loop and
     * hands off the tap so the hit test runs on that thread. */
    g_idle_add(process_tap, req);
}

int vncAtSpiGetFocusState(TapFocusInfo *out) {
    pthread_mutex_lock(&state_mutex);
    int had = pending_count > 0;
    if (had) {
        *out = pending_states[pending_head];
        pending_head = (pending_head + 1) % PENDING_MAX;
        pending_count--;
    }
    pthread_mutex_unlock(&state_mutex);
    return had;
}

int vncAtSpiGetTextInputFields(TextInputFields *out) {
    pthread_mutex_lock(&state_mutex);

    int had = fields_count > 0;
    if (had) {
        *out = pending_fields[fields_head];
        fields_head = (fields_head + 1) % FIELDS_PENDING_MAX;
        fields_count--;
    }

    pthread_mutex_unlock(&state_mutex);

    return had;
}
