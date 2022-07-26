#pragma once

#include <stdint.h>

struct WuPool;

WuPool* WuPoolCreate(int32_t blockSize, int32_t numBlocks);
void WuPoolDestroy(WuPool* pool);
void* WuPoolAcquire(WuPool* pool);
void WuPoolRelease(WuPool* pool, void* ptr);
