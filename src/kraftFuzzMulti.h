/* Kraft Multi-Raft Fuzzer - Multi-Group Consensus Testing
 * =========================================================
 * Tests multiple independent Raft groups running actual consensus.
 * Each group is a full testCluster with real leader election,
 * log replication, and invariant checking.
 *
 * Features:
 * - Multiple concurrent Raft groups with real consensus
 * - Per-group: command submission, crashes, partitions
 * - Cross-group: coordinated failures, global partitions
 * - Configurable: groups, nodes per group, operations, time
 * - Full Raft invariant checking per group
 *
 * This is NOT just metadata testing - it runs real Raft.
 */

#ifndef KRAFT_FUZZ_MULTI_H
#define KRAFT_FUZZ_MULTI_H

#include <stdbool.h>
#include <stdint.h>

/* Multi-Raft Fuzzer Configuration */
typedef struct {
    /* Cluster configuration */
    uint32_t groupCount;    /* Number of Raft groups to run */
    uint32_t nodesPerGroup; /* Nodes per group (3, 5, 7, etc.) */

    /* Limits */
    uint64_t maxOperations; /* Max operations before stopping */
    uint64_t maxTime;       /* Max time (microseconds) */
    uint32_t seed;          /* Random seed for reproducibility */

    /* Per-group operation weights (out of 1000) */
    uint32_t weightSubmitCommand;  /* Submit command to a group */
    uint32_t weightNodeCrash;      /* Crash a node in a group */
    uint32_t weightNodeRecover;    /* Recover a crashed node */
    uint32_t weightPartitionGroup; /* Partition within a group */
    uint32_t weightHealGroup;      /* Heal a group's partition */
    uint32_t weightTickGroup;      /* Tick a specific group */

    /* Cross-group operation weights */
    uint32_t weightGlobalPartition; /* Partition affects all groups */
    uint32_t weightGlobalHeal;      /* Heal all partitions */
    uint32_t weightTickAll;         /* Tick all groups */

    /* Constraints */
    uint32_t maxCrashedPerGroup; /* Max crashed nodes per group */
    uint64_t minRecoveryTime;    /* Min time before recovery (us) */
    uint64_t maxRecoveryTime;    /* Max time before recovery (us) */

    /* Invariant checking */
    bool checkElectionSafety;
    bool checkLogMatching;
    bool checkLeaderCompleteness;
    bool checkStateMachineSafety;

    /* Reporting */
    bool verbose;
    uint64_t reportInterval;
} kraftFuzzMultiConfig;

/* Per-group Statistics */
typedef struct {
    uint64_t commandsSubmitted;
    uint64_t nodesCrashed;
    uint64_t nodesRecovered;
    uint64_t partitionsCreated;
    uint64_t partitionsHealed;
    uint64_t electionsTotal;
    uint64_t messagesDelivered;
    uint64_t messagesDropped;
} kraftFuzzMultiGroupStats;

/* Overall Fuzzer Statistics */
typedef struct {
    uint64_t totalOperations;
    uint64_t totalTicks;

    /* Aggregated across all groups */
    uint64_t totalCommandsSubmitted;
    uint64_t totalNodesCrashed;
    uint64_t totalNodesRecovered;
    uint64_t totalPartitions;
    uint64_t totalElections;
    uint64_t totalMessagesDelivered;
    uint64_t totalMessagesDropped;

    /* Cross-group operations */
    uint64_t globalPartitions;
    uint64_t globalHeals;

    /* Invariant checking */
    uint64_t invariantChecks;
    uint64_t invariantViolations;
    uint32_t groupsWithViolations;

    /* Peak values */
    uint64_t peakCrashedNodes;
    uint64_t peakPartitionedGroups;
} kraftFuzzMultiStats;

/* Fuzzer Result */
typedef enum {
    FUZZ_MULTI_SUCCESS = 0,
    FUZZ_MULTI_INVARIANT_VIOLATION,
    FUZZ_MULTI_TIMEOUT,
    FUZZ_MULTI_ERROR
} kraftFuzzMultiResult;

/* Operation types for replay/debugging */
typedef enum {
    FUZZ_MULTI_OP_SUBMIT_COMMAND,
    FUZZ_MULTI_OP_NODE_CRASH,
    FUZZ_MULTI_OP_NODE_RECOVER,
    FUZZ_MULTI_OP_PARTITION_GROUP,
    FUZZ_MULTI_OP_HEAL_GROUP,
    FUZZ_MULTI_OP_TICK_GROUP,
    FUZZ_MULTI_OP_GLOBAL_PARTITION,
    FUZZ_MULTI_OP_GLOBAL_HEAL,
    FUZZ_MULTI_OP_TICK_ALL
} kraftFuzzMultiOpType;

/* Get default configuration */
kraftFuzzMultiConfig kraftFuzzMultiConfigDefault(void);

/* Run Multi-Raft fuzzer */
kraftFuzzMultiResult kraftFuzzMultiRun(const kraftFuzzMultiConfig *config,
                                       kraftFuzzMultiStats *stats);

#endif /* KRAFT_FUZZ_MULTI_H */
