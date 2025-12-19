/* Kraft Feature Combination Fuzzer - Second-Order Feature Testing
 * ================================================================
 * Tests Kraft with various feature combinations enabled/disabled:
 * - Leadership Transfer
 * - ReadIndex (Linearizable Reads)
 * - Command Batching
 * - Buffer Pool
 * - Client Sessions
 * - Witness Nodes
 * - Async Commit
 * - Snapshots
 * - Multi-Raft Groups
 *
 * Each run tests a random combination of feature flags to find
 * interactions and edge cases between features.
 */

#ifndef KRAFT_FUZZ_COMBO_H
#define KRAFT_FUZZ_COMBO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Feature flags for combination testing */
typedef struct {
    bool enableLeadershipTransfer;
    bool enableReadIndex;
    bool enableBatching;
    bool enableBufferPool;
    bool enableSessions;
    bool enableWitness;
    bool enableAsyncCommit;
    bool enableSnapshots;
} kraftFuzzComboFeatures;

/* Combo Fuzzer Configuration */
typedef struct {
    uint32_t nodeCount;          /* Number of nodes in cluster */
    uint64_t maxOperations;      /* Max operations per combination */
    uint64_t maxTime;            /* Max time per combination (us) */
    uint32_t seed;               /* Random seed */
    uint32_t combinationsToTest; /* Number of feature combinations to test */

    /* Feature weights (probability each is enabled, out of 100) */
    uint32_t weightLeadershipTransfer;
    uint32_t weightReadIndex;
    uint32_t weightBatching;
    uint32_t weightBufferPool;
    uint32_t weightSessions;
    uint32_t weightWitness;
    uint32_t weightAsyncCommit;
    uint32_t weightSnapshots;

    /* Operation weights (probability out of 1000) */
    uint32_t weightSubmitCommand;
    uint32_t weightNodeCrash;
    uint32_t weightNodeRecover;
    uint32_t weightNetworkPartition;
    uint32_t weightNetworkHeal;
    uint32_t weightLeaderTransfer;
    uint32_t weightReadIndexRequest;
    uint32_t weightWitnessOp;
    uint32_t weightAsyncCommitOp;
    uint32_t weightSnapshotOp;
    uint32_t weightTick;

    /* Invariant checking */
    bool checkElectionSafety;
    bool checkLogMatching;
    bool checkLeaderCompleteness;
    bool checkFeatureInvariants;

    /* Reporting */
    bool verbose;
    uint64_t reportInterval;
} kraftFuzzComboConfig;

/* Fuzzer Statistics (per combination) */
typedef struct {
    uint64_t totalOperations;
    uint64_t commandsSubmitted;
    uint64_t nodesCrashed;
    uint64_t nodesRecovered;
    uint64_t partitionsCreated;
    uint64_t leaderTransfers;
    uint64_t readIndexRequests;
    uint64_t witnessOperations;
    uint64_t asyncCommitOperations;
    uint64_t snapshotOperations;
    uint64_t invariantChecks;
    uint64_t invariantViolations;
} kraftFuzzComboRunStats;

/* Overall Fuzzer Statistics */
typedef struct {
    uint32_t combinationsTested;
    uint32_t combinationsPassed;
    uint32_t combinationsFailed;
    kraftFuzzComboFeatures failedFeatures[32]; /* Features of failed combos */
    uint32_t failedCount;
    uint64_t totalOperations;
    uint64_t totalInvariantChecks;
    uint64_t totalInvariantViolations;
} kraftFuzzComboStats;

/* Fuzzer Result */
typedef enum {
    FUZZ_COMBO_SUCCESS = 0,
    FUZZ_COMBO_INVARIANT_VIOLATION,
    FUZZ_COMBO_TIMEOUT,
    FUZZ_COMBO_ERROR
} kraftFuzzComboResult;

/* Get default configuration */
kraftFuzzComboConfig kraftFuzzComboConfigDefault(void);

/* Generate random feature combination */
kraftFuzzComboFeatures
kraftFuzzComboRandomFeatures(uint32_t seed, const kraftFuzzComboConfig *config);

/* Run a single combination */
kraftFuzzComboResult
kraftFuzzComboRunSingle(const kraftFuzzComboConfig *config,
                        const kraftFuzzComboFeatures *features,
                        kraftFuzzComboRunStats *stats);

/* Run full combo fuzzer (multiple combinations) */
kraftFuzzComboResult kraftFuzzComboRun(const kraftFuzzComboConfig *config,
                                       kraftFuzzComboStats *stats);

/* Format feature combination as string */
int kraftFuzzComboFormatFeatures(const kraftFuzzComboFeatures *features,
                                 char *buf, size_t bufLen);

#endif /* KRAFT_FUZZ_COMBO_H */
