# Multi-Raft

Multi-Raft enables horizontal scaling by running multiple independent Raft groups, each managing a subset of your data.

## Why Multi-Raft?

### Single Raft Limitations

```
┌─────────────────────────────────────────────────────────────────┐
│                      Single Raft Group                           │
│                                                                  │
│  ┌─────────┐                                                    │
│  │ Leader  │ ─── All writes go through one node                 │
│  └────┬────┘                                                    │
│       │                                                          │
│  ┌────┴────┐                                                    │
│  │         │                                                    │
│  ▼         ▼                                                    │
│ ┌───┐   ┌───┐                                                   │
│ │ F │   │ F │  Followers only replicate                         │
│ └───┘   └───┘                                                   │
│                                                                  │
│  Bottlenecks:                                                   │
│  • Single leader handles all writes                             │
│  • Log grows unbounded for all data                             │
│  • Memory limited to single machine                             │
│  • CPU bound by single leader's capacity                        │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Multi-Raft Solution

```
┌─────────────────────────────────────────────────────────────────┐
│                      Multi-Raft Groups                           │
│                                                                  │
│  ┌──────────────┐  ┌──────────────┐  ┌──────────────┐          │
│  │  Group A     │  │  Group B     │  │  Group C     │          │
│  │  (keys: a-m) │  │  (keys: n-z) │  │  (metadata)  │          │
│  │              │  │              │  │              │          │
│  │  L → F → F   │  │  F → L → F   │  │  F → F → L   │          │
│  │              │  │              │  │              │          │
│  └──────────────┘  └──────────────┘  └──────────────┘          │
│                                                                  │
│  Node 1: Leader(A), Follower(B), Follower(C)                    │
│  Node 2: Follower(A), Leader(B), Follower(C)                    │
│  Node 3: Follower(A), Follower(B), Leader(C)                    │
│                                                                  │
│  Benefits:                                                       │
│  • Write load distributed across nodes                          │
│  • Each group has independent log                               │
│  • Data partitioned across machines                             │
│  • Linear scalability                                           │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Architecture

### Multi-Raft Manager

```c
typedef struct kraftMultiRaftManager {
    // Group registry
    kraftRaftGroup **groups;
    size_t groupCount;
    size_t groupCapacity;

    // Group lookup
    hashTable *groupsByName;
    hashTable *groupsById;

    // Shared resources
    kraftAdapter *adapter;
    kraftProtocol *protocol;

    // Configuration
    kraftMultiRaftConfig config;

    // Metrics
    kraftMultiRaftMetrics metrics;

} kraftMultiRaftManager;

typedef struct kraftRaftGroup {
    char *name;
    uint64_t groupId;
    kraftState *state;

    // Group-specific callbacks
    kraftApplyCallback apply;
    kraftSnapshotCallback snapshot;
    void *userData;

} kraftRaftGroup;
```

### Shared vs. Dedicated Resources

```
┌─────────────────────────────────────────────────────────────────┐
│                    Resource Sharing Options                      │
│                                                                  │
│  Shared Adapter (Recommended):                                  │
│  ┌────────────────────────────────────────────────────┐         │
│  │                   kraftAdapter                      │         │
│  │  (Single event loop, multiplexed connections)      │         │
│  └─────────────────────────┬──────────────────────────┘         │
│                            │                                     │
│       ┌────────────────────┼────────────────────┐               │
│       ▼                    ▼                    ▼               │
│   Group A              Group B              Group C             │
│                                                                  │
│  Pros:                                                          │
│  • Efficient connection sharing                                 │
│  • Single event loop overhead                                   │
│  • Simpler resource management                                  │
│                                                                  │
│  Dedicated Adapters (Special cases):                            │
│  ┌──────────┐      ┌──────────┐      ┌──────────┐              │
│  │ Adapter A│      │ Adapter B│      │ Adapter C│              │
│  └────┬─────┘      └────┬─────┘      └────┬─────┘              │
│       ▼                 ▼                 ▼                     │
│   Group A           Group B           Group C                   │
│                                                                  │
│  Pros:                                                          │
│  • Isolation between groups                                     │
│  • Different network configurations                             │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Creating Multi-Raft Groups

### Basic Setup

```c
#include "kraftMultiRaft.h"

// Create manager with shared adapter
kraftAdapter *adapter = kraftAdapterLibuvCreate();
kraftProtocol *protocol = kraftProtocolCreate(
    kraftProtocolGetBinaryOps(), NULL);

kraftMultiRaftManager *manager = kraftMultiRaftManagerCreate(
    adapter, protocol);

// Create first group (users partition)
kraftRaftGroup *usersGroup = kraftMultiRaftCreateGroup(manager, "users");
kraftMultiRaftGroupSetApply(usersGroup, applyUserCommand);

// Configure cluster for this group
kraftIp usersPeers[] = {
    {"node1.cluster", 7001, true, 1},
    {"node2.cluster", 7001, true, 2},
    {"node3.cluster", 7001, true, 3},
};
kraftMultiRaftGroupInitCluster(usersGroup, usersPeers, 3, nodeId);

// Create second group (orders partition)
kraftRaftGroup *ordersGroup = kraftMultiRaftCreateGroup(manager, "orders");
kraftMultiRaftGroupSetApply(ordersGroup, applyOrderCommand);

kraftIp ordersPeers[] = {
    {"node1.cluster", 7002, true, 1},
    {"node2.cluster", 7002, true, 2},
    {"node3.cluster", 7002, true, 3},
};
kraftMultiRaftGroupInitCluster(ordersGroup, ordersPeers, 3, nodeId);

// Start all groups
kraftMultiRaftStart(manager);

// Run (blocks)
kraftMultiRaftRun(manager);
```

### Dynamic Group Creation

```c
// Create groups dynamically (e.g., per-tenant)
kraftRaftGroup *createTenantGroup(kraftMultiRaftManager *manager,
                                   const char *tenantId) {
    char groupName[256];
    snprintf(groupName, sizeof(groupName), "tenant-%s", tenantId);

    kraftRaftGroup *group = kraftMultiRaftCreateGroup(manager, groupName);

    // Configure with tenant-specific peers
    kraftIp *peers = getTenantPeers(tenantId);
    size_t peerCount = getTenantPeerCount(tenantId);

    kraftMultiRaftGroupInitCluster(group, peers, peerCount, getNodeId());

    // Start the group
    kraftMultiRaftGroupStart(group);

    return group;
}

// Remove a group
void removeTenantGroup(kraftMultiRaftManager *manager,
                       const char *tenantId) {
    char groupName[256];
    snprintf(groupName, sizeof(groupName), "tenant-%s", tenantId);

    kraftRaftGroup *group = kraftMultiRaftGetGroup(manager, groupName);
    if (group) {
        kraftMultiRaftGroupStop(group);
        kraftMultiRaftRemoveGroup(manager, groupName);
    }
}
```

---

## Routing Requests

### Hash-Based Routing

```c
// Simple hash routing
uint32_t hashKey(const char *key, size_t len) {
    uint32_t hash = 0;
    for (size_t i = 0; i < len; i++) {
        hash = hash * 31 + key[i];
    }
    return hash;
}

kraftRaftGroup *routeByHash(kraftMultiRaftManager *manager,
                            const char *key) {
    uint32_t hash = hashKey(key, strlen(key));
    size_t groupIndex = hash % manager->groupCount;
    return manager->groups[groupIndex];
}

// Consistent hashing for better distribution
kraftRaftGroup *routeByConsistentHash(kraftMultiRaftManager *manager,
                                       const char *key) {
    uint32_t hash = hashKey(key, strlen(key));

    // Find group in ring
    for (size_t i = 0; i < manager->hashRingSize; i++) {
        if (hash <= manager->hashRing[i].position) {
            return manager->hashRing[i].group;
        }
    }
    return manager->hashRing[0].group;  // Wrap around
}
```

### Range-Based Routing

```c
typedef struct {
    char *startKey;
    char *endKey;
    kraftRaftGroup *group;
} RangeMapping;

kraftRaftGroup *routeByRange(RangeMapping *mappings, size_t count,
                             const char *key) {
    for (size_t i = 0; i < count; i++) {
        if (strcmp(key, mappings[i].startKey) >= 0 &&
            strcmp(key, mappings[i].endKey) < 0) {
            return mappings[i].group;
        }
    }
    return NULL;  // Key not in any range
}

// Example mappings
RangeMapping mappings[] = {
    {"a", "m", usersGroup},
    {"m", "z", ordersGroup},
};
```

### Prefix-Based Routing

```c
kraftRaftGroup *routeByPrefix(kraftMultiRaftManager *manager,
                              const char *key) {
    // Route by key prefix
    if (strncmp(key, "user:", 5) == 0) {
        return kraftMultiRaftGetGroup(manager, "users");
    }
    if (strncmp(key, "order:", 6) == 0) {
        return kraftMultiRaftGetGroup(manager, "orders");
    }
    if (strncmp(key, "session:", 8) == 0) {
        return kraftMultiRaftGetGroup(manager, "sessions");
    }

    // Default group
    return kraftMultiRaftGetGroup(manager, "default");
}
```

---

## Client Integration

### Multi-Raft Client

```c
typedef struct {
    kraftMultiRaftManager *manager;
    kraftClientSession **groupClients;  // One per group
    size_t groupCount;
} MultiRaftClient;

MultiRaftClient *multiRaftClientCreate(kraftMultiRaftManager *manager) {
    MultiRaftClient *client = calloc(1, sizeof(*client));
    client->manager = manager;
    client->groupCount = manager->groupCount;
    client->groupClients = calloc(client->groupCount, sizeof(void *));

    // Create client session for each group
    for (size_t i = 0; i < client->groupCount; i++) {
        kraftRaftGroup *group = manager->groups[i];
        client->groupClients[i] = kraftClientCreate(
            manager->adapter, manager->protocol, NULL);

        // Add endpoints for this group's peers
        for (size_t j = 0; j < group->peerCount; j++) {
            kraftClientAddEndpoint(client->groupClients[i],
                                   group->peers[j].ip,
                                   group->peers[j].port);
        }

        kraftClientConnect(client->groupClients[i]);
    }

    return client;
}

// Submit to appropriate group
bool multiRaftSubmit(MultiRaftClient *client, const char *key,
                     const uint8_t *data, size_t len,
                     kraftClientCallback callback, void *userData) {
    // Route to group
    kraftRaftGroup *group = routeByPrefix(client->manager, key);
    if (!group) return false;

    // Find client for this group
    kraftClientSession *groupClient = NULL;
    for (size_t i = 0; i < client->groupCount; i++) {
        if (client->manager->groups[i] == group) {
            groupClient = client->groupClients[i];
            break;
        }
    }

    if (!groupClient) return false;

    return kraftClientSubmit(groupClient, data, len, callback, userData);
}
```

### Cross-Group Transactions

```c
typedef struct {
    kraftRaftGroup **groups;
    uint8_t **commands;
    size_t *commandLens;
    size_t count;
    kraftTransactionCallback callback;
    void *userData;
} CrossGroupTransaction;

// Two-phase commit across groups
void crossGroupCommit(MultiRaftClient *client,
                      CrossGroupTransaction *txn) {
    // Phase 1: Prepare
    size_t prepared = 0;
    for (size_t i = 0; i < txn->count; i++) {
        bool ok = prepareOnGroup(txn->groups[i],
                                 txn->commands[i],
                                 txn->commandLens[i]);
        if (ok) prepared++;
    }

    // Check for unanimous prepare
    if (prepared != txn->count) {
        // Abort all
        for (size_t i = 0; i < txn->count; i++) {
            abortOnGroup(txn->groups[i]);
        }
        txn->callback(txn->userData, false);
        return;
    }

    // Phase 2: Commit
    for (size_t i = 0; i < txn->count; i++) {
        commitOnGroup(txn->groups[i]);
    }

    txn->callback(txn->userData, true);
}
```

---

## Group Management

### Rebalancing

```c
// Move a range from one group to another
typedef struct {
    char *startKey;
    char *endKey;
    kraftRaftGroup *sourceGroup;
    kraftRaftGroup *targetGroup;
} RebalanceTask;

void rebalanceRange(RebalanceTask *task) {
    // 1. Freeze writes to range on source
    freezeRange(task->sourceGroup, task->startKey, task->endKey);

    // 2. Snapshot range data
    RangeSnapshot *snapshot = snapshotRange(
        task->sourceGroup, task->startKey, task->endKey);

    // 3. Transfer to target group
    transferSnapshot(task->targetGroup, snapshot);

    // 4. Update routing
    updateRouting(task->startKey, task->endKey, task->targetGroup);

    // 5. Unfreeze and delete from source
    deleteRange(task->sourceGroup, task->startKey, task->endKey);
    unfreezeRange(task->sourceGroup, task->startKey, task->endKey);
}
```

### Group Splitting

```c
// Split a group into two when it gets too large
void splitGroup(kraftMultiRaftManager *manager, kraftRaftGroup *group) {
    // Find split point
    char *splitKey = findMedianKey(group);

    // Create new group for upper half
    char newName[256];
    snprintf(newName, sizeof(newName), "%s-split", group->name);
    kraftRaftGroup *newGroup = kraftMultiRaftCreateGroup(manager, newName);

    // Configure with same peers (different ports)
    configureSplitPeers(newGroup, group);

    // Transfer data above split point
    RebalanceTask task = {
        .startKey = splitKey,
        .endKey = "\xff",  // End of range
        .sourceGroup = group,
        .targetGroup = newGroup,
    };
    rebalanceRange(&task);

    // Update group's range
    group->endKey = splitKey;
    newGroup->startKey = splitKey;
}
```

### Group Merging

```c
// Merge two groups when they're underutilized
void mergeGroups(kraftMultiRaftManager *manager,
                 kraftRaftGroup *group1, kraftRaftGroup *group2) {
    // Ensure groups are adjacent
    if (strcmp(group1->endKey, group2->startKey) != 0) {
        return;  // Can't merge non-adjacent groups
    }

    // Transfer all data from group2 to group1
    RebalanceTask task = {
        .startKey = group2->startKey,
        .endKey = group2->endKey,
        .sourceGroup = group2,
        .targetGroup = group1,
    };
    rebalanceRange(&task);

    // Extend group1's range
    group1->endKey = group2->endKey;

    // Remove empty group
    kraftMultiRaftRemoveGroup(manager, group2->name);
}
```

---

## Coordination

### Metadata Group

Use a dedicated Raft group for cluster metadata:

```c
typedef struct {
    kraftRaftGroup *metaGroup;    // Stores routing, membership
    kraftRaftGroup **dataGroups;  // Data partitions
    size_t dataGroupCount;
} CoordinatedMultiRaft;

// Operations that modify metadata go through metaGroup
void createDataGroup(CoordinatedMultiRaft *cluster, const char *name) {
    // Propose to metadata group
    MetaCommand cmd = {
        .type = META_CREATE_GROUP,
        .groupName = name,
    };

    kraftSubmitSync(cluster->metaGroup->state, &cmd, sizeof(cmd));

    // Metadata apply will actually create the group
}

void applyMetaCommand(const void *data, size_t len) {
    const MetaCommand *cmd = data;

    switch (cmd->type) {
        case META_CREATE_GROUP:
            // Actually create the data group
            createDataGroupInternal(cmd->groupName);
            break;

        case META_DELETE_GROUP:
            deleteDataGroupInternal(cmd->groupName);
            break;

        case META_UPDATE_ROUTING:
            updateRoutingInternal(cmd->routingUpdate);
            break;
    }
}
```

### Leader Locality

Prefer placing leaders close to where they're needed:

```c
typedef struct {
    char *datacenter;
    float loadFactor;
} NodeLocality;

// Score nodes for leadership
float scoreLeaderCandidate(kraftRaftGroup *group, uint64_t nodeId) {
    float score = 0;

    // Prefer same datacenter as most clients
    NodeLocality *locality = getNodeLocality(nodeId);
    if (strcmp(locality->datacenter, group->primaryDatacenter) == 0) {
        score += 10.0;
    }

    // Prefer lower load
    score -= locality->loadFactor * 5.0;

    // Prefer fewer existing leader roles
    score -= countLeaderRoles(nodeId) * 2.0;

    return score;
}

// Periodically rebalance leaders
void rebalanceLeaders(kraftMultiRaftManager *manager) {
    for (size_t i = 0; i < manager->groupCount; i++) {
        kraftRaftGroup *group = manager->groups[i];

        if (!kraftStateIsLeader(group->state)) continue;

        // Find better candidate
        uint64_t best = 0;
        float bestScore = scoreLeaderCandidate(group, getNodeId());

        for (size_t j = 0; j < group->peerCount; j++) {
            float score = scoreLeaderCandidate(group, group->peers[j].nodeId);
            if (score > bestScore) {
                best = group->peers[j].nodeId;
                bestScore = score;
            }
        }

        // Transfer if significantly better
        if (best != 0 && bestScore > scoreLeaderCandidate(group, getNodeId()) + 5.0) {
            kraftStateTransferLeadership(group->state, best);
        }
    }
}
```

---

## Performance Optimization

### Batching Across Groups

```c
typedef struct {
    kraftRaftGroup *group;
    uint8_t *command;
    size_t commandLen;
    void *userData;
} PendingCommand;

typedef struct {
    PendingCommand *commands;
    size_t count;
    size_t capacity;
} CommandBatch;

// Batch commands by group
void batchSubmit(MultiRaftClient *client, PendingCommand *cmds, size_t count) {
    // Group by target
    hashTable *byGroup = hashTableCreate();

    for (size_t i = 0; i < count; i++) {
        CommandBatch *batch = hashTableGet(byGroup, cmds[i].group->name);
        if (!batch) {
            batch = calloc(1, sizeof(*batch));
            hashTableSet(byGroup, cmds[i].group->name, batch);
        }
        addToBatch(batch, &cmds[i]);
    }

    // Submit batches
    hashTableIterator it;
    hashTableIteratorInit(byGroup, &it);
    while (hashTableIteratorNext(&it)) {
        CommandBatch *batch = it.value;
        submitBatchToGroup(client, it.key, batch);
    }

    hashTableDestroy(byGroup);
}
```

### Parallel Reads

```c
typedef struct {
    char *key;
    void *result;
    bool complete;
} ReadRequest;

void parallelRead(MultiRaftClient *client,
                  ReadRequest *requests, size_t count) {
    // Issue all reads in parallel
    for (size_t i = 0; i < count; i++) {
        kraftRaftGroup *group = routeByPrefix(client->manager, requests[i].key);
        kraftClientRead(getClientForGroup(client, group),
                        requests[i].key, onReadComplete, &requests[i]);
    }

    // Wait for all to complete
    while (!allComplete(requests, count)) {
        kraftMultiRaftPoll(client->manager, 10);
    }
}
```

---

## Monitoring

### Per-Group Metrics

```c
typedef struct {
    // State
    kraftRole role;
    uint64_t term;
    uint64_t commitIndex;
    uint64_t lastApplied;

    // Counters
    uint64_t commandsApplied;
    uint64_t bytesReplicated;

    // Latencies
    histogram *commitLatency;
    histogram *applyLatency;
} GroupMetrics;

void collectMultiRaftMetrics(kraftMultiRaftManager *manager) {
    for (size_t i = 0; i < manager->groupCount; i++) {
        kraftRaftGroup *group = manager->groups[i];
        GroupMetrics *m = &group->metrics;

        m->role = kraftStateGetRole(group->state);
        m->term = group->state->self.currentTerm;
        m->commitIndex = group->state->commitIndex;
        m->lastApplied = group->state->lastApplied;

        // Export to monitoring system
        exportGauge("raft_role", m->role, "group", group->name);
        exportGauge("raft_term", m->term, "group", group->name);
        exportGauge("raft_commit_index", m->commitIndex, "group", group->name);
    }
}
```

### Health Aggregation

```c
typedef struct {
    size_t healthyGroups;
    size_t leaderGroups;
    size_t laggingGroups;
    size_t totalGroups;
} MultiRaftHealth;

MultiRaftHealth getMultiRaftHealth(kraftMultiRaftManager *manager) {
    MultiRaftHealth health = {0};
    health.totalGroups = manager->groupCount;

    for (size_t i = 0; i < manager->groupCount; i++) {
        kraftRaftGroup *group = manager->groups[i];

        if (isGroupHealthy(group)) {
            health.healthyGroups++;
        }
        if (kraftStateIsLeader(group->state)) {
            health.leaderGroups++;
        }
        if (getGroupLag(group) > 1000) {  // > 1s lag
            health.laggingGroups++;
        }
    }

    return health;
}
```

---

## Next Steps

- [Architecture](../architecture.md) - System design overview
- [Snapshots](../features/snapshots.md) - Per-group snapshots
- [Production Deployment](../operations/deployment.md) - Multi-raft deployment
