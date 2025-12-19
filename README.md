# kraft: Kit (for) Raft

kraft is an implementation of Raft in C easily extensible to be used
as the basis for strongly (or, optionally, weakly) consistent distributed
systems.

The goal of kraft is: be a testable and verifiable Raft algorithm
others can easily build on top of for creating distributed consistent
server platforms.

kraft ships with a reference server and protocol, but you can easily
replace the server interface, protocol format, or persistent logging
interfaces as needed.

## Building

```bash
mkdir build && cd build
cmake ..
make
```

## Testing

kraft includes a comprehensive test suite with:

- Core Raft protocol tests
- Distributed systems fault injection tests
- Invariant checking (all 5 Raft safety properties)
- Fuzzing for stress testing
- Performance benchmarks

### Running Tests

```bash
# Build tests
make kraft-test

# Run all tests
./src/kraft-test

# Run specific test
./src/kraft-test 1  # Single node test
./src/kraft-test 2  # Initial election (3 nodes)
# etc...

# Using CTest for granular control
make test                    # Run all tests
ctest -R Kraft_Election     # Run election tests
ctest -L quick              # Run quick tests only
ctest -L fault-tolerance    # Run fault tolerance tests
```

### Fuzzing

The fuzzer performs randomized testing with node crashes, network partitions,
packet loss, and delays while verifying all Raft invariants hold.

```bash
# Build fuzzer
make kraft-fuzz

# Run fuzzer with default settings
./src/kraft-fuzz

# Custom fuzzing parameters
./src/kraft-fuzz -n 7 -o 5000 -t 120
#   -n: number of nodes (3-11)
#   -o: number of operations (100-100000)
#   -t: max time in seconds (10-3600)

# Run fuzzer tests via CTest
ctest -R Kraft_Fuzz_Quick     # Quick fuzz test
ctest -R Kraft_Fuzz_Standard  # Standard fuzz test
ctest -R Kraft_Fuzz_Stress    # Stress fuzz test
ctest -L fuzz                 # All fuzz tests
```

### Performance Benchmarking

kraft includes comprehensive performance benchmarks for measuring throughput,
latency, election speed, and scalability characteristics.

```bash
# Build benchmark suite
make kraft-bench

# Run all benchmarks
./src/kraft-bench

# Run specific benchmarks
./src/kraft-bench throughput       # Measure command commit throughput
./src/kraft-bench latency          # Measure commit latency distribution
./src/kraft-bench election         # Measure leader election speed
./src/kraft-bench scalability      # Performance vs cluster size (3-11 nodes)
./src/kraft-bench fault-tolerance  # Performance under network issues
```

Example benchmark output:

```
====================================================================
Performance Report
====================================================================
Test Duration: 1.000 seconds (1000000 us)

RPC Performance:
  Total RPCs:        8016
  Throughput:        8016 RPCs/sec

Commit Performance:
  Total Commits:     1000
  Throughput:        1000 commits/sec
  Latency (avg):     1000 us
  Latency (p50):     1000 us
  Latency (p95):     1200 us
  Latency (p99):     1500 us

Election Performance:
  Total Elections:   1
  Latency (avg):     179.1 ms

Network Performance:
  Messages Delivered:8016
  Throughput:        8016 msgs/sec
====================================================================
```

## Features

### Core Raft Protocol

- **Complete Raft Implementation**: Leader election, log replication, safety
- **Pre-Vote Protocol**: Prevents disruption from partitioned nodes rejoining
- **Leadership Transfer**: Graceful leader handoff for maintenance operations
- **ReadIndex**: Linearizable reads without log replication overhead
- **Log Compaction**: Snapshot support with configurable thresholds
- **Membership Changes**: Joint consensus for safe cluster reconfiguration

### Advanced Features

- **Witness Nodes**: Non-voting read replicas for scaling reads
  - Learner/observer mode for catching up without affecting elections
  - Promotion to full voting member when ready
  - Read serving for stale-tolerant queries

- **Async Commit Modes**: Configurable consistency/latency tradeoff
  - `STRONG`: Wait for majority acknowledgment (default Raft)
  - `LEADER_ONLY`: Acknowledge after leader persistence (lowest latency)
  - `ASYNC`: Fire-and-forget with background replication
  - `FLEXIBLE`: Per-command consistency level selection

- **Intelligent Snapshots**: Advanced log compaction
  - Automatic triggering based on log size, entry count, or time interval
  - Incremental snapshot support (delta from previous)
  - Streaming chunk-based transfer to followers
  - Parallel transfers to multiple followers
  - Configurable retention and compaction policies

### Testing Infrastructure

- **Comprehensive Testing**: Deterministic test framework with fault injection
- **Performance Profiling**: Built-in latency/throughput measurement
- **Invariant Checking**: Automatic verification of all 5 Raft safety properties
- **Fuzzing**: Randomized testing with network partitions, crashes, packet loss

## Implementation Status

| Feature                 | Status      | Tests      |
| ----------------------- | ----------- | ---------- |
| Leader Election         | ✅ Complete | 6 tests    |
| Log Replication         | ✅ Complete | 2 tests    |
| Fault Tolerance         | ✅ Complete | 3 tests    |
| Pre-Vote Protocol       | ✅ Complete | Integrated |
| Leadership Transfer     | ✅ Complete | 1 test     |
| ReadIndex               | ✅ Complete | Integrated |
| Session Tracking        | ✅ Complete | Integrated |
| Witness Nodes           | ✅ Complete | 5 tests    |
| Async Commit Modes      | ✅ Complete | 9 tests    |
| Intelligent Snapshots   | ✅ Complete | 7 tests    |
| Multi-Raft Groups       | ✅ Complete | 21 tests   |
| Configuration Profiles  | ✅ Complete | 5 tests    |
| Chaos Testing Framework | ✅ Complete | 10 tests   |
| CRDT-Aware Compaction   | ✅ Complete | 8 tests    |

**Total Tests: 77** (all passing)

## Multi-Raft Groups

### What are Multi-Raft Groups?

In a single-group Raft cluster, all data is replicated across all nodes using one
Raft consensus instance. This works well for small datasets but becomes a bottleneck
as data grows because:

1. **All writes go through one leader** - single point of throughput limitation
2. **All data on all nodes** - storage limited to smallest node
3. **Log grows unboundedly** - snapshot/compaction for entire dataset

**Multi-Raft** solves this by partitioning data across multiple independent Raft
consensus groups, each responsible for a subset of the data (typically a key range).

```
Single-Raft:                    Multi-Raft:
┌─────────────────────┐        ┌──────────┐ ┌──────────┐ ┌──────────┐
│  All Data (1 Group) │        │ Range A  │ │ Range B  │ │ Range C  │
│  Leader: Node 1     │   →    │ Group 1  │ │ Group 2  │ │ Group 3  │
│  Nodes: 1,2,3,4,5   │        │ L:N1     │ │ L:N2     │ │ L:N3     │
└─────────────────────┘        └──────────┘ └──────────┘ └──────────┘
```

Each group:

- Has its own leader election
- Maintains its own Raft log
- Can be on different nodes (leadership spread across cluster)
- Operates completely independently from other groups

### Use Cases

| Use Case                     | Description                                       |
| ---------------------------- | ------------------------------------------------- |
| **Distributed Databases**    | CockroachDB, TiKV partition data by key ranges    |
| **Distributed Queues**       | Partition message queues by topic or partition ID |
| **Sharded Key-Value Stores** | Each shard is a separate Raft group               |
| **Multi-Tenant Systems**     | Each tenant gets isolated consensus group         |

### API Overview

```c
#include "multiRaft.h"

/* Create the Multi-Raft manager */
kraftMultiRaft mr;
kraftMultiRaftConfig config = kraftMultiRaftConfigDefault();
config.maxGroups = 1024;  /* Maximum groups this node can host */
kraftMultiRaftInit(&mr, &config, localNodeId);

/* Create a new Raft group for a key range */
kraftReplicaId replicas[] = {1, 2, 3, 4, 5};
kraftGroup *group = kraftMultiRaftCreateGroupWithRange(
    &mr,
    groupId,           /* Unique group identifier */
    replicas, 5,       /* Replica set for this group */
    startKey, startLen,/* Start of key range (inclusive) */
    endKey, endLen     /* End of key range (exclusive) */
);

/* Route a command to the appropriate group */
kraftMultiRaftSubmitCommandForKey(&mr, key, keyLen, command, cmdLen);

/* Tick all groups (call periodically) */
kraftMultiRaftTick(&mr, currentTimeUs);

/* Split a group when it gets too large */
kraftGroupId leftId, rightId;
kraftMultiRaftSplitGroup(&mr, groupId, splitKey, splitLen, &leftId, &rightId);

/* Merge adjacent groups when they shrink */
kraftMultiRaftMergeGroups(&mr, leftGroupId, rightGroupId, &mergedId);
```

### Configuration

| Parameter             | Default         | Description                                |
| --------------------- | --------------- | ------------------------------------------ |
| `maxGroups`           | 1024            | Maximum groups per node (production limit) |
| `initialCapacity`     | 16              | Initial hash table capacity                |
| `tickIntervalUs`      | 10,000 (10ms)   | How often to tick groups                   |
| `electionTimeoutUs`   | 150,000 (150ms) | Default election timeout for new groups    |
| `heartbeatIntervalUs` | 50,000 (50ms)   | Default heartbeat interval                 |
| `maxBatchSize`        | 100             | Max messages to batch per tick             |
| `enableLoadBalancing` | true            | Auto-spread leadership across nodes        |

### Multi-Raft Fuzzer

The Multi-Raft fuzzer tests multiple independent Raft groups running actual
consensus under randomized fault conditions:

```bash
# Build Multi-Raft fuzzer
make kraft-fuzz-multi

# Run with default settings (3 groups x 5 nodes = 15 total nodes)
./src/kraft-fuzz-multi

# Custom configuration
./src/kraft-fuzz-multi -g 10 -n 7 -o 100000 -t 3600
#   -g: number of Raft groups (1-64 for testing)
#   -n: nodes per group (3-15)
#   -o: max operations (100-1000000)
#   -t: max time in seconds

# Large scale test: 10 groups x 7 nodes = 70 total Raft nodes
./src/kraft-fuzz-multi -g 10 -n 7 -o 50000 -t 1800

# Reproduce a failure
./src/kraft-fuzz-multi -s 12345 -g 5 -n 5
```

**Why 64 groups in fuzzer?** The fuzzer limit of 64 groups is a practical limit
for testing - running 64 groups x 15 nodes = 960 simulated Raft nodes is already
substantial. The production API supports up to `maxGroups` (default 1024) groups
per process.

**Operations tested by the fuzzer:**

| Operation          | Description                                   |
| ------------------ | --------------------------------------------- |
| `SUBMIT_COMMAND`   | Submit commands through actual Raft consensus |
| `NODE_CRASH`       | Crash nodes within a group                    |
| `NODE_RECOVER`     | Recover crashed nodes                         |
| `PARTITION_GROUP`  | Network partition within a single group       |
| `HEAL_GROUP`       | Heal a group's network partition              |
| `TICK_GROUP`       | Advance time for a specific group             |
| `GLOBAL_PARTITION` | Partition ALL groups simultaneously           |
| `GLOBAL_HEAL`      | Heal all partitions across all groups         |
| `TICK_ALL`         | Advance time for all groups                   |

**Invariants checked per group:**

- Election Safety (at most one leader per term)
- Log Matching (identical prefixes have identical entries)
- Leader Completeness (committed entries present in future leaders)

### Architecture

```
┌─────────────────────────────────────────────────────────────────────┐
│                        kraftMultiRaft                                │
│  ┌──────────────────────────────────────────────────────────────┐   │
│  │                    Group Registry                             │   │
│  │  ┌─────────┐ ┌─────────┐ ┌─────────┐ ┌─────────┐            │   │
│  │  │ Group 1 │ │ Group 2 │ │ Group 3 │ │ Group N │            │   │
│  │  │ [A-M)   │ │ [M-Z)   │ │ [0-9)   │ │ ...     │            │   │
│  │  │ L:Node1 │ │ L:Node3 │ │ L:Node2 │ │         │            │   │
│  │  └────┬────┘ └────┬────┘ └────┬────┘ └─────────┘            │   │
│  │       │           │           │                              │   │
│  │       ▼           ▼           ▼                              │   │
│  │  ┌─────────────────────────────────────────────────────────┐ │   │
│  │  │              Hash Table: groupId → kraftGroup*          │ │   │
│  │  └─────────────────────────────────────────────────────────┘ │   │
│  └──────────────────────────────────────────────────────────────┘   │
│                                                                      │
│  Each kraftGroup contains:                                          │
│  - kraftState *raft (full Raft state machine)                       │
│  - kraftGroupDescriptor (key range, replicas, state)                │
│  - Statistics (messages, leader changes, entries applied)           │
└─────────────────────────────────────────────────────────────────────┘
```

### Example: Distributed Key-Value Store

```c
/* Initialize Multi-Raft manager */
kraftMultiRaft mr;
kraftMultiRaftConfig config = kraftMultiRaftConfigDefault();
kraftMultiRaftInit(&mr, &config, myNodeId);

/* Create initial groups covering the key space */
/* Group 1: keys [0x00..., 0x55...) */
kraftMultiRaftCreateGroupWithRange(&mr, 1, replicas, 5,
    "\x00", 1, "\x55", 1);

/* Group 2: keys [0x55..., 0xAA...) */
kraftMultiRaftCreateGroupWithRange(&mr, 2, replicas, 5,
    "\x55", 1, "\xAA", 1);

/* Group 3: keys [0xAA..., 0xFF...) */
kraftMultiRaftCreateGroupWithRange(&mr, 3, replicas, 5,
    "\xAA", 1, "\xFF", 1);

/* Handle a PUT request */
void handlePut(uint8_t *key, size_t keyLen, uint8_t *value, size_t valueLen) {
    /* Route to correct group automatically */
    kraftMultiRaftSubmitCommandForKey(&mr, key, keyLen,
        makeCommand(PUT, key, keyLen, value, valueLen),
        commandLen);
}

/* Periodically tick all groups */
while (running) {
    kraftMultiRaftTick(&mr, getCurrentTimeUs());
    usleep(10000);  /* 10ms */
}
```

## Chaos Testing Framework

The chaos testing framework provides comprehensive fault injection capabilities for
testing Raft consensus under adversarial network and node conditions.

### Features

- **Network Chaos**: Packet drops, delays, duplicates, reordering, and corruption
- **Partition Chaos**: Symmetric and asymmetric network partitions
- **Node Chaos**: Crashes, pauses, slow nodes, and Byzantine behavior
- **Clock Chaos**: Clock drift, jumps, and desynchronization
- **Byzantine Fault Tolerance**: Simulated Byzantine nodes with various attack patterns

### Usage

```bash
# Build chaos fuzzer
make kraft-fuzz-chaos

# Run with default settings (mixed chaos)
./src/kraft-fuzz-chaos

# Run a specific chaos scenario
./src/kraft-fuzz-chaos --scenario partition    # Network partition testing
./src/kraft-fuzz-chaos --scenario storm        # Partition storm testing
./src/kraft-fuzz-chaos --scenario byzantine    # Byzantine fault testing
./src/kraft-fuzz-chaos --scenario hellweek     # Maximum adversarial conditions

# Custom parameters
./src/kraft-fuzz-chaos -n 7 -o 10000 --scenario mixed --verbose
#   -n: number of nodes (3-11)
#   -o: max operations
#   --scenario: chaos scenario to use
#   --verbose: detailed progress output
```

### Chaos Scenarios

| Scenario          | Description                                               |
| ----------------- | --------------------------------------------------------- |
| `none`            | Baseline - no chaos (validates normal operation)          |
| `flaky`           | Flaky network: 15% packet loss, occasional delays         |
| `delay`           | High latency: 50-500ms delays on 80% of messages          |
| `partition`       | Frequent partitions: 5% chance of partition per operation |
| `storm`           | Partition storm: rapid partition changes, asymmetric      |
| `byzantine`       | Single Byzantine node: equivocation, lie votes            |
| `byzantine-multi` | Multiple Byzantine nodes: coordinated attacks             |
| `clock`           | Clock chaos: drift up to 20%, occasional jumps            |
| `gc`              | Simulated GC pauses: 100-500ms stop-the-world             |
| `mixed`           | Mix of all fault types (default)                          |
| `hellweek`        | Maximum adversarial: all chaos at high intensity          |

### Byzantine Behaviors

When using `--byzantine N` flag, the fuzzer simulates N Byzantine (malicious) nodes:

- **Equivocation**: Send conflicting messages to different nodes
- **Lie Votes**: Grant votes to multiple candidates in same term
- **Fake Leader**: Claim leadership without winning election
- **Corrupt Log**: Send incorrect log entries to followers
- **Silent Drop**: Selectively drop messages to create inconsistency

Example:

```bash
# Enable 2 Byzantine nodes with rotation
./src/kraft-fuzz-chaos --byzantine 2 --rotate --scenario mixed
```

### Programmatic API

```c
#include "chaos.h"

/* Create chaos engine */
kraftChaosEngine *chaos = kraftChaosEngineNew();

/* Configure network faults */
kraftChaosSetDropProbability(chaos, 0.1);    /* 10% packet loss */
kraftChaosSetDelay(chaos, 0.2, 10, 100);     /* 20% delayed 10-100ms */

/* Create a network partition */
uint32_t isolated[] = {2, 3};
kraftChaosCreatePartition(chaos, isolated, 2, 5000); /* isolate nodes 2,3 for 5s */

/* Simulate node failures */
kraftChaosCrashNode(chaos, 1);     /* Crash node 1 */
kraftChaosPauseNode(chaos, 2, 500); /* Pause node 2 for 500ms */

/* Configure Byzantine behavior */
kraftChaosSetByzantineNode(chaos, 3, KRAFT_BYZANTINE_EQUIVOCATE);

/* Enable chaos and tick */
kraftChaosSetEnabled(chaos, true);
kraftChaosTick(chaos, currentTimeMs);

/* Get statistics */
kraftChaosStats stats;
kraftChaosGetStats(chaos, &stats);
```

## CRDT System

kraft includes a complete CRDT (Conflict-free Replicated Data Type) implementation
for eventual consistency operations that can be merged without coordination.

### Supported CRDT Types

| Type             | Description                                 | Use Case                             |
| ---------------- | ------------------------------------------- | ------------------------------------ |
| **LWW Register** | Last-Writer-Wins register with vector clock | Single-value storage                 |
| **G-Counter**    | Grow-only counter                           | Distributed counters (views, clicks) |
| **PN-Counter**   | Positive-Negative counter                   | Counters with increment/decrement    |
| **G-Set**        | Grow-only set                               | Add-only collections (likes, tags)   |
| **OR-Set**       | Observed-Remove set                         | Sets with add and remove operations  |

### Mathematical Properties

All CRDT types satisfy these properties (verified by the CRDT fuzzer):

1. **Commutativity**: `merge(a, b) == merge(b, a)`
2. **Associativity**: `merge(merge(a, b), c) == merge(a, merge(b, c))`
3. **Idempotence**: `merge(a, a) == a`
4. **Monotonicity**: Values only grow (for counters/sets)
5. **Convergence**: All replicas eventually reach same state

### Usage

```bash
# Build CRDT fuzzer
make kraft-fuzz-crdt

# Run property-based tests on all CRDT types
./src/kraft-fuzz-crdt

# Test specific CRDT type
./src/kraft-fuzz-crdt --type gcounter
./src/kraft-fuzz-crdt --type orset

# Test specific property
./src/kraft-fuzz-crdt --property comm     # Commutativity
./src/kraft-fuzz-crdt --property conv     # Convergence

# Intensive testing
./src/kraft-fuzz-crdt -o 10000 --type pncounter --verbose
```

### Programmatic API

```c
#include "crdt.h"

/* G-Counter example */
kraftCrdt *counter = kraftCrdtNew(KRAFT_CRDT_G_COUNTER, nodeId);
kraftCrdtGCounterIncrement(counter, 5);

/* Get local value */
int64_t value = kraftCrdtGCounterValue(counter);

/* Merge with remote replica */
kraftCrdtMerge(counter, remoteCounter);

/* PN-Counter for increment/decrement */
kraftCrdt *pn = kraftCrdtNew(KRAFT_CRDT_PN_COUNTER, nodeId);
kraftCrdtPNCounterIncrement(pn, 10);
kraftCrdtPNCounterDecrement(pn, 3);
/* Value is now 7 */

/* OR-Set for sets with removal */
kraftCrdt *set = kraftCrdtNew(KRAFT_CRDT_OR_SET, nodeId);
kraftCrdtORSetAdd(set, "item1", 5);
kraftCrdtORSetAdd(set, "item2", 5);
kraftCrdtORSetRemove(set, "item1", 5);
/* Set contains only "item2" */

/* Serialize for network transfer */
uint8_t *data;
size_t len;
kraftCrdtSerialize(set, &data, &len);

/* Deserialize received CRDT */
kraftCrdt *received = kraftCrdtDeserialize(data, len);
kraftCrdtMerge(set, received);
```

### CRDT-Aware Log Compaction

kraft's snapshot system is CRDT-aware, allowing efficient compaction of CRDT
state without losing convergence guarantees:

```c
/* Configure CRDT-aware compaction */
kraftSnapshotConfig config = kraftSnapshotConfigDefault();
config.crdtAware = true;
config.crdtMergeBeforeSnapshot = true;

/* CRDT state is automatically merged during compaction */
kraftSetSnapshotConfig(raft, &config);
```

### Integration with Raft

CRDTs can be used with Raft for different consistency models:

- **Strong Consistency**: Replicate CRDT operations through Raft log
- **Eventual Consistency**: Gossip CRDT state between nodes, merge on receipt
- **Hybrid**: Use Raft for critical operations, CRDTs for high-frequency updates

```c
/* Replicate CRDT operation through Raft */
kraftCrdtOp op = kraftCrdtMakeIncrementOp(counter, 1);
kraftSubmitCommand(raft, &op, sizeof(op));

/* Or use async mode for eventual consistency */
kraftAsyncBroadcastCrdtState(node, counter);
```
