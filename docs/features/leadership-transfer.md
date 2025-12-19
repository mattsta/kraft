# Leadership Transfer

Leadership transfer enables graceful handoff of leader role to another node, useful for maintenance, load balancing, and planned failovers.

## Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                  Leadership Transfer Flow                        │
│                                                                  │
│  Current Leader          Target Node           Other Nodes      │
│       │                       │                     │           │
│       │  1. Stop accepting    │                     │           │
│       │     new writes        │                     │           │
│       │                       │                     │           │
│       │  2. Ensure target     │                     │           │
│       │     is caught up      │                     │           │
│       │──────────────────────►│                     │           │
│       │                       │                     │           │
│       │  3. Send TimeoutNow   │                     │           │
│       │──────────────────────►│                     │           │
│       │                       │                     │           │
│       │                       │  4. Start election  │           │
│       │                       │─────────────────────►           │
│       │                       │                     │           │
│       │                       │  5. Win (guaranteed) │          │
│       │◄──────────────────────│◄────────────────────│           │
│       │                       │                     │           │
│       │  6. Step down to      │  New Leader!        │           │
│       │     follower          │                     │           │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## When to Use

### Maintenance & Updates

```c
// Rolling restart: transfer leadership before shutting down
void rollingRestart(kraftState *state) {
    if (kraftStateIsLeader(state)) {
        // Find healthy follower
        uint64_t target = kraftStateFindBestTransferTarget(state);

        if (target != 0) {
            printf("Transferring leadership to node %llu\n", target);
            kraftStateTransferLeadership(state, target);

            // Wait for transfer
            while (kraftStateIsLeader(state)) {
                usleep(10000);  // 10ms
            }
        }
    }

    // Now safe to restart
    shutdownNode();
}
```

### Load Balancing

```c
// Rebalance leaders across nodes
void rebalanceLeaders(Cluster *cluster) {
    // Count leaders per node
    int leaderCounts[MAX_NODES] = {0};
    for (int i = 0; i < cluster->groupCount; i++) {
        uint64_t leader = getLeader(cluster->groups[i]);
        leaderCounts[leader]++;
    }

    // Find overloaded and underloaded nodes
    for (int i = 0; i < cluster->groupCount; i++) {
        kraftState *state = cluster->groups[i];
        if (!kraftStateIsLeader(state)) continue;

        uint64_t currentNode = state->self.nodeId;
        if (leaderCounts[currentNode] <= cluster->avgLeadersPerNode) continue;

        // Find underloaded node
        uint64_t target = findUnderloadedNode(state, leaderCounts);
        if (target != 0) {
            kraftStateTransferLeadership(state, target);
            leaderCounts[currentNode]--;
            leaderCounts[target]++;
        }
    }
}
```

### Locality Optimization

```c
// Transfer leadership to node closest to most clients
void optimizeLeaderLocality(kraftState *state, ClientMetrics *metrics) {
    // Find node with lowest average client latency
    uint64_t bestNode = 0;
    uint64_t bestLatency = UINT64_MAX;

    for (int i = 0; i < state->clusterSize; i++) {
        uint64_t nodeId = state->nodes[i].nodeId;
        uint64_t avgLatency = metrics->avgClientLatency[nodeId];

        if (avgLatency < bestLatency) {
            bestLatency = avgLatency;
            bestNode = nodeId;
        }
    }

    if (bestNode != 0 && bestNode != state->self.nodeId) {
        if (kraftStateIsLeader(state)) {
            kraftStateTransferLeadership(state, bestNode);
        }
    }
}
```

---

## API

### Basic Transfer

```c
// Transfer to specific node
bool kraftStateTransferLeadership(kraftState *state, uint64_t targetNodeId);

// Transfer to best available node
uint64_t kraftStateFindBestTransferTarget(kraftState *state);
bool kraftStateTransferLeadershipToBest(kraftState *state);

// Check transfer status
typedef enum {
    KRAFT_TRANSFER_NONE,
    KRAFT_TRANSFER_IN_PROGRESS,
    KRAFT_TRANSFER_SUCCEEDED,
    KRAFT_TRANSFER_FAILED,
    KRAFT_TRANSFER_TIMEOUT,
} kraftTransferStatus;

kraftTransferStatus kraftStateGetTransferStatus(kraftState *state);
```

### With Callback

```c
typedef void (*kraftTransferCallback)(
    kraftState *state,
    uint64_t targetNode,
    kraftTransferStatus status,
    void *userData
);

void kraftStateTransferLeadershipAsync(
    kraftState *state,
    uint64_t targetNodeId,
    uint64_t timeoutMs,
    kraftTransferCallback callback,
    void *userData
);

// Example
void onTransferComplete(kraftState *state, uint64_t target,
                        kraftTransferStatus status, void *userData) {
    if (status == KRAFT_TRANSFER_SUCCEEDED) {
        printf("Successfully transferred to node %llu\n", target);
        proceedWithMaintenance();
    } else {
        printf("Transfer failed: %d\n", status);
        retryOrAbort();
    }
}

kraftStateTransferLeadershipAsync(
    state, targetNode, 5000, onTransferComplete, NULL);
```

---

## Target Selection

### Best Target Algorithm

```c
uint64_t findBestTransferTarget(kraftState *state) {
    uint64_t best = 0;
    int bestScore = INT_MIN;

    for (int i = 0; i < state->clusterSize; i++) {
        uint64_t nodeId = state->nodes[i].nodeId;
        if (nodeId == state->self.nodeId) continue;  // Skip self

        int score = 0;

        // Prefer caught-up nodes
        uint64_t matchIndex = state->replication[nodeId].matchIndex;
        if (matchIndex >= state->commitIndex) {
            score += 100;
        } else {
            score -= (state->commitIndex - matchIndex);
        }

        // Prefer healthy nodes
        if (isNodeHealthy(state, nodeId)) {
            score += 50;
        }

        // Prefer nodes with lower leader count (for multi-raft)
        score -= getLeaderCount(nodeId) * 10;

        // Prefer nodes in same datacenter
        if (sameDatacenter(state->self.nodeId, nodeId)) {
            score += 20;
        }

        if (score > bestScore) {
            bestScore = score;
            best = nodeId;
        }
    }

    return best;
}
```

### Exclude Nodes

```c
// Transfer to any node except specified ones
uint64_t kraftStateFindTransferTargetExcluding(
    kraftState *state,
    uint64_t *excludeNodes,
    size_t excludeCount
);

// Example: Transfer before removing self
void prepareSelfRemoval(kraftState *state) {
    uint64_t exclude[] = {state->self.nodeId};

    uint64_t target = kraftStateFindTransferTargetExcluding(
        state, exclude, 1);

    if (target != 0) {
        kraftStateTransferLeadership(state, target);
    }
}
```

---

## Protocol Details

### TimeoutNow RPC

```c
typedef struct {
    uint64_t term;        // Leader's current term
    uint64_t leaderId;    // Current leader ID
} kraftTimeoutNowArgs;

// Receiver immediately starts election
void handleTimeoutNow(kraftState *state, kraftTimeoutNowArgs *args) {
    // Verify sender is current leader
    if (args->leaderId != state->leader ||
        args->term != state->self.currentTerm) {
        return;  // Ignore stale message
    }

    // Start election immediately (skip timeout)
    state->self.currentTerm++;
    state->self.role = KRAFT_CANDIDATE;
    state->self.votedFor = state->self.nodeId;

    // Request votes
    broadcastRequestVote(state);
}
```

### Transfer State Machine

```c
typedef struct {
    kraftTransferStatus status;
    uint64_t targetNode;
    uint64_t startTime;
    uint64_t timeout;
    kraftTransferCallback callback;
    void *userData;
} kraftTransferState;

void transferTick(kraftState *state) {
    kraftTransferState *t = &state->transfer;

    if (t->status != KRAFT_TRANSFER_IN_PROGRESS) return;

    // Check timeout
    if (now() > t->startTime + t->timeout) {
        t->status = KRAFT_TRANSFER_TIMEOUT;
        if (t->callback) {
            t->callback(state, t->targetNode, t->status, t->userData);
        }
        return;
    }

    // Check if we're still leader
    if (!kraftStateIsLeader(state)) {
        t->status = KRAFT_TRANSFER_SUCCEEDED;
        if (t->callback) {
            t->callback(state, t->targetNode, t->status, t->userData);
        }
        return;
    }

    // Check if target is caught up
    uint64_t matchIndex = state->replication[t->targetNode].matchIndex;
    if (matchIndex >= state->log.lastIndex) {
        // Target is caught up, send TimeoutNow
        sendTimeoutNow(state, t->targetNode);
    } else {
        // Continue replicating to target
        sendAppendEntries(state, t->targetNode);
    }
}
```

---

## Safety Guarantees

### Pre-Transfer Checks

```c
bool canTransferTo(kraftState *state, uint64_t targetNode) {
    // Must be leader
    if (!kraftStateIsLeader(state)) return false;

    // Target must be in cluster
    if (!isClusterMember(state, targetNode)) return false;

    // Target must not be self
    if (targetNode == state->self.nodeId) return false;

    // No transfer already in progress
    if (state->transfer.status == KRAFT_TRANSFER_IN_PROGRESS) return false;

    // Target must be reachable
    if (!isNodeReachable(state, targetNode)) return false;

    return true;
}
```

### During Transfer

```c
// Leader behavior during transfer
void leaderBehaviorDuringTransfer(kraftState *state) {
    // Option 1: Stop accepting new writes
    state->acceptingWrites = false;

    // Option 2: Accept writes but prioritize replication to target
    // (depends on configuration)
}

// Handle incoming requests during transfer
bool handleClientRequest(kraftState *state, ClientRequest *req) {
    if (state->transfer.status == KRAFT_TRANSFER_IN_PROGRESS) {
        if (state->config.rejectDuringTransfer) {
            return rejectWithRetry(req, "leadership transfer in progress");
        }
        // Or queue for new leader
    }

    return processNormally(req);
}
```

---

## Failure Handling

### Transfer Failures

```c
typedef enum {
    KRAFT_TRANSFER_FAIL_UNREACHABLE,  // Target not reachable
    KRAFT_TRANSFER_FAIL_BEHIND,       // Target too far behind
    KRAFT_TRANSFER_FAIL_REJECTED,     // Target declined
    KRAFT_TRANSFER_FAIL_TIMEOUT,      // Timed out
    KRAFT_TRANSFER_FAIL_LOST_LEADER,  // Lost leadership during transfer
} kraftTransferFailReason;

void handleTransferFailure(kraftState *state, kraftTransferFailReason reason) {
    switch (reason) {
        case KRAFT_TRANSFER_FAIL_UNREACHABLE:
            // Try different target
            uint64_t newTarget = findAlternativeTarget(state);
            if (newTarget) {
                kraftStateTransferLeadership(state, newTarget);
            }
            break;

        case KRAFT_TRANSFER_FAIL_BEHIND:
            // Wait and retry
            scheduleRetry(state, 1000);  // 1 second
            break;

        case KRAFT_TRANSFER_FAIL_TIMEOUT:
            // Abort and resume normal operation
            state->acceptingWrites = true;
            notifyOperator("transfer timed out");
            break;

        case KRAFT_TRANSFER_FAIL_LOST_LEADER:
            // Someone else became leader, that's fine
            break;
    }
}
```

### Automatic Retry

```c
kraftTransferConfig config = {
    .timeout = 10000,        // 10 second total timeout
    .retryInterval = 500,    // Retry every 500ms
    .maxRetries = 3,         // Max 3 attempts per target
    .autoFallback = true,    // Try other targets if first fails
};

kraftStateConfigureTransfer(state, &config);
```

---

## Client Handling

### During Transfer

```c
// Client-side handling of leadership changes
void handleLeadershipChange(kraftClientSession *client) {
    // Option 1: Queue requests
    pauseRequests(client);
    waitForNewLeader(client, 5000);
    retryQueuedRequests(client);

    // Option 2: Return error to application
    notifyApplication("leader changing, retry later");

    // Option 3: Automatic redirect
    refreshLeaderInfo(client);
    resendPendingRequests(client);
}
```

### Notification

```c
// Leader notifies clients before transfer
void prepareClientsForTransfer(kraftState *state) {
    // Send "draining" signal to clients
    LeaderDrainingNotification notify = {
        .newLeader = state->transfer.targetNode,
        .drainingUntil = now() + 5000,
    };

    broadcastToClients(state, &notify);
}

// Client handles notification
void handleDrainingNotification(kraftClientSession *client,
                                LeaderDrainingNotification *notify) {
    // Preemptively connect to new leader
    connectToNode(client, notify->newLeader);

    // Start sending new requests to new leader
    client->preferredLeader = notify->newLeader;
}
```

---

## Monitoring

### Transfer Metrics

```c
typedef struct {
    uint64_t transfersInitiated;
    uint64_t transfersSucceeded;
    uint64_t transfersFailed;
    uint64_t transfersTimedOut;

    histogram *transferDuration;
    histogram *catchUpDuration;
} TransferMetrics;

void exportTransferMetrics(kraftState *state) {
    TransferMetrics *m = &state->transferMetrics;

    exportCounter("leadership_transfers_total", m->transfersInitiated);
    exportCounter("leadership_transfers_succeeded", m->transfersSucceeded);
    exportCounter("leadership_transfers_failed", m->transfersFailed);
    exportHistogram("leadership_transfer_duration_ms", m->transferDuration);
}
```

### Events

```c
// Log transfer events
void logTransferEvent(kraftState *state, const char *event) {
    LogEntry entry = {
        .timestamp = now(),
        .event = event,
        .from = state->self.nodeId,
        .to = state->transfer.targetNode,
        .term = state->self.currentTerm,
    };

    logEvent(&entry);
}

// Events to log:
// - "transfer_started"
// - "target_caught_up"
// - "timeout_now_sent"
// - "transfer_succeeded"
// - "transfer_failed"
// - "transfer_timeout"
```

---

## Best Practices

### When to Transfer

```
┌─────────────────────────────────────────────────────────────────┐
│                   Transfer Recommendations                       │
│                                                                  │
│  DO transfer before:                                            │
│  • Node shutdown or restart                                     │
│  • Node decommission                                            │
│  • High load period (move to more capable node)                 │
│  • Network maintenance                                          │
│                                                                  │
│  DON'T transfer:                                                │
│  • During high write load (increases latency)                   │
│  • When all followers are lagging                               │
│  • During cluster membership changes                            │
│  • For no reason (elections have cost)                          │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Timeout Configuration

```c
// Timeout should be at least:
// (log_lag / replication_rate) + election_timeout + buffer

uint64_t calculateTransferTimeout(kraftState *state, uint64_t targetNode) {
    uint64_t lag = state->log.lastIndex -
                   state->replication[targetNode].matchIndex;
    uint64_t entriesPerSec = state->metrics.replicationRate;
    uint64_t catchupTime = (lag * 1000) / entriesPerSec;

    uint64_t electionTimeout = state->config.electionTimeoutMax;
    uint64_t buffer = 2000;  // 2 second buffer

    return catchupTime + electionTimeout + buffer;
}
```

---

## Next Steps

- [ReadIndex](read-index.md) - Linearizable reads
- [Async Commit](async-commit.md) - Durability modes
- [Production Deployment](../operations/deployment.md) - Operational procedures
