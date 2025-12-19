/* loopyConnection - Per-connection I/O abstraction
 *
 * Implementation wrapping loopyStream for clean TCP connection management.
 */

#include "loopyConnection.h"

/* loopy headers */
#include "../../deps/loopy/src/loopy.h"
#include "../../deps/loopy/src/loopyIdle.h"
#include "../../deps/loopy/src/loopyStream.h"

#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Internal Structures
 * ============================================================================
 */

struct loopyConn {
    loopyStream *stream;
    loopyLoop *loop;
    loopyConnCallback *callback;
    void *userData;
    loopyConnState state;
    loopyConnConfig config;
    loopyConnStats stats;

    /* Read buffer */
    void *readBuf;
    size_t readBufSize;
    size_t readBufCapacity;

    /* Deferred close tracking.
     *
     * We cannot call loopyStreamClose from within a stream callback
     * (e.g., shutdown callback) as the stream may still be in use higher
     * in the call stack. Instead, we schedule an idle callback to perform
     * the close on the next event loop iteration.
     */
    bool closePending;
    loopyIdleHandle *closeIdleHandle;
};

/* ============================================================================
 * Default Configuration
 * ============================================================================
 */

#define DEFAULT_READ_BUF_SIZE (4 * 1024)
#define DEFAULT_MAX_READ_BUF_SIZE (64 * 1024)
#define DEFAULT_WRITE_BUF_SIZE (4 * 1024)

void loopyConnConfigInit(loopyConnConfig *cfg) {
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->readBufSize = DEFAULT_READ_BUF_SIZE;
    cfg->maxReadBufSize = DEFAULT_MAX_READ_BUF_SIZE;
    cfg->writeBufSize = DEFAULT_WRITE_BUF_SIZE;
    cfg->tcpNoDelay = true;
    cfg->tcpKeepAlive = true;
}

/* ============================================================================
 * Forward Declarations
 * ============================================================================
 */

static void connCloseCallback(loopyStream *stream, void *userData);

/* ============================================================================
 * Deferred Close (via idle callback)
 *
 * This mechanism safely closes a stream after all current callbacks have
 * completed. The idle callback runs at the start of the next event loop
 * iteration, ensuring the stream is no longer in use.
 *
 * IMPORTANT: We cannot free the idle handle from within:
 *   1. The idle callback itself (loopy is iterating the idle list)
 *   2. Any callback invoked from within the idle callback (same issue)
 *
 * Since loopyStreamClose may call the close callback synchronously (if no
 * pending writes), and that callback frees the conn, we must save the handle
 * reference externally and defer its cleanup.
 *
 * We use a simple pending-free list that gets processed by a prepare callback.
 * ============================================================================
 */

/* ============================================================================
 * Dynamic Pending-Free List for Idle Handles
 *
 * Uses a singly-linked list instead of a fixed-size array to handle any number
 * of simultaneous deferred closes without memory leaks. Each node is allocated
 * when scheduling cleanup and freed when processing the list.
 * ============================================================================
 */

typedef struct pendingIdleNode {
    loopyIdleHandle *handle;
    struct pendingIdleNode *next;
} pendingIdleNode;

static pendingIdleNode *pendingIdleFreeList = NULL;
static loopyPrepareHandle *cleanupPrepareHandle = NULL;

/**
 * cleanupPendingIdleHandles - Process and free all pending idle handles
 *
 * Called from a prepare callback at the start of each event loop iteration.
 * Frees all idle handles that were scheduled for cleanup during the previous
 * iteration.
 */
static void cleanupPendingIdleHandles(loopyLoop *loop,
                                      loopyPrepareHandle *handle,
                                      void *userData) {
    (void)loop;
    (void)handle;
    (void)userData;

    /* Process entire pending list */
    while (pendingIdleFreeList) {
        pendingIdleNode *node = pendingIdleFreeList;
        pendingIdleFreeList = node->next;

        if (node->handle) {
            loopyIdleFree(node->handle);
        }
        free(node);
    }

    /* Stop the prepare handle - we don't need it until next time */
    loopyPrepareStop(handle);
}

/**
 * scheduleIdleHandleFree - Schedule an idle handle for deferred cleanup
 *
 * Adds the handle to a linked list that will be processed by a prepare callback
 * at the start of the next event loop iteration. This allows any number of
 * handles to be scheduled without a fixed upper limit.
 *
 * @param loop       Event loop (for starting prepare callback if needed)
 * @param idleHandle Handle to schedule for cleanup (NULL is ignored)
 */
static void scheduleIdleHandleFree(loopyLoop *loop,
                                   loopyIdleHandle *idleHandle) {
    if (!idleHandle) {
        return;
    }

    /* Allocate node for linked list */
    pendingIdleNode *node = malloc(sizeof(*node));
    if (!node) {
        /* OOM: Best effort - free directly. This is risky but better than
         * leaking. */
        loopyIdleFree(idleHandle);
        return;
    }

    node->handle = idleHandle;
    node->next = pendingIdleFreeList;
    pendingIdleFreeList = node;

    /* Ensure cleanup prepare handle is running */
    if (!cleanupPrepareHandle) {
        cleanupPrepareHandle =
            loopyPrepareStart(loop, cleanupPendingIdleHandles, NULL);
    } else if (!loopyPrepareIsActive(cleanupPrepareHandle)) {
        loopyPrepareRestart(cleanupPrepareHandle);
    }
}

static bool deferredCloseIdleCallback(loopyLoop *loop, loopyIdleHandle *handle,
                                      void *userData) {
    loopyConn *conn = (loopyConn *)userData;

    /* Save handle reference BEFORE calling loopyStreamClose, because:
     * 1. loopyStreamClose may call connCloseCallback synchronously
     * 2. connCloseCallback frees conn
     * 3. We need to schedule handle cleanup after that */
    loopyIdleHandle *savedHandle = conn->closeIdleHandle;
    conn->closeIdleHandle = NULL;

    /* Now safe to close the stream */
    if (conn->stream) {
        /* WARNING: This may call connCloseCallback synchronously, freeing conn
         */
        loopyStreamClose(conn->stream, connCloseCallback, conn);
        /* conn is now potentially freed - do not access it */
    }

    /* Schedule the idle handle for deferred free.
     * We can't free it here because loopy will call loopyIdleStop on it
     * after we return false. The prepare callback will free it before
     * the next I/O poll. */
    scheduleIdleHandleFree(loop, savedHandle);

    /* Return false to auto-stop. Do NOT access handle after this -
     * loopy will call loopyIdleStop which requires the handle to be valid. */
    (void)handle; /* Suppress unused warning */
    return false;
}

/**
 * Schedule a deferred close via idle callback.
 *
 * This is called when we need to close from within a stream callback.
 * The actual close happens on the next event loop iteration.
 */
static void scheduleDeferredClose(loopyConn *conn) {
    /* Already scheduled? */
    if (conn->closePending) {
        return;
    }
    conn->closePending = true;

    /* Schedule idle callback for next iteration */
    conn->closeIdleHandle =
        loopyIdleStart(conn->loop, deferredCloseIdleCallback, conn);
    if (!conn->closeIdleHandle) {
        /* Fallback: close directly (may cause issues, but better than leak) */
        if (conn->stream) {
            loopyStreamClose(conn->stream, connCloseCallback, conn);
        }
    }
}

/* ============================================================================
 * Internal Callbacks
 * ============================================================================
 */

static void connAllocCallback(loopyStream *stream, size_t suggested, void **buf,
                              size_t *bufLen, void *userData) {
    loopyConn *conn = (loopyConn *)userData;
    (void)stream;
    (void)suggested;

    /* Ensure we have a read buffer */
    if (!conn->readBuf) {
        conn->readBufCapacity = conn->config.readBufSize;
        conn->readBuf = malloc(conn->readBufCapacity);
        if (!conn->readBuf) {
            *buf = NULL;
            *bufLen = 0;
            return;
        }
    }

    *buf = conn->readBuf;
    *bufLen = conn->readBufCapacity;
}

static void connReadCallback(loopyStream *stream, ssize_t nread,
                             const void *buf, void *userData) {
    loopyConn *conn = (loopyConn *)userData;
    (void)stream;

    if (nread > 0) {
        /* Data received */
        conn->stats.bytesRead += (uint64_t)nread;
        conn->stats.readCallbacks++;

        if (conn->callback) {
            conn->callback(conn, LOOPY_CONN_EVENT_DATA, buf, (size_t)nread,
                           conn->userData);
        }
    } else if (nread == 0) {
        /* EOF - peer closed */
        conn->state = LOOPY_CONN_CLOSED;
        if (conn->callback) {
            conn->callback(conn, LOOPY_CONN_EVENT_CLOSE, NULL, 0,
                           conn->userData);
        }
    } else {
        /* Error */
        conn->state = LOOPY_CONN_CLOSED;
        if (conn->callback) {
            conn->callback(conn, LOOPY_CONN_EVENT_ERROR, NULL, 0,
                           conn->userData);
        }
    }
}

static void connConnectCallback(loopyStream *stream, int status,
                                void *userData) {
    loopyConn *conn = (loopyConn *)userData;
    (void)stream;

    if (status == 0) {
        /* Connected successfully */
        conn->state = LOOPY_CONN_CONNECTED;

        /* Start reading */
        loopyStreamReadStart(conn->stream, connAllocCallback, connReadCallback,
                             conn);

        if (conn->callback) {
            conn->callback(conn, LOOPY_CONN_EVENT_CONNECTED, NULL, 0,
                           conn->userData);
        }
    } else {
        /* Connection failed */
        conn->state = LOOPY_CONN_CLOSED;
        if (conn->callback) {
            conn->callback(conn, LOOPY_CONN_EVENT_ERROR, NULL, 0,
                           conn->userData);
        }
    }
}

static void connCloseCallback(loopyStream *stream, void *userData) {
    loopyConn *conn = (loopyConn *)userData;
    (void)stream;

    conn->state = LOOPY_CONN_CLOSED;
    conn->stream = NULL;

    /* Note: We do NOT free conn->closeIdleHandle here.
     * If we're being called from within an idle callback (via deferred close),
     * the handle is managed by the prepare callback cleanup mechanism.
     * If we're being called directly (not from idle), closeIdleHandle is NULL.
     */

    if (conn->callback) {
        conn->callback(conn, LOOPY_CONN_EVENT_CLOSE, NULL, 0, conn->userData);
    }

    /* Free resources */
    free(conn->readBuf);
    free(conn);
}

static void connShutdownCallback(loopyStream *stream, int status,
                                 void *userData) {
    loopyConn *conn = (loopyConn *)userData;
    (void)status;
    (void)stream;

    /* After shutdown completes, schedule deferred close.
     *
     * We cannot call loopyStreamClose directly here because this callback
     * is invoked from within the stream's write callback chain. Closing
     * the stream from within its own callback would cause use-after-free.
     *
     * Instead, we use an idle callback to defer the close to the next
     * event loop iteration when the stream is no longer in use.
     */
    scheduleDeferredClose(conn);
}

/* ============================================================================
 * Internal Helpers
 * ============================================================================
 */

static loopyConn *connCreate(loopyLoop *loop, loopyStream *stream,
                             loopyConnCallback *cb, void *userData,
                             const loopyConnConfig *cfg) {
    loopyConn *conn = calloc(1, sizeof(*conn));
    if (!conn) {
        return NULL;
    }

    conn->stream = stream;
    conn->loop = loop;
    conn->callback = cb;
    conn->userData = userData;

    /* Apply configuration */
    if (cfg) {
        conn->config = *cfg;
    } else {
        loopyConnConfigInit(&conn->config);
    }

    return conn;
}

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

loopyConn *loopyConnFromStream(loopyStream *stream, loopyConnCallback *cb,
                               void *userData, const loopyConnConfig *cfg) {
    if (!stream) {
        return NULL;
    }

    loopyLoop *loop = loopyStreamGetLoop(stream);
    if (!loop) {
        return NULL;
    }

    loopyConn *conn = connCreate(loop, stream, cb, userData, cfg);
    if (!conn) {
        return NULL;
    }

    conn->state = LOOPY_CONN_CONNECTED;

    /* Start reading */
    if (!loopyStreamReadStart(stream, connAllocCallback, connReadCallback,
                              conn)) {
        free(conn);
        return NULL;
    }

    return conn;
}

loopyConn *loopyConnConnect(loopyLoop *loop, const char *addr, int port,
                            loopyConnCallback *cb, void *userData,
                            const loopyConnConfig *cfg) {
    if (!loop || !addr || port <= 0) {
        return NULL;
    }

    loopyStream *stream = loopyStreamNewTcp(loop);
    if (!stream) {
        return NULL;
    }

    loopyConn *conn = connCreate(loop, stream, cb, userData, cfg);
    if (!conn) {
        loopyStreamClose(stream, NULL, NULL);
        return NULL;
    }

    conn->state = LOOPY_CONN_CONNECTING;

    if (!loopyStreamConnect(stream, addr, port, connConnectCallback, conn)) {
        free(conn);
        loopyStreamClose(stream, NULL, NULL);
        return NULL;
    }

    return conn;
}

void loopyConnClose(loopyConn *conn) {
    if (!conn || conn->state == LOOPY_CONN_CLOSED ||
        conn->state == LOOPY_CONN_CLOSING || conn->closePending) {
        return;
    }

    conn->state = LOOPY_CONN_CLOSING;

    /* Graceful shutdown - flush writes then close */
    if (!loopyStreamShutdown(conn->stream, connShutdownCallback, conn)) {
        /* Shutdown failed, schedule deferred close */
        scheduleDeferredClose(conn);
    }
}

void loopyConnCloseImmediate(loopyConn *conn) {
    if (!conn || conn->state == LOOPY_CONN_CLOSED || conn->closePending) {
        return;
    }

    conn->state = LOOPY_CONN_CLOSING;
    /* For immediate close, use deferred mechanism to ensure safety */
    scheduleDeferredClose(conn);
}

/* ============================================================================
 * I/O Operations
 * ============================================================================
 */

bool loopyConnWrite(loopyConn *conn, const void *data, size_t len) {
    if (!conn || !data || len == 0) {
        return false;
    }

    if (conn->state != LOOPY_CONN_CONNECTED) {
        return false;
    }

    conn->stats.bytesWritten += len;
    conn->stats.writeCallbacks++;

    return loopyStreamWrite(conn->stream, data, len, NULL, NULL);
}

size_t loopyConnGetWriteQueueSize(const loopyConn *conn) {
    if (!conn || !conn->stream) {
        return 0;
    }
    return loopyStreamGetWriteQueueSize(conn->stream);
}

void loopyConnPauseRead(loopyConn *conn) {
    if (!conn || !conn->stream) {
        return;
    }
    loopyStreamReadStop(conn->stream);
}

void loopyConnResumeRead(loopyConn *conn) {
    if (!conn || !conn->stream) {
        return;
    }
    loopyStreamReadStart(conn->stream, connAllocCallback, connReadCallback,
                         conn);
}

/* ============================================================================
 * Properties
 * ============================================================================
 */

loopyConnState loopyConnGetState(const loopyConn *conn) {
    if (!conn) {
        return LOOPY_CONN_CLOSED;
    }
    return conn->state;
}

loopyLoop *loopyConnGetLoop(const loopyConn *conn) {
    if (!conn) {
        return NULL;
    }
    return conn->loop;
}

loopyStream *loopyConnGetStream(const loopyConn *conn) {
    if (!conn) {
        return NULL;
    }
    return conn->stream;
}

void *loopyConnGetUserData(const loopyConn *conn) {
    if (!conn) {
        return NULL;
    }
    return conn->userData;
}

void loopyConnSetUserData(loopyConn *conn, void *userData) {
    if (!conn) {
        return;
    }
    conn->userData = userData;
}

void loopyConnSetCallback(loopyConn *conn, loopyConnCallback *cb,
                          void *userData) {
    if (!conn) {
        return;
    }
    conn->callback = cb;
    conn->userData = userData;
}

bool loopyConnGetLocalAddr(loopyConn *conn, char *addr, size_t addrLen,
                           int *port) {
    if (!conn || !conn->stream || !addr || addrLen == 0) {
        return false;
    }
    return loopyStreamGetSockName(conn->stream, addr, addrLen, port);
}

bool loopyConnGetPeerAddr(loopyConn *conn, char *addr, size_t addrLen,
                          int *port) {
    if (!conn || !conn->stream || !addr || addrLen == 0) {
        return false;
    }
    return loopyStreamGetPeerName(conn->stream, addr, addrLen, port);
}

void loopyConnGetStats(const loopyConn *conn, loopyConnStats *stats) {
    if (!stats) {
        return;
    }

    if (!conn) {
        memset(stats, 0, sizeof(*stats));
        return;
    }

    *stats = conn->stats;
}
