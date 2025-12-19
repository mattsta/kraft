# Kraft Architecture

This document describes the architecture of Kraft, a production-ready Raft consensus implementation with pluggable networking.

## System Overview

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                            APPLICATION LAYER                                 │
│                                                                              │
│  Your state machine, business logic, command handlers, queries              │
│                                                                              │
└──────────────────────────────────┬──────────────────────────────────────────┘
                                   │
                                   ▼
┌─────────────────────────────────────────────────────────────────────────────┐
│                           KRAFT CLIENT API                                   │
│                                                                              │
│  kraftClientAPI.h                                                            │
│  ├── Command submission (sync/async)                                        │
│  ├── Linearizable reads (ReadIndex)                                         │
│  ├── Leader discovery & redirect handling                                   │
│  ├── Request batching & pipelining                                          │
│  └── Session management (exactly-once semantics)                            │
│                                                                              │
└──────────────────────────────────┬──────────────────────────────────────────┘
                                   │
         ┌─────────────────────────┼─────────────────────────┐
         │                         │                         │
         ▼                         ▼                         ▼
┌─────────────────┐    ┌─────────────────────┐    ┌─────────────────────┐
│    ADAPTER      │    │     KRAFT CORE      │    │      PROTOCOL       │
│                 │    │                     │    │                     │
│ kraftAdapter.h  │    │  kraft.h            │    │  kraftProtocol.h    │
│                 │    │  rpc.c              │    │                     │
│ ┌─────────────┐ │    │  ┌───────────────┐  │    │  ┌───────────────┐  │
│ │ Connections │ │    │  │ Leader        │  │    │  │ Encode        │  │
│ │ Timers      │ │    │  │ Election      │  │    │  │ AppendEntries │  │
│ │ Event Loop  │ │    │  ├───────────────┤  │    │  │ RequestVote   │  │
│ │ Async I/O   │ │    │  │ Log           │  │    │  │ InstallSnap   │  │
│ └─────────────┘ │    │  │ Replication   │  │    │  ├───────────────┤  │
│                 │    │  ├───────────────┤  │    │  │ Decode        │  │
│ Implementations:│    │  │ Commit &      │  │    │  │ All Messages  │  │
│ • libuv         │    │  │ Apply         │  │    │  └───────────────┘  │
│ • epoll         │    │  ├───────────────┤  │    │                     │
│ • kqueue        │    │  │ Membership    │  │    │  Implementations:   │
│ • io_uring      │    │  │ Changes       │  │    │  • Binary (fast)    │
│ • poll/select   │    │  └───────────────┘  │    │  • Protobuf         │
│                 │    │                     │    │  • MsgPack          │
└────────┬────────┘    └──────────┬──────────┘    │  • JSON (debug)     │
         │                        │               └──────────┬──────────┘
         │                        │                          │
         ▼                        ▼                          ▼
┌─────────────────┐    ┌─────────────────────┐    ┌─────────────────────┐
│                 │    │                     │    │                     │
│  OS Network     │    │  Log Storage        │    │  Wire Format        │
│  TCP/UDP        │    │  (kvidxkit)         │    │  (Bytes)            │
│                 │    │                     │    │                     │
└─────────────────┘    └─────────────────────┘    └─────────────────────┘
```

## Core Components

### 1. Kraft State (`kraftState`)

The central Raft state machine containing:

| Field                   | Description                                   |
| ----------------------- | --------------------------------------------- |
| `self`                  | This node's identity and consensus state      |
| `leader`                | Current known leader                          |
| `nodesOld` / `nodesNew` | Cluster membership (supports joint consensus) |
| `commitIndex`           | Highest committed log index                   |
| `lastApplied`           | Highest applied log index                     |
| `log`                   | Persistent log storage (kvidxkit)             |
| `vote`                  | Voting state for elections                    |

```c
// Key state transitions
KRAFT_FOLLOWER     // Passive, responds to leader
    │
    ▼ (election timeout)
KRAFT_PRE_CANDIDATE // Pre-vote phase (optional)
    │
    ▼ (pre-vote granted)
KRAFT_CANDIDATE    // Requesting votes
    │
    ▼ (majority votes)
KRAFT_LEADER       // Active, manages cluster
```

### 2. Network Adapter (`kraftAdapter`)

Abstracts network I/O and timers. Implementations handle:

- **Connections**: Accept, connect, read, write, close
- **Timers**: Election timeout, heartbeat, maintenance
- **Event Loop**: Run, stop, poll

```c
typedef struct kraftAdapterOps {
    // Lifecycle
    bool (*init)(kraftAdapter *adapter);
    void (*destroy)(kraftAdapter *adapter);

    // Server
    bool (*listen)(kraftAdapter *adapter, const char *addr, uint16_t port, ...);
    void (*run)(kraftAdapter *adapter);
    void (*stop)(kraftAdapter *adapter);

    // Connections
    kraftConnection *(*connect)(kraftAdapter *adapter, const char *addr, ...);
    bool (*write)(kraftAdapter *adapter, kraftConnection *conn, ...);
    void (*close)(kraftAdapter *adapter, kraftConnection *conn);

    // Timers
    kraftTimer *(*timerCreate)(kraftAdapter *adapter, ...);
    bool (*timerStart)(kraftAdapter *adapter, kraftTimer *timer, ...);
    void (*timerStop)(kraftAdapter *adapter, kraftTimer *timer);

    // Utilities
    uint64_t (*now)(kraftAdapter *adapter);
} kraftAdapterOps;
```

### 3. Protocol (`kraftProtocol`)

Handles message serialization:

```c
typedef struct kraftProtocolOps {
    // Encoding
    bool (*encodeAppendEntries)(kraftProtocol *proto, kraftEncodeBuffer *buf, ...);
    bool (*encodeRequestVote)(kraftProtocol *proto, kraftEncodeBuffer *buf, ...);
    bool (*encodeInstallSnapshot)(kraftProtocol *proto, kraftEncodeBuffer *buf, ...);
    // ... more encode functions

    // Decoding
    kraftDecodeStatus (*decode)(kraftProtocol *proto, const uint8_t *data, ...);
} kraftProtocolOps;
```

### 4. Client API (`kraftClientSession`)

High-level client interface:

```c
// Command submission
kraftClientSubmit()      // Async with callback
kraftClientSubmitSync()  // Blocking

// Linearizable reads
kraftClientRead()        // Async
kraftClientReadSync()    // Blocking

// Leader management
kraftClientGetLeader()
kraftClientRefreshLeader()

// Batching
kraftClientBatchBegin()
kraftClientBatchEnd()
```

---

## Message Flow

### Write Path (Command Submission)

```
Client                    Leader                    Followers
   │                         │                          │
   │ ──── ClientRequest ───► │                          │
   │                         │                          │
   │                         │ ──── AppendEntries ────► │
   │                         │ ◄─── AppendEntriesResp ─ │
   │                         │                          │
   │                         │ (wait for majority)      │
   │                         │                          │
   │ ◄── ClientResponse ──── │                          │
   │    (committed index)    │                          │
   │                         │                          │
   │                         │ ──── AppendEntries ────► │
   │                         │    (leaderCommit updated)│
```

### Read Path (ReadIndex)

```
Client                    Leader                    Followers
   │                         │                          │
   │ ──── ReadIndex ───────► │                          │
   │                         │                          │
   │                         │ ──── Heartbeat ────────► │
   │                         │ ◄─── HeartbeatResp ───── │
   │                         │                          │
   │                         │ (confirm still leader)   │
   │                         │                          │
   │ ◄── ReadIndexResp ───── │                          │
   │    (safe read index)    │                          │
   │                         │                          │
   │    (read from state     │                          │
   │     machine locally)    │                          │
```

### Leader Election

```
Follower                  Candidate                 Other Nodes
   │                          │                          │
   │ (election timeout)       │                          │
   │                          │                          │
   ├─────────────────────────►│                          │
   │  (become PRE_CANDIDATE)  │                          │
   │                          │                          │
   │                          │ ──── RequestPreVote ───► │
   │                          │ ◄─── PreVoteResp ─────── │
   │                          │                          │
   │                          │ (if majority pre-vote)   │
   │                          │                          │
   ├─────────────────────────►│                          │
   │  (become CANDIDATE)      │                          │
   │                          │                          │
   │                          │ ──── RequestVote ──────► │
   │                          │ ◄─── VoteResp ────────── │
   │                          │                          │
   │                          │ (if majority vote)       │
   │                          │                          │
   ├─────────────────────────►│                          │
   │  (become LEADER)         │                          │
```

---

## Log Storage

Kraft uses **kvidxkit** for persistent log storage:

```c
// Log structure
┌─────────────────────────────────────────────────────────────┐
│ Index │ Term │ Command │ Data                               │
├───────┼──────┼─────────┼─────────────────────────────────────┤
│   1   │  1   │  101    │ "SET key1 value1"                   │
│   2   │  1   │  101    │ "SET key2 value2"                   │
│   3   │  2   │  102    │ <config change: add node4>          │
│   4   │  2   │  101    │ "DELETE key1"                       │
│  ...  │ ...  │  ...    │ ...                                 │
└───────┴──────┴─────────┴─────────────────────────────────────┘
```

### Command Types

| Command                                   | Value | Description                |
| ----------------------------------------- | ----- | -------------------------- |
| `KRAFT_STORAGE_CMD_APPEND_ENTRIES`        | 100   | Normal client commands     |
| `KRAFT_STORAGE_CMD_JOINT_CONSENSUS_START` | 101   | Begin membership change    |
| `KRAFT_STORAGE_CMD_JOINT_CONSENSUS_END`   | 102   | Complete membership change |

---

## Advanced Features Architecture

### Multi-Raft Groups

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                         kraftMultiRaftManager                                │
│                                                                              │
│   ┌───────────────┐  ┌───────────────┐  ┌───────────────┐                   │
│   │   Group 1     │  │   Group 2     │  │   Group 3     │                   │
│   │ (keys: a-m)   │  │ (keys: n-z)   │  │ (tenant: X)   │                   │
│   │               │  │               │  │               │                   │
│   │ kraftState    │  │ kraftState    │  │ kraftState    │                   │
│   │ ├─ log        │  │ ├─ log        │  │ ├─ log        │                   │
│   │ ├─ members    │  │ ├─ members    │  │ ├─ members    │                   │
│   │ └─ consensus  │  │ └─ consensus  │  │ └─ consensus  │                   │
│   └───────────────┘  └───────────────┘  └───────────────┘                   │
│                                                                              │
│   Key Router: hash(key) → groupId                                           │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

### Snapshot & Compaction

```
┌─────────────────────────────────────────┐
│              Log (growing)              │
│  ┌─────┬─────┬─────┬─────┬─────┬─────┐  │
│  │  1  │  2  │  3  │  4  │  5  │ ... │  │
│  └─────┴─────┴─────┴─────┴─────┴─────┘  │
└─────────────────────────────────────────┘
                    │
                    │ Snapshot at index 3
                    ▼
┌─────────────────────────────────────────┐
│  ┌──────────────────┐  ┌─────┬─────┐   │
│  │    Snapshot      │  │  4  │  5  │   │
│  │  (state @ idx 3) │  └─────┴─────┘   │
│  └──────────────────┘                   │
│  lastIncludedIndex=3                    │
│  lastIncludedTerm=1                     │
└─────────────────────────────────────────┘
```

### CRDT-Aware Compaction

```
┌─────────────────────────────────────────────────────────────────────────────┐
│                           CRDT Manager                                       │
│                                                                              │
│   Key Pattern → CRDT Type                                                   │
│   ──────────────────────                                                    │
│   "counter:*"  → G-Counter    (merge: sum all increments)                   │
│   "profile:*"  → LWW-Register (merge: keep latest timestamp)               │
│   "tags:*"     → G-Set        (merge: union of all additions)              │
│   "cart:*"     → OR-Set       (merge: track add/remove pairs)              │
│                                                                              │
│   Compaction: Multiple entries for same CRDT key → Single merged entry     │
│                                                                              │
└─────────────────────────────────────────────────────────────────────────────┘
```

---

## Thread Model

Kraft is **single-threaded** by design:

1. All Raft operations run in the adapter's event loop thread
2. Callbacks are invoked from the event loop thread
3. `kraftAdapterStop()` is the only thread-safe operation

For multi-threaded applications:

- Use one kraft instance per thread, OR
- Protect kraft operations with external mutex, OR
- Use async message passing to kraft thread

---

## Memory Model

### Ownership Rules

| Object            | Owner       | Lifecycle                                                               |
| ----------------- | ----------- | ----------------------------------------------------------------------- |
| `kraftState`      | Application | Create with `kraftStateNew()`, free with `kraftStateFree()`             |
| `kraftAdapter`    | Application | Create with `kraftAdapterCreate()`, free with `kraftAdapterDestroy()`   |
| `kraftProtocol`   | Application | Create with `kraftProtocolCreate()`, free with `kraftProtocolDestroy()` |
| `kraftConnection` | Adapter     | Created on accept/connect, freed on close                               |
| `kraftTimer`      | Adapter     | Created with `timerCreate()`, freed with `timerDestroy()`               |
| Log entries       | kvidxkit    | Managed by log storage                                                  |

### Zero-Copy Design

- Incoming data buffers are passed directly to protocol decoder
- Protocol decoder returns pointers into buffer (no copy)
- Application must process before returning from callback

---

## Configuration Hierarchy

```
kraftState
├── config
│   ├── enablePreVote          (Pre-vote protocol)
│   ├── enableLeadershipTransfer
│   ├── enableReadIndex
│   └── testMode
├── snapshot
│   ├── lastIncludedIndex
│   ├── lastIncludedTerm
│   └── triggerThreshold
├── transfer
│   └── (leadership transfer state)
├── readIndex
│   └── (ReadIndex protocol state)
├── metrics
│   └── (counters, histograms)
├── witnesses
│   └── (non-voting replicas)
├── asyncCommit
│   └── (durability modes)
└── snapshots
    └── (snapshot manager)
```

---

## Next Steps

- [Pluggable Networking](concepts/pluggable-networking.md) - Deep dive into adapters and protocols
- [Custom Adapters Guide](guides/custom-adapters.md) - Implement your own adapter
- [Production Deployment](operations/deployment.md) - Production configuration
