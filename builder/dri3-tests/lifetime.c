/* Copyright (c) 2026 Kasm. SPDX-License-Identifier: GPL-2.0-or-later */
/* Exercise real DRI3 imports, exports, deferred destruction, and a root export
 * retained until CloseScreen. Run with the server-side GBM lifetime audit. */
#include <assert.h>
#include <fcntl.h>
#include <gbm.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <xcb/dri3.h>
#include <xcb/xcb.h>

static void checked(xcb_connection_t *c, xcb_void_cookie_t cookie)
{
    xcb_generic_error_t *error = xcb_request_check(c, cookie);
    if (error) fprintf(stderr, "X error %u\n", error->error_code);
    assert(!error);
}

static void export_pixmap(xcb_connection_t *c, xcb_pixmap_t pixmap)
{
    xcb_generic_error_t *error = NULL;
    xcb_dri3_buffer_from_pixmap_reply_t *reply =
        xcb_dri3_buffer_from_pixmap_reply(c,
            xcb_dri3_buffer_from_pixmap(c, pixmap), &error);
    assert(!error && reply && reply->nfd == 1);
    int *fds = xcb_dri3_buffer_from_pixmap_reply_fds(c, reply);
    assert(fds[0] >= 0);
    close(fds[0]);
    free(reply);
}

int main(int argc, char **argv)
{
    assert(argc == 2);
    alarm(30);
    int fd = open(argv[1], O_RDWR | O_CLOEXEC);
    assert(fd >= 0);
    struct gbm_device *dev = gbm_create_device(fd);
    assert(dev);
    xcb_connection_t *c = xcb_connect(NULL, NULL);
    assert(!xcb_connection_has_error(c));
    xcb_screen_t *screen = xcb_setup_roots_iterator(xcb_get_setup(c)).data;
    assert(screen->root_depth == 24);
    xcb_dri3_query_version_reply_t *version = xcb_dri3_query_version_reply(
        c, xcb_dri3_query_version(c, 1, 0), NULL);
    assert(version && version->major_version >= 1);
    free(version);
    for (int i = 0; i < 100; i++) {
        struct gbm_bo *bo = gbm_bo_create(dev, 64, 64, GBM_FORMAT_XRGB8888,
            GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
        assert(bo);
        int dma = gbm_bo_get_fd(bo);
        assert(dma >= 0);
        xcb_pixmap_t imported = xcb_generate_id(c);
        checked(c, xcb_dri3_pixmap_from_buffer_checked(c, imported, screen->root,
            gbm_bo_get_stride(bo) * 64, 64, 64, gbm_bo_get_stride(bo), 24, 32, dma));
        /* xcb owns the transmitted fd. Alternate ordinary imports and imports
         * re-exported into the server's deferred texture list. */
        if (i % 2) export_pixmap(c, imported);
        checked(c, xcb_free_pixmap_checked(c, imported));
        gbm_bo_destroy(bo);
        xcb_pixmap_t exported = xcb_generate_id(c);
        checked(c, xcb_create_pixmap_checked(c, 24, exported, screen->root, 64, 64));
        export_pixmap(c, exported);
        export_pixmap(c, exported); /* Re-export must not take another reference. */
        checked(c, xcb_free_pixmap_checked(c, exported));
    }
    /* Let the server's 16ms texture timer release its final references. */
    usleep(250000);
    xcb_pixmap_t retained = xcb_generate_id(c);
    checked(c, xcb_create_pixmap_checked(c, 24, retained, screen->root, 64, 64));
    export_pixmap(c, retained);
    checked(c, xcb_change_window_attributes_checked(c, screen->root,
        XCB_CW_BACK_PIXMAP, &retained));
    checked(c, xcb_free_pixmap_checked(c, retained));
    xcb_disconnect(c);
    gbm_device_destroy(dev);
    close(fd);
    puts("100 import/export cycles plus retained root export completed");
    return 0;
}
