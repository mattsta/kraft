# API Reference

Complete API documentation for Kraft.

## Client API

### Session Management

```c
/**
 * Create a new client session
 *
 * @param adapter Network adapter for I/O
 * @param protocol Protocol encoder/decoder
 * @param config Optional configuration (NULL for defaults)
 * @return New client session or NULL on failure
 */
kraftClientSession *kraftClientCreate(
    kraftAdapter *adapter,
    kraftProtocol *protocol,
    kraftClientConfig *config
);

/**
 * Destroy client session and release resources
 */
void kraftClientDestroy(kraftClientSession *session);

/**
 * Add cluster endpoint for connection
 *
 * @param session Client session
 * @param address Hostname or IP address
 * @param port Port number
 * @return true on success
 */
bool kraftClientAddEndpoint(
    kraftClientSession *session,
    const char *address,
    uint16_t port
);

/**
 * Connect to cluster
 *
 * @param session Client session
 * @return true if connection initiated
 */
bool kraftClientConnect(kraftClientSession *session);

/**
 * Disconnect from cluster
 */
void kraftClientDisconnect(kraftClientSession *session);
```

### Command Submission

```c
/**
 * Callback for command completion
 */
typedef void (*kraftClientCallback)(
    kraftClientSession *session,
    const kraftClientRequest *request,
    const kraftClientResponse *response,
    void *userData
);

/**
 * Submit command asynchronously
 *
 * @param session Client session
 * @param data Command data
 * @param len Command length
 * @param callback Completion callback (NULL for fire-and-forget)
 * @param userData User data passed to callback
 * @return Request ID or 0 on failure
 */
uint64_t kraftClientSubmit(
    kraftClientSession *session,
    const uint8_t *data,
    size_t len,
    kraftClientCallback callback,
    void *userData
);

/**
 * Submit command synchronously (blocking)
 *
 * @param session Client session
 * @param data Command data
 * @param len Command length
 * @param response Output response (must not be NULL)
 * @param timeoutMs Timeout in milliseconds
 * @return Client status
 */
kraftClientStatus kraftClientSubmitSync(
    kraftClientSession *session,
    const uint8_t *data,
    size_t len,
    kraftClientResponse *response,
    uint64_t timeoutMs
);

/**
 * Submit with custom options
 */
uint64_t kraftClientSubmitWithOptions(
    kraftClientSession *session,
    const kraftSubmitOptions *options
);
```

### Read Operations

```c
/**
 * Callback for read completion
 */
typedef void (*kraftReadCallback)(
    kraftClientSession *session,
    const kraftReadResponse *response,
    void *userData
);

/**
 * Perform linearizable read (async)
 *
 * @param session Client session
 * @param callback Completion callback
 * @param userData User data
 * @return Request ID or 0 on failure
 */
uint64_t kraftClientRead(
    kraftClientSession *session,
    kraftReadCallback callback,
    void *userData
);

/**
 * Perform linearizable read (blocking)
 *
 * @param session Client session
 * @param readIndex Output: safe read index
 * @param timeoutMs Timeout
 * @return Client status
 */
kraftClientStatus kraftClientReadSync(
    kraftClientSession *session,
    uint64_t *readIndex,
    uint64_t timeoutMs
);

/**
 * Read with specified consistency level
 */
uint64_t kraftClientReadWithOptions(
    kraftClientSession *session,
    const kraftReadOptions *options,
    kraftReadCallback callback,
    void *userData
);
```

### Leader Discovery

```c
/**
 * Get current known leader
 *
 * @param session Client session
 * @param leaderId Output: leader node ID (0 if unknown)
 * @param address Output: leader address (may be NULL)
 * @param port Output: leader port
 * @return true if leader is known
 */
bool kraftClientGetLeader(
    kraftClientSession *session,
    uint64_t *leaderId,
    char **address,
    uint16_t *port
);

/**
 * Refresh leader information from cluster
 *
 * @param session Client session
 * @return true if refresh initiated
 */
bool kraftClientRefreshLeader(kraftClientSession *session);
```

### Batching

```c
/**
 * Begin a request batch
 *
 * @param session Client session
 * @return true on success
 */
bool kraftClientBatchBegin(kraftClientSession *session);

/**
 * End batch and send all requests
 *
 * @param session Client session
 * @return true on success
 */
bool kraftClientBatchEnd(kraftClientSession *session);

/**
 * Cancel current batch
 */
void kraftClientBatchCancel(kraftClientSession *session);
```

### Event Loop

```c
/**
 * Run event loop (blocking)
 */
void kraftClientRun(kraftClientSession *session);

/**
 * Stop event loop (thread-safe)
 */
void kraftClientStop(kraftClientSession *session);

/**
 * Poll for events (non-blocking)
 *
 * @param session Client session
 * @param timeoutMs Max time to wait
 * @return Number of events processed
 */
int kraftClientPoll(kraftClientSession *session, uint64_t timeoutMs);
```

---

## Server API

### State Management

```c
/**
 * Create new Raft state
 *
 * @return New state or NULL on failure
 */
kraftState *kraftStateNew(void);

/**
 * Create state with storage configuration
 */
kraftState *kraftStateNewWithStorage(kraftStorageConfig *config);

/**
 * Recover state from persistent storage
 *
 * @param config Storage configuration
 * @return Recovered state or NULL if no prior state
 */
kraftState *kraftStateRecover(kraftStorageConfig *config);

/**
 * Free Raft state
 */
void kraftStateFree(kraftState *state);

/**
 * Initialize Raft cluster configuration
 *
 * @param state Raft state
 * @param peerCount Number of peers
 * @param peers Array of peer configurations
 * @param clusterId Unique cluster identifier
 * @param nodeId This node's ID
 * @return true on success
 */
bool kraftStateInitRaft(
    kraftState *state,
    size_t peerCount,
    kraftIp **peers,
    uint64_t clusterId,
    uint64_t nodeId
);
```

### Role Queries

```c
/**
 * Check if this node is leader
 */
bool kraftStateIsLeader(kraftState *state);

/**
 * Get current role
 */
kraftRole kraftStateGetRole(kraftState *state);

/**
 * Get current term
 */
uint64_t kraftStateGetTerm(kraftState *state);

/**
 * Get current leader ID (0 if unknown)
 */
uint64_t kraftStateGetLeader(kraftState *state);
```

### Callbacks

```c
/**
 * Apply callback - called when entries are committed
 */
typedef void (*kraftApplyCallback)(const void *data, uint64_t len);

/**
 * State change callback
 */
typedef void (*kraftStateChangeCallback)(
    kraftState *state,
    kraftRole oldRole,
    kraftRole newRole
);

/**
 * Leader change callback
 */
typedef void (*kraftLeaderChangeCallback)(
    kraftState *state,
    uint64_t oldLeader,
    uint64_t newLeader
);

/**
 * Set callback
 */
void kraftStateSetCallback(
    kraftState *state,
    kraftCallbackType type,
    void *callback
);
```

### Membership

```c
/**
 * Add a voting node to cluster
 *
 * @param state Raft state
 * @param peer Peer configuration
 * @return true if change proposed
 */
bool kraftStateAddNode(kraftState *state, kraftIp *peer);

/**
 * Remove a node from cluster
 */
bool kraftStateRemoveNode(kraftState *state, uint64_t nodeId);

/**
 * Add a witness (non-voting) node
 */
bool kraftStateAddWitness(kraftState *state, kraftWitnessConfig *config);

/**
 * Remove a witness
 */
bool kraftStateRemoveWitness(kraftState *state, uint64_t nodeId);

/**
 * Get cluster membership
 */
size_t kraftStateGetMembers(
    kraftState *state,
    kraftMember *members,
    size_t maxMembers
);
```

### Leadership Transfer

```c
/**
 * Transfer leadership to specific node
 *
 * @param state Raft state
 * @param targetNodeId Target node ID
 * @return true if transfer initiated
 */
bool kraftStateTransferLeadership(kraftState *state, uint64_t targetNodeId);

/**
 * Find best transfer target
 *
 * @return Node ID or 0 if no suitable target
 */
uint64_t kraftStateFindBestTransferTarget(kraftState *state);

/**
 * Get transfer status
 */
kraftTransferStatus kraftStateGetTransferStatus(kraftState *state);
```

### Features

```c
/**
 * Enable/disable pre-vote protocol
 */
void kraftStateEnablePreVote(kraftState *state, bool enable);

/**
 * Enable/disable ReadIndex protocol
 */
void kraftStateEnableReadIndex(kraftState *state, bool enable);

/**
 * Enable/disable leadership transfer
 */
void kraftStateEnableLeadershipTransfer(kraftState *state, bool enable);

/**
 * Configure snapshots
 */
void kraftStateConfigureSnapshots(
    kraftState *state,
    kraftSnapshotConfig *config
);

/**
 * Configure commit behavior
 */
void kraftStateConfigureCommit(
    kraftState *state,
    kraftCommitConfig *config
);
```

---

## Adapter API

### Core Operations

```c
/**
 * Create adapter with specified operations
 */
kraftAdapter *kraftAdapterCreate(
    kraftAdapterOps *ops,
    void *userData
);

/**
 * Destroy adapter
 */
void kraftAdapterDestroy(kraftAdapter *adapter);

/**
 * Listen for connections
 */
bool kraftAdapterListen(
    kraftAdapter *adapter,
    const char *address,
    uint16_t port,
    kraftConnectionCallback onConnection,
    void *userData
);

/**
 * Run event loop
 */
void kraftAdapterRun(kraftAdapter *adapter);

/**
 * Stop event loop (thread-safe)
 */
void kraftAdapterStop(kraftAdapter *adapter);

/**
 * Connect to remote host
 */
kraftConnection *kraftAdapterConnect(
    kraftAdapter *adapter,
    const char *address,
    uint16_t port,
    kraftConnectCallback onConnect,
    void *userData
);

/**
 * Write data to connection
 */
bool kraftAdapterWrite(
    kraftAdapter *adapter,
    kraftConnection *conn,
    const uint8_t *data,
    size_t len,
    kraftWriteCallback onComplete,
    void *userData
);

/**
 * Close connection
 */
void kraftAdapterClose(kraftAdapter *adapter, kraftConnection *conn);
```

### Timer Operations

```c
/**
 * Create a timer
 */
kraftTimer *kraftAdapterTimerCreate(
    kraftAdapter *adapter,
    kraftTimerCallback callback,
    void *userData
);

/**
 * Start timer
 */
bool kraftAdapterTimerStart(
    kraftAdapter *adapter,
    kraftTimer *timer,
    uint64_t timeoutMs,
    bool repeat
);

/**
 * Stop timer
 */
void kraftAdapterTimerStop(kraftAdapter *adapter, kraftTimer *timer);

/**
 * Destroy timer
 */
void kraftAdapterTimerDestroy(kraftAdapter *adapter, kraftTimer *timer);

/**
 * Get current time in milliseconds
 */
uint64_t kraftAdapterNow(kraftAdapter *adapter);
```

---

## Protocol API

### Creation

```c
/**
 * Create protocol with specified operations
 */
kraftProtocol *kraftProtocolCreate(
    kraftProtocolOps *ops,
    void *userData
);

/**
 * Get built-in binary protocol operations
 */
kraftProtocolOps *kraftProtocolGetBinaryOps(void);

/**
 * Destroy protocol
 */
void kraftProtocolDestroy(kraftProtocol *protocol);
```

### Encoding

```c
/**
 * Encode AppendEntries request
 */
bool kraftProtocolEncodeAppendEntries(
    kraftProtocol *protocol,
    kraftEncodeBuffer *buf,
    const kraftAppendEntriesArgs *args
);

/**
 * Encode AppendEntries response
 */
bool kraftProtocolEncodeAppendEntriesResponse(
    kraftProtocol *protocol,
    kraftEncodeBuffer *buf,
    const kraftAppendEntriesResponse *resp
);

/**
 * Encode RequestVote request
 */
bool kraftProtocolEncodeRequestVote(
    kraftProtocol *protocol,
    kraftEncodeBuffer *buf,
    const kraftRequestVoteArgs *args
);

// Similar for other message types...
```

### Decoding

```c
/**
 * Decode message from buffer
 *
 * @param protocol Protocol instance
 * @param data Input buffer
 * @param len Buffer length
 * @param out Output message
 * @param consumed Output: bytes consumed
 * @return Decode status
 */
kraftDecodeStatus kraftProtocolDecode(
    kraftProtocol *protocol,
    const uint8_t *data,
    size_t len,
    kraftMessage *out,
    size_t *consumed
);
```

---

## Data Types

### Enumerations

```c
typedef enum {
    KRAFT_FOLLOWER = 0,
    KRAFT_PRE_CANDIDATE = 1,
    KRAFT_CANDIDATE = 2,
    KRAFT_LEADER = 3,
} kraftRole;

typedef enum {
    KRAFT_CLIENT_OK = 0,
    KRAFT_CLIENT_NOT_LEADER = 1,
    KRAFT_CLIENT_TIMEOUT = 2,
    KRAFT_CLIENT_ERROR = 3,
    KRAFT_CLIENT_REDIRECT = 4,
} kraftClientStatus;

typedef enum {
    KRAFT_DECODE_OK = 0,
    KRAFT_DECODE_NEED_MORE = 1,
    KRAFT_DECODE_ERROR = 2,
} kraftDecodeStatus;

typedef enum {
    KRAFT_READ_LINEARIZABLE = 0,
    KRAFT_READ_SERIALIZABLE = 1,
    KRAFT_READ_BOUNDED_STALENESS = 2,
    KRAFT_READ_STALE = 3,
} kraftReadConsistency;

typedef enum {
    KRAFT_COMMIT_SYNC = 0,
    KRAFT_COMMIT_LEADER_ONLY = 1,
    KRAFT_COMMIT_ASYNC = 2,
    KRAFT_COMMIT_ALL = 3,
} kraftCommitMode;

typedef enum {
    KRAFT_FSYNC_NONE = 0,
    KRAFT_FSYNC_BATCH = 1,
    KRAFT_FSYNC_COMMIT = 2,
    KRAFT_FSYNC_ALWAYS = 3,
} kraftFsyncMode;
```

### Structures

```c
typedef struct {
    char *ip;
    uint16_t port;
    bool outbound;
    uint64_t nodeId;
} kraftIp;

typedef struct {
    uint64_t requestId;
    kraftClientStatus status;
    uint64_t logIndex;
    uint64_t term;
    char *leaderAddress;
    uint16_t leaderPort;
} kraftClientResponse;

typedef struct {
    uint64_t index;
    uint64_t term;
    uint16_t command;
    uint8_t *data;
    uint32_t dataLen;
} kraftLogEntry;

typedef struct {
    uint64_t term;
    uint64_t leaderId;
    uint64_t prevLogIndex;
    uint64_t prevLogTerm;
    uint64_t leaderCommit;
    kraftLogEntry *entries;
    uint32_t entriesCount;
} kraftAppendEntriesArgs;

typedef struct {
    uint64_t term;
    bool success;
    uint64_t matchIndex;
    uint64_t conflictIndex;
    uint64_t conflictTerm;
} kraftAppendEntriesResponse;

typedef struct {
    uint64_t term;
    uint64_t candidateId;
    uint64_t lastLogIndex;
    uint64_t lastLogTerm;
    bool preVote;
} kraftRequestVoteArgs;

typedef struct {
    uint64_t term;
    bool voteGranted;
    bool preVote;
} kraftRequestVoteResponse;
```

---

## Error Handling

### Status Codes

```c
const char *kraftClientStatusString(kraftClientStatus status);
const char *kraftDecodeStatusString(kraftDecodeStatus status);
const char *kraftRoleString(kraftRole role);
```

### Error Callbacks

```c
typedef void (*kraftErrorCallback)(
    kraftState *state,
    kraftError error,
    const char *message,
    void *userData
);

void kraftStateSetErrorCallback(
    kraftState *state,
    kraftErrorCallback callback,
    void *userData
);
```

---

## Thread Safety

```
┌─────────────────────────────────────────────────────────────────┐
│                   Thread Safety Summary                          │
│                                                                  │
│  Function                      │ Thread Safe                    │
│  ─────────────────────────────┼───────────────────────────────  │
│  kraftAdapterStop()            │ YES - safe from any thread     │
│  kraftClientStop()             │ YES - safe from any thread     │
│  All other kraft* functions    │ NO - call from event loop      │
│                                │     thread only                 │
│                                                                  │
│  For multi-threaded applications:                               │
│  - Use one kraft instance per thread, OR                        │
│  - Protect with external mutex, OR                              │
│  - Use message passing to kraft thread                          │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Next Steps

- [Configuration Reference](configuration.md) - All configuration options
- [Message Reference](messages.md) - Message format details
- [Client Development](../guides/client-development.md) - Usage examples
