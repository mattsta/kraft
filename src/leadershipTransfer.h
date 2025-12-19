#ifndef LEADERSHIP_TRANSFER_H
#define LEADERSHIP_TRANSFER_H

#include "kraft.h"

/* Leadership Transfer API
 *
 * Enables zero-downtime leadership changes for maintenance operations.
 * The leader helps a specific follower catch up to its log state, then
 * sends it a TimeoutNow RPC to immediately trigger an election.
 *
 * Based on Raft thesis §3.10 - Leadership transfer extension
 */

/* Status codes for leadership transfer operations */
typedef enum kraftTransferStatus {
    KRAFT_TRANSFER_STATUS_OK = 0,
    KRAFT_TRANSFER_STATUS_IN_PROGRESS,
    KRAFT_TRANSFER_STATUS_NOT_LEADER,
    KRAFT_TRANSFER_STATUS_TARGET_NOT_FOUND,
    KRAFT_TRANSFER_STATUS_TARGET_NOT_CAUGHT_UP,
    KRAFT_TRANSFER_STATUS_ALREADY_IN_PROGRESS,
    KRAFT_TRANSFER_STATUS_TIMEOUT,
    KRAFT_TRANSFER_STATUS_COMPLETE
} kraftTransferStatus;

/* Initiate leadership transfer to a target node
 *
 * Parameters:
 *   state: Current Raft state (must be leader)
 *   targetNode: Node to transfer leadership to
 *
 * Returns:
 *   KRAFT_TRANSFER_STATUS_OK - Transfer initiated successfully
 *   KRAFT_TRANSFER_STATUS_NOT_LEADER - Cannot transfer, not the leader
 *   KRAFT_TRANSFER_STATUS_TARGET_NOT_FOUND - Target node doesn't exist
 *   KRAFT_TRANSFER_STATUS_ALREADY_IN_PROGRESS - Transfer already active
 *
 * Algorithm:
 *   1. Validate we're the leader
 *   2. Validate target node exists
 *   3. Set transfer state to CATCHING_UP
 *   4. Aggressively replicate to target
 *   5. When caught up, send TimeoutNow RPC
 *   6. Step down as leader
 */
kraftTransferStatus kraftTransferLeadershipBegin(kraftState *state,
                                                 kraftNode *targetNode);

/* Get current transfer status
 *
 * Returns:
 *   KRAFT_TRANSFER_STATUS_OK - No transfer in progress
 *   KRAFT_TRANSFER_STATUS_IN_PROGRESS - Transfer ongoing
 *   KRAFT_TRANSFER_STATUS_COMPLETE - Transfer succeeded
 *   KRAFT_TRANSFER_STATUS_TIMEOUT - Transfer failed
 */
kraftTransferStatus kraftTransferLeadershipStatus(kraftState *state);

/* Cancel ongoing leadership transfer
 *
 * Safe to call even if no transfer is in progress.
 * Resets transfer state to NONE.
 */
void kraftTransferLeadershipCancel(kraftState *state);

/* Process leadership transfer state machine
 *
 * Called periodically by the leader to:
 *   - Check if target is caught up
 *   - Send TimeoutNow when ready
 *   - Handle timeouts
 *
 * This is called from the leader's heartbeat timer.
 */
void kraftTransferLeadershipProcess(kraftState *state);

/* Check if transfer is active and blocking new writes
 *
 * Returns true if transfer is in READY state (about to send TimeoutNow)
 * During this brief window, new writes should be rejected to ensure
 * clean handoff.
 */
bool kraftTransferIsBlocking(kraftState *state);

#endif /* LEADERSHIP_TRANSFER_H */
