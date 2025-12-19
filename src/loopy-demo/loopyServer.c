/* loopyServer - High-level Raft server API
 *
 * Implementation of high-level server API wrapping loopyPlatform and kraft.
 */

#include "loopyServer.h"
#include "loopyPlatform.h"

/* kraft headers */
#include "../../deps/datakit/src/flex.h"
#include "../../deps/kvidxkit/src/kvidxkit.h"
#include "../kraft.h"
#include "../protoKit.h"
#include "../rpc.h"
#include "../rpcLeader.h"
#include "../vec.h"

#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Internal Structures
 * ============================================================================
 */

/* Forward declaration */
struct loopyServer;

/* Global server instance for kraft callbacks that don't pass context */
static struct loopyServer *g_kraftServer = NULL;

/* Pending command - tracks callback for submitted commands awaiting commit */
typedef struct PendingCommand {
    uint64_t index;           /* Log index for this command */
    loopyCommandCallback *cb; /* Callback to invoke when committed */
    void *userData;           /* User data for callback */
    struct PendingCommand *next;
} PendingCommand;

struct loopyServer {
    /* Platform (owned) */
    loopyPlatform *platform;

    /* Kraft Raft state (THE ACTUAL CONSENSUS ENGINE) */
    kraftState *raft;

    /* Configuration */
    uint64_t nodeId;
    uint64_t clusterId;

    /* State machine */
    loopyStateMachine *stateMachine;

    /* Pending commands awaiting commit (linked list for simplicity) */
    PendingCommand *pendingCommands;

    /* Callbacks */
    loopyServerCallback *callback;
    void *userData;

    /* Current state (cached from kraft) */
    loopyServerRole role;
    uint64_t leaderId;
    uint64_t term;
    uint64_t commitIndex;
    uint64_t lastApplied;

    /* Statistics */
    loopyServerStats stats;

    /* Run state */
    bool running;
};

/* ============================================================================
 * Forward Declarations
 * ============================================================================
 */

static void platformCallback(loopyPlatform *platform, loopyPlatformEvent event,
                             loopySession *session, const void *data,
                             size_t len, void *userData);

/* ============================================================================
 * Configuration
 * ============================================================================
 */

void loopyServerConfigInit(loopyServerConfig *cfg) {
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
    cfg->clusterId = 1;
    cfg->listenAddr = "0.0.0.0";

    /* Raft paper timing recommendations */
    cfg->electionTimeoutMinMs = 150;
    cfg->electionTimeoutMaxMs = 300;
    cfg->heartbeatIntervalMs = 50;

    /* Features enabled by default */
    cfg->preVoteEnabled = true;
    cfg->readIndexEnabled = true;
}

bool loopyServerConfigAddPeer(loopyServerConfig *cfg, uint64_t nodeId,
                              const char *addr, int port) {
    if (!cfg || nodeId == 0 || !addr || port <= 0) {
        return false;
    }

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

void loopyServerConfigCleanup(loopyServerConfig *cfg) {
    if (!cfg) {
        return;
    }
    free(cfg->peers);
    cfg->peers = NULL;
    cfg->peerCount = 0;
}

/* ============================================================================
 * Kraft Integration - Timer Callbacks
 * ============================================================================
 */

/**
 * Kraft timer callback: Election timeout
 *
 * Called by kraft to control the election timeout timer (follower/candidate).
 * When kraft wants to start/stop/restart election timeout, this is called.
 */
static bool loopyServerKraftTimerElection(kraftState *state,
                                          kraftTimerAction action) {
    loopyServer *server = (loopyServer *)state->adapterState;
    if (!server || !server->platform) {
        return false;
    }

    switch (action) {
    case KRAFT_TIMER_START:
    case KRAFT_TIMER_AGAIN:
        loopyPlatformStartElectionTimer(server->platform);
        break;
    case KRAFT_TIMER_STOP:
        loopyPlatformStopElectionTimer(server->platform);
        break;
    }
    return true;
}

/**
 * Kraft timer callback: Heartbeat
 *
 * Called by kraft to control the heartbeat timer (leader only).
 * When kraft becomes leader, it starts heartbeat. When it loses leadership,
 * stops.
 */
static bool loopyServerKraftTimerHeartbeat(kraftState *state,
                                           kraftTimerAction action) {
    loopyServer *server = (loopyServer *)state->adapterState;
    if (!server || !server->platform) {
        return false;
    }

    switch (action) {
    case KRAFT_TIMER_START:
        loopyPlatformStartHeartbeatTimer(server->platform);
        break;
    case KRAFT_TIMER_STOP:
        loopyPlatformStopHeartbeatTimer(server->platform);
        break;
    case KRAFT_TIMER_AGAIN:
        // Heartbeat is periodic, no explicit restart needed
        break;
    }
    return true;
}

/**
 * Kraft timer callback: Election backoff
 *
 * Called by kraft when election fails (no majority) to implement
 * exponential backoff before retrying.
 */
static bool loopyServerKraftTimerBackoff(kraftState *state,
                                         kraftTimerAction action) {
    loopyServer *server = (loopyServer *)state->adapterState;
    if (!server || !server->platform) {
        return false;
    }

    loopyTimerMgr *timerMgr = loopyPlatformGetTimerMgr(server->platform);
    if (!timerMgr) {
        return false;
    }

    switch (action) {
    case KRAFT_TIMER_START:
        loopyTimerStartElectionBackoff(timerMgr);
        break;
    case KRAFT_TIMER_STOP:
        loopyTimerStopElectionBackoff(timerMgr);
        break;
    case KRAFT_TIMER_AGAIN:
        loopyTimerStopElectionBackoff(timerMgr);
        loopyTimerStartElectionBackoff(timerMgr);
        break;
    }
    return true;
}

/* ============================================================================
 * Kraft Integration - Helper Functions
 * ============================================================================
 */

/**
 * Find kraftNode by node ID
 */
static kraftNode *loopyServerFindKraftNode(kraftState *raft, uint64_t nodeId) {
    if (!raft) {
        return NULL;
    }

    // Iterate through old nodes (active cluster members)
    uint32_t nodeCount = vecPtrCount(NODES_OLD_NODES(raft));
    for (uint32_t i = 0; i < nodeCount; i++) {
        kraftNode *node = NULL;
        if (vecPtrGet(NODES_OLD_NODES(raft), i, (void **)&node) == VEC_OK &&
            node && node->nodeId == nodeId) {
            return node;
        }
    }
    return NULL;
}

/**
 * Sync loopyServer state from kraftState
 *
 * Updates cached state and fires callbacks on changes.
 * Call after any kraft operation (RPC, timer, etc.)
 */
static void loopyServerSyncStateFromKraft(loopyServer *server) {
    if (!server || !server->raft) {
        return;
    }

    kraftState *raft = server->raft;
    loopyServerRole prevRole = server->role;
    uint64_t prevLeader = server->leaderId;

    // Update role from kraft
    kraftRole kraftRole = raft->self->consensus.role;
    fprintf(stderr, "[SYNC] Node %lu kraft role=%u\n", server->nodeId,
            kraftRole);

    if (IS_LEADER(raft->self)) {
        fprintf(stderr, "[SYNC] Node %lu: LEADER detected, updating\n",
                server->nodeId);
        server->role = LOOPY_SERVER_ROLE_LEADER;
    } else if (IS_CANDIDATE(raft->self) || IS_PRE_CANDIDATE(raft->self)) {
        server->role = LOOPY_SERVER_ROLE_CANDIDATE;
    } else {
        server->role = LOOPY_SERVER_ROLE_FOLLOWER;
    }

    // Update leader, term, indices
    server->leaderId = raft->leader ? raft->leader->nodeId : 0;
    server->term = raft->self->consensus.term;
    server->commitIndex = raft->commitIndex;
    server->lastApplied = raft->lastApplied;

    // Fire callbacks on state changes
    if (server->callback) {
        if (server->role != prevRole) {
            server->callback(server, LOOPY_SERVER_EVENT_ROLE_CHANGED,
                             &server->role, server->userData);
        }
        if (server->leaderId != prevLeader) {
            server->callback(server, LOOPY_SERVER_EVENT_LEADER_CHANGED,
                             &server->leaderId, server->userData);
        }
    }
}

/* ============================================================================
 * Kraft Integration - Network & RPC Callbacks
 * ============================================================================
 */

/**
 * Kraft RPC send callback
 *
 * Called when kraft wants to send an RPC to a peer node.
 */
static void loopyServerKraftSendRpc(kraftState *state, kraftRpc *rpc,
                                    uint8_t *buf, uint64_t len,
                                    kraftNode *node) {
    loopyServer *server = (loopyServer *)state->adapterState;
    if (!server || !buf || !node) {
        fprintf(
            stderr,
            "[RPC-SEND] ERROR: NULL parameter (server=%p, buf=%p, node=%p)\n",
            (void *)server, (void *)buf, (void *)node);
        return;
    }

    fprintf(stderr, "[RPC-SEND] Sending %lu bytes to node %lu\n", len,
            node->nodeId);

    // Find session for this peer node
    loopySession *session =
        loopyPlatformGetPeerSession(server->platform, node->nodeId);
    if (!session) {
        // Not connected yet - kraft will retry
        fprintf(stderr, "[RPC-SEND] WARNING: No session for node %lu\n",
                node->nodeId);
        return;
    }

    // Send RPC bytes over network
    fprintf(stderr, "[RPC-SEND] Session found, sending...\n");
    loopySessionSend(session, buf, (size_t)len);
    fprintf(stderr, "[RPC-SEND] Sent successfully\n");
}

/* Helper: Find and remove pending command by index */
static PendingCommand *popPendingCommand(loopyServer *server, uint64_t index) {
    PendingCommand **pp = &server->pendingCommands;
    while (*pp) {
        if ((*pp)->index == index) {
            PendingCommand *cmd = *pp;
            *pp = cmd->next;
            return cmd;
        }
        pp = &(*pp)->next;
    }
    return NULL;
}

/* Helper: Add pending command */
static void addPendingCommand(loopyServer *server, uint64_t index,
                              loopyCommandCallback *cb, void *userData) {
    PendingCommand *cmd = malloc(sizeof(*cmd));
    if (!cmd) {
        return;
    }
    cmd->index = index;
    cmd->cb = cb;
    cmd->userData = userData;
    cmd->next = server->pendingCommands;
    server->pendingCommands = cmd;
}

/**
 * Kraft state machine apply callback
 *
 * Called when a log entry is committed and ready to apply.
 */
static void loopyServerKraftApply(const void *data, uint64_t len) {
    /* Use global server instance (kraft callback doesn't pass context) */
    if (!g_kraftServer || !g_kraftServer->stateMachine ||
        !g_kraftServer->raft) {
        fprintf(stderr,
                "[APPLY] WARNING: No state machine or raft! data=%p len=%lu\n",
                data, len);
        return;
    }

    /* kraft has already incremented lastApplied to the current index being
     * applied */
    uint64_t index = g_kraftServer->raft->lastApplied;

    fprintf(stderr, "[APPLY] Applying index=%lu len=%lu to state machine\n",
            index, len);

    loopyStateMachine *sm = g_kraftServer->stateMachine;
    void *result = NULL;
    size_t resultLen = 0;
    bool success = false;

    if (sm->apply) {
        success = sm->apply(sm, index, data, len, &result, &resultLen);
        fprintf(stderr, "[APPLY] Applied index=%lu success=%d resultLen=%zu\n",
                index, success, resultLen);
    } else {
        fprintf(stderr,
                "[APPLY] WARNING: State machine has no apply function!\n");
    }

    /* Find and invoke pending command callback */
    PendingCommand *pending = popPendingCommand(g_kraftServer, index);
    if (pending) {
        if (pending->cb) {
            pending->cb(g_kraftServer, index, success, result, resultLen,
                        pending->userData);
        }
        free(pending);
    }

    /* Free result after callback */
    if (result) {
        free(result);
    }
}

/* ============================================================================
 * Kraft Integration - Protocol Encoding/Decoding
 *
 * NEW clean implementation for loopy-demo (self-contained, well-encapsulated)
 * ============================================================================
 */

/**
 * Check if buffer has a complete valid RPC PDU
 */
static bool bufferHasValidRPC(const protoKit *pk, const protoKitBuffer *buf) {
    (void)pk;
    if (!buf || buf->used < 4) {
        return false; // Need at least 4 bytes for PDU size
    }

    // PDU format: [4-byte length][data]
    uint32_t pduSize;
    memcpy(&pduSize, buf->buf, sizeof(pduSize));

    return (buf->used >= pduSize + 4);
}

/**
 * Encode kraft RPC to binary format for transmission
 *
 * Simple binary format: [4-byte length][serialized RPC data]
 */
static bool loopyServerKraftEncodeRpc(const kraftState *state,
                                      const kraftRpc *rpc, uint8_t **buf,
                                      uint64_t *len) {
    (void)state;
    if (!rpc || !buf || !len) {
        return false;
    }

    // For now, use simple serialization (can enhance later)
    // Just copy the RPC struct as-is (works for in-memory, not for network)
    size_t rpcSize = sizeof(kraftRpc);
    uint8_t *buffer = malloc(4 + rpcSize);
    if (!buffer) {
        return false;
    }

    // Write PDU size
    uint32_t pduSize = (uint32_t)rpcSize;
    memcpy(buffer, &pduSize, 4);

    // Write RPC data
    memcpy(buffer + 4, rpc, rpcSize);

    *buf = buffer;
    *len = 4 + rpcSize;
    return true;
}

/**
 * Decode/populate kraft RPC from received network data
 */
static kraftProtocolStatus
loopyServerKraftPopulateRpc(kraftState *state, kraftNode *node, kraftRpc *rpc) {
    (void)state;
    if (!node || !rpc || !node->client || !node->client->buf.buf) {
        return KRAFT_PROTOCOL_STATUS_PARSE_ERROR;
    }

    protoKitBuffer *buf = &node->client->buf;
    if (buf->used < 4) {
        return KRAFT_PROTOCOL_STATUS_NO_RPC;
    }

    // Read PDU size
    uint32_t pduSize;
    memcpy(&pduSize, buf->buf, 4);

    if (buf->used < pduSize + 4) {
        return KRAFT_PROTOCOL_STATUS_NO_RPC; // Incomplete
    }

    // Read RPC data
    memcpy(rpc, buf->buf + 4, sizeof(kraftRpc));

    // Consume PDU from buffer
    buf->used -= (pduSize + 4);
    if (buf->used > 0) {
        memmove(buf->buf, buf->buf + pduSize + 4, buf->used);
    }

    return KRAFT_PROTOCOL_STATUS_RPC_POPULATED;
}

/**
 * Free RPC buffer allocated by encode
 */
static void loopyServerKraftFreeRpc(void *buf) {
    free(buf);
}

/**
 * Register all kraft integration callbacks
 *
 * Wires kraft engine to loopy networking, timers, and state machine.
 */
/* ============================================================================
 * Entry Codec Functions (for AppendEntries RPCs)
 * ============================================================================
 */

/**
 * Encode a single log entry into RPC
 * Format: [idx:8][term:8][cmd:8][len:8][data:len]
 */
static bool loopyServerEntryEncode(kraftRpc *rpc, kraftRpcUniqueIndex idx,
                                   kraftTerm term, kraftStorageCmd cmd,
                                   const void *data, size_t len) {
    // Allocate flex if needed
    if (!rpc->entry) {
        rpc->entry = flexNew();
        rpc->resume = NULL;
        rpc->remaining = 0;
    }

    // Encode to tail: index, term, command, length, data
    flexPushUnsigned(&rpc->entry, idx, FLEX_ENDPOINT_TAIL);
    flexPushUnsigned(&rpc->entry, term, FLEX_ENDPOINT_TAIL);
    flexPushUnsigned(&rpc->entry, (uint64_t)cmd, FLEX_ENDPOINT_TAIL);
    flexPushUnsigned(&rpc->entry, len, FLEX_ENDPOINT_TAIL);

    if (len > 0 && data) {
        flexPushBytes(&rpc->entry, data, len, FLEX_ENDPOINT_TAIL);
    }

    rpc->len = flexBytes(rpc->entry);
    rpc->remaining++;
    if (!rpc->resume) {
        rpc->resume = flexHead(rpc->entry);
    }

    return true;
}

/**
 * Encode multiple entries in bulk
 */
static bool loopyServerEntryEncodeBulk(kraftRpc *rpc, kraftTerm term,
                                       kraftStorageCmd cmd, size_t count,
                                       const kraftRpcUniqueIndex idx[],
                                       const void *data[], const size_t len[]) {
    for (size_t i = 0; i < count; i++) {
        if (!loopyServerEntryEncode(rpc, idx[i], term, cmd, data[i], len[i])) {
            return false;
        }
    }
    return true;
}

/**
 * Decode one entry from RPC
 */
static bool loopyServerEntryDecode(kraftRpc *rpc, kraftRpcUniqueIndex *idx,
                                   kraftTerm *term, kraftStorageCmd *cmd,
                                   uint8_t **data, size_t *len) {
    if (!rpc->entry || !rpc->resume) {
        return false;
    }

    // Use temporary variables for validation
    uint64_t idxVal, termVal, cmdVal, lenVal;

    // Decode index
    if (!flexGetUnsigned(rpc->resume, &idxVal)) {
        return false;
    }
    rpc->resume = flexNext(rpc->entry, rpc->resume);

    // Decode term
    if (!flexGetUnsigned(rpc->resume, &termVal)) {
        return false;
    }
    rpc->resume = flexNext(rpc->entry, rpc->resume);

    // Decode command
    if (!flexGetUnsigned(rpc->resume, &cmdVal)) {
        return false;
    }
    rpc->resume = flexNext(rpc->entry, rpc->resume);

    // Decode length
    if (!flexGetUnsigned(rpc->resume, &lenVal)) {
        return false;
    }
    rpc->resume = flexNext(rpc->entry, rpc->resume);

    // Decode data if present
    uint8_t *dataPtr = NULL;
    if (lenVal > 0) {
        // Get pointer to bytes in flex
        databox box;
        flexGetByType(rpc->resume, &box);
        if (box.type != DATABOX_BYTES) {
            return false;
        }
        dataPtr = (uint8_t *)box.data.bytes.start;
        rpc->resume = flexNext(rpc->entry, rpc->resume);
    }

    // All validation passed - copy to output
    *idx = (kraftRpcUniqueIndex)idxVal;
    *term = (kraftTerm)termVal;
    *cmd = (kraftStorageCmd)cmdVal;
    *data = dataPtr;
    *len = (size_t)lenVal;

    rpc->remaining--;
    return true;
}

/**
 * Release entry resources
 */
static void loopyServerEntryRelease(kraftRpc *rpc) {
    if (rpc->entry) {
        flexFree(rpc->entry);
        rpc->entry = NULL;
        rpc->resume = NULL;
        rpc->remaining = 0;
    }
}

/* ============================================================================
 * Callback Registration
 * ============================================================================
 */

static void loopyServerRegisterKraftCallbacks(loopyServer *server) {
    if (!server || !server->raft) {
        return;
    }

    kraftState *raft = server->raft;

    // === PROTOCOL (RPC encoding/decoding) ===
    raft->internals.protocol.encodeRpc = loopyServerKraftEncodeRpc;
    raft->internals.protocol.freeRpc = loopyServerKraftFreeRpc;
    raft->internals.protocol.populateRpcFromClient =
        loopyServerKraftPopulateRpc;

    // === NETWORK (RPC transmission) ===
    raft->internals.rpc.sendRpc = loopyServerKraftSendRpc;

    // === TIMERS (kraft controls loopy timers) ===
    raft->internals.timer.leaderContactTimer = loopyServerKraftTimerElection;
    raft->internals.timer.heartbeatTimer = loopyServerKraftTimerHeartbeat;
    raft->internals.timer.electionBackoffTimer = loopyServerKraftTimerBackoff;

    // === ELECTIONS (kraft election functions) ===
    raft->internals.election.vote = kraftRpcRequestVote;
    raft->internals.election.sendHeartbeat = kraftRpcSendHeartbeat;

    // === STATE MACHINE (apply committed entries) ===
    raft->internals.data.apply = loopyServerKraftApply;

    // === ENTRY CODEC (for AppendEntries log replication) ===
    raft->internals.entry.encode = loopyServerEntryEncode;
    raft->internals.entry.encodeBulk = loopyServerEntryEncodeBulk;
    raft->internals.entry.decode = loopyServerEntryDecode;
    raft->internals.entry.release = loopyServerEntryRelease;

    // protoKit already initialized in loopyServerCreate
}

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

loopyServer *loopyServerCreate(const loopyServerConfig *cfg) {
    if (!cfg) {
        fprintf(stderr, "[CREATE] ERROR: cfg is NULL\n");
        return NULL;
    }
    if (cfg->nodeId == 0) {
        fprintf(stderr, "[CREATE] ERROR: nodeId is 0\n");
        return NULL;
    }
    if (cfg->listenPort <= 0) {
        fprintf(stderr, "[CREATE] ERROR: listenPort is %d\n", cfg->listenPort);
        return NULL;
    }

    fprintf(stderr, "[CREATE] Starting: nodeId=%lu, port=%d, peerCount=%zu\n",
            cfg->nodeId, cfg->listenPort, cfg->peerCount);

    loopyServer *server = calloc(1, sizeof(*server));
    if (!server) {
        fprintf(stderr, "[CREATE] ERROR: calloc failed\n");
        return NULL;
    }

    server->nodeId = cfg->nodeId;
    server->clusterId = cfg->clusterId;
    server->callback = cfg->callback;
    server->userData = cfg->userData;
    server->role = LOOPY_SERVER_ROLE_FOLLOWER;

    /* Set global for kraft callbacks that don't pass context */
    g_kraftServer = server;

    fprintf(stderr,
            "[CREATE] Set global server, creating platform config...\n");

    /* Create platform config */
    loopyPlatformConfig platCfg;
    loopyPlatformConfigInit(&platCfg);
    platCfg.nodeId = cfg->nodeId;
    platCfg.clusterId = cfg->clusterId;
    platCfg.listenAddr = cfg->listenAddr;
    platCfg.listenPort = cfg->listenPort;
    platCfg.callback = platformCallback;
    platCfg.userData = server;

    /* Copy timer configuration */
    platCfg.timerConfig.electionTimeoutMinMs = cfg->electionTimeoutMinMs;
    platCfg.timerConfig.electionTimeoutMaxMs = cfg->electionTimeoutMaxMs;
    platCfg.timerConfig.heartbeatIntervalMs = cfg->heartbeatIntervalMs;

    /* Copy feature flags */
    platCfg.preVoteEnabled = cfg->preVoteEnabled;
    platCfg.readIndexEnabled = cfg->readIndexEnabled;

    /* Add peers */
    fprintf(stderr, "[CREATE] Adding %zu peers...\n", cfg->peerCount);
    for (size_t i = 0; i < cfg->peerCount; i++) {
        fprintf(stderr, "[CREATE]   Peer %zu: nodeId=%lu %s:%d\n", i,
                cfg->peers[i].nodeId, cfg->peers[i].addr, cfg->peers[i].port);
        if (!loopyPlatformConfigAddPeer(&platCfg, cfg->peers[i].nodeId,
                                        cfg->peers[i].addr,
                                        cfg->peers[i].port)) {
            fprintf(stderr, "[CREATE] ERROR: Failed to add peer %zu\n", i);
            free(server);
            return NULL;
        }
    }
    fprintf(stderr, "[CREATE] Peers added, creating platform...\n");

    /* Create platform */
    server->platform = loopyPlatformCreate(&platCfg);
    fprintf(stderr, "[CREATE] Platform create returned %p\n",
            (void *)server->platform);
    loopyPlatformConfigCleanup(&platCfg);

    if (!server->platform) {
        fprintf(stderr, "[CREATE] ERROR: Platform creation failed!\n");
        free(server);
        return NULL;
    }
    fprintf(stderr, "[CREATE] Platform created successfully\n");

    /* === KRAFT RAFT INITIALIZATION === */

    // 1. Create kraftState
    server->raft = kraftStateNew();
    if (!server->raft) {
        fprintf(stderr, "ERROR: kraftStateNew failed\n");
        loopyPlatformDestroy(server->platform);
        free(server);
        return NULL;
    }

    // 2. Build peer IP array (self + peers)
    size_t peerCount = cfg->peerCount + 1; // +1 for self
    fprintf(stderr,
            "[KRAFT] Init: nodeId=%lu, peerCount=%zu (including self)\n",
            cfg->nodeId, peerCount);

    kraftIp **ips = calloc(peerCount, sizeof(kraftIp *));
    if (!ips) {
        fprintf(stderr, "[KRAFT] ERROR: Failed to allocate IP array\n");
        kraftStateFree(server->raft);
        loopyPlatformDestroy(server->platform);
        free(server);
        return NULL;
    }

    // Add self first
    ips[0] = calloc(1, sizeof(kraftIp));
    strncpy(ips[0]->ip, cfg->listenAddr, sizeof(ips[0]->ip) - 1);
    ips[0]->port = cfg->listenPort;
    ips[0]->family = KRAFT_IPv4;
    ips[0]->outbound = false;
    fprintf(stderr, "[KRAFT] Self: %s:%d (node %lu)\n", ips[0]->ip,
            ips[0]->port, cfg->nodeId);

    // Add peers
    for (size_t i = 0; i < cfg->peerCount; i++) {
        ips[i + 1] = calloc(1, sizeof(kraftIp));
        strncpy(ips[i + 1]->ip, cfg->peers[i].addr, sizeof(ips[i + 1]->ip) - 1);
        ips[i + 1]->port = cfg->peers[i].port;
        ips[i + 1]->family = KRAFT_IPv4;
        ips[i + 1]->outbound = true;
        fprintf(stderr, "[KRAFT] Peer %zu: %s:%d (node %lu)\n", i + 1,
                ips[i + 1]->ip, ips[i + 1]->port, cfg->peers[i].nodeId);
    }

    // 3. Initialize kraft Raft protocol
    fprintf(stderr,
            "[KRAFT] Calling kraftStateInitRaft(peerCount=%zu, clusterId=%lu, "
            "nodeId=%lu)...\n",
            peerCount, cfg->clusterId, cfg->nodeId);
    if (!kraftStateInitRaft(server->raft, peerCount, ips, cfg->clusterId,
                            cfg->nodeId)) {
        fprintf(stderr, "[KRAFT] ERROR: kraftStateInitRaft FAILED\n");
        for (size_t i = 0; i < peerCount; i++) {
            free(ips[i]);
        }
        free(ips);
        kraftStateFree(server->raft);
        loopyPlatformDestroy(server->platform);
        free(server);
        return NULL;
    }

    fprintf(stderr, "[KRAFT] kraftStateInitRaft succeeded\n");

    // 3b. Create kraftNode objects for each PEER (kraft already created self!)
    fprintf(stderr, "[KRAFT] Creating peer nodes...\n");
    uint64_t baseRunId = kraftustime();

    // Start at i=1 to skip self (index 0), kraft already created it
    for (size_t i = 1; i < peerCount; i++) {
        // Create kraftClient with initialized buffer
        kraftClient *client = calloc(1, sizeof(kraftClient));
        if (!client) {
            fprintf(stderr, "ERROR: Failed to create kraftClient\n");
            for (size_t j = 0; j < peerCount; j++) {
                free(ips[j]);
            }
            free(ips);
            kraftStateFree(server->raft);
            loopyPlatformDestroy(server->platform);
            free(server);
            return NULL;
        }

        // Initialize client buffer for RPC reception
        client->buf.allocated = 8192;
        client->buf.buf = calloc(1, client->buf.allocated);
        client->buf.used = 0;
        client->ip = *ips[i];

        // Create kraftNode with client
        kraftNode *node = kraftNodeNew(client);
        if (!node) {
            fprintf(stderr, "ERROR: Failed to create kraftNode\n");
            free(client->buf.buf);
            free(client);
            for (size_t j = 0; j < peerCount; j++) {
                free(ips[j]);
            }
            free(ips);
            kraftStateFree(server->raft);
            loopyPlatformDestroy(server->platform);
            free(server);
            return NULL;
        }

        node->clusterId = cfg->clusterId;
        node->nodeId =
            ips[i]->port; // Using port as node ID (matches parseClusterPeers)
        node->runId = baseRunId + i;
        node->isOurself = (node->nodeId == cfg->nodeId);

        // Add to kraft node list
        vecPtrPush(NODES_OLD_NODES(server->raft), node);

        fprintf(stderr, "[KRAFT]   Node %lu (runId=%lu, self=%d)\n",
                node->nodeId, node->runId, node->isOurself);
    }

    fprintf(stderr, "[KRAFT] Created %zu nodes, cluster size = %u\n", peerCount,
            vecPtrCount(NODES_OLD_NODES(server->raft)));

    // 4. Configure kraft features
    fprintf(stderr, "[KRAFT] Configuring features...\n");
    server->raft->config.enablePreVote = cfg->preVoteEnabled;
    server->raft->config.enableReadIndex = cfg->readIndexEnabled;

    // 5. Initialize protoKit (MUST be before kvidxOpen)
    fprintf(stderr, "[KRAFT] Initializing protoKit...\n");
    protoKit pk = {.bufferHasValidPDU = bufferHasValidRPC};
    server->raft->protoKit = pk;

    // 6. Set up log storage interface
    fprintf(stderr, "[KRAFT] Setting up log storage...\n");
    const kvidxAdapterInfo *adapter = kvidxGetAdapterByName("SQLite3");
    if (!adapter) {
        fprintf(stderr, "ERROR: SQLite3 adapter not found\n");
        for (size_t i = 0; i < peerCount; i++) {
            free(ips[i]);
        }
        free(ips);
        kraftStateFree(server->raft);
        loopyPlatformDestroy(server->platform);
        free(server);
        return NULL;
    }

    server->raft->log.kvidx.interface = *adapter->iface;

    // 7. Open log storage (in-memory for testing, can use file later)
    char logPath[256];
    snprintf(logPath, sizeof(logPath), ":memory:"); // In-memory SQLite
    fprintf(stderr, "[KRAFT] Opening log storage (in-memory)...\n");

    const char *errMsg = NULL;
    bool openSuccess = kvidxOpen(KRAFT_ENTRIES(server->raft), logPath, &errMsg);
    if (!openSuccess) {
        fprintf(stderr, "ERROR: kvidxOpen failed: %s\n",
                errMsg ? errMsg : "unknown error");
        for (size_t i = 0; i < peerCount; i++) {
            free(ips[i]);
        }
        free(ips);
        kraftStateFree(server->raft);
        loopyPlatformDestroy(server->platform);
        free(server);
        return NULL;
    }

    fprintf(stderr, "[KRAFT] Log storage opened successfully\n");

    // 8. Store back-pointer for callbacks
    server->raft->adapterState = server;

    // 9. Register kraft integration callbacks
    fprintf(stderr, "[KRAFT] Registering callbacks...\n");
    loopyServerRegisterKraftCallbacks(server);
    fprintf(stderr, "[KRAFT] Callbacks registered\n");

    // 10. Start election timer (kraft won't start automatically!)
    fprintf(stderr, "[KRAFT] Starting election timer...\n");
    if (server->raft->internals.timer.leaderContactTimer) {
        server->raft->internals.timer.leaderContactTimer(server->raft,
                                                         KRAFT_TIMER_START);
    }
    fprintf(stderr, "[KRAFT] Kraft initialization complete!\n");

    // 11. Free temporary IP array (kraft made copies)
    for (size_t i = 0; i < peerCount; i++) {
        free(ips[i]);
    }
    free(ips);

    return server;
}

loopyServer *loopyServerCreateSingle(uint64_t nodeId, const char *listenAddr,
                                     int listenPort) {
    if (nodeId == 0 || !listenAddr || listenPort <= 0) {
        return NULL;
    }

    loopyServerConfig cfg;
    loopyServerConfigInit(&cfg);
    cfg.nodeId = nodeId;
    cfg.listenAddr = listenAddr;
    cfg.listenPort = listenPort;

    /* Single node - no peers */

    loopyServer *server = loopyServerCreate(&cfg);
    if (server) {
        /* Single node immediately becomes leader */
        server->role = LOOPY_SERVER_ROLE_LEADER;
        server->leaderId = nodeId;
    }

    return server;
}

void loopyServerSetStateMachine(loopyServer *server, loopyStateMachine *sm) {
    if (!server) {
        return;
    }
    server->stateMachine = sm;
}

int loopyServerRun(loopyServer *server) {
    if (!server || !server->platform) {
        return -1;
    }

    server->running = true;

    /* Notify startup */
    if (server->callback) {
        server->callback(server, LOOPY_SERVER_EVENT_STARTED, NULL,
                         server->userData);
    }

    /* Run platform */
    int result = loopyPlatformRun(server->platform);

    server->running = false;

    /* Notify shutdown */
    if (server->callback) {
        server->callback(server, LOOPY_SERVER_EVENT_STOPPED, NULL,
                         server->userData);
    }

    return result;
}

void loopyServerStop(loopyServer *server) {
    if (!server || !server->platform) {
        return;
    }
    loopyPlatformStop(server->platform);
}

void loopyServerDestroy(loopyServer *server) {
    if (!server) {
        return;
    }

    // Clean up kraft Raft state
    if (server->raft) {
        // Close log storage
        kvidxClose(KRAFT_ENTRIES(server->raft));

        // Free kraft state
        kraftStateFree(server->raft);
        server->raft = NULL;
    }

    if (server->platform) {
        loopyPlatformDestroy(server->platform);
    }

    free(server);
}

/* ============================================================================
 * Command Submission
 * ============================================================================
 */

uint64_t loopyServerSubmit(loopyServer *server, const void *data, size_t len,
                           loopyCommandCallback *cb, void *userData) {
    if (!server || !server->raft || !data || len == 0) {
        return 0;
    }

    /* Only leader can accept commands */
    if (server->role != LOOPY_SERVER_ROLE_LEADER) {
        server->stats.commandsRejected++;
        if (cb) {
            cb(server, 0, false, NULL, 0, userData);
        }
        return 0;
    }

    server->stats.commandsSubmitted++;

    fprintf(stderr, "[SUBMIT] Leader submitting %zu bytes to kraft log\n", len);

    /* Create RPC with entry */
    kraftRpc rpc = {0};

    /* Populate entry with the command data */
    bool populated = kraftRpcPopulateEntryAppend(
        server->raft, &rpc, KRAFT_STORAGE_CMD_APPEND_ENTRIES, data, len);

    if (!populated) {
        fprintf(stderr,
                "[SUBMIT] ERROR: kraftRpcPopulateEntryAppend failed!\n");
        server->stats.commandsRejected++;
        if (cb) {
            cb(server, 0, false, NULL, 0, userData);
        }
        return 0;
    }

    fprintf(stderr, "[SUBMIT] Entry populated, sending to cluster...\n");

    /* Send RPC to all followers (leader will also persist locally) */
    bool sent = kraftRpcLeaderSendRpc(server->raft, &rpc);

    /* Get the index that was assigned - MUST do this before
     * kraftUpdateCommitIndex because that call may trigger apply callbacks
     * synchronously */
    uint64_t index = server->raft->self->consensus.currentIndex;

    fprintf(stderr,
            "[SUBMIT] kraftRpcLeaderSendRpc returned %d, index=%lu, "
            "commitIndex=%lu\n",
            sent, index, server->raft->commitIndex);

    /* Clean up RPC resources before potentially triggering apply */
    if (rpc.entry) {
        server->raft->internals.entry.release(&rpc);
    }

    /* Track pending callback BEFORE advancing commit index, because
     * kraftUpdateCommitIndex triggers apply callbacks synchronously */
    if (cb) {
        addPendingCommand(server, index, cb, userData);
    }

    /* For single-node clusters, manually advance commit since there are no
     * followers to reply. Majority=1=self, so entry is committed immediately.
     * This will synchronously call the apply callback which invokes our pending
     * command callback. */
    uint32_t clusterSize = vecPtrCount(NODES_OLD_NODES(server->raft));
    if (clusterSize == 1) {
        fprintf(stderr, "[SUBMIT] Single-node: advancing commitIndex to %lu\n",
                index);
        kraftUpdateCommitIndex(server->raft, index);
    } else {
        fprintf(
            stderr,
            "[SUBMIT] Multi-node cluster (size=%u): waiting for replication\n",
            clusterSize);
    }

    return index;
}

bool loopyServerIsLeader(const loopyServer *server) {
    if (!server) {
        return false;
    }
    return server->role == LOOPY_SERVER_ROLE_LEADER;
}

loopyServerRole loopyServerGetRole(const loopyServer *server) {
    if (!server) {
        return LOOPY_SERVER_ROLE_FOLLOWER;
    }
    return server->role;
}

uint64_t loopyServerGetLeaderId(const loopyServer *server) {
    if (!server) {
        return 0;
    }
    return server->leaderId;
}

/* ============================================================================
 * Read Queries
 * ============================================================================
 */

bool loopyServerReadLinearizable(loopyServer *server, loopyReadCallback *cb,
                                 void *userData) {
    if (!server || !cb) {
        return false;
    }

    /* For single-node or leader, read is immediately safe */
    if (server->role == LOOPY_SERVER_ROLE_LEADER) {
        server->stats.readsExecuted++;
        cb(server, true, userData);
        return true;
    }

    /* Not leader - read may be stale */
    cb(server, false, userData);
    return true;
}

/* ============================================================================
 * Properties
 * ============================================================================
 */

uint64_t loopyServerGetNodeId(const loopyServer *server) {
    if (!server) {
        return 0;
    }
    return server->nodeId;
}

uint64_t loopyServerGetClusterId(const loopyServer *server) {
    if (!server) {
        return 0;
    }
    return server->clusterId;
}

uint64_t loopyServerGetTerm(const loopyServer *server) {
    if (!server) {
        return 0;
    }
    return server->term;
}

uint64_t loopyServerGetCommitIndex(const loopyServer *server) {
    if (!server) {
        return 0;
    }
    return server->commitIndex;
}

uint64_t loopyServerGetLastApplied(const loopyServer *server) {
    if (!server) {
        return 0;
    }
    return server->lastApplied;
}

loopyPlatform *loopyServerGetPlatform(const loopyServer *server) {
    if (!server) {
        return NULL;
    }
    return server->platform;
}

void *loopyServerGetUserData(const loopyServer *server) {
    if (!server) {
        return NULL;
    }
    return server->userData;
}

void loopyServerSetUserData(loopyServer *server, void *userData) {
    if (!server) {
        return;
    }
    server->userData = userData;
}

void loopyServerSetCallback(loopyServer *server, loopyServerCallback *callback,
                            void *userData) {
    if (!server) {
        return;
    }
    server->callback = callback;
    server->userData = userData;
}

void loopyServerGetStats(const loopyServer *server, loopyServerStats *stats) {
    if (!stats) {
        return;
    }

    if (!server) {
        memset(stats, 0, sizeof(*stats));
        return;
    }

    *stats = server->stats;
}

/* ============================================================================
 * Cluster Management
 * ============================================================================
 */

bool loopyServerAddNode(loopyServer *server, uint64_t nodeId, const char *addr,
                        int port) {
    if (!server || nodeId == 0 || !addr || port <= 0) {
        return false;
    }

    /* Only leader can modify membership */
    if (server->role != LOOPY_SERVER_ROLE_LEADER) {
        return false;
    }

    /* TODO: Implement joint consensus via kraft */
    /* For now, just connect to the new node */
    return loopyPlatformConnectPeer(server->platform, nodeId, addr, port);
}

bool loopyServerRemoveNode(loopyServer *server, uint64_t nodeId) {
    if (!server || nodeId == 0) {
        return false;
    }

    /* Only leader can modify membership */
    if (server->role != LOOPY_SERVER_ROLE_LEADER) {
        return false;
    }

    /* TODO: Implement via kraft joint consensus */
    loopyPlatformDisconnectPeer(server->platform, nodeId);
    return true;
}

bool loopyServerTransferLeadership(loopyServer *server, uint64_t nodeId) {
    if (!server) {
        return false;
    }

    /* Must be leader to transfer */
    if (server->role != LOOPY_SERVER_ROLE_LEADER) {
        return false;
    }

    /* TODO: Implement via kraft leadership transfer */
    (void)nodeId;
    return false;
}

/* ============================================================================
 * Internal: Platform Callback
 * ============================================================================
 */

static void platformCallback(loopyPlatform *platform, loopyPlatformEvent event,
                             loopySession *session, const void *data,
                             size_t len, void *userData) {
    loopyServer *server = (loopyServer *)userData;
    (void)platform;
    (void)session;
    (void)data;
    (void)len;

    switch (event) {
    case LOOPY_PLATFORM_EVENT_STARTED:
        /* Platform started */
        break;

    case LOOPY_PLATFORM_EVENT_STOPPED:
        /* Platform stopped */
        break;

    case LOOPY_PLATFORM_EVENT_PEER_JOINED:
        /* Peer connected and verified */
        if (server->callback) {
            server->callback(server, LOOPY_SERVER_EVENT_PEER_ADDED, session,
                             server->userData);
        }
        break;

    case LOOPY_PLATFORM_EVENT_PEER_LEFT:
        /* Peer disconnected */
        if (server->callback) {
            server->callback(server, LOOPY_SERVER_EVENT_PEER_REMOVED, session,
                             server->userData);
        }
        break;

    case LOOPY_PLATFORM_EVENT_MESSAGE:
        /* RPC message received from peer - route to kraft consensus */
        if (server->raft && session && data && len > 0) {
            // Get peer node ID from session
            const loopyNodeId *peerId = loopySessionGetPeerId(session);
            if (!peerId) {
                fprintf(stderr, "[RPC-RECV] No peer ID from session!\n");
                break;
            }

            fprintf(stderr, "[RPC-RECV] Got %zu bytes from node %lu\n", len,
                    peerId->nodeId);

            kraftNode *node =
                loopyServerFindKraftNode(server->raft, peerId->nodeId);

            if (!node) {
                fprintf(stderr, "[RPC-RECV] WARNING: No kraft node for %lu\n",
                        peerId->nodeId);
                break;
            }

            if (node && node->client) {
                fprintf(stderr, "[RPC-RECV] Found node, adding to buffer\n");

                // Add incoming data to node's protocol buffer
                protoKitProcessPacketData(&server->raft->protoKit,
                                          &node->client->buf, (uint8_t *)data,
                                          len);

                fprintf(stderr, "[RPC-RECV] Processing RPCs from buffer...\n");

                // Process all complete RPCs in the buffer
                kraftRpc rpc;
                memset(&rpc, 0, sizeof(rpc));
                kraftProtocolStatus protocolStatus;

                while (
                    (protocolStatus =
                         server->raft->internals.protocol.populateRpcFromClient(
                             server->raft, node, &rpc)) ==
                    KRAFT_PROTOCOL_STATUS_RPC_POPULATED) {
                    fprintf(stderr, "[RPC-RECV] Got complete RPC, cmd=%u\n",
                            rpc.cmd);

                    // CRITICAL: Set source node for RPC replies
                    rpc.src = node;

                    // Process RPC through kraft consensus engine
                    kraftRpcStatus rpcStatus;
                    fprintf(stderr, "[RPC-RECV] Calling kraftRpcProcess...\n");
                    if (kraftRpcProcess(server->raft, &rpc, &rpcStatus)) {
                        fprintf(stderr,
                                "[RPC-RECV] RPC processed OK, syncing state\n");
                        // Sync state and fire callbacks
                        loopyServerSyncStateFromKraft(server);
                    } else {
                        fprintf(stderr,
                                "[RPC-RECV] RPC processing returned false, "
                                "status=%u\n",
                                rpcStatus);
                    }

                    // Prepare for next RPC
                    memset(&rpc, 0, sizeof(rpc));
                }

                fprintf(stderr, "[RPC-RECV] Done processing all RPCs\n");
            }
        }
        break;

    case LOOPY_PLATFORM_EVENT_TIMER:
        /* Timer fired - trigger kraft consensus */
        if (data && server->raft) {
            loopyTimerType timerType = *(loopyTimerType *)data;

            if (timerType == LOOPY_TIMER_ELECTION) {
                server->stats.electionTimeouts++;
                fprintf(stderr,
                        "[TIMER] ELECTION timeout fired, calling vote\n");
                // Trigger kraft election (will use pre-vote if enabled)
                if (server->raft->internals.election.vote) {
                    bool result =
                        server->raft->internals.election.vote(server->raft);
                    fprintf(stderr, "[TIMER] Vote returned %d\n", result);
                    loopyServerSyncStateFromKraft(server);
                }

            } else if (timerType == LOOPY_TIMER_HEARTBEAT) {
                fprintf(stderr, "[TIMER] HEARTBEAT timer fired\n");
                // Trigger kraft heartbeat (leader only)
                if (server->raft->internals.election.sendHeartbeat) {
                    server->raft->internals.election.sendHeartbeat(
                        server->raft);
                    loopyServerSyncStateFromKraft(server);
                }

            } else if (timerType == LOOPY_TIMER_ELECTION_BACKOFF) {
                // Retry election after backoff
                if (server->raft->internals.election.vote) {
                    server->raft->internals.election.vote(server->raft);
                    loopyServerSyncStateFromKraft(server);
                }
            }
        }
        break;
    }
}
