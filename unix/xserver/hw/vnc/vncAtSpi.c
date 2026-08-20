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

static int notifypipe[2] = {-1, -1};
static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;
static VncAtSpiFocusInfo pending_state;
static int pending = 0;
static VncAtSpiFocusInfo last_reported = {.focused = -1};
static char display_name[32];
static pthread_t atspi_tid;
static int thread_started = 0;
static int shutdown_requested = 0;
static pthread_mutex_t shutdown_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t shutdown_cond = PTHREAD_COND_INITIALIZER;

static pthread_mutex_t tap_mutex = PTHREAD_MUTEX_INITIALIZER;
static int tap_x, tap_y;
static int tap_queued = 0;      /* a GLib source is already scheduled */
static volatile int atspi_ready = 0;

static void report_state_full(const VncAtSpiFocusInfo *info, int force) {
    if (!force && memcmp(info, &last_reported, sizeof(*info)) == 0)
        return;
    last_reported = *info;

    pthread_mutex_lock(&state_mutex);
    pending_state = *info;
    pending = 1;
    pthread_mutex_unlock(&state_mutex);

    if (write(notifypipe[1], "", 1) < 0) {
        /* pipe full or closed; the server thread will still pick the
         * state up on the next wakeup */
    }
}

static void report_state(const VncAtSpiFocusInfo *info) {
    report_state_full(info, 0);
}

/* Decides whether the tapped/focused widget is a text input. On return
 * *reason names the check that decided it, for the debug trace. */
static int is_text_control(AtspiAccessible *acc, const char **reason) {
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

    AtspiText *text_iface = atspi_accessible_get_text_iface(acc);
    gboolean has_text = (text_iface != NULL);
    if (text_iface)
        g_object_unref(text_iface);

    fprintf(stderr,
        "vncAtSpi:   checks: states=%s editable=%d focusable=%d role=%d(%s) text_iface=%d\n",
        has_states ? "yes" : "unavailable",
        editable,
        focusable,
        role,
        role_name ? role_name : "?",
        has_text);
    g_free(role_name);

    if (editable) {
        *reason = "EDITABLE state";
        return 1;
    }

    /* terminals and some password fields do not report EDITABLE */
    if (role == ATSPI_ROLE_TERMINAL || role == ATSPI_ROLE_PASSWORD_TEXT ||
        role == ATSPI_ROLE_ENTRY || role == ATSPI_ROLE_TEXT ||
        role == ATSPI_ROLE_SPIN_BUTTON || role == ATSPI_ROLE_DOCUMENT_TEXT) {
        *reason = "role whitelist";
        return 1;
    }

    *reason = "no editable state, non-text role";
    return 0;
}

static void get_rects(AtspiAccessible *acc, VncAtSpiFocusInfo *info) {
    AtspiText *text = atspi_accessible_get_text_iface(acc);
    if (text) {
        gint offset = atspi_text_get_caret_offset(text, NULL);
        AtspiRect *r = atspi_text_get_character_extents(text, offset, ATSPI_COORD_TYPE_SCREEN, NULL);
        if (r) {
            info->caretX = r->x;
            info->caretY = r->y;
            info->caretW = r->width;
            info->caretH = r->height;
            g_free(r);
        }
        g_object_unref(text);
    }

    AtspiComponent *comp = atspi_accessible_get_component_iface(acc);
    if (comp) {
        AtspiRect *r = atspi_component_get_extents(comp, ATSPI_COORD_TYPE_SCREEN, NULL);
        if (r) {
            info->fieldX = r->x;
            info->fieldY = r->y;
            info->fieldW = r->width;
            info->fieldH = r->height;
            g_free(r);
        }
        g_object_unref(comp);
    }
}

static AtspiAccessible *deep_search_at_point(AtspiAccessible *acc, int x, int y,
                                             int depth, int *budget) {
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
                usable = r->width > 1 && r->height > 1 &&
                         r->x > -100000 && r->y > -100000;
                contains = usable &&
                           x >= r->x && x < r->x + r->width &&
                           y >= r->y && y < r->y + r->height;
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

/* Deepest accessible under (x, y), starting from root.
 * get_accessible_at_point only descends one level per call, so loop;
 * the depth cap guards against cyclic or misbehaving trees. Returns a
 * new reference. */
static AtspiAccessible *descend_at_point(AtspiAccessible *root, int x, int y) {
    AtspiAccessible *cur = g_object_ref(root);
    int budget = 200;

    for (int depth = 0; depth < 64; depth++) {
        AtspiAccessible *child = NULL;
        AtspiComponent *comp = atspi_accessible_get_component_iface(cur);
        if (comp) {
            child = atspi_component_get_accessible_at_point(
                comp, x, y, ATSPI_COORD_TYPE_SCREEN, NULL);
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

static int window_contains_point(AtspiAccessible *win, int x, int y) {
    AtspiComponent *comp = atspi_accessible_get_component_iface(win);
    if (!comp)
        return 0;
    int inside = 0;
    AtspiRect *r = atspi_component_get_extents(comp, ATSPI_COORD_TYPE_SCREEN, NULL);
    if (r) {
        inside = x >= r->x && x < r->x + r->width &&
                 y >= r->y && y < r->y + r->height;
        g_free(r);
    }
    g_object_unref(comp);
    return inside;
}

/* The active (focused) toplevel window containing (x, y), or NULL.
 * Taps that change the active window are already covered by focus
 * events, so the hit-test only needs to see inside the window that
 * stays active. Containment matters: several clients can claim ACTIVE
 * at once - xfwm4 for one always exposes a 5x5 decoy window at
 * (-1000,-1000) with the ACTIVE state set. */
static AtspiAccessible *find_active_window(int x, int y) {
    AtspiAccessible *desktop = atspi_get_desktop(0);
    if (!desktop)
        return NULL;

    AtspiAccessible *found = NULL;
    gint napps = atspi_accessible_get_child_count(desktop, NULL);
    for (gint i = 0; i < napps && !found; i++) {
        AtspiAccessible *app = atspi_accessible_get_child_at_index(desktop, i, NULL);
        if (!app)
            continue;
        gint nwins = atspi_accessible_get_child_count(app, NULL);
        for (gint j = 0; j < nwins && !found; j++) {
            AtspiAccessible *win = atspi_accessible_get_child_at_index(app, j, NULL);
            if (!win)
                continue;
            AtspiStateSet *states = atspi_accessible_get_state_set(win);
            if (states) {
                if (atspi_state_set_contains(states, ATSPI_STATE_ACTIVE) &&
                    window_contains_point(win, x, y))
                    found = g_object_ref(win);
                g_object_unref(states);
            }
            g_object_unref(win);
        }
        g_object_unref(app);
    }
    g_object_unref(desktop);
    return found;
}

static void hit_test_tap(int x, int y) {
    VncAtSpiFocusInfo info = {0};
    info.tapped = 1;

    AtspiAccessible *win = find_active_window(x, y);
    if (!win) {
        /* tap on the desktop or into a window that is only now being
         * activated; the resulting focus change (if any) arrives as a
         * regular focus event */
        fprintf(stderr, "vncAtSpi: tap (%d,%d): no active window at that point\n", x, y);
        report_state_full(&info, 1);
        return;
    }

    AtspiAccessible *target = descend_at_point(win, x, y);

    gchar *win_name = atspi_accessible_get_name(win, NULL);
    gchar *name = atspi_accessible_get_name(target, NULL);
    gchar *role = atspi_accessible_get_role_name(target, NULL);
    fprintf(stderr,
        "vncAtSpi: tap (%d,%d) in window \"%s\" hit widget \"%s\" role=%s\n",
        x, y,
        win_name && win_name[0] ? win_name : "?",
        name && name[0] ? name : "?",
        role ? role : "?");
    g_free(role);
    g_free(name);
    g_free(win_name);

    const char *reason = "?";
    if (target != win && is_text_control(target, &reason)) {
        info.focused = 1;
        get_rects(target, &info);
    } else if (target == win) {
        reason = "tap did not hit a child widget";
    }

    fprintf(stderr,
        "vncAtSpi:   tap verdict: %s (%s)",
        info.focused ? "TEXT INPUT -> show keyboard" : "not a text input",
        reason);
    if (info.focused)
        fprintf(stderr,
            " caret=%dx%d+%d+%d field=%dx%d+%d+%d",
            info.caretW, info.caretH, info.caretX, info.caretY,
            info.fieldW, info.fieldH, info.fieldX, info.fieldY);
    fprintf(stderr, "\n");

    g_object_unref(target);
    g_object_unref(win);

    report_state_full(&info, 1);
}

/* Runs on the AT-SPI thread's main loop, ~100ms after the tap so the
 * application has processed the click and settled focus and caret. */
static gboolean process_tap(gpointer data) {
    (void) data;

    pthread_mutex_lock(&tap_mutex);
    int x = tap_x;
    int y = tap_y;
    tap_queued = 0;
    pthread_mutex_unlock(&tap_mutex);

    if (atspi_ready)
        hit_test_tap(x, y);
    return G_SOURCE_REMOVE;
}

static void on_focus_event(AtspiEvent *event, void *data) {
    if (!event->detail1) {
        g_boxed_free(ATSPI_TYPE_EVENT, event);
        return;
    }

    AtspiAccessible *obj = event->source;
    if (!obj) {
        g_boxed_free(ATSPI_TYPE_EVENT, event);
        return;
    }

    VncAtSpiFocusInfo info = {0};

    /* identify the tapped widget up front so every decision below can
     * be correlated with it in the trace */
    gchar *name = atspi_accessible_get_name(obj, NULL);
    gchar *role = atspi_accessible_get_role_name(obj, NULL);
    gchar *app_name = NULL;
    AtspiAccessible *app = atspi_accessible_get_application(obj, NULL);
    if (app) {
        app_name = atspi_accessible_get_name(app, NULL);
        g_object_unref(app);
    }

    fprintf(stderr,
        "vncAtSpi: event=%s detail1=%d app=\"%s\" widget=\"%s\" role=%s\n",
        event->type,
        event->detail1,
        app_name && app_name[0] ? app_name : "?",
        name && name[0] ? name : "?",
        role ? role : "?");

    /* "object:state-changed:focused" carries gain/loss in detail1;
     * legacy "focus:" events only fire on gain */
    int gained = strncmp(event->type, "focus", 5) == 0 || event->detail1;

    const char *reason = "focus lost, not classified";
    if (gained && is_text_control(obj, &reason)) {
        info.focused = 1;
        get_rects(obj, &info);
    }

    fprintf(stderr,
        "vncAtSpi:   verdict: %s (%s)",
        info.focused ? "TEXT INPUT -> show keyboard" : "not a text input",
        reason);
    if (info.focused)
        fprintf(stderr,
            " caret=%dx%d+%d+%d field=%dx%d+%d+%d",
            info.caretW, info.caretH, info.caretX, info.caretY,
            info.fieldW, info.fieldH, info.fieldX, info.fieldY);
    fprintf(stderr, "\n");

    g_free(app_name);
    g_free(role);
    g_free(name);

    report_state(&info);
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
        fprintf(stderr, "vncAtSpi: cannot reach session bus to enable a11y status flags\n");
        return;
    }

    static const char *props[] = {"IsEnabled", "ScreenReaderEnabled"};
    for (size_t i = 0; i < sizeof(props) / sizeof(props[0]); i++) {
        DBusMessage *msg = dbus_message_new_method_call(
            "org.a11y.Bus", "/org/a11y/bus", "org.freedesktop.DBus.Properties", "Set");
        if (!msg)
            continue;

        const char *status_iface = "org.a11y.Status";
        dbus_bool_t enabled = TRUE;
        DBusMessageIter iter, variant;
        dbus_message_iter_init_append(msg, &iter);
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &status_iface);
        dbus_message_iter_append_basic(&iter, DBUS_TYPE_STRING, &props[i]);
        dbus_message_iter_open_container(&iter, DBUS_TYPE_VARIANT,
            DBUS_TYPE_BOOLEAN_AS_STRING, &variant);
        dbus_message_iter_append_basic(&variant, DBUS_TYPE_BOOLEAN, &enabled);
        dbus_message_iter_close_container(&iter, &variant);

        DBusMessage *reply = dbus_connection_send_with_reply_and_block(conn, msg, 2000, NULL);
        fprintf(stderr, "vncAtSpi: set org.a11y.Status.%s=true %s\n",
            props[i], reply ? "ok" : "FAILED");
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
        if (!address)
            address = a11y_address_from_root_window();

        if (address) {
            if (a11y_address_alive(address))
                return address;
            free(address);
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
    fprintf(stderr, "vncAtSpi: connecting to a11y bus at %s\n", address);
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

    fprintf(stderr, "vncAtSpi: focus tracking active\n");
    atspi_ready = 1;
    /* a shutdown requested between init and here still quits: the idle
     * source added by vncAtSpiShutdown fires on the first iteration */
    atspi_event_main();

    atspi_ready = 0;
    atspi_event_listener_deregister(listener, "object:state-changed:focused", NULL);
    atspi_event_listener_deregister(listener, "focus:", NULL);
    g_object_unref(listener);
    atspi_exit();
    fprintf(stderr, "vncAtSpi: shut down\n");
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
        fprintf(stderr, "vncAtSpi: shutdown timed out, abandoning thread\n");
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

void vncAtSpiNotifyTap(int x, int y) {
    if (!thread_started)
        return;

    pthread_mutex_lock(&tap_mutex);
    tap_x = x;
    tap_y = y;
    int need_source = !tap_queued;
    tap_queued = 1;
    pthread_mutex_unlock(&tap_mutex);

    if (need_source)
        g_timeout_add(100, process_tap, NULL);
}

int vncAtSpiGetFocusState(VncAtSpiFocusInfo *out) {
    pthread_mutex_lock(&state_mutex);
    int had = pending;
    if (pending) {
        *out = pending_state;
        pending = 0;
    }
    pthread_mutex_unlock(&state_mutex);
    return had;
}
