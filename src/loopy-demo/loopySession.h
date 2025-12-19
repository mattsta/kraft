/* loopySession - Per-connection protocol state and handshake
 *
 * Manages the session layer for Raft node connections:
 *   - Protocol state machine (CONNECTING -> HELLO_SENT -> VERIFIED)
 *   - HELLO/VERIFIED/GOAWAY handshake protocol
 *   - Message framing and buffering
 *   - Connection identity (runId, clusterId, nodeId)
 *
 * Handshake Flow:
 *   1. Connection established (inbound or outbound)
 *   2. Both sides send HELLO with their identity
 *   3. Each side validates the peer:
 *      - Same clusterId? (must match)
 *      - Different runId? (can't be self)
 *      - Not a duplicate? (no existing connection to same node)
 *   4. On success: send VERIFIED, transition to ready state
 *   5. On failure: send GOAWAY, close connection
 *
 * This is part of loopy-demo, a consumer of kraft (not part of kraft itself).
 */

#ifndef LOOPY_SESSION_H
#define LOOPY_SESSION_H

#include "loopyConnection.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ============================================================================
 * Types
 * ============================================================================
 */

/**
 * Session handle.
 *
 * Wraps a loopyConn with protocol-level state.
 */
typedef struct loopySession loopySession;

/**
 * Session state machine.
 *
 * Handshake flow:
 *   CONNECTED -> (send HELLO) -> HELLO_SENT -> (recv HELLO, send VERIFIED) ->
 *   AWAIT_VERIFIED -> (recv VERIFIED) -> VERIFIED
 */
typedef enum loopySessionState {
    LOOPY_SESSION_CONNECTING,     /* Connection in progress */
    LOOPY_SESSION_CONNECTED,      /* TCP connected, handshake not started */
    LOOPY_SESSION_HELLO_SENT,     /* HELLO sent, awaiting peer's HELLO */
    LOOPY_SESSION_AWAIT_VERIFIED, /* VERIFIED sent, awaiting peer's VERIFIED */
    LOOPY_SESSION_VERIFIED,       /* Fully verified, ready for RPCs */
    LOOPY_SESSION_GOAWAY_SENT,    /* GOAWAY sent, closing */
    LOOPY_SESSION_CLOSED          /* Session closed */
} loopySessionState;

/**
 * Session event types.
 */
typedef enum loopySessionEvent {
    LOOPY_SESSION_EVENT_CONNECTED,    /* TCP connection established */
    LOOPY_SESSION_EVENT_VERIFIED,     /* Handshake completed successfully */
    LOOPY_SESSION_EVENT_REJECTED,     /* Handshake rejected (GOAWAY received) */
    LOOPY_SESSION_EVENT_MESSAGE,      /* Complete RPC message received */
    LOOPY_SESSION_EVENT_DISCONNECTED, /* Connection closed or errored */
    LOOPY_SESSION_EVENT_ERROR         /* Protocol error */
} loopySessionEvent;

/**
 * Rejection reasons (why GOAWAY was sent/received).
 */
typedef enum loopySessionRejectReason {
    LOOPY_REJECT_NONE = 0,
    LOOPY_REJECT_SELF_CONNECTION,  /* Connected to ourselves */
    LOOPY_REJECT_CLUSTER_MISMATCH, /* Different cluster ID */
    LOOPY_REJECT_DUPLICATE,        /* Already have connection to this node */
    LOOPY_REJECT_PROTOCOL_ERROR,   /* Invalid handshake message */
    LOOPY_REJECT_TIMEOUT           /* Handshake timed out */
} loopySessionRejectReason;

/**
 * Node identity information.
 */
typedef struct loopyNodeId {
    uint64_t runId;     /* Unique instance ID (changes on restart) */
    uint64_t clusterId; /* Cluster this node belongs to */
    uint64_t nodeId;    /* Stable node identifier */
    char addr[64];      /* Node address (for reconnection) */
    int port;           /* Node port */
} loopyNodeId;

/**
 * Session event callback.
 *
 * @param session   Session handle
 * @param event     Event type
 * @param data      For MESSAGE: pointer to message data
 * @param len       For MESSAGE: message length
 * @param userData  User context
 */
typedef void loopySessionCallback(loopySession *session,
                                  loopySessionEvent event, const void *data,
                                  size_t len, void *userData);

/**
 * Session configuration.
 */
typedef struct loopySessionConfig {
    /* Our identity (sent in HELLO) */
    loopyNodeId localId;

    /* Handshake timeout in milliseconds (default: 5000ms) */
    uint32_t handshakeTimeoutMs;

    /* Connection config (passed to loopyConn) */
    loopyConnConfig connConfig;
} loopySessionConfig;

/* ============================================================================
 * Configuration
 * ============================================================================
 */

/**
 * Initialize config with defaults.
 */
void loopySessionConfigInit(loopySessionConfig *cfg);

/**
 * Set local node identity.
 */
void loopySessionConfigSetLocalId(loopySessionConfig *cfg, uint64_t runId,
                                  uint64_t clusterId, uint64_t nodeId);

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

/**
 * Create session from an existing connection.
 *
 * Takes ownership of the connection. Used by transport layer
 * after accepting or establishing a connection.
 *
 * @param conn     Connection (ownership transferred)
 * @param inbound  true if this was an accepted connection
 * @param cb       Event callback
 * @param userData User context for callback
 * @param cfg      Configuration
 * @return Session handle, or NULL on error
 */
loopySession *loopySessionCreate(loopyConn *conn, bool inbound,
                                 loopySessionCallback *cb, void *userData,
                                 const loopySessionConfig *cfg);

/**
 * Start the handshake.
 *
 * Sends HELLO message and starts handshake timeout.
 * Call this after creating a session.
 *
 * @param session Session handle
 * @return true if HELLO sent
 */
bool loopySessionStartHandshake(loopySession *session);

/**
 * Close the session.
 *
 * Gracefully closes the connection.
 *
 * @param session Session (may be NULL)
 */
void loopySessionClose(loopySession *session);

/**
 * Close with GOAWAY.
 *
 * Sends GOAWAY message before closing.
 *
 * @param session Session (may be NULL)
 * @param reason  Rejection reason
 */
void loopySessionReject(loopySession *session, loopySessionRejectReason reason);

/* ============================================================================
 * Messaging
 * ============================================================================
 */

/**
 * Send a message.
 *
 * Only valid when session is VERIFIED.
 *
 * @param session Session handle
 * @param data    Message data
 * @param len     Message length
 * @return true if queued successfully
 */
bool loopySessionSend(loopySession *session, const void *data, size_t len);

/**
 * Send raw bytes (bypasses framing).
 *
 * For sending pre-encoded RPC messages.
 *
 * @param session Session handle
 * @param data    Raw bytes
 * @param len     Length
 * @return true if queued
 */
bool loopySessionSendRaw(loopySession *session, const void *data, size_t len);

/* ============================================================================
 * Properties
 * ============================================================================
 */

/**
 * Get session state.
 */
loopySessionState loopySessionGetState(const loopySession *session);

/**
 * Get state name string.
 */
const char *loopySessionStateName(loopySessionState state);

/**
 * Check if session is verified and ready for RPCs.
 */
bool loopySessionIsReady(const loopySession *session);

/**
 * Check if this is an inbound (accepted) session.
 */
bool loopySessionIsInbound(const loopySession *session);

/**
 * Get the underlying connection.
 */
loopyConn *loopySessionGetConn(const loopySession *session);

/**
 * Get local identity.
 */
const loopyNodeId *loopySessionGetLocalId(const loopySession *session);

/**
 * Get peer identity (valid after VERIFIED).
 */
const loopyNodeId *loopySessionGetPeerId(const loopySession *session);

/**
 * Get rejection reason (if rejected).
 */
loopySessionRejectReason
loopySessionGetRejectReason(const loopySession *session);

/**
 * Get reject reason name string.
 */
const char *loopySessionRejectReasonName(loopySessionRejectReason reason);

/**
 * Get/set user data.
 */
void *loopySessionGetUserData(const loopySession *session);
void loopySessionSetUserData(loopySession *session, void *userData);

/**
 * Session statistics.
 */
typedef struct loopySessionStats {
    uint64_t messagesSent;
    uint64_t messagesReceived;
    uint64_t bytesSent;
    uint64_t bytesReceived;
} loopySessionStats;

void loopySessionGetStats(const loopySession *session,
                          loopySessionStats *stats);

/* ============================================================================
 * Validation Helpers
 *
 * These are used during handshake to validate peer connections.
 * ============================================================================
 */

/**
 * Validator callback for duplicate detection.
 *
 * Called during handshake to check if we already have a session
 * with the same peer.
 *
 * @param peerId   Peer's identity
 * @param userData User context
 * @return true if connection should be rejected (duplicate found)
 */
typedef bool loopySessionDuplicateCheck(const loopyNodeId *peerId,
                                        void *userData);

/**
 * Set duplicate checker.
 *
 * @param session Session handle
 * @param check   Callback function
 * @param userData User context for callback
 */
void loopySessionSetDuplicateCheck(loopySession *session,
                                   loopySessionDuplicateCheck *check,
                                   void *userData);

#endif /* LOOPY_SESSION_H */
