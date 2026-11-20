/* Copyright (c) 2026 Kasm. SPDX-License-Identifier: GPL-2.0-or-later */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <gbm.h>
#include <stdio.h>
#include <stdlib.h>
static struct gbm_bo *bos[8192];
static unsigned created, destroyed;
static void track(struct gbm_bo *bo) {
    if (!bo) return;
    for (int i=0;i<8192;i++) if (!bos[i]) {bos[i]=bo; created++; fprintf(stderr,"GBM lifetime: created=%u destroyed=%u live=%u\n", created, destroyed, created-destroyed); return;}
    abort();
}
struct gbm_bo *gbm_bo_create(struct gbm_device *d, unsigned w, unsigned h, unsigned f, unsigned u) {
    struct gbm_bo *(*next)(struct gbm_device *,unsigned,unsigned,unsigned,unsigned)=dlsym(RTLD_NEXT,"gbm_bo_create");
    struct gbm_bo *bo=next(d,w,h,f,u); track(bo); return bo;
}
struct gbm_bo *gbm_bo_import(struct gbm_device *d, unsigned t, void *b, unsigned u) {
    struct gbm_bo *(*next)(struct gbm_device *,unsigned,void *,unsigned)=dlsym(RTLD_NEXT,"gbm_bo_import");
    struct gbm_bo *bo=next(d,t,b,u); track(bo); return bo;
}
void gbm_bo_destroy(struct gbm_bo *bo) {
    void (*next)(struct gbm_bo *)=dlsym(RTLD_NEXT,"gbm_bo_destroy");
    for (int i=0;i<8192;i++) if (bos[i]==bo) {bos[i]=NULL; destroyed++; fprintf(stderr,"GBM lifetime: created=%u destroyed=%u live=%u\n", created, destroyed, created-destroyed); break;}
    next(bo);
}
void gbm_device_destroy(struct gbm_device *d) {
    void (*next)(struct gbm_device *)=dlsym(RTLD_NEXT,"gbm_device_destroy");
    unsigned live=0;
    for (int i=0;i<8192;i++) if (bos[i] && gbm_bo_get_device(bos[i])==d) live++;
    fprintf(stderr,"GBM audit: created=%u destroyed=%u live_at_device_destroy=%u\n",created,destroyed,live);
    if(live) abort();
    next(d);
}
