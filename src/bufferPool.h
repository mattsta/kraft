#ifndef KRAFT_BUFFER_POOL_H
#define KRAFT_BUFFER_POOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Kraft Buffer Pool
 * =============================
 * Provides fast buffer allocation for RPC encoding without malloc per
 * operation.
 *
 * Design:
 * - Pre-allocated pool of fixed-size buffers
 * - O(1) allocation and deallocation
 * - Thread-local pools for multi-threaded usage (future)
 * - Automatic fallback to malloc for oversized requests
 *
 * Performance Benefits:
 * - Eliminates malloc/free overhead in RPC hot path
 * - Better cache locality (reused buffers stay hot)
 * - Reduced memory fragmentation
 */

/* Buffer sizes for different use cases */
#define KRAFT_BUFFER_SMALL 256   /* Headers, small RPCs */
#define KRAFT_BUFFER_MEDIUM 4096 /* Typical entries */
#define KRAFT_BUFFER_LARGE 65536 /* Batch entries */

/* Pool configuration */
#define KRAFT_POOL_SMALL_COUNT 64
#define KRAFT_POOL_MEDIUM_COUNT 32
#define KRAFT_POOL_LARGE_COUNT 8

/* Single buffer in the pool */
typedef struct kraftPoolBuffer {
    uint8_t *data;                /* Buffer data */
    size_t capacity;              /* Allocated capacity */
    size_t used;                  /* Current usage (for users) */
    bool inUse;                   /* Whether buffer is currently allocated */
    struct kraftPoolBuffer *next; /* Next in free list */
} kraftPoolBuffer;

/* Buffer pool for a single size class */
typedef struct kraftBufferPoolClass {
    kraftPoolBuffer *buffers;  /* Array of buffers */
    kraftPoolBuffer *freeList; /* Head of free list */
    uint32_t totalCount;       /* Total buffers in this class */
    uint32_t freeCount;        /* Currently free buffers */
    size_t bufferSize;         /* Size of each buffer */

    /* Statistics */
    uint64_t allocations;
    uint64_t hits;   /* Served from pool */
    uint64_t misses; /* Fell back to malloc */
} kraftBufferPoolClass;

/* Complete buffer pool with multiple size classes */
typedef struct kraftBufferPool {
    kraftBufferPoolClass small;  /* KRAFT_BUFFER_SMALL */
    kraftBufferPoolClass medium; /* KRAFT_BUFFER_MEDIUM */
    kraftBufferPoolClass large;  /* KRAFT_BUFFER_LARGE */

    /* Fallback allocation tracking */
    uint64_t fallbackAllocs;
    uint64_t fallbackBytes;
} kraftBufferPool;

/* =========================================================================
 * Initialization & Lifecycle
 * ========================================================================= */

/* Initialize buffer pool with default configuration */
bool kraftBufferPoolInit(kraftBufferPool *pool);

/* Free all buffers in pool */
void kraftBufferPoolFree(kraftBufferPool *pool);

/* Reset pool statistics */
void kraftBufferPoolResetStats(kraftBufferPool *pool);

/* =========================================================================
 * Allocation Interface
 * ========================================================================= */

/* Allocate buffer of at least 'size' bytes.
 * Returns NULL on failure. Buffer must be freed with kraftBufferPoolRelease. */
uint8_t *kraftBufferPoolAlloc(kraftBufferPool *pool, size_t size);

/* Get the actual capacity of an allocated buffer (may be larger than requested)
 */
size_t kraftBufferPoolCapacity(kraftBufferPool *pool, uint8_t *buf);

/* Release buffer back to pool */
void kraftBufferPoolRelease(kraftBufferPool *pool, uint8_t *buf);

/* =========================================================================
 * Statistics
 * ========================================================================= */

/* Pool statistics */
typedef struct kraftBufferPoolStats {
    uint64_t totalAllocations;
    uint64_t poolHits;
    uint64_t poolMisses;
    uint64_t fallbackAllocs;
    double hitRate;

    /* Per-class stats */
    struct {
        uint32_t total;
        uint32_t free;
        uint64_t allocations;
    } small, medium, large;
} kraftBufferPoolStats;

/* Get current pool statistics */
void kraftBufferPoolGetStats(const kraftBufferPool *pool,
                             kraftBufferPoolStats *stats);

/* Format statistics as string */
int kraftBufferPoolFormatStats(const kraftBufferPoolStats *stats, char *buf,
                               size_t bufLen);

#endif /* KRAFT_BUFFER_POOL_H */
