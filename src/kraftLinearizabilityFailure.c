/* kraftLinearizabilityFailure.c - Failure Injection Implementation
 *
 * Implements controlled failure injection for testing cluster resilience.
 */

#include "kraftLinearizabilityFailure.h"

#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

/* ============================================================================
 * Static State for Partition Tracking
 * ============================================================================
 */

static uint32_t *suspendedNodes = NULL;
static uint32_t suspendedCount = 0;
static uint32_t suspendedCapacity = 0;

static void addSuspendedNode(uint32_t nodeId) {
    if (suspendedCount >= suspendedCapacity) {
        uint32_t newCap = suspendedCapacity ? suspendedCapacity * 2 : 8;
        uint32_t *newArray = realloc(suspendedNodes, newCap * sizeof(uint32_t));
        if (!newArray) {
            return;
        }
        suspendedNodes = newArray;
        suspendedCapacity = newCap;
    }
    suspendedNodes[suspendedCount++] = nodeId;
}

/* ============================================================================
 * Failure Injection Implementations
 * ============================================================================
 */

static bool injectCrashLeader(kraftLinCluster *cluster,
                              const kraftLinFailureSpec *spec) {
    /* Find current leader */
    uint32_t leader = kraftLinClusterFindLeader(cluster);
    if (leader == 0) {
        if (spec->verbose) {
            fprintf(stderr, "Cannot crash leader: no leader found\n");
        }
        return false;
    }

    if (spec->verbose) {
        printf("[FAILURE] Crashing leader (node %u) with SIGKILL\n", leader);
    }

    return kraftLinClusterKillNode(cluster, leader, SIGKILL);
}

static bool injectCrashFollower(kraftLinCluster *cluster,
                                const kraftLinFailureSpec *spec) {
    /* Find a follower (non-leader node that's alive) */
    uint32_t leader = kraftLinClusterFindLeader(cluster);
    uint32_t target = 0;

    for (uint32_t i = 1; i <= cluster->config.nodeCount; i++) {
        if (i != leader && cluster->nodeAlive[i - 1]) {
            target = i;
            break;
        }
    }

    if (target == 0) {
        if (spec->verbose) {
            fprintf(stderr, "Cannot crash follower: no followers alive\n");
        }
        return false;
    }

    if (spec->verbose) {
        printf("[FAILURE] Crashing follower (node %u) with SIGKILL\n", target);
    }

    return kraftLinClusterKillNode(cluster, target, SIGKILL);
}

static bool injectPartitionLeader(kraftLinCluster *cluster,
                                  const kraftLinFailureSpec *spec) {
    /* Find and isolate leader using SIGSTOP */
    uint32_t leader = kraftLinClusterFindLeader(cluster);
    if (leader == 0) {
        if (spec->verbose) {
            fprintf(stderr, "Cannot partition leader: no leader found\n");
        }
        return false;
    }

    if (spec->verbose) {
        printf("[FAILURE] Partitioning leader (node %u) with SIGSTOP\n",
               leader);
    }

    uint32_t idx = leader - 1;
    if (cluster->nodePids[idx] > 0) {
        kill(cluster->nodePids[idx], SIGSTOP);
        addSuspendedNode(leader);
        return true;
    }

    return false;
}

static bool injectPartitionMinority(kraftLinCluster *cluster,
                                    const kraftLinFailureSpec *spec) {
    /* Suspend minority of nodes */
    uint32_t minority = cluster->config.nodeCount / 2; /* Floor division */
    if (minority == 0) {
        minority = 1;
    }

    if (spec->verbose) {
        printf("[FAILURE] Partitioning minority (%u nodes) with SIGSTOP\n",
               minority);
    }

    uint32_t suspended = 0;
    for (uint32_t i = 1; i <= cluster->config.nodeCount && suspended < minority;
         i++) {
        if (cluster->nodeAlive[i - 1]) {
            kill(cluster->nodePids[i - 1], SIGSTOP);
            addSuspendedNode(i);
            suspended++;
        }
    }

    return (suspended > 0);
}

/* ============================================================================
 * Public API
 * ============================================================================
 */

bool kraftLinInjectFailure(kraftLinCluster *cluster,
                           const kraftLinFailureSpec *spec) {
    if (!cluster || !spec) {
        return false;
    }

    switch (spec->type) {
    case KRAFT_LIN_FAILURE_NONE:
        return true;

    case KRAFT_LIN_FAILURE_CRASH_LEADER:
        return injectCrashLeader(cluster, spec);

    case KRAFT_LIN_FAILURE_CRASH_FOLLOWER:
        return injectCrashFollower(cluster, spec);

    case KRAFT_LIN_FAILURE_PARTITION_LEADER:
        return injectPartitionLeader(cluster, spec);

    case KRAFT_LIN_FAILURE_PARTITION_MINORITY:
        return injectPartitionMinority(cluster, spec);

    case KRAFT_LIN_FAILURE_TRANSFER_LEADER:
        /* TODO: Implement via TRANSFER command to Client API */
        if (spec->verbose) {
            printf("[FAILURE] Leadership transfer not yet implemented\n");
        }
        return false;

    case KRAFT_LIN_FAILURE_ROLLING_RESTART:
        /* TODO: Implement sequential restart pattern */
        if (spec->verbose) {
            printf("[FAILURE] Rolling restart not yet implemented\n");
        }
        return false;

    default:
        return false;
    }
}

bool kraftLinRecoverFailure(kraftLinCluster *cluster,
                            const kraftLinFailureSpec *spec) {
    if (!cluster || !spec) {
        return false;
    }

    /* Resume any suspended (SIGSTOP'd) nodes */
    for (uint32_t i = 0; i < suspendedCount; i++) {
        uint32_t nodeId = suspendedNodes[i];
        if (nodeId >= 1 && nodeId <= cluster->config.nodeCount) {
            uint32_t idx = nodeId - 1;
            if (cluster->nodePids[idx] > 0) {
                if (spec->verbose) {
                    printf("[RECOVERY] Resuming node %u with SIGCONT\n",
                           nodeId);
                }
                kill(cluster->nodePids[idx], SIGCONT);
            }
        }
    }

    suspendedCount = 0;
    return true;
}
