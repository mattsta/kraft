# Pluggable Networking

Kraft's pluggable networking architecture separates consensus logic from I/O, enabling deployment across diverse environments.

## Design Philosophy

```
┌─────────────────────────────────────────────────────────────────┐
│                     Traditional Approach                         │
│                                                                  │
│  ┌────────────────────────────────────────────────────────┐     │
│  │              Raft + Networking + Serialization          │     │
│  │                    (Tightly Coupled)                    │     │
│  └────────────────────────────────────────────────────────┘     │
│                                                                  │
│  Problems:                                                       │
│  • Hard to integrate with existing event loops                  │
│  • Can't change wire format without modifying core              │
│  • Testing requires real network                                │
│  • Platform-specific code scattered throughout                  │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│                      Kraft Approach                              │
│                                                                  │
│  ┌──────────┐    ┌──────────┐    ┌──────────┐                  │
│  │  Raft    │◄──►│  Adapter │◄──►│ Protocol │                  │
│  │  Core    │    │  (I/O)   │    │ (Format) │                  │
│  └──────────┘    └──────────┘    └──────────┘                  │
│       │               │               │                         │
│       │               │               │                         │
│       ▼               ▼               ▼                         │
│  Pure state      Event loop      Wire format                    │
│  machine         abstraction     abstraction                    │
│                                                                  │
│  Benefits:                                                       │
│  • Use any event loop (libuv, epoll, kqueue, io_uring)         │
│  • Use any wire format (binary, protobuf, JSON)                │
│  • Easy testing with mock adapters                              │
│  • Clean separation of concerns                                 │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Architecture Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                        Application                               │
└───────────────────────────────┬─────────────────────────────────┘
                                │
                                ▼
┌─────────────────────────────────────────────────────────────────┐
│                      kraftClientAPI                              │
│                                                                  │
│  • Command submission                                            │
│  • Read operations                                               │
│  • Leader discovery                                              │
│  • Session management                                            │
│                                                                  │
└───────────────────────────────┬─────────────────────────────────┘
                                │
            ┌───────────────────┼───────────────────┐
            │                   │                   │
            ▼                   ▼                   ▼
┌───────────────────┐ ┌─────────────────┐ ┌─────────────────────┐
│   kraftAdapter    │ │  kraftProtocol  │ │    kraftState       │
│                   │ │                 │ │                     │
│ ┌───────────────┐ │ │ ┌─────────────┐ │ │ ┌─────────────────┐ │
│ │ Connections   │ │ │ │ Encode      │ │ │ │ Raft Core       │ │
│ │ Timers        │ │ │ │ Decode      │ │ │ │ Log             │ │
│ │ Event Loop    │ │ │ │ Validate    │ │ │ │ State Machine   │ │
│ └───────────────┘ │ │ └─────────────┘ │ │ └─────────────────┘ │
│                   │ │                 │ │                     │
│ Implementations:  │ │ Implementations:│ │                     │
│ • libuv           │ │ • Binary        │ │                     │
│ • epoll           │ │ • Protobuf      │ │                     │
│ • kqueue          │ │ • MessagePack   │ │                     │
│ • io_uring        │ │ • JSON          │ │                     │
│ • Mock (testing)  │ │                 │ │                     │
│                   │ │                 │ │                     │
└───────────────────┘ └─────────────────┘ └─────────────────────┘
```

---

## The Adapter Interface

### Core Abstraction

The adapter provides all I/O and timing operations:

```c
typedef struct kraftAdapterOps {
    // === Lifecycle ===

    // Initialize adapter resources
    bool (*init)(kraftAdapter *adapter);

    // Clean up all resources
    void (*destroy)(kraftAdapter *adapter);

    // === Server Operations ===

    // Start listening for connections
    bool (*listen)(
        kraftAdapter *adapter,
        const char *address,
        uint16_t port,
        kraftConnectionCallback onConnection,
        void *userData
    );

    // Run the event loop (blocking)
    void (*run)(kraftAdapter *adapter);

    // Stop the event loop (thread-safe)
    void (*stop)(kraftAdapter *adapter);

    // === Client Operations ===

    // Establish outbound connection
    kraftConnection *(*connect)(
        kraftAdapter *adapter,
        const char *address,
        uint16_t port,
        kraftConnectCallback onConnect,
        void *userData
    );

    // === Connection Operations ===

    // Send data on connection
    bool (*write)(
        kraftAdapter *adapter,
        kraftConnection *conn,
        const uint8_t *data,
        size_t len,
        kraftWriteCallback onComplete,
        void *userData
    );

    // Set data received callback
    void (*setReadCallback)(
        kraftAdapter *adapter,
        kraftConnection *conn,
        kraftReadCallback onData,
        void *userData
    );

    // Close connection
    void (*close)(
        kraftAdapter *adapter,
        kraftConnection *conn
    );

    // === Timer Operations ===

    // Create a timer
    kraftTimer *(*timerCreate)(
        kraftAdapter *adapter,
        kraftTimerCallback callback,
        void *userData
    );

    // Start timer with timeout
    bool (*timerStart)(
        kraftAdapter *adapter,
        kraftTimer *timer,
        uint64_t timeoutMs,
        bool repeat
    );

    // Stop timer
    void (*timerStop)(
        kraftAdapter *adapter,
        kraftTimer *timer
    );

    // Destroy timer
    void (*timerDestroy)(
        kraftAdapter *adapter,
        kraftTimer *timer
    );

    // === Utilities ===

    // Get current timestamp (milliseconds)
    uint64_t (*now)(kraftAdapter *adapter);

} kraftAdapterOps;
```

### Callbacks

```c
// Connection established (client) or accepted (server)
typedef void (*kraftConnectionCallback)(
    kraftAdapter *adapter,
    kraftConnection *conn,
    void *userData
);

// Connection attempt completed
typedef void (*kraftConnectCallback)(
    kraftAdapter *adapter,
    kraftConnection *conn,
    bool success,
    void *userData
);

// Data received
typedef void (*kraftReadCallback)(
    kraftAdapter *adapter,
    kraftConnection *conn,
    const uint8_t *data,
    size_t len,
    void *userData
);

// Write completed
typedef void (*kraftWriteCallback)(
    kraftAdapter *adapter,
    kraftConnection *conn,
    bool success,
    void *userData
);

// Timer fired
typedef void (*kraftTimerCallback)(
    kraftAdapter *adapter,
    kraftTimer *timer,
    void *userData
);
```

---

## The Protocol Interface

### Core Abstraction

The protocol handles message serialization:

```c
typedef struct kraftProtocolOps {
    // === Encoding ===

    // Encode each message type
    bool (*encodeAppendEntries)(
        kraftProtocol *proto,
        kraftEncodeBuffer *buf,
        const kraftAppendEntriesArgs *args
    );

    bool (*encodeAppendEntriesResponse)(
        kraftProtocol *proto,
        kraftEncodeBuffer *buf,
        const kraftAppendEntriesResponse *resp
    );

    bool (*encodeRequestVote)(
        kraftProtocol *proto,
        kraftEncodeBuffer *buf,
        const kraftRequestVoteArgs *args
    );

    bool (*encodeRequestVoteResponse)(
        kraftProtocol *proto,
        kraftEncodeBuffer *buf,
        const kraftRequestVoteResponse *resp
    );

    // ... other message types

    // === Decoding ===

    // Unified decoder
    kraftDecodeStatus (*decode)(
        kraftProtocol *proto,
        const uint8_t *data,
        size_t len,
        kraftMessage *out,
        size_t *consumed
    );

    // === Lifecycle ===

    void (*destroy)(kraftProtocol *proto);

} kraftProtocolOps;
```

### Decode Status

```c
typedef enum {
    KRAFT_DECODE_OK,         // Complete message decoded
    KRAFT_DECODE_NEED_MORE,  // Partial message, need more bytes
    KRAFT_DECODE_ERROR,      // Invalid data, cannot recover
} kraftDecodeStatus;
```

---

## Data Flow

### Outbound Message

```
Application
    │
    │  kraftClientSubmit(command)
    ▼
kraftClientAPI
    │
    │  Create kraftClientRequestArgs
    ▼
kraftProtocol
    │
    │  encodeClientRequest(args) → bytes
    ▼
kraftAdapter
    │
    │  write(connection, bytes)
    ▼
Network (TCP/UDP/etc.)
```

### Inbound Message

```
Network (TCP/UDP/etc.)
    │
    │  data arrives
    ▼
kraftAdapter
    │
    │  onData(connection, bytes)
    ▼
kraftProtocol
    │
    │  decode(bytes) → kraftMessage
    ▼
kraftClientAPI / kraftServer
    │
    │  Handle message type
    ▼
Application callback
```

---

## Built-in Implementations

### libuv Adapter

Production-ready adapter using libuv:

```c
#include "kraftAdapterLibuv.h"

// Create libuv adapter
kraftAdapter *adapter = kraftAdapterLibuvCreate();

// Optional: Use existing loop
uv_loop_t *loop = uv_default_loop();
kraftAdapter *adapter = kraftAdapterLibuvCreateWithLoop(loop);

// Optional: Configure
kraftAdapterLibuvConfig config = {
    .maxConnections = 1000,
    .readBufferSize = 65536,
    .writeQueueMax = 1024 * 1024,
    .tcpNoDelay = true,
    .tcpKeepAlive = true,
    .keepAliveDelay = 60,
};
kraftAdapterLibuvConfigure(adapter, &config);
```

### Binary Protocol

Default high-performance binary format:

```c
#include "kraftProtocol.h"

// Create binary protocol
kraftProtocol *proto = kraftProtocolCreate(
    kraftProtocolGetBinaryOps(),
    NULL
);
```

### Mock Adapter (Testing)

For unit testing without real I/O:

```c
#include "kraftAdapterMock.h"

// Create mock adapter
kraftAdapter *adapter = kraftAdapterMockCreate();

// Simulate incoming connection
kraftConnection *conn = kraftAdapterMockSimulateConnect(adapter, "peer1");

// Simulate received data
kraftAdapterMockSimulateRead(adapter, conn, data, len);

// Verify sent data
kraftAdapterMockExpectWrite(adapter, expectedData, expectedLen);

// Advance time (for timer testing)
kraftAdapterMockAdvanceTime(adapter, 100);  // 100ms
```

---

## Common Patterns

### Connection Management

```c
typedef struct {
    kraftConnection *conn;
    uint64_t nodeId;
    bool isOutbound;

    // Receive buffer for partial messages
    uint8_t recvBuf[65536];
    size_t recvLen;

    // Send queue
    struct WriteRequest *writeQueue;
} PeerConnection;

void onConnect(kraftAdapter *adapter, kraftConnection *conn,
               bool success, void *userData) {
    PeerConnection *peer = userData;

    if (!success) {
        // Schedule reconnect
        scheduleReconnect(peer);
        return;
    }

    peer->conn = conn;

    // Set up read handler
    adapter->ops.setReadCallback(adapter, conn, onData, peer);

    // Send any queued messages
    flushWriteQueue(peer);
}

void onData(kraftAdapter *adapter, kraftConnection *conn,
            const uint8_t *data, size_t len, void *userData) {
    PeerConnection *peer = userData;

    // Append to receive buffer
    memcpy(peer->recvBuf + peer->recvLen, data, len);
    peer->recvLen += len;

    // Try to decode messages
    while (peer->recvLen > 0) {
        kraftMessage msg;
        size_t consumed;

        kraftDecodeStatus status = proto->ops.decode(
            proto, peer->recvBuf, peer->recvLen, &msg, &consumed);

        if (status == KRAFT_DECODE_NEED_MORE) break;
        if (status == KRAFT_DECODE_ERROR) {
            closeConnection(peer);
            return;
        }

        handleMessage(&msg, peer);

        // Remove consumed bytes
        memmove(peer->recvBuf, peer->recvBuf + consumed,
                peer->recvLen - consumed);
        peer->recvLen -= consumed;
    }
}
```

### Timer Management

```c
typedef struct {
    kraftAdapter *adapter;
    kraftTimer *electionTimer;
    kraftTimer *heartbeatTimer;
} RaftTimers;

void initTimers(RaftTimers *timers, kraftAdapter *adapter) {
    timers->adapter = adapter;

    timers->electionTimer = adapter->ops.timerCreate(
        adapter, onElectionTimeout, timers);

    timers->heartbeatTimer = adapter->ops.timerCreate(
        adapter, onHeartbeatTimeout, timers);
}

void resetElectionTimer(RaftTimers *timers) {
    kraftAdapter *adapter = timers->adapter;

    // Random timeout between 150-300ms
    uint64_t timeout = 150 + (rand() % 150);

    adapter->ops.timerStop(adapter, timers->electionTimer);
    adapter->ops.timerStart(adapter, timers->electionTimer, timeout, false);
}

void startHeartbeat(RaftTimers *timers) {
    kraftAdapter *adapter = timers->adapter;

    // Heartbeat every 50ms
    adapter->ops.timerStart(adapter, timers->heartbeatTimer, 50, true);
}

void onElectionTimeout(kraftAdapter *adapter, kraftTimer *timer, void *userData) {
    RaftTimers *timers = userData;
    startElection();
}

void onHeartbeatTimeout(kraftAdapter *adapter, kraftTimer *timer, void *userData) {
    sendHeartbeats();
}
```

### Graceful Shutdown

```c
typedef struct {
    kraftAdapter *adapter;
    kraftConnection **connections;
    size_t connectionCount;
    bool stopping;
} Server;

void shutdownServer(Server *server) {
    server->stopping = true;

    // Stop accepting new connections
    // (handled by kraftAdapterStop)

    // Close all connections gracefully
    for (size_t i = 0; i < server->connectionCount; i++) {
        if (server->connections[i]) {
            server->adapter->ops.close(
                server->adapter,
                server->connections[i]
            );
        }
    }

    // Stop event loop
    server->adapter->ops.stop(server->adapter);
}
```

---

## Integration Examples

### With Existing Event Loop

```c
// Your application already uses libuv
uv_loop_t *yourLoop = getExistingLoop();

// Create kraft adapter using your loop
kraftAdapter *adapter = kraftAdapterLibuvCreateWithLoop(yourLoop);

// Kraft callbacks integrate with your loop
kraftClientSession *client = kraftClientCreate(adapter, protocol, NULL);
kraftClientConnect(client);

// Run your existing loop (kraft operates within it)
uv_run(yourLoop, UV_RUN_DEFAULT);
```

### With epoll Directly

```c
// Implement minimal epoll adapter
typedef struct {
    kraftAdapterOps ops;
    int epfd;
    bool running;
    struct epoll_event events[MAX_EVENTS];
} EpollAdapter;

void epollRun(kraftAdapter *adapter) {
    EpollAdapter *ea = (EpollAdapter *)adapter;

    while (ea->running) {
        int n = epoll_wait(ea->epfd, ea->events, MAX_EVENTS, 10);

        for (int i = 0; i < n; i++) {
            handleEvent(&ea->events[i]);
        }

        // Process timers
        checkTimers(ea);
    }
}
```

### With io_uring

```c
// High-performance Linux adapter
typedef struct {
    kraftAdapterOps ops;
    struct io_uring ring;
    // ...
} IoUringAdapter;

bool ioUringWrite(kraftAdapter *adapter, kraftConnection *conn,
                  const uint8_t *data, size_t len, ...) {
    IoUringAdapter *ua = (IoUringAdapter *)adapter;

    struct io_uring_sqe *sqe = io_uring_get_sqe(&ua->ring);
    io_uring_prep_send(sqe, conn->fd, data, len, 0);
    io_uring_sqe_set_data(sqe, conn);

    io_uring_submit(&ua->ring);
    return true;
}
```

---

## Testing with Mock Adapter

```c
void testLeaderElection(void) {
    // Create mock adapters for 3 nodes
    kraftAdapter *adapters[3];
    kraftState *states[3];

    for (int i = 0; i < 3; i++) {
        adapters[i] = kraftAdapterMockCreate();
        states[i] = kraftStateNew();
        initNode(states[i], i + 1, 3);
    }

    // Simulate network connectivity
    connectNodes(adapters, 3);

    // Node 1: Trigger election timeout
    kraftAdapterMockAdvanceTime(adapters[0], 200);

    // Verify RequestVote sent
    assertMessageSent(adapters[0], KRAFT_MSG_REQUEST_VOTE);

    // Deliver to other nodes
    deliverMessages(adapters, 3);

    // Advance time on other nodes
    for (int i = 1; i < 3; i++) {
        kraftAdapterMockAdvanceTime(adapters[i], 10);
    }

    // Verify votes granted
    assertMessageSent(adapters[1], KRAFT_MSG_REQUEST_VOTE_RESPONSE);
    assertMessageSent(adapters[2], KRAFT_MSG_REQUEST_VOTE_RESPONSE);

    // Deliver responses
    deliverMessages(adapters, 3);

    // Node 1 should be leader
    assert(kraftStateIsLeader(states[0]));

    // Cleanup
    for (int i = 0; i < 3; i++) {
        kraftStateFree(states[i]);
        kraftAdapterDestroy(adapters[i]);
    }
}
```

---

## Performance Considerations

### Zero-Copy Where Possible

```c
// BAD: Copy on receive
void onData(..., const uint8_t *data, size_t len, ...) {
    uint8_t *copy = malloc(len);
    memcpy(copy, data, len);
    processLater(copy, len);  // Process after callback returns
}

// GOOD: Process immediately
void onData(..., const uint8_t *data, size_t len, ...) {
    // Data is valid only during callback
    processImmediately(data, len);
}

// GOOD: Zero-copy decode
kraftDecodeStatus decode(...) {
    // Point directly into receive buffer
    msg->logEntry.data = (uint8_t *)ptr;  // No copy
    msg->logEntry.dataLen = len;
}
```

### Buffer Pooling

```c
typedef struct {
    uint8_t *buffers[POOL_SIZE];
    bool inUse[POOL_SIZE];
} BufferPool;

uint8_t *poolAlloc(BufferPool *pool, size_t size) {
    for (int i = 0; i < POOL_SIZE; i++) {
        if (!pool->inUse[i]) {
            pool->inUse[i] = true;
            return pool->buffers[i];
        }
    }
    return malloc(size);  // Fallback
}

void poolFree(BufferPool *pool, uint8_t *buf) {
    for (int i = 0; i < POOL_SIZE; i++) {
        if (pool->buffers[i] == buf) {
            pool->inUse[i] = false;
            return;
        }
    }
    free(buf);  // Not from pool
}
```

### Batching

```c
// Batch multiple messages in one write
void flushOutbound(Connection *conn) {
    if (conn->outboundCount == 0) return;

    // Combine all pending messages
    kraftEncodeBuffer combined;
    kraftEncodeBufferInitDynamic(&combined, 4096);

    for (int i = 0; i < conn->outboundCount; i++) {
        kraftEncodeAppend(&combined,
                         conn->outbound[i].data,
                         conn->outbound[i].len);
    }

    // Single write system call
    adapter->ops.write(adapter, conn->conn,
                       combined.data, combined.len, NULL, NULL);

    conn->outboundCount = 0;
    kraftEncodeBufferFree(&combined);
}
```

---

## Next Steps

- [Custom Adapters Guide](../guides/custom-adapters.md) - Implement your own adapter
- [Custom Protocols Guide](../guides/custom-protocols.md) - Implement wire formats
- [Architecture](../architecture.md) - Full system design
