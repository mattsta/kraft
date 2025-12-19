# Server Development Guide

This guide covers building production Kraft server nodes, from basic setup to advanced cluster management.

## Overview

A Kraft server is a node in a Raft cluster that:

- Participates in leader elections
- Replicates log entries
- Applies committed commands to your state machine
- Handles client requests (if leader)

---

## Basic Server Setup

### Minimal Server

```c
#include "kraft.h"
#include "kraftNetwork.h"
#include "kraftAdapterLibuv.h"

// Your state machine apply function
void applyCommand(const void *data, uint64_t len) {
    // Process the committed command
    printf("Applying: %.*s\n", (int)len, (const char *)data);
}

int main(int argc, char **argv) {
    uint64_t nodeId = 1;
    uint16_t port = 7001;

    // 1. Create Raft state
    kraftState *state = kraftStateNew();

    // 2. Configure cluster
    kraftIp peers[3] = {
        {.ip = "127.0.0.1", .port = 7001, .outbound = true},
        {.ip = "127.0.0.1", .port = 7002, .outbound = true},
        {.ip = "127.0.0.1", .port = 7003, .outbound = true},
    };
    kraftIp *peerPtrs[] = {&peers[0], &peers[1], &peers[2]};

    kraftStateInitRaft(state, 3, peerPtrs, 0x12345678, nodeId);

    // 3. Set apply callback
    state->internals.data.apply = applyCommand;

    // 4. Create network components
    kraftAdapter *adapter = kraftAdapterLibuvCreate();
    kraftProtocol *protocol = kraftProtocolCreate(
        kraftProtocolGetBinaryOps(), NULL);

    // 5. Create and run server
    kraftServer *server = kraftServerCreate(adapter, protocol);
    kraftServerSetState(server, state);
    kraftServerListenOn(server, "0.0.0.0", port);

    printf("Node %llu listening on port %u\n",
           (unsigned long long)nodeId, port);

    kraftServerRun(server);  // Blocks

    // Cleanup
    kraftServerDestroy(server);
    kraftStateFree(state);
    return 0;
}
```

---

## State Machine Integration

### The Apply Callback

The apply callback is invoked for each committed log entry:

```c
typedef void (*kraftApplyCallback)(const void *data, uint64_t len);
```

**Important properties:**

- Called in order (entries applied sequentially by index)
- Called exactly once per committed entry
- Called on the event loop thread
- Must not block (defer heavy work if needed)

### Stateful Apply

For real applications, you need access to your state machine:

```c
typedef struct {
    // Your state machine
    hashTable *keyValue;
    uint64_t lastApplied;

    // Reference to kraft state for reads
    kraftState *raft;
} MyStateMachine;

MyStateMachine *globalSM;  // Or pass via kraft state userData

void applyCommand(const void *data, uint64_t len) {
    // Parse command
    const char *cmd = (const char *)data;

    if (strncmp(cmd, "SET ", 4) == 0) {
        // Parse key and value
        char key[256], value[256];
        sscanf(cmd + 4, "%s %s", key, value);
        hashTableSet(globalSM->keyValue, key, strdup(value));
    } else if (strncmp(cmd, "DEL ", 4) == 0) {
        char key[256];
        sscanf(cmd + 4, "%s", key);
        hashTableDelete(globalSM->keyValue, key);
    }

    globalSM->lastApplied++;
}
```

### Handling Different Command Types

```c
typedef enum {
    CMD_SET = 1,
    CMD_DELETE = 2,
    CMD_INCREMENT = 3,
    CMD_APPEND = 4,
} CommandType;

typedef struct __attribute__((packed)) {
    uint8_t type;
    uint16_t keyLen;
    uint32_t valueLen;
    // Followed by: key bytes, value bytes
} CommandHeader;

void applyCommand(const void *data, uint64_t len) {
    const CommandHeader *hdr = data;
    const char *key = (const char *)(hdr + 1);
    const char *value = key + hdr->keyLen;

    switch (hdr->type) {
        case CMD_SET:
            storeSet(key, hdr->keyLen, value, hdr->valueLen);
            break;
        case CMD_DELETE:
            storeDelete(key, hdr->keyLen);
            break;
        case CMD_INCREMENT:
            storeIncrement(key, hdr->keyLen, *(int64_t *)value);
            break;
        case CMD_APPEND:
            storeAppend(key, hdr->keyLen, value, hdr->valueLen);
            break;
    }
}
```

---

## Server Configuration

### Full Configuration Example

```c
kraftServerConfig config = {
    // Network
    .bindAddress = "0.0.0.0",
    .port = 7001,
    .maxConnections = 1000,
    .connectionTimeout = 30000,  // 30 seconds

    // Raft timing (milliseconds)
    .electionTimeoutMin = 150,
    .electionTimeoutMax = 300,
    .heartbeatInterval = 50,

    // Log compaction
    .snapshotThreshold = 10000,      // Entries before snapshot
    .snapshotChunkSize = 1024 * 1024, // 1MB chunks

    // Performance
    .maxAppendBatchSize = 100,
    .pipelineEnabled = true,
    .pipelineWindow = 10,

    // Features
    .enablePreVote = true,
    .enableReadIndex = true,
    .enableLeadershipTransfer = true,

    // Callbacks
    .onStateChange = onRaftStateChange,
    .onLeaderChange = onLeaderChange,
    .onMembershipChange = onMembershipChange,
};

kraftServer *server = kraftServerCreateWithConfig(adapter, protocol, &config);
```

### Election Timing

Choose election timeouts carefully:

```
┌─────────────────────────────────────────────────────────────────┐
│                    Election Timeout Guidelines                    │
├─────────────────────────────────────────────────────────────────┤
│                                                                   │
│  heartbeatInterval << electionTimeoutMin << electionTimeoutMax  │
│                                                                   │
│  Recommended ratios:                                             │
│  • electionTimeoutMin ≥ 3 × heartbeatInterval                   │
│  • electionTimeoutMax ≤ 2 × electionTimeoutMin                  │
│                                                                   │
│  Example configurations:                                         │
│                                                                   │
│  Low-latency (same datacenter):                                  │
│    heartbeat: 10ms, election: 50-100ms                          │
│                                                                   │
│  Standard (typical production):                                  │
│    heartbeat: 50ms, election: 150-300ms                         │
│                                                                   │
│  High-latency (cross-datacenter):                               │
│    heartbeat: 100ms, election: 500-1000ms                       │
│                                                                   │
└─────────────────────────────────────────────────────────────────┘
```

---

## Cluster Lifecycle

### Bootstrap (Initial Cluster)

```c
// First-time cluster initialization
void bootstrapCluster(kraftState *state, kraftIp **peers, size_t count,
                      uint64_t clusterId, uint64_t nodeId) {
    // Initialize with known peers
    kraftStateInitRaft(state, count, peers, clusterId, nodeId);

    // On the designated initial leader (usually node 1):
    if (nodeId == 1) {
        // Force become leader for bootstrap
        kraftStateBootstrapLeader(state);
    }
}
```

### Joining Existing Cluster

```c
// Node joining an existing cluster
void joinCluster(kraftServer *server, const char *existingNode, uint16_t port) {
    // Connect to existing cluster member
    kraftServerConnectTo(server, existingNode, port);

    // Request cluster configuration
    // (Handled automatically via Raft protocol)
}
```

### Graceful Shutdown

```c
void gracefulShutdown(kraftServer *server, kraftState *state) {
    // 1. Stop accepting new client requests
    kraftServerStopAccepting(server);

    // 2. If leader, transfer leadership
    if (kraftStateIsLeader(state)) {
        uint64_t target = kraftStateFindBestTransferTarget(state);
        if (target != 0) {
            kraftStateTransferLeadership(state, target);

            // Wait for transfer (with timeout)
            int attempts = 0;
            while (kraftStateIsLeader(state) && attempts < 50) {
                kraftServerPoll(server, 100);  // 100ms
                attempts++;
            }
        }
    }

    // 3. Stop the server
    kraftServerStop(server);
}

// Signal handler
void handleSignal(int sig) {
    if (sig == SIGTERM || sig == SIGINT) {
        gracefulShutdown(globalServer, globalState);
    }
}
```

---

## Membership Changes

### Adding a Node

```c
// Add new node to cluster (run on leader)
bool addNode(kraftState *state, const char *ip, uint16_t port, uint64_t nodeId) {
    kraftIp newNode = {
        .ip = ip,
        .port = port,
        .nodeId = nodeId,
        .outbound = true,
    };

    // Propose membership change (uses joint consensus)
    return kraftStateAddNode(state, &newNode);
}
```

### Removing a Node

```c
// Remove node from cluster (run on leader)
bool removeNode(kraftState *state, uint64_t nodeId) {
    // Check if trying to remove self
    if (nodeId == state->self.nodeId) {
        // Transfer leadership first if we're leader
        if (kraftStateIsLeader(state)) {
            uint64_t target = kraftStateFindTransferTargetExcluding(state, nodeId);
            kraftStateTransferLeadership(state, target);
            return false;  // Let new leader remove us
        }
    }

    return kraftStateRemoveNode(state, nodeId);
}
```

### Monitoring Membership Changes

```c
void onMembershipChange(kraftState *state, kraftMembershipEvent event) {
    switch (event.type) {
        case KRAFT_MEMBER_ADDED:
            printf("Node %llu added to cluster\n", event.nodeId);
            break;

        case KRAFT_MEMBER_REMOVED:
            printf("Node %llu removed from cluster\n", event.nodeId);
            break;

        case KRAFT_JOINT_STARTED:
            printf("Joint consensus started: %zu old, %zu new\n",
                   event.oldCount, event.newCount);
            break;

        case KRAFT_JOINT_COMPLETED:
            printf("Joint consensus completed\n");
            break;
    }
}
```

---

## Persistence & Recovery

### Log Storage

Kraft uses kvidxkit for durable log storage:

```c
// Configure log storage
kraftStorageConfig storageConfig = {
    .dataDir = "/var/lib/kraft/data",
    .walDir = "/var/lib/kraft/wal",      // Separate WAL for performance
    .maxLogSize = 1024 * 1024 * 1024,    // 1GB max log
    .syncMode = KRAFT_SYNC_ALWAYS,       // fsync every write
};

kraftState *state = kraftStateNewWithStorage(&storageConfig);
```

### Recovery on Startup

```c
kraftState *recoverOrCreate(const char *dataDir, kraftIp **peers,
                            size_t count, uint64_t clusterId, uint64_t nodeId) {
    kraftStorageConfig config = {.dataDir = dataDir};

    // Try to recover existing state
    kraftState *state = kraftStateRecover(&config);

    if (state) {
        printf("Recovered: term=%llu, commitIndex=%llu\n",
               state->self.currentTerm, state->commitIndex);
        return state;
    }

    // Fresh start
    state = kraftStateNewWithStorage(&config);
    kraftStateInitRaft(state, count, peers, clusterId, nodeId);
    return state;
}
```

### Sync Modes

```c
typedef enum {
    KRAFT_SYNC_NONE,      // No fsync (fastest, unsafe)
    KRAFT_SYNC_BATCH,     // Batch syncs every N ms
    KRAFT_SYNC_ALWAYS,    // fsync every write (safest)
} kraftSyncMode;
```

---

## Snapshots

### Implementing Snapshot Callbacks

```c
typedef struct {
    hashTable *data;
    uint64_t snapshotIndex;
} MyStateMachine;

// Called when snapshot should be taken
void onSnapshotCreate(kraftState *state, kraftSnapshotWriter *writer, void *ctx) {
    MyStateMachine *sm = ctx;

    // Iterate your state machine and write to snapshot
    hashTableIterator it;
    hashTableIteratorInit(sm->data, &it);

    while (hashTableIteratorNext(&it)) {
        const char *key = it.key;
        const char *value = it.value;

        // Write key-value pair
        uint32_t keyLen = strlen(key);
        uint32_t valLen = strlen(value);

        kraftSnapshotWrite(writer, &keyLen, sizeof(keyLen));
        kraftSnapshotWrite(writer, key, keyLen);
        kraftSnapshotWrite(writer, &valLen, sizeof(valLen));
        kraftSnapshotWrite(writer, value, valLen);
    }

    kraftSnapshotComplete(writer, sm->snapshotIndex);
}

// Called when snapshot should be restored
void onSnapshotRestore(kraftState *state, kraftSnapshotReader *reader, void *ctx) {
    MyStateMachine *sm = ctx;

    // Clear current state
    hashTableClear(sm->data);

    // Read snapshot
    uint32_t keyLen, valLen;
    while (kraftSnapshotRead(reader, &keyLen, sizeof(keyLen)) > 0) {
        char *key = malloc(keyLen + 1);
        char *value;

        kraftSnapshotRead(reader, key, keyLen);
        key[keyLen] = '\0';

        kraftSnapshotRead(reader, &valLen, sizeof(valLen));
        value = malloc(valLen + 1);
        kraftSnapshotRead(reader, value, valLen);
        value[valLen] = '\0';

        hashTableSet(sm->data, key, value);
        free(key);
    }

    sm->snapshotIndex = kraftSnapshotGetIndex(reader);
}
```

### Snapshot Configuration

```c
kraftSnapshotConfig snapConfig = {
    .triggerThreshold = 10000,           // Snapshot every 10k entries
    .minInterval = 60 * 1000,            // At least 60s between snapshots
    .maxSize = 100 * 1024 * 1024,        // 100MB max snapshot
    .compressionEnabled = true,
    .compressionLevel = 6,               // LZ4 compression level

    .onCreate = onSnapshotCreate,
    .onRestore = onSnapshotRestore,
    .userData = &myStateMachine,
};

kraftStateConfigureSnapshots(state, &snapConfig);
```

---

## Event Callbacks

### State Change Notifications

```c
void onStateChange(kraftState *state, kraftRole oldRole, kraftRole newRole) {
    const char *roleNames[] = {
        "FOLLOWER", "PRE_CANDIDATE", "CANDIDATE", "LEADER"
    };

    printf("Role changed: %s -> %s\n",
           roleNames[oldRole], roleNames[newRole]);

    if (newRole == KRAFT_LEADER) {
        // We became leader - initialize leader state
        initializeLeaderState(state);
    } else if (oldRole == KRAFT_LEADER) {
        // Lost leadership - clean up
        cleanupLeaderState(state);
    }
}

void onLeaderChange(kraftState *state, uint64_t oldLeader, uint64_t newLeader) {
    if (newLeader == 0) {
        printf("Cluster has no leader\n");
    } else {
        printf("New leader: node %llu\n", (unsigned long long)newLeader);
    }
}
```

### Commit Notifications

```c
void onCommit(kraftState *state, uint64_t index, uint64_t term) {
    printf("Committed up to index %llu (term %llu)\n",
           (unsigned long long)index, (unsigned long long)term);

    // Trigger apply if needed
    while (state->lastApplied < state->commitIndex) {
        applyNextEntry(state);
    }
}
```

---

## Multi-Raft Server

For horizontal scaling, run multiple Raft groups:

```c
// Create multi-raft manager
kraftMultiRaftServer *multiRaft = kraftMultiRaftServerCreate(adapter, protocol);

// Add raft groups
kraftState *group1 = kraftStateNew();
kraftStateInitRaft(group1, 3, peersGroup1, CLUSTER_ID, nodeId);
kraftMultiRaftAddGroup(multiRaft, "users", group1);

kraftState *group2 = kraftStateNew();
kraftStateInitRaft(group2, 3, peersGroup2, CLUSTER_ID, nodeId);
kraftMultiRaftAddGroup(multiRaft, "orders", group2);

// Route requests by key
kraftState *getGroupForKey(const char *key) {
    uint32_t hash = hashKey(key);
    if (hash < UINT32_MAX / 2) {
        return kraftMultiRaftGetGroup(multiRaft, "users");
    }
    return kraftMultiRaftGetGroup(multiRaft, "orders");
}

// Run all groups
kraftMultiRaftRun(multiRaft);
```

---

## Production Considerations

### Health Checks

```c
typedef struct {
    bool isHealthy;
    bool isLeader;
    uint64_t commitIndex;
    uint64_t lastApplied;
    uint64_t term;
    uint64_t lagMs;
} kraftHealthStatus;

kraftHealthStatus getHealth(kraftState *state) {
    kraftHealthStatus status = {0};

    status.isLeader = kraftStateIsLeader(state);
    status.term = state->self.currentTerm;
    status.commitIndex = state->commitIndex;
    status.lastApplied = state->lastApplied;

    // Check if we're caught up
    status.lagMs = kraftStateGetReplicationLag(state);
    status.isHealthy = status.lagMs < 1000;  // Less than 1s lag

    return status;
}

// HTTP health endpoint
void handleHealthCheck(httpRequest *req, httpResponse *resp) {
    kraftHealthStatus health = getHealth(globalState);

    if (health.isHealthy) {
        httpRespondJSON(resp, 200,
            "{\"status\":\"healthy\",\"leader\":%s}",
            health.isLeader ? "true" : "false");
    } else {
        httpRespondJSON(resp, 503,
            "{\"status\":\"unhealthy\",\"lag_ms\":%llu}",
            health.lagMs);
    }
}
```

### Metrics

```c
// Key metrics to export
typedef struct {
    // Raft state
    uint64_t currentTerm;
    uint64_t commitIndex;
    uint64_t lastApplied;
    uint64_t lastLogIndex;

    // Role (as gauge: 0=follower, 1=candidate, 2=leader)
    int role;

    // Counters
    uint64_t appendEntriesSent;
    uint64_t appendEntriesReceived;
    uint64_t requestVotesSent;
    uint64_t requestVotesReceived;
    uint64_t electionsStarted;
    uint64_t electionsWon;
    uint64_t snapshotsTaken;
    uint64_t snapshotsInstalled;

    // Histograms (latency in microseconds)
    histogram *commitLatency;
    histogram *applyLatency;
    histogram *replicationLatency;
} kraftMetrics;
```

### Graceful Degradation

```c
// Witness mode for degraded operation
void enterWitnessMode(kraftState *state) {
    // Stop participating in elections
    kraftStateSetWitnessMode(state, true);

    // Can still serve stale reads
    printf("Entered witness mode - serving stale reads only\n");
}

void exitWitnessMode(kraftState *state) {
    kraftStateSetWitnessMode(state, false);
    printf("Exited witness mode - full participation resumed\n");
}
```

---

## Complete Production Server

```c
#include "kraft.h"
#include "kraftNetwork.h"
#include "kraftAdapterLibuv.h"
#include <signal.h>
#include <stdio.h>

static kraftServer *server = NULL;
static kraftState *state = NULL;

void handleSignal(int sig) {
    printf("\nReceived signal %d, shutting down...\n", sig);
    if (server) kraftServerStop(server);
}

void applyCommand(const void *data, uint64_t len) {
    // Your state machine here
    printf("APPLY [%llu]: %.*s\n",
           (unsigned long long)state->lastApplied + 1,
           (int)len, (const char *)data);
}

void onStateChange(kraftState *s, kraftRole old, kraftRole new) {
    printf("State: %d -> %d\n", old, new);
}

int main(int argc, char **argv) {
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <node_id> <port> <data_dir>\n", argv[0]);
        return 1;
    }

    uint64_t nodeId = atoi(argv[1]);
    uint16_t port = atoi(argv[2]);
    const char *dataDir = argv[3];

    // Signal handling
    signal(SIGINT, handleSignal);
    signal(SIGTERM, handleSignal);

    // Recovery or fresh start
    kraftStorageConfig storageConfig = {.dataDir = dataDir};
    state = kraftStateRecover(&storageConfig);

    if (!state) {
        state = kraftStateNewWithStorage(&storageConfig);

        kraftIp peers[3] = {
            {"127.0.0.1", 7001, true, 1},
            {"127.0.0.1", 7002, true, 2},
            {"127.0.0.1", 7003, true, 3},
        };
        kraftIp *peerPtrs[] = {&peers[0], &peers[1], &peers[2]};
        kraftStateInitRaft(state, 3, peerPtrs, 0xCAFE, nodeId);
    }

    // Configure callbacks
    state->internals.data.apply = applyCommand;
    kraftStateSetCallback(state, KRAFT_CB_STATE_CHANGE, onStateChange);

    // Enable features
    kraftStateEnablePreVote(state, true);
    kraftStateEnableReadIndex(state, true);

    // Create server
    kraftAdapter *adapter = kraftAdapterLibuvCreate();
    kraftProtocol *protocol = kraftProtocolCreate(
        kraftProtocolGetBinaryOps(), NULL);

    server = kraftServerCreate(adapter, protocol);
    kraftServerSetState(server, state);
    kraftServerListenOn(server, "0.0.0.0", port);

    printf("Node %llu ready on port %u\n",
           (unsigned long long)nodeId, port);

    // Run
    kraftServerRun(server);

    // Cleanup
    kraftServerDestroy(server);
    kraftStateFree(state);
    kraftProtocolDestroy(protocol);
    kraftAdapterDestroy(adapter);

    printf("Shutdown complete\n");
    return 0;
}
```

---

## Next Steps

- [Custom Adapters](custom-adapters.md) - Implement your own network backend
- [Custom Protocols](custom-protocols.md) - Implement wire format encodings
- [Snapshots & Compaction](../features/snapshots.md) - Log compaction strategies
- [Production Deployment](../operations/deployment.md) - Cluster sizing and configuration
