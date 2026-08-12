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

static void report_state(const VncAtSpiFocusInfo *info) {
    /* toolkits emit both "focus:" and "object:state-changed:focused" for
     * the same change; drop exact repeats */
    if (memcmp(info, &last_reported, sizeof(*info)) == 0)
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

static int is_text_control(AtspiAccessible *acc) {
    AtspiStateSet *states = atspi_accessible_get_state_set(acc);
    if (states) {
        gboolean editable = atspi_state_set_contains(states, ATSPI_STATE_EDITABLE);
        g_object_unref(states);
        if (editable)
            return 1;
    }

    /* terminals and some password fields do not report EDITABLE */
    AtspiRole role = atspi_accessible_get_role(acc, NULL);
    if (role == ATSPI_ROLE_TERMINAL || role == ATSPI_ROLE_PASSWORD_TEXT ||
        role == ATSPI_ROLE_ENTRY || role == ATSPI_ROLE_TEXT ||
        role == ATSPI_ROLE_SPIN_BUTTON)
        return 1;

    AtspiText *text_iface = atspi_accessible_get_text_iface(acc);
    if (text_iface) {
        g_object_unref(text_iface);
        return 1;
    }

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

    /* "object:state-changed:focused" carries gain/loss in detail1;
     * legacy "focus:" events only fire on gain */
    int gained = strncmp(event->type, "focus", 5) == 0 || event->detail1;

    if (gained && obj && is_text_control(obj)) {
        info.focused = 1;
        get_rects(obj, &info);
    }

    if (obj) {
        gchar *role = atspi_accessible_get_role_name(obj, NULL);
        fprintf(stderr,
            "vncAtSpi: event=%s detail1=%d role=%s -> focused=%d\n",
            event->type,
            event->detail1,
            role ? role : "?",
            info.focused);
        g_free(role);
    }

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

/* Ask org.a11y.Bus on the user's session bus for the a11y bus address.
 * This works even after an X server restart, unlike the AT_SPI_BUS root
 * window property, which at-spi-bus-launcher only sets once at its own
 * startup. */
static char *a11y_address_from_session_bus(void) {
    char busaddr[64];
    snprintf(busaddr, sizeof(busaddr), "unix:path=/run/user/%u/bus", (unsigned) getuid());

    DBusConnection *conn = dbus_connection_open_private(busaddr, NULL);
    if (!conn)
        return NULL;

    char *address = NULL;
    if (dbus_bus_register(conn, NULL)) {
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
    }
    dbus_connection_close(conn);
    dbus_connection_unref(conn);
    return address;
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
    /* a shutdown requested between init and here still quits: the idle
     * source added by vncAtSpiShutdown fires on the first iteration */
    atspi_event_main();

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
