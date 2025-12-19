/* loopySession - Per-connection protocol state and handshake
 *
 * Implementation of session layer for Raft node connections.
 *
 * Wire Protocol:
 *   Each message is framed as: [4-byte length][payload]
 *   Length is little-endian, includes only payload bytes.
 *
 * Handshake Messages (first byte is message type):
 *   HELLO:    [0x01][8-byte runId][8-byte clusterId][8-byte nodeId]
 *   VERIFIED: [0x02]
 *   GOAWAY:   [0x03][1-byte reason]
 *
 * After handshake, payload is passed directly to application.
 */

#include "loopySession.h"

/* loopy headers */
#include "../../deps/loopy/src/loopy.h"
#include "../../deps/loopy/src/loopyTimer.h"

#include <arpa/inet.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Constants
 * ============================================================================
 */

/* Frame header size (4-byte length prefix) */
#define FRAME_HEADER_SIZE 4

/* Maximum message size (prevent memory exhaustion) */
#define MAX_MESSAGE_SIZE (64 * 1024 * 1024) /* 64 MB */

/* Handshake message types */
#define MSG_HELLO 0x01
#define MSG_VERIFIED 0x02
#define MSG_GOAWAY 0x03

/* HELLO message size: type(1) + runId(8) + clusterId(8) + nodeId(8) = 25 */
#define HELLO_MSG_SIZE 25

/* VERIFIED message size: type(1) = 1 */
#define VERIFIED_MSG_SIZE 1

/* GOAWAY message size: type(1) + reason(1) = 2 */
#define GOAWAY_MSG_SIZE 2

/* ============================================================================
 * Internal Structures
 * ============================================================================
 */

struct loopySession {
    /* Connection (owned by session) */
    loopyConn *conn;
    bool inbound;

    /* State machine */
    loopySessionState state;
    loopySessionRejectReason rejectReason;

    /* Identity */
    loopyNodeId localId;
    loopyNodeId peerId;
    bool peerIdValid;

    /* Callbacks */
    loopySessionCallback *callback;
    void *userData;

    /* Duplicate detection */
    loopySessionDuplicateCheck *duplicateCheck;
    void *duplicateCheckUserData;

    /* Handshake timeout */
    loopyTimer *handshakeTimer;
    uint32_t handshakeTimeoutMs;

    /* Receive buffer for message framing */
    uint8_t *recvBuf;
    size_t recvBufSize;
    size_t recvBufUsed;

    /* Statistics */
    loopySessionStats stats;
};

/* ============================================================================
 * Forward Declarations
 * ============================================================================
 */

static void sessionConnCallback(loopyConn *conn, loopyConnEvent event,
                                const void *data, size_t len, void *userData);
static void sessionHandshakeTimeout(loopyLoop *loop, loopyTimer *timer,
                                    void *userData);
static bool processReceivedData(loopySession *session, const void *data,
                                size_t len);
static bool processHandshakeMessage(loopySession *session, const uint8_t *data,
                                    size_t len);
static bool processHello(loopySession *session, const uint8_t *data,
                         size_t len);
static bool sendHello(loopySession *session);
static bool sendVerified(loopySession *session);
static bool sendGoaway(loopySession *session, loopySessionRejectReason reason);
static bool writeFrame(loopySession *session, const void *data, size_t len);

/* ============================================================================
 * Configuration
 * ============================================================================
 */

void loopySessionConfigInit(loopySessionConfig *cfg) {
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->handshakeTimeoutMs = 5000; /* 5 second default */
    loopyConnConfigInit(&cfg->connConfig);
}

void loopySessionConfigSetLocalId(loopySessionConfig *cfg, uint64_t runId,
                                  uint64_t clusterId, uint64_t nodeId) {
    if (!cfg) {
        return;
    }
    cfg->localId.runId = runId;
    cfg->localId.clusterId = clusterId;
    cfg->localId.nodeId = nodeId;
}

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

loopySession *loopySessionCreate(loopyConn *conn, bool inbound,
                                 loopySessionCallback *cb, void *userData,
                                 const loopySessionConfig *cfg) {
    if (!conn || !cfg) {
        return NULL;
    }

    loopySession *session = calloc(1, sizeof(*session));
    if (!session) {
        return NULL;
    }

    session->conn = conn;
    session->inbound = inbound;
    session->state = LOOPY_SESSION_CONNECTED;
    session->callback = cb;
    session->userData = userData;
    session->localId = cfg->localId;
    session->handshakeTimeoutMs = cfg->handshakeTimeoutMs;

    /* Initial receive buffer */
    session->recvBufSize = 4096;
    session->recvBuf = malloc(session->recvBufSize);
    if (!session->recvBuf) {
        free(session);
        return NULL;
    }

    /* Wire up connection callback */
    loopyConnSetCallback(conn, sessionConnCallback, session);

    return session;
}

bool loopySessionStartHandshake(loopySession *session) {
    if (!session) {
        return false;
    }

    if (session->state != LOOPY_SESSION_CONNECTED) {
        return false;
    }

    /* Start handshake timeout timer */
    if (session->handshakeTimeoutMs > 0) {
        loopyLoop *loop = loopyConnGetLoop(session->conn);
        if (loop) {
            session->handshakeTimer =
                loopyTimerOneShotMs(loop, session->handshakeTimeoutMs,
                                    sessionHandshakeTimeout, session);
        }
    }

    /* Send HELLO */
    if (!sendHello(session)) {
        return false;
    }

    session->state = LOOPY_SESSION_HELLO_SENT;
    return true;
}

void loopySessionClose(loopySession *session) {
    if (!session) {
        return;
    }

    /* Cancel handshake timer */
    if (session->handshakeTimer) {
        loopyTimerCancel(session->handshakeTimer);
        session->handshakeTimer = NULL;
    }

    /* Update state */
    session->state = LOOPY_SESSION_CLOSED;

    /* Close connection */
    if (session->conn) {
        loopyConnClose(session->conn);
        session->conn = NULL;
    }

    /* Free receive buffer */
    if (session->recvBuf) {
        free(session->recvBuf);
        session->recvBuf = NULL;
    }

    free(session);
}

void loopySessionReject(loopySession *session,
                        loopySessionRejectReason reason) {
    if (!session) {
        return;
    }

    if (session->state == LOOPY_SESSION_CLOSED ||
        session->state == LOOPY_SESSION_GOAWAY_SENT) {
        return;
    }

    sendGoaway(session, reason);
    session->state = LOOPY_SESSION_GOAWAY_SENT;
    session->rejectReason = reason;

    /* Notify callback about rejection */
    if (session->callback) {
        session->callback(session, LOOPY_SESSION_EVENT_REJECTED, NULL, 0,
                          session->userData);
    }

    /* Close connection - this will trigger DISCONNECTED event and cleanup.
     * Don't free session here as we may still be in the callback chain. */
    if (session->conn) {
        loopyConnClose(session->conn);
        session->conn = NULL;
    }
}

/* ============================================================================
 * Messaging
 * ============================================================================
 */

bool loopySessionSend(loopySession *session, const void *data, size_t len) {
    if (!session || !data || len == 0) {
        return false;
    }

    if (session->state != LOOPY_SESSION_VERIFIED) {
        return false;
    }

    if (!writeFrame(session, data, len)) {
        return false;
    }

    session->stats.messagesSent++;
    session->stats.bytesSent += len;
    return true;
}

bool loopySessionSendRaw(loopySession *session, const void *data, size_t len) {
    if (!session || !data || len == 0) {
        return false;
    }

    if (session->state != LOOPY_SESSION_VERIFIED) {
        return false;
    }

    if (!loopyConnWrite(session->conn, data, len)) {
        return false;
    }

    session->stats.bytesSent += len;
    return true;
}

/* ============================================================================
 * Properties
 * ============================================================================
 */

loopySessionState loopySessionGetState(const loopySession *session) {
    if (!session) {
        return LOOPY_SESSION_CLOSED;
    }
    return session->state;
}

const char *loopySessionStateName(loopySessionState state) {
    switch (state) {
    case LOOPY_SESSION_CONNECTING:
        return "CONNECTING";
    case LOOPY_SESSION_CONNECTED:
        return "CONNECTED";
    case LOOPY_SESSION_HELLO_SENT:
        return "HELLO_SENT";
    case LOOPY_SESSION_AWAIT_VERIFIED:
        return "AWAIT_VERIFIED";
    case LOOPY_SESSION_VERIFIED:
        return "VERIFIED";
    case LOOPY_SESSION_GOAWAY_SENT:
        return "GOAWAY_SENT";
    case LOOPY_SESSION_CLOSED:
        return "CLOSED";
    default:
        return "UNKNOWN";
    }
}

bool loopySessionIsReady(const loopySession *session) {
    if (!session) {
        return false;
    }
    return session->state == LOOPY_SESSION_VERIFIED;
}

bool loopySessionIsInbound(const loopySession *session) {
    if (!session) {
        return false;
    }
    return session->inbound;
}

loopyConn *loopySessionGetConn(const loopySession *session) {
    if (!session) {
        return NULL;
    }
    return session->conn;
}

const loopyNodeId *loopySessionGetLocalId(const loopySession *session) {
    if (!session) {
        return NULL;
    }
    return &session->localId;
}

const loopyNodeId *loopySessionGetPeerId(const loopySession *session) {
    if (!session || !session->peerIdValid) {
        return NULL;
    }
    return &session->peerId;
}

loopySessionRejectReason
loopySessionGetRejectReason(const loopySession *session) {
    if (!session) {
        return LOOPY_REJECT_NONE;
    }
    return session->rejectReason;
}

const char *loopySessionRejectReasonName(loopySessionRejectReason reason) {
    switch (reason) {
    case LOOPY_REJECT_NONE:
        return "NONE";
    case LOOPY_REJECT_SELF_CONNECTION:
        return "SELF_CONNECTION";
    case LOOPY_REJECT_CLUSTER_MISMATCH:
        return "CLUSTER_MISMATCH";
    case LOOPY_REJECT_DUPLICATE:
        return "DUPLICATE";
    case LOOPY_REJECT_PROTOCOL_ERROR:
        return "PROTOCOL_ERROR";
    case LOOPY_REJECT_TIMEOUT:
        return "TIMEOUT";
    default:
        return "UNKNOWN";
    }
}

void *loopySessionGetUserData(const loopySession *session) {
    if (!session) {
        return NULL;
    }
    return session->userData;
}

void loopySessionSetUserData(loopySession *session, void *userData) {
    if (!session) {
        return;
    }
    session->userData = userData;
}

void loopySessionGetStats(const loopySession *session,
                          loopySessionStats *stats) {
    if (!stats) {
        return;
    }

    if (!session) {
        memset(stats, 0, sizeof(*stats));
        return;
    }

    *stats = session->stats;
}

/* ============================================================================
 * Validation
 * ============================================================================
 */

void loopySessionSetDuplicateCheck(loopySession *session,
                                   loopySessionDuplicateCheck *check,
                                   void *userData) {
    if (!session) {
        return;
    }
    session->duplicateCheck = check;
    session->duplicateCheckUserData = userData;
}

/* ============================================================================
 * Internal: Connection Callback
 * ============================================================================
 */

static void sessionConnCallback(loopyConn *conn, loopyConnEvent event,
                                const void *data, size_t len, void *userData) {
    loopySession *session = (loopySession *)userData;
    (void)conn;

    switch (event) {
    case LOOPY_CONN_EVENT_CONNECTED:
        /* Connection established - notify and wait for handshake start */
        if (session->callback) {
            session->callback(session, LOOPY_SESSION_EVENT_CONNECTED, NULL, 0,
                              session->userData);
        }
        break;

    case LOOPY_CONN_EVENT_DATA:
        /* Process received data */
        if (!processReceivedData(session, data, len)) {
            /* Protocol error - close session */
            loopySessionReject(session, LOOPY_REJECT_PROTOCOL_ERROR);
        }
        break;

    case LOOPY_CONN_EVENT_CLOSE:
    case LOOPY_CONN_EVENT_ERROR:
        /* Connection closed or errored - NULL out reference since
         * the connection will be freed by the close callback */
        session->conn = NULL;
        session->state = LOOPY_SESSION_CLOSED;
        if (session->callback) {
            session->callback(session, LOOPY_SESSION_EVENT_DISCONNECTED, NULL,
                              0, session->userData);
        }
        break;

    default:
        break;
    }
}

/* ============================================================================
 * Internal: Handshake Timeout
 * ============================================================================
 */

static void sessionHandshakeTimeout(loopyLoop *loop, loopyTimer *timer,
                                    void *userData) {
    loopySession *session = (loopySession *)userData;
    (void)loop;
    (void)timer;

    session->handshakeTimer = NULL; /* One-shot, auto-cleaned */

    /* If still in handshake (any state before VERIFIED), timeout */
    if (session->state == LOOPY_SESSION_HELLO_SENT ||
        session->state == LOOPY_SESSION_CONNECTED ||
        session->state == LOOPY_SESSION_AWAIT_VERIFIED) {
        loopySessionReject(session, LOOPY_REJECT_TIMEOUT);
    }
}

/* ============================================================================
 * Internal: Message Framing
 * ============================================================================
 */

static bool ensureRecvBuffer(loopySession *session, size_t needed) {
    if (session->recvBufSize >= needed) {
        return true;
    }

    size_t newSize = session->recvBufSize;
    while (newSize < needed) {
        newSize *= 2;
    }

    uint8_t *newBuf = realloc(session->recvBuf, newSize);
    if (!newBuf) {
        return false;
    }

    session->recvBuf = newBuf;
    session->recvBufSize = newSize;
    return true;
}

static bool processReceivedData(loopySession *session, const void *data,
                                size_t len) {
    if (!session || !data || len == 0) {
        return true; /* No data to process */
    }

    /* Bail out if session is already closed or rejected */
    if (session->state == LOOPY_SESSION_CLOSED ||
        session->state == LOOPY_SESSION_GOAWAY_SENT) {
        return true; /* Ignore data */
    }

    session->stats.bytesReceived += len;

    /* Append to receive buffer */
    if (!ensureRecvBuffer(session, session->recvBufUsed + len)) {
        return false; /* Out of memory */
    }

    memcpy(session->recvBuf + session->recvBufUsed, data, len);
    session->recvBufUsed += len;

    /* Process complete frames */
    while (session->recvBufUsed >= FRAME_HEADER_SIZE) {
        /* Read length prefix (little-endian) */
        uint32_t frameLen = 0;
        frameLen |= (uint32_t)session->recvBuf[0];
        frameLen |= (uint32_t)session->recvBuf[1] << 8;
        frameLen |= (uint32_t)session->recvBuf[2] << 16;
        frameLen |= (uint32_t)session->recvBuf[3] << 24;

        /* Sanity check */
        if (frameLen > MAX_MESSAGE_SIZE) {
            return false; /* Message too large */
        }

        /* Check if we have complete frame */
        size_t totalSize = FRAME_HEADER_SIZE + frameLen;
        if (session->recvBufUsed < totalSize) {
            break; /* Need more data */
        }

        /* Extract payload */
        uint8_t *payload = session->recvBuf + FRAME_HEADER_SIZE;

        /* Process based on state */
        if (session->state == LOOPY_SESSION_HELLO_SENT ||
            session->state == LOOPY_SESSION_CONNECTED ||
            session->state == LOOPY_SESSION_AWAIT_VERIFIED) {
            /* In handshake - process as handshake message */
            if (!processHandshakeMessage(session, payload, frameLen)) {
                return false;
            }
        } else if (session->state == LOOPY_SESSION_VERIFIED) {
            /* Verified - deliver to application */
            session->stats.messagesReceived++;
            if (session->callback) {
                session->callback(session, LOOPY_SESSION_EVENT_MESSAGE, payload,
                                  frameLen, session->userData);
            }
        } else {
            /* Invalid state for receiving messages */
            return false;
        }

        /* Remove processed frame from buffer */
        size_t remaining = session->recvBufUsed - totalSize;
        if (remaining > 0) {
            memmove(session->recvBuf, session->recvBuf + totalSize, remaining);
        }
        session->recvBufUsed = remaining;
    }

    return true;
}

/* ============================================================================
 * Internal: Handshake Protocol
 * ============================================================================
 */

static bool processHandshakeMessage(loopySession *session, const uint8_t *data,
                                    size_t len) {
    if (len < 1) {
        return false; /* No message type */
    }

    uint8_t msgType = data[0];

    switch (msgType) {
    case MSG_HELLO:
        /* Only accept HELLO in states where we haven't received one yet */
        if (session->state != LOOPY_SESSION_HELLO_SENT &&
            session->state != LOOPY_SESSION_CONNECTED) {
            /* Already processed HELLO (in AWAIT_VERIFIED) - ignore duplicate */
            return true;
        }
        return processHello(session, data, len);

    case MSG_VERIFIED:
        /* Only transition to VERIFIED if we're in AWAIT_VERIFIED
         * (meaning we've already sent our VERIFIED after receiving HELLO) */
        if (session->state != LOOPY_SESSION_AWAIT_VERIFIED) {
            /* Received VERIFIED before HELLO - protocol error */
            return false;
        }
        /* Peer has verified us - handshake complete */
        if (session->handshakeTimer) {
            loopyTimerCancel(session->handshakeTimer);
            session->handshakeTimer = NULL;
        }
        session->state = LOOPY_SESSION_VERIFIED;
        if (session->callback) {
            session->callback(session, LOOPY_SESSION_EVENT_VERIFIED, NULL, 0,
                              session->userData);
        }
        return true;

    case MSG_GOAWAY:
        /* Peer rejected us */
        if (len >= 2) {
            session->rejectReason = (loopySessionRejectReason)data[1];
        }
        session->state = LOOPY_SESSION_CLOSED;
        if (session->callback) {
            session->callback(session, LOOPY_SESSION_EVENT_REJECTED, NULL, 0,
                              session->userData);
        }
        return true;

    default:
        return false; /* Unknown message type */
    }
}

static bool processHello(loopySession *session, const uint8_t *data,
                         size_t len) {
    if (len < HELLO_MSG_SIZE) {
        return false; /* Truncated HELLO */
    }

    /* Parse HELLO: type(1) + runId(8) + clusterId(8) + nodeId(8) */
    const uint8_t *p = data + 1; /* Skip type byte */

    uint64_t peerRunId = 0;
    for (int i = 0; i < 8; i++) {
        peerRunId |= (uint64_t)p[i] << (i * 8);
    }
    p += 8;

    uint64_t peerClusterId = 0;
    for (int i = 0; i < 8; i++) {
        peerClusterId |= (uint64_t)p[i] << (i * 8);
    }
    p += 8;

    uint64_t peerNodeId = 0;
    for (int i = 0; i < 8; i++) {
        peerNodeId |= (uint64_t)p[i] << (i * 8);
    }

    /* Store peer identity */
    session->peerId.runId = peerRunId;
    session->peerId.clusterId = peerClusterId;
    session->peerId.nodeId = peerNodeId;
    session->peerIdValid = true;

    /* Validation 1: Check for self-connection */
    if (peerRunId == session->localId.runId) {
        loopySessionReject(session, LOOPY_REJECT_SELF_CONNECTION);
        return true; /* Message was valid, just rejected */
    }

    /* Validation 2: Check cluster ID */
    if (peerClusterId != session->localId.clusterId) {
        loopySessionReject(session, LOOPY_REJECT_CLUSTER_MISMATCH);
        return true;
    }

    /* Validation 3: Check for duplicate connection */
    if (session->duplicateCheck) {
        if (session->duplicateCheck(&session->peerId,
                                    session->duplicateCheckUserData)) {
            loopySessionReject(session, LOOPY_REJECT_DUPLICATE);
            return true;
        }
    }

    /* All validations passed - send VERIFIED */
    if (!sendVerified(session)) {
        return false;
    }

    /* If we haven't sent HELLO yet, do so now (inbound connection case) */
    if (session->state == LOOPY_SESSION_CONNECTED) {
        if (!sendHello(session)) {
            return false;
        }
    }

    /* Transition to AWAIT_VERIFIED - we've sent our VERIFIED, now waiting
     * for the peer's VERIFIED to complete the handshake */
    session->state = LOOPY_SESSION_AWAIT_VERIFIED;

    /* Note: Don't cancel timeout yet - we still need peer's VERIFIED */
    /* Note: Don't fire VERIFIED callback yet - handshake not complete */

    return true;
}

/* ============================================================================
 * Internal: Message Sending
 * ============================================================================
 */

static bool writeFrame(loopySession *session, const void *data, size_t len) {
    if (!session || !session->conn) {
        return false;
    }

    /* Write length prefix (little-endian) */
    uint8_t header[FRAME_HEADER_SIZE];
    header[0] = (uint8_t)(len & 0xFF);
    header[1] = (uint8_t)((len >> 8) & 0xFF);
    header[2] = (uint8_t)((len >> 16) & 0xFF);
    header[3] = (uint8_t)((len >> 24) & 0xFF);

    if (!loopyConnWrite(session->conn, header, FRAME_HEADER_SIZE)) {
        return false;
    }

    if (len > 0 && data) {
        if (!loopyConnWrite(session->conn, data, len)) {
            return false;
        }
    }

    return true;
}

static bool sendHello(loopySession *session) {
    /* Build HELLO message: type(1) + runId(8) + clusterId(8) + nodeId(8) */
    uint8_t msg[HELLO_MSG_SIZE];
    uint8_t *p = msg;

    *p++ = MSG_HELLO;

    /* runId (little-endian) */
    uint64_t runId = session->localId.runId;
    for (int i = 0; i < 8; i++) {
        *p++ = (uint8_t)(runId >> (i * 8));
    }

    /* clusterId (little-endian) */
    uint64_t clusterId = session->localId.clusterId;
    for (int i = 0; i < 8; i++) {
        *p++ = (uint8_t)(clusterId >> (i * 8));
    }

    /* nodeId (little-endian) */
    uint64_t nodeId = session->localId.nodeId;
    for (int i = 0; i < 8; i++) {
        *p++ = (uint8_t)(nodeId >> (i * 8));
    }

    return writeFrame(session, msg, HELLO_MSG_SIZE);
}

static bool sendVerified(loopySession *session) {
    uint8_t msg[VERIFIED_MSG_SIZE];
    msg[0] = MSG_VERIFIED;
    return writeFrame(session, msg, VERIFIED_MSG_SIZE);
}

static bool sendGoaway(loopySession *session, loopySessionRejectReason reason) {
    uint8_t msg[GOAWAY_MSG_SIZE];
    msg[0] = MSG_GOAWAY;
    msg[1] = (uint8_t)reason;
    return writeFrame(session, msg, GOAWAY_MSG_SIZE);
}
