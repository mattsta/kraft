# Client Development Guide

This guide covers building robust client applications that interact with Kraft clusters.

## Overview

The Kraft Client API provides:

- **Async & sync command submission**
- **Linearizable reads** via ReadIndex
- **Automatic leader discovery** and redirect handling
- **Request batching** for throughput
- **Configurable retries** and timeouts
- **Session tracking** for exactly-once semantics

---

## Client Lifecycle

### 1. Create Components

```c
#include "kraftClientAPI.h"
#include "kraftAdapterLibuv.h"
#include "kraftProtocol.h"

// Create network adapter
kraftAdapter *adapter = kraftAdapterLibuvCreate();

// Create protocol encoder/decoder
kraftProtocol *protocol = kraftProtocolCreate(
    kraftProtocolGetBinaryOps(), NULL);

// Create client with optional custom config
kraftClientConfig config;
kraftClientConfigDefault(&config);
config.requestTimeoutMs = 30000;  // 30 second timeout

kraftClientSession *client = kraftClientCreate(adapter, protocol, &config);
```

### 2. Configure Cluster

```c
// Add all known cluster nodes
// Client will discover the leader automatically
kraftClientAddEndpoint(client, "node1.example.com", 7001);
kraftClientAddEndpoint(client, "node2.example.com", 7001);
kraftClientAddEndpoint(client, "node3.example.com", 7001);

// Or use cluster config structure
kraftClusterConfig cluster = {
    .clusterId = 0x12345678,  // Expected cluster ID (0 = any)
    .endpoints = endpoints,
    .endpointCount = 3
};
kraftClientSetCluster(client, &cluster);
```

### 3. Set Callbacks (Optional)

```c
void onConnect(kraftClientSession *session, bool connected,
               const char *address, uint16_t port,
               const char *errorMsg) {
    if (connected) {
        printf("Connected to %s:%d\n", address, port);
    } else {
        printf("Connection failed: %s\n", errorMsg);
    }
}

void onLeaderChange(kraftClientSession *session, uint64_t newLeaderId,
                    const char *newAddress, uint16_t newPort) {
    printf("New leader: node %llu at %s:%d\n",
           (unsigned long long)newLeaderId, newAddress, newPort);
}

kraftClientSetCallbacks(client, onConnect, onLeaderChange, myUserData);
```

### 4. Connect

```c
if (!kraftClientConnect(client)) {
    fprintf(stderr, "Failed to initiate connection\n");
    exit(1);
}
```

### 5. Event Loop

```c
// Option A: Run dedicated event loop (blocks)
kraftClientRun(client);

// Option B: Poll in your own loop
while (running) {
    int events = kraftClientPoll(client, 100);  // 100ms timeout
    // ... do other work ...
}

// Option C: Integrate with existing event loop
// (Get underlying loop from adapter and add your handles)
```

### 6. Cleanup

```c
kraftClientDisconnect(client);
kraftClientDestroy(client);
kraftProtocolDestroy(protocol);
kraftAdapterDestroy(adapter);
```

---

## Command Submission Patterns

### Pattern 1: Fire-and-Forget

Best for: High-throughput scenarios where you don't need confirmation.

```c
void submitFireAndForget(kraftClientSession *client,
                         const char *command) {
    kraftClientSubmit(client,
                      (uint8_t *)command,
                      strlen(command),
                      NULL,   // No callback
                      NULL);  // No user data
}

// Usage
submitFireAndForget(client, "INCREMENT pageviews");
submitFireAndForget(client, "LOG event:click user:123");
```

### Pattern 2: Async with Callback

Best for: Responsive applications that need to track results.

```c
typedef struct {
    char requestId[64];
    void (*onSuccess)(const char *id, uint64_t logIndex);
    void (*onError)(const char *id, kraftClientStatus status);
} RequestContext;

void onResponse(kraftClientSession *session,
                const kraftClientRequest *request,
                const kraftClientResponse *response,
                void *userData) {
    RequestContext *ctx = (RequestContext *)userData;

    if (response->status == KRAFT_CLIENT_OK) {
        ctx->onSuccess(ctx->requestId, response->logIndex);
    } else {
        ctx->onError(ctx->requestId, response->status);
    }

    free(ctx);
}

void submitAsync(kraftClientSession *client,
                 const char *requestId,
                 const char *command,
                 void (*onSuccess)(const char *, uint64_t),
                 void (*onError)(const char *, kraftClientStatus)) {
    RequestContext *ctx = malloc(sizeof(*ctx));
    strncpy(ctx->requestId, requestId, sizeof(ctx->requestId));
    ctx->onSuccess = onSuccess;
    ctx->onError = onError;

    kraftClientSubmit(client,
                      (uint8_t *)command,
                      strlen(command),
                      onResponse,
                      ctx);
}
```

### Pattern 3: Synchronous (Blocking)

Best for: Simple scripts, tests, or when blocking is acceptable.

```c
bool submitSync(kraftClientSession *client,
                const char *command,
                uint64_t *outLogIndex) {
    kraftClientResponse response;

    kraftClientStatus status = kraftClientSubmitSync(
        client,
        (uint8_t *)command,
        strlen(command),
        &response,
        5000);  // 5 second timeout

    if (status == KRAFT_CLIENT_OK) {
        if (outLogIndex) {
            *outLogIndex = response.logIndex;
        }
        return true;
    }

    if (status == KRAFT_CLIENT_NOT_LEADER) {
        printf("Redirect to leader at %s:%d\n",
               response.leaderAddress, response.leaderPort);
    }

    return false;
}

// Usage
uint64_t logIndex;
if (submitSync(client, "SET user:1 {name:'Alice'}", &logIndex)) {
    printf("Committed at index %llu\n", (unsigned long long)logIndex);
}
```

### Pattern 4: Batched Commands

Best for: Maximum throughput when submitting many commands.

```c
void submitBatch(kraftClientSession *client,
                 const char **commands,
                 size_t count,
                 void (*onBatchComplete)(size_t succeeded, size_t failed)) {

    typedef struct {
        size_t total;
        size_t succeeded;
        size_t failed;
        void (*callback)(size_t, size_t);
    } BatchContext;

    BatchContext *ctx = malloc(sizeof(*ctx));
    ctx->total = count;
    ctx->succeeded = 0;
    ctx->failed = 0;
    ctx->callback = onBatchComplete;

    // Start batching
    kraftClientBatchBegin(client);

    for (size_t i = 0; i < count; i++) {
        kraftClientSubmit(client,
                          (uint8_t *)commands[i],
                          strlen(commands[i]),
                          batchItemCallback,
                          ctx);
    }

    // Send all at once
    kraftClientBatchEnd(client);
}
```

### Pattern 5: With Custom Timeout

```c
kraftClientRequest *submitWithTimeout(kraftClientSession *client,
                                       const char *command,
                                       uint64_t timeoutMs) {
    return kraftClientSubmitEx(client,
                               0,  // command type
                               (uint8_t *)command,
                               strlen(command),
                               timeoutMs,
                               onResponse,
                               NULL);
}

// Critical operation with long timeout
submitWithTimeout(client, "MIGRATE data TO node5", 300000);  // 5 minutes
```

---

## Linearizable Reads

Kraft implements the **ReadIndex** protocol (Raft thesis section 6.4) for linearizable reads without writing to the log.

### Benefits

| Approach               | Writes to Log | Latency | Throughput                 |
| ---------------------- | ------------- | ------- | -------------------------- |
| Read via normal commit | Yes           | High    | Low                        |
| **ReadIndex**          | No            | Low     | **100x higher**            |
| Stale read (follower)  | No            | Lowest  | Highest (not linearizable) |

### Async Read

```c
void onReadComplete(kraftClientSession *session,
                    const kraftClientRequest *request,
                    const kraftClientResponse *response,
                    const uint8_t *value,
                    size_t valueLen,
                    void *userData) {
    if (response->status == KRAFT_CLIENT_OK) {
        printf("Value: %.*s\n", (int)valueLen, (char *)value);
    } else {
        printf("Read failed: %s\n",
               kraftClientStatusString(response->status));
    }
}

void readKey(kraftClientSession *client, const char *key) {
    kraftClientRead(client,
                    (uint8_t *)key,
                    strlen(key),
                    onReadComplete,
                    NULL);
}
```

### Sync Read

```c
bool readKeySync(kraftClientSession *client,
                 const char *key,
                 char *valueOut,
                 size_t valueOutSize) {
    uint8_t *value = NULL;
    size_t valueLen = 0;

    kraftClientStatus status = kraftClientReadSync(
        client,
        (uint8_t *)key,
        strlen(key),
        &value,
        &valueLen,
        5000);  // 5 second timeout

    if (status == KRAFT_CLIENT_OK && value) {
        size_t copyLen = valueLen < valueOutSize - 1 ?
                         valueLen : valueOutSize - 1;
        memcpy(valueOut, value, copyLen);
        valueOut[copyLen] = '\0';
        free(value);
        return true;
    }

    return false;
}

// Usage
char value[1024];
if (readKeySync(client, "user:123:profile", value, sizeof(value))) {
    printf("Profile: %s\n", value);
}
```

---

## Leader Handling

### Automatic Redirects

By default, clients automatically follow leader redirects:

```c
kraftClientConfig config;
kraftClientConfigDefault(&config);
config.followRedirects = true;  // Default: enabled
```

### Manual Leader Discovery

```c
// Get current known leader
uint64_t leaderId;
char address[256];
uint16_t port;

if (kraftClientGetLeader(client, &leaderId, address, sizeof(address), &port)) {
    printf("Leader: node %llu at %s:%d\n",
           (unsigned long long)leaderId, address, port);
} else {
    printf("Leader unknown\n");
}

// Force leader refresh
kraftClientRefreshLeader(client);

// Check if connected to leader
if (kraftClientIsConnectedToLeader(client)) {
    printf("Currently connected to leader\n");
}
```

### Handling NOT_LEADER Responses

```c
void handleResponse(const kraftClientResponse *response) {
    switch (response->status) {
    case KRAFT_CLIENT_OK:
        // Success
        break;

    case KRAFT_CLIENT_NOT_LEADER:
        // Redirect info available
        if (response->leaderNodeId != 0) {
            printf("Try leader at %s:%d\n",
                   response->leaderAddress,
                   response->leaderPort);
        } else {
            printf("Leader unknown, retry later\n");
        }
        break;

    case KRAFT_CLIENT_NO_LEADER:
        // Election in progress
        printf("No leader elected, retry later\n");
        break;

    default:
        printf("Error: %s\n", kraftClientStatusString(response->status));
    }
}
```

---

## Multi-Raft Groups

When using multi-raft, route commands to specific groups:

```c
// Submit to specific group
kraftClientSubmitToGroup(client,
                         groupId,      // Target group
                         (uint8_t *)command,
                         strlen(command),
                         onResponse,
                         NULL);

// Get leader for specific group
uint64_t leaderId;
char address[256];
uint16_t port;

if (kraftClientGetGroupLeader(client, groupId,
                               &leaderId, address, sizeof(address), &port)) {
    printf("Group %u leader at %s:%d\n", groupId, address, port);
}
```

---

## Configuration Reference

```c
typedef struct kraftClientConfig {
    // Timeouts (milliseconds)
    uint64_t connectTimeoutMs;     // Connection establishment (default: 5000)
    uint64_t requestTimeoutMs;     // Per-request timeout (default: 10000)
    uint64_t sessionTimeoutMs;     // Session keepalive (default: 30000)

    // Retry behavior
    uint32_t maxRetries;           // Max retries per request (default: 3)
    uint64_t retryBackoffMs;       // Initial backoff (default: 100)
    uint64_t retryBackoffMaxMs;    // Maximum backoff (default: 5000)
    double retryBackoffMultiplier; // Backoff multiplier (default: 2.0)

    // Connection management
    uint32_t maxPendingRequests;   // Max in-flight requests (default: 256)
    uint32_t connectionPoolSize;   // Connections to maintain (default: 1)
    bool autoReconnect;            // Auto-reconnect on disconnect (default: true)
    bool followRedirects;          // Follow leader redirects (default: true)

    // Request options
    bool enableBatching;           // Enable batching (default: false)
    uint32_t batchMaxSize;         // Max requests per batch (default: 64)
    uint64_t batchDelayMs;         // Max batch delay (default: 1)

    // Session options
    bool enableSession;            // Enable session tracking (default: true)
    uint64_t clientId;             // Client ID (0 = auto-generate)
} kraftClientConfig;
```

### Tuning Profiles

**High Throughput:**

```c
config.enableBatching = true;
config.batchMaxSize = 100;
config.batchDelayMs = 10;
config.maxPendingRequests = 1000;
```

**Low Latency:**

```c
config.enableBatching = false;
config.maxRetries = 1;
config.requestTimeoutMs = 1000;
config.retryBackoffMs = 50;
```

**High Reliability:**

```c
config.maxRetries = 10;
config.retryBackoffMs = 100;
config.retryBackoffMaxMs = 30000;
config.autoReconnect = true;
config.sessionTimeoutMs = 60000;
```

---

## Error Handling

### Status Codes

| Status                      | Description          | Action            |
| --------------------------- | -------------------- | ----------------- |
| `KRAFT_CLIENT_OK`           | Success              | -                 |
| `KRAFT_CLIENT_ERROR`        | Generic error        | Retry or report   |
| `KRAFT_CLIENT_TIMEOUT`      | Request timed out    | Retry             |
| `KRAFT_CLIENT_NOT_LEADER`   | Node is not leader   | Use redirect hint |
| `KRAFT_CLIENT_NO_LEADER`    | No leader elected    | Wait and retry    |
| `KRAFT_CLIENT_DISCONNECTED` | Connection lost      | Reconnect         |
| `KRAFT_CLIENT_REJECTED`     | Request rejected     | Check payload     |
| `KRAFT_CLIENT_INVALID`      | Invalid request      | Fix request       |
| `KRAFT_CLIENT_SHUTDOWN`     | Client shutting down | -                 |

### Retry Strategy

```c
bool submitWithRetry(kraftClientSession *client,
                     const char *command,
                     int maxAttempts) {
    kraftClientResponse response;

    for (int attempt = 1; attempt <= maxAttempts; attempt++) {
        kraftClientStatus status = kraftClientSubmitSync(
            client,
            (uint8_t *)command,
            strlen(command),
            &response,
            5000);

        switch (status) {
        case KRAFT_CLIENT_OK:
            return true;

        case KRAFT_CLIENT_TIMEOUT:
        case KRAFT_CLIENT_DISCONNECTED:
        case KRAFT_CLIENT_NO_LEADER:
            // Retriable - wait and retry
            usleep(100000 * attempt);  // Exponential backoff
            continue;

        case KRAFT_CLIENT_NOT_LEADER:
            // Automatic redirect if enabled
            continue;

        default:
            // Non-retriable
            return false;
        }
    }

    return false;
}
```

---

## Monitoring

### Client Statistics

```c
kraftClientStats stats;
kraftClientGetStats(client, &stats);

printf("Requests sent:     %llu\n", (unsigned long long)stats.requestsSent);
printf("Responses recv:    %llu\n", (unsigned long long)stats.responsesReceived);
printf("Timeouts:          %llu\n", (unsigned long long)stats.timeouts);
printf("Retries:           %llu\n", (unsigned long long)stats.retries);
printf("Redirects:         %llu\n", (unsigned long long)stats.redirects);
printf("Errors:            %llu\n", (unsigned long long)stats.errors);
printf("Pending requests:  %llu\n", (unsigned long long)stats.pendingRequests);
printf("Avg latency (us):  %llu\n", (unsigned long long)stats.avgLatencyUs);
printf("Connected:         %s\n", stats.connected ? "yes" : "no");
printf("Known leader:      %llu\n", (unsigned long long)stats.knownLeaderId);
```

### Latency Tracking

```c
uint64_t minUs, maxUs, avgUs, p99Us;
kraftClientGetLatencyStats(client, &minUs, &maxUs, &avgUs, &p99Us);

printf("Latency - min: %lluus, max: %lluus, avg: %lluus, p99: %lluus\n",
       (unsigned long long)minUs,
       (unsigned long long)maxUs,
       (unsigned long long)avgUs,
       (unsigned long long)p99Us);
```

---

## Best Practices

### 1. Reuse Client Instances

```c
// Good: Create once, reuse
kraftClientSession *client = createClient();
for (int i = 0; i < 1000000; i++) {
    submitCommand(client, commands[i]);
}

// Bad: Create per request
for (int i = 0; i < 1000000; i++) {
    kraftClientSession *c = createClient();  // Expensive!
    submitCommand(c, commands[i]);
    kraftClientDestroy(c);
}
```

### 2. Handle All Response Statuses

```c
// Don't just check for OK
if (status == KRAFT_CLIENT_OK) { ... }

// Handle all cases
switch (status) {
case KRAFT_CLIENT_OK: ...
case KRAFT_CLIENT_NOT_LEADER: ...
case KRAFT_CLIENT_TIMEOUT: ...
// etc.
}
```

### 3. Use Batching for Bulk Operations

```c
// Good: Batch multiple commands
kraftClientBatchBegin(client);
for (int i = 0; i < 1000; i++) {
    kraftClientSubmit(client, ...);
}
kraftClientBatchEnd(client);

// Less efficient: Individual submits
for (int i = 0; i < 1000; i++) {
    kraftClientSubmitSync(client, ...);
}
```

### 4. Set Appropriate Timeouts

```c
// Interactive operations: short timeout
config.requestTimeoutMs = 5000;

// Bulk operations: longer timeout
submitWithTimeout(client, bulkCommand, 60000);

// Critical operations: very long timeout with retries
config.maxRetries = 10;
config.requestTimeoutMs = 30000;
```

---

## Next Steps

- [Server Development](server-development.md) - Build Kraft cluster nodes
- [Multi-Raft Guide](../concepts/multi-raft.md) - Scale with multiple groups
- [API Reference](../reference/api.md) - Complete API documentation
