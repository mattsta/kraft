/* kraftLinearizability.h - Linearizability Testing for Kraft Raft
 *
 * This module provides linearizability verification for distributed systems
 * using a Jepsen-style approach with happens-before graph analysis.
 *
 * Key capabilities:
 * - Track concurrent operation histories from multiple clients
 * - Build happens-before relationships based on real-time order and causality
 * - Verify linearizability using Wing & Gong algorithm
 * - Generate counterexamples for violations
 * - Support for various data models (register, key-value, counter, etc.)
 *
 * Architecture:
 * - Thread-safe operation recording for concurrent clients
 * - Efficient indexing for fast verification
 * - Scalable to millions of operations
 * - Deterministic verification with reproducible results
 */

#ifndef KRAFT_LINEARIZABILITY_H
#define KRAFT_LINEARIZABILITY_H

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Operation Tracking
 * ============================================================================
 */

/**
 * kraftLinOpId - Unique operation identifier
 *
 * Combines client ID, per-client sequence, and global sequence to uniquely
 * identify each operation in the system.
 */
typedef struct kraftLinOpId {
    uint64_t clientId; /* Client thread that submitted */
    uint64_t seq;      /* Sequence number within client */
    uint64_t global;   /* Global sequence across all clients */
} kraftLinOpId;

/**
 * kraftLinOpType - Operation type classification
 */
typedef enum kraftLinOpType {
    KRAFT_LIN_OP_SET = 'S',  /* SET key value - write operation */
    KRAFT_LIN_OP_GET = 'G',  /* GET key - read operation */
    KRAFT_LIN_OP_INCR = 'I', /* INCR key - increment operation */
    KRAFT_LIN_OP_DEL = 'D',  /* DEL key - delete operation */
    KRAFT_LIN_OP_CAS = 'C',  /* CAS key old new - compare-and-swap */
} kraftLinOpType;

/**
 * kraftLinOp - Single operation invocation and response
 *
 * Records both the invocation (when the client submits) and the return
 * (when the client receives a response). For linearizability verification,
 * we need both timestamps to establish happens-before relationships.
 */
typedef struct kraftLinOp {
    kraftLinOpId id;

    /* Operation details */
    kraftLinOpType type;
    char key[256];
    char value[512]; /* Value for writes, observed for reads */

    /* Timing (monotonic clock in microseconds) */
    uint64_t invokeUs; /* When operation was invoked */
    uint64_t returnUs; /* When response was received (0 if pending) */

    /* Result */
    bool completed; /* True if operation received response */
    bool success;   /* True if operation succeeded */
    bool timedOut;  /* True if operation timed out */
    bool pending;   /* True if operation is in-flight */

    /* Optional: Raft metadata (if available) */
    uint64_t logIndex; /* Log index where committed (0 if unknown) */
    uint64_t term;     /* Term when committed (0 if unknown) */

} kraftLinOp;

/* ============================================================================
 * Operation History
 * ============================================================================
 */

/**
 * kraftLinHistory - Complete history of all operations
 *
 * Stores all operation invocations and responses from all clients.
 * Provides efficient lookups for verification.
 *
 * Thread-safety: All mutation operations are protected by internal mutex.
 * Read operations during verification assume no concurrent mutations.
 */
typedef struct kraftLinHistory {
    /* Operation storage */
    kraftLinOp *ops;      /* Dynamic array of operations */
    uint32_t count;       /* Number of operations recorded */
    uint32_t capacity;    /* Allocated capacity */
    pthread_mutex_t lock; /* Protects concurrent updates */

    /* Global operation sequence (atomic) */
    uint64_t nextGlobalSeq;

    /* Statistics */
    uint64_t totalInvocations;
    uint64_t totalCompletions;
    uint64_t totalTimeouts;
    uint64_t totalFailures;

} kraftLinHistory;

/**
 * kraftLinHistoryCreate - Create a new operation history
 *
 * @param initialCapacity  Initial capacity for operations (default: 10000)
 * @return                 New history, or NULL on failure
 */
kraftLinHistory *kraftLinHistoryCreate(uint32_t initialCapacity);

/**
 * kraftLinHistoryDestroy - Free all history resources
 *
 * @param history  History to destroy
 */
void kraftLinHistoryDestroy(kraftLinHistory *history);

/**
 * kraftLinHistoryRecordInvoke - Record operation invocation
 *
 * Called when a client submits an operation. Assigns a unique operation ID
 * and records the invocation timestamp.
 *
 * Thread-safe: Can be called concurrently from multiple client threads.
 *
 * @param history   Operation history
 * @param clientId  Client thread ID
 * @param clientSeq Per-client sequence number
 * @param type      Operation type
 * @param key       Key being operated on
 * @param value     Value for writes (or expected for CAS)
 * @return          Assigned operation ID
 */
kraftLinOpId kraftLinHistoryRecordInvoke(kraftLinHistory *history,
                                         uint64_t clientId, uint64_t clientSeq,
                                         kraftLinOpType type, const char *key,
                                         const char *value);

/**
 * kraftLinHistoryRecordReturn - Record operation response
 *
 * Called when a client receives a response. Updates the operation with
 * completion time and result.
 *
 * Thread-safe: Can be called concurrently from multiple client threads.
 *
 * @param history  Operation history
 * @param opId     Operation ID (from RecordInvoke)
 * @param success  Whether operation succeeded
 * @param value    Observed value for reads
 */
void kraftLinHistoryRecordReturn(kraftLinHistory *history, kraftLinOpId opId,
                                 bool success, const char *value);

/**
 * kraftLinHistoryRecordTimeout - Record operation timeout
 *
 * Called when an operation times out without receiving a response.
 *
 * Thread-safe: Can be called concurrently from multiple client threads.
 *
 * @param history  Operation history
 * @param opId     Operation ID
 */
void kraftLinHistoryRecordTimeout(kraftLinHistory *history, kraftLinOpId opId);

/**
 * kraftLinHistoryGetOp - Find operation by ID
 *
 * Not thread-safe: Should only be called after all clients have stopped.
 *
 * @param history  Operation history
 * @param opId     Operation ID to find
 * @return         Pointer to operation, or NULL if not found
 */
kraftLinOp *kraftLinHistoryGetOp(kraftLinHistory *history, kraftLinOpId opId);

/**
 * kraftLinHistoryGetStats - Get history statistics
 *
 * @param history  Operation history
 * @param invocations  Output: total invocations
 * @param completions  Output: total completions
 * @param timeouts     Output: total timeouts
 */
void kraftLinHistoryGetStats(const kraftLinHistory *history,
                             uint64_t *invocations, uint64_t *completions,
                             uint64_t *timeouts);

/* ============================================================================
 * Linearizability Verification
 * ============================================================================
 */

/**
 * kraftLinResult - Verification result
 */
typedef struct kraftLinResult {
    bool isLinearizable;

    /* If not linearizable, provide counterexample */
    struct {
        uint32_t *opIndices;    /* Indices into history.ops */
        uint32_t opCount;       /* Number of operations in violation */
        char explanation[1024]; /* Human-readable explanation */
    } violation;

    /* Verification statistics */
    uint64_t verificationTimeUs;
    uint64_t graphEdgeCount;
    uint64_t statesExplored;

} kraftLinResult;

/**
 * kraftLinVerify - Verify linearizability of operation history
 *
 * Uses the Wing & Gong algorithm with optimizations to determine if the
 * recorded operation history is linearizable. If not, generates a minimal
 * counterexample.
 *
 * Not thread-safe: Assumes no concurrent modifications to history during
 * verification.
 *
 * @param history  Complete operation history
 * @return         Verification result (caller must free violation.opIndices)
 */
kraftLinResult kraftLinVerify(kraftLinHistory *history);

/**
 * kraftLinResultDestroy - Free verification result resources
 *
 * @param result  Result to free
 */
void kraftLinResultDestroy(kraftLinResult *result);

/**
 * kraftLinResultPrint - Print verification result
 *
 * Outputs human-readable verification result with counterexample if
 * linearizability was violated.
 *
 * @param result   Verification result
 * @param history  Operation history (for printing violation details)
 */
void kraftLinResultPrint(const kraftLinResult *result,
                         const kraftLinHistory *history);

/**
 * kraftLinResultPrintJSON - Print verification result as JSON
 *
 * @param result   Verification result
 * @param history  Operation history
 * @param out      Output file
 */
void kraftLinResultPrintJSON(const kraftLinResult *result,
                             const kraftLinHistory *history, FILE *out);

/* ============================================================================
 * Utility Functions
 * ============================================================================
 */

/**
 * kraftLinGetMonotonicUs - Get monotonic timestamp in microseconds
 *
 * Uses CLOCK_MONOTONIC to avoid issues with system clock adjustments.
 *
 * @return  Current monotonic time in microseconds
 */
static inline uint64_t kraftLinGetMonotonicUs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000UL + (uint64_t)ts.tv_nsec / 1000UL;
}

/**
 * kraftLinOpTypeString - Get string name for operation type
 *
 * @param type  Operation type
 * @return      String representation
 */
static inline const char *kraftLinOpTypeString(kraftLinOpType type) {
    switch (type) {
    case KRAFT_LIN_OP_SET:
        return "SET";
    case KRAFT_LIN_OP_GET:
        return "GET";
    case KRAFT_LIN_OP_INCR:
        return "INCR";
    case KRAFT_LIN_OP_DEL:
        return "DEL";
    case KRAFT_LIN_OP_CAS:
        return "CAS";
    default:
        return "UNKNOWN";
    }
}

#ifdef __cplusplus
}
#endif

#endif /* KRAFT_LINEARIZABILITY_H */
