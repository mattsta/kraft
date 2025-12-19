#include "bufferPool.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * Internal: Size Class Initialization
 * ========================================================================= */

static bool initPoolClass(kraftBufferPoolClass *cls, size_t bufferSize,
                          uint32_t count) {
    cls->bufferSize = bufferSize;
    cls->totalCount = count;
    cls->freeCount = count;
    cls->allocations = 0;
    cls->hits = 0;
    cls->misses = 0;
    cls->freeList = NULL;

    if (count == 0) {
        cls->buffers = NULL;
        return true;
    }

    /* Allocate buffer metadata array */
    cls->buffers = calloc(count, sizeof(kraftPoolBuffer));
    if (!cls->buffers) {
        return false;
    }

    /* Allocate each buffer and build free list */
    for (uint32_t i = 0; i < count; i++) {
        cls->buffers[i].data = malloc(bufferSize);
        if (!cls->buffers[i].data) {
            /* Cleanup on failure */
            for (uint32_t j = 0; j < i; j++) {
                free(cls->buffers[j].data);
            }
            free(cls->buffers);
            cls->buffers = NULL;
            return false;
        }
        cls->buffers[i].capacity = bufferSize;
        cls->buffers[i].used = 0;
        cls->buffers[i].inUse = false;
        cls->buffers[i].next = cls->freeList;
        cls->freeList = &cls->buffers[i];
    }

    return true;
}

static void freePoolClass(kraftBufferPoolClass *cls) {
    if (!cls->buffers) {
        return;
    }

    for (uint32_t i = 0; i < cls->totalCount; i++) {
        free(cls->buffers[i].data);
    }
    free(cls->buffers);
    cls->buffers = NULL;
    cls->freeList = NULL;
    cls->freeCount = 0;
}

/* =========================================================================
 * Internal: Find buffer in pool
 * ========================================================================= */

static kraftPoolBuffer *findBufferInClass(kraftBufferPoolClass *cls,
                                          uint8_t *buf) {
    if (!cls->buffers) {
        return NULL;
    }

    for (uint32_t i = 0; i < cls->totalCount; i++) {
        if (cls->buffers[i].data == buf) {
            return &cls->buffers[i];
        }
    }
    return NULL;
}

/* =========================================================================
 * Initialization & Lifecycle
 * ========================================================================= */

bool kraftBufferPoolInit(kraftBufferPool *pool) {
    memset(pool, 0, sizeof(*pool));

    if (!initPoolClass(&pool->small, KRAFT_BUFFER_SMALL,
                       KRAFT_POOL_SMALL_COUNT)) {
        return false;
    }

    if (!initPoolClass(&pool->medium, KRAFT_BUFFER_MEDIUM,
                       KRAFT_POOL_MEDIUM_COUNT)) {
        freePoolClass(&pool->small);
        return false;
    }

    if (!initPoolClass(&pool->large, KRAFT_BUFFER_LARGE,
                       KRAFT_POOL_LARGE_COUNT)) {
        freePoolClass(&pool->small);
        freePoolClass(&pool->medium);
        return false;
    }

    return true;
}

void kraftBufferPoolFree(kraftBufferPool *pool) {
    freePoolClass(&pool->small);
    freePoolClass(&pool->medium);
    freePoolClass(&pool->large);
    memset(pool, 0, sizeof(*pool));
}

void kraftBufferPoolResetStats(kraftBufferPool *pool) {
    pool->small.allocations = 0;
    pool->small.hits = 0;
    pool->small.misses = 0;
    pool->medium.allocations = 0;
    pool->medium.hits = 0;
    pool->medium.misses = 0;
    pool->large.allocations = 0;
    pool->large.hits = 0;
    pool->large.misses = 0;
    pool->fallbackAllocs = 0;
    pool->fallbackBytes = 0;
}

/* =========================================================================
 * Allocation Interface
 * ========================================================================= */

static uint8_t *allocFromClass(kraftBufferPoolClass *cls, size_t size) {
    cls->allocations++;

    if (cls->freeList) {
        /* Take from free list */
        kraftPoolBuffer *buf = cls->freeList;
        cls->freeList = buf->next;
        buf->next = NULL;
        buf->inUse = true;
        buf->used = size;
        cls->freeCount--;
        cls->hits++;
        return buf->data;
    }

    /* Pool exhausted */
    cls->misses++;
    return NULL;
}

uint8_t *kraftBufferPoolAlloc(kraftBufferPool *pool, size_t size) {
    uint8_t *buf = NULL;

    /* Try appropriate size class */
    if (size <= KRAFT_BUFFER_SMALL) {
        buf = allocFromClass(&pool->small, size);
        if (buf) {
            return buf;
        }
    }

    if (size <= KRAFT_BUFFER_MEDIUM) {
        buf = allocFromClass(&pool->medium, size);
        if (buf) {
            return buf;
        }

        /* Try larger class if medium exhausted */
        buf = allocFromClass(&pool->large, size);
        if (buf) {
            return buf;
        }
    }

    if (size <= KRAFT_BUFFER_LARGE) {
        buf = allocFromClass(&pool->large, size);
        if (buf) {
            return buf;
        }
    }

    /* Fallback to malloc for oversized or exhausted pools */
    pool->fallbackAllocs++;
    pool->fallbackBytes += size;
    return malloc(size);
}

size_t kraftBufferPoolCapacity(kraftBufferPool *pool, uint8_t *buf) {
    kraftPoolBuffer *pb;

    pb = findBufferInClass(&pool->small, buf);
    if (pb) {
        return pb->capacity;
    }

    pb = findBufferInClass(&pool->medium, buf);
    if (pb) {
        return pb->capacity;
    }

    pb = findBufferInClass(&pool->large, buf);
    if (pb) {
        return pb->capacity;
    }

    /* Fallback buffer - we don't track its size */
    return 0;
}

static void releaseToClass(kraftBufferPoolClass *cls, kraftPoolBuffer *buf) {
    buf->inUse = false;
    buf->used = 0;
    buf->next = cls->freeList;
    cls->freeList = buf;
    cls->freeCount++;
}

void kraftBufferPoolRelease(kraftBufferPool *pool, uint8_t *buf) {
    if (!buf) {
        return;
    }

    kraftPoolBuffer *pb;

    pb = findBufferInClass(&pool->small, buf);
    if (pb) {
        releaseToClass(&pool->small, pb);
        return;
    }

    pb = findBufferInClass(&pool->medium, buf);
    if (pb) {
        releaseToClass(&pool->medium, pb);
        return;
    }

    pb = findBufferInClass(&pool->large, buf);
    if (pb) {
        releaseToClass(&pool->large, pb);
        return;
    }

    /* Not from pool - was a fallback allocation */
    free(buf);
}

/* =========================================================================
 * Statistics
 * ========================================================================= */

void kraftBufferPoolGetStats(const kraftBufferPool *pool,
                             kraftBufferPoolStats *stats) {
    memset(stats, 0, sizeof(*stats));

    stats->small.total = pool->small.totalCount;
    stats->small.free = pool->small.freeCount;
    stats->small.allocations = pool->small.allocations;

    stats->medium.total = pool->medium.totalCount;
    stats->medium.free = pool->medium.freeCount;
    stats->medium.allocations = pool->medium.allocations;

    stats->large.total = pool->large.totalCount;
    stats->large.free = pool->large.freeCount;
    stats->large.allocations = pool->large.allocations;

    stats->poolHits = pool->small.hits + pool->medium.hits + pool->large.hits;
    stats->poolMisses =
        pool->small.misses + pool->medium.misses + pool->large.misses;
    stats->totalAllocations =
        stats->poolHits + stats->poolMisses + pool->fallbackAllocs;
    stats->fallbackAllocs = pool->fallbackAllocs;

    if (stats->totalAllocations > 0) {
        stats->hitRate =
            (double)stats->poolHits / (double)stats->totalAllocations;
    } else {
        stats->hitRate = 1.0;
    }
}

int kraftBufferPoolFormatStats(const kraftBufferPoolStats *stats, char *buf,
                               size_t bufLen) {
    return snprintf(
        buf, bufLen,
        "Buffer Pool Stats:\n"
        "  Total Allocations: %llu (hits=%llu misses=%llu fallback=%llu)\n"
        "  Hit Rate: %.1f%%\n"
        "  Small  (%d bytes): %u/%u free, %llu allocs\n"
        "  Medium (%d bytes): %u/%u free, %llu allocs\n"
        "  Large  (%d bytes): %u/%u free, %llu allocs\n",
        (unsigned long long)stats->totalAllocations,
        (unsigned long long)stats->poolHits,
        (unsigned long long)stats->poolMisses,
        (unsigned long long)stats->fallbackAllocs, stats->hitRate * 100.0,
        KRAFT_BUFFER_SMALL, stats->small.free, stats->small.total,
        (unsigned long long)stats->small.allocations, KRAFT_BUFFER_MEDIUM,
        stats->medium.free, stats->medium.total,
        (unsigned long long)stats->medium.allocations, KRAFT_BUFFER_LARGE,
        stats->large.free, stats->large.total,
        (unsigned long long)stats->large.allocations);
}
