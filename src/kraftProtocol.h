/* Kraft Pluggable Protocol Interface
 * ====================================
 *
 * This header defines the abstract interface for message encoding/decoding.
 * Implementations can use any wire format (binary, protobuf, msgpack, JSON,
 * etc.)
 *
 * The protocol is responsible for:
 *   - Encoding Raft RPC messages for transmission
 *   - Decoding received bytes into Raft messages
 *   - Framing (message boundaries)
 *   - Optional compression
 *   - Optional checksums/integrity verification
 *
 * Message Flow:
 *   Outbound: kraftState -> encode() -> bytes -> network
 *   Inbound:  network -> bytes -> decode() -> kraftState
 *
 * Thread Safety:
 *   - Encoder/decoder instances are NOT thread-safe
 *   - Create one instance per connection or protect with mutex
 */

#ifndef KRAFT_PROTOCOL_H
#define KRAFT_PROTOCOL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declarations */
struct kraftState;
struct kraftProtocol;
struct kraftNode;

/* =========================================================================
 * Message Types
 * ========================================================================= */

typedef enum kraftMessageType {
    /* Handshake */
    KRAFT_MSG_HELLO = 1, /* Initial peer handshake */
    KRAFT_MSG_HELLO_ACK, /* Handshake acknowledgment */

    /* Raft Core RPCs */
    KRAFT_MSG_APPEND_ENTRIES,      /* Leader -> Follower: replicate entries */
    KRAFT_MSG_APPEND_ENTRIES_RESP, /* Follower -> Leader: response */
    KRAFT_MSG_REQUEST_VOTE,        /* Candidate -> All: request votes */
    KRAFT_MSG_REQUEST_VOTE_RESP,   /* All -> Candidate: vote response */

    /* Pre-Vote Extension (Raft thesis section 9.6) */
    KRAFT_MSG_PRE_VOTE,      /* Pre-candidate -> All: check if election OK */
    KRAFT_MSG_PRE_VOTE_RESP, /* All -> Pre-candidate: pre-vote response */

    /* InstallSnapshot RPC (log compaction) */
    KRAFT_MSG_INSTALL_SNAPSHOT, /* Leader -> Follower: send snapshot chunk */
    KRAFT_MSG_INSTALL_SNAPSHOT_RESP, /* Follower -> Leader: response */

    /* Leadership Transfer Extension */
    KRAFT_MSG_TIMEOUT_NOW, /* Leader -> Target: become leader immediately */

    /* ReadIndex Extension (Raft thesis section 6.4) */
    KRAFT_MSG_READ_INDEX,      /* Client/Follower -> Leader: read request */
    KRAFT_MSG_READ_INDEX_RESP, /* Leader -> Client/Follower: read response */

    /* Membership Changes */
    KRAFT_MSG_ADD_SERVER, /* Add server to cluster */
    KRAFT_MSG_ADD_SERVER_RESP,
    KRAFT_MSG_REMOVE_SERVER, /* Remove server from cluster */
    KRAFT_MSG_REMOVE_SERVER_RESP,

    /* Client Commands */
    KRAFT_MSG_CLIENT_REQUEST,  /* Client -> Leader: submit command */
    KRAFT_MSG_CLIENT_RESPONSE, /* Leader -> Client: command result */

    /* Multi-Raft */
    KRAFT_MSG_MULTIRAFT_WRAPPER, /* Container for multi-raft group messages */

    /* Heartbeat (optimized empty AppendEntries) */
    KRAFT_MSG_HEARTBEAT,      /* Leader -> Follower: heartbeat */
    KRAFT_MSG_HEARTBEAT_RESP, /* Follower -> Leader: heartbeat response */

    /* Internal/Debug */
    KRAFT_MSG_PING,  /* Keepalive ping */
    KRAFT_MSG_PONG,  /* Keepalive pong */
    KRAFT_MSG_ERROR, /* Error response */

    KRAFT_MSG_TYPE_MAX
} kraftMessageType;

/* =========================================================================
 * Message Header (common to all messages)
 * ========================================================================= */

typedef struct kraftMessageHeader {
    kraftMessageType type; /* Message type */
    uint32_t version;      /* Protocol version */
    uint64_t clusterId;    /* Cluster identifier */
    uint64_t senderId;     /* Sender node ID */
    uint64_t term;         /* Current term of sender */
    uint32_t payloadLen;   /* Length of message payload */
    uint32_t checksum;     /* Optional payload checksum */

    /* Multi-Raft support */
    uint32_t groupId; /* Raft group ID (0 = single-raft mode) */

    /* Tracing/debugging */
    uint64_t messageId;   /* Unique message ID for tracing */
    uint64_t timestampUs; /* Send timestamp in microseconds */
} kraftMessageHeader;

/* =========================================================================
 * Raft RPC Message Structures
 * ========================================================================= */

/* HELLO handshake (peer identification) */
typedef struct kraftMsgHello {
    uint64_t nodeId;          /* Sender's permanent node ID */
    uint64_t runId;           /* Sender's current run instance */
    uint64_t clusterId;       /* Cluster the sender belongs to */
    char address[256];        /* Sender's advertised address */
    uint16_t port;            /* Sender's advertised port */
    uint32_t protocolVersion; /* Supported protocol version */
    uint32_t capabilities;    /* Feature capability bitmap */
} kraftMsgHello;

/* AppendEntries RPC (Raft section 5.3) */
typedef struct kraftMsgAppendEntries {
    uint64_t term;         /* Leader's term */
    uint64_t leaderId;     /* So follower can redirect clients */
    uint64_t prevLogIndex; /* Index of log entry before new ones */
    uint64_t prevLogTerm;  /* Term of prevLogIndex entry */
    uint64_t leaderCommit; /* Leader's commitIndex */

    /* Entries to replicate */
    uint32_t entryCount; /* Number of entries (0 = heartbeat) */
    /* Entries follow in payload */
} kraftMsgAppendEntries;

/* AppendEntries Response */
typedef struct kraftMsgAppendEntriesResp {
    uint64_t term;       /* CurrentTerm, for leader to update itself */
    bool success;        /* True if follower contained entry matching prevLog */
    uint64_t matchIndex; /* Highest index known to be replicated */
    uint64_t conflictIndex; /* If rejected: first conflicting index */
    uint64_t conflictTerm;  /* If rejected: term at conflictIndex */
} kraftMsgAppendEntriesResp;

/* RequestVote RPC (Raft section 5.2) */
typedef struct kraftMsgRequestVote {
    uint64_t term;         /* Candidate's term */
    uint64_t candidateId;  /* Candidate requesting vote */
    uint64_t lastLogIndex; /* Index of candidate's last log entry */
    uint64_t lastLogTerm;  /* Term of candidate's last log entry */
    bool isPreVote;        /* True if this is a pre-vote (section 9.6) */
} kraftMsgRequestVote;

/* RequestVote Response */
typedef struct kraftMsgRequestVoteResp {
    uint64_t term;    /* CurrentTerm, for candidate to update itself */
    bool voteGranted; /* True means candidate received vote */
    bool isPreVote;   /* True if this is a pre-vote response */
} kraftMsgRequestVoteResp;

/* InstallSnapshot RPC (Raft section 7) */
typedef struct kraftMsgInstallSnapshot {
    uint64_t term;              /* Leader's term */
    uint64_t leaderId;          /* Leader's ID */
    uint64_t lastIncludedIndex; /* Last index included in snapshot */
    uint64_t lastIncludedTerm;  /* Term of lastIncludedIndex */
    uint64_t offset;            /* Byte offset in snapshot file */
    bool done;                  /* True if this is the last chunk */
    uint32_t dataLen;           /* Length of snapshot data in this chunk */
    /* Snapshot data follows in payload */
} kraftMsgInstallSnapshot;

/* InstallSnapshot Response */
typedef struct kraftMsgInstallSnapshotResp {
    uint64_t term;          /* CurrentTerm, for leader to update itself */
    bool success;           /* True if chunk was accepted */
    uint64_t bytesReceived; /* Total bytes received so far */
} kraftMsgInstallSnapshotResp;

/* TimeoutNow (Leadership Transfer, section 3.10) */
typedef struct kraftMsgTimeoutNow {
    uint64_t term;     /* Current term */
    uint64_t targetId; /* Target node that should become leader */
} kraftMsgTimeoutNow;

/* ReadIndex Request (Raft thesis section 6.4) */
typedef struct kraftMsgReadIndex {
    uint64_t clientId;  /* Client making the read request */
    uint64_t requestId; /* Request identifier for correlation */
    /* Optional: key/query to read - in payload */
} kraftMsgReadIndex;

/* ReadIndex Response */
typedef struct kraftMsgReadIndexResp {
    uint64_t clientId;  /* Original client ID */
    uint64_t requestId; /* Original request ID */
    uint64_t readIndex; /* Index at which read is safe */
    bool success;       /* True if read can proceed */
    char error[128];    /* Error message if !success */
} kraftMsgReadIndexResp;

/* Client Request */
typedef struct kraftMsgClientRequest {
    uint64_t clientId;    /* Client identifier */
    uint64_t requestSeq;  /* Client request sequence number */
    uint32_t commandType; /* Application-specific command type */
    /* Command data follows in payload */
} kraftMsgClientRequest;

/* Client Response */
typedef struct kraftMsgClientResponse {
    uint64_t clientId;       /* Original client ID */
    uint64_t requestSeq;     /* Original request sequence */
    bool success;            /* True if command was applied */
    uint64_t logIndex;       /* Index where command was logged */
    uint64_t leaderHint;     /* If not leader, hint at leader ID */
    char leaderAddress[256]; /* If not leader, hint at leader address */
    /* Response data follows in payload */
} kraftMsgClientResponse;

/* Heartbeat (optimized empty AppendEntries) */
typedef struct kraftMsgHeartbeat {
    uint64_t term;         /* Leader's term */
    uint64_t leaderId;     /* Leader's ID */
    uint64_t leaderCommit; /* Leader's commit index */
} kraftMsgHeartbeat;

/* Heartbeat Response */
typedef struct kraftMsgHeartbeatResp {
    uint64_t term;       /* Follower's term */
    uint64_t matchIndex; /* Follower's match index */
    bool success;        /* True if follower accepts leader */
} kraftMsgHeartbeatResp;

/* Error Message */
typedef struct kraftMsgError {
    uint32_t errorCode; /* Error code */
    char message[256];  /* Human-readable error message */
} kraftMsgError;

/* =========================================================================
 * Log Entry Structure
 * ========================================================================= */

typedef struct kraftLogEntry {
    uint64_t index;      /* Log index */
    uint64_t term;       /* Term when entry was created */
    uint32_t type;       /* Entry type (command, config change, etc.) */
    uint32_t dataLen;    /* Length of entry data */
    const uint8_t *data; /* Entry data (not owned, points into buffer) */
} kraftLogEntry;

/* Entry types */
typedef enum kraftEntryType {
    KRAFT_ENTRY_COMMAND = 1,     /* Normal client command */
    KRAFT_ENTRY_CONFIG,          /* Configuration change */
    KRAFT_ENTRY_NOOP,            /* No-op entry (new leader commit) */
    KRAFT_ENTRY_SNAPSHOT_MARKER, /* Marks snapshot boundary */
} kraftEntryType;

/* =========================================================================
 * Decode Result
 * ========================================================================= */

typedef enum kraftDecodeStatus {
    KRAFT_DECODE_OK = 0,           /* Message decoded successfully */
    KRAFT_DECODE_NEED_MORE,        /* Need more data (incomplete frame) */
    KRAFT_DECODE_ERROR,            /* Decode error (corrupt/invalid) */
    KRAFT_DECODE_VERSION_MISMATCH, /* Unsupported protocol version */
    KRAFT_DECODE_CHECKSUM_FAIL,    /* Checksum verification failed */
} kraftDecodeStatus;

typedef struct kraftDecodeResult {
    kraftDecodeStatus status;
    size_t bytesConsumed;      /* Bytes consumed from input buffer */
    kraftMessageHeader header; /* Decoded header */

    /* Union of message types - only one is valid based on header.type */
    union {
        kraftMsgHello hello;
        kraftMsgAppendEntries appendEntries;
        kraftMsgAppendEntriesResp appendEntriesResp;
        kraftMsgRequestVote requestVote;
        kraftMsgRequestVoteResp requestVoteResp;
        kraftMsgInstallSnapshot installSnapshot;
        kraftMsgInstallSnapshotResp installSnapshotResp;
        kraftMsgTimeoutNow timeoutNow;
        kraftMsgReadIndex readIndex;
        kraftMsgReadIndexResp readIndexResp;
        kraftMsgClientRequest clientRequest;
        kraftMsgClientResponse clientResponse;
        kraftMsgHeartbeat heartbeat;
        kraftMsgHeartbeatResp heartbeatResp;
        kraftMsgError error;
    } msg;

    /* For messages with entries (AppendEntries) */
    uint32_t entryCount;
    kraftLogEntry *entries; /* Array of decoded entries */

    /* Raw payload access (for application data) */
    const uint8_t *payload;
    size_t payloadLen;
} kraftDecodeResult;

/* =========================================================================
 * Encode Buffer
 * ========================================================================= */

typedef struct kraftEncodeBuffer {
    uint8_t *data;   /* Buffer data */
    size_t len;      /* Current data length */
    size_t capacity; /* Buffer capacity */
    bool owned;      /* True if buffer owns the data */
} kraftEncodeBuffer;

/* Initialize encode buffer with given capacity */
bool kraftEncodeBufferInit(kraftEncodeBuffer *buf, size_t capacity);

/* Reset buffer for reuse (keeps allocated memory) */
void kraftEncodeBufferReset(kraftEncodeBuffer *buf);

/* Free encode buffer */
void kraftEncodeBufferFree(kraftEncodeBuffer *buf);

/* =========================================================================
 * Protocol Operations (from kraft core TO protocol implementation)
 * ========================================================================= */

typedef struct kraftProtocolOps {
    /* ===== Lifecycle ===== */

    /* Initialize protocol instance */
    bool (*init)(struct kraftProtocol *proto);

    /* Cleanup protocol instance */
    void (*destroy)(struct kraftProtocol *proto);

    /* ===== Encoding ===== */

    /* Encode message header */
    bool (*encodeHeader)(struct kraftProtocol *proto, kraftEncodeBuffer *buf,
                         const kraftMessageHeader *header);

    /* Encode HELLO message */
    bool (*encodeHello)(struct kraftProtocol *proto, kraftEncodeBuffer *buf,
                        const kraftMsgHello *msg);

    /* Encode AppendEntries */
    bool (*encodeAppendEntries)(struct kraftProtocol *proto,
                                kraftEncodeBuffer *buf,
                                const kraftMsgAppendEntries *msg,
                                const kraftLogEntry *entries,
                                uint32_t entryCount);

    /* Encode AppendEntries Response */
    bool (*encodeAppendEntriesResp)(struct kraftProtocol *proto,
                                    kraftEncodeBuffer *buf,
                                    const kraftMsgAppendEntriesResp *msg);

    /* Encode RequestVote */
    bool (*encodeRequestVote)(struct kraftProtocol *proto,
                              kraftEncodeBuffer *buf,
                              const kraftMsgRequestVote *msg);

    /* Encode RequestVote Response */
    bool (*encodeRequestVoteResp)(struct kraftProtocol *proto,
                                  kraftEncodeBuffer *buf,
                                  const kraftMsgRequestVoteResp *msg);

    /* Encode InstallSnapshot */
    bool (*encodeInstallSnapshot)(struct kraftProtocol *proto,
                                  kraftEncodeBuffer *buf,
                                  const kraftMsgInstallSnapshot *msg,
                                  const uint8_t *snapshotData);

    /* Encode InstallSnapshot Response */
    bool (*encodeInstallSnapshotResp)(struct kraftProtocol *proto,
                                      kraftEncodeBuffer *buf,
                                      const kraftMsgInstallSnapshotResp *msg);

    /* Encode TimeoutNow */
    bool (*encodeTimeoutNow)(struct kraftProtocol *proto,
                             kraftEncodeBuffer *buf,
                             const kraftMsgTimeoutNow *msg);

    /* Encode ReadIndex */
    bool (*encodeReadIndex)(struct kraftProtocol *proto, kraftEncodeBuffer *buf,
                            const kraftMsgReadIndex *msg,
                            const uint8_t *queryData, size_t queryLen);

    /* Encode ReadIndex Response */
    bool (*encodeReadIndexResp)(struct kraftProtocol *proto,
                                kraftEncodeBuffer *buf,
                                const kraftMsgReadIndexResp *msg);

    /* Encode Client Request */
    bool (*encodeClientRequest)(struct kraftProtocol *proto,
                                kraftEncodeBuffer *buf,
                                const kraftMsgClientRequest *msg,
                                const uint8_t *commandData, size_t commandLen);

    /* Encode Client Response */
    bool (*encodeClientResponse)(struct kraftProtocol *proto,
                                 kraftEncodeBuffer *buf,
                                 const kraftMsgClientResponse *msg,
                                 const uint8_t *responseData,
                                 size_t responseLen);

    /* Encode Heartbeat */
    bool (*encodeHeartbeat)(struct kraftProtocol *proto, kraftEncodeBuffer *buf,
                            const kraftMsgHeartbeat *msg);

    /* Encode Heartbeat Response */
    bool (*encodeHeartbeatResp)(struct kraftProtocol *proto,
                                kraftEncodeBuffer *buf,
                                const kraftMsgHeartbeatResp *msg);

    /* Encode Error */
    bool (*encodeError)(struct kraftProtocol *proto, kraftEncodeBuffer *buf,
                        const kraftMsgError *msg);

    /* ===== Decoding ===== */

    /* Decode message from buffer
     * Returns decode status; fills result structure
     * On KRAFT_DECODE_NEED_MORE, caller should accumulate more data and retry
     */
    kraftDecodeStatus (*decode)(struct kraftProtocol *proto,
                                const uint8_t *data, size_t len,
                                kraftDecodeResult *result);

    /* Free decoded result (entries array, etc.) */
    void (*freeDecodeResult)(struct kraftProtocol *proto,
                             kraftDecodeResult *result);

    /* ===== Utilities ===== */

    /* Get protocol version */
    uint32_t (*getVersion)(struct kraftProtocol *proto);

    /* Get human-readable protocol name */
    const char *(*getName)(struct kraftProtocol *proto);

    /* Estimate encoded size for planning buffer allocation */
    size_t (*estimateSize)(struct kraftProtocol *proto, kraftMessageType type,
                           size_t payloadLen);

    /* Check if protocol supports a feature */
    bool (*supportsFeature)(struct kraftProtocol *proto, uint32_t feature);

} kraftProtocolOps;

/* =========================================================================
 * Protocol Features (for capability negotiation)
 * ========================================================================= */

#define KRAFT_PROTO_FEATURE_COMPRESSION (1 << 0) /* Supports compression */
#define KRAFT_PROTO_FEATURE_CHECKSUM (1 << 1)    /* Supports checksums */
#define KRAFT_PROTO_FEATURE_BATCHING (1 << 2)    /* Supports message batching */
#define KRAFT_PROTO_FEATURE_MULTIRAFT                                          \
    (1 << 3)                                 /* Supports multi-raft groupId    \
                                              */
#define KRAFT_PROTO_FEATURE_PREVOTE (1 << 4) /* Supports pre-vote */
#define KRAFT_PROTO_FEATURE_LEADERSHIP_TXF                                     \
    (1 << 5) /* Supports leadership transfer */
#define KRAFT_PROTO_FEATURE_READINDEX (1 << 6) /* Supports ReadIndex */
#define KRAFT_PROTO_FEATURE_SNAPSHOT (1 << 7)  /* Supports InstallSnapshot */

/* =========================================================================
 * Protocol Handle
 * ========================================================================= */

typedef struct kraftProtocol {
    /* Protocol implementation operations */
    const kraftProtocolOps *ops;

    /* Implementation-specific data */
    void *impl;

    /* User data */
    void *userData;

    /* Configuration */
    struct {
        bool enableCompression;  /* Compress payloads */
        bool enableChecksum;     /* Add checksums */
        uint32_t maxMessageSize; /* Maximum message size */
        uint32_t maxBatchSize;   /* Maximum batch size */
    } config;

    /* Statistics */
    struct {
        uint64_t messagesEncoded;
        uint64_t messagesDecoded;
        uint64_t bytesEncoded;
        uint64_t bytesDecoded;
        uint64_t encodeErrors;
        uint64_t decodeErrors;
        uint64_t checksumFailures;
    } stats;

} kraftProtocol;

/* =========================================================================
 * Protocol Lifecycle Functions
 * ========================================================================= */

/* Create protocol with given operations */
kraftProtocol *kraftProtocolCreate(const kraftProtocolOps *ops, void *userData);

/* Set default configuration values */
void kraftProtocolConfigDefault(kraftProtocol *proto);

/* Destroy protocol and free resources */
void kraftProtocolDestroy(kraftProtocol *proto);

/* =========================================================================
 * Convenience Functions
 * ========================================================================= */

/* Get message type name as string */
const char *kraftMessageTypeName(kraftMessageType type);

/* Check if message type is a request (vs response) */
bool kraftMessageIsRequest(kraftMessageType type);

/* Check if message type requires a response */
bool kraftMessageNeedsResponse(kraftMessageType type);

/* =========================================================================
 * Built-in Protocol Implementations
 * ========================================================================= */

/* Binary protocol (compact, fast) */
extern const kraftProtocolOps kraftProtocolBinaryOps;

/* Get binary protocol operations (for creating instances) */
const kraftProtocolOps *kraftProtocolGetBinaryOps(void);

/* =========================================================================
 * Protocol Statistics
 * ========================================================================= */

typedef struct kraftProtocolStats {
    uint64_t messagesEncoded;
    uint64_t messagesDecoded;
    uint64_t bytesEncoded;
    uint64_t bytesDecoded;
    uint64_t encodeErrors;
    uint64_t decodeErrors;
    uint64_t checksumFailures;
    double avgEncodeTimeUs;
    double avgDecodeTimeUs;
} kraftProtocolStats;

void kraftProtocolGetStats(const kraftProtocol *proto,
                           kraftProtocolStats *stats);

/* Format stats as string (returns bytes written) */
int kraftProtocolFormatStats(const kraftProtocolStats *stats, char *buf,
                             size_t bufLen);

#endif /* KRAFT_PROTOCOL_H */
