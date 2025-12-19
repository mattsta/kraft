#ifndef KRAFT_FUZZ_H
#define KRAFT_FUZZ_H

#include "kraftTest.h"
#include <stdbool.h>
#include <stdint.h>

/* Fuzzer configuration */
typedef struct {
    uint32_t nodeCount;     /* Number of nodes in cluster */
    uint64_t maxOperations; /* Max operations before stopping */
    uint64_t maxTime;       /* Max simulated time (microseconds) */
    uint32_t seed;          /* Random seed for reproducibility */

    /* Operation weights (probability out of 1000) */
    uint32_t weightSubmitCommand;
    uint32_t weightNodeCrash;
    uint32_t weightNodeRecover;
    uint32_t weightNetworkPartition;
    uint32_t weightNetworkHeal;
    uint32_t weightNetworkDelay;
    uint32_t weightNetworkLoss;

    /* Fault injection parameters */
    uint32_t maxCrashedNodes; /* Max simultaneous crashed nodes */
    uint32_t minRecoveryTime; /* Min time before recovering (us) */
    uint32_t maxRecoveryTime; /* Max time before recovering (us) */

    /* Invariant checking */
    bool checkElectionSafety;
    bool checkLogMatching;
    bool checkLeaderCompleteness;
    bool checkStateMachineSafety;

    /* Reporting */
    bool verbose;
    bool replayMode;         /* Output each operation for debugging */
    uint64_t reportInterval; /* Report stats every N operations */
} kraftFuzzConfig;

/* Fuzzer statistics */
typedef struct {
    uint64_t totalOperations;
    uint64_t commandsSubmitted;
    uint64_t nodesCrashed;
    uint64_t nodesRecovered;
    uint64_t partitionsCreated;
    uint64_t partitionsHealed;
    uint64_t invariantChecks;
    uint64_t invariantViolations;
    uint64_t electionsTotal;
    uint64_t messagesDelivered;
    uint64_t messagesDropped;
} kraftFuzzStats;

/* Fuzzer result */
typedef enum {
    FUZZ_SUCCESS = 0,
    FUZZ_INVARIANT_VIOLATION,
    FUZZ_TIMEOUT,
    FUZZ_ERROR
} kraftFuzzResult;

/* Fuzzer operation types */
typedef enum {
    FUZZ_OP_SUBMIT_COMMAND,
    FUZZ_OP_NODE_CRASH,
    FUZZ_OP_NODE_RECOVER,
    FUZZ_OP_NETWORK_PARTITION,
    FUZZ_OP_NETWORK_HEAL,
    FUZZ_OP_NETWORK_SET_DELAY,
    FUZZ_OP_NETWORK_SET_LOSS,
    FUZZ_OP_TICK
} kraftFuzzOpType;

/* Create default fuzzer configuration */
kraftFuzzConfig kraftFuzzConfigDefault(void);

/* Run fuzzer with given configuration */
kraftFuzzResult kraftFuzzRun(const kraftFuzzConfig *config,
                             kraftFuzzStats *stats);

/* Replay a specific sequence of operations (for debugging) */
kraftFuzzResult kraftFuzzReplay(uint32_t seed, const kraftFuzzOpType *ops,
                                uint64_t opCount, kraftFuzzStats *stats);

/* Save failing test case to file */
bool kraftFuzzSaveFailure(uint32_t seed, const kraftFuzzOpType *ops,
                          uint64_t opCount, const char *filename);

#endif /* KRAFT_FUZZ_H */
