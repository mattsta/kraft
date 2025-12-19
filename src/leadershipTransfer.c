#include "leadershipTransfer.h"
#include "rpc.h"
#include <inttypes.h>

/* Default transfer timeout: 2x maximum election timeout (500ms) = 1000ms */
#define KRAFT_TRANSFER_TIMEOUT_MS 1000

/* Initiate leadership transfer to target node */
kraftTransferStatus kraftTransferLeadershipBegin(kraftState *state,
                                                 kraftNode *targetNode) {
    /* Validate we're the leader */
    if (!IS_LEADER(state->self)) {
        D("Cannot transfer leadership: not the leader");
        return KRAFT_TRANSFER_STATUS_NOT_LEADER;
    }

    /* Check if transfer already in progress */
    if (state->transfer.state != KRAFT_TRANSFER_NONE) {
        D("Cannot transfer leadership: transfer already in progress");
        return KRAFT_TRANSFER_STATUS_ALREADY_IN_PROGRESS;
    }

    /* Validate target node exists */
    if (!targetNode) {
        D("Cannot transfer leadership: target node is NULL");
        return KRAFT_TRANSFER_STATUS_TARGET_NOT_FOUND;
    }

    /* Don't transfer to ourselves */
    if (targetNode->isOurself) {
        D("Cannot transfer leadership: target is ourselves");
        return KRAFT_TRANSFER_STATUS_TARGET_NOT_FOUND;
    }

    /* Validate target is in our cluster */
    bool targetInOld = vecPtrContains(NODES_OLD_NODES(state), targetNode);
    bool targetInNew = vecPtrContains(NODES_NEW_NODES(state), targetNode);

    if (!targetInOld && !targetInNew) {
        D("Cannot transfer leadership: target not in cluster");
        return KRAFT_TRANSFER_STATUS_TARGET_NOT_FOUND;
    }

    /* Initialize transfer state */
    state->transfer.targetNode = targetNode;
    state->transfer.state = KRAFT_TRANSFER_CATCHING_UP;
    state->transfer.startTime = kraftmstime();
    state->transfer.timeout =
        state->transfer.startTime + KRAFT_TRANSFER_TIMEOUT_MS;
    state->transfer.targetMatchIndex =
        targetNode->consensus.leaderData.matchIndex;

    D("Starting leadership transfer to node %p (current matchIndex: %" PRIu64
      ")",
      (void *)targetNode, targetNode->consensus.leaderData.matchIndex);

    /* Immediately send AppendEntries to help target catch up */
    kraftRpcSendHeartbeat(state);

    return KRAFT_TRANSFER_STATUS_OK;
}

/* Get current transfer status */
kraftTransferStatus kraftTransferLeadershipStatus(kraftState *state) {
    switch (state->transfer.state) {
    case KRAFT_TRANSFER_NONE:
        return KRAFT_TRANSFER_STATUS_OK;
    case KRAFT_TRANSFER_CATCHING_UP:
    case KRAFT_TRANSFER_READY:
        return KRAFT_TRANSFER_STATUS_IN_PROGRESS;
    case KRAFT_TRANSFER_TIMEOUT:
        return KRAFT_TRANSFER_STATUS_TIMEOUT;
    default:
        return KRAFT_TRANSFER_STATUS_OK;
    }
}

/* Cancel ongoing leadership transfer */
void kraftTransferLeadershipCancel(kraftState *state) {
    if (state->transfer.state != KRAFT_TRANSFER_NONE) {
        D("Cancelling leadership transfer to node %p",
          (void *)state->transfer.targetNode);

        state->transfer.state = KRAFT_TRANSFER_NONE;
        state->transfer.targetNode = NULL;
        state->transfer.startTime = 0;
        state->transfer.timeout = 0;
        state->transfer.targetMatchIndex = 0;
    }
}

/* Send TimeoutNow RPC to target to trigger immediate election */
static void sendTimeoutNow(kraftState *state, kraftNode *target) {
    kraftRpc timeoutNow = {0};
    kraftRpcPopulate(state, &timeoutNow);
    timeoutNow.cmd = KRAFT_RPC_TIMEOUT_NOW;

    D("Sending TimeoutNow to node %p", (void *)target);
    kraftRpcSendSingle(state, &timeoutNow, target);
}

/* Process leadership transfer state machine */
void kraftTransferLeadershipProcess(kraftState *state) {
    /* No transfer in progress */
    if (state->transfer.state == KRAFT_TRANSFER_NONE) {
        return;
    }

    /* Check for timeout */
    uint64_t now = kraftmstime();
    if (now >= state->transfer.timeout) {
        D("Leadership transfer timed out after %" PRIu64 " ms",
          now - state->transfer.startTime);
        state->transfer.state = KRAFT_TRANSFER_TIMEOUT;
        kraftTransferLeadershipCancel(state);
        return;
    }

    /* Check if we're still the leader */
    if (!IS_LEADER(state->self)) {
        D("No longer leader (target likely won election), transfer "
          "successful!");
        kraftTransferLeadershipCancel(state);
        return;
    }

    /* Validate target still exists */
    if (!state->transfer.targetNode) {
        D("Target node disappeared, cancelling transfer");
        kraftTransferLeadershipCancel(state);
        return;
    }

    kraftNode *target = state->transfer.targetNode;

    /* Check if transfer is in catching up state */
    if (state->transfer.state == KRAFT_TRANSFER_CATCHING_UP) {
        /* Check if target has caught up to our log */
        kraftRpcUniqueIndex currentIndex = state->self->consensus.currentIndex;
        kraftRpcUniqueIndex commitIndex = state->commitIndex;
        kraftRpcUniqueIndex targetMatch =
            target->consensus.leaderData.matchIndex;

        D("Transfer progress: target matchIndex=%" PRIu64
          ", leader currentIndex=%" PRIu64 ", commitIndex=%" PRIu64,
          targetMatch, currentIndex, commitIndex);

        /* Safety: Target must have all committed entries AND all our current
         * entries. Also, only transfer if we have a stable log (commitIndex > 0
         * OR currentIndex == 0). This prevents transferring leadership before
         * the cluster log has stabilized. */
        bool targetHasAllEntries = (targetMatch >= currentIndex);
        bool targetHasAllCommitted = (targetMatch >= commitIndex);
        bool logIsStable =
            (commitIndex > 0) || (currentIndex == 0 && commitIndex == 0);

        /* Target is caught up when all conditions are met */
        if (targetHasAllEntries && targetHasAllCommitted && logIsStable) {
            D("Target caught up! Sending TimeoutNow");
            state->transfer.state = KRAFT_TRANSFER_READY;

            /* Send TimeoutNow RPC to trigger immediate election */
            sendTimeoutNow(state, target);

            /* CRITICAL: Do NOT step down immediately!
             * Per Raft thesis §3.10: "The old leader steps down once it learns
             * that the target has won the election."
             *
             * Stepping down immediately creates a dangerous window where
             * another node might win an election with a stale log, violating
             * safety.
             *
             * Instead, we stay as leader but stop accepting new writes. When we
             * receive an AppendEntries from the new leader (the target), we'll
             * step down then. The transfer state KRAFT_TRANSFER_READY blocks
             * new writes via kraftTransferIsBlocking(). */

            D("Sent TimeoutNow to target - waiting for target to win election");
            D("Leadership transfer initiated - blocking new writes");
        } else {
            /* Target not caught up yet, send more AppendEntries */
            D("Target not caught up yet, sending AppendEntries");
            kraftRpcSendHeartbeat(state);
        }
    }
}

/* Check if transfer is active and blocking new writes */
bool kraftTransferIsBlocking(kraftState *state) {
    /* Block writes when we're in READY state (brief window before step-down) */
    return state->transfer.state == KRAFT_TRANSFER_READY;
}
