# Message Reference

Complete reference for Kraft protocol messages.

## Message Types

```c
typedef enum {
    // Core Raft RPCs
    KRAFT_MSG_APPEND_ENTRIES = 1,
    KRAFT_MSG_APPEND_ENTRIES_RESPONSE = 2,
    KRAFT_MSG_REQUEST_VOTE = 3,
    KRAFT_MSG_REQUEST_VOTE_RESPONSE = 4,
    KRAFT_MSG_INSTALL_SNAPSHOT = 5,
    KRAFT_MSG_INSTALL_SNAPSHOT_RESPONSE = 6,

    // Pre-vote extension
    KRAFT_MSG_PRE_VOTE = 7,
    KRAFT_MSG_PRE_VOTE_RESPONSE = 8,

    // Leadership transfer
    KRAFT_MSG_TIMEOUT_NOW = 9,

    // Client messages
    KRAFT_MSG_CLIENT_REQUEST = 10,
    KRAFT_MSG_CLIENT_RESPONSE = 11,

    // ReadIndex
    KRAFT_MSG_READ_INDEX = 12,
    KRAFT_MSG_READ_INDEX_RESPONSE = 13,

    // Heartbeat (optimized AppendEntries)
    KRAFT_MSG_HEARTBEAT = 14,
    KRAFT_MSG_HEARTBEAT_RESPONSE = 15,

} kraftMessageType;
```

---

## AppendEntries

Used for log replication and heartbeats.

### Request

```c
typedef struct {
    uint64_t term;              // Leader's term
    uint64_t leaderId;          // For follower to redirect clients
    uint64_t prevLogIndex;      // Index of log entry before new ones
    uint64_t prevLogTerm;       // Term of prevLogIndex entry
    uint64_t leaderCommit;      // Leader's commit index
    kraftLogEntry *entries;     // Log entries to store (empty for heartbeat)
    uint32_t entriesCount;      // Number of entries
} kraftAppendEntriesArgs;

typedef struct {
    uint64_t index;             // Log index
    uint64_t term;              // Term when entry was received
    uint16_t command;           // Command type
    uint8_t *data;              // Command data
    uint32_t dataLen;           // Data length
} kraftLogEntry;
```

### Response

```c
typedef struct {
    uint64_t term;              // Current term, for leader update
    bool success;               // True if matched prevLogIndex/Term
    uint64_t matchIndex;        // Follower's match index (optimization)
    uint64_t conflictIndex;     // First index with conflicting term
    uint64_t conflictTerm;      // Term of conflicting entry
} kraftAppendEntriesResponse;
```

### Binary Wire Format

```
┌────────────────────────────────────────────────────────────────┐
│ AppendEntries Request                                           │
├─────────┬──────────────────────────────────────────────────────┤
│ Offset  │ Field                                                 │
├─────────┼──────────────────────────────────────────────────────┤
│ 0       │ uint8_t msgType (= 1)                                │
│ 1       │ uint64_t term                                        │
│ 9       │ uint64_t leaderId                                    │
│ 17      │ uint64_t prevLogIndex                                │
│ 25      │ uint64_t prevLogTerm                                 │
│ 33      │ uint64_t leaderCommit                                │
│ 41      │ uint32_t entriesCount                                │
│ 45      │ [entries...]                                         │
└─────────┴──────────────────────────────────────────────────────┘

┌────────────────────────────────────────────────────────────────┐
│ Log Entry                                                       │
├─────────┬──────────────────────────────────────────────────────┤
│ 0       │ uint64_t index                                       │
│ 8       │ uint64_t term                                        │
│ 16      │ uint16_t command                                     │
│ 18      │ uint32_t dataLen                                     │
│ 22      │ uint8_t data[dataLen]                                │
└─────────┴──────────────────────────────────────────────────────┘

┌────────────────────────────────────────────────────────────────┐
│ AppendEntries Response                                          │
├─────────┬──────────────────────────────────────────────────────┤
│ 0       │ uint8_t msgType (= 2)                                │
│ 1       │ uint64_t term                                        │
│ 9       │ uint8_t success (0 or 1)                             │
│ 10      │ uint64_t matchIndex                                  │
│ 18      │ uint64_t conflictIndex                               │
│ 26      │ uint64_t conflictTerm                                │
└─────────┴──────────────────────────────────────────────────────┘
```

---

## RequestVote

Used for leader election.

### Request

```c
typedef struct {
    uint64_t term;              // Candidate's term
    uint64_t candidateId;       // Candidate requesting vote
    uint64_t lastLogIndex;      // Index of candidate's last log entry
    uint64_t lastLogTerm;       // Term of candidate's last log entry
    bool preVote;               // Is this a pre-vote? (internal use)
} kraftRequestVoteArgs;
```

### Response

```c
typedef struct {
    uint64_t term;              // Current term, for candidate update
    bool voteGranted;           // True if vote was granted
    bool preVote;               // Is this a pre-vote response?
} kraftRequestVoteResponse;
```

### Binary Wire Format

```
┌────────────────────────────────────────────────────────────────┐
│ RequestVote Request                                             │
├─────────┬──────────────────────────────────────────────────────┤
│ 0       │ uint8_t msgType (= 3 for vote, 7 for pre-vote)       │
│ 1       │ uint64_t term                                        │
│ 9       │ uint64_t candidateId                                 │
│ 17      │ uint64_t lastLogIndex                                │
│ 25      │ uint64_t lastLogTerm                                 │
└─────────┴──────────────────────────────────────────────────────┘

┌────────────────────────────────────────────────────────────────┐
│ RequestVote Response                                            │
├─────────┬──────────────────────────────────────────────────────┤
│ 0       │ uint8_t msgType (= 4 or 8)                           │
│ 1       │ uint64_t term                                        │
│ 9       │ uint8_t voteGranted (0 or 1)                         │
└─────────┴──────────────────────────────────────────────────────┘
```

---

## InstallSnapshot

Used for snapshot transfer to lagging followers.

### Request

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
```

### Response

```c
typedef struct {
    uint64_t term;              // Current term
    uint64_t bytesStored;       // Total bytes received so far
} kraftInstallSnapshotResponse;
```

### Binary Wire Format

```
┌────────────────────────────────────────────────────────────────┐
│ InstallSnapshot Request                                         │
├─────────┬──────────────────────────────────────────────────────┤
│ 0       │ uint8_t msgType (= 5)                                │
│ 1       │ uint64_t term                                        │
│ 9       │ uint64_t leaderId                                    │
│ 17      │ uint64_t lastIncludedIndex                           │
│ 25      │ uint64_t lastIncludedTerm                            │
│ 33      │ uint64_t offset                                      │
│ 41      │ uint32_t dataLen                                     │
│ 45      │ uint8_t done (0 or 1)                                │
│ 46      │ uint8_t data[dataLen]                                │
└─────────┴──────────────────────────────────────────────────────┘

┌────────────────────────────────────────────────────────────────┐
│ InstallSnapshot Response                                        │
├─────────┬──────────────────────────────────────────────────────┤
│ 0       │ uint8_t msgType (= 6)                                │
│ 1       │ uint64_t term                                        │
│ 9       │ uint64_t bytesStored                                 │
└─────────┴──────────────────────────────────────────────────────┘
```

---

## TimeoutNow

Used for leadership transfer.

### Request

```c
typedef struct {
    uint64_t term;              // Leader's current term
    uint64_t leaderId;          // Current leader ID
} kraftTimeoutNowArgs;
```

### Binary Wire Format

```
┌────────────────────────────────────────────────────────────────┐
│ TimeoutNow Request                                              │
├─────────┬──────────────────────────────────────────────────────┤
│ 0       │ uint8_t msgType (= 9)                                │
│ 1       │ uint64_t term                                        │
│ 9       │ uint64_t leaderId                                    │
└─────────┴──────────────────────────────────────────────────────┘
```

---

## Client Request

Commands from clients.

### Request

```c
typedef struct {
    uint64_t clientId;          // Client identifier
    uint64_t sequenceNum;       // For deduplication
    uint8_t *command;           // Command data
    uint32_t commandLen;        // Command length
    kraftCommitMode commitMode; // Requested commit mode (optional)
} kraftClientRequestArgs;
```

### Response

```c
typedef struct {
    kraftClientStatus status;   // Result status
    uint64_t logIndex;          // Committed log index (if success)
    uint64_t term;              // Term of commit
    uint64_t leaderNodeId;      // Current leader (for redirect)
    char *leaderAddress;        // Leader address (for redirect)
    uint16_t leaderPort;        // Leader port
    uint8_t *response;          // Response data (optional)
    uint32_t responseLen;       // Response length
} kraftClientResponseArgs;
```

### Binary Wire Format

```
┌────────────────────────────────────────────────────────────────┐
│ Client Request                                                  │
├─────────┬──────────────────────────────────────────────────────┤
│ 0       │ uint8_t msgType (= 10)                               │
│ 1       │ uint64_t clientId                                    │
│ 9       │ uint64_t sequenceNum                                 │
│ 17      │ uint8_t commitMode                                   │
│ 18      │ uint32_t commandLen                                  │
│ 22      │ uint8_t command[commandLen]                          │
└─────────┴──────────────────────────────────────────────────────┘

┌────────────────────────────────────────────────────────────────┐
│ Client Response                                                 │
├─────────┬──────────────────────────────────────────────────────┤
│ 0       │ uint8_t msgType (= 11)                               │
│ 1       │ uint8_t status                                       │
│ 2       │ uint64_t logIndex                                    │
│ 10      │ uint64_t term                                        │
│ 18      │ uint64_t leaderNodeId                                │
│ 26      │ uint16_t leaderPort                                  │
│ 28      │ uint16_t leaderAddressLen                            │
│ 30      │ char leaderAddress[leaderAddressLen]                 │
│ ...     │ uint32_t responseLen                                 │
│ ...     │ uint8_t response[responseLen]                        │
└─────────┴──────────────────────────────────────────────────────┘
```

---

## ReadIndex

For linearizable reads.

### Request

```c
typedef struct {
    uint64_t clientId;          // Client identifier
    uint64_t requestId;         // Request identifier
    uint8_t *context;           // Application context (optional)
    uint32_t contextLen;        // Context length
} kraftReadIndexArgs;
```

### Response

```c
typedef struct {
    kraftClientStatus status;   // Result status
    uint64_t readIndex;         // Safe read index
    uint64_t term;              // Current term
    uint64_t leaderNodeId;      // Leader (for redirect)
} kraftReadIndexResponse;
```

### Binary Wire Format

```
┌────────────────────────────────────────────────────────────────┐
│ ReadIndex Request                                               │
├─────────┬──────────────────────────────────────────────────────┤
│ 0       │ uint8_t msgType (= 12)                               │
│ 1       │ uint64_t clientId                                    │
│ 9       │ uint64_t requestId                                   │
│ 17      │ uint32_t contextLen                                  │
│ 21      │ uint8_t context[contextLen]                          │
└─────────┴──────────────────────────────────────────────────────┘

┌────────────────────────────────────────────────────────────────┐
│ ReadIndex Response                                              │
├─────────┬──────────────────────────────────────────────────────┤
│ 0       │ uint8_t msgType (= 13)                               │
│ 1       │ uint8_t status                                       │
│ 2       │ uint64_t readIndex                                   │
│ 10      │ uint64_t term                                        │
│ 18      │ uint64_t leaderNodeId                                │
└─────────┴──────────────────────────────────────────────────────┘
```

---

## Heartbeat

Optimized heartbeat (empty AppendEntries).

### Request

```c
typedef struct {
    uint64_t term;              // Leader's term
    uint64_t leaderId;          // Leader ID
    uint64_t leaderCommit;      // Leader's commit index
    uint64_t readIndex;         // For follower reads (optional)
} kraftHeartbeatArgs;
```

### Response

```c
typedef struct {
    uint64_t term;              // Current term
    uint64_t matchIndex;        // Follower's match index
} kraftHeartbeatResponse;
```

### Binary Wire Format

```
┌────────────────────────────────────────────────────────────────┐
│ Heartbeat Request                                               │
├─────────┬──────────────────────────────────────────────────────┤
│ 0       │ uint8_t msgType (= 14)                               │
│ 1       │ uint64_t term                                        │
│ 9       │ uint64_t leaderId                                    │
│ 17      │ uint64_t leaderCommit                                │
│ 25      │ uint64_t readIndex                                   │
└─────────┴──────────────────────────────────────────────────────┘

┌────────────────────────────────────────────────────────────────┐
│ Heartbeat Response                                              │
├─────────┬──────────────────────────────────────────────────────┤
│ 0       │ uint8_t msgType (= 15)                               │
│ 1       │ uint64_t term                                        │
│ 9       │ uint64_t matchIndex                                  │
└─────────┴──────────────────────────────────────────────────────┘
```

---

## Message Framing

All messages are framed with a length prefix:

```
┌────────────────────────────────────────────────────────────────┐
│ Wire Frame                                                      │
├─────────┬──────────────────────────────────────────────────────┤
│ 0-3     │ uint32_t totalLength (including this header)         │
│ 4-...   │ Message payload                                      │
└─────────┴──────────────────────────────────────────────────────┘
```

## Endianness

All multi-byte integers are little-endian.

## Version Negotiation

Connections start with a protocol header:

```
┌────────────────────────────────────────────────────────────────┐
│ Protocol Header (sent on connection)                            │
├─────────┬──────────────────────────────────────────────────────┤
│ 0-3     │ char[4] magic = "RAFT"                               │
│ 4       │ uint8_t protocolVersion                              │
│ 5       │ uint8_t encodingType (0=binary, 1=json, etc)         │
│ 6-7     │ uint16_t reserved                                    │
└─────────┴──────────────────────────────────────────────────────┘
```

---

## Next Steps

- [API Reference](api.md) - Complete API documentation
- [Configuration Reference](configuration.md) - Configuration options
- [Custom Protocols](../guides/custom-protocols.md) - Implement custom encodings
