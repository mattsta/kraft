#include "rpcFollower.h"
#include "rpc.h"
#include <stdio.h>

static bool processAppendEntries(kraftState *state, kraftRpc *rpc,
                                 kraftRpcStatus *status) {
    kraftTerm entryTerm = 0;
    kraftRpcUniqueIndex entryIndex = 0;
    uint8_t *entry = NULL;
    size_t entryLen = 0;
    bool first = true;
    kraftStorageCmd originalCmd = 0;
    /* Iterate over every entry in this AppendEntries RPC */

    /* Start a transaction so our DB knows we only need one fsync
     * if we are processing multiple entries below. */

    D("Processing APPEND_ENTRIES!\n");
    /* Reset resume pointer to start of flex for decoding */
    if (rpc->entry) {
        rpc->resume = flexHead(rpc->entry);
    }
    kvidxBegin(KRAFT_ENTRIES(state));
    while (state->internals.entry.decode(rpc, &entryIndex, &entryTerm,
                                         &originalCmd, &entry, &entryLen)) {
        D("Decoded entry (%p, %zu) index at term: (%" PRIu64 ", %" PRIu64 ")",
          (void *)entry, entryLen, entryIndex, entryTerm);
        /* NOTE! If this is a management command (originalCmd == joint
         * consensus) we must *run* the entry instead AND
         * still append to local log. */
        if (first) {
            if ((entryIndex - 1) != rpc->previousIndex) {
                /* The first entry isn't immediately after previousIndex.
                 * This means either:
                 * 1. Leader's log has gaps (shouldn't happen, but handle
                 * gracefully)
                 * 2. We're missing entries between previousIndex and entryIndex
                 *
                 * In either case, reply with MISMATCH so the leader retries
                 * with earlier entries. This is safer than asserting.
                 *
                 * Example: previousIndex=28, first entry at 30 means we need 29
                 * first. We request the leader try again at previousIndex-1. */
                D("Gap detected: first entry index %" PRIu64
                  " but previousIndex=%" PRIu64
                  " (expected previousIndex=%" PRIu64 ")",
                  entryIndex, rpc->previousIndex, entryIndex - 1);
                kvidxCommit(KRAFT_ENTRIES(state));
                kraftErrorReply(KRAFT_RPC_STATUS_REJECTED_IDX_TERM_MISMATCH);
                return false;
            }

            first = false;
        }

        /* Check to see if the index we're about to populate already exists. */
        bool indexExists = kvidxExists(KRAFT_ENTRIES(state), entryIndex);

        /* If the index *does* exist, we need to check if it has the same term
         * we're trying to populate. */
        if (indexExists) {
            bool indexTermMatch =
                kvidxExistsDual(KRAFT_ENTRIES(state), entryIndex, entryTerm);
            /* If the index exists, but the term doesn't match our data, we need
             * to delete the existing entry. */
            if (!indexTermMatch) {
                /* CRITICAL: Never truncate committed entries!
                 * Committed entries have been replicated to a majority and are
                 * permanent. If a leader sends conflicting data for a committed
                 * index, something is wrong (bug or Byzantine behavior).
                 *
                 * FIX: We must REJECT THE ENTIRE RPC, not just skip this entry!
                 * The old code used "continue" which would skip the conflicting
                 * entry but continue appending subsequent entries. This created
                 * an INVALID log state where terms were not monotonically
                 * non-decreasing (e.g., entry 5 at term 5, entry 6 at term 2).
                 * This invalid state then caused Leader Completeness violations
                 * because the Election Restriction compares by LAST log entry
                 * term, not committed entry terms. */
                if (entryIndex <= state->commitIndex) {
                    D("NODE %u: CRITICAL: Leader trying to overwrite committed "
                      "entry at index %" PRIu64 " (commitIndex=%" PRIu64
                      ") with term %" PRIu64 " - REJECTING ENTIRE RPC!",
                      state->self->nodeId, entryIndex, state->commitIndex,
                      entryTerm);
                    /* Reject the entire AppendEntries RPC. The leader has
                     * entries that conflict with our committed state - this
                     * indicates a serious bug or Byzantine behavior. We keep
                     * our committed entries intact. */
                    kvidxCommit(KRAFT_ENTRIES(state));
                    kraftErrorReply(
                        KRAFT_RPC_STATUS_REJECTED_IDX_TERM_MISMATCH);
                    return false;
                }

                /* "If an existing entry conflicts with a new one (same index
                 * but different terms), delete the existing entry and all that
                 * follow it (§5.3)" */
                D("NODE %u: Deleting every entry >= index %" PRIu64
                  " (commitIndex=%" PRIu64 ")",
                  state->self->nodeId, entryIndex, state->commitIndex);

                /* DEBUG: Track all log truncations */
                fprintf(stderr,
                        "[TRUNCATE] node=%u term=%" PRIu64
                        " truncating from idx=%" PRIu64 " (commitIdx=%" PRIu64
                        ") newTerm=%" PRIu64 " from leader\n",
                        state->self->nodeId, state->self->consensus.term,
                        entryIndex, state->commitIndex, entryTerm);

                kvidxRemoveAfterNInclusive(KRAFT_ENTRIES(state), entryIndex);
                /* Now, with the old bad entries deleted, we add the known-good
                 * entry again. */
                if (!kraftLogAppend(state, entryIndex, entryTerm, originalCmd,
                                    entry, entryLen)) {
                    /* Append failed after truncation - this is a storage error.
                     * We must reject the RPC to avoid inconsistent state. */
                    D("STORAGE ERROR: kraftLogAppend failed for entry at "
                      "(%" PRIu64 ", %" PRIu64 ") after truncation!",
                      entryIndex, entryTerm);
                    kvidxCommit(KRAFT_ENTRIES(state));
                    kraftErrorReply(
                        KRAFT_RPC_STATUS_REJECTED_IDX_TERM_MISMATCH);
                    return false;
                }
            }
        } else {
            /* "Append any new entries not already in the log" */
            if (!kraftLogAppend(state, entryIndex, entryTerm, originalCmd,
                                entry, entryLen)) {
                /* Append failed - this is a storage error.
                 * We must reject the RPC to avoid reporting entries that
                 * weren't actually stored. */
                D("STORAGE ERROR: kraftLogAppend failed for NEW entry at "
                  "(%" PRIu64 ", %" PRIu64 ")!",
                  entryIndex, entryTerm);
                kvidxCommit(KRAFT_ENTRIES(state));
                kraftErrorReply(KRAFT_RPC_STATUS_REJECTED_IDX_TERM_MISMATCH);
                return false;
            }
        }

        kraftRpcJointConsensusIfNecessary(state, entryIndex, originalCmd, entry,
                                          entryLen);
    }

    kvidxCommit(KRAFT_ENTRIES(state));

    /* CRITICAL: Refresh previousEntry from storage after potential log
     * modifications. When entries are deleted and re-appended (lines 61-66
     * above), previousEntry becomes stale. This causes Election Restriction
     * (§5.4.1) violations during voting because we compare against stale
     * previousEntry values, potentially allowing candidates with less
     * complete logs to win elections. */
    kraftStateRefreshPreviousEntry(state);

    D("LEADER COMMIT: %" PRIu64 "; OUR COMMIT: %" PRIu64, rpc->leaderCommit,
      state->commitIndex);
    /* "If leaderCommit > commitIndex, set commitIndex =
     * min(leaderCommit, index of last new entry)" */
    if (rpc->leaderCommit > state->commitIndex) {
        kraftUpdateCommitIndex(state, MIN(rpc->leaderCommit, entryIndex));
    }

    /* CRITICAL FIX: Report the correct match index in the OK reply.
     *
     * If entries were appended, report the last appended entry index.
     * If heartbeat (no entries), report the RPC's previousIndex - this is
     * what the leader asked about and what we confirmed we have.
     *
     * BUG FIX: Previously the heartbeat reply reported the follower's ACTUAL
     * last log entry, which could be from a PREVIOUS LEADER at a DIFFERENT
     * TERM. This caused the leader to incorrectly update matchIndex based on
     * entries that don't match its own log. Example:
     *   - Leader (term 2) has entries 1-3 at term 2
     *   - Follower has entries 1-10 at term 1 (from previous leader)
     *   - Leader sends heartbeat with previousIndex=1
     *   - Follower reports matchIndex=10 (its actual last)
     *   - Leader caps to 3, thinks follower has entry 3 at term 2
     *   - BUG: Follower's entry 3 is at term 1, not term 2!
     *   - Leader incorrectly commits entry 3
     *
     * Fix: For heartbeats, report rpc->previousIndex (what the leader asked
     * about), NOT the follower's actual storage state. This ensures matchIndex
     * only advances based on entries the leader has explicitly replicated.
     */
    kraftRpcUniqueIndex matchIndex;
    kraftTerm matchTerm;
    if (entryIndex > 0) {
        /* Entries were appended - report the last one.
         *
         * DEFENSIVE CHECK: Verify we actually stored the entry before
         * reporting. This guards against bugs where decode succeeded but
         * storage failed. */
        if (!kvidxExistsDual(KRAFT_ENTRIES(state), entryIndex, entryTerm)) {
            D("BUG: Decoded entry at (%" PRIu64 ", %" PRIu64
              ") but it's not in storage! Reporting (0, 0) instead.",
              entryIndex, entryTerm);
            matchIndex = 0;
            matchTerm = 0;
        } else {
            matchIndex = entryIndex;
            matchTerm = entryTerm;
        }
    } else {
        /* Heartbeat (no entries) - report what the leader asked about.
         *
         * DEFENSIVE CHECK: Verify we actually have the entry at previousIndex
         * before reporting it. This guards against bugs where a follower might
         * somehow reach this code without having the entry (e.g., due to
         * message routing bugs, race conditions, or stale RPC data). If we
         * don't have the entry, report (0, 0) - the safest fallback that won't
         * incorrectly advance the leader's matchIndex. */
        if (rpc->previousIndex > 0 &&
            !kvidxExistsDual(KRAFT_ENTRIES(state), rpc->previousIndex,
                             rpc->previousTerm)) {
            D("BUG: Heartbeat reply - passed previousIndex check but missing "
              "entry at (%" PRIu64 ", %" PRIu64 ")! Reporting (0, 0) instead.",
              rpc->previousIndex, rpc->previousTerm);
            matchIndex = 0;
            matchTerm = 0;
        } else {
            matchIndex = rpc->previousIndex;
            matchTerm = rpc->previousTerm;
        }
    }
    kraftRpcReplyAppendEntriesOK(state, rpc, matchIndex, matchTerm);
    /* NEED MORE IMPLEMENTATION:
     *   - actually applies entries to state machine
     *   - updating state->lastApplied
     *   - replying to leader with our commitIndex */
    return true;
}

/* SPEC: Receiver implementation of AppendEntries RPC */
bool kraftRpcProcessFollower(kraftState *state, kraftRpc *rpc,
                             kraftRpcStatus *status) {
    if (!IS_FOLLOWER(state->self)) {
        return false;
    }

    /* CRITICAL: Filter RPC command types. Followers should ONLY process request
     * messages, never reply messages. Reply messages are for
     * leaders/candidates. Bug fix: Previously accepted all messages, causing
     * infinite loops when followers received reply messages and processed them
     * as requests. */
    switch (rpc->cmd) {
    case KRAFT_RPC_APPEND_ENTRIES:
        /* This is the primary message type followers process */
        break;

    case KRAFT_RPC_REQUEST_VOTE:
    case KRAFT_RPC_REQUEST_PREVOTE:
    case KRAFT_RPC_INSTALL_SNAPSHOT:
    case KRAFT_RPC_TIMEOUT_NOW:
        /* These are valid request messages, but followers can't handle them
         * yet. They should be processed by kraftRpcProcess before role
         * dispatch. */
        D("Follower received %s - should be handled before role dispatch",
          kraftRpcCmds[rpc->cmd]);
        *status = KRAFT_RPC_STATUS_OK;
        return false;

    default:
        /* REJECT ALL REPLY MESSAGES - followers don't process replies! */
        D("ERROR! Follower received invalid RPC: %s (cmd=%d) - rejecting",
          kraftRpcCmds[rpc->cmd], rpc->cmd);
        *status = KRAFT_RPC_STATUS_OK;
        return false;
    }

    /* Ideal path: Add entry to log.  Reply with SUCCESS. */

    /* "To ensure this, each client command has an associated serial number.
     * Each consensus module caches the last serial number processed from each
     * client and each response given. If a consensus module is given the same
     * command twice, then the second time it simply returns the cached
     * response."
     */

    /* "Reply false if log doesn't contain an entry at prevLogIndex
     *  whose term matches prevLogTerm (§5.3)" */
    /* Note: we only compare if previousIndex > 0 because when
     * previousIndex == 0 this is the first entry in our log, so we have
     * no previous entries to compare against. */
    if (rpc->previousIndex > 0) {
        D("RPC provided Previous Index of: %" PRIu64
          " and Previous Term: %" PRIu64,
          rpc->previousIndex, rpc->previousTerm);

        const bool logMatch = kvidxExistsDual(
            KRAFT_ENTRIES(state), rpc->previousIndex, rpc->previousTerm);

        /* "If the follower does not find an entry in its log with the same
         * index and term, then it refuses the new entries." */
        if (!logMatch) {
            /* Reply to leader saying the previous idx/term doesn't match for
             * us. */
            D("LOG NO MATCH! Replying with mismatch error. No match for "
              "(%" PRIu64 ", %" PRIu64 ")",
              rpc->previousIndex, rpc->previousTerm);
            kraftErrorReply(KRAFT_RPC_STATUS_REJECTED_IDX_TERM_MISMATCH);
            return false; /* CRITICAL: Must return after rejecting! */
        }

        D("Successful previous index match for previous term!\n");
    }

    return processAppendEntries(state, rpc, status);
}
