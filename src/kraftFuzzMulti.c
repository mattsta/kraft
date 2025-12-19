/* Kraft Multi-Raft Fuzzer - Multi-Group Consensus Testing
 *
 * Runs multiple independent Raft groups with actual consensus.
 * Each group is a full testCluster running real Raft operations.
 */

#include "kraftFuzzMulti.h"
#include "../deps/datakit/src/datakit.h"
#include "kraftTest.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Maximum operation history for failure debugging */
#define MAX_OP_HISTORY_MULTI 1000

/* Maximum groups for fuzzing.
 * This is a FUZZER limit, not a production limit!
 * 64 groups x 15 nodes = 960 simulated Raft nodes is substantial for testing.
 * Production API supports up to kraftMultiRaftConfig.maxGroups (default 1024).
 * See multiRaft.h for production configuration. */
#define MAX_GROUPS 64

/* Per-group state */
typedef struct {
    testCluster *cluster;           /* The actual Raft cluster */
    uint32_t groupId;               /* Group identifier */
    bool *crashed;                  /* Per-node crash state */
    uint64_t *crashTime;            /* Time each node crashed */
    bool partitioned;               /* Is this group partitioned */
    kraftFuzzMultiGroupStats stats; /* Per-group statistics */
} multiRaftGroup;

/* Internal fuzzer state */
typedef struct {
    kraftFuzzMultiConfig config;
    kraftFuzzMultiStats stats;
    uint64_t currentTime;

    /* Groups array */
    multiRaftGroup groups[MAX_GROUPS];
    uint32_t groupCount;

    /* Global partition state */
    bool globalPartitioned;

    /* Operation history circular buffer */
    kraftFuzzMultiOpType opHistory[MAX_OP_HISTORY_MULTI];
    uint64_t opHistoryIndex;
    uint64_t opHistoryCount;

    /* Random seed state */
    uint32_t randState;
} kraftFuzzMultiState;

/* ====================================================================
 * Random number generation with state
 * ==================================================================== */
static uint32_t fuzzRand(kraftFuzzMultiState *state) {
    uint32_t x = state->randState;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    state->randState = x;
    return x;
}

static uint32_t fuzzRandRange(kraftFuzzMultiState *state, uint32_t min,
                              uint32_t max) {
    if (min >= max) {
        return min;
    }
    return min + (fuzzRand(state) % (max - min + 1));
}

static uint32_t fuzzRandWeight(kraftFuzzMultiState *state, uint32_t maxWeight) {
    return fuzzRand(state) % maxWeight;
}

/* ====================================================================
 * Configuration
 * ==================================================================== */
kraftFuzzMultiConfig kraftFuzzMultiConfigDefault(void) {
    kraftFuzzMultiConfig config = {0};

    /* Cluster configuration */
    config.groupCount = 3;    /* 3 independent groups */
    config.nodesPerGroup = 5; /* 5 nodes per group */

    /* Limits - meaningful defaults for actual fuzzing */
    config.maxOperations = 50000; /* 50k operations */
    config.maxTime = 120000000UL; /* 120 seconds */
    config.seed = (uint32_t)time(NULL);

    /* Per-group operation weights (out of 1000) */
    config.weightSubmitCommand = 250; /* 25% submit commands */
    config.weightNodeCrash = 40;      /* 4% crash */
    config.weightNodeRecover = 80;    /* 8% recover */
    config.weightPartitionGroup = 20; /* 2% partition a group */
    config.weightHealGroup = 60;      /* 6% heal a group */
    config.weightTickGroup = 200;     /* 20% tick specific group */

    /* Cross-group operation weights */
    config.weightGlobalPartition = 10; /* 1% global partition */
    config.weightGlobalHeal = 40;      /* 4% global heal */
    config.weightTickAll = 300;        /* 30% tick all groups */

    /* Constraints */
    config.maxCrashedPerGroup = 2;    /* Allow minority failures */
    config.minRecoveryTime = 500000;  /* 0.5s minimum down */
    config.maxRecoveryTime = 5000000; /* 5s maximum down */

    /* Enable all invariant checks */
    config.checkElectionSafety = true;
    config.checkLogMatching = true;
    config.checkLeaderCompleteness = true;
    config.checkStateMachineSafety = true;

    config.verbose = false;
    config.reportInterval = 1000;

    return config;
}

/* ====================================================================
 * Internal helpers
 * ==================================================================== */
static void recordOp(kraftFuzzMultiState *state, kraftFuzzMultiOpType op) {
    state->opHistory[state->opHistoryIndex] = op;
    state->opHistoryIndex = (state->opHistoryIndex + 1) % MAX_OP_HISTORY_MULTI;
    state->opHistoryCount++;
}

static const char *opTypeToString(kraftFuzzMultiOpType op) {
    static const char *names[] = {
        "SUBMIT_COMMAND",   "NODE_CRASH",  "NODE_RECOVER",
        "PARTITION_GROUP",  "HEAL_GROUP",  "TICK_GROUP",
        "GLOBAL_PARTITION", "GLOBAL_HEAL", "TICK_ALL"};
    if ((uint32_t)op < sizeof(names) / sizeof(names[0])) {
        return names[op];
    }
    return "UNKNOWN";
}

/* ====================================================================
 * Invariant checking - per group
 * ==================================================================== */
static bool checkGroupInvariants(kraftFuzzMultiState *state,
                                 uint32_t groupIdx) {
    kraftFuzzMultiConfig *config = &state->config;
    multiRaftGroup *group = &state->groups[groupIdx];
    testCluster *cluster = group->cluster;

    state->stats.invariantChecks++;

    if (config->checkElectionSafety) {
        if (!testInvariantElectionSafety(cluster)) {
            fprintf(stderr,
                    "\n[FUZZ-MULTI] INVARIANT VIOLATION: Election Safety "
                    "(Group %" PRIu32 ")\n",
                    groupIdx);
            fprintf(stderr, "  Operation: %" PRIu64 ", Time: %" PRIu64 "\n",
                    state->stats.totalOperations, state->currentTime);
            state->stats.invariantViolations++;
            state->stats.groupsWithViolations++;
            return false;
        }
    }

    if (config->checkLogMatching) {
        if (!testInvariantLogMatching(cluster)) {
            fprintf(stderr,
                    "\n[FUZZ-MULTI] INVARIANT VIOLATION: Log Matching (Group "
                    "%" PRIu32 ")\n",
                    groupIdx);
            fprintf(stderr, "  Operation: %" PRIu64 ", Time: %" PRIu64 "\n",
                    state->stats.totalOperations, state->currentTime);
            state->stats.invariantViolations++;
            state->stats.groupsWithViolations++;
            return false;
        }
    }

    if (config->checkLeaderCompleteness) {
        if (!testInvariantLeaderCompleteness(cluster)) {
            fprintf(stderr,
                    "\n[FUZZ-MULTI] INVARIANT VIOLATION: Leader Completeness "
                    "(Group %" PRIu32 ")\n",
                    groupIdx);
            fprintf(stderr, "  Operation: %" PRIu64 ", Time: %" PRIu64 "\n",
                    state->stats.totalOperations, state->currentTime);
            state->stats.invariantViolations++;
            state->stats.groupsWithViolations++;
            return false;
        }
    }

    return true;
}

static bool checkAllInvariants(kraftFuzzMultiState *state) {
    for (uint32_t i = 0; i < state->groupCount; i++) {
        if (!checkGroupInvariants(state, i)) {
            return false;
        }
    }
    return true;
}

/* ====================================================================
 * Fuzzing operations - Per-group
 * ==================================================================== */
static bool fuzzSubmitCommand(kraftFuzzMultiState *state) {
    /* Pick a random group */
    uint32_t groupIdx = fuzzRandRange(state, 0, state->groupCount - 1);
    multiRaftGroup *group = &state->groups[groupIdx];
    testCluster *cluster = group->cluster;

    uint32_t leader = testClusterGetLeader(cluster);
    if (leader == UINT32_MAX) {
        return true; /* No leader in this group, skip */
    }

    uint8_t data[256] = {0};
    uint32_t len = fuzzRandRange(state, 1, 255);
    for (uint32_t i = 0; i < len; i++) {
        data[i] = (uint8_t)fuzzRandRange(state, 32, 126);
    }

    testClusterSubmitCommand(cluster, data, len);
    group->stats.commandsSubmitted++;
    state->stats.totalCommandsSubmitted++;
    recordOp(state, FUZZ_MULTI_OP_SUBMIT_COMMAND);

    if (state->config.verbose) {
        printf("[FUZZ-MULTI] Group %" PRIu32 ": Submitted command (len=%" PRIu32
               ")\n",
               groupIdx, len);
    }

    return true;
}

static bool fuzzNodeCrash(kraftFuzzMultiState *state) {
    /* Pick a random group */
    uint32_t groupIdx = fuzzRandRange(state, 0, state->groupCount - 1);
    multiRaftGroup *group = &state->groups[groupIdx];

    /* Count currently crashed in this group */
    uint32_t crashedCount = 0;
    for (uint32_t i = 0; i < state->config.nodesPerGroup; i++) {
        if (group->crashed[i]) {
            crashedCount++;
        }
    }

    if (crashedCount >= state->config.maxCrashedPerGroup) {
        return true; /* Too many crashed in this group */
    }

    /* Find a running node to crash */
    uint32_t attempts = 0;
    while (attempts++ < 100) {
        uint32_t node =
            fuzzRandRange(state, 0, state->config.nodesPerGroup - 1);
        if (!group->crashed[node]) {
            testNodeCrash(group->cluster, node);
            group->crashed[node] = true;
            group->crashTime[node] = state->currentTime;
            group->stats.nodesCrashed++;
            state->stats.totalNodesCrashed++;
            recordOp(state, FUZZ_MULTI_OP_NODE_CRASH);

            if (state->config.verbose) {
                printf("[FUZZ-MULTI] Group %" PRIu32 ": Crashed node %" PRIu32
                       "\n",
                       groupIdx, node);
            }
            return true;
        }
    }

    return true;
}

static bool fuzzNodeRecover(kraftFuzzMultiState *state) {
    /* Pick a random group */
    uint32_t groupIdx = fuzzRandRange(state, 0, state->groupCount - 1);
    multiRaftGroup *group = &state->groups[groupIdx];

    /* Find a crashed node to recover */
    uint32_t attempts = 0;
    while (attempts++ < 100) {
        uint32_t node =
            fuzzRandRange(state, 0, state->config.nodesPerGroup - 1);
        if (group->crashed[node]) {
            uint64_t downTime = state->currentTime - group->crashTime[node];
            if (downTime >= state->config.minRecoveryTime) {
                testNodeRecover(group->cluster, node);
                group->crashed[node] = false;
                group->stats.nodesRecovered++;
                state->stats.totalNodesRecovered++;
                recordOp(state, FUZZ_MULTI_OP_NODE_RECOVER);

                if (state->config.verbose) {
                    printf("[FUZZ-MULTI] Group %" PRIu32
                           ": Recovered node %" PRIu32 " (down %" PRIu64
                           " us)\n",
                           groupIdx, node, downTime);
                }
                return true;
            }
        }
    }

    return true;
}

static bool fuzzPartitionGroup(kraftFuzzMultiState *state) {
    /* Pick a random group to partition */
    uint32_t groupIdx = fuzzRandRange(state, 0, state->groupCount - 1);
    multiRaftGroup *group = &state->groups[groupIdx];

    if (group->partitioned) {
        return true; /* Already partitioned */
    }

    if (state->config.nodesPerGroup < 3) {
        return true; /* Need at least 3 nodes */
    }

    /* Create random partition within this group */
    uint32_t split = fuzzRandRange(state, 1, state->config.nodesPerGroup - 1);
    uint32_t *minority = zmalloc(split * sizeof(uint32_t));
    uint32_t *majority =
        zmalloc((state->config.nodesPerGroup - split) * sizeof(uint32_t));
    if (!minority || !majority) {
        zfree(minority);
        zfree(majority);
        return true;
    }

    uint32_t minIdx = 0, majIdx = 0;
    for (uint32_t i = 0; i < state->config.nodesPerGroup; i++) {
        if (minIdx < split && (majIdx >= state->config.nodesPerGroup - split ||
                               fuzzRandRange(state, 0, 1) == 0)) {
            minority[minIdx++] = i;
        } else {
            majority[majIdx++] = i;
        }
    }

    testNetworkPartition(group->cluster, minority, split, majority,
                         state->config.nodesPerGroup - split);
    group->partitioned = true;
    group->stats.partitionsCreated++;
    state->stats.totalPartitions++;
    recordOp(state, FUZZ_MULTI_OP_PARTITION_GROUP);

    if (state->config.verbose) {
        printf("[FUZZ-MULTI] Group %" PRIu32 ": Partitioned %" PRIu32
               " vs %" PRIu32 " nodes\n",
               groupIdx, split, state->config.nodesPerGroup - split);
    }

    zfree(minority);
    zfree(majority);
    return true;
}

static bool fuzzHealGroup(kraftFuzzMultiState *state) {
    /* Pick a random group to heal */
    uint32_t groupIdx = fuzzRandRange(state, 0, state->groupCount - 1);
    multiRaftGroup *group = &state->groups[groupIdx];

    if (!group->partitioned) {
        return true; /* Not partitioned */
    }

    testNetworkHeal(group->cluster);
    group->partitioned = false;
    group->stats.partitionsHealed++;
    recordOp(state, FUZZ_MULTI_OP_HEAL_GROUP);

    if (state->config.verbose) {
        printf("[FUZZ-MULTI] Group %" PRIu32 ": Healed partition\n", groupIdx);
    }

    return true;
}

static bool fuzzTickGroup(kraftFuzzMultiState *state) {
    /* Pick a random group to tick */
    uint32_t groupIdx = fuzzRandRange(state, 0, state->groupCount - 1);
    multiRaftGroup *group = &state->groups[groupIdx];

    uint64_t tickSize = fuzzRandRange(state, 1000, 50000); /* 1-50ms */
    testClusterTick(group->cluster, tickSize);
    state->stats.totalTicks++;
    recordOp(state, FUZZ_MULTI_OP_TICK_GROUP);

    return true;
}

/* ====================================================================
 * Fuzzing operations - Cross-group
 * ==================================================================== */
static bool fuzzGlobalPartition(kraftFuzzMultiState *state) {
    if (state->globalPartitioned) {
        return true; /* Already globally partitioned */
    }

    /* Partition ALL groups with the same split pattern */
    for (uint32_t g = 0; g < state->groupCount; g++) {
        multiRaftGroup *group = &state->groups[g];
        if (group->partitioned) {
            continue;
        }

        if (state->config.nodesPerGroup < 3) {
            continue;
        }

        uint32_t split =
            fuzzRandRange(state, 1, state->config.nodesPerGroup - 1);
        uint32_t *minority = zmalloc(split * sizeof(uint32_t));
        uint32_t *majority =
            zmalloc((state->config.nodesPerGroup - split) * sizeof(uint32_t));
        if (!minority || !majority) {
            zfree(minority);
            zfree(majority);
            continue;
        }

        uint32_t minIdx = 0, majIdx = 0;
        for (uint32_t i = 0; i < state->config.nodesPerGroup; i++) {
            if (minIdx < split &&
                (majIdx >= state->config.nodesPerGroup - split ||
                 fuzzRandRange(state, 0, 1) == 0)) {
                minority[minIdx++] = i;
            } else {
                majority[majIdx++] = i;
            }
        }

        testNetworkPartition(group->cluster, minority, split, majority,
                             state->config.nodesPerGroup - split);
        group->partitioned = true;
        group->stats.partitionsCreated++;
        state->stats.totalPartitions++;

        zfree(minority);
        zfree(majority);
    }

    state->globalPartitioned = true;
    state->stats.globalPartitions++;
    recordOp(state, FUZZ_MULTI_OP_GLOBAL_PARTITION);

    if (state->config.verbose) {
        printf("[FUZZ-MULTI] GLOBAL PARTITION: All groups partitioned\n");
    }

    return true;
}

static bool fuzzGlobalHeal(kraftFuzzMultiState *state) {
    if (!state->globalPartitioned) {
        return true;
    }

    /* Heal ALL groups */
    for (uint32_t g = 0; g < state->groupCount; g++) {
        multiRaftGroup *group = &state->groups[g];
        if (group->partitioned) {
            testNetworkHeal(group->cluster);
            group->partitioned = false;
            group->stats.partitionsHealed++;
        }
    }

    state->globalPartitioned = false;
    state->stats.globalHeals++;
    recordOp(state, FUZZ_MULTI_OP_GLOBAL_HEAL);

    if (state->config.verbose) {
        printf("[FUZZ-MULTI] GLOBAL HEAL: All partitions healed\n");
    }

    return true;
}

static bool fuzzTickAll(kraftFuzzMultiState *state) {
    uint64_t tickSize = fuzzRandRange(state, 1000, 50000); /* 1-50ms */

    for (uint32_t g = 0; g < state->groupCount; g++) {
        testClusterTick(state->groups[g].cluster, tickSize);
    }

    state->currentTime += tickSize;
    state->stats.totalTicks++;
    recordOp(state, FUZZ_MULTI_OP_TICK_ALL);

    return true;
}

/* ====================================================================
 * Reporting
 * ==================================================================== */
static void printStats(const kraftFuzzMultiState *state) {
    const kraftFuzzMultiStats *s = &state->stats;

    printf("[FUZZ-MULTI] Ops: %" PRIu64 " | Cmds: %" PRIu64
           " | Crashes: %" PRIu64 " | Recoveries: %" PRIu64 "\n",
           s->totalOperations, s->totalCommandsSubmitted, s->totalNodesCrashed,
           s->totalNodesRecovered);
    printf("             Elections: %" PRIu64 " | Partitions: %" PRIu64
           " (%" PRIu64 " global) | Ticks: %" PRIu64 "\n",
           s->totalElections, s->totalPartitions, s->globalPartitions,
           s->totalTicks);
    printf("             Invariants: %" PRIu64 "/%" PRIu64 " passed\n",
           s->invariantChecks - s->invariantViolations, s->invariantChecks);

    /* Per-group summary */
    printf("             Per-group leaders: ");
    for (uint32_t g = 0; g < state->groupCount; g++) {
        uint32_t leader = testClusterGetLeader(state->groups[g].cluster);
        if (leader == UINT32_MAX) {
            printf("[G%" PRIu32 ":NONE] ", g);
        } else {
            printf("[G%" PRIu32 ":N%" PRIu32 "] ", g, leader);
        }
    }
    printf("\n");
}

/* ====================================================================
 * Main fuzzing loop
 * ==================================================================== */
kraftFuzzMultiResult kraftFuzzMultiRun(const kraftFuzzMultiConfig *config,
                                       kraftFuzzMultiStats *stats) {
    kraftFuzzMultiState state = {0};
    state.config = *config;
    state.randState = config->seed;
    state.groupCount = config->groupCount;

    if (state.groupCount > MAX_GROUPS) {
        fprintf(stderr,
                "[FUZZ-MULTI] Error: groupCount %" PRIu32
                " exceeds MAX_GROUPS %d\n",
                state.groupCount, MAX_GROUPS);
        return FUZZ_MULTI_ERROR;
    }

    printf("=======================================================\n");
    printf("   Kraft Multi-Raft Fuzzer                             \n");
    printf("=======================================================\n");
    printf("[FUZZ-MULTI] Config: %" PRIu32 " groups x %" PRIu32
           " nodes = %" PRIu32 " total nodes\n",
           config->groupCount, config->nodesPerGroup,
           config->groupCount * config->nodesPerGroup);
    printf("[FUZZ-MULTI] Max ops: %" PRIu64 ", Max time: %" PRIu64
           " us, Seed: %" PRIu32 "\n",
           config->maxOperations, config->maxTime, config->seed);

    /* Initialize RNG */
    testSeed(config->seed);

    /* Create all groups */
    printf("[FUZZ-MULTI] Creating %" PRIu32 " Raft groups...\n",
           state.groupCount);
    for (uint32_t g = 0; g < state.groupCount; g++) {
        multiRaftGroup *group = &state.groups[g];
        group->groupId = g;

        group->cluster = testClusterNew(config->nodesPerGroup);
        if (!group->cluster) {
            fprintf(stderr,
                    "[FUZZ-MULTI] Failed to create cluster for group %" PRIu32
                    "\n",
                    g);
            /* Cleanup already created */
            for (uint32_t j = 0; j < g; j++) {
                testClusterFree(state.groups[j].cluster);
                zfree(state.groups[j].crashed);
                zfree(state.groups[j].crashTime);
            }
            return FUZZ_MULTI_ERROR;
        }

        group->crashed = zcalloc(config->nodesPerGroup, sizeof(bool));
        group->crashTime = zcalloc(config->nodesPerGroup, sizeof(uint64_t));
        if (!group->crashed || !group->crashTime) {
            fprintf(stderr,
                    "[FUZZ-MULTI] Failed to allocate crash tracking for group "
                    "%" PRIu32 "\n",
                    g);
            testClusterFree(group->cluster);
            zfree(group->crashed);
            zfree(group->crashTime);
            for (uint32_t j = 0; j < g; j++) {
                testClusterFree(state.groups[j].cluster);
                zfree(state.groups[j].crashed);
                zfree(state.groups[j].crashTime);
            }
            return FUZZ_MULTI_ERROR;
        }

        group->partitioned = false;
        memset(&group->stats, 0, sizeof(group->stats));

        testClusterStart(group->cluster);
    }

    /* Wait for initial leader elections in all groups */
    printf("[FUZZ-MULTI] Waiting for leader elections...\n");
    for (uint32_t g = 0; g < state.groupCount; g++) {
        testClusterRunUntilLeaderElected(state.groups[g].cluster, 5000000UL);
        uint32_t leader = testClusterGetLeader(state.groups[g].cluster);
        printf("[FUZZ-MULTI] Group %" PRIu32 ": Leader = Node %" PRIu32 "\n", g,
               leader);
    }

    /* Sync time across groups */
    state.currentTime = 0;
    for (uint32_t g = 0; g < state.groupCount; g++) {
        if (state.groups[g].cluster->currentTime > state.currentTime) {
            state.currentTime = state.groups[g].cluster->currentTime;
        }
    }

    printf("[FUZZ-MULTI] Starting fuzzing at time %" PRIu64 " us\n",
           state.currentTime);
    printf("=======================================================\n\n");

    kraftFuzzMultiResult result = FUZZ_MULTI_SUCCESS;
    kraftFuzzMultiOpType lastOp = FUZZ_MULTI_OP_TICK_ALL;

    /* Main fuzzing loop */
    while (state.stats.totalOperations < config->maxOperations &&
           state.currentTime < config->maxTime) {
        /* Choose random operation based on weights */
        uint32_t roll = fuzzRandWeight(&state, 1000);
        uint32_t cumulative = 0;

#define CHECK_OP(weight, func, optype)                                         \
    cumulative += config->weight;                                              \
    if (roll < cumulative) {                                                   \
        if (!func(&state))                                                     \
            goto cleanup;                                                      \
        lastOp = optype;                                                       \
        goto op_done;                                                          \
    }

        CHECK_OP(weightSubmitCommand, fuzzSubmitCommand,
                 FUZZ_MULTI_OP_SUBMIT_COMMAND)
        CHECK_OP(weightNodeCrash, fuzzNodeCrash, FUZZ_MULTI_OP_NODE_CRASH)
        CHECK_OP(weightNodeRecover, fuzzNodeRecover, FUZZ_MULTI_OP_NODE_RECOVER)
        CHECK_OP(weightPartitionGroup, fuzzPartitionGroup,
                 FUZZ_MULTI_OP_PARTITION_GROUP)
        CHECK_OP(weightHealGroup, fuzzHealGroup, FUZZ_MULTI_OP_HEAL_GROUP)
        CHECK_OP(weightTickGroup, fuzzTickGroup, FUZZ_MULTI_OP_TICK_GROUP)
        CHECK_OP(weightGlobalPartition, fuzzGlobalPartition,
                 FUZZ_MULTI_OP_GLOBAL_PARTITION)
        CHECK_OP(weightGlobalHeal, fuzzGlobalHeal, FUZZ_MULTI_OP_GLOBAL_HEAL)
        CHECK_OP(weightTickAll, fuzzTickAll, FUZZ_MULTI_OP_TICK_ALL)

        /* Default: tick all */
        if (!fuzzTickAll(&state)) {
            goto cleanup;
        }
        lastOp = FUZZ_MULTI_OP_TICK_ALL;

#undef CHECK_OP

    op_done:
        state.stats.totalOperations++;

        /* Update aggregate statistics from all groups */
        state.stats.totalElections = 0;
        state.stats.totalMessagesDelivered = 0;
        state.stats.totalMessagesDropped = 0;
        for (uint32_t g = 0; g < state.groupCount; g++) {
            testCluster *cluster = state.groups[g].cluster;
            state.stats.totalElections += cluster->totalElections;
            state.stats.totalMessagesDelivered +=
                cluster->network.totalDelivered;
            state.stats.totalMessagesDropped += cluster->network.totalDropped;
        }

        /* Update current time from all groups */
        for (uint32_t g = 0; g < state.groupCount; g++) {
            if (state.groups[g].cluster->currentTime > state.currentTime) {
                state.currentTime = state.groups[g].cluster->currentTime;
            }
        }

        /* Track peak values */
        uint64_t currentCrashed = 0;
        uint64_t currentPartitioned = 0;
        for (uint32_t g = 0; g < state.groupCount; g++) {
            for (uint32_t n = 0; n < config->nodesPerGroup; n++) {
                if (state.groups[g].crashed[n]) {
                    currentCrashed++;
                }
            }
            if (state.groups[g].partitioned) {
                currentPartitioned++;
            }
        }
        if (currentCrashed > state.stats.peakCrashedNodes) {
            state.stats.peakCrashedNodes = currentCrashed;
        }
        if (currentPartitioned > state.stats.peakPartitionedGroups) {
            state.stats.peakPartitionedGroups = currentPartitioned;
        }

        /* Check invariants periodically */
        if (state.stats.totalOperations % 100 == 0) {
            if (!checkAllInvariants(&state)) {
                result = FUZZ_MULTI_INVARIANT_VIOLATION;
                goto cleanup;
            }
        }

        /* Print stats periodically */
        if (config->reportInterval > 0 &&
            state.stats.totalOperations % config->reportInterval == 0) {
            printStats(&state);
        }
    }

    /* Final invariant check */
    if (!checkAllInvariants(&state)) {
        result = FUZZ_MULTI_INVARIANT_VIOLATION;
    }

    if (state.currentTime >= config->maxTime) {
        result = FUZZ_MULTI_TIMEOUT;
    }

cleanup:
    printf("\n=======================================================\n");
    printf("[FUZZ-MULTI] Fuzzing complete\n");
    printStats(&state);

    if (result == FUZZ_MULTI_INVARIANT_VIOLATION) {
        printf("[FUZZ-MULTI] FAILURE - Invariant violation detected!\n");
        printf("[FUZZ-MULTI] Reproduce with: -s %" PRIu32 " -g %" PRIu32
               " -n %" PRIu32 "\n",
               config->seed, config->groupCount, config->nodesPerGroup);
    } else if (result == FUZZ_MULTI_SUCCESS) {
        printf("[FUZZ-MULTI] SUCCESS - All %" PRIu64 " operations passed!\n",
               state.stats.totalOperations);
    } else if (result == FUZZ_MULTI_TIMEOUT) {
        printf("[FUZZ-MULTI] TIMEOUT - Reached time limit (no violations)\n");
    }

    printf("=======================================================\n");

    /* Copy stats back */
    if (stats) {
        *stats = state.stats;
    }

    /* Cleanup all groups */
    for (uint32_t g = 0; g < state.groupCount; g++) {
        testClusterFree(state.groups[g].cluster);
        zfree(state.groups[g].crashed);
        zfree(state.groups[g].crashTime);
    }

    return result;
}
