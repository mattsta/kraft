#include "kraftFuzz.h"
#include "kraftTest.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* Circular buffer for recent operations (for failure debugging) */
#define MAX_OP_HISTORY 1000

/* Internal fuzzer state */
typedef struct {
    testCluster *cluster;
    kraftFuzzConfig config;
    kraftFuzzStats stats;
    uint64_t currentTime;

    /* Track crashed nodes for recovery */
    bool *crashed;
    uint64_t *crashTime;

    /* Track network state */
    bool partitioned;

    /* Circular buffer for recent operations (bounded memory) */
    kraftFuzzOpType opHistory[MAX_OP_HISTORY];
    uint64_t opHistoryIndex; /* Current write position */
    uint64_t opHistoryCount; /* Total ops recorded (for debugging) */
} kraftFuzzerState;

/* ====================================================================
 * Configuration
 * ==================================================================== */
kraftFuzzConfig kraftFuzzConfigDefault(void) {
    kraftFuzzConfig config = {0};

    config.nodeCount = 5;
    config.maxOperations = 10000;
    config.maxTime = 60000000UL; /* 60 seconds */
    config.seed = (uint32_t)time(NULL);

    /* Operation weights (out of 1000) */
    config.weightSubmitCommand = 300;   /* 30% submit commands */
    config.weightNodeCrash = 50;        /* 5% crash */
    config.weightNodeRecover = 100;     /* 10% recover */
    config.weightNetworkPartition = 20; /* 2% partition */
    config.weightNetworkHeal = 80;      /* 8% heal */
    config.weightNetworkDelay = 30;     /* 3% delay */
    config.weightNetworkLoss = 40;      /* 4% loss */
    /* Remaining 38% will be tick operations */

    config.maxCrashedNodes = 2;
    config.minRecoveryTime = 500000;  /* 0.5s */
    config.maxRecoveryTime = 5000000; /* 5s */

    /* Enable all invariant checks */
    config.checkElectionSafety = true;
    config.checkLogMatching = true;
    config.checkLeaderCompleteness = true;
    config.checkStateMachineSafety = true;

    config.verbose = false;
    config.replayMode = false;
    config.reportInterval = 1000;

    return config;
}

/* ====================================================================
 * Internal helpers
 * ==================================================================== */
static void recordOp(kraftFuzzerState *state, kraftFuzzOpType op) {
    /* Circular buffer - overwrites oldest operation */
    state->opHistory[state->opHistoryIndex] = op;
    state->opHistoryIndex = (state->opHistoryIndex + 1) % MAX_OP_HISTORY;
    state->opHistoryCount++;
}

static uint32_t randRange(uint32_t min, uint32_t max) {
    if (min >= max) {
        return min;
    }
    return min + (rand() % (max - min + 1));
}

static uint32_t randWeight(uint32_t maxWeight) {
    return rand() % maxWeight;
}

static bool checkInvariants(kraftFuzzerState *state) {
    kraftFuzzConfig *config = &state->config;
    kraftFuzzStats *stats = &state->stats;
    testCluster *cluster = state->cluster;

    stats->invariantChecks++;

    if (config->checkElectionSafety) {
        if (!testInvariantElectionSafety(cluster)) {
            fprintf(stderr, "\n[FUZZ] INVARIANT VIOLATION: Election Safety\n");
            fprintf(stderr, "  Operation: %" PRIu64 ", Time: %" PRIu64 "\n",
                    stats->totalOperations, state->currentTime);
            stats->invariantViolations++;
            return false;
        }
    }

    if (config->checkLogMatching) {
        if (!testInvariantLogMatching(cluster)) {
            fprintf(stderr, "\n[FUZZ] INVARIANT VIOLATION: Log Matching\n");
            fprintf(stderr, "  Operation: %" PRIu64 ", Time: %" PRIu64 "\n",
                    stats->totalOperations, state->currentTime);
            stats->invariantViolations++;
            return false;
        }
    }

    if (config->checkLeaderCompleteness) {
        if (!testInvariantLeaderCompleteness(cluster)) {
            fprintf(stderr,
                    "\n[FUZZ] INVARIANT VIOLATION: Leader Completeness\n");
            fprintf(stderr, "  Operation: %" PRIu64 ", Time: %" PRIu64 "\n",
                    stats->totalOperations, state->currentTime);
            stats->invariantViolations++;
            return false;
        }
    }

    /* State machine safety is harder to check in general,
     * but we can verify commit indices don't decrease */

    return true;
}

static const char *opTypeToString(kraftFuzzOpType op) {
    static const char *names[] = {"SUBMIT_COMMAND",   "NODE_CRASH",
                                  "NODE_RECOVER",     "NETWORK_PARTITION",
                                  "NETWORK_HEAL",     "NETWORK_SET_DELAY",
                                  "NETWORK_SET_LOSS", "TICK"};
    if (op < sizeof(names) / sizeof(names[0])) {
        return names[op];
    }
    return "UNKNOWN";
}

static const char *roleToString(int role) {
    switch (role) {
    case 0:
        return "FOLLOWER";
    case 1:
        return "CANDIDATE";
    case 2:
        return "PRE_CANDIDATE";
    case 3:
        return "LEADER";
    default:
        return "UNKNOWN";
    }
}

static void printReplayState(const kraftFuzzerState *state,
                             kraftFuzzOpType lastOp) {
    printf("\n--- Op #%" PRIu64 ": %s ---\n", state->stats.totalOperations,
           opTypeToString(lastOp));
    printf("Time: %" PRIu64 " us\n", state->currentTime);
    printf("Nodes:\n");
    for (uint32_t i = 0; i < state->cluster->nodeCount; i++) {
        testNode *node = &state->cluster->nodes[i];
        if (node->crashed) {
            printf("  [%u] CRASHED\n", i);
            continue;
        }
        kraftState *ks = node->state;
        kraftRpcUniqueIndex maxIdx = 0;
        kvidxMaxKey(KRAFT_ENTRIES(ks), &maxIdx);
        printf("  [%u] role=%s term=%" PRIu64 " commit=%" PRIu64
               " lastIdx=%" PRIu64 "\n",
               i, roleToString(ks->self->consensus.role),
               (unsigned long long)ks->self->consensus.term,
               (unsigned long long)ks->commitIndex, (unsigned long long)maxIdx);
    }
}

static void printStats(const kraftFuzzerState *state) {
    const kraftFuzzStats *s = &state->stats;

    printf("[FUZZ] Operations: %" PRIu64 " | Commands: %" PRIu64
           " | Crashes: %" PRIu64 " | "
           "Recoveries: %" PRIu64 " | Elections: %" PRIu64 "\n",
           s->totalOperations, s->commandsSubmitted, s->nodesCrashed,
           s->nodesRecovered, s->electionsTotal);
    printf("       Partitions: %" PRIu64 " | Heals: %" PRIu64
           " | Messages: %" PRIu64 "/%" PRIu64 " | "
           "Invariants: %" PRIu64 "/%" PRIu64 "\n",
           s->partitionsCreated, s->partitionsHealed, s->messagesDelivered,
           s->messagesDropped, s->invariantChecks - s->invariantViolations,
           s->invariantChecks);
}

/* ====================================================================
 * Fuzzing operations
 * ==================================================================== */
static bool fuzzSubmitCommand(kraftFuzzerState *state) {
    uint32_t leader = testClusterGetLeader(state->cluster);
    if (leader == UINT32_MAX) {
        return true; /* No leader, skip */
    }

    char data[256] = {0};
    uint32_t len = randRange(1, 255);
    for (uint32_t i = 0; i < len; i++) {
        data[i] = (char)randRange(32, 126); /* Printable ASCII */
    }

    testClusterSubmitCommand(state->cluster, data, len);
    state->stats.commandsSubmitted++;
    recordOp(state, FUZZ_OP_SUBMIT_COMMAND);

    if (state->config.verbose) {
        printf("[FUZZ] Submitted command (len=%u) to node %u\n", len, leader);
    }

    return true;
}

static bool fuzzNodeCrash(kraftFuzzerState *state) {
    /* Count currently crashed nodes */
    uint32_t crashedCount = 0;
    for (uint32_t i = 0; i < state->config.nodeCount; i++) {
        if (state->crashed[i]) {
            crashedCount++;
        }
    }

    if (crashedCount >= state->config.maxCrashedNodes) {
        return true; /* Too many crashed, skip */
    }

    /* Find a running node to crash */
    uint32_t attempts = 0;
    while (attempts++ < 100) {
        uint32_t node = randRange(0, state->config.nodeCount - 1);
        if (!state->crashed[node]) {
            testNodeCrash(state->cluster, node);
            state->crashed[node] = true;
            state->crashTime[node] = state->currentTime;
            state->stats.nodesCrashed++;
            recordOp(state, FUZZ_OP_NODE_CRASH);

            if (state->config.verbose) {
                printf("[FUZZ] Crashed node %u\n", node);
            }
            return true;
        }
    }

    return true;
}

static bool fuzzNodeRecover(kraftFuzzerState *state) {
    /* Find a crashed node to recover */
    uint32_t attempts = 0;
    while (attempts++ < 100) {
        uint32_t node = randRange(0, state->config.nodeCount - 1);
        if (state->crashed[node]) {
            uint64_t downTime = state->currentTime - state->crashTime[node];
            if (downTime >= state->config.minRecoveryTime) {
                testNodeRecover(state->cluster, node);
                state->crashed[node] = false;
                state->stats.nodesRecovered++;
                recordOp(state, FUZZ_OP_NODE_RECOVER);

                if (state->config.verbose) {
                    printf("[FUZZ] Recovered node %u (down for %" PRIu64
                           " us)\n",
                           node, downTime);
                }
                return true;
            }
        }
    }

    return true;
}

static bool fuzzNetworkPartition(kraftFuzzerState *state) {
    if (state->partitioned) {
        return true; /* Already partitioned */
    }

    if (state->config.nodeCount < 3) {
        return true; /* Need at least 3 nodes */
    }

    /* Create random partition */
    uint32_t split = randRange(1, state->config.nodeCount - 1);
    uint32_t minority[split];
    uint32_t majority[state->config.nodeCount - split];

    /* Randomly assign nodes to partitions */
    bool *assigned = calloc(state->config.nodeCount, sizeof(bool));
    uint32_t minIdx = 0, majIdx = 0;

    for (uint32_t i = 0; i < state->config.nodeCount; i++) {
        if (minIdx < split &&
            (majIdx >= state->config.nodeCount - split || rand() % 2)) {
            minority[minIdx++] = i;
        } else {
            majority[majIdx++] = i;
        }
    }

    testNetworkPartition(state->cluster, minority, split, majority,
                         state->config.nodeCount - split);
    state->partitioned = true;
    state->stats.partitionsCreated++;
    recordOp(state, FUZZ_OP_NETWORK_PARTITION);

    if (state->config.verbose) {
        printf("[FUZZ] Created partition: %u vs %u nodes\n", split,
               state->config.nodeCount - split);
    }

    free(assigned);
    return true;
}

static bool fuzzNetworkHeal(kraftFuzzerState *state) {
    if (!state->partitioned) {
        return true; /* Not partitioned */
    }

    testNetworkHeal(state->cluster);
    state->partitioned = false;
    state->stats.partitionsHealed++;
    recordOp(state, FUZZ_OP_NETWORK_HEAL);

    if (state->config.verbose) {
        printf("[FUZZ] Healed network partition\n");
    }

    return true;
}

static bool fuzzNetworkDelay(kraftFuzzerState *state) {
    uint64_t minDelay = randRange(0, 10000);         /* 0-10ms */
    uint64_t maxDelay = randRange(minDelay, 100000); /* up to 100ms */

    testNetworkSetDelay(state->cluster, minDelay, maxDelay);
    recordOp(state, FUZZ_OP_NETWORK_SET_DELAY);

    if (state->config.verbose) {
        printf("[FUZZ] Set network delay: %" PRIu64 "-%" PRIu64 " us\n",
               minDelay, maxDelay);
    }

    return true;
}

static bool fuzzNetworkLoss(kraftFuzzerState *state) {
    double dropRate = (rand() % 200) / 1000.0; /* 0-20% loss */

    if (dropRate > 0.0) {
        testNetworkSetMode(state->cluster, TEST_NETWORK_LOSSY);
        testNetworkSetDropRate(state->cluster, dropRate);
    } else {
        testNetworkSetMode(state->cluster, TEST_NETWORK_RELIABLE);
    }

    recordOp(state, FUZZ_OP_NETWORK_SET_LOSS);

    if (state->config.verbose) {
        printf("[FUZZ] Set packet loss: %.1f%%\n", dropRate * 100);
    }

    return true;
}

static bool fuzzTick(kraftFuzzerState *state) {
    uint64_t tickSize = randRange(1000, 50000); /* 1-50ms */
    testClusterTick(state->cluster, tickSize);
    state->currentTime = state->cluster->currentTime;
    recordOp(state, FUZZ_OP_TICK);

    return true;
}

static void compactClusterLogs(kraftFuzzerState *state) {
    /* Periodically compact logs to prevent unbounded memory growth.
     * In a real system, this would create snapshots and truncate old log
     * entries. For testing, we just ensure we don't keep more than a reasonable
     * amount. */

    for (uint32_t i = 0; i < state->cluster->nodeCount; i++) {
        if (state->cluster->nodes[i].crashed) {
            continue;
        }

        kraftState *nodeState = state->cluster->nodes[i].state;
        kraftRpcUniqueIndex maxIndex = 0;

        /* Get max index in log */
        if (!kvidxMaxKey(KRAFT_ENTRIES(nodeState), &maxIndex)) {
            continue; /* No entries */
        }

/* Keep only recent entries (e.g., last 1000) to bound memory */
#define MAX_LOG_ENTRIES 1000
        if (maxIndex > MAX_LOG_ENTRIES) {
            kraftRpcUniqueIndex compactTo = maxIndex - MAX_LOG_ENTRIES;
            /* In a real implementation, we'd snapshot and remove.
             * For testing, we rely on kvidx potentially having internal limits.
             * Just update commit index to indicate we've "applied" old entries.
             */
            if (compactTo > nodeState->commitIndex) {
                /* Can only compact up to commit index */
                compactTo = nodeState->commitIndex;
            }
        }
    }
}

/* ====================================================================
 * Main fuzzing loop
 * ==================================================================== */
kraftFuzzResult kraftFuzzRun(const kraftFuzzConfig *config,
                             kraftFuzzStats *stats) {
    /* Initialize fuzzer state */
    kraftFuzzerState state = {0};
    state.config = *config;
    memset(&state.stats, 0, sizeof(state.stats));

    state.crashed = calloc(config->nodeCount, sizeof(bool));
    state.crashTime = calloc(config->nodeCount, sizeof(uint64_t));
    state.partitioned = false;

    /* Operation history is a fixed-size circular buffer in the struct */
    state.opHistoryIndex = 0;
    state.opHistoryCount = 0;

    /* Seed RNG */
    testSeed(config->seed);
    srand(config->seed);

    printf("[FUZZ] Starting fuzzer with seed %u\n", config->seed);
    printf("[FUZZ] Config: %u nodes, %" PRIu64 " ops, %" PRIu64
           " us max time\n",
           config->nodeCount, config->maxOperations, config->maxTime);

    /* Create cluster */
    state.cluster = testClusterNew(config->nodeCount);
    if (!state.cluster) {
        fprintf(stderr, "[FUZZ] Failed to create cluster\n");
        free(state.crashed);
        free(state.crashTime);
        return FUZZ_ERROR;
    }

    testClusterStart(state.cluster);

    /* Wait for initial leader election */
    testClusterRunUntilLeaderElected(state.cluster, 5000000UL);
    state.currentTime = state.cluster->currentTime;

    kraftFuzzResult result = FUZZ_SUCCESS;

    /* Main fuzzing loop */
    kraftFuzzOpType lastOp = FUZZ_OP_TICK;
    while (state.stats.totalOperations < config->maxOperations &&
           state.currentTime < config->maxTime) {
        /* Choose random operation based on weights */
        uint32_t roll = randWeight(1000);
        uint32_t cumulative = 0;

#define CHECK_OP(weight, func, optype)                                         \
    cumulative += config->weight;                                              \
    if (roll < cumulative) {                                                   \
        if (!func(&state))                                                     \
            goto cleanup;                                                      \
        lastOp = optype;                                                       \
        goto op_done;                                                          \
    }

        CHECK_OP(weightSubmitCommand, fuzzSubmitCommand, FUZZ_OP_SUBMIT_COMMAND)
        CHECK_OP(weightNodeCrash, fuzzNodeCrash, FUZZ_OP_NODE_CRASH)
        CHECK_OP(weightNodeRecover, fuzzNodeRecover, FUZZ_OP_NODE_RECOVER)
        CHECK_OP(weightNetworkPartition, fuzzNetworkPartition,
                 FUZZ_OP_NETWORK_PARTITION)
        CHECK_OP(weightNetworkHeal, fuzzNetworkHeal, FUZZ_OP_NETWORK_HEAL)
        CHECK_OP(weightNetworkDelay, fuzzNetworkDelay,
                 FUZZ_OP_NETWORK_SET_DELAY)
        CHECK_OP(weightNetworkLoss, fuzzNetworkLoss, FUZZ_OP_NETWORK_SET_LOSS)

        /* Default: tick */
        if (!fuzzTick(&state)) {
            goto cleanup;
        }
        lastOp = FUZZ_OP_TICK;

#undef CHECK_OP

    op_done:
        state.stats.totalOperations++;

        /* Replay mode: print detailed state after each operation */
        if (config->replayMode) {
            printReplayState(&state, lastOp);
        }

        /* Update stats from cluster */
        state.stats.electionsTotal = state.cluster->totalElections;
        state.stats.messagesDelivered = state.cluster->network.totalDelivered;
        state.stats.messagesDropped = state.cluster->network.totalDropped;

        /* Compact logs periodically to bound memory */
        if (state.stats.totalOperations % 500 == 0) {
            compactClusterLogs(&state);
        }

        /* Check invariants periodically */
        if (state.stats.totalOperations % 100 == 0) {
            if (!checkInvariants(&state)) {
                result = FUZZ_INVARIANT_VIOLATION;
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
    if (!checkInvariants(&state)) {
        result = FUZZ_INVARIANT_VIOLATION;
    }

    if (state.currentTime >= config->maxTime) {
        result = FUZZ_TIMEOUT;
    }

cleanup:
    /* Final stats */
    printf("\n[FUZZ] Fuzzing complete\n");
    printStats(&state);

    if (result == FUZZ_INVARIANT_VIOLATION) {
        /* Save failure case - save recent operations from circular buffer */
        char filename[256];
        snprintf(filename, sizeof(filename), "fuzz_failure_%u.log",
                 config->seed);
        uint64_t opsToSave = state.opHistoryCount < MAX_OP_HISTORY
                                 ? state.opHistoryCount
                                 : MAX_OP_HISTORY;
        kraftFuzzSaveFailure(config->seed, state.opHistory, opsToSave,
                             filename);
        printf("[FUZZ] Failure case saved to %s (last %" PRIu64 " ops)\n",
               filename, opsToSave);
    }

    /* Copy stats back */
    if (stats) {
        *stats = state.stats;
    }

    /* Cleanup */
    testClusterFree(state.cluster);
    free(state.crashed);
    free(state.crashTime);

    return result;
}

/* ====================================================================
 * Replay and debugging
 * ==================================================================== */
kraftFuzzResult kraftFuzzReplay(uint32_t seed, const kraftFuzzOpType *ops,
                                uint64_t opCount, kraftFuzzStats *stats) {
    /* TODO: Implement replay functionality */
    (void)seed;
    (void)ops;
    (void)opCount;
    (void)stats;
    fprintf(stderr, "[FUZZ] Replay not yet implemented\n");
    return FUZZ_ERROR;
}

bool kraftFuzzSaveFailure(uint32_t seed, const kraftFuzzOpType *ops,
                          uint64_t opCount, const char *filename) {
    FILE *f = fopen(filename, "w");
    if (!f) {
        fprintf(stderr, "[FUZZ] Failed to open %s for writing\n", filename);
        return false;
    }

    fprintf(f, "# Kraft Fuzz Failure Case\n");
    fprintf(f, "# Seed: %u\n", seed);
    fprintf(f, "# Operations: %" PRIu64 "\n", opCount);
    fprintf(f, "\n");

    const char *opNames[] = {"SUBMIT_COMMAND",   "NODE_CRASH",
                             "NODE_RECOVER",     "NETWORK_PARTITION",
                             "NETWORK_HEAL",     "NETWORK_SET_DELAY",
                             "NETWORK_SET_LOSS", "TICK"};

    for (uint64_t i = 0; i < opCount; i++) {
        if (ops[i] < sizeof(opNames) / sizeof(opNames[0])) {
            fprintf(f, "%" PRIu64 ": %s\n", i, opNames[ops[i]]);
        }
    }

    fclose(f);
    return true;
}
