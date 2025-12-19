# Implementing Custom Adapters

This guide explains how to implement a custom network adapter for Kraft, allowing you to integrate with any event loop or networking library.

## Overview

A Kraft adapter abstracts:

- **Connections**: TCP accept, connect, read, write, close
- **Timers**: Create, start, stop, reset
- **Event Loop**: Run, stop, poll

Built-in adapters:

- `kraftAdapterLibuv` - libuv-based (cross-platform, production-ready)
- Stub adapter (for testing without real networking)

You might implement a custom adapter for:

- Integration with existing event loops (Qt, GTK, game engines)
- High-performance I/O (io_uring on Linux)
- Embedded systems with custom networking
- Simulation/testing environments

---

## Adapter Architecture

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                              kraftAdapter                                    │
│                                                                              │
│  ┌──────────────────────────────────────────────────────────────────────┐   │
│  │                         kraftAdapterOps                               │   │
│  │                                                                       │   │
│  │  Lifecycle:    init(), destroy()                                      │   │
│  │  Server:       listen(), stopListening(), run(), stop(), poll()      │   │
│  │  Connections:  connect(), write(), close(), getConnectionInfo()      │   │
│  │  Timers:       timerCreate(), timerStart(), timerStop(), etc.        │   │
│  │  Utilities:    now(), scheduleCallback(), connectionCount()          │   │
│  │                                                                       │   │
│  └──────────────────────────────────────────────────────────────────────┘   │
│                                                                              │
│  Callbacks:                                                                  │
│  ├── onAccept(adapter, conn, type)                                          │
│  ├── onConnect(adapter, conn, success, errorMsg)                            │
│  ├── onData(adapter, conn, data, len)                                       │
│  ├── onClose(adapter, conn, reason)                                         │
│  ├── onWriteComplete(adapter, conn, bytesWritten, success)                  │
│  └── onTimer(adapter, timer)                                                │
│                                                                              │
│  State:                                                                      │
│  ├── impl (your implementation data)                                        │
│  ├── state (back-reference to kraftState)                                   │
│  ├── config (connection limits, timeouts, etc.)                             │
│  └── stats (bytes sent/received, connections, etc.)                         │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## Step 1: Define Your Implementation Structure

```c
#include "kraftAdapter.h"
#include <your_event_loop.h>

// Your implementation-specific data
typedef struct MyAdapterImpl {
    your_loop_t *loop;           // Your event loop handle
    bool running;                // Event loop running flag
    bool ownLoop;                // True if we created the loop

    // Connection tracking
    uint32_t activeConnections;
    uint64_t nextConnId;
    uint64_t nextTimerId;

    // Statistics
    uint64_t startTimeUs;

    // Configuration
    int listenBacklog;
    bool tcpNoDelay;

} MyAdapterImpl;

// Your connection wrapper
typedef struct MyConnection {
    kraftConnection base;        // MUST be first member
    your_socket_t socket;        // Your socket handle
    your_buffer_t readBuffer;    // Read buffer
    bool closing;                // Close in progress
} MyConnection;

// Your timer wrapper
typedef struct MyTimer {
    kraftTimer base;             // MUST be first member
    your_timer_t timer;          // Your timer handle
    kraftAdapter *adapter;       // Back-reference
} MyTimer;
```

---

## Step 2: Implement Required Operations

### Lifecycle Operations

```c
static bool myAdapterInit(kraftAdapter *adapter) {
    MyAdapterImpl *impl = calloc(1, sizeof(*impl));
    if (!impl) {
        return false;
    }

    // Initialize your event loop
    impl->loop = your_loop_create();
    if (!impl->loop) {
        free(impl);
        return false;
    }
    impl->ownLoop = true;

    // Initialize state
    impl->nextConnId = 1;
    impl->nextTimerId = 1;
    impl->startTimeUs = myGetTimeUs();

    // Store implementation
    adapter->impl = impl;

    return true;
}

static void myAdapterDestroy(kraftAdapter *adapter) {
    if (!adapter || !adapter->impl) {
        return;
    }

    MyAdapterImpl *impl = adapter->impl;

    // Cleanup your event loop
    if (impl->ownLoop && impl->loop) {
        your_loop_destroy(impl->loop);
    }

    free(impl);
    adapter->impl = NULL;
}
```

### Server Operations

```c
static bool myAdapterListen(kraftAdapter *adapter,
                            const char *address,
                            uint16_t port,
                            kraftConnectionType acceptAs) {
    MyAdapterImpl *impl = adapter->impl;

    // Create listening socket
    your_socket_t server = your_socket_create();
    your_socket_bind(server, address, port);
    your_socket_listen(server, impl->listenBacklog);

    // Set up accept callback
    your_socket_on_accept(server, onAcceptCallback, adapter);

    // Store accept type for later
    your_socket_set_user_data(server, (void *)(uintptr_t)acceptAs);

    return true;
}

// Called by your event loop when a connection is accepted
static void onAcceptCallback(your_socket_t server,
                             your_socket_t clientSocket,
                             void *userData) {
    kraftAdapter *adapter = userData;
    MyAdapterImpl *impl = adapter->impl;
    kraftConnectionType type = (kraftConnectionType)(uintptr_t)
        your_socket_get_user_data(server);

    // Create connection wrapper
    MyConnection *conn = calloc(1, sizeof(*conn));
    conn->base.id = impl->nextConnId++;
    conn->base.type = type;
    conn->base.state = KRAFT_CONN_STATE_CONNECTED;
    conn->base.connectTimeUs = myGetTimeUs();
    conn->socket = clientSocket;

    // Get peer address
    your_socket_get_peer(clientSocket,
                         conn->base.remoteIp,
                         sizeof(conn->base.remoteIp),
                         &conn->base.remotePort);

    // Set up read callback
    your_socket_on_read(clientSocket, onReadCallback, conn);

    impl->activeConnections++;
    adapter->stats.connectionsAccepted++;

    // Notify Kraft core
    KRAFT_ADAPTER_CALLBACK(adapter, onAccept, &conn->base, type);
}

static void myAdapterRun(kraftAdapter *adapter) {
    MyAdapterImpl *impl = adapter->impl;
    impl->running = true;

    while (impl->running) {
        your_loop_run_once(impl->loop);
    }
}

static void myAdapterStop(kraftAdapter *adapter) {
    MyAdapterImpl *impl = adapter->impl;
    impl->running = false;

    // If your loop supports async wakeup:
    your_loop_wakeup(impl->loop);
}

static int myAdapterPoll(kraftAdapter *adapter, uint64_t timeoutMs) {
    MyAdapterImpl *impl = adapter->impl;
    return your_loop_poll(impl->loop, timeoutMs);
}
```

### Connection Operations

```c
static kraftConnection *myAdapterConnect(kraftAdapter *adapter,
                                          const char *address,
                                          uint16_t port,
                                          kraftConnectionType type) {
    MyAdapterImpl *impl = adapter->impl;

    // Create connection
    MyConnection *conn = calloc(1, sizeof(*conn));
    conn->base.id = impl->nextConnId++;
    conn->base.type = type;
    conn->base.state = KRAFT_CONN_STATE_CONNECTING;

    strncpy(conn->base.remoteIp, address, sizeof(conn->base.remoteIp) - 1);
    conn->base.remotePort = port;

    // Initiate async connect
    conn->socket = your_socket_create();
    your_socket_connect_async(conn->socket, address, port,
                              onConnectCallback, conn);

    adapter->stats.connectionsInitiated++;
    return &conn->base;
}

static void onConnectCallback(your_socket_t socket,
                               bool success,
                               const char *error,
                               void *userData) {
    MyConnection *conn = userData;
    kraftAdapter *adapter = /* get from socket or conn */;
    MyAdapterImpl *impl = adapter->impl;

    if (success) {
        conn->base.state = KRAFT_CONN_STATE_CONNECTED;
        impl->activeConnections++;

        // Set up read callback
        your_socket_on_read(socket, onReadCallback, conn);

        KRAFT_ADAPTER_CALLBACK(adapter, onConnect, &conn->base, true, NULL);
    } else {
        adapter->stats.connectionsFailed++;
        KRAFT_ADAPTER_CALLBACK(adapter, onConnect, &conn->base, false, error);

        // Cleanup failed connection
        your_socket_close(socket);
        free(conn);
    }
}

static bool myAdapterWrite(kraftAdapter *adapter,
                           kraftConnection *conn,
                           const void *data,
                           size_t len) {
    MyConnection *myConn = (MyConnection *)conn;

    if (myConn->closing) {
        return false;
    }

    // Queue async write
    bool success = your_socket_write_async(myConn->socket,
                                            data, len,
                                            onWriteCallback, myConn);

    if (success) {
        conn->bytesSent += len;
        conn->messagesSent++;
        adapter->stats.bytesSent += len;
    }

    return success;
}

static void onWriteCallback(your_socket_t socket,
                             size_t bytesWritten,
                             bool success,
                             void *userData) {
    MyConnection *conn = userData;
    kraftAdapter *adapter = /* get from socket or conn */;

    KRAFT_ADAPTER_CALLBACK(adapter, onWriteComplete,
                           &conn->base, bytesWritten, success);
}

static void onReadCallback(your_socket_t socket,
                            const void *data,
                            size_t len,
                            void *userData) {
    MyConnection *conn = userData;
    kraftAdapter *adapter = /* get from socket or conn */;

    if (len > 0) {
        conn->base.bytesReceived += len;
        conn->base.messagesReceived++;
        adapter->stats.bytesReceived += len;

        KRAFT_ADAPTER_CALLBACK(adapter, onData, &conn->base, data, len);
    } else if (len == 0) {
        // EOF - connection closed by peer
        onCloseCallback(socket, NULL, userData);
    }
}

static void myAdapterClose(kraftAdapter *adapter, kraftConnection *conn) {
    MyConnection *myConn = (MyConnection *)conn;

    if (myConn->closing) {
        return;
    }
    myConn->closing = true;

    your_socket_close_async(myConn->socket, onCloseCallback, myConn);
}

static void onCloseCallback(your_socket_t socket,
                             const char *reason,
                             void *userData) {
    MyConnection *conn = userData;
    kraftAdapter *adapter = /* get from socket or conn */;
    MyAdapterImpl *impl = adapter->impl;

    impl->activeConnections--;
    adapter->stats.connectionsClosed++;

    KRAFT_ADAPTER_CALLBACK(adapter, onClose, &conn->base, reason);

    // Free connection
    your_socket_destroy(socket);
    free(conn);
}
```

### Timer Operations

```c
static kraftTimer *myAdapterTimerCreate(kraftAdapter *adapter,
                                         kraftTimerType type,
                                         uint64_t intervalMs,
                                         bool repeating,
                                         void *userData) {
    MyAdapterImpl *impl = adapter->impl;

    MyTimer *timer = calloc(1, sizeof(*timer));
    timer->base.id = impl->nextTimerId++;
    timer->base.type = type;
    timer->base.intervalMs = intervalMs;
    timer->base.repeating = repeating;
    timer->base.active = false;
    timer->base.userData = userData;
    timer->adapter = adapter;

    timer->timer = your_timer_create(impl->loop);
    your_timer_set_user_data(timer->timer, timer);

    adapter->stats.timersCreated++;
    return &timer->base;
}

static bool myAdapterTimerStart(kraftAdapter *adapter,
                                 kraftTimer *timer,
                                 uint64_t intervalMs) {
    MyTimer *myTimer = (MyTimer *)timer;

    timer->intervalMs = intervalMs;
    timer->active = true;

    uint64_t repeat = timer->repeating ? intervalMs : 0;
    your_timer_start(myTimer->timer, intervalMs, repeat, onTimerCallback);

    return true;
}

static void onTimerCallback(your_timer_t timer, void *userData) {
    MyTimer *myTimer = userData;
    kraftAdapter *adapter = myTimer->adapter;

    adapter->stats.timersFired++;

    KRAFT_ADAPTER_CALLBACK(adapter, onTimer, &myTimer->base);
}

static void myAdapterTimerStop(kraftAdapter *adapter, kraftTimer *timer) {
    MyTimer *myTimer = (MyTimer *)timer;
    timer->active = false;
    your_timer_stop(myTimer->timer);
}

static void myAdapterTimerDestroy(kraftAdapter *adapter, kraftTimer *timer) {
    MyTimer *myTimer = (MyTimer *)timer;
    your_timer_destroy(myTimer->timer);
    free(myTimer);
}

static void myAdapterTimerReset(kraftAdapter *adapter, kraftTimer *timer) {
    MyTimer *myTimer = (MyTimer *)timer;
    if (timer->active) {
        your_timer_reset(myTimer->timer);
    }
}
```

### Utility Operations

```c
static uint64_t myAdapterNow(kraftAdapter *adapter) {
    (void)adapter;
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + (uint64_t)tv.tv_usec;
}

static bool myAdapterScheduleCallback(kraftAdapter *adapter,
                                       void (*callback)(void *),
                                       void *data) {
    MyAdapterImpl *impl = adapter->impl;
    return your_loop_schedule(impl->loop, callback, data);
}

static uint32_t myAdapterConnectionCount(kraftAdapter *adapter) {
    MyAdapterImpl *impl = adapter->impl;
    return impl->activeConnections;
}

static bool myAdapterGetConnectionInfo(kraftConnection *conn,
                                        char *remoteIp,
                                        size_t ipLen,
                                        uint16_t *remotePort) {
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
```

---

## Step 3: Create Operations Table

```c
static const kraftAdapterOps myAdapterOps = {
    // Lifecycle
    .init = myAdapterInit,
    .destroy = myAdapterDestroy,

    // Server
    .listen = myAdapterListen,
    .stopListening = myAdapterStopListening,  // Optional
    .run = myAdapterRun,
    .stop = myAdapterStop,
    .poll = myAdapterPoll,

    // Connections
    .connect = myAdapterConnect,
    .write = myAdapterWrite,
    .close = myAdapterClose,
    .getConnectionInfo = myAdapterGetConnectionInfo,

    // Timers
    .timerCreate = myAdapterTimerCreate,
    .timerStart = myAdapterTimerStart,
    .timerStop = myAdapterTimerStop,
    .timerDestroy = myAdapterTimerDestroy,
    .timerReset = myAdapterTimerReset,  // Optional

    // Utilities
    .now = myAdapterNow,
    .scheduleCallback = myAdapterScheduleCallback,  // Optional
    .connectionCount = myAdapterConnectionCount,    // Optional
};
```

---

## Step 4: Create Public API

```c
// Header file: myAdapter.h
#ifndef MY_ADAPTER_H
#define MY_ADAPTER_H

#include "kraftAdapter.h"

// Get operations table
const kraftAdapterOps *myAdapterGetOps(void);

// Create adapter with defaults
kraftAdapter *myAdapterCreate(void);

// Create adapter with custom loop
kraftAdapter *myAdapterCreateWithLoop(your_loop_t *loop);

#endif

// Implementation file: myAdapter.c
const kraftAdapterOps *myAdapterGetOps(void) {
    return &myAdapterOps;
}

kraftAdapter *myAdapterCreate(void) {
    return kraftAdapterCreate(&myAdapterOps, NULL);
}

kraftAdapter *myAdapterCreateWithLoop(your_loop_t *loop) {
    kraftAdapter *adapter = kraftAdapterCreate(&myAdapterOps, NULL);
    if (adapter) {
        MyAdapterImpl *impl = adapter->impl;
        impl->loop = loop;
        impl->ownLoop = false;  // Don't destroy external loop
    }
    return adapter;
}
```

---

## Example: io_uring Adapter (Linux)

High-performance adapter using Linux io_uring:

```c
#include <liburing.h>

typedef struct IoUringAdapter {
    struct io_uring ring;
    bool running;
    // ... connection/timer tracking
} IoUringAdapter;

static bool ioUringInit(kraftAdapter *adapter) {
    IoUringAdapter *impl = calloc(1, sizeof(*impl));

    // Initialize io_uring with submission queue size
    int ret = io_uring_queue_init(256, &impl->ring, 0);
    if (ret < 0) {
        free(impl);
        return false;
    }

    adapter->impl = impl;
    return true;
}

static void ioUringRun(kraftAdapter *adapter) {
    IoUringAdapter *impl = adapter->impl;
    impl->running = true;

    while (impl->running) {
        struct io_uring_cqe *cqe;

        // Wait for completion
        io_uring_wait_cqe(&impl->ring, &cqe);

        // Process completion based on user_data
        void *userData = io_uring_cqe_get_data(cqe);
        // ... dispatch to appropriate handler

        io_uring_cqe_seen(&impl->ring, cqe);
    }
}

static bool ioUringWrite(kraftAdapter *adapter,
                          kraftConnection *conn,
                          const void *data,
                          size_t len) {
    IoUringAdapter *impl = adapter->impl;
    IoUringConnection *myConn = (IoUringConnection *)conn;

    // Get submission queue entry
    struct io_uring_sqe *sqe = io_uring_get_sqe(&impl->ring);
    if (!sqe) {
        return false;
    }

    // Prepare write operation
    io_uring_prep_send(sqe, myConn->fd, data, len, 0);
    io_uring_sqe_set_data(sqe, myConn);

    // Submit
    io_uring_submit(&impl->ring);

    return true;
}
```

---

## Testing Your Adapter

### Unit Test Template

```c
#include "myAdapter.h"
#include <assert.h>

void testAdapterLifecycle(void) {
    kraftAdapter *adapter = myAdapterCreate();
    assert(adapter != NULL);
    assert(adapter->impl != NULL);

    kraftAdapterDestroy(adapter);
    printf("Lifecycle test passed\n");
}

void testTimers(void) {
    kraftAdapter *adapter = myAdapterCreate();

    int timerFired = 0;
    adapter->onTimer = [](kraftAdapter *a, kraftTimer *t) {
        (*(int *)t->userData)++;
    };

    kraftTimer *timer = adapter->ops->timerCreate(
        adapter, KRAFT_TIMER_CUSTOM, 100, false, &timerFired);

    adapter->ops->timerStart(adapter, timer, 100);

    // Poll for 200ms
    for (int i = 0; i < 20; i++) {
        adapter->ops->poll(adapter, 10);
    }

    assert(timerFired == 1);
    adapter->ops->timerDestroy(adapter, timer);

    kraftAdapterDestroy(adapter);
    printf("Timer test passed\n");
}

void testConnections(void) {
    // Start a test server
    // Connect with adapter
    // Send data
    // Verify received
    // Close
}

int main(void) {
    testAdapterLifecycle();
    testTimers();
    testConnections();
    printf("All adapter tests passed\n");
    return 0;
}
```

---

## Checklist

Before using your adapter in production:

- [ ] All required operations implemented
- [ ] Connections properly cleaned up on close
- [ ] Timers properly cleaned up on destroy
- [ ] Memory leaks checked (valgrind)
- [ ] Thread safety documented
- [ ] Error handling for all I/O operations
- [ ] Statistics updated correctly
- [ ] Back-pressure handling (flow control)
- [ ] Tested with kraft-test suite
- [ ] Tested with kraft-fuzz

---

## Next Steps

- [Custom Protocols](custom-protocols.md) - Implement wire format encoding
- [Architecture](../architecture.md) - Understand full system design
- [libuv Adapter Source](../../src/kraftAdapterLibuv.c) - Reference implementation
