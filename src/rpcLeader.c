#include "rpcLeader.h"
#include "leadershipTransfer.h"

/* Append 'data' to 'rpc' as new entry */
bool kraftRpcPopulateEntryAppend(kraftState *state, kraftRpc *rpc,
                                 kraftStorageCmd cmd, const void *data,
                                 const size_t len) {
    /* CRITICAL FIX: Base index on storage's actual max, not cached
     * currentIndex. Previously, currentIndex was incremented blindly, which
     * caused gaps when operations failed to persist entries to storage. This
     * manifested as:
     * - Entry 2 in storage, but currentIndex=7
     * - New entries have prevIdx=0 (wrong!) because kvidxGetPrev can't find
     *   the expected previous entry (e.g., looking for entry 6 when only 2
     * exists)
     * - Followers reject these entries, but leader's currentIndex keeps growing
     * - Eventually causes non-monotonic terms in logs and Leader Completeness
     *   violations
     *
     * By fetching max from storage, we ensure the new entry index is always
     * contiguous with existing entries. If previous operations failed to
     * persist, we simply retry at the correct index. */
    kraftRpcUniqueIndex maxIndex = 0;
    kvidxMaxKey(KRAFT_ENTRIES(state), &maxIndex);

    kraftRpcUniqueIndex index = maxIndex + 1;

    /* Update currentIndex to match storage reality */
    if (state->self->consensus.currentIndex != index) {
        D("SYNC: currentIndex was %" PRIu64 " but storage max is %" PRIu64
          " - setting to %" PRIu64,
          state->self->consensus.currentIndex, maxIndex, index);
    }
    state->self->consensus.currentIndex = index;

    D("INCREMENTED CURRENT INDEX TO: %" PRIu64 " (storage max was %" PRIu64 ")",
      state->self->consensus.currentIndex, maxIndex);

    kraftTerm term = state->self->consensus.term;

    return state->internals.entry.encode(rpc, index, term, cmd, data, len);
}

/* ====================================================================
 * NON-SPEC: Persist as appropriate depending on 'runningAs'
 * ==================================================================== */
bool kraftRpcLeaderPersistCustom(
    kraftState *restrict state, void *restrict customSource,
    const size_t sourceLength,
    bool (*sourceGetEntry)(void *customSource, size_t i, void **data,
                           size_t *len),
    void (*sourceFreeEntry)(void *entry),
    uint64_t (*sourceWriteEntriesStandalone)(kvidxInstance *restrict ki,
                                             void *restrict customSource,
                                             uint64_t startIdx,
                                             uint64_t term)) {
    /* Depending on our operating mode, we need two different behaviors:
     *  - RAFT MODE
     *      - We must generate a linearization of custom entry for event log
     *  - STANDALONE MODE
     *      - We can just send the entry down to the custom handler DIRECTLY
     */

    if (state->mode.runningAs == KRAFT_RUN_MODE_RAFT) {
        /* If we are fully encumbered by the RAFT state machine,
         * we still log locally and we also still insert into our own
         * local exploded persist state, BUT if we have to roll back
         * (or forward-erase) our RAFT log, we also need to revert our
         * locally exploded persist state. :TODO */

        assert(NULL && "Not tested!");
        return kraftRpcLeaderSendRpcCustom(state, customSource, sourceLength,
                                           sourceGetEntry, sourceFreeEntry);
    }

    /* else, is standalone (yes, INVALID also exists, but we don't care here) */

    /* CRITICAL FIX: Sync currentIndex with storage before use.
     * Same fix as other increment locations - prevents index gaps. */
    kraftRpcUniqueIndex maxIndex = 0;
    kvidxMaxKey(KRAFT_ENTRIES(state), &maxIndex);
    if (state->self->consensus.currentIndex != maxIndex) {
        D("SYNC [C]: currentIndex was %" PRIu64 " but storage max is %" PRIu64,
          state->self->consensus.currentIndex, maxIndex);
        state->self->consensus.currentIndex = maxIndex;
    }

    /* In STANDALONE mode, we DO NOT USE THE RAFT RPC LOG. We go DIRECTLY into
     * the custom-provided state DB. */

    kraftRpcUniqueIndex newIndex = sourceWriteEntriesStandalone(
        &state->log.kvidx, customSource, state->self->consensus.currentIndex,
        state->self->consensus.currentTerm);

    /* Update currentIndex, currentTerm, and previousEntry */
    kraftStateSetPreviousIndexTerm(state, newIndex,
                                   state->self->consensus.currentTerm);

    D("INCREMENTED CURRENT INDEX [C] TO: %" PRIu64 "",
      state->self->consensus.currentIndex);

    return true;
}

/* ====================================================================
 * NON-SPEC: Extract values from 'customSource' then run AppendEntries RPC
 * ==================================================================== */
bool kraftRpcLeaderSendRpcCustom(kraftState *state, void *customSource,
                                 const size_t sourceLength,
                                 bool (*sourceGetEntry)(void *customSource,
                                                        size_t i, void **data,
                                                        size_t *len),
                                 void (*sourceFreeEntry)(void *entry)) {
    const kraftTerm term = state->self->consensus.term;

    kraftRpc rpc_ = {0};
    kraftRpc *rpc = &rpc_;

    /* Incoming zvm has:
     *  - user command, user data (key->value or key->{field,value}.
     *  - We need to generate: entryIndex per command */

    /* MUST iterate RPC to:
     *  - pull commands and data/parameters
     *  - append all the data points to the local log. (generating entryIndex
     * for each row)
     *  - convert commands+params into linear encoding for replication
     *
     *  We want to iterate ONCE, which means we need one iterator for the
     *  RPC to: extract indexed data and append data as we come across it. */

    /* TODO: implement entrySize API and make it a param, or just make
     *       it a const argument */

    /* Use VLA for small arrays, malloc for large ones.
     * VLA must be declared at function scope to remain valid. */
    kraftRpcUniqueIndex idx_stack[sourceLength <= 64 ? sourceLength : 1];
    void *data_stack[sourceLength <= 64 ? sourceLength : 1];
    size_t len_stack[sourceLength <= 64 ? sourceLength : 1];

    kraftRpcUniqueIndex *idx_;
    void **data_;
    size_t *len_;
    bool created = false;

    if (sourceLength <= 64) {
        idx_ = idx_stack;
        data_ = data_stack;
        len_ = len_stack;
    } else {
        idx_ = malloc(sizeof(*idx_) * sourceLength);
        data_ = malloc(sizeof(*data_) * sourceLength);
        len_ = malloc(sizeof(*len_) * sourceLength);
        created = true;
    }

    /* Open transaction on the local log */
    kvidxBegin(KRAFT_ENTRIES(state));

    /* CRITICAL FIX: Sync currentIndex with storage before incrementing.
     * Same fix as kraftRpcPopulateEntryAppend - prevents index gaps. */
    kraftRpcUniqueIndex maxIndex = 0;
    kvidxMaxKey(KRAFT_ENTRIES(state), &maxIndex);
    if (state->self->consensus.currentIndex != maxIndex) {
        D("SYNC [B]: currentIndex was %" PRIu64 " but storage max is %" PRIu64,
          state->self->consensus.currentIndex, maxIndex);
        state->self->consensus.currentIndex = maxIndex;
    }

    for (size_t i = 0; i < sourceLength; i++) {
        void *d;
        size_t l;

        /* TODO: implement entryGet API */
        /* TODO: do we need entryFree? how will this entry be encoded? */
        sourceGetEntry(customSource, i, &d, &l);

        idx_[i] = ++state->self->consensus.currentIndex;
        data_[i] = d;
        len_[i] = l;

        D("INCREMENTED CURRENT INDEX [B] TO: %" PRIu64
          " (storage max was %" PRIu64 ")",
          state->self->consensus.currentIndex, maxIndex);

        if (i == 0) {
            /* Need to send RPC header with previous fields. */
            /* We could *potentially* use cached values here, but evaluate how
             * saving this value could interact during failover or startup. */
            const bool hasPrev =
                kvidxGetPrev(KRAFT_ENTRIES(state), idx_[i], &rpc->previousIndex,
                             &rpc->previousTerm, NULL, NULL, NULL);

            if (!hasPrev) {
                /* If no previous, then we are the first entry and we
                 * are starting new logs on all followers. */
                rpc->previousIndex = 0;
                rpc->previousTerm = 0;
            }
        }

        // JC if necessary

        /* TODO: we need TWO PATHS HERE:
         *          - PATH A: Full Raft state machine by append to log, sending
         *                    to followers, waiting for quorum, then applying to
         *                    state machine (includes updating on-disk DB).
         *          - PATH B: no Raft log, just update on-disk DB directly. */
        const bool inserted = kraftLogAppend(
            state, idx_[i], term, KRAFT_STORAGE_CMD_APPEND_ENTRIES, d, l);
        if (!inserted) {
            /* reasons could be: bad management (duplicate indexes), out of disk
             * space, ... ? */
            assert(NULL && "Didn't insert at entryIndex!");
            kvidxCommit(KRAFT_ENTRIES(state));
            return false;
        }
    }

    /* Commit the local log transaction */
    kvidxCommit(KRAFT_ENTRIES(state));

    /* TODO: implement encodeBulk API */
    state->internals.entry.encodeBulk(rpc, KRAFT_STORAGE_CMD_APPEND_ENTRIES,
                                      term, sourceLength, idx_,
                                      (const void **const)data_, len_);

    /* "then issues AppendEntries RPCs in parallel to each of the other servers
     * to replicate the entry." */
    kraftRpcSendBroadcast(state, rpc);

    if (created) {
        free(idx_);
        free(data_);
        free(len_);
    }

    return false;
}

/* ====================================================================
 * SPEC: AppendEntries RPC
 * "AppendEntries RPCs are initiated by leaders to replicate log entries and
 * to provide a form of heartbeat (Section 5.3)"
 * ==================================================================== */
/* SPEC: Leader portion of AppendEntries RPC */
bool kraftRpcLeaderSendRpc(kraftState *state, kraftRpc *rpc) {
    if (!IS_LEADER(state->self)) {
        assert(NULL && "Attempted to send RPC when not leader!");
        return false;
    }

    /* Populate RPC with common request parameters */
    kraftRpcPopulate(state, rpc);

    /* Tell RPC we're APPEND_ENTRIES */
    rpc->cmd = KRAFT_RPC_APPEND_ENTRIES;

    /* Replicate to majority of nodes */
    /* SYNCHRONOUS:
     *   - If successfully replicated, commit, apply to state machine, return
     * result.
     *   - If a mjaority of other nodes have more updated data, update our data.
     * (§5.3, §5.4)
     * ASYNC:
     *   - replicate, apply locally, return result.
     * ASYNC ERROR:
     *   - if error detected from previous async replication, return to SYNC
     * flow. */

    /* "In Raft, the leader handles inconsistencies by forcing the followers’
     * logs to duplicate its own. This means that conflicting entries in
     * follower logs will be overwritten with entries from the leader’s log." */

    /* "To bring a follower’s log into consistency with its own, the leader must
     * find the latest log entry where the two logs agree, delete any entries in
     * the follower’s log after that point, and send the follower all of the
     * leader’s entries after that point. All of these actions happen in
     * response to the consistency check performed by AppendEntries RPCs." */

    /* "We recommend that if a client request is blocked by the extra condition
     * on commitment, the leader should create a no-op entry in its log and
     * replicate this across the cluster. We refer the reader to our technical
     * report for a detailed analysis of this issue.
     *
     * (The “extra condition on commitment” refers to the fact that a new leader
     * can only commit entries from a previous term once it has successfully
     * replicated an entry from the current term)." */

    /* "The leader appends the command to its log as a new entry" */
    kraftTerm entryTerm = 0;
    kraftRpcUniqueIndex entryIndex = 0;
    uint8_t *entry = NULL;
    size_t entryLen = 0;
    kraftStorageCmd originalCmd;
    bool first = true;

    /* If we get called here, we expect to have at least one
     * entry available. If we don't have an entry, it's a logic error. */
    assert(rpc->entry && rpc->remaining);

    /* Reset resume pointer to start of flex for decoding */
    rpc->resume = flexHead(rpc->entry);

    /* Open transaction on the local log */
    kvidxBegin(KRAFT_ENTRIES(state));
    while (state->internals.entry.decode(rpc, &entryIndex, &entryTerm,
                                         &originalCmd, &entry, &entryLen)) {
        assert(entryIndex > 0);
        assert(originalCmd > 0);

        /* "When sending an AppendEntries RPC, the leader includes the index
         * and term of the entry in its log that immediately precedes the
         * new entries." */
        if (first) {
            /* If this is the first entry in the RPC (meaning then
             * _lowest_ index in the RPC, then we want to get the previous
             * index/term pair for sending the RPC (AppendEntries contract). */

            /* We could *potentially* use cached values here, but evaluate how
             * that could interact during failover or startup. */
            const bool hasPrev = kvidxGetPrev(
                KRAFT_ENTRIES(state), entryIndex, &rpc->previousIndex,
                &rpc->previousTerm, NULL, NULL, NULL);

            if (!hasPrev) {
                /* If no previous, then we are the first entry and we
                 * are starting new logs on all followers. */
                rpc->previousIndex = 0;
                rpc->previousTerm = 0;
            }

            first = false;
        }

        kraftRpcJointConsensusIfNecessary(state, entryIndex, originalCmd, entry,
                                          entryLen);

        const bool inserted =
            kraftLogAppend(state, entryIndex, state->self->consensus.term,
                           originalCmd, entry, entryLen);
        if (!inserted) {
            /* reasons could be: bad management (duplicate indexes), out of disk
             * space, ... ? */
            assert(NULL && "Didn't insert at entryIndex!");
            kvidxCommit(KRAFT_ENTRIES(state));
            return false;
        }
    }

    /* Commit the local log transaction */
    kvidxCommit(KRAFT_ENTRIES(state));

    /* "then issues AppendEntries RPCs in parallel to each of the other servers
     * to replicate the entry." */
    kraftRpcSendBroadcast(state, rpc);

    return true;
}

static bool kraftRpcLeaderSendRpcCatchup(kraftState *state, kraftNode *node,
                                         kraftRpcUniqueIndex previousIndex,
                                         uint32_t maxBytes) {
    if (!IS_LEADER(state->self)) {
        return false;
    }

    D("Sending Leader RPC Catchup!");

    kraftRpc rpc = {0};
    rpc.cmd = KRAFT_RPC_APPEND_ENTRIES;

    /* Populate RPC with common request parameters */
    kraftRpcPopulate(state, &rpc);

    bool first = true;
    /* previousIndex is the reported highest index from the follower.
     * Upon bootstraping a new follower, previousIndex == 0, so our
     * nextIndex is our first entry in the log. */
    D("CATCHUP for node %p: Setting nextIndex from %" PRIu64 " to %" PRIu64
      " (previousIndex=%" PRIu64 ")",
      (void *)node, node->consensus.leaderData.nextIndex, previousIndex + 1,
      previousIndex);
    node->consensus.leaderData.nextIndex = previousIndex + 1;

    /* Populate 'rpc' with entires until we reach 'maxBytes' or run out of
     * entries. */
    while (rpc.len < maxBytes) {
        /* Encode 'index' into RPC */
        kraftTerm entryTerm = 0;
        const uint8_t *entry = NULL;
        size_t entryLen = 0;
        uint64_t originalCmd = 0;

        bool indexFound =
            kvidxGet(KRAFT_ENTRIES(state), node->consensus.leaderData.nextIndex,
                     &entryTerm, &originalCmd, &entry, &entryLen);

        if (!indexFound) {
            D("Aborting catchup -- entry not found at index %" PRIu64,
              node->consensus.leaderData.nextIndex);
            /* We ran out of entries; send what we have. */
            break;
        }

        /* For the first entry in our AppendedEntries, we must populate the
         * *previous* index and term as well. */
        if (first) {
            kraftRpcUniqueIndex prevEntryIndex = 0;
            kraftTerm prevEntryTerm = 0;
            bool previousFound = kvidxGetPrev(
                KRAFT_ENTRIES(state), node->consensus.leaderData.nextIndex,
                &prevEntryIndex, &prevEntryTerm, NULL, NULL, NULL);

            /* If no previous, then 'index' is our first entry. */
            if (!previousFound) {
                /* CRITICAL: previousFound=false is only valid for nextIndex=1.
                 * If we're at a high index and don't have the previous entry,
                 * it means our log has a gap. We can't send entries without
                 * the previous entry for consistency checking.
                 * Solution: Reset to beginning and let follower catch up from
                 * index 1. */
                if (node->consensus.leaderData.nextIndex > 1) {
                    D("WARNING: Leader missing entry before index %" PRIu64
                      ", resetting to 1",
                      node->consensus.leaderData.nextIndex);
                    node->consensus.leaderData.nextIndex = 1;
                    /* Retry from the beginning */
                    return kraftRpcLeaderSendRpcCatchup(state, node, 0,
                                                        maxBytes);
                }
                prevEntryIndex = 0;
                prevEntryTerm = 0;
            }

            /* Note: prevEntryIndex may not equal (nextIndex - 1) if the log has
             * gaps due to compaction. The important thing is that
             * prevEntryIndex is strictly less than nextIndex and is the actual
             * previous entry we have available. The follower will verify this
             * matches. */
            if (prevEntryIndex >= node->consensus.leaderData.nextIndex) {
                D("ERROR: prevEntryIndex=%" PRIu64 " >= nextIndex=%" PRIu64,
                  prevEntryIndex, node->consensus.leaderData.nextIndex);
                return false;
            }

            rpc.previousIndex = prevEntryIndex;
            rpc.previousTerm = prevEntryTerm;
            first = false;
        }

        D("Encoding entry at index=%" PRIu64 " (previousIndex=%" PRIu64
          ", first=%d)",
          node->consensus.leaderData.nextIndex, rpc.previousIndex,
          first ? 1 : 0);

        state->internals.entry.encode(&rpc,
                                      node->consensus.leaderData.nextIndex,
                                      entryTerm, originalCmd, entry, entryLen);

        /* SPEC:
         *   Leader tracks next index to send to node. */
        node->consensus.leaderData.nextIndex++;
    }

    /* Now send the entire aggregated RPC. */
    kraftRpcSendSingle(state, &rpc, node);
    return true;
}

static uint32_t countLoggedRemotely(kraftState *state, kraftRpcUniqueIndex N,
                                    kraftNodes *inNodes) {
    uint32_t nodeCount = vecPtrCount(inNodes->nodes);
    uint32_t aboveN = 0;
    for (uint32_t i = 0; i < nodeCount; i++) {
        kraftNode *node;
        vecPtrGet(inNodes->nodes, i, (void **)&node);

        /* Skip self - leader's matchIndex isn't tracked via AppendEntries OK
         * replies. The leader is counted separately in
         * majorityOfNodesLoggedEntry. */
        if (node == state->self) {
            D("Skipping self (node %u) in remote count", i);
            continue;
        }

        D("Checking node %u matchindex %" PRIu64 " >= N %" PRIu64 "", i,
          node->consensus.leaderData.matchIndex, N);

        if (node->consensus.leaderData.matchIndex >= N) {
            aboveN++;
        }
    }

    return aboveN;
}

static bool majorityOfNodesLoggedEntry(kraftState *state, kraftRpcUniqueIndex N,
                                       kraftNodes *nodes) {
    uint32_t remotelyLoggedCount = countLoggedRemotely(state, N, nodes);

    /* CRITICAL: The leader itself also has the entry (it created it), but
     * countLoggedRemotely only counts followers by matchIndex. We must add 1
     * to include the leader itself in the majority calculation.
     *
     * For a 5-node cluster needing majority=3:
     * - Leader has entry N (1 node)
     * - 2 followers with matchIndex >= N (2 nodes from countLoggedRemotely)
     * - Total = 3 = majority
     *
     * Without this +1, we'd require 3 followers, meaning 4 total nodes,
     * which is stricter than Raft's majority requirement. */
    uint32_t totalLogged = remotelyLoggedCount + 1; /* +1 for leader itself */

    D("Majority check: remotelyLogged=%u, +leader=1, total=%u, needed=%u",
      remotelyLoggedCount, totalLogged, _majorityOfNodes(nodes));

    if (totalLogged >= _majorityOfNodes(nodes)) {
        return true;
    }

    return false;
}

static bool majorityOfAllNodesLoggedEntry(kraftState *state,
                                          kraftRpcUniqueIndex N) {
    if (kraftOperatingUnderJointConsensusOldNew(state)) {
        /* "Agreement(forelectionsandentrycommitment) requires separate
         * majorities from both the old and new configurations." */
        return majorityOfNodesLoggedEntry(state, N, _NODES_OLD(state)) &&
               majorityOfNodesLoggedEntry(state, N, _NODES_NEW(state));
    }

    if (kraftOperatingUnderJointConsensusNew(state)) {
        return majorityOfNodesLoggedEntry(state, N, _NODES_NEW(state));
    }

    return majorityOfNodesLoggedEntry(state, N, _NODES_OLD(state));
}

static bool commitIfNecessary(kraftState *state, kraftRpcUniqueIndex N) {
    if (majorityOfAllNodesLoggedEntry(state, N)) {
        /* DEBUG: Log when leader commits */
        uint32_t remoteCount = countLoggedRemotely(state, N, _NODES_OLD(state));
        fprintf(
            stderr,
            "[LEADER-COMMIT] leader=%u term=%" PRIu64 " committing idx=%" PRIu64
            " (remotelyLogged=%u +1leader = %u, need=%u)\n",
            state->self->nodeId, state->self->consensus.term, N, remoteCount,
            remoteCount + 1, _majorityOfNodes(_NODES_OLD(state)));

        D("Committing index %" PRIu64 " because majority logged the entry!", N);
        kraftUpdateCommitIndex(state, N);
        return true;
    }

    D("Not committing %" PRIu64, N);

    return false;
}

static void sendJointConsensusFinalize(kraftState *state) {
    kraftRpc entries = {0};
    kraftRpcPopulateEntryAppend(state, &entries,
                                KRAFT_STORAGE_CMD_JOINT_CONSENSUS_END, NULL, 0);
    kraftRpcLeaderSendRpc(state, &entries);
}

static void processAppendEntriesOk(kraftState *state, kraftRpc *rpc) {
    /* rpc->previousIndex is the HIGHEST index on rpc->src resulting from the
     * most recent AppendEntries RPC it received. */

    /* CRITICAL: Reject stale RPC replies from previous terms.
     * Stale replies can poison matchIndex values, causing leaders to
     * incorrectly commit entries that weren't actually replicated to a
     * majority. This violates Raft safety and can lead to Leader Completeness
     * violations. */
    if (rpc->term < state->self->consensus.term) {
        D("IGNORING stale AppendEntries reply from term %" PRIu64
          " (current term: %" PRIu64 ")",
          rpc->term, state->self->consensus.term);
        return;
    }

    /* CRITICAL FIX: matchIndex can NEVER exceed the leader's log length.
     * A follower may report entries from a PREVIOUS leader. We can only count
     * entries that THIS leader has in its log. If a follower reports
     * previousIndex=51 but the leader only has 10 entries, we cannot trust
     * that those entries match - they came from a different leader.
     *
     * This bug caused Leader Completeness violations: a newly elected leader
     * would accept high matchIndex values from followers who had entries from
     * previous leaders, then incorrectly commit entries it never replicated. */
    kraftRpcUniqueIndex leaderMaxIndex = state->self->consensus.currentIndex;
    kraftRpcUniqueIndex effectivePreviousIndex = rpc->previousIndex;
    if (effectivePreviousIndex > leaderMaxIndex) {
        D("Capping follower's reported index: follower has %" PRIu64
          " but leader only has %" PRIu64,
          effectivePreviousIndex, leaderMaxIndex);
        effectivePreviousIndex = leaderMaxIndex;
    }

    /* CRITICAL FIX: Only update nextIndex if moving forward.
     * Prevents stale replies from overwriting catchup adjustments.
     * When catchup sets nextIndex back to retry earlier entries, we must not
     * let delayed successful replies advance it incorrectly. */
    kraftRpcUniqueIndex newNextIndex = effectivePreviousIndex + 1;
    if (newNextIndex > rpc->src->consensus.leaderData.nextIndex) {
        rpc->src->consensus.leaderData.nextIndex = newNextIndex;
        D("Updated nextIndex for node %p to: %" PRIu64 " (was %" PRIu64 ")",
          (void *)rpc->src, newNextIndex, effectivePreviousIndex);

        /* CRITICAL: Only update matchIndex for non-stale replies.
         * If we didn't update nextIndex, the reply is stale and should not
         * affect matchIndex. Stale matchIndex updates can cause leaders to
         * incorrectly commit entries that weren't replicated to a majority. */
        if (effectivePreviousIndex >
            rpc->src->consensus.leaderData.matchIndex) {
            rpc->src->consensus.leaderData.matchIndex = effectivePreviousIndex;
            D("Updated matchIndex for node %p to: %" PRIu64, (void *)rpc->src,
              effectivePreviousIndex);
        }
    } else {
        D("Ignoring stale OK reply for node %p: newNext=%" PRIu64
          " <= currentNext=%" PRIu64 " - NOT updating matchIndex",
          (void *)rpc->src, newNextIndex,
          rpc->src->consensus.leaderData.nextIndex);
    }

    /* CHECK:
     * "If there exists an N such that
     *   N > commitIndex,
     *   a majority of matchIndex[i] ≥ N, and
     *   log[N].term == currentTerm:
     * set commitIndex = N (§5.3, §5.4)." */
    kraftRpcUniqueIndex N = effectivePreviousIndex;

    /* CRITICAL FIX: Must fetch actual log term at index N, not from RPC.
     * The Raft safety requirement is that we only commit entries from
     * current term. rpc->previousTerm is follower's reported term,
     * not necessarily the term of entry at index N. */
    kraftTerm logTerm = 0;
    const uint8_t *dummy;
    size_t dummyLen;
    uint64_t dummyCmd;
    bool found = kvidxGet(KRAFT_ENTRIES(state), N, &logTerm, &dummyCmd, &dummy,
                          &dummyLen);

    if (!found) {
        D("WARNING: Could not retrieve term for index %" PRIu64, N);
        return;
    }

    D("Our commit index: %" PRIu64 "; against N: %" PRIu64 "",
      state->commitIndex, N);
    D("logTerm against our term: %" PRIu64 ", %" PRIu64 "", logTerm,
      state->self->consensus.term);
    if (logTerm == state->self->consensus.term) {
        if (N > state->commitIndex) {
            /* "The leader decides when it is safe to apply a log entry to
             * the state machines; such an entry is called committed.
             * Raft guarantees committed entries are durable and will
             * eventually be executed by all of the available state machines.
             * A log entry is committed once the leader that created the entry
             * has replicated it on a majority of the servers
             * This also commits all preceding entries in the leader’s log,
             * including entries created by previous leaders." */
            bool committed = commitIfNecessary(state, N);

            /* "Once C{old,new} has been committed ... it is now safe for the
             * leader to create a log entry describing Cnew and replicate it
             * to the cluster." */
            if (kraftOperatingUnderJointConsensusOldNew(state) && committed &&
                (N >= state->jointConsensusStartIndex)) {
                /* We commited our JOINT_CONSENSUS_START, so now replicate
                 * JOINT_CONSENSUS_END so all nodes disconnect from OLD and
                 * replace OLD with NEW. */
                sendJointConsensusFinalize(state);
            } else if (kraftOperatingUnderJointConsensusNew(state) &&
                       committed && (N >= state->jointConsensusEndIndex)) {
                /* Our JOINT_CONSENSUS_END has been committed (received and
                 * ack'd by majority of nodes), so now we can finally move
                 * to using only the new configuration. */
                kraftRpcJointConsensusFinalize(state);
            }
        }
    }
}

/* As a Leader, all our incoming Raft traffic will be RPC replies
 * from previous RPCs we originated to followers (assuming
 * no prior failovers and all other nodes are up to date on
 * what they should be sending to whom).
 * Here we process two things:
 *   - update our per-node tracking information
 *   - If running fully sync:
 *     - when a majority of nodes have responded to an RPC, we
 *       commit the entry ourselves then signal we can reply back
 *       to the original client. */
bool kraftRpcProcessLeader(kraftState *state, kraftRpc *rpc,
                           kraftRpcStatus *status) {
    *status = KRAFT_RPC_STATUS_OK;

    D("LEADER REPLY PROCESSING: %s", kraftRpcCmds[rpc->cmd]);

    /* TODO: Fix these cases against errors. */
    switch (rpc->cmd) {
    case KRAFT_RPC_APPEND_ENTRIES_REPLY_IDX_TERM_MISMATCH: {
        /* SPEC: decrement nextIndex by one, then send previous index. */
        /* Actual: Send ~2 MB of previous entries at once. */

        /* With concurrent RPCs, we may receive stale mismatch responses.
         * Only process if this response's previousIndex is >= the node's
         * current nextIndex - 1. Otherwise, we've already decremented past
         * this point and should ignore the stale response.
         *
         * Example: Node has nextIndex=50. We sent RPCs with
         * previousIndex=49,48,47. If response for previousIndex=49 comes back,
         * retryFrom=48, set nextIndex=49. If response for previousIndex=47
         * comes back (stale), we should ignore it because we're already at
         * nextIndex=49 and waiting for responses > 47. */
        kraftRpcUniqueIndex currentNextIndex =
            rpc->src->consensus.leaderData.nextIndex;
        kraftRpcUniqueIndex retryFromIndex =
            (rpc->previousIndex > 0) ? (rpc->previousIndex - 1) : 0;

        /* Ignore stale responses: if we've already decremented past this point
         */
        if (retryFromIndex + 1 < currentNextIndex) {
            D("MISMATCH STALE: ignoring response for previousIndex=%" PRIu64
              " (would set nextIndex=%" PRIu64
              ", but current nextIndex=%" PRIu64 ")",
              rpc->previousIndex, retryFromIndex + 1, currentNextIndex);
            return true;
        }

        D("MISMATCH for node %p at previousIndex=%" PRIu64
          ", retrying from %" PRIu64,
          (void *)rpc->src, rpc->previousIndex, retryFromIndex);

        bool sendingCatchup = kraftRpcLeaderSendRpcCatchup(
            state, rpc->src, retryFromIndex, 1024 * 1024 * 2); /* 2 MB */

        if (!sendingCatchup) {
            /* Catchup failed - likely due to log gaps. Don't assert, just log.
             */
            D("WARNING: Catchup failed for node %p from index %" PRIu64,
              (void *)rpc->src, retryFromIndex);
        }
    }
        return true;
    case KRAFT_RPC_APPEND_ENTRIES_REPLY_OK:
        /* Mark this node's nextIndex to one position after it's max reported
         * index. */
        D("Processing OK REPLY for APPEND ENTREIS!");
        processAppendEntriesOk(state, rpc);
        return true;
    default:
        D("ERROR!  Received %s while LEADER!", kraftRpcCmds[rpc->cmd]);
        return false;
    }
}

/* ====================================================================
 * Callback for heartbeat timer to send heartbeats
 * ==================================================================== */
void kraftRpcSendHeartbeat(kraftState *state) {
    /* Process leadership transfer if in progress and enabled */
    if (state->config.enableLeadershipTransfer) {
        kraftTransferLeadershipProcess(state);

        /* If transfer process stepped us down, don't send heartbeat */
        if (!IS_LEADER(state->self)) {
            return;
        }
    }

    /* Check if we should create a snapshot before sending heartbeat */
    kraftSnapshotCheckAndTrigger(state);

    kraftRpc authority = {0};
    kraftRpcPopulate(state, &authority);
    authority.cmd = KRAFT_RPC_APPEND_ENTRIES;
    kraftRpcSendBroadcast(state, &authority);
}

/* All nodes, when added, must have their NextIndex set properly.
 * Also, upon failover, all known nodes have their NextIndex set
 * to our current index + 1. */
kraftRpcUniqueIndex kraftRpcSetNextIndex(kraftState *state, kraftNode *node) {
    kraftRpcUniqueIndex maxIndex = 0;
    kvidxMaxKey(KRAFT_ENTRIES(state), &maxIndex);

    kraftRpcUniqueIndex nextIndex = maxIndex + 1;
    if (node) {
        D("Setting MAX INDEX to: %" PRIu64 "", maxIndex);
        node->consensus.leaderData.nextIndex = nextIndex;
    }

    return nextIndex;
}
