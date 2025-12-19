#include "rpc.h"
#include <stdio.h>

#include "rpcCandidate.h"
#include "rpcFollower.h"
#include "rpcLeader.h"

#include "serverKit.h"

#define FMTIPPORT "%s:%d"
#define ipport(node) (node)->client->ip.ip, (node)->client->ip.port

/* ====================================================================
 * Accounting for nodes in cluster
 * ==================================================================== */
/* FIX: use configured nodes list. */
/* Ourself is included in the nodes list with a marker of 'self' so
 * we don't send data back to ourself, but we're still in the list
 * for accounting reasons. */
static uint32_t clusterNodesCount(const kraftNodes *nodes) {
    return vecPtrCount(nodes->nodes);
}

uint32_t clusterNodesCountOld(kraftState *state) {
    return clusterNodesCount(_NODES_OLD(state));
}

uint32_t clusterNodesCountNew(kraftState *state) {
    return clusterNodesCount(_NODES_NEW(state));
}

/* FIX: use configured nodes list, not just live nodes list.
 * We don't want majority of live nodes, we need majority of all
 * nodes even if a few are offline or unreachable. */
uint32_t _majorityOfNodes(const kraftNodes *nodes) {
    /* Majority is 50% + 1 */
    return MAJORITY(clusterNodesCount(nodes));
}

uint32_t majorityOfNodesOld(kraftState *state) {
    return _majorityOfNodes(_NODES_OLD(state));
}

uint32_t majorityOfNodesNew(kraftState *state) {
    return _majorityOfNodes(_NODES_NEW(state));
}

uint32_t majorityOfNodesAll(kraftState *state) {
    /* TODO: should be configured node count, not size of current nodes list. */

    uint32_t oldCount = clusterNodesCountOld(state);
    uint32_t newCount = clusterNodesCountNew(state);

    return MAJORITY(oldCount + newCount);
}

/* ====================================================================
 * Common Voting Actions
 * ==================================================================== */
static bool rpcLogIsEqualOrGreaterThanSelf(kraftState *state,
                                           const kraftRpc *rpc) {
    /* "RPC includes information about the candidate's log,
     * and the voter denies its vote if its own log is more
     * up-to-date than that of the candidate." */

    /* "Raft determines which of two logs is more up-to-date by comparing the
     * index and term of the last entries in the logs. If the logs have last
     * entries with different terms, then the log with the later term is more
     * up-to-date. If the logs end with the same term, then whichever log is
     * longer is more up-to-date." */

    /* CRITICAL: Query ACTUAL log state, not cached previousEntry!
     * Bug fix: Previously this function compared against
     * state->self->consensus.previousEntry which is a cached value that can
     * become stale after operations like log truncation, crash recovery, or
     * network partitions. Using stale previousEntry data causes voters to
     * incorrectly grant votes to candidates with less complete logs, violating
     * Leader Completeness (Raft Safety Property #4).
     *
     * Fix: Query the actual log directly to get fresh index/term values. */
    uint64_t selfLastIndex = 0;
    uint64_t selfLastTerm = 0;

    if (kvidxMaxKey(KRAFT_ENTRIES(state), &selfLastIndex)) {
        /* Log has entries - get the term of the last entry */
        uint64_t cmd;
        const uint8_t *data;
        size_t len;
        if (!kvidxGet(KRAFT_ENTRIES(state), selfLastIndex, &selfLastTerm, &cmd,
                      &data, &len)) {
            /* This shouldn't happen but handle gracefully */
            D("WARNING: kvidxMaxKey returned %" PRIu64
              " but kvidxGet failed - using cached previousEntry",
              selfLastIndex);
            selfLastIndex = state->self->consensus.previousEntry.index;
            selfLastTerm = state->self->consensus.previousEntry.term;
        }
    }
    /* else: Log is empty, selfLastIndex=0 and selfLastTerm=0 */

    D("LOG COMPARISON: candidate[term=%" PRIu64 ", index=%" PRIu64
      "] vs self[term=%" PRIu64 ", index=%" PRIu64
      "] (actualLog vs cached[term=%" PRIu64 ", index=%" PRIu64 "])",
      rpc->previousTerm, rpc->previousIndex, selfLastTerm, selfLastIndex,
      state->self->consensus.previousEntry.term,
      state->self->consensus.previousEntry.index);

    if (rpc->previousTerm > selfLastTerm) {
        D("  -> GRANT: candidate term > self term");
        return true;
    }

    if (rpc->previousTerm == selfLastTerm) {
        if (rpc->previousIndex >= selfLastIndex) {
            D("  -> GRANT: same term, candidate index >= self index");
            return true;
        }
    }

    D("  -> DENY: self log is more up-to-date");
    return false;
}

/* All roles are capable of voting */
static bool allowVote(kraftState *state, const kraftRpc *rpc) {
    /* If we already voted in this term *and* the matching vote isn't
     * for this requesting client, deny vote. */
    /* "Receiver implementation:
     * 1. Reply false if term < currentTerm (§5.1)
     * 2. If votedFor is null or candidateId, and candidate's log is at
     * least as up-to-date as receiver's log, grant vote (§5.2, §5.4)" */

    /* If requested vote is from the past, ignore. */
    if (rpc->term < state->self->consensus.term) {
        D("ALLOW_VOTE: NO - rpc term %" PRIu64 " < self term %" PRIu64,
          rpc->term, state->self->consensus.term);
        return false;
    }

    /* If already voted this term for *another* node, don't allow other vote.
     * NOTE: votedFor == 0 means "haven't voted yet" (equivalent to null),
     * so we must check for that case explicitly. */
    if (rpc->term == state->self->consensus.term &&
        KRAFT_VOTED_FOR(state) != 0 &&          /* Have voted for someone */
        rpc->runId != KRAFT_VOTED_FOR(state)) { /* Not the same candidate */
        D("ALLOW_VOTE: NO - already voted in term %" PRIu64
          " for runId %" PRIu64 " (not %" PRIu64 ")",
          state->self->consensus.term, KRAFT_VOTED_FOR(state), rpc->runId);
        return false;
    }

    D("ALLOW_VOTE: YES - term %" PRIu64, rpc->term);
    return true;
}

/* All roles reply to RequestVote RPC */
bool kraftRpcProcessRequestVote(kraftState *state, kraftRpc *rpc,
                                kraftRpcStatus *status) {
    /* CRITICAL (Raft §5.1): "If RPC request or response contains term T >
     * currentTerm: set currentTerm = T, convert to follower (§5.1)" This MUST
     * happen BEFORE any other processing to maintain safety invariants.
     *
     * Bug fix: The previous code checked kraftHasLeader() before updating term,
     * which violated Raft's term update rule and caused Leader Completeness
     * violations. Nodes would stay stuck in old terms when they had a leader,
     * preventing them from participating in new elections. */
    if (rpc->term > state->self->consensus.term) {
        D("RequestVote from higher term %" PRIu64 " > %" PRIu64
          " - updating term and converting to follower",
          rpc->term, state->self->consensus.term);
        state->self->consensus.term = rpc->term;
        KRAFT_VOTED_FOR(state) = 0; /* Clear vote when term changes */
        if (!IS_FOLLOWER(state->self)) {
            state->self->consensus.role = KRAFT_FOLLOWER;
            /* TODO: Cancel any pending operations */
        }
    }

    /* "Each server will vote for at most one candidate in a given term, on a
     * first-come-first-served basis
     * (note: Section 5.4 adds an additional restriction on votes)." */

    /* "Reply false if term < currentTerm (§5.1)" */
    if (rpc->term < state->self->consensus.term) {
        kraftErrorReply(KRAFT_RPC_STATUS_REJECTED_VOTE_TERM_TOO_OLD);
        return false; /* CRITICAL: Must return after rejecting! */
    }

    if (rpc->src && rpc->src->client) {
        D("Vote request from " FMTIPPORT, ipport(rpc->src));
    } else {
        D("Vote request from <node without client: src=%p>", (void *)rpc->src);
    }
    /* "If votedFor is null or candidateId, and candidate's log is at
     * least as up-to-date as receiver's log, grant vote (§5.2, §5.4)" */
    if (allowVote(state, rpc)) {
        if (rpcLogIsEqualOrGreaterThanSelf(state, rpc)) {
            /* DEFENSIVE CHECK: Verify we're not granting vote when we have
             * committed entries the candidate might not have. This catches
             * any bugs in the vote comparison logic or log state. */
            if (state->commitIndex > rpc->previousIndex) {
                /* Get voter's actual last log info for debugging */
                uint64_t voterLastIdx = 0;
                uint64_t voterLastTerm = 0;
                if (kvidxMaxKey(KRAFT_ENTRIES(state), &voterLastIdx)) {
                    uint64_t cmd;
                    const uint8_t *data;
                    size_t len;
                    kvidxGet(KRAFT_ENTRIES(state), voterLastIdx, &voterLastTerm,
                             &cmd, &data, &len);
                }
                D("*** VOTE GRANTED WITH COMMITTED ENTRY RISK! "
                  "Voter commitIdx=%" PRIu64 ", voterLast=[%" PRIu64
                  ",t=%" PRIu64 "], candLast=[%" PRIu64 ",t=%" PRIu64 "] -> %s",
                  state->commitIndex, voterLastIdx, voterLastTerm,
                  rpc->previousIndex, rpc->previousTerm,
                  rpc->previousTerm > voterLastTerm    ? "CAND_WINS_BY_TERM"
                  : rpc->previousTerm == voterLastTerm ? "SAME_TERM"
                                                       : "VOTER_WINS_BY_TERM");
            }

            /* Record who we voted for to prevent multiple votes (§5.2) */
            KRAFT_VOTED_FOR(state) = rpc->runId;

            if (rpc->src && rpc->src->client) {
                D("GRANTING VOTE TO " FMTIPPORT " FOR TERM [%" PRIu64 "]",
                  ipport(rpc->src), state->self->consensus.term);
            } else {
                D("GRANTING VOTE TO <node without client: src=%p> FOR TERM "
                  "[%" PRIu64 "]",
                  (void *)rpc->src, state->self->consensus.term);
            }
            kraftOKReply(KRAFT_RPC_VOTE_REPLY_GRANTED);
            return true;
        } else {
            if (rpc->src && rpc->src->client) {
                D("DENYING VOTE TO " FMTIPPORT, ipport(rpc->src));
            } else {
                D("DENYING VOTE TO <node without client: src=%p>",
                  (void *)rpc->src);
            }
            kraftErrorReply(KRAFT_RPC_STATUS_REJECTED_VOTE_LOG_NOT_CURRENT);
            return false;
        }
    } else {
        D("ALREADY VOTED IN RPC TERM %" PRIu64, rpc->term);
        kraftErrorReply(KRAFT_RPC_STATUS_REJECTED_VOTE_ALREADY_VOTED);
        return false;
    }
}

/* Pre-Vote Request Processing (Raft thesis §9.6)
 *
 * KEY DIFFERENCES from regular RequestVote:
 * 1. Does NOT update term
 * 2. Does NOT update votedFor
 * 3. Grants pre-vote if candidate's log is up-to-date
 * 4. Can grant pre-vote even if already voted in current term
 *
 * This prevents disruption from partitioned nodes while still
 * allowing them to check if they could win an election.
 */
bool kraftRpcProcessRequestPreVote(kraftState *state, kraftRpc *rpc,
                                   kraftRpcStatus *status) {
    /* Per Raft thesis §9.6: Only deny pre-vote if we have a leader AND
     * we've heard from them within the MINIMUM election timeout.
     *
     * The thesis specifically says "within the minimum election timeout"
     * not the maximum. This allows pre-votes to succeed more quickly
     * after leader failure, while still preventing disruption when a
     * leader is actively sending heartbeats.
     *
     * NOTE: In test mode, we skip the timing check because the test
     * framework is a state-machine simulation that doesn't use wall clock
     * time. Without this skip, pre-votes would be stuck in denial loops. */
    if (!state->config.testMode && kraftHasLeader(state) &&
        state->leader->lastContact > 0) {
        uint64_t now = kraftmstime();
        uint64_t timeSinceContact = now - state->leader->lastContact;
        /* Use MINIMUM election timeout (150ms) per Raft thesis */
        if (timeSinceContact < 150) {
            D("DENYING PRE-VOTE: heard from leader %" PRIu64 "ms ago (< 150ms)",
              timeSinceContact);
            kraftOKReply(KRAFT_RPC_PREVOTE_REPLY_DENIED);
            return false; /* CRITICAL: Must return after denying! */
        }
        /* Leader exists but no contact for >= minimum election timeout - allow
         * pre-vote */
        D("ALLOWING PRE-VOTE: leader last contact was %" PRIu64
          "ms ago (>= 150ms)",
          timeSinceContact);
    }

    /* Deny pre-vote if requester's term is stale */
    if (rpc->term < state->self->consensus.term) {
        D("DENYING PRE-VOTE: term %" PRIu64 " < currentTerm %" PRIu64,
          rpc->term, state->self->consensus.term);
        kraftOKReply(KRAFT_RPC_PREVOTE_REPLY_DENIED);
        return false; /* CRITICAL: Must return after denying! */
    }

    if (rpc->src && rpc->src->client) {
        D("Pre-vote request from " FMTIPPORT, ipport(rpc->src));
    } else {
        D("Pre-vote request from <node without client: src=%p>",
          (void *)rpc->src);
    }

    /* Grant pre-vote if candidate's log is at least as up-to-date as ours.
     * CRITICAL: We do NOT check votedFor here - pre-votes can be granted
     * even if we've already voted in this term. This is safe because
     * pre-votes don't affect the actual election. */
    if (rpcLogIsEqualOrGreaterThanSelf(state, rpc)) {
        if (rpc->src && rpc->src->client) {
            D("GRANTING PRE-VOTE TO " FMTIPPORT " FOR TERM [%" PRIu64 "]",
              ipport(rpc->src), rpc->term);
        } else {
            D("GRANTING PRE-VOTE TO <node without client: src=%p> FOR TERM "
              "[%" PRIu64 "]",
              (void *)rpc->src, rpc->term);
        }
        kraftOKReply(KRAFT_RPC_PREVOTE_REPLY_GRANTED);
        return true;
    } else {
        if (rpc->src && rpc->src->client) {
            D("DENYING PRE-VOTE TO " FMTIPPORT ": log not current",
              ipport(rpc->src));
        } else {
            D("DENYING PRE-VOTE TO <node without client: src=%p>: log not "
              "current",
              (void *)rpc->src);
        }
        kraftOKReply(KRAFT_RPC_PREVOTE_REPLY_DENIED);
        return false;
    }
}

/* ====================================================================
 * Event Loop Sending
 * ==================================================================== */
void kraftRpcSendSingle(kraftState *state, kraftRpc *rpc, kraftNode *node) {
    if (node->isOurself) {
        return;
    }

    uint8_t *buf;
    uint64_t len;
    state->internals.protocol.encodeRpc(state, rpc, &buf, &len);
    state->internals.rpc.sendRpc(state, rpc, buf, len, node);
    state->internals.protocol.freeRpc(buf);
}

static void eventLoopSendNodes(kraftState *state, kraftRpc *rpc) {
    assert(rpc->cmd > 0 && "No RPC Command Defined!");

    D("eventLoopSendNodes called with cmd=%d, len=%zu", rpc->cmd, rpc->len);

    kraftNodes *nodes[2] = {0};
    if (kraftOperatingUnderJointConsensusOldNew(state)) {
        nodes[0] = _NODES_OLD(state);
        nodes[1] = _NODES_NEW(state);
    } else if (kraftOperatingUnderJointConsensusNew(state)) {
        nodes[0] = _NODES_NEW(state);
    } else {
        nodes[0] = _NODES_OLD(state);
    }

    for (size_t j = 0; j < sizeof(nodes) / sizeof(*nodes); j++) {
        if (!nodes[j]) {
            continue;
        }

        const size_t innerNodeCount = vecPtrCount(nodes[j]->nodes);
        for (size_t i = 0; i < innerNodeCount; i++) {
            kraftNode *node = NULL;
            vecError err = vecPtrGet(nodes[j]->nodes, i, (void **)&node);
            if (err != VEC_OK || !node) {
                D("CRITICAL: Failed to get node at index %zu: error=%d", i,
                  err);
                continue;
            }

            if (!node->isOurself) {
                /* For heartbeats (empty AppendEntries), customize
                 * previousIndex/previousTerm per node. For broadcasts with
                 * entries, only send if node's nextIndex matches the RPC.
                 * Rationale: Each node may be at a different nextIndex.
                 * Broadcasting entries only makes sense when nodes are already
                 * caught up. Nodes that are behind should receive catchup RPCs
                 * instead. */

                D("Processing node %p: rpc->cmd=%d, IS_LEADER=%d, role=%d",
                  (void *)node, rpc->cmd, IS_LEADER(state->self),
                  state->self->consensus.role);

                if (rpc->cmd == KRAFT_RPC_APPEND_ENTRIES &&
                    IS_LEADER(state->self)) {
                    /* Check if this RPC has entries using rpc->len */
                    D("Checking AppendEntries: rpc->len=%zu, prevIdx=%" PRIu64
                      ", prevTerm=%" PRIu64,
                      rpc->len, rpc->previousIndex, rpc->previousTerm);

                    if (rpc->len > 0) {
                        /* RPC has entries - only send to nodes whose nextIndex
                         * would match. The first entry in this RPC should be at
                         * previousIndex + 1. Only send to nodes where nextIndex
                         * == previousIndex + 1. For nodes that don't match,
                         * send catchup instead. */
                        kraftRpcUniqueIndex expectedNextIndex =
                            rpc->previousIndex + 1;
                        D("BROADCAST CHECK: node %p nextIndex=%" PRIu64
                          ", expected=%" PRIu64,
                          (void *)node, node->consensus.leaderData.nextIndex,
                          expectedNextIndex);

                        if (node->consensus.leaderData.nextIndex !=
                            expectedNextIndex) {
                            D("SKIPPING broadcast to node %p: "
                              "nextIndex=%" PRIu64 " but RPC expects %" PRIu64
                              " (will catchup via heartbeat)",
                              (void *)node,
                              node->consensus.leaderData.nextIndex,
                              expectedNextIndex);
                            /* Node is behind - skip this broadcast.
                             * Regular heartbeat mechanism will send catchup
                             * RPCs. */
                            continue;
                        }
                        D("SENDING broadcast to node %p: nextIndex matches "
                          "expected %" PRIu64,
                          (void *)node, expectedNextIndex);
                    } else {
                        /* Empty AppendEntries (heartbeat) - customize per node
                         */
                        kraftRpc nodeRpc = *rpc;
                        kraftRpcUniqueIndex nextIdx =
                            node->consensus.leaderData.nextIndex;

                        if (nextIdx > 1) {
                            kraftRpcUniqueIndex prevIdx;
                            kraftTerm prevTerm;
                            bool found = kvidxGetPrev(
                                KRAFT_ENTRIES(state), nextIdx, &prevIdx,
                                &prevTerm, NULL, NULL, NULL);
                            if (found) {
                                nodeRpc.previousIndex = prevIdx;
                                nodeRpc.previousTerm = prevTerm;
                            } else {
                                /* Leader missing entry before nextIndex - skip
                                 * this node */
                                D("Leader missing entry before "
                                  "nextIndex=%" PRIu64 " for node %p, skipping",
                                  nextIdx, (void *)node);
                                continue;
                            }
                        } else {
                            nodeRpc.previousIndex = 0;
                            nodeRpc.previousTerm = 0;
                        }

                        /* Send customized heartbeat */
                        uint8_t *buf;
                        uint64_t len;
                        state->internals.protocol.encodeRpc(state, &nodeRpc,
                                                            &buf, &len);
                        state->internals.rpc.sendRpc(state, &nodeRpc, buf, len,
                                                     node);
                        state->internals.protocol.freeRpc(buf);
                        continue;
                    }
                }

                /* Send RPC as-is (for non-AppendEntries or broadcast with
                 * matching nextIndex) */
                uint8_t *buf;
                uint64_t len;
                state->internals.protocol.encodeRpc(state, rpc, &buf, &len);
                state->internals.rpc.sendRpc(state, rpc, buf, len, node);
                state->internals.protocol.freeRpc(buf);
            }
        }
    }
}

void kraftRpcSendBroadcast(kraftState *state, kraftRpc *rpc) {
    /* "issue AppendEntries RPCs in parallel to each of the other servers to
     * replicate the entry" */
    /* "If followers crash or run slowly, or if network packets are lost, the
     * leader retries AppendEntries RPCs indefinitely (even after it has
     * responded to the client) until all followers eventually store all log
     * entries." */

    /* TODO: need to track replies to RPCs and re-send if reply not received
     * within a reasonable timeframe. */

    eventLoopSendNodes(state, rpc);

    /* If we're the leader, reset our idle heartbeat timer. */
    /* TODO: per-node leader contact timers? */
    if (IS_LEADER(state->self)) {
        state->internals.timer.heartbeatTimer(state, KRAFT_TIMER_AGAIN);
    }
}

/* ====================================================================
 * Generate RPCs
 * ==================================================================== */
/* Populate 'rpc' with metadata from self. */
bool kraftRpcPopulate(kraftState *state, kraftRpc *rpc) {
    if (!state || !rpc) {
        return false;
    }

    rpc->term = state->self->consensus.term;

    rpc->timestamp = kraftustime();
    rpc->nodeId = state->self->nodeId;
    rpc->runId = state->self->runId;
    rpc->clusterId = state->self->clusterId;

#if 0
    rpc->previousTerm = state->self->consensus.previousEntry.term;
    rpc->previousIndex = state->self->consensus.previousEntry.index;
#endif

    if (IS_LEADER(state->self)) {
        rpc->leaderCommit = state->commitIndex;
    }

    return true;
}

void kraftRpcReplyNoBody(kraftState *state, kraftRpcCmd cmd, kraftRpc *rpc) {
    kraftRpc reply = {0};
    kraftRpcPopulate(state, &reply);

    reply.cmd = cmd;

    /* Slightly confusing terms here.
     * We tell the sender our "previous" index is our maximum/highest index. */
    reply.previousIndex = state->self->consensus.currentIndex;
    reply.previousTerm = state->self->consensus.currentTerm;

    D("At RPC term [%" PRIu64 "] and self-term [%" PRIu64 "]", rpc->term,
      state->self->consensus.term);
    if (rpc->src && rpc->src->client) {
        D("Sending reply with command [%d]: %s to " FMTIPPORT, cmd,
          kraftRpcCmds[cmd], ipport(rpc->src));
    } else {
        D("Sending reply with command [%d]: %s to <node without client: "
          "src=%p>",
          cmd, kraftRpcCmds[cmd], (void *)rpc->src);
    }

    D("Sending OK RPC with max index and max term: (%" PRIu64 ", %" PRIu64 ")",
      reply.previousIndex, reply.previousTerm);
    kraftRpcSendSingle(state, &reply, rpc->src);

    rpc->disconnectSource = false; /* mark original RPC as replied-to */
}

/* Specialized OK reply for AppendEntries that reports the confirmed match
 * index. This is CRITICAL for correct commit calculation. The leader uses this
 * index to determine which entries have been replicated to this follower.
 *
 * BUG FIX: Previously, OK replies reported the follower's own currentIndex,
 * which could be from a DIFFERENT term. This caused leaders to incorrectly
 * think followers had their entries when they actually had old entries from
 * previous leaders, leading to false commits and Leader Completeness
 * violations.
 *
 * The matchIndex should be:
 * - If entries were appended: the last appended entry index
 * - If heartbeat (no entries): the RPC's previousIndex (confirmed match point)
 */
void kraftRpcReplyAppendEntriesOK(kraftState *state, kraftRpc *rpc,
                                  kraftRpcUniqueIndex matchIndex,
                                  kraftTerm matchTerm) {
    kraftRpc reply = {0};
    kraftRpcPopulate(state, &reply);

    reply.cmd = KRAFT_RPC_APPEND_ENTRIES_REPLY_OK;

    /* Report the confirmed match point, not our own currentIndex */
    reply.previousIndex = matchIndex;
    reply.previousTerm = matchTerm;

    D("NODE %u: Sending AppendEntries OK with MATCH index/term: (%" PRIu64
      ", %" PRIu64 ")",
      state->self->nodeId, reply.previousIndex, reply.previousTerm);
    kraftRpcSendSingle(state, &reply, rpc->src);

    rpc->disconnectSource = false;
}

void kraftRpcReplyError(kraftState *state, kraftRpcStatus status,
                        kraftRpc *rpc) {
    D("Replying with error because %s (our term: %" PRIu64
      "; RPC term: %" PRIu64 ")",
      kraftRpcStatuses[status], state->self->consensus.term, rpc->term);

    kraftRpcCmd cmd;
    switch (status) {
    case KRAFT_RPC_STATUS_OK:
        assert(NULL); /* Why request an error reply with OK status? */
    case KRAFT_RPC_STATUS_REJECTED_SELF_RUNID:
        rpc->disconnectSource = true;
        return; /* just drop it, don't tell ourselves we're ourselves. */
    case KRAFT_RPC_STATUS_REJECTED_IDX_TERM_MISMATCH:
        cmd = KRAFT_RPC_APPEND_ENTRIES_REPLY_IDX_TERM_MISMATCH;
        break;
    case KRAFT_RPC_STATUS_REJECTED_RPC_TERM_TOO_OLD:
        /* NOTE: This is a normal condition during elections and re-elections.
         * Don't send a reply for stale RPCs - just ignore them. */
        return; /* We shouldn't have this happen during current testing. */
        break;
    case KRAFT_RPC_STATUS_INVALID_SELF_ROLE:
        cmd = 99999; /* no proper response yet! */
        break;
    case KRAFT_RPC_STATUS_REJECTED_VOTE_TERM_TOO_OLD:
    case KRAFT_RPC_STATUS_REJECTED_VOTE_ALREADY_VOTED:
    case KRAFT_RPC_STATUS_REJECTED_VOTE_LOG_NOT_CURRENT:
        cmd = KRAFT_RPC_VOTE_REPLY_DENIED;
        break;
    default:
        assert(NULL);
        //            kraftRpcReplyNoBody(state, cmd, rpc); /* causes looping */
    }
    D("Sending error reply...");
    kraftRpc reply = {0};
    kraftRpcPopulate(state, &reply);

    /* CRITICAL: For mismatch errors, echo back the FAILED previousIndex so the
     * leader knows which index to decrement from. Previously we sent our
     * highest index, which caused infinite loops when the leader decremented to
     * the same index that just failed.
     *
     * Example of the bug:
     *   1. Leader sends AppendEntries with previousIndex=45
     *   2. Follower doesn't have (45, term), rejects
     *   3. Follower sent previousIndex=46 (its highest) - WRONG!
     *   4. Leader decrements 46->45, resends with previousIndex=45
     *   5. Infinite loop!
     *
     * Fix: Echo the original rpc->previousIndex so leader decrements correctly.
     */
    reply.previousIndex = rpc->previousIndex;

    reply.cmd = cmd;

    kraftRpcSendSingle(state, &reply, rpc->src);
}

/* ====================================================================
 * Apply logs to state machine
 * ==================================================================== */
static void applyLogs(kraftState *state) {
    /* "A log entry is committed once the leader that created the entry
     * has replicated it on a majority of the servers."
     * Commit means 'log has been replicated across majority of cluster.'
     * Applied means 'log has been run on state machine.' */

    /* "If commitIndex > lastApplied:
     * increment lastApplied,
     * apply log[lastApplied] to state machine (§5.3)" */
    while (state->commitIndex > state->lastApplied) {
        state->lastApplied++;

        /* Skip entries that are included in a snapshot */
        if (state->lastApplied <= state->snapshot.lastIncludedIndex) {
            D("Skipping index %" PRIu64 " - included in snapshot at %" PRIu64,
              state->lastApplied, state->snapshot.lastIncludedIndex);
            continue;
        }

        const uint8_t *log;
        size_t len;
        uint64_t cmd;

        D("APPLYING index %" PRIu64 " to state machine!", state->lastApplied);

        const bool retrieved = kvidxGet(
            KRAFT_ENTRIES(state), state->lastApplied, NULL, &cmd, &log, &len);

        if (!retrieved) {
            /* Entry not found - might have been compacted. This shouldn't
             * happen if snapshot logic is correct, but don't crash. */
            D("WARNING: Could not retrieve log entry at index %" PRIu64
              " (snapshot at %" PRIu64 ")",
              state->lastApplied, state->snapshot.lastIncludedIndex);
            continue;
        }

        /* Now act on our STORAGE commands retrieved from the persistent log */
        switch ((kraftStorageCmd)cmd) {
        case KRAFT_STORAGE_CMD_JOINT_CONSENSUS_START:
            /* Joint consensus START: transition to using both old and new
             * configs. The actual node list management is handled by
             * kraftRpcJointConsensusIfNecessary when the entry is first
             * processed. Here we just need to ensure the state is applied. */
            D("Applied JOINT_CONSENSUS_START at index %" PRIu64,
              state->lastApplied);
            break;
        case KRAFT_STORAGE_CMD_JOINT_CONSENSUS_END:
            /* Joint consensus END: transition from old+new to just new config.
             * The node disconnection logic is handled elsewhere.
             * Here we just mark that we've completed the transition. */
            D("Applied JOINT_CONSENSUS_END at index %" PRIu64,
              state->lastApplied);
            /* The actual transition is managed by the leader's RPC processing,
             * which calls disconnectIfNotNew to remove old-only nodes.
             * This apply function just ensures the command is marked as
             * applied. */
            break;
        case KRAFT_STORAGE_CMD_APPEND_ENTRIES:
            state->internals.data.apply(log, len);
            break;
        default:
            D("Attempted log cmd: %" PRIu64 " with len: %zu", cmd, len);
            assert(NULL && "Invalid log cmd!");
        }
    }
}

void kraftUpdateCommitIndex(kraftState *state, kraftRpcUniqueIndex N) {
    /* It doesn't make sense to commit at index 0, but this runs
     * when a follower has no data and receives a heartbeat. */
    if (N == 0) {
        return;
    }

    D("NODE %u: UPDATING COMMIT INDEX FROM %" PRIu64 " TO %" PRIu64,
      state->self->nodeId, state->commitIndex, N);
    state->commitIndex = N;
    applyLogs(state);
}

/* dumbptrforeach callback allowing us to remove nodes
 * if they only exist in OLD and not also in NEW */
static void disconnectIfNotNew(void *element, void *inState) {
    kraftState *state = inState;
    if (!vecPtrContains(NODES_NEW_NODES(state), element)) {
        serverKitSendGoAway(state, element);
    }
}

/* If our storage command is configuration related, then
 * manage configuration actions here. */
void kraftRpcJointConsensusIfNecessary(kraftState *state,
                                       kraftRpcUniqueIndex idx,
                                       kraftStorageCmd cmd, uint8_t *entry,
                                       uint32_t entryLen) {
    if (cmd == KRAFT_STORAGE_CMD_JOINT_CONSENSUS_START) {
        assert(!kraftOperatingUnderJointConsensusOldNew(state));
        assert(!kraftOperatingUnderJointConsensusNew(state));

        state->jointConsensusStartIndex = idx;

        /* Connect to every node in 'entry' */
        kraftRpcJointConsensusBegin(state, entry, entryLen);
    } else if (cmd == KRAFT_STORAGE_CMD_JOINT_CONSENSUS_END) {
        assert(kraftOperatingUnderJointConsensusNew(state));

        state->jointConsensusStartIndex = 0; /* reset */
        state->jointConsensusEndIndex = idx;

        /* Cleanup joint consensus and stop being a leader
         * if we are current leader but not in new configuration. */
        kraftRpcJointConsensusFinalize(state);
    }
}

void kraftRpcJointConsensusBegin(kraftState *state, uint8_t *entry,
                                 uint32_t entryLen) {
    /* Extract {old, new} details from RPC.
     * Enter joint consensus mode until {new} committed. */
    D("Starting JOINT CONSENSUS...");
    /* Populate NEW with list of nodes in RPC */

    /* unwrap rpc->entry for new configuration.
     * configuration is transmitted as COMPLETE LIST OF NODES.
     *
     * Take new COMPLETE LIST OF NODES and compare against
     * existing OLD NODES.  Copy existing OLD NODES to
     * NEW NODES then connect to any remaining nodes
     * in our NEW NODES list. */

    uint32_t ipEntryOffset = 0;
    uint32_t ipEntryLen = entryLen;
    uint8_t *ip;
    uint16_t port;

    /* For each [IP][Port] in RPC, establish outbound connection. */
    while ((ipEntryOffset = state->internals.config.netDecode(
                &ip, &port, entry + ipEntryOffset, ipEntryLen))) {
        /* Advance entry position for next read. */
        entry += ipEntryOffset;
        ipEntryLen -= ipEntryOffset;

        kraftIp krIp = {{0}};
        strncpy(krIp.ip, (char *)ip, sizeof(krIp.ip));
        krIp.port = port;
        state->internals.network.connectOutbound(state, &krIp);
    }
}

/* "When the new configuration has been committed under the rules of C{new},
 * the old configuration is irrelevant and servers not in the new configuration
 * can be shut down." */
void kraftRpcJointConsensusFinalize(kraftState *state) {
    D("Ending JOINT CONSENSUS...");

    /* If ourself is *not* in the list of new nodes, we need to stop
     * being a part of this cluster. */
    if (!vecPtrContains(NODES_NEW_NODES(state), state->self)) {
        /* If we are the current leader, stop being a leader. */
        if (state->self == state->leader) {
            BECOME(FOLLOWER, state->self);

            /* Potentially send an RPC to everything in 'NEW' requesting
             * an immediate election. */

            /* stop heartbeats, we aren't a leader anymore */
            state->internals.timer.heartbeatTimer(state, KRAFT_TIMER_STOP);
        } else {
            /* We're not the leader, but we are still removed from new
             * nodes, so we don't want to trigger an election either. */

            /* stop waiting for heartbeats, we know we want an election. */
            state->internals.timer.leaderContactTimer(state, KRAFT_TIMER_STOP);
        }
    }

    /* Now we disconnect every node in OLD that isn't in NEW */
    vecPtrForEach(NODES_OLD_NODES(state), disconnectIfNotNew, state);

    /* Now reset OLD so it can be used as empty */
    vecPtrClear(NODES_OLD_NODES(state));
    vecPtrClear(NODES_OLD_PENDING(state));
    /* TODO: reset reconnect? */
    /* TODO: reset NEW pending? */

    /* Now swap NEW and OLD so the NEW set of nodes becomes baseline config. */
    vecPtr *tmp = NODES_OLD_NODES(state);
    NODES_OLD_NODES(state) = NODES_NEW_NODES(state);
    NODES_NEW_NODES(state) = tmp;

    /* We are done with this joint consensus. */
    state->jointConsensusStartIndex = 0;
    state->jointConsensusEndIndex = 0;
}

/* ====================================================================
 * Process RPC (RPC populated by protocol parsing)
 * ==================================================================== */
bool kraftRpcProcess(kraftState *state, kraftRpc *rpc, kraftRpcStatus *status) {
    /* "If a server receives a request with a stale term number, it rejects the
     * request." */
    /* Modification: we only care about term numbers for APPEND ENTRIES.
     * It's valid for old terms to vote for a node with a newer term. */
    if (rpc->cmd == KRAFT_RPC_APPEND_ENTRIES) {
        assert(rpc->term > 0);

        if (rpc->term < state->self->consensus.term) {
            kraftErrorReply(KRAFT_RPC_STATUS_REJECTED_RPC_TERM_TOO_OLD);
        }
    } else if (rpc->cmd == KRAFT_RPC_REQUEST_VOTE) {
        /* If vote request, process it. */
        return kraftRpcProcessRequestVote(state, rpc, status);
    } else if (rpc->cmd == KRAFT_RPC_REQUEST_PREVOTE) {
        /* If pre-vote request, process it. */
        return kraftRpcProcessRequestPreVote(state, rpc, status);
    } else if (rpc->cmd == KRAFT_RPC_TIMEOUT_NOW) {
        /* TimeoutNow RPC for leadership transfer.
         * Immediately triggers an election regardless of timer state.
         * This enables zero-downtime leadership handoff. */
        if (!state->config.enableLeadershipTransfer) {
            D("Ignoring TimeoutNow: leadership transfer disabled");
            return false;
        }

        if (!IS_FOLLOWER(state->self)) {
            D("Ignoring TimeoutNow: not a follower (role=%s)",
              kraftRoles[state->self->consensus.role]);
            return false;
        }

        D("Received TimeoutNow RPC - immediately starting election");

        /* CRITICAL: TimeoutNow MUST bypass Pre-Vote and go directly to real
         * election. Per Raft thesis §3.10 (Leadership Transfer Extension):
         * - The leader has already verified target is caught up
         * - This is an intentional coordinated transfer, not a partition
         * scenario
         * - Pre-Vote would fail because old leader is still active
         * Always use kraftRpcRequestVote() regardless of enablePreVote config.
         */
        kraftRpcRequestVote(state);

        return true;
    }

    /* "If RPC request or response contains term T > currentTerm: set
     * currentTerm = T, convert to follower (§5.1)" */
    if (rpc->term > state->self->consensus.term) {
        D("Term higher than our own detected! Installing new term %" PRIu64
          " instead of old term %" PRIu64,
          rpc->term, state->self->consensus.term);
        state->self->consensus.term = rpc->term;
        /* CRITICAL: Reset votedFor when entering new term */
        KRAFT_VOTED_FOR(state) = 0;
        if (!IS_FOLLOWER(state->self)) {
            BECOME(FOLLOWER, state->self);
        }
    }

    /* TODO: fix condition. */
    /* Only our leader can send current term entries, so reset our
     * contact/election timer. */
    if (!IS_LEADER(state->self) && rpc->term == state->self->consensus.term) {
        state->internals.timer.leaderContactTimer(state, KRAFT_TIMER_AGAIN);
        /* Track when we last heard from the leader (for pre-vote decisions) */
        if (state->leader) {
            state->leader->lastContact = kraftmstime();
        }
    }

    D("Processing %s as %s", kraftRpcCmds[rpc->cmd],
      kraftRoles[state->self->consensus.role]);
    switch (state->self->consensus.role) {
    case KRAFT_FOLLOWER:
        return kraftRpcProcessFollower(state, rpc, status);
    case KRAFT_PRE_CANDIDATE:
        /* PRE_CANDIDATE uses same processing as CANDIDATE */
        return kraftRpcProcessCandidate(state, rpc, status);
    case KRAFT_CANDIDATE:
        return kraftRpcProcessCandidate(state, rpc, status);
    case KRAFT_LEADER:
        return kraftRpcProcessLeader(state, rpc, status);
    default:
        assert(NULL);
    }
}

/* ====================================================================
 * Appending to persistent log
 * ==================================================================== */
bool kraftLogAppend(kraftState *state, kraftRpcUniqueIndex index,
                    kraftTerm term, kraftStorageCmd cmd, const void *data,
                    uint64_t len) {
    D("APPENDING TO LOG WITH INDEX, TERM, DATA, LEN, CMD: %" PRIu64 ", %" PRIu64
      ", %p, %" PRIu64 ", (%d)",
      index, term, data, len, cmd);
    const bool inserted =
        kvidxInsert(KRAFT_ENTRIES(state), index, term, cmd, data, len);
    /* Update currentIndex, currentTerm, and previousEntry for vote comparison
     */
    kraftStateSetPreviousIndexTerm(state, index, term);
    return inserted;
}

/* ====================================================================
 * Snapshot and log compaction
 * ==================================================================== */

/* Create a snapshot at the current commit index.
 * This captures the state machine state and allows discarding old log entries.
 */
bool kraftSnapshotCreate(kraftState *state) {
    if (state->commitIndex == 0) {
        return false; /* Nothing to snapshot yet */
    }

    /* Get the term of the last committed entry */
    kraftTerm lastTerm = 0;
    const uint8_t *dummy;
    size_t dummyLen;
    uint64_t dummyCmd;
    bool found = kvidxGet(KRAFT_ENTRIES(state), state->commitIndex, &lastTerm,
                          &dummyCmd, &dummy, &dummyLen);

    if (!found) {
        D("WARNING: Could not get term for commit index %" PRIu64,
          state->commitIndex);
        return false;
    }

    D("Creating snapshot at index %" PRIu64 " term %" PRIu64,
      state->commitIndex, lastTerm);

    /* Update snapshot metadata */
    state->snapshot.lastIncludedIndex = state->commitIndex;
    state->snapshot.lastIncludedTerm = lastTerm;

    /* Update lastApplied - all entries up to snapshot are already applied */
    if (state->snapshot.lastIncludedIndex > state->lastApplied) {
        state->lastApplied = state->snapshot.lastIncludedIndex;
    }

    /* In a full implementation, we would:
     * 1. Call state machine to serialize its state
     * 2. Write snapshot data to disk
     * 3. Persist snapshot metadata
     * For now, we just update the metadata which allows log compaction. */

    /* Compact the log by removing entries before the snapshot */
    kraftLogCompact(state, state->snapshot.lastIncludedIndex);

    return true;
}

/* Install a snapshot received from the leader.
 * This replaces the state machine state and discards conflicting log entries.
 */
bool kraftSnapshotInstall(kraftState *state,
                          kraftRpcUniqueIndex lastIncludedIndex,
                          kraftTerm lastIncludedTerm) {
    D("Installing snapshot at index %" PRIu64 " term %" PRIu64,
      lastIncludedIndex, lastIncludedTerm);

    /* Update snapshot metadata */
    state->snapshot.lastIncludedIndex = lastIncludedIndex;
    state->snapshot.lastIncludedTerm = lastIncludedTerm;

    /* Update commit and apply indices - snapshot is already applied state */
    if (lastIncludedIndex > state->commitIndex) {
        state->commitIndex = lastIncludedIndex;
    }
    if (lastIncludedIndex > state->lastApplied) {
        state->lastApplied = lastIncludedIndex;
    }

    /* Discard log entries covered by snapshot */
    kraftLogCompact(state, lastIncludedIndex);

    /* In a full implementation, we would:
     * 1. Receive and write snapshot data to disk
     * 2. Reset state machine with snapshot data
     * 3. Persist snapshot metadata
     * For now, we just update metadata and compact the log. */

    return true;
}

/* Compact the log by removing entries up to (but not including) upToIndex.
 * Entries from 1 to (upToIndex-1) are removed. Entry at upToIndex is kept. */
void kraftLogCompact(kraftState *state, kraftRpcUniqueIndex upToIndex) {
    if (upToIndex == 0) {
        return; /* Nothing to compact */
    }

    D("Compacting log up to index %" PRIu64, upToIndex);

    /* Remove log entries from index 1 to (upToIndex - 1)
     * The kvidx storage system should support range deletion.
     * For now, we delete entries one by one. */
    for (kraftRpcUniqueIndex i = 1; i < upToIndex; i++) {
        /* Note: kvidx may not have a delete API yet.
         * In that case, this is a no-op and log entries remain in storage.
         * A full implementation would need kvidx to support:
         *   kvidxDeleteRange(KRAFT_ENTRIES(state), 1, upToIndex - 1);
         * For now, we just document the intent. */
        D("Would delete log entry at index %" PRIu64, i);
    }

    /* The important part is that we've updated snapshot metadata,
     * so we know which entries are included in the snapshot.
     * Even if we can't physically delete them yet, we won't send
     * them to followers - we'll send the snapshot instead. */
}

/* Check if we should create a snapshot based on log size */
void kraftSnapshotCheckAndTrigger(kraftState *state) {
    /* Only leaders should trigger snapshots.
     * Followers will receive snapshots via InstallSnapshot RPC. */
    if (!IS_LEADER(state->self)) {
        return;
    }

    /* Check if log has grown beyond threshold */
    kraftRpcUniqueIndex maxIndex = 0;
    if (!kvidxMaxKey(KRAFT_ENTRIES(state), &maxIndex)) {
        return; /* No entries yet */
    }

    kraftRpcUniqueIndex logSize = maxIndex - state->snapshot.lastIncludedIndex;

    if (logSize > state->snapshot.triggerThreshold) {
        D("Log size %" PRIu64 " exceeds threshold %" PRIu64
          ", creating snapshot",
          logSize, state->snapshot.triggerThreshold);
        kraftSnapshotCreate(state);
    }
}
