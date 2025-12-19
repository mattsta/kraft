# Snapshots & Compaction

Snapshots enable log compaction and fast recovery by capturing point-in-time state machine state.

## Overview

```
Without Snapshots:                    With Snapshots:
┌─────────────────────────────┐      ┌─────────────────────────────┐
│ Log grows forever           │      │ Snapshot + Recent Log       │
│                             │      │                             │
│ [1][2][3][4][5][6][7][8][9] │      │ [Snapshot @ 6] [7][8][9]   │
│                             │      │                             │
│ • Slow recovery             │      │ • Fast recovery             │
│ • Unbounded disk usage      │      │ • Bounded disk usage        │
│ • Memory pressure           │      │ • Memory efficient          │
└─────────────────────────────┘      └─────────────────────────────┘
```

---

## Snapshot Lifecycle

### Creation

```
┌─────────────────────────────────────────────────────────────────┐
│                    Snapshot Creation                             │
│                                                                  │
│  1. Trigger Condition Met                                        │
│     • Log entries > threshold                                    │
│     • Time since last snapshot > interval                        │
│     • Manual trigger                                             │
│                                                                  │
│  2. Capture State                                                │
│     ┌──────────────────────────────────────────────────────┐    │
│     │            State Machine                              │    │
│     │  ┌─────────────────────────────────────────────┐     │    │
│     │  │ key1 → value1                               │     │    │
│     │  │ key2 → value2                               │     │    │
│     │  │ key3 → value3                               │     │    │
│     │  │ ...                                         │     │    │
│     │  └─────────────────────────────────────────────┘     │    │
│     │                                                       │    │
│     │  lastAppliedIndex = 1000                             │    │
│     │  lastAppliedTerm = 5                                 │    │
│     └──────────────────────────────────────────────────────┘    │
│                         │                                        │
│                         ▼                                        │
│  3. Write Snapshot File                                          │
│     ┌──────────────────────────────────────────────────────┐    │
│     │ snapshot-1000-5.dat                                   │    │
│     │ ┌───────────────────────────────────────────────┐    │    │
│     │ │ Header: index=1000, term=5, size=...          │    │    │
│     │ │ Data: [compressed state machine data]         │    │    │
│     │ │ Checksum: SHA1(data)                          │    │    │
│     │ └───────────────────────────────────────────────┘    │    │
│     └──────────────────────────────────────────────────────┘    │
│                                                                  │
│  4. Truncate Log                                                 │
│     Before: [1][2][3]...[998][999][1000][1001][1002]            │
│     After:  [Snapshot @ 1000]      [1001][1002]                 │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Restoration

```c
// On node startup
kraftState *state = kraftStateRecover(&storageConfig);

if (state->snapshot.exists) {
    // 1. Load snapshot
    kraftSnapshotReader *reader = kraftSnapshotOpen(state);

    // 2. Restore state machine
    callbacks.onRestore(state, reader, userData);

    // 3. Set Raft indices
    state->lastApplied = state->snapshot.lastIncludedIndex;
    state->commitIndex = state->snapshot.lastIncludedIndex;

    kraftSnapshotClose(reader);

    // 4. Replay log entries after snapshot
    for (uint64_t i = state->lastApplied + 1; i <= lastLogIndex; i++) {
        kraftLogEntry *entry = kraftLogGet(state->log, i);
        callbacks.apply(entry->data, entry->dataLen);
        state->lastApplied = i;
    }
}
```

---

## Configuration

### Basic Configuration

```c
kraftSnapshotConfig config = {
    // Trigger thresholds
    .triggerThreshold = 10000,       // Entries since last snapshot
    .minInterval = 60 * 1000,        // Minimum 60s between snapshots

    // Size limits
    .maxSize = 1024 * 1024 * 1024,   // 1GB max snapshot

    // Compression
    .compressionEnabled = true,
    .compressionLevel = 6,           // 1-9, higher = better compression

    // Chunking for transfer
    .chunkSize = 1024 * 1024,        // 1MB chunks

    // Callbacks
    .onCreate = onSnapshotCreate,
    .onRestore = onSnapshotRestore,
    .userData = &myStateMachine,
};

kraftStateConfigureSnapshots(state, &config);
```

### Advanced Options

```c
kraftSnapshotConfig advanced = {
    // ... basic options ...

    // Incremental snapshots
    .incrementalEnabled = true,
    .incrementalBase = "snapshot-previous.dat",

    // Concurrent I/O
    .asyncWrite = true,
    .writeBufferSize = 4 * 1024 * 1024,  // 4MB buffer

    // Verification
    .checksumAlgorithm = KRAFT_CHECKSUM_SHA256,
    .verifyOnLoad = true,

    // Retention
    .retainCount = 3,                // Keep last 3 snapshots
    .retainDays = 7,                 // Or last 7 days
};
```

---

## Implementation Guide

### Snapshot Writer

```c
typedef struct {
    hashTable *data;
    uint64_t version;
} MyStateMachine;

void onSnapshotCreate(kraftState *state, kraftSnapshotWriter *writer,
                      void *userData) {
    MyStateMachine *sm = userData;

    // Write header/metadata
    uint64_t version = sm->version;
    kraftSnapshotWrite(writer, &version, sizeof(version));

    // Write entry count
    uint64_t count = hashTableSize(sm->data);
    kraftSnapshotWrite(writer, &count, sizeof(count));

    // Write all entries
    hashTableIterator it;
    hashTableIteratorInit(sm->data, &it);

    while (hashTableIteratorNext(&it)) {
        // Write key
        uint32_t keyLen = strlen(it.key);
        kraftSnapshotWrite(writer, &keyLen, sizeof(keyLen));
        kraftSnapshotWrite(writer, it.key, keyLen);

        // Write value
        MyValue *val = it.value;
        uint32_t valLen = val->len;
        kraftSnapshotWrite(writer, &valLen, sizeof(valLen));
        kraftSnapshotWrite(writer, val->data, valLen);
    }

    // Finalize
    kraftSnapshotComplete(writer, state->lastApplied);
}
```

### Snapshot Reader

```c
void onSnapshotRestore(kraftState *state, kraftSnapshotReader *reader,
                       void *userData) {
    MyStateMachine *sm = userData;

    // Clear existing state
    hashTableClear(sm->data);

    // Read header
    kraftSnapshotRead(reader, &sm->version, sizeof(sm->version));

    // Read entry count
    uint64_t count;
    kraftSnapshotRead(reader, &count, sizeof(count));

    // Read entries
    for (uint64_t i = 0; i < count; i++) {
        // Read key
        uint32_t keyLen;
        kraftSnapshotRead(reader, &keyLen, sizeof(keyLen));
        char *key = malloc(keyLen + 1);
        kraftSnapshotRead(reader, key, keyLen);
        key[keyLen] = '\0';

        // Read value
        uint32_t valLen;
        kraftSnapshotRead(reader, &valLen, sizeof(valLen));
        MyValue *val = myValueCreate(valLen);
        kraftSnapshotRead(reader, val->data, valLen);
        val->len = valLen;

        // Store
        hashTableSet(sm->data, key, val);
        free(key);
    }
}
```

---

## Snapshot Transfer (InstallSnapshot)

When a follower is too far behind, the leader sends its snapshot:

```
Leader                                       Follower
   │                                             │
   │  (Follower's nextIndex < firstLogIndex)     │
   │                                             │
   │  InstallSnapshot(chunk 0)                   │
   ├────────────────────────────────────────────►│
   │                                             │ Save chunk
   │                         InstallSnapshotResp │
   │◄────────────────────────────────────────────┤
   │                                             │
   │  InstallSnapshot(chunk 1)                   │
   ├────────────────────────────────────────────►│
   │                                             │ Save chunk
   │                         InstallSnapshotResp │
   │◄────────────────────────────────────────────┤
   │                                             │
   │  ... more chunks ...                        │
   │                                             │
   │  InstallSnapshot(chunk N, done=true)        │
   ├────────────────────────────────────────────►│
   │                                             │ Complete snapshot
   │                                             │ Restore state machine
   │                         InstallSnapshotResp │
   │◄────────────────────────────────────────────┤
   │                                             │
   │  AppendEntries(index > snapshot)            │
   ├────────────────────────────────────────────►│ Resume normal
   │                                             │ replication
```

### InstallSnapshot RPC

```c
typedef struct {
    uint64_t term;              // Leader's term
    uint64_t leaderId;          // For redirect
    uint64_t lastIncludedIndex; // Snapshot replaces all entries up through
    uint64_t lastIncludedTerm;  // Term of lastIncludedIndex

    uint64_t offset;            // Byte offset in snapshot file
    uint8_t *data;              // Raw snapshot chunk
    uint32_t dataLen;           // Chunk size
    bool done;                  // True if last chunk
} kraftInstallSnapshotArgs;

typedef struct {
    uint64_t term;              // Current term
    uint64_t bytesStored;       // Total bytes received so far
} kraftInstallSnapshotResponse;
```

---

## CRDT-Aware Compaction

For CRDT data types, compaction can merge multiple operations:

```
┌─────────────────────────────────────────────────────────────────┐
│                   CRDT-Aware Compaction                          │
│                                                                  │
│  Before Compaction (G-Counter):                                 │
│  ┌─────┬─────┬─────┬─────┬─────┐                               │
│  │ +1  │ +3  │ +2  │ +1  │ +5  │  counter:hits                 │
│  └─────┴─────┴─────┴─────┴─────┘                               │
│                                                                  │
│  After Compaction:                                              │
│  ┌──────────┐                                                   │
│  │ SET 12   │  counter:hits (1+3+2+1+5 = 12)                   │
│  └──────────┘                                                   │
│                                                                  │
│  Before Compaction (LWW-Register):                              │
│  ┌─────────────┬─────────────┬─────────────┐                   │
│  │ SET@t1:A    │ SET@t2:B    │ SET@t3:C    │  user:name       │
│  └─────────────┴─────────────┴─────────────┘                   │
│                                                                  │
│  After Compaction:                                              │
│  ┌─────────────┐                                                │
│  │ SET@t3:C    │  user:name (latest wins)                      │
│  └─────────────┘                                                │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Configuration

```c
typedef struct {
    char *keyPattern;           // Glob pattern for keys
    kraftCRDTType type;         // CRDT type
} kraftCRDTMapping;

kraftCRDTConfig crdtConfig = {
    .mappings = (kraftCRDTMapping[]){
        {"counter:*", KRAFT_CRDT_GCOUNTER},
        {"gauge:*", KRAFT_CRDT_PNCOUNTER},
        {"profile:*", KRAFT_CRDT_LWW_REGISTER},
        {"tags:*", KRAFT_CRDT_GSET},
        {"cart:*", KRAFT_CRDT_ORSET},
    },
    .mappingCount = 5,
};

kraftStateCRDTConfigure(state, &crdtConfig);
```

---

## Performance Optimization

### Incremental Snapshots

```c
// Track changes since last snapshot
typedef struct {
    hashTable *changes;        // Keys modified since last snapshot
    uint64_t baseSnapshotIndex;
} IncrementalTracker;

void onSnapshotCreateIncremental(kraftState *state, kraftSnapshotWriter *writer,
                                  void *userData) {
    IncrementalTracker *tracker = userData;

    // Write base reference
    kraftSnapshotWrite(writer, &tracker->baseSnapshotIndex, sizeof(uint64_t));

    // Write only changed keys
    uint64_t changeCount = hashTableSize(tracker->changes);
    kraftSnapshotWrite(writer, &changeCount, sizeof(changeCount));

    hashTableIterator it;
    hashTableIteratorInit(tracker->changes, &it);
    while (hashTableIteratorNext(&it)) {
        writeKeyValue(writer, it.key, getStateMachineValue(it.key));
    }

    // Clear change tracker
    hashTableClear(tracker->changes);
    tracker->baseSnapshotIndex = state->lastApplied;
}
```

### Concurrent Snapshot

```c
// Take snapshot without blocking applies
void concurrentSnapshot(kraftState *state, MyStateMachine *sm) {
    // 1. Acquire read lock / COW snapshot
    sm->snapshotting = true;
    MyStateMachine *snapshot = copyOnWriteSnapshot(sm);

    // 2. Continue applying new commands to sm
    // 3. Write snapshot in background

    pthread_t snapshotThread;
    pthread_create(&snapshotThread, NULL, writeSnapshotAsync, snapshot);

    // 4. When complete, release COW resources
}
```

### Streaming Large Snapshots

```c
// For very large state machines, stream directly to disk
void streamingSnapshot(kraftState *state, kraftSnapshotWriter *writer) {
    // Create pipeline: state machine → compress → write
    CompressionStream *compressor = lz4StreamCreate();

    // Iterate state machine in sorted key order
    Iterator *it = stateMachineIterator(sm);
    while (iteratorValid(it)) {
        // Compress and write in chunks
        uint8_t chunk[4096];
        size_t chunkLen = serializeEntry(it, chunk, sizeof(chunk));

        size_t compressed = lz4StreamCompress(compressor, chunk, chunkLen);
        kraftSnapshotWrite(writer, compressor->output, compressed);

        iteratorNext(it);
    }

    // Flush compressor
    lz4StreamFlush(compressor);
    kraftSnapshotWrite(writer, compressor->output, compressor->outputLen);

    lz4StreamDestroy(compressor);
}
```

---

## Monitoring

### Snapshot Metrics

```c
typedef struct {
    // Timing
    uint64_t lastSnapshotTime;
    uint64_t lastSnapshotDuration;

    // Size
    uint64_t lastSnapshotSize;
    uint64_t lastSnapshotEntries;

    // Transfer
    uint64_t snapshotsInstalled;
    uint64_t snapshotBytesSent;
    uint64_t snapshotBytesReceived;

    // Errors
    uint64_t snapshotErrors;
} SnapshotMetrics;

void exportSnapshotMetrics(kraftState *state) {
    SnapshotMetrics *m = &state->snapshotMetrics;

    exportGauge("kraft_snapshot_last_time", m->lastSnapshotTime);
    exportGauge("kraft_snapshot_last_duration_ms", m->lastSnapshotDuration);
    exportGauge("kraft_snapshot_last_size_bytes", m->lastSnapshotSize);
    exportCounter("kraft_snapshots_installed", m->snapshotsInstalled);
    exportCounter("kraft_snapshot_errors", m->snapshotErrors);
}
```

### Log Size Monitoring

```c
void monitorLogSize(kraftState *state) {
    uint64_t logEntries = state->log.lastIndex - state->snapshot.lastIncludedIndex;
    uint64_t logBytes = kraftLogGetSizeBytes(state->log);

    if (logEntries > state->snapshotConfig.triggerThreshold * 0.8) {
        // Approaching threshold, might want to trigger early
        logWarning("Log approaching snapshot threshold: %llu entries",
                   logEntries);
    }

    exportGauge("kraft_log_entries_since_snapshot", logEntries);
    exportGauge("kraft_log_bytes_since_snapshot", logBytes);
}
```

---

## Best Practices

### Snapshot Size

```
┌─────────────────────────────────────────────────────────────────┐
│                   Snapshot Size Considerations                   │
│                                                                  │
│  Too Small (frequent snapshots):                                │
│  • High I/O overhead                                            │
│  • CPU spent compressing                                        │
│  • Disk write wear                                              │
│                                                                  │
│  Too Large (infrequent snapshots):                              │
│  • Slow recovery                                                │
│  • Large log between snapshots                                  │
│  • Memory pressure during snapshot                              │
│                                                                  │
│  Recommended:                                                    │
│  • Snapshot every 10,000-100,000 entries                        │
│  • Or every 5-15 minutes                                        │
│  • Keep snapshot size < available memory                        │
│  • Keep snapshot time < 2× heartbeat interval                   │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Recovery Speed

```c
// Parallel restore for faster recovery
void parallelRestore(kraftState *state, kraftSnapshotReader *reader) {
    // Read metadata
    SnapshotMeta meta;
    kraftSnapshotRead(reader, &meta, sizeof(meta));

    // Split work across threads
    pthread_t workers[NUM_WORKERS];
    for (int i = 0; i < NUM_WORKERS; i++) {
        WorkerArgs *args = &workerArgs[i];
        args->start = meta.entryCount * i / NUM_WORKERS;
        args->end = meta.entryCount * (i + 1) / NUM_WORKERS;
        args->reader = reader;

        pthread_create(&workers[i], NULL, restoreWorker, args);
    }

    // Wait for completion
    for (int i = 0; i < NUM_WORKERS; i++) {
        pthread_join(workers[i], NULL);
    }
}
```

---

## Next Steps

- [CRDT Support](crdt.md) - CRDT types and compaction
- [Production Deployment](../operations/deployment.md) - Snapshot configuration
- [Monitoring](../operations/monitoring.md) - Snapshot metrics
