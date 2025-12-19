# Raft Fundamentals

This document covers the core Raft consensus algorithm as implemented in Kraft.

## What is Raft?

Raft is a consensus algorithm that enables a cluster of machines to maintain a consistent, replicated log even when some machines fail. It provides:

- **Consensus**: Agreement on log contents across nodes
- **Fault Tolerance**: Continued operation despite minority failures
- **Strong Consistency**: Linearizable reads and writes
- **Safety**: Never returns incorrect results

---

## Key Concepts

### Replicated State Machine

```
┌─────────────────────────────────────────────────────────────────┐
│                     Replicated State Machine                     │
│                                                                  │
│  Client                                                          │
│    │                                                             │
│    │  Command                                                    │
│    ▼                                                             │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │                    Consensus Module                      │    │
│  │                         (Raft)                           │    │
│  │  ┌─────┬─────┬─────┬─────┬─────┬─────┐                  │    │
│  │  │  1  │  2  │  3  │  4  │  5  │ ... │  Log             │    │
│  │  └─────┴─────┴─────┴─────┴─────┴─────┘                  │    │
│  └────────────────────────┬────────────────────────────────┘    │
│                           │                                      │
│                           │  Apply committed entries             │
│                           ▼                                      │
│  ┌─────────────────────────────────────────────────────────┐    │
│  │                    State Machine                         │    │
│  │                 (Your Application)                       │    │
│  └─────────────────────────────────────────────────────────┘    │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Terms

Raft divides time into **terms**:

```
Term 1          Term 2          Term 3          Term 4
│   Election    │   Election    │   Election    │
│   ↓           │   ↓           │   ↓           │
├───┬───────────┼───┬───────────┼───────────────┼───┬─────────
│   │  Normal   │   │  Normal   │  No leader    │   │  Normal
│   │  operation│   │  operation│  (split vote) │   │  operation
│   │           │   │           │               │   │
time ─────────────────────────────────────────────────────────►
```

- Terms are numbered with consecutive integers
- Each term begins with an election
- A term may have zero or one leader
- Terms act as a logical clock

### Node States

```
                    ┌──────────────────┐
                    │                  │
                    │    FOLLOWER      │◄─────────────────────┐
                    │                  │                      │
                    └────────┬─────────┘                      │
                             │                                │
              election       │                                │
              timeout        │                                │
                             ▼                                │
                    ┌──────────────────┐                      │
         ┌──────────│                  │                      │
         │          │  PRE_CANDIDATE   │──────────────────────┤
         │          │   (optional)     │  higher term         │
         │          └────────┬─────────┘  discovered          │
         │                   │                                │
         │    pre-votes      │                                │
         │    granted        │                                │
         │                   ▼                                │
         │          ┌──────────────────┐                      │
         │          │                  │                      │
         └──────────│   CANDIDATE      │──────────────────────┤
                    │                  │  higher term         │
                    └────────┬─────────┘  discovered          │
                             │                                │
              votes          │                                │
              received       │                                │
                             ▼                                │
                    ┌──────────────────┐                      │
                    │                  │                      │
                    │     LEADER       │──────────────────────┘
                    │                  │  higher term discovered
                    └──────────────────┘
```

---

## Leader Election

### Election Process

1. **Timeout**: Follower times out waiting for heartbeat
2. **Pre-Vote** (optional): Request pre-votes without incrementing term
3. **Candidate**: Increment term, vote for self, request votes
4. **Leader**: Receive majority votes, begin leading

### RequestVote RPC

```c
// Request
typedef struct {
    uint64_t term;           // Candidate's term
    uint64_t candidateId;    // Candidate requesting vote
    uint64_t lastLogIndex;   // Index of candidate's last log entry
    uint64_t lastLogTerm;    // Term of candidate's last log entry
} RequestVoteArgs;

// Response
typedef struct {
    uint64_t term;           // Current term, for candidate to update
    bool voteGranted;        // True if vote granted
} RequestVoteResponse;
```

### Vote Granting Rules

A node grants a vote if ALL conditions are met:

1. Candidate's term ≥ current term
2. Haven't voted for another candidate this term (or voted for this candidate)
3. Candidate's log is at least as up-to-date as receiver's log

### Log Comparison

```c
// Log A is "at least as up-to-date" as Log B if:
bool isAtLeastAsUpToDate(Log A, Log B) {
    if (A.lastTerm > B.lastTerm) return true;
    if (A.lastTerm < B.lastTerm) return false;
    return A.lastIndex >= B.lastIndex;
}
```

### Pre-Vote Protocol

Pre-vote prevents disruptions from partitioned nodes:

```
Normal Election (can disrupt):
┌─────────────────────────────────────────────────────────────────┐
│  Partitioned node times out, increments term, rejoins           │
│  → Forces all nodes to higher term                              │
│  → Causes unnecessary election                                  │
└─────────────────────────────────────────────────────────────────┘

With Pre-Vote:
┌─────────────────────────────────────────────────────────────────┐
│  Partitioned node times out, sends PreVote (doesn't increment)  │
│  → Other nodes reject (they have leader)                        │
│  → Node remains at old term, no disruption                      │
└─────────────────────────────────────────────────────────────────┘
```

---

## Log Replication

### Log Structure

```
┌─────────────────────────────────────────────────────────────────┐
│                            Log                                   │
├───────┬───────┬───────┬───────┬───────┬───────┬───────┬────────┤
│ Index │   1   │   2   │   3   │   4   │   5   │   6   │   7    │
├───────┼───────┼───────┼───────┼───────┼───────┼───────┼────────┤
│ Term  │   1   │   1   │   1   │   2   │   2   │   2   │   3    │
├───────┼───────┼───────┼───────┼───────┼───────┼───────┼────────┤
│ Cmd   │ SET x │ SET y │ DEL z │ SET a │ SET b │ DEL x │ SET c  │
└───────┴───────┴───────┴───────┴───────┴───────┴───────┴────────┘
                                          ▲             ▲
                                     committed      lastIndex
```

### AppendEntries RPC

```c
// Request (Leader → Follower)
typedef struct {
    uint64_t term;           // Leader's term
    uint64_t leaderId;       // So follower can redirect clients
    uint64_t prevLogIndex;   // Index of log entry before new ones
    uint64_t prevLogTerm;    // Term of prevLogIndex entry
    LogEntry *entries;       // Log entries to store (empty for heartbeat)
    uint32_t entriesCount;
    uint64_t leaderCommit;   // Leader's commitIndex
} AppendEntriesArgs;

// Response
typedef struct {
    uint64_t term;           // Current term, for leader to update
    bool success;            // True if follower matched prevLog
    // Optimization fields for faster recovery:
    uint64_t conflictIndex;  // First index with conflicting term
    uint64_t conflictTerm;   // Term of conflicting entry
} AppendEntriesResponse;
```

### Replication Flow

```
Leader                              Follower
   │                                    │
   │  AppendEntries(prevIdx=5, entries) │
   ├───────────────────────────────────►│
   │                                    │ Check: log[5].term == prevLogTerm?
   │                                    │
   │              success=true          │
   │◄───────────────────────────────────┤ If yes: append entries
   │                                    │
   │  (Update matchIndex, nextIndex)    │
   │                                    │
```

### Conflict Resolution

When AppendEntries fails:

```
Leader's log:   [1:1] [2:1] [3:2] [4:2] [5:2]
Follower's log: [1:1] [2:1] [3:3] [4:3]       ← Conflict at index 3
                            ↑
                            Different term

Resolution:
1. Follower rejects: success=false, conflictTerm=3, conflictIndex=3
2. Leader searches for conflictTerm in its log
3. Leader decrements nextIndex
4. Leader retries with earlier prevLogIndex
5. Eventually finds common prefix, follower appends leader's entries
```

### Commit Rules

An entry is committed when:

1. It's stored on a majority of servers
2. At least one entry from the leader's current term is committed

```
             Before Commit          After Commit
Leader:      [1:1] [2:1] [3:2]     [1:1] [2:1] [3:2]
                         ↑                      ↑
                    uncommitted             committed
                                            (majority)

Follower 1:  [1:1] [2:1] [3:2]     [1:1] [2:1] [3:2]
Follower 2:  [1:1] [2:1]           [1:1] [2:1] [3:2]

             Majority = 2/3         Committed!
```

---

## Safety Properties

### Election Safety

Only one leader can be elected in a given term:

- Nodes only vote once per term
- Winning requires majority votes
- Two majorities must overlap

### Leader Append-Only

Leaders never overwrite or delete entries:

- Only append new entries
- Followers may truncate conflicting entries

### Log Matching

If two logs contain an entry with the same index and term:

1. They store the same command
2. All preceding entries are identical

### Leader Completeness

If a log entry is committed, it will be present in all future leaders' logs.

### State Machine Safety

If a server has applied a log entry at index i, no other server will ever apply a different entry at index i.

---

## Cluster Membership Changes

### Joint Consensus

Safe membership changes using two-phase approach:

```
Phase 1: Joint Configuration (C_old,new)
┌─────────────────────────────────────────────────────────────────┐
│  Entry: [index: N, term: T, cmd: JOINT_START, data: new_config] │
│                                                                  │
│  Majority required from BOTH:                                    │
│  • Old configuration                                             │
│  • New configuration                                             │
└─────────────────────────────────────────────────────────────────┘

Phase 2: New Configuration (C_new)
┌─────────────────────────────────────────────────────────────────┐
│  Entry: [index: M, term: T, cmd: JOINT_END]                     │
│                                                                  │
│  Once committed:                                                 │
│  • Old configuration nodes can be safely removed                 │
│  • Only new configuration required for majorities               │
└─────────────────────────────────────────────────────────────────┘
```

### Adding a Node

```
Timeline:
─────────────────────────────────────────────────────────────────►

1. Admin issues AddNode(new_node)
2. Leader proposes C_old,new entry
3. Leader replicates to both old and new configs
4. Once committed, leader proposes C_new entry
5. Once committed, old nodes can leave

Safety: At every step, overlapping majorities ensure consistency
```

### Removing a Node

```
Special case: Removing the leader

1. Leader receives RemoveNode(self)
2. Leader proposes C_old,new entry
3. Once committed, leader steps down
4. New leader (from new config) proposes C_new
5. Old leader can safely shut down
```

---

## Linearizable Reads

### Read Concern Levels

| Level             | Guarantee | Method                                   |
| ----------------- | --------- | ---------------------------------------- |
| Stale             | None      | Read from any node                       |
| Bounded Staleness | Max age   | Read from follower with recent heartbeat |
| Linearizable      | Real-time | ReadIndex or LogRead                     |

### ReadIndex Protocol

```
Client                    Leader                    Followers
   │                         │                          │
   │  ReadIndex(ctx)         │                          │
   ├────────────────────────►│                          │
   │                         │                          │
   │                         │  recordReadIndex = commitIndex
   │                         │                          │
   │                         │  Heartbeat (confirm leadership)
   │                         ├─────────────────────────►│
   │                         │◄─────────────────────────┤
   │                         │                          │
   │                         │  (majority confirm)      │
   │                         │                          │
   │  ReadIndexResp(index)   │                          │
   │◄────────────────────────┤                          │
   │                         │                          │
   │  (wait for apply ≥ index, then read)               │
```

### Lease-Based Reads

Optimization when clocks are synchronized:

```c
// Leader maintains lease
if (now < leaseExpiry) {
    // Safe to serve reads without heartbeat round-trip
    return readFromStateMachine(key);
}

// Lease expired, must confirm leadership
confirmLeadership();
extendLease();
return readFromStateMachine(key);
```

---

## Persistence Requirements

### What Must Be Persisted

| Data          | Reason                                  |
| ------------- | --------------------------------------- |
| `currentTerm` | Prevent voting twice in same term       |
| `votedFor`    | Prevent voting for multiple candidates  |
| `log[]`       | Committed entries must survive restarts |

### Write-Ahead Logging

```
1. Receive AppendEntries
2. Write entries to WAL (fsync)
3. Update in-memory log
4. Respond to leader
5. Apply committed entries to state machine
```

---

## Performance Optimizations

### Batching

```c
// Instead of one entry per RPC:
AppendEntries(entries: [e1])
AppendEntries(entries: [e2])
AppendEntries(entries: [e3])

// Batch multiple entries:
AppendEntries(entries: [e1, e2, e3])
```

### Pipelining

```c
// Without pipelining:
send(batch1) → wait(ack1) → send(batch2) → wait(ack2)

// With pipelining:
send(batch1) → send(batch2) → send(batch3) → wait(acks)
```

### Parallel Replication

```c
// Replicate to all followers concurrently:
for (follower in followers) {
    async sendAppendEntries(follower, entries);
}
awaitMajority();
```

---

## Common Failure Scenarios

### Leader Crash

```
1. Leader crashes mid-replication
2. Followers time out
3. New election starts
4. New leader has all committed entries
5. May have uncommitted entries (will be overwritten or committed)
```

### Network Partition

```
┌─────────────────────┐     ┌─────────────────────┐
│  Partition A        │     │  Partition B        │
│  (Minority)         │     │  (Majority)         │
│                     │     │                     │
│  Old Leader ───X────│─────│→ Followers          │
│                     │     │                     │
│  Cannot commit      │     │  Elect new leader   │
│  (no majority)      │     │  Continue operating │
│                     │     │                     │
└─────────────────────┘     └─────────────────────┘

When partition heals:
• Old leader discovers higher term
• Steps down to follower
• Truncates uncommitted entries
• Syncs with new leader
```

### Follower Crash and Recovery

```
1. Follower crashes
2. Leader continues with remaining majority
3. Follower restarts, loads persisted state
4. Receives AppendEntries from leader
5. Catches up on missed entries
```

---

## Next Steps

- [Architecture](../architecture.md) - Kraft implementation details
- [Pluggable Networking](pluggable-networking.md) - Network abstraction
- [Multi-Raft](multi-raft.md) - Scaling with multiple groups
