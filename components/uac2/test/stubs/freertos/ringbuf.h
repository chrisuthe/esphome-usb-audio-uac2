#pragma once
#include "freertos/FreeRTOS.h"
// A ring buffer that only counts bytes: the tests look at how much the driver holds, never at the audio in it.
typedef void *RingbufHandle_t;
typedef enum { RINGBUF_TYPE_NOSPLIT, RINGBUF_TYPE_ALLOWSPLIT, RINGBUF_TYPE_BYTEBUF } RingbufferType_t;
static size_t g_ring_fill;
static size_t g_ring_capacity = SIZE_MAX;
static uint8_t g_ring_zeros[65536];
// A recording test sets this to see the bytes the driver stores, in the order it stores them
static void (*g_ring_on_send)(const uint8_t *data, size_t size);
static inline RingbufHandle_t xRingbufferCreate(size_t s, RingbufferType_t t) { return (void *)1; }
static inline void vRingbufferDelete(RingbufHandle_t r) {}
static inline BaseType_t xRingbufferSend(RingbufHandle_t r, const void *d, size_t s, TickType_t t)
{
    if (s > g_ring_capacity - g_ring_fill) {
        return pdFALSE;
    }
    if (g_ring_on_send) {
        g_ring_on_send((const uint8_t *)d, s);
    }
    g_ring_fill += s;
    return pdTRUE;
}
static inline void *xRingbufferReceiveUpTo(RingbufHandle_t r, size_t *n, TickType_t t, size_t m)
{
    size_t take = m < g_ring_fill ? m : g_ring_fill;
    if (take > sizeof(g_ring_zeros)) {
        take = sizeof(g_ring_zeros);
    }
    *n = take;
    if (take == 0) {
        return NULL;
    }
    g_ring_fill -= take;
    return g_ring_zeros;
}
static inline void vRingbufferReturnItem(RingbufHandle_t r, void *i) {}
static inline void vRingbufferGetInfo(RingbufHandle_t r, void *a, void *b, void *c, void *d, void *w) { if (w) *(size_t *)w = g_ring_fill; }
