/* Copyright (c) 2023 Kasm
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
 * 
 * You should have received a copy of the GNU General Public License
 * along with this software; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307,
 * USA.
*/

#ifdef HAVE_DIX_CONFIG_H
#include <dix-config.h>
#endif

#ifdef DRI3

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <X11/X.h>
#include <X11/Xmd.h>
#include <dri3.h>
#include <drm_fourcc.h>
#include <fb.h>
#include <gcstruct.h>
#include <gbm.h>

extern const char *driNode;

static struct priv_t {
    struct gbm_device *gbm;
    int fd;
} priv;

struct gbm_pixmap {
    struct gbm_bo *bo;
};

typedef struct gbm_pixmap gbm_pixmap;

static DevPrivateKeyRec dri3_pixmap_private_key;
static struct timeval start;

struct texpixmap {
    PixmapPtr pixmap;
    struct xorg_list entry;
};

static struct xorg_list texpixmaps;
static CARD32 update_texpixmaps(OsTimerPtr timer, CARD32 time, void *arg);
static OsTimerPtr texpixmaptimer;

void xvnc_sync_dri3_textures(void);
void xvnc_sync_dri3_pixmap(PixmapPtr pixmap);
void xvnc_init_dri3(void);


static int
xvnc_dri3_open_client(ClientPtr client,
                     ScreenPtr screen,
                     RRProviderPtr provider,
                     int *pfd)
{
    int fd = open(driNode, O_RDWR | O_CLOEXEC);
    if (fd < 0)
        return BadAlloc;
    *pfd = fd;
    return Success;
}

static uint32_t
gbm_format_for_depth(CARD8 depth)
{
    switch (depth) {
    case 16:
        return GBM_FORMAT_RGB565;
    case 24:
        return GBM_FORMAT_XRGB8888;
    case 30:
        return GBM_FORMAT_ARGB2101010;
    default:
        ErrorF("unexpected depth: %d\n", depth);
        /* fallthrough */
    case 32:
        return GBM_FORMAT_ARGB8888;
    }

}

static void dri3_pixmap_set_private(PixmapPtr pixmap, gbm_pixmap *gp)
{
    dixSetPrivate(&pixmap->devPrivates, &dri3_pixmap_private_key, gp);
}

static gbm_pixmap *gbm_pixmap_get(PixmapPtr pixmap)
{
    return dixLookupPrivate(&pixmap->devPrivates, &dri3_pixmap_private_key);
}

static void add_texpixmap(PixmapPtr pix)
{
    struct texpixmap *ptr;

    xorg_list_for_each_entry(ptr, &texpixmaps, entry) {
        if (ptr->pixmap == pix)
            return;
    }

    ptr = calloc(1, sizeof(struct texpixmap));
    ptr->pixmap = pix;
    pix->refcnt++;
    xorg_list_append(&ptr->entry, &texpixmaps);

    // start if not running
    if (!texpixmaptimer)
        texpixmaptimer = TimerSet(NULL, 0, 16, update_texpixmaps, NULL);
}

static PixmapPtr
create_pixmap_for_bo(ScreenPtr screen, struct gbm_bo *bo, CARD8 depth)
{
    PixmapPtr pixmap;

    gbm_pixmap *gp = calloc(1, sizeof(gbm_pixmap));
    if (!gp)
        return NULL;

    pixmap = screen->CreatePixmap(screen, gbm_bo_get_width(bo), gbm_bo_get_height(bo),
                                  depth, CREATE_PIXMAP_USAGE_SCRATCH);
    if (!pixmap)
        return NULL;

    gp->bo = bo;
    dri3_pixmap_set_private(pixmap, gp);

    return pixmap;
}

static PixmapPtr
xvnc_pixmap_from_fds(ScreenPtr screen, CARD8 num_fds, const int *fds,
                       CARD16 width, CARD16 height,
                       const CARD32 *strides, const CARD32 *offsets,
                       CARD8 depth, CARD8 bpp, uint64_t modifier)
{
    struct gbm_bo *bo = NULL;
    PixmapPtr pixmap;

    if (width == 0 || height == 0 || num_fds == 0 ||
        depth < 15 || bpp != BitsPerPixel(depth) ||
        strides[0] < width * bpp / 8)
        return NULL;

    if (num_fds == 1) {
        struct gbm_import_fd_data data;

        data.fd = fds[0];
        data.width = width;
        data.height = height;
        data.stride = strides[0];
        data.format = gbm_format_for_depth(depth);
        bo = gbm_bo_import(priv.gbm, GBM_BO_IMPORT_FD, &data,
                           GBM_BO_USE_RENDERING);
        if (!bo)
            return NULL;
    } else {
        return NULL;
    }

    pixmap = create_pixmap_for_bo(screen, bo, depth);
    if (pixmap == NULL) {
        gbm_bo_destroy(bo);
        return NULL;
    }

    return pixmap;
}

static int
xvnc_fds_from_pixmap(ScreenPtr screen, PixmapPtr pixmap, int *fds,
                     uint32_t *strides, uint32_t *offsets,
                     uint64_t *modifier)
{
    gbm_pixmap *gp = gbm_pixmap_get(pixmap);

    if (!gp) {
        gp = calloc(1, sizeof(gbm_pixmap));
        if (!gp)
            return 0;
        gp->bo = gbm_bo_create(priv.gbm,
                               pixmap->drawable.width,
                               pixmap->drawable.height,
                               gbm_format_for_depth(pixmap->drawable.depth),
                               (pixmap->usage_hint == CREATE_PIXMAP_USAGE_SHARED ?
                                GBM_BO_USE_LINEAR : 0) |
                                GBM_BO_USE_RENDERING | GBM_BO_USE_SCANOUT);
        if (!gp->bo) {
            ErrorF("Failed to create bo\n");
            return 0;
        }

        dri3_pixmap_set_private(pixmap, gp);
    }

    fds[0] = gbm_bo_get_fd(gp->bo);
    strides[0] = gbm_bo_get_stride(gp->bo);
    offsets[0] = 0;
    *modifier = DRM_FORMAT_MOD_INVALID;

    add_texpixmap(pixmap);

    return 1;
}

static Bool
xvnc_get_formats(ScreenPtr screen,
                 CARD32 *num_formats, CARD32 **formats)
{
    ErrorF("xvnc_get_formats\n");
    return FALSE;
}

static Bool
xvnc_get_modifiers(ScreenPtr screen, uint32_t format,
                   uint32_t *num_modifiers, uint64_t **modifiers)
{
    ErrorF("xvnc_get_modifiers\n");
    return FALSE;
}

static Bool
xvnc_get_drawable_modifiers(DrawablePtr draw, uint32_t format,
                            uint32_t *num_modifiers, uint64_t **modifiers)
{
    ErrorF("xvnc_get_drawable_modifiers\n");
    return FALSE;
}

static const dri3_screen_info_rec xvnc_dri3_info = {
    .version = 2,
    .open = NULL,
    .pixmap_from_fds = xvnc_pixmap_from_fds,
    .fds_from_pixmap = xvnc_fds_from_pixmap,
    .open_client = xvnc_dri3_open_client,
    .get_formats = xvnc_get_formats,
    .get_modifiers = xvnc_get_modifiers,
    .get_drawable_modifiers = xvnc_get_drawable_modifiers,
};

void xvnc_sync_dri3_pixmap(PixmapPtr pixmap)
{
    // There doesn't seem to be a good hook or sync point, so we do it manually
    // here, right before Present copies from the pixmap
    DrawablePtr pDraw;
    GCPtr gc;
    void *ptr;
    uint32_t stride, w, h;
    void *opaque = NULL;
    gbm_pixmap *gp;

    // We may not be running on hw if there's a compositor using PRESENT on llvmpipe
    if (!driNode)
        return;

    gp = gbm_pixmap_get(pixmap);
    if (!gp) {
        //ErrorF("Present tried to copy from a non-dri3 pixmap\n");
        return;
    }

    w = gbm_bo_get_width(gp->bo);
    h = gbm_bo_get_height(gp->bo);

    ptr = gbm_bo_map(gp->bo, 0, 0, w, h,
                     GBM_BO_TRANSFER_READ, &stride, &opaque);
    if (!ptr) {
        ErrorF("gbm map failed, errno %d\n", errno);
        return;
    }

    pDraw = &pixmap->drawable;
    if ((gc = GetScratchGC(pDraw->depth, pDraw->pScreen))) {
        ValidateGC(pDraw, gc);
        //gc->ops->PutImage(pDraw, gc, pDraw->depth, 0, 0, w, h, 0, ZPixmap, data);
        fbPutZImage(pDraw, fbGetCompositeClip(gc), gc->alu, fbGetGCPrivate(gc)->pm,
                    0, 0, w, h, ptr, stride / sizeof(FbStip));
        FreeScratchGC(gc);
    }

    gbm_bo_unmap(gp->bo, opaque);
}

void xvnc_sync_dri3_textures(void)
{
    // Sync the tracked pixmaps into their textures (bos)
    // This is a bit of an ugly solution, but we don't know
    // when the pixmaps have changed nor when the textures are read.
    //
    // This is called both from the global damage report and the timer,
    // to account for cases that do not use the damage report.

    uint32_t y;
    gbm_pixmap *gp;
    uint8_t *src, *dst;
    uint32_t srcstride, dststride;
    void *opaque = NULL;
    struct texpixmap *ptr, *tmpptr;

    // We may not be running on hw if there's a compositor using PRESENT on llvmpipe
    if (!driNode)
        return;

    xorg_list_for_each_entry_safe(ptr, tmpptr, &texpixmaps, entry) {
        if (ptr->pixmap->refcnt == 1) {
            // We are the only user left, delete it
            ptr->pixmap->drawable.pScreen->DestroyPixmap(ptr->pixmap);
            xorg_list_del(&ptr->entry);
            free(ptr);
            continue;
        }

        gp = gbm_pixmap_get(ptr->pixmap);
        opaque = NULL;
        dst = gbm_bo_map(gp->bo, 0, 0,
                         ptr->pixmap->drawable.width,
                         ptr->pixmap->drawable.height,
                         GBM_BO_TRANSFER_WRITE, &dststride, &opaque);
        if (!dst) {
            ErrorF("gbm map failed, errno %d\n", errno);
            continue;
        }

        srcstride = ptr->pixmap->devKind;
        src = ptr->pixmap->devPrivate.ptr;

        for (y = 0; y < ptr->pixmap->drawable.height; y++) {
            memcpy(dst, src, srcstride);
            dst += dststride;
            src += srcstride;
        }

        gbm_bo_unmap(gp->bo, opaque);
    }
}

static CARD32 update_texpixmaps(OsTimerPtr timer, CARD32 time, void *arg)
{
    xvnc_sync_dri3_textures();

    if (xorg_list_is_empty(&texpixmaps)) {
        TimerFree(texpixmaptimer);
        texpixmaptimer = NULL;
        return 0;
    }

    return 16; // Reschedule next tick
}

void xvnc_init_dri3(void)
{
    memset(&priv, 0, sizeof(priv));

    gettimeofday(&start, NULL);

    if (!dixRegisterPrivateKey(&dri3_pixmap_private_key, PRIVATE_PIXMAP, 0))
        FatalError("dix\n");

    if (!driNode)
        driNode = "/dev/dri/renderD128";

    priv.fd = open(driNode, O_RDWR | O_CLOEXEC);
    if (!priv.fd)
        FatalError("Failed to open %s\n", driNode);

    priv.gbm = gbm_create_device(priv.fd);
    if (!priv.gbm)
        FatalError("Failed to create gbm\n");

    xorg_list_init(&texpixmaps);

    if (!dri3_screen_init(screenInfo.screens[0], &xvnc_dri3_info))
        FatalError("Couldn't init dri3\n");
}

#endif // DRI3
