/* Kraft libuv Adapter Implementation
 * ====================================
 *
 * This is a reference implementation of the kraftAdapter interface using libuv.
 * It demonstrates how to implement a pluggable network adapter.
 *
 * libuv provides:
 *   - Cross-platform event loop (Windows, Linux, macOS, *BSD)
 *   - TCP/UDP networking
 *   - Timers
 *   - Async I/O
 *
 * Usage:
 *   kraftAdapter *adapter = kraftAdapterLibuvCreate();
 *   kraftAdapterSetState(adapter, kraftState);
 *   adapter->ops->listen(adapter, "0.0.0.0", 7001, KRAFT_CONN_PEER);
 *   adapter->ops->run(adapter);  // blocks
 *   kraftAdapterDestroy(adapter);
 *
 * Requirements:
 *   - libuv >= 1.0 (install via: apt install libuv1-dev, brew install libuv)
 *   - Link with: -luv
 *
 * Thread Safety:
 *   - All operations must be called from the event loop thread
 *   - kraftAdapterStop() is the only thread-safe operation
 */

#ifndef KRAFT_ADAPTER_LIBUV_H
#define KRAFT_ADAPTER_LIBUV_H

#include "kraftAdapter.h"

/* =========================================================================
 * libuv-specific Configuration
 * ========================================================================= */

typedef struct kraftAdapterLibuvConfig {
    /* Thread pool size for async operations */
    uint32_t threadPoolSize; /* Default: 4 */

    /* TCP options */
    bool tcpNoDelay;               /* Disable Nagle (default: true) */
    bool tcpKeepAlive;             /* Enable keepalive (default: true) */
    uint32_t tcpKeepAliveDelaySec; /* Keepalive delay (default: 60) */

    /* Buffer sizes */
    size_t readBufferSize;      /* Suggested read buffer (default: 64KB) */
    size_t writeQueueHighWater; /* Pause writes above this (default: 1MB) */
    size_t writeQueueLowWater;  /* Resume writes below this (default: 64KB) */

    /* Accept queue */
    int listenBacklog; /* TCP listen backlog (default: 128) */

} kraftAdapterLibuvConfig;

/* =========================================================================
 * Creation and Configuration
 * ========================================================================= */

/* Get the libuv adapter operations table */
const kraftAdapterOps *kraftAdapterLibuvGetOps(void);

/* Create libuv adapter with default configuration */
kraftAdapter *kraftAdapterLibuvCreate(void);

/* Create libuv adapter with custom configuration */
kraftAdapter *
kraftAdapterLibuvCreateWithConfig(const kraftAdapterLibuvConfig *config);

/* Initialize default libuv configuration */
void kraftAdapterLibuvConfigDefault(kraftAdapterLibuvConfig *config);

/* =========================================================================
 * libuv-Specific Operations
 * ========================================================================= */

/* Get the underlying uv_loop_t* (for advanced integration) */
void *kraftAdapterLibuvGetLoop(kraftAdapter *adapter);

/* Queue a callback to run on the event loop thread (thread-safe)
 * This is useful for cross-thread communication */
bool kraftAdapterLibuvAsync(kraftAdapter *adapter, void (*callback)(void *),
                            void *data);

/* Get write queue size for a connection (for flow control) */
size_t kraftAdapterLibuvWriteQueueSize(kraftConnection *conn);

/* Check if connection is writable (write queue below high water) */
bool kraftAdapterLibuvIsWritable(kraftConnection *conn);

/* =========================================================================
 * Connection Extensions
 * ========================================================================= */

/* Set custom allocator for read buffers */
typedef void *(*kraftLibuvAllocFn)(size_t suggestedSize, void *userData);
typedef void (*kraftLibuvFreeFn)(void *buf, size_t len, void *userData);

void kraftAdapterLibuvSetAllocator(kraftAdapter *adapter,
                                   kraftLibuvAllocFn alloc,
                                   kraftLibuvFreeFn free, void *userData);

/* =========================================================================
 * Statistics Extensions
 * ========================================================================= */

typedef struct kraftAdapterLibuvStats {
    /* Base stats */
    kraftAdapterStats base;

    /* libuv-specific */
    uint64_t loopIterations;  /* Number of event loop iterations */
    uint64_t activeHandles;   /* Currently active handles */
    uint64_t activeRequests;  /* Currently active requests */
    uint64_t writeQueueBytes; /* Total bytes in write queues */
    double avgLoopTimeMs;     /* Average loop iteration time */
} kraftAdapterLibuvStats;

void kraftAdapterLibuvGetExtendedStats(const kraftAdapter *adapter,
                                       kraftAdapterLibuvStats *stats);

#endif /* KRAFT_ADAPTER_LIBUV_H */
