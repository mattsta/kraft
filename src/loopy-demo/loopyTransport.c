/* loopyTransport - TCP transport layer for Raft networking
 *
 * Implementation of TCP server and outbound connection management.
 */

#include "loopyTransport.h"

/* loopy headers */
#include "../../deps/loopy/src/loopy.h"
#include "../../deps/loopy/src/loopyStream.h"
#include "../../deps/loopy/src/loopyTimer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Internal Structures
 * ============================================================================
 */

/* Peer entry for outbound connections */
typedef struct loopyPeerEntry {
    loopyPeerId id;
    char addr[64];
    int port;
    loopyConn *conn;
    loopyTimer *reconnectTimer;
    uint32_t reconnectAttempts;
    uint32_t currentDelay;
    bool shouldReconnect;
    struct loopyPeerEntry *next;
} loopyPeerEntry;

/* Inbound connection entry */
typedef struct loopyInboundEntry {
    loopyConn *conn;
    struct loopyInboundEntry *next;
} loopyInboundEntry;

struct loopyTransport {
    loopyLoop *loop;
    loopyTransportCallback *callback;
    void *userData;
    loopyTransportConfig config;
    loopyTransportStats stats;

    /* Server */
    loopyStream *server;
    bool running;

    /* Connections */
    loopyInboundEntry *inbound; /* Linked list of inbound */
    loopyPeerEntry *peers;      /* Linked list of peers */
    size_t inboundCount;
    size_t outboundCount;
};

/* ============================================================================
 * Internal Helpers
 * ============================================================================
 */

static loopyPeerEntry *findPeer(loopyTransport *t, loopyPeerId id) {
    for (loopyPeerEntry *p = t->peers; p; p = p->next) {
        if (p->id == id) {
            return p;
        }
    }
    return NULL;
}

static void removePeerEntry(loopyTransport *t, loopyPeerEntry *entry) {
    loopyPeerEntry **pp = &t->peers;
    while (*pp) {
        if (*pp == entry) {
            *pp = entry->next;
            if (entry->reconnectTimer) {
                loopyTimerCancel(entry->reconnectTimer);
            }
            free(entry);
            return;
        }
        pp = &(*pp)->next;
    }
}

static loopyInboundEntry *addInbound(loopyTransport *t, loopyConn *conn) {
    loopyInboundEntry *entry = calloc(1, sizeof(*entry));
    if (!entry) {
        return NULL;
    }
    entry->conn = conn;
    entry->next = t->inbound;
    t->inbound = entry;
    t->inboundCount++;
    return entry;
}

static void removeInbound(loopyTransport *t, loopyConn *conn) {
    loopyInboundEntry **pp = &t->inbound;
    while (*pp) {
        if ((*pp)->conn == conn) {
            loopyInboundEntry *entry = *pp;
            *pp = entry->next;
            free(entry);
            t->inboundCount--;
            return;
        }
        pp = &(*pp)->next;
    }
}

/* ============================================================================
 * Connection Callbacks
 * ============================================================================
 */

/* Context for connection callbacks to identify the transport and type */
typedef struct {
    loopyTransport *transport;
    loopyTransportConnType type;
    loopyPeerId peerId; /* Only for outbound */
} ConnContext;

static void peerConnCallback(loopyConn *conn, loopyConnEvent event,
                             const void *data, size_t len, void *userData);

static void scheduleReconnect(loopyTransport *t, loopyPeerEntry *peer);

static void inboundConnCallback(loopyConn *conn, loopyConnEvent event,
                                const void *data, size_t len, void *userData) {
    ConnContext *ctx = (ConnContext *)userData;
    loopyTransport *t = ctx->transport;

    switch (event) {
    case LOOPY_CONN_EVENT_DATA:
        if (t->callback) {
            t->callback(t, LOOPY_TRANSPORT_DATA, conn, LOOPY_CONN_INBOUND, data,
                        len, t->userData);
        }
        break;

    case LOOPY_CONN_EVENT_CLOSE:
    case LOOPY_CONN_EVENT_ERROR:
        removeInbound(t, conn);
        t->stats.totalDisconnected++;

        if (t->callback) {
            t->callback(t, LOOPY_TRANSPORT_DISCONNECTED, conn,
                        LOOPY_CONN_INBOUND, NULL, 0, t->userData);
        }

        free(ctx);
        break;

    default:
        break;
    }
}

static void peerConnCallback(loopyConn *conn, loopyConnEvent event,
                             const void *data, size_t len, void *userData) {
    ConnContext *ctx = (ConnContext *)userData;
    loopyTransport *t = ctx->transport;
    loopyPeerEntry *peer = findPeer(t, ctx->peerId);

    switch (event) {
    case LOOPY_CONN_EVENT_CONNECTED:
        if (peer) {
            peer->reconnectAttempts = 0;
            peer->currentDelay = t->config.reconnectDelayMs;
            t->outboundCount++;
        }
        t->stats.totalConnected++;

        if (t->callback) {
            t->callback(t, LOOPY_TRANSPORT_CONNECTED, conn, LOOPY_CONN_OUTBOUND,
                        NULL, 0, t->userData);
        }
        break;

    case LOOPY_CONN_EVENT_DATA:
        if (t->callback) {
            t->callback(t, LOOPY_TRANSPORT_DATA, conn, LOOPY_CONN_OUTBOUND,
                        data, len, t->userData);
        }
        break;

    case LOOPY_CONN_EVENT_CLOSE:
    case LOOPY_CONN_EVENT_ERROR:
        if (peer) {
            peer->conn = NULL;
            if (t->outboundCount > 0) {
                t->outboundCount--;
            }
        }
        t->stats.totalDisconnected++;

        if (t->callback) {
            t->callback(t, LOOPY_TRANSPORT_DISCONNECTED, conn,
                        LOOPY_CONN_OUTBOUND, NULL, 0, t->userData);
        }

        /* Schedule reconnect if enabled */
        if (peer && peer->shouldReconnect && t->config.autoReconnect) {
            scheduleReconnect(t, peer);
        } else {
            free(ctx);
        }
        break;

    default:
        break;
    }
}

/* ============================================================================
 * Reconnection Logic
 * ============================================================================
 */

static void reconnectTimerCallback(loopyLoop *loop, loopyTimer *timer,
                                   void *userData) {
    ConnContext *ctx = (ConnContext *)userData;
    loopyTransport *t = ctx->transport;
    loopyPeerEntry *peer = findPeer(t, ctx->peerId);
    (void)loop;
    (void)timer;

    if (!peer || !peer->shouldReconnect) {
        free(ctx);
        return;
    }

    peer->reconnectTimer = NULL;

    /* Check max attempts */
    if (t->config.reconnectMaxAttempts > 0 &&
        peer->reconnectAttempts >= t->config.reconnectMaxAttempts) {
        peer->shouldReconnect = false;
        free(ctx);
        return;
    }

    peer->reconnectAttempts++;
    t->stats.reconnectAttempts++;

    if (t->callback) {
        t->callback(t, LOOPY_TRANSPORT_RECONNECTING, NULL, LOOPY_CONN_OUTBOUND,
                    NULL, 0, t->userData);
    }

    /* Attempt reconnection */
    peer->conn = loopyConnConnect(t->loop, peer->addr, peer->port,
                                  peerConnCallback, ctx, &t->config.connConfig);

    if (!peer->conn) {
        /* Failed immediately, schedule another retry */
        scheduleReconnect(t, peer);
        free(ctx);
    }
}

static void scheduleReconnect(loopyTransport *t, loopyPeerEntry *peer) {
    if (peer->reconnectTimer) {
        return; /* Already scheduled */
    }

    /* Create context for callback */
    ConnContext *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        return;
    }
    ctx->transport = t;
    ctx->type = LOOPY_CONN_OUTBOUND;
    ctx->peerId = peer->id;

    /* Exponential backoff */
    uint32_t delay = peer->currentDelay;
    peer->currentDelay = (peer->currentDelay * 2);
    if (peer->currentDelay > t->config.reconnectMaxDelayMs) {
        peer->currentDelay = t->config.reconnectMaxDelayMs;
    }

    peer->reconnectTimer =
        loopyTimerOneShotMs(t->loop, delay, reconnectTimerCallback, ctx);
    if (!peer->reconnectTimer) {
        free(ctx);
    }
}

/* ============================================================================
 * Server Accept Callback
 * ============================================================================
 */

static void serverAcceptCallback(loopyStream *server, int status,
                                 void *userData) {
    loopyTransport *t = (loopyTransport *)userData;

    if (status < 0) {
        return;
    }

    loopyStream *clientStream = loopyStreamAccept(server);
    if (!clientStream) {
        return;
    }

    /* Create context for callbacks */
    ConnContext *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        loopyStreamClose(clientStream, NULL, NULL);
        return;
    }
    ctx->transport = t;
    ctx->type = LOOPY_CONN_INBOUND;
    ctx->peerId = 0;

    /* Wrap in loopyConn */
    loopyConn *conn = loopyConnFromStream(clientStream, inboundConnCallback,
                                          ctx, &t->config.connConfig);
    if (!conn) {
        free(ctx);
        loopyStreamClose(clientStream, NULL, NULL);
        return;
    }

    /* Track the connection */
    if (!addInbound(t, conn)) {
        free(ctx);
        loopyConnCloseImmediate(conn);
        return;
    }

    t->stats.totalAccepted++;

    if (t->callback) {
        t->callback(t, LOOPY_TRANSPORT_NEW_CONNECTION, conn, LOOPY_CONN_INBOUND,
                    NULL, 0, t->userData);
    }
}

/* ============================================================================
 * Configuration
 * ============================================================================
 */

void loopyTransportConfigInit(loopyTransportConfig *cfg) {
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));

    cfg->bindAddr = "0.0.0.0";
    cfg->port = 0; /* Must be set by user */
    cfg->backlog = 128;

    loopyConnConfigInit(&cfg->connConfig);

    cfg->autoReconnect = true;
    cfg->reconnectDelayMs = 100;
    cfg->reconnectMaxDelayMs = 5000;
    cfg->reconnectMaxAttempts = 0; /* Unlimited */
}

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

loopyTransport *loopyTransportCreate(loopyLoop *loop,
                                     loopyTransportCallback *cb, void *userData,
                                     const loopyTransportConfig *cfg) {
    if (!loop || !cfg || cfg->port <= 0) {
        return NULL;
    }

    loopyTransport *t = calloc(1, sizeof(*t));
    if (!t) {
        return NULL;
    }

    t->loop = loop;
    t->callback = cb;
    t->userData = userData;
    t->config = *cfg;

    /* Create server stream */
    t->server = loopyStreamNewTcp(loop);
    if (!t->server) {
        free(t);
        return NULL;
    }

    /* Bind to address */
    fprintf(stderr, "[TRANSPORT] Binding to %s:%d...\n",
            cfg->bindAddr ? cfg->bindAddr : "(null)", cfg->port);
    if (!loopyStreamBind(t->server, cfg->bindAddr, cfg->port)) {
        fprintf(stderr, "[TRANSPORT] ERROR: loopyStreamBind failed for %s:%d\n",
                cfg->bindAddr ? cfg->bindAddr : "(null)", cfg->port);
        loopyStreamClose(t->server, NULL, NULL);
        free(t);
        return NULL;
    }
    fprintf(stderr, "[TRANSPORT] Bind successful\n");

    return t;
}

bool loopyTransportStart(loopyTransport *transport) {
    if (!transport || transport->running) {
        return false;
    }

    if (!loopyStreamListen(transport->server, transport->config.backlog,
                           serverAcceptCallback, transport)) {
        return false;
    }

    transport->running = true;
    return true;
}

void loopyTransportStop(loopyTransport *transport) {
    if (!transport || !transport->running) {
        return;
    }

    /* Stop accepting new connections by closing the server */
    if (transport->server) {
        loopyStreamClose(transport->server, NULL, NULL);
        transport->server = NULL;
    }

    transport->running = false;
}

void loopyTransportDestroy(loopyTransport *transport) {
    if (!transport) {
        return;
    }

    /* Close server */
    if (transport->server) {
        loopyStreamClose(transport->server, NULL, NULL);
        transport->server = NULL;
    }

    /* Free inbound connection tracking.
     *
     * Note: We do NOT try to close connections here. Connections may have
     * already been closed and freed via the deferred close mechanism during
     * normal event loop operation. The entry->conn pointers may be stale.
     *
     * Proper shutdown sequence is:
     * 1. Stop accepting new connections (loopyTransportStop)
     * 2. Close all connections gracefully (let deferred closes complete)
     * 3. When all connections are closed, entries are removed via callbacks
     * 4. Then call destroy to free any remaining tracking structures
     *
     * If destroy is called with connections still active, they will leak.
     * This is acceptable for shutdown - the OS will reclaim resources.
     */
    while (transport->inbound) {
        loopyInboundEntry *entry = transport->inbound;
        transport->inbound = entry->next;
        /* Don't close - just free tracking structure */
        free(entry);
    }

    /* Free peer connection tracking and cancel reconnect timers */
    while (transport->peers) {
        loopyPeerEntry *entry = transport->peers;
        transport->peers = entry->next;
        if (entry->reconnectTimer) {
            loopyTimerCancel(entry->reconnectTimer);
        }
        /* Don't close entry->conn - may be stale pointer */
        free(entry);
    }

    free(transport);
}

/* ============================================================================
 * Outbound Connections
 * ============================================================================
 */

bool loopyTransportConnect(loopyTransport *transport, loopyPeerId peerId,
                           const char *addr, int port) {
    if (!transport || !addr || port <= 0) {
        return false;
    }

    /* Check if peer already exists */
    if (findPeer(transport, peerId)) {
        return false; /* Already have this peer */
    }

    /* Create peer entry */
    loopyPeerEntry *peer = calloc(1, sizeof(*peer));
    if (!peer) {
        return false;
    }

    peer->id = peerId;
    strncpy(peer->addr, addr, sizeof(peer->addr) - 1);
    peer->port = port;
    peer->shouldReconnect = true;
    peer->currentDelay = transport->config.reconnectDelayMs;

    /* Add to list */
    peer->next = transport->peers;
    transport->peers = peer;

    /* Create context for callbacks */
    ConnContext *ctx = calloc(1, sizeof(*ctx));
    if (!ctx) {
        removePeerEntry(transport, peer);
        return false;
    }
    ctx->transport = transport;
    ctx->type = LOOPY_CONN_OUTBOUND;
    ctx->peerId = peerId;

    /* Initiate connection */
    peer->conn = loopyConnConnect(transport->loop, addr, port, peerConnCallback,
                                  ctx, &transport->config.connConfig);

    if (!peer->conn) {
        /* Connection failed to start, but we can schedule reconnect */
        if (transport->config.autoReconnect) {
            scheduleReconnect(transport, peer);
        } else {
            removePeerEntry(transport, peer);
            free(ctx);
            return false;
        }
    }

    return true;
}

void loopyTransportDisconnect(loopyTransport *transport, loopyPeerId peerId) {
    if (!transport) {
        return;
    }

    loopyPeerEntry *peer = findPeer(transport, peerId);
    if (!peer) {
        return;
    }

    /* Disable reconnection */
    peer->shouldReconnect = false;

    /* Cancel any pending reconnect timer */
    if (peer->reconnectTimer) {
        loopyTimerCancel(peer->reconnectTimer);
        peer->reconnectTimer = NULL;
    }

    /* Close connection if open */
    if (peer->conn) {
        loopyConnClose(peer->conn);
        peer->conn = NULL;
        if (transport->outboundCount > 0) {
            transport->outboundCount--;
        }
    }

    removePeerEntry(transport, peer);
}

loopyConn *loopyTransportGetPeerConn(loopyTransport *transport,
                                     loopyPeerId peerId) {
    if (!transport) {
        return NULL;
    }

    loopyPeerEntry *peer = findPeer(transport, peerId);
    return peer ? peer->conn : NULL;
}

bool loopyTransportIsPeerConnected(loopyTransport *transport,
                                   loopyPeerId peerId) {
    loopyConn *conn = loopyTransportGetPeerConn(transport, peerId);
    return conn && loopyConnGetState(conn) == LOOPY_CONN_CONNECTED;
}

/* ============================================================================
 * Connection Management
 * ============================================================================
 */

size_t loopyTransportGetConnectionCount(const loopyTransport *transport) {
    if (!transport) {
        return 0;
    }
    return transport->inboundCount + transport->outboundCount;
}

size_t loopyTransportGetInboundCount(const loopyTransport *transport) {
    if (!transport) {
        return 0;
    }
    return transport->inboundCount;
}

size_t loopyTransportGetOutboundCount(const loopyTransport *transport) {
    if (!transport) {
        return 0;
    }
    return transport->outboundCount;
}

void loopyTransportCloseInbound(loopyTransport *transport) {
    if (!transport) {
        return;
    }

    while (transport->inbound) {
        loopyInboundEntry *entry = transport->inbound;
        transport->inbound = entry->next;
        loopyConnClose(entry->conn);
        transport->inboundCount--;
        free(entry);
    }
}

void loopyTransportCloseOutbound(loopyTransport *transport) {
    if (!transport) {
        return;
    }

    for (loopyPeerEntry *p = transport->peers; p; p = p->next) {
        p->shouldReconnect = false;
        if (p->reconnectTimer) {
            loopyTimerCancel(p->reconnectTimer);
            p->reconnectTimer = NULL;
        }
        if (p->conn) {
            loopyConnClose(p->conn);
            p->conn = NULL;
        }
    }
    transport->outboundCount = 0;
}

/* ============================================================================
 * Properties
 * ============================================================================
 */

loopyLoop *loopyTransportGetLoop(const loopyTransport *transport) {
    if (!transport) {
        return NULL;
    }
    return transport->loop;
}

int loopyTransportGetPort(const loopyTransport *transport) {
    if (!transport) {
        return 0;
    }
    return transport->config.port;
}

bool loopyTransportIsRunning(const loopyTransport *transport) {
    if (!transport) {
        return false;
    }
    return transport->running;
}

void *loopyTransportGetUserData(const loopyTransport *transport) {
    if (!transport) {
        return NULL;
    }
    return transport->userData;
}

void loopyTransportSetUserData(loopyTransport *transport, void *userData) {
    if (!transport) {
        return;
    }
    transport->userData = userData;
}

void loopyTransportGetStats(const loopyTransport *transport,
                            loopyTransportStats *stats) {
    if (!stats) {
        return;
    }

    if (!transport) {
        memset(stats, 0, sizeof(*stats));
        return;
    }

    *stats = transport->stats;
}

/* ============================================================================
 * Iteration
 * ============================================================================
 */

void loopyTransportForEach(loopyTransport *transport,
                           loopyTransportIterCallback *cb, void *userData) {
    if (!transport || !cb) {
        return;
    }

    /* Iterate inbound */
    for (loopyInboundEntry *e = transport->inbound; e; e = e->next) {
        if (!cb(transport, e->conn, LOOPY_CONN_INBOUND, userData)) {
            return;
        }
    }

    /* Iterate outbound (peers) */
    for (loopyPeerEntry *p = transport->peers; p; p = p->next) {
        if (p->conn) {
            if (!cb(transport, p->conn, LOOPY_CONN_OUTBOUND, userData)) {
                return;
            }
        }
    }
}
