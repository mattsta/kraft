#include "kraft.h"
#include "readIndex.h"

void kraftNodeForceVerify(kraftNodes *nodes, kraftNode *node) {
    /* Add node to verified list */
    vecError err = vecPtrPushUnique(nodes->nodes, node);
    if (err != VEC_OK && err != VEC_ERROR_EXISTS) {
        D("CRITICAL: Failed to add node to verified list: error=%d", err);
        assert(NULL && "Failed to add node to verified list");
        return;
    }
    node->status = KRAFT_NODE_VERIFIED;
    D("After grow to VERIFIED, now we have %" PRIu32 " nodes (added %p)",
      vecPtrCount(nodes->nodes), (void *)node);
}

static bool _kraftNodeMoveFromPendingToVerified(kraftState *state,
                                                kraftNodes *nodes,
                                                kraftNode *pending) {
    (void)state;
    /* We're verified, so we can delete our pending entry. */
    if (!vecPtrRemoveValue(nodes->pending, pending)) {
        D("Node %p not found in pending nodes!", (void *)pending);
        assert(NULL && "Requested node not found in pending nodes!");
        return false;
    }

    /* Now add new node to proper authorized nodes list */
    kraftNodeForceVerify(nodes, pending);
    return true;
}

bool kraftNodeMoveFromPendingToVerified(kraftState *state, kraftNode *pending) {
    if (vecPtrContains(NODES_OLD_PENDING(state), pending)) {
        _kraftNodeMoveFromPendingToVerified(state, _NODES_OLD(state), pending);
        return true;
    }

    /* If the pending->nodeId is in newNodes->members, add to new. */
    if (vecPtrContains(NODES_NEW_PENDING(state), pending)) {
        _kraftNodeMoveFromPendingToVerified(state, _NODES_NEW(state), pending);
        return true;
    }
    return false;
}

kraftNode *kraftNodeNew(kraftClient *client) {
    kraftNode *node = calloc(1, sizeof(*node));
    /* "When servers start up, they begin as followers." */
    node->consensus.role = KRAFT_FOLLOWER;
    node->client = client;
    D("NEW NODE IS: %p", (void *)node);
    return node;
}

kraftNode *kraftNodeAllocateSelf(kraftClusterId clusterId, kraftNodeId nodeId) {
    kraftNode *node = kraftNodeNew(NULL);
    node->clusterId = clusterId;
    node->isOurself = true;
    node->nodeId = nodeId;
    node->runId = kraftustime();
    return node;
}

kraftNode *kraftNodeAddPending(kraftState *state, kraftNodes *nodes,
                               kraftNode *node) {
    (void)state;
    vecError err = vecPtrPushUnique(nodes->pending, node);
    if (err != VEC_OK && err != VEC_ERROR_EXISTS) {
        D("CRITICAL: Failed to add node to pending list: error=%d", err);
        assert(NULL && "Failed to add node to pending list");
        return NULL;
    }

    D("After grow of PENDING, now we have pending %u nodes (added %p)",
      vecPtrCount(nodes->pending), (void *)node);

    return node;
}

/* Allocate new node and attach to list of nodes the system knows about. */
kraftNode *kraftNodeCreateOld(kraftState *state, kraftClient *client) {
    return kraftNodeAddPending(state, _NODES_OLD(state), kraftNodeNew(client));
}

kraftNode *kraftNodeCreateNew(kraftState *state, kraftClient *client) {
    return kraftNodeAddPending(state, _NODES_NEW(state), kraftNodeNew(client));
}

kraftClient *kraftClientNew(kraftState *state, void *stream) {
    (void)state;
    kraftClient *client = calloc(1, sizeof(*client));
    client->stream = stream;
    return client;
}

/* dumb foreach callback */
static void zeroClient(void *element, void *client) {
    kraftNode *node = element;

    if (node->client == client) {
        node->client = NULL;
    }
}

void kraftNodeFree(kraftState *state, kraftNode *node) {
    /* Remove from list of clients */
    if (node->status == KRAFT_NODE_VERIFIED) {
        D("FREEING VERIFIED NODE %p", (void *)node);
        /* If this node->client matches the client of any element in
         * NODES_OLD_NODES, zero the client. */
        vecPtrForEach(NODES_OLD_NODES(state), zeroClient, node->client);
        vecPtrForEach(NODES_NEW_NODES(state), zeroClient, node->client);
        /* Delete 'node' from NODES_OLD_NODES (if it exists) */
        vecPtrRemoveValue(NODES_OLD_NODES(state), node);
        vecPtrRemoveValue(NODES_NEW_NODES(state), node);
    } else {
        D("REMOVING and FREEING PENDING NODE %p", (void *)node);
        vecPtrForEach(NODES_OLD_PENDING(state), zeroClient, node->client);
        vecPtrRemoveValue(NODES_OLD_PENDING(state), node);
        vecPtrForEach(NODES_NEW_PENDING(state), zeroClient, node->client);
        vecPtrRemoveValue(NODES_NEW_PENDING(state), node);
    }

    if (node->client) {
        free(node->client->buf.buf);
    }

    free(node->client);
    free(node);
}

/* Our pass-through argument is in the wrong position to calloc
 * 'kraftNodeFree()' directly, so let's call it in a wrapper. */
static void kraftEachNodeFree(void *element, void *state) {
    kraftNodeFree(state, element);
}

void kraftNodesFree(kraftState *state) {
    vecPtrForEach(NODES_OLD_NODES(state), kraftEachNodeFree, state);
    if (NODES_NEW_NODES(state)) {
        vecPtrForEach(NODES_NEW_NODES(state), kraftEachNodeFree, state);
    }
}

/* epoch us */
uint64_t kraftustime(void) {
    struct timeval tv = {0};
    uint64_t ust;

    gettimeofday(&tv, NULL);
    ust = ((uint64_t)tv.tv_sec) * 1000000;
    ust += tv.tv_usec;
    return ust;
}

/* epoch ms */
uint64_t kraftmstime(void) {
    return kraftustime() / 1000;
}

static bool kraftInitNodesInternal(kraftState *state) {
    /* Holds pointers to *kraftNode */
    NODES_OLD_NODES(state) = vecPtrNewGrowable(32, NULL);
    if (!NODES_OLD_NODES(state)) {
        D("CRITICAL: Failed to allocate NODES_OLD_NODES");
        return false;
    }

    NODES_NEW_NODES(state) = vecPtrNewGrowable(32, NULL);
    if (!NODES_NEW_NODES(state)) {
        D("CRITICAL: Failed to allocate NODES_NEW_NODES");
        vecPtrFree(NODES_OLD_NODES(state));
        return false;
    }

    NODES_OLD_PENDING(state) = vecPtrNewGrowable(32, NULL);
    if (!NODES_OLD_PENDING(state)) {
        D("CRITICAL: Failed to allocate NODES_OLD_PENDING");
        vecPtrFree(NODES_OLD_NODES(state));
        vecPtrFree(NODES_NEW_NODES(state));
        return false;
    }

    NODES_NEW_PENDING(state) = vecPtrNewGrowable(32, NULL);
    if (!NODES_NEW_PENDING(state)) {
        D("CRITICAL: Failed to allocate NODES_NEW_PENDING");
        vecPtrFree(NODES_OLD_NODES(state));
        vecPtrFree(NODES_NEW_NODES(state));
        vecPtrFree(NODES_OLD_PENDING(state));
        return false;
    }

    /* Holds runId for voting nodes */
    KRAFT_SELF_VOTES_FROM_OLD(state) = vecNewGrowable(32, NULL);
    if (!KRAFT_SELF_VOTES_FROM_OLD(state)) {
        D("CRITICAL: Failed to allocate KRAFT_SELF_VOTES_FROM_OLD");
        vecPtrFree(NODES_OLD_NODES(state));
        vecPtrFree(NODES_NEW_NODES(state));
        vecPtrFree(NODES_OLD_PENDING(state));
        vecPtrFree(NODES_NEW_PENDING(state));
        return false;
    }

    KRAFT_SELF_VOTES_FROM_NEW(state) = vecNewGrowable(32, NULL);
    if (!KRAFT_SELF_VOTES_FROM_NEW(state)) {
        D("CRITICAL: Failed to allocate KRAFT_SELF_VOTES_FROM_NEW");
        vecPtrFree(NODES_OLD_NODES(state));
        vecPtrFree(NODES_NEW_NODES(state));
        vecPtrFree(NODES_OLD_PENDING(state));
        vecPtrFree(NODES_NEW_PENDING(state));
        vecFree(KRAFT_SELF_VOTES_FROM_OLD(state));
        return false;
    }

    /* Holds runId for pre-voting nodes (Pre-Vote Protocol, Raft thesis §9.6) */
    KRAFT_SELF_PREVOTES_FROM_OLD(state) = vecNewGrowable(32, NULL);
    if (!KRAFT_SELF_PREVOTES_FROM_OLD(state)) {
        D("CRITICAL: Failed to allocate KRAFT_SELF_PREVOTES_FROM_OLD");
        vecPtrFree(NODES_OLD_NODES(state));
        vecPtrFree(NODES_NEW_NODES(state));
        vecPtrFree(NODES_OLD_PENDING(state));
        vecPtrFree(NODES_NEW_PENDING(state));
        vecFree(KRAFT_SELF_VOTES_FROM_OLD(state));
        vecFree(KRAFT_SELF_VOTES_FROM_NEW(state));
        return false;
    }

    KRAFT_SELF_PREVOTES_FROM_NEW(state) = vecNewGrowable(32, NULL);
    if (!KRAFT_SELF_PREVOTES_FROM_NEW(state)) {
        D("CRITICAL: Failed to allocate KRAFT_SELF_PREVOTES_FROM_NEW");
        vecPtrFree(NODES_OLD_NODES(state));
        vecPtrFree(NODES_NEW_NODES(state));
        vecPtrFree(NODES_OLD_PENDING(state));
        vecPtrFree(NODES_NEW_PENDING(state));
        vecFree(KRAFT_SELF_VOTES_FROM_OLD(state));
        vecFree(KRAFT_SELF_VOTES_FROM_NEW(state));
        vecFree(KRAFT_SELF_PREVOTES_FROM_OLD(state));
        return false;
    }

    return true;
}

kraftState *kraftStateNew(void) {
    kraftState *state = calloc(1, sizeof(*state));

    return state;
}

bool kraftStateInitRaft(kraftState *state, const size_t ipCount, kraftIp **ip,
                        const kraftClusterId clusterId,
                        const kraftNodeId selfNodeId) {
    state->initialIpsCount = ipCount;
    state->initialIps = ip;

    state->mode.runningAs = KRAFT_RUN_MODE_RAFT;

    /* Initialize configuration defaults */
    /* Pre-Vote Protocol (Raft thesis §9.6)
     * Bug fixes applied Nov 26:
     * - Bug #1: PRE_CANDIDATE now properly transitions to CANDIDATE
     * - Bug #2: Vote replies validated against election term
     * - Bug #3: Pre-vote replies validated against pre-vote term (prevents
     * double election) STILL DEBUGGING: Seed 1764198454 hangs with Pre-Vote
     * enabled but not disabled */
    state->config.enablePreVote = true; /* RE-ENABLED - Leader Completeness
                                           violation specific to Pre-Vote */
    state->config.enableLeadershipTransfer =
        true; /* Re-enabled after confirming not the cause */
    state->config.enableReadIndex =
        false; /* Disable ReadIndex by default (enable when needed) */

    /* Initialize snapshot state (log compaction) */
    state->snapshot.lastIncludedIndex = 0;
    state->snapshot.lastIncludedTerm = 0;
    state->snapshot.triggerThreshold =
        1000; /* Create snapshot after 1000 entries */

    /* Initialize leadership transfer state */
    state->transfer.state = KRAFT_TRANSFER_NONE;
    state->transfer.targetNode = NULL;
    state->transfer.startTime = 0;
    state->transfer.timeout = 0;
    state->transfer.targetMatchIndex = 0;

    /* Initialize ReadIndex state (Raft thesis §6.4) */
    state->readIndex.pendingReads = NULL;
    state->readIndex.currentRound = 0;
    state->readIndex.lastConfirmedRound = 0;

    /* Add ourself to nodes so we know not to count ourself for replication */
    state->self = kraftNodeAllocateSelf(clusterId, selfNodeId);

    /* Add ourself to nodes list for accounting and vote regulation. */
    kraftInitNodesInternal(state);
    kraftNodeForceVerify(_NODES_OLD(state), state->self);

    /* Initialize ReadIndex after nodes are set up (needs vec allocation) */
    kraftReadIndexInit(state);

    /* Initialize metrics */
    kraftMetricsInit(&state->metrics);

    return true;
}

bool kraftStateInitStandalone(kraftState *state) {
    state->mode.runningAs = KRAFT_RUN_MODE_STANDALONE;

    /* Add ourself to nodes since we are both the only "leader" and the
     * only node to exist here. Certain fields are tracked off the self
     * node, so we need it to exist. */
    state->self = kraftNodeAllocateSelf(0, 0);

    return true;
}

void kraftStateSetPreviousIndexTerm(kraftState *state,
                                    kraftRpcUniqueIndex previousIndex,
                                    kraftTerm previousTerm) {
    state->self->consensus.currentIndex = previousIndex;
    state->self->consensus.currentTerm = previousTerm;
    /* CRITICAL: Update previousEntry for vote comparison in elections.
     * Vote comparison uses previousEntry.{term,index} to determine if a
     * candidate's log is at least as up-to-date as ours. Without this update,
     * previousEntry remains {0,0} allowing any candidate to win regardless of
     * their actual log state, violating Raft safety invariants. */
    D("SET_PREV_ENTRY: node runId=%u, setting previousEntry from [idx=%" PRIu64
      ", term=%" PRIu64 "] to [idx=%" PRIu64 ", term=%" PRIu64 "]",
      state->self->runId, state->self->consensus.previousEntry.index,
      state->self->consensus.previousEntry.term, previousIndex, previousTerm);
    state->self->consensus.previousEntry.index = previousIndex;
    state->self->consensus.previousEntry.term = previousTerm;
}

/* Refresh previousEntry from actual storage state.
 * This MUST be called after any operation that modifies the log (deletions,
 * truncations) to keep previousEntry synchronized with actual log state.
 * Election Restriction (§5.4.1) uses previousEntry to compare logs during
 * voting - if previousEntry is stale, we may incorrectly grant votes to
 * candidates with less complete logs, violating Leader Completeness. */
bool kraftStateRefreshPreviousEntry(kraftState *state) {
    uint64_t maxIndex = 0;
    uint64_t maxTerm = 0;
    uint64_t cmd = 0;
    const uint8_t *data = NULL;
    size_t len = 0;

    /* Get the highest index currently in the log */
    if (!kvidxMaxKey(KRAFT_ENTRIES(state), &maxIndex)) {
        /* Log is empty - reset previousEntry to (0, 0) */
        D("REFRESH_PREV: Log is empty, resetting previousEntry to (0, 0)");
        state->self->consensus.previousEntry.index = 0;
        state->self->consensus.previousEntry.term = 0;
        state->self->consensus.currentIndex = 0;
        state->self->consensus.currentTerm = 0;
        return true;
    }

    /* Get the term for that index */
    if (!kvidxGet(KRAFT_ENTRIES(state), maxIndex, &maxTerm, &cmd, &data,
                  &len)) {
        /* This shouldn't happen - maxKey returned an index but we can't get it
         */
        D("REFRESH_PREV: ERROR - maxKey=%" PRIu64
          " exists but kvidxGet failed!",
          maxIndex);
        return false;
    }

    D("REFRESH_PREV: Updating previousEntry from [idx=%" PRIu64
      ", term=%" PRIu64 "] "
      "to [idx=%" PRIu64 ", term=%" PRIu64 "]",
      state->self->consensus.previousEntry.index,
      state->self->consensus.previousEntry.term, maxIndex, maxTerm);

    state->self->consensus.previousEntry.index = maxIndex;
    state->self->consensus.previousEntry.term = maxTerm;
    state->self->consensus.currentIndex = maxIndex;
    state->self->consensus.currentTerm = maxTerm;

    return true;
}

void kraftStateFree(kraftState *state) {
    if (!state) {
        return;
    }

    free(state->leaderContactTimer);
    free(state->self); /* TODO: Verify this doesn't double free since self is
                          now in nodes */
    vecFree(KRAFT_SELF_VOTES_FROM_OLD(state));
    vecFree(KRAFT_SELF_VOTES_FROM_NEW(state));
    vecFree(KRAFT_SELF_PREVOTES_FROM_OLD(state));
    vecFree(KRAFT_SELF_PREVOTES_FROM_NEW(state));

    vecPtrFree(NODES_OLD_NODES(state));
    vecPtrFree(NODES_OLD_PENDING(state));

    vecPtrFree(NODES_NEW_NODES(state));
    vecPtrFree(NODES_NEW_PENDING(state));

    /* Free ReadIndex state */
    kraftReadIndexFree(state);

    free(state);
}
