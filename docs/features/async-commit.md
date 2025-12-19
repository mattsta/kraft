# Async Commit Modes

Kraft supports configurable durability levels that trade off between latency and safety.

## Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                   Durability Spectrum                            │
│                                                                  │
│  ◄─────────────────────────────────────────────────────────────►│
│  Faster                                              Safer      │
│                                                                  │
│  ┌──────────┐  ┌──────────┐  ┌──────────┐  ┌──────────────────┐│
│  │ Fire &   │  │ Leader   │  │ Majority │  │ All Nodes +      ││
│  │ Forget   │  │ Ack      │  │ Ack      │  │ Fsync            ││
│  └──────────┘  └──────────┘  └──────────┘  └──────────────────┘│
│       │             │             │               │             │
│       │             │             │               │             │
│  No durability  Leader crash  Quorum crash   No data loss     │
│  guarantee      loses data    loses data     ever              │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Commit Modes

### KRAFT_COMMIT_SYNC (Default)

Standard Raft behavior: wait for majority acknowledgment.

```c
kraftCommitConfig config = {
    .mode = KRAFT_COMMIT_SYNC,
    // No additional options needed
};

kraftStateConfigureCommit(state, &config);
```

```
Client                 Leader                  Follower 1          Follower 2
   │                      │                        │                   │
   │  Submit command      │                        │                   │
   ├─────────────────────►│                        │                   │
   │                      │  AppendEntries         │                   │
   │                      ├───────────────────────►│                   │
   │                      ├────────────────────────────────────────────►│
   │                      │                        │                   │
   │                      │◄───────────────────────┤  Ack              │
   │                      │  (majority reached)    │                   │
   │                      │                        │                   │
   │◄─────────────────────┤  Committed             │                   │
   │                      │                        │                   │
```

**Guarantees**: Committed entries survive any minority of failures.

### KRAFT_COMMIT_LEADER_ONLY

Ack after leader writes to local log, before replication.

```c
kraftCommitConfig config = {
    .mode = KRAFT_COMMIT_LEADER_ONLY,
    .leaderFsync = true,        // Optional: fsync on leader
    .asyncReplicate = true,     // Continue replicating in background
};

kraftStateConfigureCommit(state, &config);
```

```
Client                 Leader                  Followers
   │                      │                        │
   │  Submit command      │                        │
   ├─────────────────────►│                        │
   │                      │  Write to log          │
   │                      │  ────────────          │
   │                      │                        │
   │◄─────────────────────┤  Ack (fast!)           │
   │                      │                        │
   │                      │  AppendEntries (async) │
   │                      ├───────────────────────►│
   │                      │                        │
```

**Trade-off**: ~2x faster, but leader crash before replication loses data.

### KRAFT_COMMIT_ASYNC

Fire and forget: no ack, maximum throughput.

```c
kraftCommitConfig config = {
    .mode = KRAFT_COMMIT_ASYNC,
    .batchSize = 100,           // Batch before replicating
    .batchTimeout = 10,         // Max 10ms batch delay
};

kraftStateConfigureCommit(state, &config);
```

```
Client                 Leader
   │                      │
   │  Submit command      │
   ├─────────────────────►│
   │◄─────────────────────┤  Ack (immediate)
   │                      │
   │  Submit command      │
   ├─────────────────────►│
   │◄─────────────────────┤  Ack (immediate)
   │                      │
   │                      │  Batch + Replicate (background)
   │                      │  ─────────────────────────────
```

**Use case**: High-throughput, loss-tolerant workloads (metrics, logs).

### KRAFT_COMMIT_ALL

Wait for all nodes (stronger than quorum).

```c
kraftCommitConfig config = {
    .mode = KRAFT_COMMIT_ALL,
    .timeout = 5000,            // 5 second timeout
    .fallbackToMajority = true, // Fall back if some nodes slow
};

kraftStateConfigureCommit(state, &config);
```

```
Client                 Leader              Follower 1        Follower 2
   │                      │                    │                 │
   │  Submit              │                    │                 │
   ├─────────────────────►│                    │                 │
   │                      │  AppendEntries     │                 │
   │                      ├───────────────────►│                 │
   │                      ├──────────────────────────────────────►│
   │                      │                    │                 │
   │                      │◄───────────────────┤  Ack            │
   │                      │◄──────────────────────────────────────┤  Ack
   │                      │  (all responded)   │                 │
   │◄─────────────────────┤  Committed         │                 │
```

**Guarantee**: All nodes have the data before ack.

---

## Fsync Modes

Control when data is synced to disk:

```c
typedef enum {
    KRAFT_FSYNC_NONE,       // Never fsync (OS decides)
    KRAFT_FSYNC_BATCH,      // Fsync every N entries or M ms
    KRAFT_FSYNC_COMMIT,     // Fsync on commit (before ack)
    KRAFT_FSYNC_ALWAYS,     // Fsync every write
} kraftFsyncMode;

kraftStorageConfig storageConfig = {
    .fsyncMode = KRAFT_FSYNC_COMMIT,
    .batchFsyncCount = 100,     // For BATCH mode
    .batchFsyncInterval = 100,  // For BATCH mode (ms)
};
```

### Fsync Impact

```
┌─────────────────────────────────────────────────────────────────┐
│                   Fsync Performance Impact                       │
│                                                                  │
│  Mode          │ Latency  │ Throughput │ Durability             │
│  ──────────────┼──────────┼────────────┼──────────────────────  │
│  NONE          │ ~0.1ms   │ 100k/s     │ Power loss = data loss │
│  BATCH         │ ~1-5ms   │ 50k/s      │ Batch loss possible    │
│  COMMIT        │ ~5-10ms  │ 10k/s      │ Committed = durable    │
│  ALWAYS        │ ~10-20ms │ 1k/s       │ All writes durable     │
│                                                                  │
│  Note: Actual numbers depend on storage hardware                │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Configuration Examples

### High Durability (Financial)

```c
kraftCommitConfig commit = {
    .mode = KRAFT_COMMIT_ALL,
    .timeout = 10000,
    .fallbackToMajority = false,  // Fail rather than lose durability
};

kraftStorageConfig storage = {
    .fsyncMode = KRAFT_FSYNC_ALWAYS,
};

kraftStateConfigureCommit(state, &commit);
kraftStateConfigureStorage(state, &storage);
```

### Balanced (General Purpose)

```c
kraftCommitConfig commit = {
    .mode = KRAFT_COMMIT_SYNC,
};

kraftStorageConfig storage = {
    .fsyncMode = KRAFT_FSYNC_COMMIT,
};

kraftStateConfigureCommit(state, &commit);
kraftStateConfigureStorage(state, &storage);
```

### Low Latency (User-Facing)

```c
kraftCommitConfig commit = {
    .mode = KRAFT_COMMIT_LEADER_ONLY,
    .leaderFsync = false,
    .asyncReplicate = true,
};

kraftStorageConfig storage = {
    .fsyncMode = KRAFT_FSYNC_BATCH,
    .batchFsyncCount = 50,
    .batchFsyncInterval = 10,  // 10ms
};

kraftStateConfigureCommit(state, &commit);
kraftStateConfigureStorage(state, &storage);
```

### Maximum Throughput (Metrics/Logs)

```c
kraftCommitConfig commit = {
    .mode = KRAFT_COMMIT_ASYNC,
    .batchSize = 1000,
    .batchTimeout = 50,  // 50ms batches
};

kraftStorageConfig storage = {
    .fsyncMode = KRAFT_FSYNC_BATCH,
    .batchFsyncCount = 1000,
    .batchFsyncInterval = 1000,  // 1 second
};

kraftStateConfigureCommit(state, &commit);
kraftStateConfigureStorage(state, &storage);
```

---

## Per-Request Durability

Override default commit mode per request:

```c
typedef struct {
    uint8_t *data;
    size_t len;
    kraftCommitMode commitMode;     // Override for this request
    kraftCommitCallback callback;
    void *userData;
} kraftSubmitOptions;

// Critical financial transaction
kraftSubmitOptions criticalTx = {
    .data = txData,
    .len = txLen,
    .commitMode = KRAFT_COMMIT_ALL,
    .callback = onCriticalCommit,
};
kraftClientSubmitWithOptions(client, &criticalTx);

// Background analytics event
kraftSubmitOptions analyticsEvent = {
    .data = eventData,
    .len = eventLen,
    .commitMode = KRAFT_COMMIT_ASYNC,
    .callback = NULL,  // Fire and forget
};
kraftClientSubmitWithOptions(client, &analyticsEvent);
```

---

## Durability Zones

Configure different durability for different data types:

```c
typedef struct {
    const char *keyPattern;
    kraftCommitMode mode;
    kraftFsyncMode fsync;
} DurabilityZone;

DurabilityZone zones[] = {
    // Financial data: maximum durability
    {"account:*", KRAFT_COMMIT_ALL, KRAFT_FSYNC_ALWAYS},
    {"transaction:*", KRAFT_COMMIT_ALL, KRAFT_FSYNC_ALWAYS},

    // User data: standard durability
    {"user:*", KRAFT_COMMIT_SYNC, KRAFT_FSYNC_COMMIT},
    {"profile:*", KRAFT_COMMIT_SYNC, KRAFT_FSYNC_COMMIT},

    // Analytics: low durability
    {"metric:*", KRAFT_COMMIT_ASYNC, KRAFT_FSYNC_BATCH},
    {"event:*", KRAFT_COMMIT_ASYNC, KRAFT_FSYNC_NONE},

    // Session: ephemeral
    {"session:*", KRAFT_COMMIT_LEADER_ONLY, KRAFT_FSYNC_NONE},
};

kraftCommitMode getModeForKey(const char *key) {
    for (size_t i = 0; i < ARRAY_SIZE(zones); i++) {
        if (matchPattern(zones[i].keyPattern, key)) {
            return zones[i].mode;
        }
    }
    return KRAFT_COMMIT_SYNC;  // Default
}
```

---

## Failure Scenarios

### Leader Crash with LEADER_ONLY

```
┌─────────────────────────────────────────────────────────────────┐
│                 Data Loss Scenario                               │
│                                                                  │
│  Time 0: Client submits, leader acks                            │
│  Time 1: Leader crashes before replication                      │
│  Time 2: New leader elected                                     │
│  Time 3: Client's "committed" data is gone!                     │
│                                                                  │
│  Leader (crashed)         Follower 1            Follower 2       │
│  ┌─────────────────┐     ┌─────────────────┐   ┌──────────────┐ │
│  │ Entry 1 ✓       │     │ Entry 1 ✓       │   │ Entry 1 ✓    │ │
│  │ Entry 2 ✓       │     │ Entry 2 ✓       │   │ Entry 2 ✓    │ │
│  │ Entry 3 (acked) │     │ (no entry 3)    │   │ (no entry 3) │ │
│  │ CRASH!          │     │ becomes leader  │   │              │ │
│  └─────────────────┘     └─────────────────┘   └──────────────┘ │
│                                                                  │
│  Entry 3 was acknowledged but is now lost                       │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Power Failure with FSYNC_NONE

```
┌─────────────────────────────────────────────────────────────────┐
│               Data Loss on Power Failure                         │
│                                                                  │
│  OS File Buffer          Disk                                    │
│  ┌─────────────────┐     ┌─────────────────┐                    │
│  │ Entry 1000 ✓    │     │ Entry 1-990 ✓   │                    │
│  │ Entry 1001 ✓    │ ──► │                 │                    │
│  │ Entry 1002 ✓    │     │ (not synced)    │                    │
│  └─────────────────┘     └─────────────────┘                    │
│                                                                  │
│  POWER FAILURE                                                   │
│                                                                  │
│  After restart:                                                  │
│  - Entries 991-1002 lost (were in buffer only)                  │
│  - Node has stale log                                           │
│  - May conflict with other nodes                                │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Monitoring

### Durability Metrics

```c
typedef struct {
    // Commit latency by mode
    histogram *syncCommitLatency;
    histogram *asyncCommitLatency;
    histogram *leaderOnlyCommitLatency;

    // Fsync timing
    histogram *fsyncLatency;
    uint64_t fsyncCount;
    uint64_t fsyncBytes;

    // Replication lag for async modes
    uint64_t maxReplicationLag;
    uint64_t avgReplicationLag;

    // Data at risk (uncommitted to quorum)
    uint64_t entriesAtRisk;
    uint64_t bytesAtRisk;
} DurabilityMetrics;

void exportDurabilityMetrics(kraftState *state) {
    DurabilityMetrics *m = &state->durabilityMetrics;

    exportHistogram("commit_latency_ms", m->syncCommitLatency,
                    "mode", "sync");
    exportHistogram("fsync_latency_ms", m->fsyncLatency);

    exportGauge("entries_at_risk", m->entriesAtRisk);
    exportGauge("bytes_at_risk", m->bytesAtRisk);
}
```

### Alerting

```c
void checkDurabilityHealth(kraftState *state) {
    DurabilityMetrics *m = &state->durabilityMetrics;

    // Alert if too much data at risk
    if (m->bytesAtRisk > 100 * 1024 * 1024) {  // 100MB
        alertHighDataAtRisk(m->bytesAtRisk);
    }

    // Alert if replication is lagging
    if (m->maxReplicationLag > 10000) {  // 10 seconds
        alertReplicationLag(m->maxReplicationLag);
    }

    // Alert if fsync is slow
    uint64_t p99 = histogramPercentile(m->fsyncLatency, 0.99);
    if (p99 > 100) {  // 100ms
        alertSlowFsync(p99);
    }
}
```

---

## Best Practices

### Choosing Commit Mode

```
┌─────────────────────────────────────────────────────────────────┐
│                   Decision Matrix                                │
│                                                                  │
│  Use SYNC when:                                                 │
│  • Data loss is unacceptable                                    │
│  • Latency < 10ms is acceptable                                 │
│  • Standard Raft semantics required                             │
│                                                                  │
│  Use LEADER_ONLY when:                                          │
│  • Low latency is critical                                      │
│  • Occasional data loss is acceptable                           │
│  • Leader is on reliable hardware                               │
│                                                                  │
│  Use ASYNC when:                                                │
│  • Maximum throughput needed                                    │
│  • Data is regenerable or loss-tolerant                         │
│  • Ordering is more important than durability                   │
│                                                                  │
│  Use ALL when:                                                  │
│  • Maximum durability required                                  │
│  • All replicas must have data before proceeding                │
│  • Regulatory requirements                                      │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Fsync Recommendations

```c
// SSD storage (typical)
.fsyncMode = KRAFT_FSYNC_COMMIT  // ~5ms latency

// NVMe storage (fast)
.fsyncMode = KRAFT_FSYNC_ALWAYS  // ~1ms latency

// HDD storage (slow)
.fsyncMode = KRAFT_FSYNC_BATCH   // Amortize seek time

// RAM disk or ephemeral
.fsyncMode = KRAFT_FSYNC_NONE    // No persistence needed
```

---

## Next Steps

- [Leadership Transfer](leadership-transfer.md) - Graceful leader handoff
- [ReadIndex](read-index.md) - Linearizable reads
- [Production Deployment](../operations/deployment.md) - Storage configuration
