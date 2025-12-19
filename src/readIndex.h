#ifndef KRAFT_READ_INDEX_H
#define KRAFT_READ_INDEX_H

#include "kraft.h"

/* ReadIndex Protocol (Raft thesis §6.4)
 *
 * Enables linearizable reads without writing to the log, providing
 * dramatically higher read throughput (~100x improvement).
 *
 * Protocol:
 * 1. Leader receives read request
 * 2. Leader records current commitIndex as readIndex
 * 3. Leader sends AppendEntries heartbeat to confirm leadership
 * 4. Once majority confirms, read at readIndex is safe (linearizable)
 * 5. Execute read and return result
 *
 * Safety: Ensures we're still leader and no committed entries are missing.
 */

/* Status codes for ReadIndex operations */
typedef enum {
    KRAFT_READINDEX_OK = 0,
    KRAFT_READINDEX_NOT_LEADER = 1,
    KRAFT_READINDEX_TIMEOUT = 2,
    KRAFT_READINDEX_INVALID_REQUEST = 3,
} kraftReadIndexStatus;

/* State of a pending read request */
typedef enum {
    KRAFT_READINDEX_PENDING = 0,       /* Waiting for heartbeat confirmation */
    KRAFT_READINDEX_CONFIRMED = 1,     /* Majority confirmed, safe to execute */
    KRAFT_READINDEX_TIMEOUT_STATE = 2, /* Timed out waiting for confirmation */
} kraftReadIndexState;

/* Initialize ReadIndex tracking for a kraftState */
void kraftReadIndexInit(kraftState *state);

/* Cleanup ReadIndex tracking */
void kraftReadIndexFree(kraftState *state);

/* Begin a new read request (leader only)
 *
 * Returns readIndex identifier that will be used to track this read.
 * After calling this, you must wait for kraftReadIndexIsReady() to return
 * true before executing the read.
 *
 * Returns: readIndex (>= 0) on success, or negative error code
 */
int64_t kraftReadIndexBegin(kraftState *state);

/* Process pending ReadIndex requests (called during heartbeat processing)
 *
 * This should be called after receiving AppendEntries responses to check
 * if we've achieved quorum confirmation for any pending reads.
 */
void kraftReadIndexProcess(kraftState *state);

/* Check if a readIndex request is ready to execute
 *
 * Returns true if the read at this readIndex is safe to execute (linearizable).
 */
bool kraftReadIndexIsReady(kraftState *state, uint64_t readIndex);

/* Mark a readIndex as completed (after executing the read)
 *
 * This cleans up tracking state for the read request.
 */
void kraftReadIndexComplete(kraftState *state, uint64_t readIndex);

#endif /* KRAFT_READ_INDEX_H */
