# Witness Nodes

Witness nodes are non-voting replicas that provide read scalability and improved fault tolerance without increasing quorum requirements.

## Overview

```
┌─────────────────────────────────────────────────────────────────┐
│                    Standard Raft (3 nodes)                       │
│                                                                  │
│     ┌────────┐     ┌────────┐     ┌────────┐                   │
│     │ Leader │     │Follower│     │Follower│                   │
│     │  VOTE  │     │  VOTE  │     │  VOTE  │                   │
│     └────────┘     └────────┘     └────────┘                   │
│                                                                  │
│     Quorum: 2/3   Fault tolerance: 1 node                       │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘

┌─────────────────────────────────────────────────────────────────┐
│               With Witness Nodes (3 voters + 2 witnesses)        │
│                                                                  │
│     ┌────────┐     ┌────────┐     ┌────────┐                   │
│     │ Leader │     │Follower│     │Follower│                   │
│     │  VOTE  │     │  VOTE  │     │  VOTE  │                   │
│     └────────┘     └────────┘     └────────┘                   │
│                                                                  │
│              ┌─────────┐     ┌─────────┐                        │
│              │ Witness │     │ Witness │                        │
│              │NO VOTE  │     │NO VOTE  │                        │
│              └─────────┘     └─────────┘                        │
│                                                                  │
│     Quorum: still 2/3   Read capacity: 5x                       │
│     Data copies: 5      Cross-region reads                      │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Use Cases

### Read Scalability

```
Without Witnesses:                    With Witnesses:

Clients                              Clients
   │                                    │
   ▼                                    ▼
┌─────────────────┐                 ┌─────────────────────────────┐
│ Leader          │◄──────────────  │ Leader    Follower Follower│
│ (serves reads)  │    All reads    │                             │
└─────────────────┘    bottleneck   │ Witness   Witness  Witness │◄─ Reads
                                    │                             │   distributed
                                    └─────────────────────────────┘
```

### Cross-Region Deployment

```
┌─────────────────────────────────────────────────────────────────┐
│                    Multi-Region Setup                            │
│                                                                  │
│  US-East (Primary)              US-West                         │
│  ┌─────────────────┐           ┌─────────────────┐              │
│  │ Leader  (VOTE)  │           │ Witness         │              │
│  │ Follower (VOTE) │◄─────────►│ (serves local   │              │
│  │ Follower (VOTE) │   sync    │  reads)         │              │
│  └─────────────────┘           └─────────────────┘              │
│                                                                  │
│  EU-West                        APAC                            │
│  ┌─────────────────┐           ┌─────────────────┐              │
│  │ Witness         │           │ Witness         │              │
│  │ (serves local   │◄─────────►│ (serves local   │              │
│  │  reads)         │   sync    │  reads)         │              │
│  └─────────────────┘           └─────────────────┘              │
│                                                                  │
│  Writes: always go to US-East (quorum region)                   │
│  Reads: served locally from nearest witness                     │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Disaster Recovery

```
Primary Datacenter                  DR Site
┌─────────────────────┐            ┌─────────────────┐
│ Leader  (VOTE)      │            │ Witness         │
│ Follower (VOTE)     │───────────►│ (async replica) │
│ Follower (VOTE)     │            │                 │
└─────────────────────┘            └─────────────────┘

Normal operation:
• Witness receives all committed entries
• No participation in elections
• Provides read-only access

During DR failover:
• Promote witness to voting member
• Adjust quorum
• Resume operations
```

---

## Configuration

### Adding Witnesses

```c
// Create witness configuration
kraftWitnessConfig witnessConfig = {
    .nodeId = 101,
    .address = "witness1.region-b.cluster",
    .port = 7001,

    // Sync settings
    .syncMode = KRAFT_WITNESS_ASYNC,     // Don't wait for witness
    .maxLag = 1000,                       // Max lag before alerting

    // Read settings
    .serveReads = true,
    .maxStaleness = 5000,                 // 5 second max staleness
};

// Add witness to cluster
kraftStateAddWitness(state, &witnessConfig);
```

### Witness Sync Modes

```c
typedef enum {
    KRAFT_WITNESS_ASYNC,     // Don't wait for witness ack
    KRAFT_WITNESS_SYNC,      // Wait for witness (increases durability)
    KRAFT_WITNESS_LAZY,      // Batch updates to witness
} kraftWitnessSyncMode;
```

### Bulk Configuration

```c
// Configure multiple witnesses
kraftWitnessConfig witnesses[] = {
    {101, "us-west.cluster", 7001, KRAFT_WITNESS_ASYNC, 1000, true, 5000},
    {102, "eu-west.cluster", 7001, KRAFT_WITNESS_ASYNC, 2000, true, 10000},
    {103, "ap-south.cluster", 7001, KRAFT_WITNESS_LAZY, 5000, true, 15000},
};

for (int i = 0; i < 3; i++) {
    kraftStateAddWitness(state, &witnesses[i]);
}
```

---

## Witness Behavior

### Replication

```
Leader                    Voting Follower           Witness
   │                           │                       │
   │  AppendEntries            │                       │
   ├──────────────────────────►│                       │
   │                           │                       │
   │◄──────────────────────────┤ Ack                   │
   │                           │                       │
   │  (quorum reached)         │                       │
   │                           │                       │
   │  AppendEntries (async)    │                       │
   ├──────────────────────────────────────────────────►│
   │                           │                       │
   │  (don't wait for ack)     │                       │
   │                           │                       │
   │                           │                       │ Ack (ignored
   │◄──────────────────────────────────────────────────┤  for commit)
```

### Elections

```c
// Witnesses don't participate in elections
bool shouldParticipateInElection(kraftNode *node) {
    return !node->isWitness;  // Witnesses return false
}

// Witnesses don't vote
bool handleRequestVote(kraftNode *node, RequestVote *rv) {
    if (node->isWitness) {
        // Ignore vote requests
        return false;
    }
    // Normal vote logic
    ...
}

// Witnesses can't become leader
bool canBecomeLeader(kraftNode *node) {
    return !node->isWitness;
}
```

### Serving Reads

```c
// Check if witness can serve a read
bool witnessCanServeRead(kraftWitness *witness, kraftReadRequest *req) {
    // Check staleness
    uint64_t lag = leaderCommitIndex - witness->appliedIndex;
    uint64_t lagTime = now() - witness->lastUpdateTime;

    if (req->consistency == KRAFT_READ_LINEARIZABLE) {
        return false;  // Witnesses can't serve linearizable reads
    }

    if (req->consistency == KRAFT_READ_BOUNDED_STALENESS) {
        return lagTime <= req->maxStaleness;
    }

    // KRAFT_READ_STALE - always can serve
    return true;
}

// Read from witness
kraftReadResult witnessRead(kraftWitness *witness, const char *key) {
    kraftReadResult result = {
        .data = stateMachineGet(witness->stateMachine, key),
        .appliedIndex = witness->appliedIndex,
        .isStale = witness->appliedIndex < leaderCommitIndex,
    };
    return result;
}
```

---

## Promotion & Demotion

### Promoting Witness to Voter

```c
// For DR failover or rebalancing
bool promoteWitness(kraftState *state, uint64_t witnessId) {
    kraftWitness *witness = findWitness(state, witnessId);
    if (!witness) return false;

    // Check witness is caught up
    if (witness->appliedIndex < state->commitIndex - 100) {
        return false;  // Too far behind
    }

    // Remove from witness list
    removeFromWitnessList(state, witnessId);

    // Add as voting member (triggers joint consensus)
    kraftIp peer = {
        .ip = witness->address,
        .port = witness->port,
        .nodeId = witnessId,
    };

    return kraftStateAddNode(state, &peer);
}
```

### Demoting Voter to Witness

```c
// Reduce quorum size while keeping replica
bool demoteToWitness(kraftState *state, uint64_t nodeId) {
    // Can't demote self if leader
    if (state->self.nodeId == nodeId && kraftStateIsLeader(state)) {
        return false;
    }

    // Remove from voters (triggers joint consensus)
    kraftStateRemoveNode(state, nodeId);

    // Add as witness
    kraftIp *peer = findPeer(state, nodeId);
    kraftWitnessConfig witnessConfig = {
        .nodeId = nodeId,
        .address = peer->ip,
        .port = peer->port,
        .syncMode = KRAFT_WITNESS_ASYNC,
    };

    return kraftStateAddWitness(state, &witnessConfig);
}
```

---

## Client Integration

### Witness-Aware Client

```c
typedef struct {
    kraftClientSession *leaderClient;      // For writes
    kraftClientSession **witnessClients;   // For reads
    size_t witnessCount;
} WitnessAwareClient;

// Submit write (always to leader)
bool submitCommand(WitnessAwareClient *client, const void *cmd, size_t len) {
    return kraftClientSubmit(client->leaderClient, cmd, len, NULL, NULL);
}

// Read with automatic routing
bool read(WitnessAwareClient *client, const char *key,
          kraftReadConsistency consistency, void **value) {

    if (consistency == KRAFT_READ_LINEARIZABLE) {
        // Must go to leader
        return kraftClientRead(client->leaderClient, key, value);
    }

    // Find nearest/healthiest witness
    kraftClientSession *best = findBestWitness(client);
    if (best) {
        return kraftClientRead(best, key, value);
    }

    // Fall back to leader
    return kraftClientRead(client->leaderClient, key, value);
}
```

### Load Balancing Reads

```c
typedef struct {
    kraftClientSession *session;
    uint64_t latency;
    uint64_t lag;
    bool healthy;
} WitnessHealth;

kraftClientSession *findBestWitness(WitnessAwareClient *client) {
    WitnessHealth *best = NULL;
    uint64_t bestScore = UINT64_MAX;

    for (size_t i = 0; i < client->witnessCount; i++) {
        WitnessHealth *w = &client->witnessHealth[i];

        if (!w->healthy) continue;

        // Score: lower is better
        // Prioritize low latency, penalize high lag
        uint64_t score = w->latency + (w->lag * 10);

        if (score < bestScore) {
            bestScore = score;
            best = w;
        }
    }

    return best ? best->session : NULL;
}
```

---

## Monitoring

### Witness Metrics

```c
typedef struct {
    // Replication
    uint64_t appliedIndex;
    uint64_t lagEntries;          // Entries behind leader
    uint64_t lagMs;               // Time behind leader

    // Performance
    uint64_t readsServed;
    uint64_t avgReadLatency;

    // Health
    bool connected;
    uint64_t lastHeartbeat;
    uint64_t reconnects;
} WitnessMetrics;

void collectWitnessMetrics(kraftState *state) {
    for (size_t i = 0; i < state->witnessCount; i++) {
        kraftWitness *w = &state->witnesses[i];
        WitnessMetrics *m = &w->metrics;

        m->lagEntries = state->commitIndex - w->appliedIndex;
        m->lagMs = now() - w->lastApplyTime;

        exportGauge("witness_lag_entries", m->lagEntries,
                    "witness_id", w->nodeId);
        exportGauge("witness_lag_ms", m->lagMs,
                    "witness_id", w->nodeId);
        exportCounter("witness_reads", m->readsServed,
                      "witness_id", w->nodeId);
    }
}
```

### Alerting

```c
void checkWitnessHealth(kraftState *state) {
    for (size_t i = 0; i < state->witnessCount; i++) {
        kraftWitness *w = &state->witnesses[i];

        // Check lag
        if (w->lagMs > w->config.maxLag) {
            alertWitnessLagging(w->nodeId, w->lagMs);
        }

        // Check connectivity
        if (now() - w->lastHeartbeat > 30000) {
            alertWitnessDisconnected(w->nodeId);
        }
    }
}
```

---

## Best Practices

### Placement

```
┌─────────────────────────────────────────────────────────────────┐
│                     Witness Placement                            │
│                                                                  │
│  DO:                                                            │
│  • Place witnesses in different failure domains than voters    │
│  • Place witnesses close to read traffic sources               │
│  • Use witnesses for cross-region read serving                 │
│                                                                  │
│  DON'T:                                                         │
│  • Place all witnesses in same region as voters                │
│  • Rely on witnesses for durability (they're async)            │
│  • Have more witnesses than voters (diminishing returns)       │
│                                                                  │
│  Recommended ratios:                                            │
│  • 3 voters + 1-2 witnesses (typical)                          │
│  • 3 voters + 3-5 witnesses (global reads)                     │
│  • 5 voters + 2 witnesses (high durability)                    │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Read Consistency

```c
// Choose appropriate consistency level
typedef enum {
    KRAFT_READ_STALE,              // Fastest, may return old data
    KRAFT_READ_BOUNDED_STALENESS,  // Configurable staleness bound
    KRAFT_READ_LINEARIZABLE,       // Strongest, requires leader
} kraftReadConsistency;

// Guidelines:
// STALE: Analytics, dashboards, approximations
// BOUNDED: User-facing reads, reasonable freshness
// LINEARIZABLE: Financial, critical state, read-after-write
```

---

## Next Steps

- [Async Commit](async-commit.md) - Durability trade-offs
- [ReadIndex](read-index.md) - Linearizable reads
- [Production Deployment](../operations/deployment.md) - Witness deployment
