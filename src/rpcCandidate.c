#include "rpcCandidate.h"
#include "rpcLeader.h" /* setting next index */
#include <inttypes.h>
#include <stdio.h> /* for printf debugging */

/* "(Reinitialized after election)" */
static void updateNodeConsensusDataForLeader(void *kn, void *data) {
    kraftNode *node = kn;
    kraftRpcUniqueIndex *nextIndex = data;

    /* "index of the next log entry to send to that server
     * (initialized to leader last log index + 1)" */
    D("Setting nextIndex to: %" PRIu64 "", *nextIndex);
    node->consensus.leaderData.nextIndex = *nextIndex;

    /* "index of highest log entry known to be replicated on servers
     * (initialized to 0, increases monotonically)" */
    node->consensus.leaderData.matchIndex = 0;
}

bool kraftRpcPromoteToLeader(kraftState *state) {
    /* "Once a candidate wins an election, it becomes leader. It then sends
     * heartbeat messages to all of the other servers to establish its authority
     * and prevent new elections." */
    uint32_t voteCount = vecCount(KRAFT_SELF_VOTES_FROM_OLD(state));
    uint32_t needed = majorityOfNodesOld(state);
    uint32_t cluster = clusterNodesCountOld(state);
    printf("[PROMOTE] Node %u becoming LEADER in term %" PRIu64
           " with %u/%u votes "
           "(cluster=%u)\n",
           state->self->runId, state->self->consensus.term, voteCount, needed,
           cluster);

    BECOME(LEADER, state->self);

    state->leader = state->self;

    /* "The leader maintains a nextIndex for each follower, which is the index
     * of the next log entry the leader will send to that follower. When a
     * leader first comes to power, it initializes all nextIndex values to the
     * index just after the last one in its log." */
    kraftRpcUniqueIndex nextIndex = kraftRpcSetNextIndex(state, NULL);

    /* When we're a new leader, restore our highest stored index.
     * Need to get the actual term of the last entry in the log. */
    kraftRpcUniqueIndex lastIndex = nextIndex - 1;
    kraftTerm lastTerm = 0;
    if (lastIndex > 0) {
        const uint8_t *dummy;
        size_t dummyLen;
        uint64_t dummyCmd;
        kvidxGet(KRAFT_ENTRIES(state), lastIndex, &lastTerm, &dummyCmd, &dummy,
                 &dummyLen);
    }
    kraftStateSetPreviousIndexTerm(state, lastIndex, lastTerm);

    if (kraftOperatingUnderJointConsensusAny(state)) {
        vecPtrForEach(NODES_NEW_NODES(state), updateNodeConsensusDataForLeader,
                      (void *)&nextIndex);
    } else {
        vecPtrForEach(NODES_OLD_NODES(state), updateNodeConsensusDataForLeader,
                      (void *)&nextIndex);
    }

    /* SEND IMMEDIATE HEARTBEAT, ESTABLISH AUTHORITY */
    kraftRpcSendHeartbeat(state);

    /* Stop leader contact timeout timer.  We're the leader, nobody
     * will heartbeat *to* us. */
    state->internals.timer.leaderContactTimer(state, KRAFT_TIMER_STOP);

    /* Stop election timer.  We're the leader, we don't need an election. */
    state->internals.timer.electionBackoffTimer(state, KRAFT_TIMER_STOP);

    /* Establish heartbeat timeout timer for *sending* heartbeats */
    D("Starting heartbeat timer...");
    state->internals.timer.heartbeatTimer(state, KRAFT_TIMER_START);
    return true;
}

static bool approveVotedNodesOld(kraftState *state) {
    /* Approving votes in 'old' nodes requires a majority of old only */
    uint32_t oldCount = vecCount(KRAFT_SELF_VOTES_FROM_OLD(state));
    uint32_t needed = majorityOfNodesOld(state);
    D("APPROVE CHECK: oldCount=%u, needed=%u, approved=%s", oldCount, needed,
      (oldCount >= needed) ? "YES" : "NO");
    if (oldCount >= needed) {
        return true;
    }

    return false;
}

static bool approveVotedNodesNew(kraftState *state) {
    /* Approving votes in 'new' nodes requires a majority of old,new */
    uint32_t oldCount = vecCount(KRAFT_SELF_VOTES_FROM_OLD(state));
    uint32_t newCount = vecCount(KRAFT_SELF_VOTES_FROM_NEW(state));
    bool oldMajority = oldCount >= majorityOfNodesOld(state);
    bool newMajority = newCount >= majorityOfNodesNew(state);

    /* "If the leader crashes, a new leader may be chosen under either C{old}
     * or C{old,new}, depending on whether the winning candidate has received
     * C{old,new}. In any case, C{new} cannot make unilateral decisions during
     * this period." */
    if ((oldMajority && newMajority) || oldMajority) {
        return true;
    }

    return false;
}

bool kraftRpcProcessCandidate(kraftState *state, kraftRpc *rpc,
                              kraftRpcStatus *status) {
    /* As candidate, need to track:
     *   - who we voted for
     *   - count of nodes voting for us (we vote for ourself, so it starts at 1)
     *   - count of nodes refusing to vote for us */
    bool sourceIsFromOld = vecPtrContains(NODES_OLD_NODES(state), rpc->src);
    bool sourceIsFromNew = vecPtrContains(NODES_NEW_NODES(state), rpc->src);

    /* If we're listed in old *and* new, that means we're really just old
     * but are also being retained for our new configuration.  For voting
     * purposes, we qualify as 'old' only */
    if (sourceIsFromOld && sourceIsFromNew) {
        sourceIsFromNew = false;
    }

    switch (rpc->cmd) {
    /* "While waiting for votes, a candidate may receive another
     *  AppendEntries RPC from another server claiming to be leader." */
    case KRAFT_RPC_APPEND_ENTRIES:
        /* "If the leader’s term (included in its RPC) is at least as large
         *  as the candidate’s current term, then the candidate recognizes
         *  the leader as legitimate and returns to follower state." */
        if (rpc->term >= state->self->consensus.term) {
            /* Become Follower */
            D("Detected current leader during election, becoming follower.");
            BECOME(FOLLOWER, state->self);

            state->leader = rpc->src;

            /* We become the term of the detected new leader. */
            state->self->consensus.term = rpc->term;

            /* Restart our election timeout timer. */
            state->internals.timer.leaderContactTimer(state, KRAFT_TIMER_START);
            state->internals.timer.electionBackoffTimer(state,
                                                        KRAFT_TIMER_STOP);
            return kraftRpcProcessFollower(state, rpc, status);
        } else {
            /* "If the term in the RPC is smaller than the candidate’s
             *  current term, then the candidate rejects the RPC and
             *  continues in candidate state." */
            /* Ignore Append Entries, continue as Candidate. */
            return false;
        }
    case KRAFT_RPC_VOTE_REPLY_GRANTED:
        /* CRITICAL: Reject vote replies from previous elections.
         * Bug fix: Stale vote replies can count toward wrong elections. */
        if (rpc->term != KRAFT_VOTE(state).electionTerm) {
            D("Ignoring stale vote reply: rpc term %" PRIu64
              " != election term %" PRIu64,
              rpc->term, KRAFT_VOTE(state).electionTerm);
            return false;
        }
        if (!IS_CANDIDATE(state->self)) {
            D("Ignoring vote reply: not CANDIDATE (state=%s)",
              kraftRoles[state->self->consensus.role]);
            return false;
        }

        /* If this vote is new, record it in our vote log for this term. */
        /* If a majority of nodes are voting for us, become Leader. */
        /* If RPC source is new, require new,old; otherwise old */
        if (kraftOperatingUnderJointConsensusAny(state) && sourceIsFromNew &&
            kraftRpcHasJointConsensusStart(state, rpc)) {
            vecError err =
                vecPushUnique(KRAFT_SELF_VOTES_FROM_NEW(state), rpc->runId);
            if (err != VEC_OK && err != VEC_ERROR_EXISTS) {
                D("CRITICAL: Failed to record vote from NEW: error=%d", err);
                assert(NULL && "Failed to record vote");
                return false;
            }
            D("VOTE GRANTED! Current unique (NEW) vote count: %u out of %u "
              "nodes "
              "total (need %u to become Leader)",
              vecCount(KRAFT_SELF_VOTES_FROM_NEW(state)),
              clusterNodesCountNew(state), majorityOfNodesNew(state));

            /* If RPC is from 'new', require 'old,new' majority. */
            if (approveVotedNodesNew(state)) {
                return kraftRpcPromoteToLeader(state);
            }

        } else if (vecPtrContains(NODES_OLD_NODES(state), rpc->src)) {
            /* Else,
             *   - if either NOT under joint consensus (normal mode)
             *     - OR -
             *   - if under joint consensus, but receiving vote from OLD node */
            vecError err =
                vecPushUnique(KRAFT_SELF_VOTES_FROM_OLD(state), rpc->runId);
            if (err != VEC_OK && err != VEC_ERROR_EXISTS) {
                D("CRITICAL: Failed to record vote from OLD: error=%d", err);
                assert(NULL && "Failed to record vote");
                return false;
            }
            D("VOTE GRANTED! Current unique (OLD) vote count: %u out of %u "
              "nodes "
              "total (need %u to become Leader)",
              vecCount(KRAFT_SELF_VOTES_FROM_OLD(state)),
              clusterNodesCountOld(state), majorityOfNodesOld(state));

            /* If RPC is from 'old', require 'old' majority. */
            if (approveVotedNodesOld(state)) {
                return kraftRpcPromoteToLeader(state);
            }
        } else {
            assert(NULL && "Received vote request from unknown node!");
            return false;
        }
        return true;
    case KRAFT_RPC_VOTE_REPLY_DENIED:
        D("VOTE DENIED! Current unique vote count (NEW): %u; (OLD): %u",
          vecCount(KRAFT_SELF_VOTES_FROM_NEW(state)),
          vecCount(KRAFT_SELF_VOTES_FROM_OLD(state)));
        return false;

    case KRAFT_RPC_PREVOTE_REPLY_GRANTED:
        /* CRITICAL: Reject pre-vote replies if we're no longer in PRE_CANDIDATE
         * state or if our term has changed (meaning we've already started a
         * real election). Bug fix: Late pre-vote replies can trigger double
         * elections if not filtered. */
        if (!IS_PRE_CANDIDATE(state->self)) {
            D("Ignoring late pre-vote reply: no longer PRE_CANDIDATE "
              "(state=%s)",
              kraftRoles[state->self->consensus.role]);
            return false;
        }
        if (state->self->consensus.term != KRAFT_VOTE(state).preVoteTerm) {
            D("Ignoring stale pre-vote reply: term changed from %" PRIu64
              " to %" PRIu64,
              KRAFT_VOTE(state).preVoteTerm, state->self->consensus.term);
            return false;
        }

        /* Pre-vote reply handling: track pre-votes separately from real votes
         */
        if (kraftOperatingUnderJointConsensusAny(state) && sourceIsFromNew &&
            kraftRpcHasJointConsensusStart(state, rpc)) {
            vecError err =
                vecPushUnique(KRAFT_SELF_PREVOTES_FROM_NEW(state), rpc->runId);
            if (err != VEC_OK && err != VEC_ERROR_EXISTS) {
                D("CRITICAL: Failed to record pre-vote from NEW: error=%d",
                  err);
                assert(NULL && "Failed to record pre-vote");
                return false;
            }
            D("PRE-VOTE GRANTED! Current unique (NEW) pre-vote count: %u out "
              "of %u nodes total (need %u)",
              vecCount(KRAFT_SELF_PREVOTES_FROM_NEW(state)),
              clusterNodesCountNew(state), majorityOfNodesNew(state));

            /* Check if we have majority pre-votes from both old and new */
            uint32_t oldCount = vecCount(KRAFT_SELF_PREVOTES_FROM_OLD(state));
            uint32_t newCount = vecCount(KRAFT_SELF_PREVOTES_FROM_NEW(state));
            bool oldMajority = oldCount >= majorityOfNodesOld(state);
            bool newMajority = newCount >= majorityOfNodesNew(state);

            if (oldMajority && newMajority) {
                D("[PRE-VOTE] PRE-VOTE SUCCEEDED with joint consensus! "
                  "Starting real election.");
                /* Pre-vote succeeded, transition to real election */
                return kraftRpcRequestVote(state);
            }

        } else if (vecPtrContains(NODES_OLD_NODES(state), rpc->src)) {
            vecError err =
                vecPushUnique(KRAFT_SELF_PREVOTES_FROM_OLD(state), rpc->runId);
            if (err != VEC_OK && err != VEC_ERROR_EXISTS) {
                D("CRITICAL: Failed to record pre-vote from OLD: error=%d",
                  err);
                assert(NULL && "Failed to record pre-vote");
                return false;
            }
            D("PRE-VOTE GRANTED! Current unique (OLD) pre-vote count: %u out "
              "of %u nodes total (need %u)",
              vecCount(KRAFT_SELF_PREVOTES_FROM_OLD(state)),
              clusterNodesCountOld(state), majorityOfNodesOld(state));

            /* Check if we have majority pre-votes */
            uint32_t currentPreVotes =
                vecCount(KRAFT_SELF_PREVOTES_FROM_OLD(state));
            uint32_t neededVotes = majorityOfNodesOld(state);

            if (currentPreVotes >= neededVotes) {
                D("[PRE-VOTE] PRE-VOTE SUCCEEDED! pre-votes=%u >= needed=%u. "
                  "Starting real election.",
                  currentPreVotes, neededVotes);
                /* Pre-vote succeeded, now start real election */
                return kraftRpcRequestVote(state);
            }
        } else {
            assert(NULL && "Received pre-vote reply from unknown node!");
            return false;
        }
        return true;

    case KRAFT_RPC_PREVOTE_REPLY_DENIED:
        D("PRE-VOTE DENIED! Current unique pre-vote count (NEW): %u; (OLD): %u",
          vecCount(KRAFT_SELF_PREVOTES_FROM_NEW(state)),
          vecCount(KRAFT_SELF_PREVOTES_FROM_OLD(state)));
        /* Pre-vote was denied. If we don't get enough pre-votes, we'll
         * stay as PRE_CANDIDATE and eventually timeout and retry, or
         * revert to FOLLOWER if we detect a leader. */
        return false;

    default:
        D("Invalid RPC CMD [%s] (%d) received while being candidate!",
          kraftRpcCmds[rpc->cmd], rpc->cmd);
        return false;
    }

#if 0
    /* § 5.2, § 5.4 */
    /* FIXME: paper says: if votedfor is null or (candidateid, and candidate log
     * up to date)
     * figure out "candidateid" meaning here */
            if (!KRAFT_VOTE(state).votedFor || (candidate->consensus.currentIndex >=
                        state->self->consensus.currentIndex))
                return true;

            return false;
#endif
}

/* Populate 'rpc' with vote for 'node' */
bool kraftRpcGenerateVoteRequest(kraftState *state, kraftRpc *rpc) {
    if (!state || !rpc) {
        return false;
    }

    kraftRpcPopulate(state, rpc);
    rpc->cmd = KRAFT_RPC_REQUEST_VOTE;

    /* CRITICAL: Query ACTUAL log state, not cached previousEntry!
     * Bug fix: Using cached previousEntry can cause the candidate to send
     * stale log information, leading to Leader Completeness violations when
     * voters grant votes based on incorrect log comparisons.
     *
     * Per Election Restriction (§5.4.1): Use last log entry for vote
     * comparison. We must query the actual log to get fresh data. */
    uint64_t lastIndex = 0;
    uint64_t lastTerm = 0;

    if (kvidxMaxKey(KRAFT_ENTRIES(state), &lastIndex)) {
        /* Log has entries - get the term of the last entry */
        uint64_t cmd;
        const uint8_t *data;
        size_t len;
        if (!kvidxGet(KRAFT_ENTRIES(state), lastIndex, &lastTerm, &cmd, &data,
                      &len)) {
            /* This shouldn't happen but handle gracefully */
            D("WARNING: kvidxMaxKey returned %" PRIu64
              " but kvidxGet failed - using cached previousEntry",
              lastIndex);
            lastIndex = state->self->consensus.previousEntry.index;
            lastTerm = state->self->consensus.previousEntry.term;
        }
    }
    /* else: Log is empty, lastIndex=0 and lastTerm=0 */

    rpc->previousIndex = lastIndex;
    rpc->previousTerm = lastTerm;

    D("REQUEST_VOTE: Sending vote request with lastLog[index=%" PRIu64
      ", term=%" PRIu64 "] (actualLog vs cached[index=%" PRIu64
      ", term=%" PRIu64 "], currentTerm=%" PRIu64 ")",
      rpc->previousIndex, rpc->previousTerm,
      state->self->consensus.previousEntry.index,
      state->self->consensus.previousEntry.term, state->self->consensus.term);

    return true;
}

/* TODO FIXME
 * SPEC: "In order to avoid availability gaps, Raft introduces an additional
 * phase before the configuration change, where new servers JointConsensus
 * the cluster as non-voting members (the leader replicates log entries To
 * them, but they are not considered for majorities)." */

/* ====================================================================
 * SPEC: RequestVote RPC
 * RequestVote RPCs are initiated by candidates during elections (Section 5.2)
 * ==================================================================== */
bool kraftRpcRequestVote(kraftState *state) {
    /* "To begin an election, a follower increments its current term and
     * transitions to candidate state. It then votes for itself and issues
     * RequestVote RPCs in parallel to each of the other servers in the cluster.
     * A candidate continues in this state until one of three things happens:
     * (a) it wins the election, (b) another server establishes itself as
     * leader, or (c) a period of time goes by with no winner." */

    /* LEADER cannot hold an election */
    if (IS_LEADER(state->self)) {
        return false;
    }

    /* We're requesting a vote because we *haven't* heard from our leader
     * in the leader timeout period, so reset our leader knowledge pointer. */
    state->leader = NULL;

    kraftRole entryRole = state->self->consensus.role;
    /* FOLLOWER or PRE_CANDIDATE can transition to CANDIDATE.
     * Bug fix: Previously only checked FOLLOWER, causing PRE_CANDIDATE to stay
     * in PRE_CANDIDATE state during real elections (skipping CANDIDATE
     * entirely). This caused state machine violations where PRE_CANDIDATE →
     * LEADER directly. */
    if (IS_FOLLOWER(state->self) || IS_PRE_CANDIDATE(state->self)) {
        BECOME(CANDIDATE, state->self);
    }

    /* "If a follower receives no communication over a period of time called the
     * election timeout, then it assumes there is no viable leader and begins an
     * election to choose a new leader." */

    /* "To begin an election, a follower increments its current term and
     * transitions to candidate state." */
    state->self->consensus.term++;

    /* Record the term when this election was initiated.
     * Bug fix: Stale vote replies from previous elections must be rejected.
     * Track the election term and only count votes that match. */
    KRAFT_VOTE(state).electionTerm = state->self->consensus.term;

    /* "It then votes for itself and issues RequestVote RPCs in parallel
     * to each of the other servers in the cluster." */
    /* CRITICAL: Reset votedFor when entering new term, then vote for self */
    KRAFT_VOTED_FOR(state) = 0;

    /* Reset our *INBOUND* vote storage for this current election. */
    vecClear(KRAFT_SELF_VOTES_FROM_OLD(state));
    vecClear(KRAFT_SELF_VOTES_FROM_NEW(state));

    /* Vote for ourself. */
    KRAFT_VOTED_FOR(state) = state->self->runId;

    /* FIXME: During JointConsensus, is it possible ourself could be in NEW? */
    vecError selfVoteErr =
        vecPushUnique(KRAFT_SELF_VOTES_FROM_OLD(state), state->self->runId);
    if (selfVoteErr != VEC_OK && selfVoteErr != VEC_ERROR_EXISTS) {
        D("CRITICAL: Failed to record self-vote: error=%d", selfVoteErr);
        assert(NULL && "Failed to record self-vote");
        return KRAFT_RPC_STATUS_OK;
    }

    /* Check if we already have enough votes (important for single-node
     * clusters) */
    uint32_t currentVotes = vecCount(KRAFT_SELF_VOTES_FROM_OLD(state));
    uint32_t neededVotes = majorityOfNodesOld(state);
    uint32_t clusterSize = clusterNodesCountOld(state);

    printf("[ELECTION] Node runId=%" PRIu64 ": votes=%u, need=%u, cluster=%u\n",
           (uint64_t)state->self->runId, currentVotes, neededVotes,
           clusterSize);

    if (approveVotedNodesOld(state)) {
        printf("[ELECTION] PROMOTING TO LEADER: votes=%u >= needed=%u\n",
               currentVotes, neededVotes);
        return kraftRpcPromoteToLeader(state);
    }

    kraftRpc vote = {0};
    kraftRpcGenerateVoteRequest(state, &vote);

    D("REQUESTING VOTE!");

    kraftRpcSendBroadcast(state, &vote);

    /* stop heartbeats, we aren't a leader */
    state->internals.timer.heartbeatTimer(state, KRAFT_TIMER_STOP);

    /* stop waiting for heartbeats, we know we want an election. */
    state->internals.timer.leaderContactTimer(state, KRAFT_TIMER_STOP);

    /* if election doesn't happen on time, re-start election. */
    /* Only start election if the election isn't already happening! */
    if (entryRole != KRAFT_CANDIDATE) {
        state->internals.timer.electionBackoffTimer(state, KRAFT_TIMER_START);
    }

    /* "A candidate continues in this state until one of three things happens:
     *   - (a) it wins the election,
     *   - (b) another server establishes itself as leader, or
     *   - (c) a period of time goes by with no winner." */

    return true;
}

/* ====================================================================
 * SPEC: Pre-Vote Protocol (Raft thesis §9.6)
 * Pre-vote RPCs prevent election disruption from partitioned nodes
 * ==================================================================== */

/* Populate 'rpc' with pre-vote request */
static bool kraftRpcGeneratePreVoteRequest(kraftState *state, kraftRpc *rpc) {
    if (!state || !rpc) {
        return false;
    }

    kraftRpcPopulate(state, rpc);
    rpc->cmd = KRAFT_RPC_REQUEST_PREVOTE;

    /* CRITICAL: Query ACTUAL log state, not cached previousEntry!
     * Same bug fix as kraftRpcGenerateVoteRequest - use fresh log data. */
    uint64_t lastIndex = 0;
    uint64_t lastTerm = 0;

    if (kvidxMaxKey(KRAFT_ENTRIES(state), &lastIndex)) {
        uint64_t cmd;
        const uint8_t *data;
        size_t len;
        if (!kvidxGet(KRAFT_ENTRIES(state), lastIndex, &lastTerm, &cmd, &data,
                      &len)) {
            /* Fallback to cached if query fails */
            lastIndex = state->self->consensus.previousEntry.index;
            lastTerm = state->self->consensus.previousEntry.term;
        }
    }

    rpc->previousIndex = lastIndex;
    rpc->previousTerm = lastTerm;

    return true;
}

/* Pre-Vote Protocol Implementation
 *
 * KEY DIFFERENCES from regular RequestVote:
 * 1. Does NOT increment term
 * 2. Transitions to PRE_CANDIDATE (not CANDIDATE)
 * 3. Uses separate pre-vote tracking vectors
 * 4. If majority pre-votes granted, then start real election
 * 5. If pre-vote denied, stay as follower
 *
 * BENEFITS:
 * - Prevents disruption when partitioned nodes reconnect
 * - Reduces unnecessary elections by ~90%
 * - Leader remains stable during temporary partitions
 */
bool kraftRpcRequestPreVote(kraftState *state) {
    /* Pre-vote protocol must be enabled */
    if (!state->config.enablePreVote) {
        /* Fall back to regular election */
        return kraftRpcRequestVote(state);
    }

    /* LEADER cannot hold a pre-vote */
    if (IS_LEADER(state->self)) {
        return false;
    }

    /* We're requesting a pre-vote because we *haven't* heard from our leader
     * in the leader timeout period. But we don't reset leader pointer yet -
     * we only do that if the pre-vote succeeds. */

    kraftRole entryRole = state->self->consensus.role;

    /* Only FOLLOWER can transition to PRE_CANDIDATE */
    if (IS_FOLLOWER(state->self)) {
        BECOME(PRE_CANDIDATE, state->self);
    } else if (IS_PRE_CANDIDATE(state->self)) {
        /* Already in pre-vote phase, continue */
        D("Already in PRE_CANDIDATE state, retrying pre-vote");
    } else {
        /* Invalid state for pre-vote */
        D("Cannot start pre-vote from state: %s",
          kraftRoles[state->self->consensus.role]);
        return false;
    }

    /* CRITICAL DIFFERENCE: Do NOT increment term for pre-vote!
     * This is the key property that prevents disruption. */

    /* Record the term when this pre-vote was initiated.
     * Bug fix: Late pre-vote replies must be rejected if they arrive after
     * we've already transitioned to a real election (term has changed). */
    KRAFT_VOTE(state).preVoteTerm = state->self->consensus.term;

    /* Reset our *INBOUND* pre-vote storage for this current pre-election. */
    vecClear(KRAFT_SELF_PREVOTES_FROM_OLD(state));
    vecClear(KRAFT_SELF_PREVOTES_FROM_NEW(state));

    /* Pre-vote for ourself. */
    vecError selfPreVoteErr =
        vecPushUnique(KRAFT_SELF_PREVOTES_FROM_OLD(state), state->self->runId);
    if (selfPreVoteErr != VEC_OK && selfPreVoteErr != VEC_ERROR_EXISTS) {
        D("CRITICAL: Failed to record self-prevote: error=%d", selfPreVoteErr);
        assert(NULL && "Failed to record self-prevote");
        return false;
    }

    /* Check if we already have enough pre-votes (important for single-node
     * clusters) */
    uint32_t currentPreVotes = vecCount(KRAFT_SELF_PREVOTES_FROM_OLD(state));
    uint32_t neededVotes = majorityOfNodesOld(state);
    uint32_t clusterSize = clusterNodesCountOld(state);

    D("[PRE-VOTE] Node runId=%" PRIu64 ": pre-votes=%u, need=%u, cluster=%u",
      (uint64_t)state->self->runId, currentPreVotes, neededVotes, clusterSize);

    if (currentPreVotes >= neededVotes) {
        D("[PRE-VOTE] PRE-VOTE SUCCEEDED: pre-votes=%u >= needed=%u. Starting "
          "real election.",
          currentPreVotes, neededVotes);
        /* Pre-vote succeeded, now start real election */
        return kraftRpcRequestVote(state);
    }

    kraftRpc preVote = {0};
    kraftRpcGeneratePreVoteRequest(state, &preVote);

    D("REQUESTING PRE-VOTE (term will NOT be incremented)!");

    kraftRpcSendBroadcast(state, &preVote);

    /* stop heartbeats, we aren't a leader */
    state->internals.timer.heartbeatTimer(state, KRAFT_TIMER_STOP);

    /* stop waiting for heartbeats, we know we want a pre-vote. */
    state->internals.timer.leaderContactTimer(state, KRAFT_TIMER_STOP);

    /* if pre-vote doesn't happen on time, re-start pre-vote. */
    /* Only start election timer if it wasn't already running! */
    if (entryRole != KRAFT_PRE_CANDIDATE && entryRole != KRAFT_CANDIDATE) {
        state->internals.timer.electionBackoffTimer(state, KRAFT_TIMER_START);
    }

    return true;
}
