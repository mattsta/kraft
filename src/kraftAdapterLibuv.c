/* Kraft libuv Adapter Implementation
 * ====================================
 *
 * Reference implementation of kraftAdapter using libuv.
 * This serves as both a production-ready adapter and a template
 * for implementing other adapters (epoll, kqueue, io_uring, etc.)
 */

#include "kraftAdapterLibuv.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

/* Stub implementation for systems without libuv.
 * To enable real libuv support, compile with -DHAVE_LIBUV and link with -luv */

#ifndef HAVE_LIBUV

/* =========================================================================
 * Stub Implementation (no libuv available)
 * ========================================================================= */

static uint64_t stubNowUs(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + (uint64_t)tv.tv_usec;
}

static bool stubInit(kraftAdapter *adapter) {
    (void)adapter;
    return true;
}

static void stubDestroy(kraftAdapter *adapter) {
    if (adapter && adapter->impl) {
        free(adapter->impl);
        adapter->impl = NULL;
    }
}

static bool stubListen(kraftAdapter *adapter, const char *address,
                       uint16_t port, kraftConnectionType acceptAs) {
    (void)adapter;
    (void)address;
    (void)port;
    (void)acceptAs;
    return false; /* Not implemented without libuv */
}

static void stubStopListening(kraftAdapter *adapter) {
    (void)adapter;
}

static void stubRun(kraftAdapter *adapter) {
    (void)adapter;
    /* Would block here in real implementation */
}

static void stubStop(kraftAdapter *adapter) {
    (void)adapter;
}

static int stubPoll(kraftAdapter *adapter, uint64_t timeoutMs) {
    (void)adapter;
    (void)timeoutMs;
    return 0;
}

static kraftConnection *stubConnect(kraftAdapter *adapter, const char *address,
                                    uint16_t port, kraftConnectionType type) {
    (void)adapter;
    (void)address;
    (void)port;
    (void)type;
    return NULL;
}

static bool stubWrite(kraftAdapter *adapter, kraftConnection *conn,
                      const void *data, size_t len) {
    (void)adapter;
    (void)conn;
    (void)data;
    (void)len;
    return false;
}

static void stubClose(kraftAdapter *adapter, kraftConnection *conn) {
    (void)adapter;
    (void)conn;
}

static bool stubGetConnectionInfo(kraftConnection *conn, char *remoteIp,
                                  size_t ipLen, uint16_t *remotePort) {
    if (!conn) {
        return false;
    }

    if (remoteIp && ipLen > 0) {
        strncpy(remoteIp, conn->remoteIp, ipLen - 1);
        remoteIp[ipLen - 1] = '\0';
    }

    if (remotePort) {
        *remotePort = conn->remotePort;
    }

    return true;
}

static kraftTimer *stubTimerCreate(kraftAdapter *adapter, kraftTimerType type,
                                   uint64_t intervalMs, bool repeating,
                                   void *userData) {
    (void)adapter;

    kraftTimer *timer = calloc(1, sizeof(*timer));
    if (!timer) {
        return NULL;
    }

    static uint64_t nextId = 1;
    timer->id = nextId++;
    timer->type = type;
    timer->intervalMs = intervalMs;
    timer->repeating = repeating;
    timer->active = false;
    timer->userData = userData;

    return timer;
}

static bool stubTimerStart(kraftAdapter *adapter, kraftTimer *timer,
                           uint64_t intervalMs) {
    (void)adapter;
    if (!timer) {
        return false;
    }

    timer->intervalMs = intervalMs;
    timer->active = true;
    return true;
}

static void stubTimerStop(kraftAdapter *adapter, kraftTimer *timer) {
    (void)adapter;
    if (timer) {
        timer->active = false;
    }
}

static void stubTimerDestroy(kraftAdapter *adapter, kraftTimer *timer) {
    (void)adapter;
    free(timer);
}

static void stubTimerReset(kraftAdapter *adapter, kraftTimer *timer) {
    (void)adapter;
    (void)timer;
    /* Would reset timer countdown in real implementation */
}

static uint64_t stubNow(kraftAdapter *adapter) {
    (void)adapter;
    return stubNowUs();
}

static bool stubScheduleCallback(kraftAdapter *adapter,
                                 void (*callback)(void *), void *data) {
    (void)adapter;
    /* In real implementation, would schedule for next loop iteration */
    if (callback) {
        callback(data);
    }
    return true;
}

static uint32_t stubConnectionCount(kraftAdapter *adapter) {
    (void)adapter;
    return 0;
}

/* Stub operations table */
static const kraftAdapterOps stubOps = {
    .init = stubInit,
    .destroy = stubDestroy,
    .listen = stubListen,
    .stopListening = stubStopListening,
    .run = stubRun,
    .stop = stubStop,
    .poll = stubPoll,
    .connect = stubConnect,
    .write = stubWrite,
    .close = stubClose,
    .getConnectionInfo = stubGetConnectionInfo,
    .timerCreate = stubTimerCreate,
    .timerStart = stubTimerStart,
    .timerStop = stubTimerStop,
    .timerDestroy = stubTimerDestroy,
    .timerReset = stubTimerReset,
    .now = stubNow,
    .scheduleCallback = stubScheduleCallback,
    .connectionCount = stubConnectionCount,
};

const kraftAdapterOps *kraftAdapterLibuvGetOps(void) {
    return &stubOps;
}

kraftAdapter *kraftAdapterLibuvCreate(void) {
    return kraftAdapterCreate(&stubOps, NULL);
}

kraftAdapter *
kraftAdapterLibuvCreateWithConfig(const kraftAdapterLibuvConfig *config) {
    (void)config;
    return kraftAdapterLibuvCreate();
}

void kraftAdapterLibuvConfigDefault(kraftAdapterLibuvConfig *config) {
    if (!config) {
        return;
    }

    memset(config, 0, sizeof(*config));

    config->threadPoolSize = 4;
    config->tcpNoDelay = true;
    config->tcpKeepAlive = true;
    config->tcpKeepAliveDelaySec = 60;
    config->readBufferSize = 65536;
    config->writeQueueHighWater = 1024 * 1024;
    config->writeQueueLowWater = 65536;
    config->listenBacklog = 128;
}

void *kraftAdapterLibuvGetLoop(kraftAdapter *adapter) {
    (void)adapter;
    return NULL;
}

bool kraftAdapterLibuvAsync(kraftAdapter *adapter, void (*callback)(void *),
                            void *data) {
    return stubScheduleCallback(adapter, callback, data);
}

size_t kraftAdapterLibuvWriteQueueSize(kraftConnection *conn) {
    (void)conn;
    return 0;
}

bool kraftAdapterLibuvIsWritable(kraftConnection *conn) {
    (void)conn;
    return true;
}

void kraftAdapterLibuvSetAllocator(kraftAdapter *adapter,
                                   kraftLibuvAllocFn alloc,
                                   kraftLibuvFreeFn freeFn, void *userData) {
    (void)adapter;
    (void)alloc;
    (void)freeFn;
    (void)userData;
}

void kraftAdapterLibuvGetExtendedStats(const kraftAdapter *adapter,
                                       kraftAdapterLibuvStats *stats) {
    if (!stats) {
        return;
    }

    memset(stats, 0, sizeof(*stats));

    if (adapter) {
        kraftAdapterGetStats(adapter, &stats->base);
    }
}

#else /* HAVE_LIBUV */

/* =========================================================================
 * Real libuv Implementation
 * ========================================================================= */

#include <uv.h>

typedef struct libuvAdapter {
    uv_loop_t *loop;
    bool ownLoop; /* True if we created the loop */
    bool running;
    kraftAdapterLibuvConfig config;

    /* Connection tracking */
    uint32_t activeConnections;
    uint64_t nextConnId;
    uint64_t nextTimerId;

    /* Statistics */
    uint64_t loopIterations;
    uint64_t startTimeUs;

    /* Async handle for cross-thread stop */
    uv_async_t asyncStop;

    /* Custom allocator */
    kraftLibuvAllocFn customAlloc;
    kraftLibuvFreeFn customFree;
    void *allocUserData;

} libuvAdapter;

typedef struct libuvConnection {
    kraftConnection base; /* Must be first */
    uv_tcp_t handle;
    uv_connect_t connectReq;
    kraftAdapter *adapter;
    bool closing;
    size_t writeQueueSize;
} libuvConnection;

typedef struct libuvTimer {
    kraftTimer base; /* Must be first */
    uv_timer_t handle;
    kraftAdapter *adapter;
} libuvTimer;

typedef struct libuvWriteReq {
    uv_write_t req;
    uv_buf_t buf;
    kraftAdapter *adapter;
    libuvConnection *conn;
} libuvWriteReq;

/* ===== Helper Functions ===== */

static uint64_t libuvNowUs(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + (uint64_t)tv.tv_usec;
}

static void libuvAllocCb(uv_handle_t *handle, size_t suggestedSize,
                         uv_buf_t *buf) {
    libuvConnection *conn = (libuvConnection *)handle->data;
    libuvAdapter *impl = (libuvAdapter *)conn->adapter->impl;

    if (impl->customAlloc) {
        buf->base = impl->customAlloc(suggestedSize, impl->allocUserData);
    } else {
        buf->base = malloc(suggestedSize);
    }

    buf->len = buf->base ? suggestedSize : 0;
}

static void libuvCloseCb(uv_handle_t *handle) {
    libuvConnection *conn = (libuvConnection *)handle->data;
    if (conn) {
        libuvAdapter *impl = (libuvAdapter *)conn->adapter->impl;
        impl->activeConnections--;
        free(conn);
    }
}

static void libuvReadCb(uv_stream_t *stream, ssize_t nread,
                        const uv_buf_t *buf) {
    libuvConnection *conn = (libuvConnection *)stream->data;
    kraftAdapter *adapter = conn->adapter;
    libuvAdapter *impl = (libuvAdapter *)adapter->impl;

    if (nread > 0) {
        conn->base.bytesReceived += (uint64_t)nread;
        conn->base.messagesReceived++;
        adapter->stats.bytesReceived += (uint64_t)nread;

        KRAFT_ADAPTER_CALLBACK(adapter, onData, &conn->base, buf->base,
                               (size_t)nread);
    } else if (nread < 0) {
        const char *reason = (nread == UV_EOF) ? NULL : uv_strerror((int)nread);
        KRAFT_ADAPTER_CALLBACK(adapter, onClose, &conn->base, reason);

        conn->closing = true;
        uv_close((uv_handle_t *)stream, libuvCloseCb);
        adapter->stats.connectionsClosed++;
    }

    if (buf->base) {
        if (impl->customFree) {
            impl->customFree(buf->base, buf->len, impl->allocUserData);
        } else {
            free(buf->base);
        }
    }
}

static void libuvConnectionCb(uv_stream_t *server, int status) {
    kraftAdapter *adapter = (kraftAdapter *)server->data;
    libuvAdapter *impl = (libuvAdapter *)adapter->impl;

    if (status < 0) {
        return;
    }

    libuvConnection *conn = calloc(1, sizeof(*conn));
    if (!conn) {
        return;
    }

    conn->adapter = adapter;
    conn->base.id = impl->nextConnId++;
    conn->base.type = KRAFT_CONN_PEER_INBOUND;
    conn->base.state = KRAFT_CONN_STATE_CONNECTED;
    conn->base.connectTimeUs = libuvNowUs();

    uv_tcp_init(impl->loop, &conn->handle);
    conn->handle.data = conn;

    if (uv_accept(server, (uv_stream_t *)&conn->handle) == 0) {
        /* Get peer address */
        struct sockaddr_storage addr;
        int addrLen = sizeof(addr);
        uv_tcp_getpeername(&conn->handle, (struct sockaddr *)&addr, &addrLen);

        if (addr.ss_family == AF_INET) {
            struct sockaddr_in *in = (struct sockaddr_in *)&addr;
            uv_inet_ntop(AF_INET, &in->sin_addr, conn->base.remoteIp,
                         sizeof(conn->base.remoteIp));
            conn->base.remotePort = ntohs(in->sin_port);
        } else if (addr.ss_family == AF_INET6) {
            struct sockaddr_in6 *in6 = (struct sockaddr_in6 *)&addr;
            uv_inet_ntop(AF_INET6, &in6->sin6_addr, conn->base.remoteIp,
                         sizeof(conn->base.remoteIp));
            conn->base.remotePort = ntohs(in6->sin6_port);
        }

        /* Apply socket options */
        if (impl->config.tcpNoDelay) {
            uv_tcp_nodelay(&conn->handle, 1);
        }
        if (impl->config.tcpKeepAlive) {
            uv_tcp_keepalive(&conn->handle, 1,
                             impl->config.tcpKeepAliveDelaySec);
        }

        impl->activeConnections++;
        adapter->stats.connectionsAccepted++;

        KRAFT_ADAPTER_CALLBACK(adapter, onAccept, &conn->base,
                               KRAFT_CONN_PEER_INBOUND);

        uv_read_start((uv_stream_t *)&conn->handle, libuvAllocCb, libuvReadCb);
    } else {
        uv_close((uv_handle_t *)&conn->handle, libuvCloseCb);
    }
}

static void libuvConnectCb(uv_connect_t *req, int status) {
    libuvConnection *conn = (libuvConnection *)req->data;
    kraftAdapter *adapter = conn->adapter;
    libuvAdapter *impl = (libuvAdapter *)adapter->impl;

    if (status == 0) {
        conn->base.state = KRAFT_CONN_STATE_CONNECTED;
        impl->activeConnections++;

        KRAFT_ADAPTER_CALLBACK(adapter, onConnect, &conn->base, true, NULL);

        uv_read_start((uv_stream_t *)&conn->handle, libuvAllocCb, libuvReadCb);
    } else {
        const char *err = uv_strerror(status);
        KRAFT_ADAPTER_CALLBACK(adapter, onConnect, &conn->base, false, err);

        adapter->stats.connectionsFailed++;
        uv_close((uv_handle_t *)&conn->handle, libuvCloseCb);
    }
}

static void libuvWriteCb(uv_write_t *req, int status) {
    libuvWriteReq *writeReq = (libuvWriteReq *)req;
    kraftAdapter *adapter = writeReq->adapter;
    libuvConnection *conn = writeReq->conn;

    conn->writeQueueSize -= writeReq->buf.len;

    KRAFT_ADAPTER_CALLBACK(adapter, onWriteComplete, &conn->base,
                           writeReq->buf.len, status == 0);

    free(writeReq->buf.base);
    free(writeReq);
}

static void libuvTimerCb(uv_timer_t *handle) {
    libuvTimer *timer = (libuvTimer *)handle->data;
    kraftAdapter *adapter = timer->adapter;

    adapter->stats.timersFired++;

    KRAFT_ADAPTER_CALLBACK(adapter, onTimer, &timer->base);
}

static void libuvAsyncStopCb(uv_async_t *handle) {
    kraftAdapter *adapter = (kraftAdapter *)handle->data;
    libuvAdapter *impl = (libuvAdapter *)adapter->impl;

    impl->running = false;
    uv_stop(impl->loop);
}

/* ===== Adapter Operations ===== */

static bool libuvInit(kraftAdapter *adapter) {
    libuvAdapter *impl = calloc(1, sizeof(*impl));
    if (!impl) {
        return false;
    }

    kraftAdapterLibuvConfigDefault(&impl->config);

    impl->loop = uv_loop_new();
    if (!impl->loop) {
        free(impl);
        return false;
    }
    impl->ownLoop = true;

    impl->nextConnId = 1;
    impl->nextTimerId = 1;
    impl->startTimeUs = libuvNowUs();

    /* Initialize async handle for cross-thread stop */
    uv_async_init(impl->loop, &impl->asyncStop, libuvAsyncStopCb);
    impl->asyncStop.data = adapter;

    adapter->impl = impl;
    return true;
}

static void libuvDestroy(kraftAdapter *adapter) {
    if (!adapter || !adapter->impl) {
        return;
    }

    libuvAdapter *impl = (libuvAdapter *)adapter->impl;

    uv_close((uv_handle_t *)&impl->asyncStop, NULL);

    if (impl->ownLoop && impl->loop) {
        uv_loop_close(impl->loop);
        free(impl->loop);
    }

    free(impl);
    adapter->impl = NULL;
}

static bool libuvListen(kraftAdapter *adapter, const char *address,
                        uint16_t port, kraftConnectionType acceptAs) {
    libuvAdapter *impl = (libuvAdapter *)adapter->impl;

    uv_tcp_t *server = malloc(sizeof(*server));
    if (!server) {
        return false;
    }

    uv_tcp_init(impl->loop, server);
    server->data = adapter;

    struct sockaddr_in addr;
    uv_ip4_addr(address, port, &addr);

    uv_tcp_bind(server, (const struct sockaddr *)&addr, 0);

    int r = uv_listen((uv_stream_t *)server, impl->config.listenBacklog,
                      libuvConnectionCb);

    (void)acceptAs; /* Store for use in connection callback */

    return r == 0;
}

static void libuvStopListening(kraftAdapter *adapter) {
    (void)adapter;
    /* TODO: Track server handles and close them */
}

static void libuvRun(kraftAdapter *adapter) {
    libuvAdapter *impl = (libuvAdapter *)adapter->impl;
    impl->running = true;

    while (impl->running) {
        uv_run(impl->loop, UV_RUN_DEFAULT);
        impl->loopIterations++;
    }
}

static void libuvStop(kraftAdapter *adapter) {
    libuvAdapter *impl = (libuvAdapter *)adapter->impl;
    uv_async_send(&impl->asyncStop);
}

static int libuvPoll(kraftAdapter *adapter, uint64_t timeoutMs) {
    libuvAdapter *impl = (libuvAdapter *)adapter->impl;
    (void)timeoutMs;

    int r = uv_run(impl->loop, UV_RUN_NOWAIT);
    impl->loopIterations++;
    return r;
}

static kraftConnection *libuvConnect(kraftAdapter *adapter, const char *address,
                                     uint16_t port, kraftConnectionType type) {
    libuvAdapter *impl = (libuvAdapter *)adapter->impl;

    libuvConnection *conn = calloc(1, sizeof(*conn));
    if (!conn) {
        return NULL;
    }

    conn->adapter = adapter;
    conn->base.id = impl->nextConnId++;
    conn->base.type = type;
    conn->base.state = KRAFT_CONN_STATE_CONNECTING;

    strncpy(conn->base.remoteIp, address, sizeof(conn->base.remoteIp) - 1);
    conn->base.remotePort = port;

    uv_tcp_init(impl->loop, &conn->handle);
    conn->handle.data = conn;
    conn->connectReq.data = conn;

    struct sockaddr_in addr;
    uv_ip4_addr(address, port, &addr);

    int r = uv_tcp_connect(&conn->connectReq, &conn->handle,
                           (const struct sockaddr *)&addr, libuvConnectCb);

    if (r != 0) {
        free(conn);
        return NULL;
    }

    adapter->stats.connectionsInitiated++;
    return &conn->base;
}

static bool libuvWrite(kraftAdapter *adapter, kraftConnection *conn,
                       const void *data, size_t len) {
    libuvConnection *lconn = (libuvConnection *)conn;

    if (lconn->closing) {
        return false;
    }

    libuvWriteReq *req = malloc(sizeof(*req));
    if (!req) {
        return false;
    }

    req->buf.base = malloc(len);
    if (!req->buf.base) {
        free(req);
        return false;
    }

    memcpy(req->buf.base, data, len);
    req->buf.len = len;
    req->adapter = adapter;
    req->conn = lconn;

    lconn->writeQueueSize += len;
    conn->bytesSent += len;
    conn->messagesSent++;
    adapter->stats.bytesSent += len;

    int r = uv_write(&req->req, (uv_stream_t *)&lconn->handle, &req->buf, 1,
                     libuvWriteCb);

    if (r != 0) {
        lconn->writeQueueSize -= len;
        free(req->buf.base);
        free(req);
        return false;
    }

    return true;
}

static void libuvClose(kraftAdapter *adapter, kraftConnection *conn) {
    libuvConnection *lconn = (libuvConnection *)conn;
    (void)adapter;

    if (!lconn->closing) {
        lconn->closing = true;
        uv_close((uv_handle_t *)&lconn->handle, libuvCloseCb);
    }
}

static bool libuvGetConnectionInfo(kraftConnection *conn, char *remoteIp,
                                   size_t ipLen, uint16_t *remotePort) {
    if (!conn) {
        return false;
    }

    if (remoteIp && ipLen > 0) {
        strncpy(remoteIp, conn->remoteIp, ipLen - 1);
        remoteIp[ipLen - 1] = '\0';
    }

    if (remotePort) {
        *remotePort = conn->remotePort;
    }

    return true;
}

static kraftTimer *libuvTimerCreate(kraftAdapter *adapter, kraftTimerType type,
                                    uint64_t intervalMs, bool repeating,
                                    void *userData) {
    libuvAdapter *impl = (libuvAdapter *)adapter->impl;

    libuvTimer *timer = calloc(1, sizeof(*timer));
    if (!timer) {
        return NULL;
    }

    timer->adapter = adapter;
    timer->base.id = impl->nextTimerId++;
    timer->base.type = type;
    timer->base.intervalMs = intervalMs;
    timer->base.repeating = repeating;
    timer->base.active = false;
    timer->base.userData = userData;

    uv_timer_init(impl->loop, &timer->handle);
    timer->handle.data = timer;

    adapter->stats.timersCreated++;
    return &timer->base;
}

static bool libuvTimerStart(kraftAdapter *adapter, kraftTimer *timer,
                            uint64_t intervalMs) {
    libuvTimer *ltimer = (libuvTimer *)timer;
    (void)adapter;

    timer->intervalMs = intervalMs;
    timer->active = true;

    uint64_t repeat = timer->repeating ? intervalMs : 0;
    return uv_timer_start(&ltimer->handle, libuvTimerCb, intervalMs, repeat) ==
           0;
}

static void libuvTimerStop(kraftAdapter *adapter, kraftTimer *timer) {
    libuvTimer *ltimer = (libuvTimer *)timer;
    (void)adapter;

    timer->active = false;
    uv_timer_stop(&ltimer->handle);
}

static void libuvTimerCloseCb(uv_handle_t *handle) {
    libuvTimer *timer = (libuvTimer *)handle->data;
    free(timer);
}

static void libuvTimerDestroy(kraftAdapter *adapter, kraftTimer *timer) {
    libuvTimer *ltimer = (libuvTimer *)timer;
    (void)adapter;

    uv_close((uv_handle_t *)&ltimer->handle, libuvTimerCloseCb);
}

static void libuvTimerReset(kraftAdapter *adapter, kraftTimer *timer) {
    libuvTimer *ltimer = (libuvTimer *)timer;
    (void)adapter;

    if (timer->active) {
        uv_timer_again(&ltimer->handle);
    }
}

static uint64_t libuvNow(kraftAdapter *adapter) {
    (void)adapter;
    return libuvNowUs();
}

static bool libuvScheduleCallback(kraftAdapter *adapter,
                                  void (*callback)(void *), void *data) {
    /* For libuv, we'd use uv_async or uv_check */
    (void)adapter;
    if (callback) {
        callback(data);
    }
    return true;
}

static uint32_t libuvConnectionCount(kraftAdapter *adapter) {
    libuvAdapter *impl = (libuvAdapter *)adapter->impl;
    return impl->activeConnections;
}

/* Real libuv operations table */
static const kraftAdapterOps libuvOps = {
    .init = libuvInit,
    .destroy = libuvDestroy,
    .listen = libuvListen,
    .stopListening = libuvStopListening,
    .run = libuvRun,
    .stop = libuvStop,
    .poll = libuvPoll,
    .connect = libuvConnect,
    .write = libuvWrite,
    .close = libuvClose,
    .getConnectionInfo = libuvGetConnectionInfo,
    .timerCreate = libuvTimerCreate,
    .timerStart = libuvTimerStart,
    .timerStop = libuvTimerStop,
    .timerDestroy = libuvTimerDestroy,
    .timerReset = libuvTimerReset,
    .now = libuvNow,
    .scheduleCallback = libuvScheduleCallback,
    .connectionCount = libuvConnectionCount,
};

const kraftAdapterOps *kraftAdapterLibuvGetOps(void) {
    return &libuvOps;
}

kraftAdapter *kraftAdapterLibuvCreate(void) {
    return kraftAdapterCreate(&libuvOps, NULL);
}

kraftAdapter *
kraftAdapterLibuvCreateWithConfig(const kraftAdapterLibuvConfig *config) {
    kraftAdapter *adapter = kraftAdapterCreate(&libuvOps, NULL);
    if (!adapter) {
        return NULL;
    }

    libuvAdapter *impl = (libuvAdapter *)adapter->impl;
    if (config) {
        impl->config = *config;
    }

    return adapter;
}

void kraftAdapterLibuvConfigDefault(kraftAdapterLibuvConfig *config) {
    if (!config) {
        return;
    }

    memset(config, 0, sizeof(*config));

    config->threadPoolSize = 4;
    config->tcpNoDelay = true;
    config->tcpKeepAlive = true;
    config->tcpKeepAliveDelaySec = 60;
    config->readBufferSize = 65536;
    config->writeQueueHighWater = 1024 * 1024;
    config->writeQueueLowWater = 65536;
    config->listenBacklog = 128;
}

void *kraftAdapterLibuvGetLoop(kraftAdapter *adapter) {
    if (!adapter || !adapter->impl) {
        return NULL;
    }
    libuvAdapter *impl = (libuvAdapter *)adapter->impl;
    return impl->loop;
}

bool kraftAdapterLibuvAsync(kraftAdapter *adapter, void (*callback)(void *),
                            void *data) {
    return libuvScheduleCallback(adapter, callback, data);
}

size_t kraftAdapterLibuvWriteQueueSize(kraftConnection *conn) {
    libuvConnection *lconn = (libuvConnection *)conn;
    return lconn ? lconn->writeQueueSize : 0;
}

bool kraftAdapterLibuvIsWritable(kraftConnection *conn) {
    libuvConnection *lconn = (libuvConnection *)conn;
    if (!lconn) {
        return false;
    }
    /* Default high water mark */
    return lconn->writeQueueSize < (1024 * 1024);
}

void kraftAdapterLibuvSetAllocator(kraftAdapter *adapter,
                                   kraftLibuvAllocFn alloc,
                                   kraftLibuvFreeFn freeFn, void *userData) {
    if (!adapter || !adapter->impl) {
        return;
    }
    libuvAdapter *impl = (libuvAdapter *)adapter->impl;
    impl->customAlloc = alloc;
    impl->customFree = freeFn;
    impl->allocUserData = userData;
}

void kraftAdapterLibuvGetExtendedStats(const kraftAdapter *adapter,
                                       kraftAdapterLibuvStats *stats) {
    if (!stats) {
        return;
    }

    memset(stats, 0, sizeof(*stats));

    if (!adapter || !adapter->impl) {
        return;
    }

    kraftAdapterGetStats(adapter, &stats->base);

    libuvAdapter *impl = (libuvAdapter *)adapter->impl;
    stats->loopIterations = impl->loopIterations;
    stats->activeHandles = (uint64_t)uv_loop_alive(impl->loop);

    if (impl->loopIterations > 0) {
        uint64_t elapsed = libuvNowUs() - impl->startTimeUs;
        stats->avgLoopTimeMs =
            (double)elapsed / (double)impl->loopIterations / 1000.0;
    }
}

#endif /* HAVE_LIBUV */

/* =========================================================================
 * Base Adapter Functions (shared)
 * ========================================================================= */

kraftAdapter *kraftAdapterCreate(const kraftAdapterOps *ops, void *userData) {
    if (!ops) {
        return NULL;
    }

    kraftAdapter *adapter = calloc(1, sizeof(*adapter));
    if (!adapter) {
        return NULL;
    }

    adapter->ops = ops;
    adapter->userData = userData;

    kraftAdapterConfigDefault(adapter);

    if (ops->init && !ops->init(adapter)) {
        free(adapter);
        return NULL;
    }

    return adapter;
}

void kraftAdapterConfigDefault(kraftAdapter *adapter) {
    if (!adapter) {
        return;
    }

    adapter->config.maxConnections = 1024;
    adapter->config.recvBufferSize = 65536;
    adapter->config.sendBufferSize = 65536;
    adapter->config.connectTimeoutMs = 5000;
    adapter->config.writeTimeoutMs = 30000;
    adapter->config.enableTcpNodelay = true;
    adapter->config.enableKeepalive = true;
    adapter->config.keepaliveIntervalS = 60;
}

void kraftAdapterDestroy(kraftAdapter *adapter) {
    if (!adapter) {
        return;
    }

    if (adapter->ops && adapter->ops->destroy) {
        adapter->ops->destroy(adapter);
    }

    free(adapter);
}

void kraftAdapterSetState(kraftAdapter *adapter, struct kraftState *state) {
    if (adapter) {
        adapter->state = state;
    }
}

void kraftAdapterGetStats(const kraftAdapter *adapter,
                          kraftAdapterStats *stats) {
    if (!stats) {
        return;
    }

    memset(stats, 0, sizeof(*stats));

    if (!adapter) {
        return;
    }

    stats->connectionsAccepted = adapter->stats.connectionsAccepted;
    stats->connectionsInitiated = adapter->stats.connectionsInitiated;
    stats->connectionsFailed = adapter->stats.connectionsFailed;
    stats->connectionsClosed = adapter->stats.connectionsClosed;
    stats->bytesReceived = adapter->stats.bytesReceived;
    stats->bytesSent = adapter->stats.bytesSent;
    stats->timersFired = adapter->stats.timersFired;

    if (adapter->ops && adapter->ops->connectionCount) {
        stats->activeConnections =
            adapter->ops->connectionCount((kraftAdapter *)adapter);
    }
}

int kraftAdapterFormatStats(const kraftAdapterStats *stats, char *buf,
                            size_t bufLen) {
    if (!stats || !buf || bufLen == 0) {
        return 0;
    }

    return snprintf(buf, bufLen,
                    "KraftAdapter Stats:\n"
                    "  Connections accepted:  %" PRIu64 "\n"
                    "  Connections initiated: %" PRIu64 "\n"
                    "  Connections failed:    %" PRIu64 "\n"
                    "  Connections closed:    %" PRIu64 "\n"
                    "  Active connections:    %" PRIu64 "\n"
                    "  Bytes received:        %" PRIu64 "\n"
                    "  Bytes sent:            %" PRIu64 "\n"
                    "  Timers fired:          %" PRIu64 "\n",
                    stats->connectionsAccepted, stats->connectionsInitiated,
                    stats->connectionsFailed, stats->connectionsClosed,
                    stats->activeConnections, stats->bytesReceived,
                    stats->bytesSent, stats->timersFired);
}
