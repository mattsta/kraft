# ReadIndex Protocol

The ReadIndex protocol enables linearizable reads without writing to the log, significantly improving read performance.

## Overview

```
┌─────────────────────────────────────────────────────────────────┐
│              Traditional Log Read vs ReadIndex                   │
│                                                                  │
│  Log Read (write path):                                         │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │ Client → Write to log → Replicate → Commit → Apply → Read │ │
│  │          ~10-50ms total                                    │ │
│  └────────────────────────────────────────────────────────────┘ │
│                                                                  │
│  ReadIndex (read path):                                         │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │ Client → Confirm leadership → Wait for apply → Read        │ │
│  │          ~1-5ms total                                      │ │
│  └────────────────────────────────────────────────────────────┘ │
│                                                                  │
│  5-10x faster reads with same consistency guarantee!            │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## How It Works

### Protocol Flow

```
Client                    Leader                    Followers
   │                         │                          │
   │  ReadIndex(ctx)         │                          │
   ├────────────────────────►│                          │
   │                         │                          │
   │                         │  readIndex = commitIndex │
   │                         │                          │
   │                         │  Heartbeat               │
   │                         ├─────────────────────────►│
   │                         │                          │
   │                         │◄─────────────────────────┤ Ack
   │                         │  (majority confirms)     │
   │                         │                          │
   │  ReadIndexResp(index)   │                          │
   │◄────────────────────────┤                          │
   │                         │                          │
   │  wait(applied >= index) │                          │
   │  ─────────────────────  │                          │
   │                         │                          │
   │  read from state machine│                          │
   │  ─────────────────────  │                          │
```

### Why Heartbeat Confirmation?

```
┌─────────────────────────────────────────────────────────────────┐
│              Stale Leader Problem                                │
│                                                                  │
│  Without confirmation:                                          │
│                                                                  │
│  Node A (old leader)        Node B              Node C          │
│  ┌─────────────────┐       (new leader)                         │
│  │ Believes it's   │       ┌──────────┐                         │
│  │ still leader    │       │ Actually │                         │
│  │                 │       │ leader   │                         │
│  │ Serves stale    │       │          │                         │
│  │ read!           │       │          │                         │
│  └─────────────────┘       └──────────┘                         │
│                                                                  │
│  With confirmation:                                             │
│                                                                  │
│  Node A                    Node B              Node C           │
│  ┌─────────────────┐       ┌──────────┐       ┌──────────┐     │
│  │ Sends heartbeat │──────►│ Rejects  │       │          │     │
│  │                 │       │ (higher  │       │          │     │
│  │ Discovers it's  │◄──────│  term)   │       │          │     │
│  │ not leader!     │       │          │       │          │     │
│  └─────────────────┘       └──────────┘       └──────────┘     │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Configuration

### Enabling ReadIndex

```c
// Enable ReadIndex on state
kraftStateEnableReadIndex(state, true);

// Configure options
kraftReadIndexConfig config = {
    .enabled = true,
    .timeout = 5000,              // 5 second timeout
    .batchSize = 100,             // Batch up to 100 reads
    .batchTimeout = 5,            // Max 5ms batching delay
    .useLease = false,            // Use heartbeat confirmation
};

kraftStateConfigureReadIndex(state, &config);
```

### Lease-Based Reads

For even lower latency when clocks are synchronized:

```c
kraftReadIndexConfig leaseConfig = {
    .enabled = true,
    .useLease = true,
    .leaseTimeout = 150,          // Lease valid for 150ms
    .clockDrift = 10,             // Assume 10ms max clock drift
};

kraftStateConfigureReadIndex(state, &leaseConfig);
```

```
With Lease:
┌─────────────────────────────────────────────────────────────────┐
│                                                                  │
│  Leader receives heartbeat acks at time T                       │
│  Lease valid until T + leaseTimeout - clockDrift                │
│                                                                  │
│  During lease: serve reads immediately (no confirmation)        │
│  Lease expired: fall back to heartbeat confirmation             │
│                                                                  │
│  Trade-off: Requires reasonably synchronized clocks             │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Client API

### Basic Read

```c
// Synchronous linearizable read
void *data;
size_t len;
kraftClientReadSync(client, &data, &len);

// With callback
void onRead(kraftClientSession *session,
            kraftClientReadResponse *resp,
            void *userData) {
    if (resp->status == KRAFT_CLIENT_OK) {
        processData(resp->data, resp->dataLen);
    }
}

kraftClientRead(client, onRead, userData);
```

### Read with Context

```c
// Include application context in read request
typedef struct {
    char *key;
    size_t keyLen;
    void *userData;
} ReadContext;

ReadContext ctx = {
    .key = "user:123",
    .keyLen = 8,
    .userData = myData,
};

kraftClientReadWithContext(client, &ctx, onRead, userData);
```

### Batched Reads

```c
// Batch multiple reads for efficiency
kraftClientBatchBegin(client);

kraftClientRead(client, onRead1, userData1);
kraftClientRead(client, onRead2, userData2);
kraftClientRead(client, onRead3, userData3);

kraftClientBatchEnd(client);  // Sends as single request
```

---

## Read Consistency Levels

### Linearizable (ReadIndex)

```c
// Strongest guarantee
// Sees all previously committed writes
kraftClientReadOptions opts = {
    .consistency = KRAFT_READ_LINEARIZABLE,
};
kraftClientReadWithOptions(client, &opts, callback, userData);
```

### Serializable

```c
// Read from leader without heartbeat confirmation
// May miss very recent writes (within heartbeat interval)
kraftClientReadOptions opts = {
    .consistency = KRAFT_READ_SERIALIZABLE,
};
```

### Bounded Staleness

```c
// Accept data up to N milliseconds old
kraftClientReadOptions opts = {
    .consistency = KRAFT_READ_BOUNDED_STALENESS,
    .maxStaleness = 1000,  // 1 second max
};
```

### Stale

```c
// Read from any node, may be arbitrarily old
kraftClientReadOptions opts = {
    .consistency = KRAFT_READ_STALE,
};
```

---

## Server Implementation

### ReadIndex State

```c
typedef struct {
    uint64_t index;               // The commit index when request received
    uint64_t requestTime;         // When request was received
    uint32_t pendingConfirms;     // Heartbeat acks needed
    kraftReadCallback callback;
    void *userData;
} PendingRead;

typedef struct {
    PendingRead *reads;
    size_t count;
    size_t capacity;
    uint64_t nextId;
} ReadIndexState;
```

### Processing ReadIndex

```c
void handleReadIndex(kraftState *state, kraftReadIndexRequest *req) {
    // Not leader? Redirect
    if (!kraftStateIsLeader(state)) {
        sendRedirect(req, state->leader);
        return;
    }

    // Record read at current commit index
    PendingRead *read = addPendingRead(state);
    read->index = state->commitIndex;
    read->requestTime = now();
    read->pendingConfirms = (state->clusterSize / 2);  // Majority
    read->callback = req->callback;
    read->userData = req->userData;

    // If lease is valid, respond immediately
    if (state->readIndex.useLease && isLeaseValid(state)) {
        confirmRead(state, read);
        return;
    }

    // Send heartbeat to confirm leadership
    broadcastHeartbeat(state);
}

void handleHeartbeatResponse(kraftState *state, uint64_t nodeId) {
    // Count ack for all pending reads
    for (size_t i = 0; i < state->readIndex.count; i++) {
        PendingRead *read = &state->readIndex.reads[i];
        read->pendingConfirms--;

        if (read->pendingConfirms == 0) {
            confirmRead(state, read);
        }
    }
}

void confirmRead(kraftState *state, PendingRead *read) {
    // Wait for apply to catch up to recorded index
    if (state->lastApplied >= read->index) {
        // Can respond immediately
        sendReadIndexResponse(read);
    } else {
        // Queue until applied
        queueUntilApplied(state, read);
    }
}
```

### Apply Notification

```c
void onApply(kraftState *state, uint64_t appliedIndex) {
    // Check for pending reads that can now be satisfied
    for (size_t i = 0; i < state->readIndex.queuedCount; i++) {
        PendingRead *read = &state->readIndex.queued[i];
        if (appliedIndex >= read->index) {
            sendReadIndexResponse(read);
            removePendingRead(state, i);
            i--;
        }
    }
}
```

---

## Optimizations

### Read Batching

```c
// Batch multiple reads into single heartbeat round-trip
typedef struct {
    PendingRead *reads;
    size_t count;
    uint64_t batchIndex;      // Common read index for batch
} ReadBatch;

void batchReads(kraftState *state) {
    if (state->readIndex.pendingCount == 0) return;
    if (!shouldBatch(state)) return;

    ReadBatch *batch = createBatch(state);
    batch->batchIndex = state->commitIndex;

    // All reads in batch use same index
    for (size_t i = 0; i < batch->count; i++) {
        batch->reads[i].index = batch->batchIndex;
    }

    // Single heartbeat for entire batch
    broadcastHeartbeat(state);
}
```

### Lease Renewal

```c
// Proactively renew lease before expiry
void renewLease(kraftState *state) {
    if (!kraftStateIsLeader(state)) return;

    uint64_t leaseRemaining = state->leaseExpiry - now();
    uint64_t renewThreshold = state->config.leaseTimeout / 3;

    if (leaseRemaining < renewThreshold) {
        // Send heartbeat to extend lease
        broadcastHeartbeat(state);
    }
}

void handleHeartbeatAcks(kraftState *state) {
    if (receivedMajorityAcks()) {
        // Extend lease
        state->leaseExpiry = now() + state->config.leaseTimeout -
                             state->config.clockDrift;
    }
}
```

### Follower Reads

```c
// Allow reads from followers for lower latency
typedef struct {
    kraftState *state;
    uint64_t safeReadIndex;   // Last known safe read index
    uint64_t updateTime;      // When safeReadIndex was updated
} FollowerReadState;

// Leader includes safe read index in heartbeats
void sendHeartbeatWithReadIndex(kraftState *state) {
    AppendEntriesArgs args = {
        // ... normal fields ...
        .readIndex = state->commitIndex,  // Safe to read up to here
    };
    broadcast(&args);
}

// Follower can serve reads up to safeReadIndex
void handleFollowerRead(FollowerReadState *frs, ReadRequest *req) {
    // Check staleness
    uint64_t age = now() - frs->updateTime;
    if (age > req->maxStaleness) {
        rejectRead(req, "data too stale");
        return;
    }

    // Wait for apply
    if (frs->state->lastApplied < frs->safeReadIndex) {
        queueRead(req, frs->safeReadIndex);
        return;
    }

    // Serve read
    serveRead(req);
}
```

---

## Monitoring

### ReadIndex Metrics

```c
typedef struct {
    // Latency
    histogram *readLatency;           // Total read time
    histogram *confirmLatency;        // Time to confirm leadership
    histogram *applyWaitLatency;      // Time waiting for apply

    // Counts
    uint64_t readsTotal;
    uint64_t readsFromLease;          // Reads served from lease
    uint64_t readsFromConfirm;        // Reads requiring confirmation
    uint64_t readsTimedOut;

    // Batching
    histogram *batchSize;
} ReadIndexMetrics;

void exportReadIndexMetrics(kraftState *state) {
    ReadIndexMetrics *m = &state->readIndexMetrics;

    exportHistogram("readindex_latency_ms", m->readLatency);
    exportHistogram("readindex_confirm_latency_ms", m->confirmLatency);
    exportCounter("readindex_total", m->readsTotal);
    exportCounter("readindex_from_lease", m->readsFromLease);
    exportCounter("readindex_timed_out", m->readsTimedOut);
}
```

### Health Indicators

```c
void checkReadIndexHealth(kraftState *state) {
    ReadIndexMetrics *m = &state->readIndexMetrics;

    // Check for high timeout rate
    float timeoutRate = (float)m->readsTimedOut / m->readsTotal;
    if (timeoutRate > 0.01) {  // > 1% timeout
        alertHighReadTimeouts(timeoutRate);
    }

    // Check apply lag
    int64_t applyLag = state->commitIndex - state->lastApplied;
    if (applyLag > 100) {
        alertApplyLag(applyLag);
    }

    // Check lease health (if enabled)
    if (state->config.useLease) {
        float leaseHitRate = (float)m->readsFromLease / m->readsTotal;
        if (leaseHitRate < 0.9) {  // < 90% lease hits
            alertLowLeaseHitRate(leaseHitRate);
        }
    }
}
```

---

## Error Handling

### Timeout

```c
void handleReadTimeout(kraftState *state, PendingRead *read) {
    // Respond with timeout error
    kraftReadIndexResponse resp = {
        .status = KRAFT_READ_TIMEOUT,
        .message = "leadership confirmation timed out",
    };
    read->callback(&resp, read->userData);

    // Remove from pending
    removePendingRead(state, read);
}
```

### Leadership Loss

```c
void onLeadershipLost(kraftState *state) {
    // Fail all pending reads
    for (size_t i = 0; i < state->readIndex.count; i++) {
        PendingRead *read = &state->readIndex.reads[i];

        kraftReadIndexResponse resp = {
            .status = KRAFT_READ_NOT_LEADER,
            .newLeader = state->leader,
        };
        read->callback(&resp, read->userData);
    }

    clearPendingReads(state);
}
```

---

## Best Practices

### When to Use ReadIndex

```
┌─────────────────────────────────────────────────────────────────┐
│                   ReadIndex Decision Tree                        │
│                                                                  │
│  Need linearizable reads?                                       │
│  ├─ Yes, high volume → Use ReadIndex                            │
│  ├─ Yes, low volume → ReadIndex or Log Read both fine           │
│  └─ No → Use stale/bounded reads (even faster)                  │
│                                                                  │
│  Have synchronized clocks?                                      │
│  ├─ Yes, low drift (<10ms) → Enable lease mode                  │
│  └─ No/uncertain → Use heartbeat confirmation                   │
│                                                                  │
│  Read patterns:                                                 │
│  ├─ Many small reads → Enable batching                          │
│  └─ Few large reads → Batching less important                   │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Configuration Recommendations

```c
// High-throughput, clock-synchronized environment
kraftReadIndexConfig highThroughput = {
    .enabled = true,
    .useLease = true,
    .leaseTimeout = 150,
    .clockDrift = 5,
    .batchSize = 500,
    .batchTimeout = 2,
};

// Conservative, any environment
kraftReadIndexConfig conservative = {
    .enabled = true,
    .useLease = false,
    .timeout = 10000,
    .batchSize = 50,
    .batchTimeout = 10,
};
```

---

## Next Steps

- [Leadership Transfer](leadership-transfer.md) - Graceful leader changes
- [Witness Nodes](witnesses.md) - Read scalability
- [Async Commit](async-commit.md) - Durability trade-offs
