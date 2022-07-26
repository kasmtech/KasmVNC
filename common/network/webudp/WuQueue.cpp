#include "WuQueue.h"
#include <stdlib.h>
#include <string.h>

static int32_t WuQueueFull(const WuQueue* q) {
  if (q->length == q->capacity) {
    return 1;
  }

  return 0;
}

WuQueue* WuQueueCreate(int32_t itemSize, int32_t capacity) {
  WuQueue* q = (WuQueue*)calloc(1, sizeof(WuQueue));
  WuQueueInit(q, itemSize, capacity);
  return q;
}

void WuQueueInit(WuQueue* q, int32_t itemSize, int32_t capacity) {
  memset(q, 0, sizeof(WuQueue));
  q->itemSize = itemSize;
  q->capacity = capacity;
  q->items = (uint8_t*)calloc(q->capacity, itemSize);
}

void WuQueuePush(WuQueue* q, const void* item) {
  if (WuQueueFull(q)) {
    int32_t newCap = q->capacity * 1.5;
    uint8_t* newItems = (uint8_t*)calloc(newCap, q->itemSize);

    int32_t nUpper = q->length - q->start;
    int32_t nLower = q->length - nUpper;
    memcpy(newItems, q->items + q->start * q->itemSize, q->itemSize * nUpper);
    memcpy(newItems + q->itemSize * nUpper, q->items, q->itemSize * nLower);

    free(q->items);

    q->start = 0;
    q->capacity = newCap;
    q->items = newItems;
  }

  const int32_t insertIdx =
      ((q->start + q->length) % q->capacity) * q->itemSize;
  memcpy(q->items + insertIdx, item, q->itemSize);
  q->length++;
}

int32_t WuQueuePop(WuQueue* q, void* item) {
  if (q->length > 0) {
    memcpy(item, q->items + q->start * q->itemSize, q->itemSize);
    q->start = (q->start + 1) % q->capacity;
    q->length--;
    return 1;
  }

  return 0;
}
