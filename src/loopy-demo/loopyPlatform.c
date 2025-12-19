/* loopyPlatform - Server lifecycle and orchestration
 *
 * Implementation of central orchestrator for loopy-demo.
 */

#include "loopyPlatform.h"
#include "loopyCluster.h"

/* loopy headers */
#include "../../deps/loopy/src/loopy.h"
#include "../../deps/loopy/src/loopyTimer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ============================================================================
 * Constants
 * ============================================================================
 */

/* Maximum peers we can track */
#define MAX_PEERS 128

/* Maximum sessions (inbound + outbound) */
#define MAX_SESSIONS 256

/* ============================================================================
 * Internal Structures
 * ============================================================================
 */

/**
 * Session entry in the session table.
 */
typedef struct SessionEntry {
    loopySession *session;
    uint64_t nodeId; /* Peer's node ID (0 if unknown/pending) */
    bool inbound;    /* Whether this was an inbound connection */
    bool verified;   /* Whether handshake completed */
    struct SessionEntry *next;
} SessionEntry;

struct loopyPlatform {
    /* State */
    loopyPlatformState state;

    /* Identity */
    loopyNodeId localId;

    /* Subsystems (owned by platform) */
    loopyLoop *loop;
    loopyTimerMgr *timerMgr;
    loopyTransport *transport;
    loopyCluster *cluster; /* Optional cluster manager */

    /* Session management */
    SessionEntry *sessions; /* Linked list of all sessions */
    size_t sessionCount;
    size_t verifiedCount;

    /* Peer configuration (for outbound connections) */
    loopyPeerConfig *peerConfigs;
    size_t peerConfigCount;

    /* Raft feature configuration (for kraft integration) */
    bool preVoteEnabled;
    bool readIndexEnabled;

    /* Callbacks */
    loopyPlatformCallback *callback;
    void *userData;

    /* Statistics */
    loopyPlatformStats stats;

    /* Run state */
    bool stopRequested;
};

/* ============================================================================
 * Forward Declarations
 * ============================================================================
 */

static void transportCallback(loopyTransport *transport,
                              loopyTransportEvent event, loopyConn *conn,
                              loopyTransportConnType connType, const void *data,
                              size_t len, void *userData);

static void timerCallback(loopyTimerMgr *mgr, loopyTimerType type,
                          void *userData);

static void sessionCallback(loopySession *session, loopySessionEvent event,
                            const void *data, size_t len, void *userData);

static bool duplicateCheck(const loopyNodeId *peerId, void *userData);

static void clusterCallback(loopyCluster *cluster, loopyClusterEvent event,
                            const loopyMember *member, void *userData);

static size_t clusterSendHandler(loopyCluster *cluster, const void *data,
                                 size_t len, void *userData);

static void handleClusterMessage(loopyPlatform *platform, loopySession *session,
                                 const void *data, size_t len);

static SessionEntry *addSession(loopyPlatform *platform, loopySession *session,
                                bool inbound, uint64_t knownNodeId);
static void removeSession(loopyPlatform *platform, loopySession *session);
static SessionEntry *findSessionByNodeId(loopyPlatform *platform,
                                         uint64_t nodeId);
static SessionEntry *findSessionBySession(loopyPlatform *platform,
                                          loopySession *session);

static uint64_t generateRunId(void);

/* ============================================================================
 * Configuration
 * ============================================================================
 */

void loopyPlatformConfigInit(loopyPlatformConfig *cfg) {
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->clusterId = 1; /* Default cluster ID */
    cfg->listenAddr = "0.0.0.0";
    loopyTimerConfigInit(&cfg->timerConfig);
    loopyTransportConfigInit(&cfg->transportConfig);
    loopyClusterConfigInit(&cfg->clusterConfig);
    cfg->enableCluster = false; /* Disabled by default */
    cfg->preVoteEnabled = true; /* Enable PreVote by default (Raft paper 9.6) */
    cfg->readIndexEnabled = true; /* Enable ReadIndex by default */
}

bool loopyPlatformConfigAddPeer(loopyPlatformConfig *cfg, uint64_t nodeId,
                                const char *addr, int port) {
    if (!cfg || !addr || port <= 0) {
        return false;
    }

    /* Grow array */
    size_t newCount = cfg->peerCount + 1;
    loopyPeerConfig *newPeers =
        realloc(cfg->peers, newCount * sizeof(loopyPeerConfig));
    if (!newPeers) {
        return false;
    }

    cfg->peers = newPeers;
    loopyPeerConfig *peer = &cfg->peers[cfg->peerCount];
    peer->nodeId = nodeId;
    peer->port = port;
    strncpy(peer->addr, addr, sizeof(peer->addr) - 1);
    peer->addr[sizeof(peer->addr) - 1] = '\0';
    cfg->peerCount = newCount;

    return true;
}

void loopyPlatformConfigCleanup(loopyPlatformConfig *cfg) {
    if (!cfg) {
        return;
    }
    free(cfg->peers);
    cfg->peers = NULL;
    cfg->peerCount = 0;
}

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

loopyPlatform *loopyPlatformCreate(const loopyPlatformConfig *cfg) {
    if (!cfg || cfg->nodeId == 0 || cfg->listenPort <= 0) {
        return NULL;
    }

    loopyPlatform *platform = calloc(1, sizeof(*platform));
    if (!platform) {
        return NULL;
    }

    platform->state = LOOPY_PLATFORM_CREATED;

    /* Set up local identity */
    platform->localId.nodeId = cfg->nodeId;
    platform->localId.clusterId = cfg->clusterId;
    platform->localId.runId = generateRunId();
    platform->localId.port = cfg->listenPort;
    if (cfg->listenAddr) {
        strncpy(platform->localId.addr, cfg->listenAddr,
                sizeof(platform->localId.addr) - 1);
    }

    /* Store callbacks */
    platform->callback = cfg->callback;
    platform->userData = cfg->userData;

    /* Store feature flags for kraft integration */
    platform->preVoteEnabled = cfg->preVoteEnabled;
    platform->readIndexEnabled = cfg->readIndexEnabled;

    /* Copy peer configuration */
    if (cfg->peerCount > 0 && cfg->peers) {
        platform->peerConfigs =
            malloc(cfg->peerCount * sizeof(loopyPeerConfig));
        if (!platform->peerConfigs) {
            free(platform);
            return NULL;
        }
        memcpy(platform->peerConfigs, cfg->peers,
               cfg->peerCount * sizeof(loopyPeerConfig));
        platform->peerConfigCount = cfg->peerCount;
    }

    /* Create event loop (1024 max file descriptors) */
    fprintf(stderr, "[PLATFORM] Creating event loop...\n");
    platform->loop = loopyNew(1024);
    if (!platform->loop) {
        fprintf(stderr, "[PLATFORM] ERROR: loopyNew failed\n");
        free(platform->peerConfigs);
        free(platform);
        return NULL;
    }
    fprintf(stderr, "[PLATFORM] Event loop created\n");

    /* Create timer manager */
    platform->timerMgr = loopyTimerMgrCreate(platform->loop, timerCallback,
                                             platform, &cfg->timerConfig);
    if (!platform->timerMgr) {
        loopyDelete(platform->loop);
        free(platform->peerConfigs);
        free(platform);
        return NULL;
    }

    /* Create transport */
    loopyTransportConfig transportCfg = cfg->transportConfig;
    transportCfg.port = cfg->listenPort;
    if (cfg->listenAddr) {
        transportCfg.bindAddr = cfg->listenAddr;
    }

    fprintf(stderr, "[PLATFORM] Creating transport on port %d...\n",
            transportCfg.port);
    platform->transport = loopyTransportCreate(
        platform->loop, transportCallback, platform, &transportCfg);
    if (!platform->transport) {
        fprintf(
            stderr,
            "[PLATFORM] ERROR: loopyTransportCreate failed (port %d in use?)\n",
            transportCfg.port);
        loopyTimerMgrDestroy(platform->timerMgr);
        loopyDelete(platform->loop);
        free(platform->peerConfigs);
        free(platform);
        return NULL;
    }
    fprintf(stderr, "[PLATFORM] Transport created\n");

    /* Create cluster manager if enabled */
    if (cfg->enableCluster) {
        loopyClusterConfig clusterCfg = cfg->clusterConfig;
        clusterCfg.localNodeId = cfg->nodeId;
        clusterCfg.clusterId = cfg->clusterId;
        clusterCfg.localPort = cfg->listenPort;
        if (cfg->listenAddr) {
            strncpy(clusterCfg.localAddr, cfg->listenAddr,
                    sizeof(clusterCfg.localAddr) - 1);
        }
        clusterCfg.callback = clusterCallback;
        clusterCfg.sendHandler = clusterSendHandler;
        clusterCfg.userData = platform;

        platform->cluster = loopyClusterCreate(&clusterCfg);
        if (!platform->cluster) {
            loopyTransportDestroy(platform->transport);
            loopyTimerMgrDestroy(platform->timerMgr);
            loopyDelete(platform->loop);
            free(platform->peerConfigs);
            free(platform);
            return NULL;
        }
    }

    return platform;
}

int loopyPlatformRun(loopyPlatform *platform) {
    if (!platform) {
        return -1;
    }

    if (platform->state != LOOPY_PLATFORM_CREATED) {
        return -1; /* Can only run from CREATED state */
    }

    platform->state = LOOPY_PLATFORM_STARTING;

    /* Start transport (begin accepting connections) */
    if (!loopyTransportStart(platform->transport)) {
        platform->state = LOOPY_PLATFORM_STOPPED;
        return -1;
    }

    /* Start cluster manager if enabled */
    if (platform->cluster) {
        loopyClusterStart(platform->cluster, platform->loop);
    }

    /* Connect to configured peers */
    for (size_t i = 0; i < platform->peerConfigCount; i++) {
        loopyPeerConfig *peer = &platform->peerConfigs[i];
        /* Don't connect to ourselves */
        if (peer->nodeId != platform->localId.nodeId) {
            loopyPlatformConnectPeer(platform, peer->nodeId, peer->addr,
                                     peer->port);
        }
    }

    platform->state = LOOPY_PLATFORM_RUNNING;
    platform->stopRequested = false;

    /* Notify startup */
    if (platform->callback) {
        platform->callback(platform, LOOPY_PLATFORM_EVENT_STARTED, NULL, NULL,
                           0, platform->userData);
    }

    /* Run event loop (blocks until loopyStop is called) */
    loopyMain(platform->loop);

    /* Shutdown sequence */
    platform->state = LOOPY_PLATFORM_STOPPING;

    /* Stop cluster manager */
    if (platform->cluster) {
        loopyClusterStop(platform->cluster);
    }

    /* Stop accepting new connections */
    loopyTransportStop(platform->transport);

    /* Close all sessions */
    SessionEntry *entry = platform->sessions;
    while (entry) {
        SessionEntry *next = entry->next;
        if (entry->session) {
            loopySessionClose(entry->session);
        }
        free(entry);
        entry = next;
    }
    platform->sessions = NULL;
    platform->sessionCount = 0;
    platform->verifiedCount = 0;

    platform->state = LOOPY_PLATFORM_STOPPED;

    /* Notify shutdown */
    if (platform->callback) {
        platform->callback(platform, LOOPY_PLATFORM_EVENT_STOPPED, NULL, NULL,
                           0, platform->userData);
    }

    return 0;
}

void loopyPlatformStop(loopyPlatform *platform) {
    if (!platform) {
        return;
    }
    platform->stopRequested = true;
    loopyStop(platform->loop);
}

void loopyPlatformDestroy(loopyPlatform *platform) {
    if (!platform) {
        return;
    }

    /* Ensure stopped */
    if (platform->state == LOOPY_PLATFORM_RUNNING) {
        loopyPlatformStop(platform);
    }

    /* Destroy subsystems in reverse order */
    if (platform->cluster) {
        loopyClusterDestroy(platform->cluster);
    }
    loopyTransportDestroy(platform->transport);
    loopyTimerMgrDestroy(platform->timerMgr);
    loopyDelete(platform->loop);

    /* Free peer configs */
    free(platform->peerConfigs);

    /* Free remaining sessions */
    SessionEntry *entry = platform->sessions;
    while (entry) {
        SessionEntry *next = entry->next;
        free(entry);
        entry = next;
    }

    free(platform);
}

/* ============================================================================
 * Connection Management
 * ============================================================================
 */

bool loopyPlatformConnectPeer(loopyPlatform *platform, uint64_t nodeId,
                              const char *addr, int port) {
    if (!platform || !addr || port <= 0) {
        return false;
    }

    /* Use peer's nodeId as the loopyPeerId for transport */
    return loopyTransportConnect(platform->transport, nodeId, addr, port);
}

void loopyPlatformDisconnectPeer(loopyPlatform *platform, uint64_t nodeId) {
    if (!platform) {
        return;
    }

    SessionEntry *entry = findSessionByNodeId(platform, nodeId);
    if (entry && entry->session) {
        loopySessionClose(entry->session);
    }

    loopyTransportDisconnect(platform->transport, nodeId);
}

loopySession *loopyPlatformGetPeerSession(loopyPlatform *platform,
                                          uint64_t nodeId) {
    if (!platform) {
        return NULL;
    }

    SessionEntry *entry = findSessionByNodeId(platform, nodeId);
    if (entry && entry->verified) {
        return entry->session;
    }
    return NULL;
}

bool loopyPlatformSendToPeer(loopyPlatform *platform, uint64_t nodeId,
                             const void *data, size_t len) {
    if (!platform || !data || len == 0) {
        return false;
    }

    loopySession *session = loopyPlatformGetPeerSession(platform, nodeId);
    if (!session) {
        return false;
    }

    if (loopySessionSend(session, data, len)) {
        platform->stats.messagesSent++;
        return true;
    }
    return false;
}

size_t loopyPlatformBroadcast(loopyPlatform *platform, const void *data,
                              size_t len) {
    if (!platform || !data || len == 0) {
        return 0;
    }

    size_t sent = 0;
    SessionEntry *entry = platform->sessions;
    while (entry) {
        if (entry->verified && entry->session) {
            if (loopySessionSend(entry->session, data, len)) {
                sent++;
                platform->stats.messagesSent++;
            }
        }
        entry = entry->next;
    }
    return sent;
}

/* ============================================================================
 * Timer Control
 * ============================================================================
 */

void loopyPlatformStartElectionTimer(loopyPlatform *platform) {
    if (!platform || !platform->timerMgr) {
        return;
    }
    loopyTimerStartElection(platform->timerMgr);
}

void loopyPlatformStopElectionTimer(loopyPlatform *platform) {
    if (!platform || !platform->timerMgr) {
        return;
    }
    loopyTimerStopElection(platform->timerMgr);
}

void loopyPlatformResetElectionTimer(loopyPlatform *platform) {
    if (!platform || !platform->timerMgr) {
        return;
    }
    /* Stop and restart to get new random timeout */
    loopyTimerStopElection(platform->timerMgr);
    loopyTimerStartElection(platform->timerMgr);
}

void loopyPlatformStartHeartbeatTimer(loopyPlatform *platform) {
    if (!platform || !platform->timerMgr) {
        return;
    }
    loopyTimerStartHeartbeat(platform->timerMgr);
}

void loopyPlatformStopHeartbeatTimer(loopyPlatform *platform) {
    if (!platform || !platform->timerMgr) {
        return;
    }
    loopyTimerStopHeartbeat(platform->timerMgr);
}

/* ============================================================================
 * Properties
 * ============================================================================
 */

loopyPlatformState loopyPlatformGetState(const loopyPlatform *platform) {
    if (!platform) {
        return LOOPY_PLATFORM_STOPPED;
    }
    return platform->state;
}

const char *loopyPlatformStateName(loopyPlatformState state) {
    switch (state) {
    case LOOPY_PLATFORM_CREATED:
        return "CREATED";
    case LOOPY_PLATFORM_STARTING:
        return "STARTING";
    case LOOPY_PLATFORM_RUNNING:
        return "RUNNING";
    case LOOPY_PLATFORM_STOPPING:
        return "STOPPING";
    case LOOPY_PLATFORM_STOPPED:
        return "STOPPED";
    default:
        return "UNKNOWN";
    }
}

loopyLoop *loopyPlatformGetLoop(const loopyPlatform *platform) {
    if (!platform) {
        return NULL;
    }
    return platform->loop;
}

loopyTimerMgr *loopyPlatformGetTimerMgr(const loopyPlatform *platform) {
    if (!platform) {
        return NULL;
    }
    return platform->timerMgr;
}

loopyTransport *loopyPlatformGetTransport(const loopyPlatform *platform) {
    if (!platform) {
        return NULL;
    }
    return platform->transport;
}

loopyCluster *loopyPlatformGetCluster(const loopyPlatform *platform) {
    if (!platform) {
        return NULL;
    }
    return platform->cluster;
}

const loopyNodeId *loopyPlatformGetLocalId(const loopyPlatform *platform) {
    if (!platform) {
        return NULL;
    }
    return &platform->localId;
}

void *loopyPlatformGetUserData(const loopyPlatform *platform) {
    if (!platform) {
        return NULL;
    }
    return platform->userData;
}

void loopyPlatformSetUserData(loopyPlatform *platform, void *userData) {
    if (!platform) {
        return;
    }
    platform->userData = userData;
}

size_t loopyPlatformGetPeerCount(const loopyPlatform *platform) {
    if (!platform) {
        return 0;
    }
    return platform->verifiedCount;
}

bool loopyPlatformIsPreVoteEnabled(const loopyPlatform *platform) {
    if (!platform) {
        return false;
    }
    return platform->preVoteEnabled;
}

bool loopyPlatformIsReadIndexEnabled(const loopyPlatform *platform) {
    if (!platform) {
        return false;
    }
    return platform->readIndexEnabled;
}

void loopyPlatformGetStats(const loopyPlatform *platform,
                           loopyPlatformStats *stats) {
    if (!stats) {
        return;
    }

    if (!platform) {
        memset(stats, 0, sizeof(*stats));
        return;
    }

    *stats = platform->stats;
}

/* ============================================================================
 * Session Iteration
 * ============================================================================
 */

void loopyPlatformForEachSession(loopyPlatform *platform,
                                 loopyPlatformSessionIter *iter,
                                 void *userData) {
    if (!platform || !iter) {
        return;
    }

    SessionEntry *entry = platform->sessions;
    while (entry) {
        if (entry->verified && entry->session) {
            if (!iter(platform, entry->session, userData)) {
                break;
            }
        }
        entry = entry->next;
    }
}

/* ============================================================================
 * Internal: Transport Callback
 * ============================================================================
 */

static void transportCallback(loopyTransport *transport,
                              loopyTransportEvent event, loopyConn *conn,
                              loopyTransportConnType connType, const void *data,
                              size_t len, void *userData) {
    loopyPlatform *platform = (loopyPlatform *)userData;
    (void)transport;
    (void)data;
    (void)len;

    switch (event) {
    case LOOPY_TRANSPORT_NEW_CONNECTION:
    case LOOPY_TRANSPORT_CONNECTED: {
        /* New connection - create session */
        bool inbound = (connType == LOOPY_CONN_INBOUND);

        loopySessionConfig sessionCfg;
        loopySessionConfigInit(&sessionCfg);
        sessionCfg.localId = platform->localId;

        loopySession *session = loopySessionCreate(
            conn, inbound, sessionCallback, platform, &sessionCfg);
        if (!session) {
            loopyConnClose(conn);
            break;
        }

        /* Set duplicate check */
        loopySessionSetDuplicateCheck(session, duplicateCheck, platform);

        /* Track session */
        /* TODO: For outbound connections, set nodeId early from peer config.
         * For now, nodeId will be set from handshake for both inbound/outbound.
         */
        uint64_t knownNodeId = 0;

        SessionEntry *entry =
            addSession(platform, session, inbound, knownNodeId);
        if (!entry) {
            loopySessionClose(session);
            break;
        }

        /* Start handshake */
        loopySessionStartHandshake(session);

        if (inbound) {
            platform->stats.peersConnected++;
        }
        break;
    }

    case LOOPY_TRANSPORT_DATA:
        /* This shouldn't happen - session handles data */
        break;

    case LOOPY_TRANSPORT_DISCONNECTED:
    case LOOPY_TRANSPORT_RECONNECTING:
        /* Connection lost - session callback will handle */
        break;
    }
}

/* ============================================================================
 * Internal: Timer Callback
 * ============================================================================
 */

static void timerCallback(loopyTimerMgr *mgr, loopyTimerType type,
                          void *userData) {
    loopyPlatform *platform = (loopyPlatform *)userData;
    (void)mgr;

    switch (type) {
    case LOOPY_TIMER_ELECTION:
        platform->stats.electionTimeouts++;
        break;
    case LOOPY_TIMER_HEARTBEAT:
        platform->stats.heartbeatsSent++;
        break;
    default:
        break;
    }

    if (platform->callback) {
        platform->callback(platform, LOOPY_PLATFORM_EVENT_TIMER, NULL, &type,
                           sizeof(type), platform->userData);
    }
}

/* ============================================================================
 * Internal: Session Callback
 * ============================================================================
 */

static void sessionCallback(loopySession *session, loopySessionEvent event,
                            const void *data, size_t len, void *userData) {
    loopyPlatform *platform = (loopyPlatform *)userData;

    SessionEntry *entry = findSessionBySession(platform, session);

    switch (event) {
    case LOOPY_SESSION_EVENT_CONNECTED:
        /* TCP connected - handshake will start */
        break;

    case LOOPY_SESSION_EVENT_VERIFIED: {
        /* Handshake completed - peer verified */
        if (entry) {
            entry->verified = true;
            platform->verifiedCount++;

            /* Update nodeId from peer identity */
            const loopyNodeId *peerId = loopySessionGetPeerId(session);
            if (peerId) {
                entry->nodeId = peerId->nodeId;
            }
        }

        if (platform->callback) {
            platform->callback(platform, LOOPY_PLATFORM_EVENT_PEER_JOINED,
                               session, NULL, 0, platform->userData);
        }
        break;
    }

    case LOOPY_SESSION_EVENT_REJECTED:
        /* Handshake rejected - will be followed by DISCONNECTED */
        break;

    case LOOPY_SESSION_EVENT_MESSAGE:
        /* RPC message received */
        platform->stats.messagesReceived++;

        /* Check if this is a cluster protocol message */
        if (platform->cluster && loopyClusterIsClusterMessage(data, len)) {
            handleClusterMessage(platform, session, data, len);
        } else if (platform->callback) {
            platform->callback(platform, LOOPY_PLATFORM_EVENT_MESSAGE, session,
                               data, len, platform->userData);
        }
        break;

    case LOOPY_SESSION_EVENT_DISCONNECTED:
        /* Connection closed */
        if (entry && entry->verified) {
            platform->verifiedCount--;
            platform->stats.peersDisconnected++;

            if (platform->callback) {
                platform->callback(platform, LOOPY_PLATFORM_EVENT_PEER_LEFT,
                                   session, NULL, 0, platform->userData);
            }
        }

        removeSession(platform, session);
        break;

    case LOOPY_SESSION_EVENT_ERROR:
        /* Protocol error - treat as disconnect */
        removeSession(platform, session);
        break;
    }
}

/* ============================================================================
 * Internal: Duplicate Check
 * ============================================================================
 */

static bool duplicateCheck(const loopyNodeId *peerId, void *userData) {
    loopyPlatform *platform = (loopyPlatform *)userData;

    /* Check if we already have a verified session with this runId */
    SessionEntry *entry = platform->sessions;
    while (entry) {
        if (entry->verified && entry->session) {
            const loopyNodeId *existingPeerId =
                loopySessionGetPeerId(entry->session);
            if (existingPeerId && existingPeerId->runId == peerId->runId) {
                return true; /* Duplicate - reject */
            }
        }
        entry = entry->next;
    }

    return false; /* Not a duplicate */
}

/* ============================================================================
 * Internal: Session Management
 * ============================================================================
 */

static SessionEntry *addSession(loopyPlatform *platform, loopySession *session,
                                bool inbound, uint64_t knownNodeId) {
    SessionEntry *entry = calloc(1, sizeof(*entry));
    if (!entry) {
        return NULL;
    }

    entry->session = session;
    entry->inbound = inbound;
    entry->verified = false;

    /* For outbound connections, nodeId is known upfront from
     * loopyTransportConnect. For inbound, it will be set later from the HELLO
     * handshake. */
    entry->nodeId = knownNodeId;

    /* Add to front of list */
    entry->next = platform->sessions;
    platform->sessions = entry;
    platform->sessionCount++;

    return entry;
}

static void removeSession(loopyPlatform *platform, loopySession *session) {
    SessionEntry **ptr = &platform->sessions;
    while (*ptr) {
        SessionEntry *entry = *ptr;
        if (entry->session == session) {
            *ptr = entry->next;
            platform->sessionCount--;
            free(entry);
            return;
        }
        ptr = &entry->next;
    }
}

static SessionEntry *findSessionByNodeId(loopyPlatform *platform,
                                         uint64_t nodeId) {
    SessionEntry *entry = platform->sessions;
    while (entry) {
        if (entry->nodeId == nodeId) {
            return entry;
        }
        entry = entry->next;
    }
    return NULL;
}

static SessionEntry *findSessionBySession(loopyPlatform *platform,
                                          loopySession *session) {
    SessionEntry *entry = platform->sessions;
    while (entry) {
        if (entry->session == session) {
            return entry;
        }
        entry = entry->next;
    }
    return NULL;
}

/* ============================================================================
 * Internal: Helpers
 * ============================================================================
 */

static uint64_t generateRunId(void) {
    /* Generate a unique run ID based on time and randomness */
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);

    uint64_t runId = (uint64_t)ts.tv_sec << 32;
    runId |= (uint64_t)ts.tv_nsec & 0xFFFFFFFF;
    runId ^= (uint64_t)rand() << 16;
    runId ^= (uint64_t)rand();

    return runId;
}

/* ============================================================================
 * Internal: Cluster Message Handling
 * ============================================================================
 */

static void handleClusterMessage(loopyPlatform *platform, loopySession *session,
                                 const void *data, size_t len) {
    if (!platform->cluster) {
        return;
    }

    int msgType = loopyClusterGetMessageType(data, len);

    switch (msgType) {
    case LOOPY_CLUSTER_MSG_GOSSIP:
        loopyClusterProcessGossip(platform->cluster, data, len);
        break;

    case LOOPY_CLUSTER_MSG_JOIN_REQUEST: {
        /* Process join request and send response */
        void *response = NULL;
        size_t responseLen = 0;

        if (loopyClusterProcessJoinRequest(platform->cluster, data, len,
                                           &response, &responseLen)) {
            /* Send response back through the session */
            if (session && response && responseLen > 0) {
                loopySessionSend(session, response, responseLen);
            }
            free(response);
        }
        break;
    }

    case LOOPY_CLUSTER_MSG_JOIN_RESPONSE:
        loopyClusterProcessJoinResponse(platform->cluster, data, len);
        break;

    case LOOPY_CLUSTER_MSG_CONFIG_CHANGE:
        /* TODO: Handle config change notification */
        break;

    default:
        /* Unknown cluster message type */
        break;
    }
}

/* ============================================================================
 * Internal: Cluster Send Handler
 * ============================================================================
 */

static size_t clusterSendHandler(loopyCluster *cluster, const void *data,
                                 size_t len, void *userData) {
    loopyPlatform *platform = (loopyPlatform *)userData;
    (void)cluster;

    /* Broadcast to all verified peers */
    return loopyPlatformBroadcast(platform, data, len);
}

/* ============================================================================
 * Internal: Cluster Callback
 * ============================================================================
 */

static void clusterCallback(loopyCluster *cluster, loopyClusterEvent event,
                            const loopyMember *member, void *userData) {
    loopyPlatform *platform = (loopyPlatform *)userData;
    (void)cluster;

    switch (event) {
    case LOOPY_CLUSTER_EVENT_JOINED:
        /* Successfully joined cluster.
         * Members will be connected via MEMBER_ADDED events or gossip. */
        break;

    case LOOPY_CLUSTER_EVENT_BOOTSTRAPPED:
        /* Successfully bootstrapped - we're the first node */
        break;

    case LOOPY_CLUSTER_EVENT_MEMBER_ADDED:
        /* New member added - connect to them */
        if (member && member->nodeId != platform->localId.nodeId) {
            /* Connect to the new member */
            loopyPlatformConnectPeer(platform, member->nodeId, member->addr,
                                     member->port);
        }
        break;

    case LOOPY_CLUSTER_EVENT_MEMBER_REMOVED:
        /* Member removed - disconnect */
        if (member && member->nodeId != platform->localId.nodeId) {
            loopyPlatformDisconnectPeer(platform, member->nodeId);
        }
        break;

    case LOOPY_CLUSTER_EVENT_MEMBER_UPDATED:
        /* Member info changed */
        break;

    case LOOPY_CLUSTER_EVENT_CONFIG_STABLE:
        /* Configuration change completed */
        break;

    case LOOPY_CLUSTER_EVENT_JOIN_FAILED:
        /* Join request was rejected */
        /* TODO: Could retry with different seed or give up */
        break;

    case LOOPY_CLUSTER_EVENT_GOSSIP_RECEIVED:
        /* Processed gossip - may have learned about new members */
        break;
    }
}

/* ============================================================================
 * Cluster Operations
 * ============================================================================
 */

bool loopyPlatformBootstrap(loopyPlatform *platform) {
    if (!platform || !platform->cluster) {
        return false;
    }
    return loopyClusterBootstrap(platform->cluster);
}

bool loopyPlatformJoinCluster(loopyPlatform *platform, const char *seedAddr,
                              int seedPort) {
    if (!platform || !platform->cluster || !seedAddr || seedPort <= 0) {
        return false;
    }

    /* Initiate join in cluster manager */
    if (!loopyClusterJoin(platform->cluster, seedAddr, seedPort)) {
        return false;
    }

    /* Connect to seed node */
    /* The seed node ID is unknown until we connect, use 0 for now */
    return loopyPlatformConnectPeer(platform, 0, seedAddr, seedPort);
}
