/* kraftLinearizabilityFailure.h - Failure Injection for Testing
 *
 * Provides controlled failure injection for linearizability testing:
 * - Leader crashes
 * - Follower crashes
 * - Network partitions
 * - Leadership transfers
 *
 * Failures can be triggered at specific times or percentages through
 * the test execution.
 */

#ifndef KRAFT_LINEARIZABILITY_FAILURE_H
#define KRAFT_LINEARIZABILITY_FAILURE_H

#include "kraftLinearizabilityCluster.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Failure Types
 * ============================================================================
 */

typedef enum kraftLinFailureType {
    KRAFT_LIN_FAILURE_NONE,               /* No failure */
    KRAFT_LIN_FAILURE_CRASH_LEADER,       /* Kill current leader */
    KRAFT_LIN_FAILURE_CRASH_FOLLOWER,     /* Kill random follower */
    KRAFT_LIN_FAILURE_CRASH_MINORITY,     /* Kill minority of nodes */
    KRAFT_LIN_FAILURE_PARTITION_LEADER,   /* Isolate leader via SIGSTOP */
    KRAFT_LIN_FAILURE_PARTITION_MINORITY, /* Isolate minority */
    KRAFT_LIN_FAILURE_TRANSFER_LEADER,    /* Force leadership transfer */
    KRAFT_LIN_FAILURE_ROLLING_RESTART,    /* Sequential node restarts */
} kraftLinFailureType;

/**
 * kraftLinFailureSpec - Failure specification
 */
typedef struct kraftLinFailureSpec {
    kraftLinFailureType type;
    uint64_t triggerAtUs; /* When to inject (microseconds from start) */
    uint64_t durationUs;  /* How long failure lasts (0 = permanent) */
    uint32_t targetNode;  /* Specific node (0 = auto-select) */
    bool verbose;
} kraftLinFailureSpec;

/* ============================================================================
 * Failure Injection
 * ============================================================================
 */

/**
 * kraftLinInjectFailure - Inject a failure into the cluster
 *
 * @param cluster  Cluster to inject failure into
 * @param spec     Failure specification
 * @return         true on success
 */
bool kraftLinInjectFailure(kraftLinCluster *cluster,
                           const kraftLinFailureSpec *spec);

/**
 * kraftLinRecoverFailure - Recover from an injected failure
 *
 * For temporary failures (partitions, process suspensions), this
 * reverses the failure after the specified duration.
 *
 * @param cluster  Cluster to recover
 * @param spec     Original failure spec
 * @return         true on success
 */
bool kraftLinRecoverFailure(kraftLinCluster *cluster,
                            const kraftLinFailureSpec *spec);

/**
 * kraftLinFailureTypeString - Get string name for failure type
 *
 * @param type  Failure type
 * @return      String representation
 */
static inline const char *kraftLinFailureTypeString(kraftLinFailureType type) {
    switch (type) {
    case KRAFT_LIN_FAILURE_NONE:
        return "none";
    case KRAFT_LIN_FAILURE_CRASH_LEADER:
        return "crash-leader";
    case KRAFT_LIN_FAILURE_CRASH_FOLLOWER:
        return "crash-follower";
    case KRAFT_LIN_FAILURE_CRASH_MINORITY:
        return "crash-minority";
    case KRAFT_LIN_FAILURE_PARTITION_LEADER:
        return "partition-leader";
    case KRAFT_LIN_FAILURE_PARTITION_MINORITY:
        return "partition-minority";
    case KRAFT_LIN_FAILURE_TRANSFER_LEADER:
        return "transfer-leader";
    case KRAFT_LIN_FAILURE_ROLLING_RESTART:
        return "rolling-restart";
    default:
        return "unknown";
    }
}

#ifdef __cplusplus
}
#endif

#endif /* KRAFT_LINEARIZABILITY_FAILURE_H */
