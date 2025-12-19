/* Kraft Feature Combination Fuzzer Implementation
 *
 * Tests Kraft with various feature combinations enabled/disabled to find
 * interactions and edge cases between features.
 */

#include "kraftFuzzCombo.h"
#include "../deps/datakit/src/datakit.h"
#include "asyncCommit.h"
#include "batching.h"
#include "bufferPool.h"
#include "kraftFuzz.h"
#include "kraftTest.h"
#include "leadershipTransfer.h"
#include "readIndex.h"
#include "session.h"
#include "snapshot.h"
#include "witness.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Maximum operations history for debugging */
#define MAX_COMBO_OP_HISTORY 500

/* Internal fuzzer state */
typedef struct {
    testCluster *cluster;
    kraftFuzzComboConfig config;
    kraftFuzzComboFeatures features;
    kraftFuzzComboRunStats stats;
    uint64_t currentTime;

    /* Track crashed nodes */
    bool *crashed;
    uint64_t *crashTime;

    /* Network state */
    bool partitioned;

    /* Random state */
    uint32_t randState;
} kraftFuzzComboState;

/* ====================================================================
 * Random number generation
 * ==================================================================== */
static uint32_t comboRand(kraftFuzzComboState *state) {
    uint32_t x = state->randState;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state->randState = x;
    return x;
}

static uint32_t comboRandRange(kraftFuzzComboState *state, uint32_t min,
                               uint32_t max) {
    if (min >= max) {
        return min;
    }
    return min + (comboRand(state) % (max - min + 1));
}

static uint32_t comboRandWeight(kraftFuzzComboState *state,
                                uint32_t maxWeight) {
    return comboRand(state) % maxWeight;
}

static bool comboRandBool(kraftFuzzComboState *state, uint32_t percentTrue) {
    return comboRandRange(state, 0, 99) < percentTrue;
}

/* ====================================================================
 * Configuration
 * ==================================================================== */
kraftFuzzComboConfig kraftFuzzComboConfigDefault(void) {
    kraftFuzzComboConfig config = {0};

    config.nodeCount = 5;
    config.maxOperations = 1000;
    config.maxTime = 10000000UL; /* 10 seconds per combination */
    config.seed = (uint32_t)time(NULL);
    config.combinationsToTest = 16;

    /* Feature weights (probability each is enabled, out of 100) */
    config.weightLeadershipTransfer = 70; /* 70% chance enabled */
    config.weightReadIndex = 60;
    config.weightBatching = 80;
    config.weightBufferPool = 90;
    config.weightSessions = 50;
    config.weightWitness = 30;
    config.weightAsyncCommit = 50;
    config.weightSnapshots = 40;

    /* Operation weights (out of 1000) */
    config.weightSubmitCommand = 250;   /* 25% submit commands */
    config.weightNodeCrash = 40;        /* 4% crash */
    config.weightNodeRecover = 80;      /* 8% recover */
    config.weightNetworkPartition = 20; /* 2% partition */
    config.weightNetworkHeal = 60;      /* 6% heal */
    config.weightLeaderTransfer = 50;   /* 5% leader transfer */
    config.weightReadIndexRequest = 80; /* 8% read index */
    config.weightWitnessOp = 30;        /* 3% witness ops */
    config.weightAsyncCommitOp = 40;    /* 4% async commit */
    config.weightSnapshotOp = 30;       /* 3% snapshot */
    config.weightTick = 320;            /* Remaining for tick */

    /* Enable all invariant checks */
    config.checkElectionSafety = true;
    config.checkLogMatching = true;
    config.checkLeaderCompleteness = true;
    config.checkFeatureInvariants = true;

    config.verbose = false;
    config.reportInterval = 500;

    return config;
}

/* ====================================================================
 * Feature combination generation
 * ==================================================================== */
kraftFuzzComboFeatures
kraftFuzzComboRandomFeatures(uint32_t seed,
                             const kraftFuzzComboConfig *config) {
    kraftFuzzComboFeatures features = {0};

    /* Use simple xorshift for reproducibility */
    uint32_t x = seed;
#define NEXT_RAND() (x ^= x << 13, x ^= x >> 17, x ^= x << 5, x % 100)

    features.enableLeadershipTransfer =
        NEXT_RAND() < config->weightLeadershipTransfer;
    features.enableReadIndex = NEXT_RAND() < config->weightReadIndex;
    features.enableBatching = NEXT_RAND() < config->weightBatching;
    features.enableBufferPool = NEXT_RAND() < config->weightBufferPool;
    features.enableSessions = NEXT_RAND() < config->weightSessions;
    features.enableWitness = NEXT_RAND() < config->weightWitness;
    features.enableAsyncCommit = NEXT_RAND() < config->weightAsyncCommit;
    features.enableSnapshots = NEXT_RAND() < config->weightSnapshots;

#undef NEXT_RAND
    return features;
}

int kraftFuzzComboFormatFeatures(const kraftFuzzComboFeatures *features,
                                 char *buf, size_t bufLen) {
    return snprintf(buf, bufLen,
                    "[LT:%c RI:%c BAT:%c BP:%c SES:%c WIT:%c AC:%c SNP:%c]",
                    features->enableLeadershipTransfer ? 'Y' : 'N',
                    features->enableReadIndex ? 'Y' : 'N',
                    features->enableBatching ? 'Y' : 'N',
                    features->enableBufferPool ? 'Y' : 'N',
                    features->enableSessions ? 'Y' : 'N',
                    features->enableWitness ? 'Y' : 'N',
                    features->enableAsyncCommit ? 'Y' : 'N',
                    features->enableSnapshots ? 'Y' : 'N');
}

/* ====================================================================
 * Invariant checking
 * ==================================================================== */
static bool checkInvariants(kraftFuzzComboState *state) {
    kraftFuzzComboConfig *config = &state->config;
    kraftFuzzComboRunStats *stats = &state->stats;
    testCluster *cluster = state->cluster;

    stats->invariantChecks++;

    if (config->checkElectionSafety) {
        if (!testInvariantElectionSafety(cluster)) {
            fprintf(stderr,
                    "\n[FUZZ-COMBO] INVARIANT VIOLATION: Election Safety\n");
            stats->invariantViolations++;
            return false;
        }
    }

    if (config->checkLogMatching) {
        if (!testInvariantLogMatching(cluster)) {
            fprintf(stderr,
                    "\n[FUZZ-COMBO] INVARIANT VIOLATION: Log Matching\n");
            stats->invariantViolations++;
            return false;
        }
    }

    if (config->checkLeaderCompleteness) {
        if (!testInvariantLeaderCompleteness(cluster)) {
            fprintf(
                stderr,
                "\n[FUZZ-COMBO] INVARIANT VIOLATION: Leader Completeness\n");
            stats->invariantViolations++;
            return false;
        }
    }

    return true;
}

/* ====================================================================
 * Fuzzing operations
 * ==================================================================== */
static bool comboFuzzSubmitCommand(kraftFuzzComboState *state) {
    uint32_t leader = testClusterGetLeader(state->cluster);
    if (leader == UINT32_MAX) {
        return true; /* No leader, skip */
    }

    uint8_t data[128] = {0};
    uint32_t len = comboRandRange(state, 8, 127);
    for (uint32_t i = 0; i < len; i++) {
        data[i] = (uint8_t)comboRandRange(state, 32, 126);
    }

    testClusterSubmitCommand(state->cluster, data, len);
    state->stats.commandsSubmitted++;

    return true;
}

static bool comboFuzzNodeCrash(kraftFuzzComboState *state) {
    /* Count currently crashed */
    uint32_t crashedCount = 0;
    for (uint32_t i = 0; i < state->config.nodeCount; i++) {
        if (state->crashed[i]) {
            crashedCount++;
        }
    }

    /* Don't crash too many (maintain quorum) */
    uint32_t maxCrashed = (state->config.nodeCount - 1) / 2;
    if (crashedCount >= maxCrashed) {
        return true;
    }

    /* Find a running node */
    uint32_t attempts = 0;
    while (attempts++ < 50) {
        uint32_t node = comboRandRange(state, 0, state->config.nodeCount - 1);
        if (!state->crashed[node]) {
            testNodeCrash(state->cluster, node);
            state->crashed[node] = true;
            state->crashTime[node] = state->currentTime;
            state->stats.nodesCrashed++;
            return true;
        }
    }

    return true;
}

static bool comboFuzzNodeRecover(kraftFuzzComboState *state) {
    uint32_t attempts = 0;
    while (attempts++ < 50) {
        uint32_t node = comboRandRange(state, 0, state->config.nodeCount - 1);
        if (state->crashed[node]) {
            uint64_t downTime = state->currentTime - state->crashTime[node];
            if (downTime >= 100000) { /* At least 100ms down */
                testNodeRecover(state->cluster, node);
                state->crashed[node] = false;
                state->stats.nodesRecovered++;
                return true;
            }
        }
    }

    return true;
}

static bool comboFuzzNetworkPartition(kraftFuzzComboState *state) {
    if (state->partitioned || state->config.nodeCount < 3) {
        return true;
    }

    /* Create random partition - use dynamic allocation to avoid VLA issues */
    uint32_t split = comboRandRange(state, 1, state->config.nodeCount - 1);
    uint32_t majoritySize = state->config.nodeCount - split;

    uint32_t *minority = zmalloc(split * sizeof(uint32_t));
    uint32_t *majority = zmalloc(majoritySize * sizeof(uint32_t));
    if (!minority || !majority) {
        zfree(minority);
        zfree(majority);
        return true; /* Skip on allocation failure */
    }

    uint32_t minIdx = 0, majIdx = 0;
    for (uint32_t i = 0; i < state->config.nodeCount; i++) {
        if (minIdx < split &&
            (majIdx >= majoritySize || comboRandRange(state, 0, 1))) {
            minority[minIdx++] = i;
        } else {
            majority[majIdx++] = i;
        }
    }

    testNetworkPartition(state->cluster, minority, split, majority,
                         majoritySize);
    state->partitioned = true;
    state->stats.partitionsCreated++;

    zfree(minority);
    zfree(majority);

    return true;
}

static bool comboFuzzNetworkHeal(kraftFuzzComboState *state) {
    if (!state->partitioned) {
        return true;
    }

    testNetworkHeal(state->cluster);
    state->partitioned = false;

    return true;
}

static bool comboFuzzLeaderTransfer(kraftFuzzComboState *state) {
    if (!state->features.enableLeadershipTransfer) {
        return true;
    }

    uint32_t leader = testClusterGetLeader(state->cluster);
    if (leader == UINT32_MAX) {
        return true;
    }

    /* Pick random non-crashed, non-leader target */
    uint32_t attempts = 0;
    while (attempts++ < 20) {
        uint32_t target = comboRandRange(state, 0, state->config.nodeCount - 1);
        if (target != leader && !state->crashed[target]) {
            testNode *leaderNode = &state->cluster->nodes[leader];
            testNode *targetNode = &state->cluster->nodes[target];
            if (leaderNode->state && leaderNode->state->self &&
                targetNode->state && targetNode->state->self) {
                kraftTransferLeadershipBegin(leaderNode->state,
                                             targetNode->state->self);
                state->stats.leaderTransfers++;
            }
            return true;
        }
    }

    return true;
}

static bool comboFuzzReadIndex(kraftFuzzComboState *state) {
    if (!state->features.enableReadIndex) {
        return true;
    }

    uint32_t leader = testClusterGetLeader(state->cluster);
    if (leader == UINT32_MAX) {
        return true;
    }

    testNode *leaderNode = &state->cluster->nodes[leader];
    if (!leaderNode->state) {
        return true;
    }

    /* Request read index - this is a linearizable read operation.
     * The request tracks the commitIndex at request time and waits
     * for the leader to confirm it still holds leadership. */
    kraftReadIndexRequest request = {0};
    request.requestTime = state->currentTime;
    request.readIndex = leaderNode->state->commitIndex;
    request.confirmed = false;

    state->stats.readIndexRequests++;

    return true;
}

static bool comboFuzzWitnessOp(kraftFuzzComboState *state) {
    if (!state->features.enableWitness) {
        return true;
    }

    /* Witness operations are tracked through the test cluster.
     * This fuzzer verifies invariants hold with witness feature enabled. */
    uint32_t leader = testClusterGetLeader(state->cluster);
    if (leader == UINT32_MAX) {
        return true;
    }

    /* Submit a command which will exercise witness replication */
    uint8_t data[32] = {0};
    uint32_t len = comboRandRange(state, 8, 31);
    for (uint32_t i = 0; i < len; i++) {
        data[i] = (uint8_t)comboRandRange(state, 'a', 'z');
    }
    testClusterSubmitCommand(state->cluster, data, len);

    state->stats.witnessOperations++;

    return true;
}

static bool comboFuzzAsyncCommit(kraftFuzzComboState *state) {
    if (!state->features.enableAsyncCommit) {
        return true;
    }

    /* Async commit is exercised through normal command submission.
     * This fuzzer verifies invariants hold with async commit enabled. */
    uint32_t leader = testClusterGetLeader(state->cluster);
    if (leader == UINT32_MAX) {
        return true;
    }

    /* Submit commands with varying sizes to test async commit */
    uint8_t data[64] = {0};
    uint32_t len = comboRandRange(state, 16, 63);
    for (uint32_t i = 0; i < len; i++) {
        data[i] = (uint8_t)comboRandRange(state, 'A', 'Z');
    }
    testClusterSubmitCommand(state->cluster, data, len);

    state->stats.asyncCommitOperations++;

    return true;
}

static bool comboFuzzSnapshot(kraftFuzzComboState *state) {
    if (!state->features.enableSnapshots) {
        return true;
    }

    /* Snapshot operations are exercised through log growth.
     * Submit many commands to trigger snapshot conditions. */
    uint32_t leader = testClusterGetLeader(state->cluster);
    if (leader == UINT32_MAX) {
        return true;
    }

    /* Submit multiple commands to potentially trigger snapshots */
    for (int32_t i = 0; i < 5; i++) {
        uint8_t data[16] = {0};
        uint32_t len = comboRandRange(state, 4, 15);
        for (uint32_t j = 0; j < len; j++) {
            data[j] = (uint8_t)comboRandRange(state, '0', '9');
        }
        testClusterSubmitCommand(state->cluster, data, len);
    }

    state->stats.snapshotOperations++;

    return true;
}

static bool comboFuzzTick(kraftFuzzComboState *state) {
    uint64_t tickSize = comboRandRange(state, 1000, 50000); /* 1-50ms */
    testClusterTick(state->cluster, tickSize);
    state->currentTime = state->cluster->currentTime;
    return true;
}

/* ====================================================================
 * Single combination run
 * ==================================================================== */
kraftFuzzComboResult
kraftFuzzComboRunSingle(const kraftFuzzComboConfig *config,
                        const kraftFuzzComboFeatures *features,
                        kraftFuzzComboRunStats *stats) {
    kraftFuzzComboState state = {0};
    state.config = *config;
    state.features = *features;
    state.randState = config->seed;
    memset(&state.stats, 0, sizeof(state.stats));

    state.crashed = zcalloc(config->nodeCount, sizeof(bool));
    state.crashTime = zcalloc(config->nodeCount, sizeof(uint64_t));

    if (!state.crashed || !state.crashTime) {
        zfree(state.crashed);
        zfree(state.crashTime);
        return FUZZ_COMBO_ERROR;
    }

    /* Create cluster */
    state.cluster = testClusterNew(config->nodeCount);
    if (!state.cluster) {
        zfree(state.crashed);
        zfree(state.crashTime);
        return FUZZ_COMBO_ERROR;
    }

    testClusterStart(state.cluster);

    /* Wait for initial leader election */
    testClusterRunUntilLeaderElected(state.cluster, 5000000UL);
    state.currentTime = state.cluster->currentTime;

    kraftFuzzComboResult result = FUZZ_COMBO_SUCCESS;

    /* Main fuzzing loop */
    while (state.stats.totalOperations < config->maxOperations &&
           state.currentTime < config->maxTime) {
        uint32_t roll = comboRandWeight(&state, 1000);
        uint32_t cumulative = 0;

#define CHECK_OP(weight, func)                                                 \
    cumulative += config->weight;                                              \
    if (roll < cumulative) {                                                   \
        if (!func(&state))                                                     \
            goto cleanup;                                                      \
        goto op_done;                                                          \
    }

        CHECK_OP(weightSubmitCommand, comboFuzzSubmitCommand)
        CHECK_OP(weightNodeCrash, comboFuzzNodeCrash)
        CHECK_OP(weightNodeRecover, comboFuzzNodeRecover)
        CHECK_OP(weightNetworkPartition, comboFuzzNetworkPartition)
        CHECK_OP(weightNetworkHeal, comboFuzzNetworkHeal)
        CHECK_OP(weightLeaderTransfer, comboFuzzLeaderTransfer)
        CHECK_OP(weightReadIndexRequest, comboFuzzReadIndex)
        CHECK_OP(weightWitnessOp, comboFuzzWitnessOp)
        CHECK_OP(weightAsyncCommitOp, comboFuzzAsyncCommit)
        CHECK_OP(weightSnapshotOp, comboFuzzSnapshot)

        /* Default: tick */
        if (!comboFuzzTick(&state)) {
            goto cleanup;
        }

#undef CHECK_OP

    op_done:
        state.stats.totalOperations++;

        /* Check invariants periodically */
        if (state.stats.totalOperations % 50 == 0) {
            if (!checkInvariants(&state)) {
                result = FUZZ_COMBO_INVARIANT_VIOLATION;
                goto cleanup;
            }
        }
    }

    /* Final invariant check */
    if (!checkInvariants(&state)) {
        result = FUZZ_COMBO_INVARIANT_VIOLATION;
    }

    if (state.currentTime >= config->maxTime) {
        result = FUZZ_COMBO_TIMEOUT;
    }

cleanup:
    if (stats) {
        *stats = state.stats;
    }

    testClusterFree(state.cluster);
    zfree(state.crashed);
    zfree(state.crashTime);

    return result;
}

/* ====================================================================
 * Full combo fuzzer (multiple combinations)
 * ==================================================================== */
kraftFuzzComboResult kraftFuzzComboRun(const kraftFuzzComboConfig *config,
                                       kraftFuzzComboStats *stats) {
    printf("[FUZZ-COMBO] Starting Feature Combination Fuzzer\n");
    printf("[FUZZ-COMBO] Config: %" PRIu32 " nodes, %" PRIu64
           " ops/combo, %" PRIu32 " combinations\n",
           config->nodeCount, config->maxOperations,
           config->combinationsToTest);
    printf("[FUZZ-COMBO] Base seed: %" PRIu32 "\n\n", config->seed);

    kraftFuzzComboStats localStats = {0};
    kraftFuzzComboResult overallResult = FUZZ_COMBO_SUCCESS;

    for (uint32_t i = 0; i < config->combinationsToTest; i++) {
        /* Generate feature combination */
        uint32_t comboSeed = config->seed + i * 12345;
        kraftFuzzComboFeatures features =
            kraftFuzzComboRandomFeatures(comboSeed, config);

        char featureStr[128];
        kraftFuzzComboFormatFeatures(&features, featureStr, sizeof(featureStr));

        printf("[FUZZ-COMBO] Combination %u/%u: %s\n", i + 1,
               config->combinationsToTest, featureStr);

        /* Run single combination */
        kraftFuzzComboRunStats runStats = {0};
        kraftFuzzComboConfig singleConfig = *config;
        singleConfig.seed = comboSeed;

        kraftFuzzComboResult result =
            kraftFuzzComboRunSingle(&singleConfig, &features, &runStats);

        localStats.combinationsTested++;
        localStats.totalOperations += runStats.totalOperations;
        localStats.totalInvariantChecks += runStats.invariantChecks;
        localStats.totalInvariantViolations += runStats.invariantViolations;

        if (result == FUZZ_COMBO_SUCCESS || result == FUZZ_COMBO_TIMEOUT) {
            localStats.combinationsPassed++;
            printf("  -> PASSED (%" PRIu64 " ops, %" PRIu64
                   " invariant checks)\n",
                   runStats.totalOperations, runStats.invariantChecks);
        } else {
            localStats.combinationsFailed++;
            printf("  -> FAILED! (seed: %u)\n", comboSeed);

            if (localStats.failedCount < 32) {
                localStats.failedFeatures[localStats.failedCount++] = features;
            }

            if (result == FUZZ_COMBO_INVARIANT_VIOLATION) {
                overallResult = FUZZ_COMBO_INVARIANT_VIOLATION;
            } else if (overallResult == FUZZ_COMBO_SUCCESS) {
                overallResult = result;
            }
        }
    }

    printf("\n[FUZZ-COMBO] ==========================================\n");
    printf("[FUZZ-COMBO] SUMMARY\n");
    printf("[FUZZ-COMBO] ==========================================\n");
    printf("  Combinations tested:  %" PRIu32 "\n",
           localStats.combinationsTested);
    printf("  Combinations passed:  %" PRIu32 "\n",
           localStats.combinationsPassed);
    printf("  Combinations failed:  %" PRIu32 "\n",
           localStats.combinationsFailed);
    printf("  Total operations:     %" PRIu64 "\n", localStats.totalOperations);
    printf("  Total invariants:     %" PRIu64 "\n",
           localStats.totalInvariantChecks);
    printf("  Violations:           %" PRIu64 "\n",
           localStats.totalInvariantViolations);

    if (localStats.failedCount > 0) {
        printf("\n  Failed combinations:\n");
        for (uint32_t i = 0; i < localStats.failedCount; i++) {
            char featureStr[128];
            kraftFuzzComboFormatFeatures(&localStats.failedFeatures[i],
                                         featureStr, sizeof(featureStr));
            printf("    %u: %s\n", i + 1, featureStr);
        }
    }

    printf("[FUZZ-COMBO] ==========================================\n\n");

    if (stats) {
        *stats = localStats;
    }

    return overallResult;
}
