/* Kraft Chaos-Enhanced Fuzzer Implementation
 *
 * Integrates the chaos testing framework with property-based fuzzing
 * to comprehensively test Raft consensus under adversarial conditions.
 */

#include "kraftFuzzChaos.h"
#include "chaos.h"
#include "kraftFuzz.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* =========================================================================
 * Internal State
 * ========================================================================= */

typedef struct fuzzChaosState {
    testCluster *cluster;
    kraftChaosEngine *chaos;
    kraftFuzzChaosConfig config;
    kraftFuzzChaosStats stats;

    /* Random state */
    uint64_t rngState;

    /* Tracking */
    uint32_t currentPhase;
    uint64_t phaseStartOp;
    uint64_t lastInvariantCheck;
    uint32_t *byzantineNodes;
    uint32_t activeByzantineCount;

    /* Invariant tracking */
    uint64_t *lastCommittedByNode;
    uint64_t highestCommittedTerm;
    uint32_t lastKnownLeader;
} fuzzChaosState;

/* =========================================================================
 * Random Number Generation
 * ========================================================================= */

static uint64_t fuzzChaosRand(fuzzChaosState *state) {
    /* xorshift64* */
    state->rngState ^= state->rngState >> 12;
    state->rngState ^= state->rngState << 25;
    state->rngState ^= state->rngState >> 27;
    return state->rngState * 0x2545F4914F6CDD1DULL;
}

static uint32_t fuzzChaosRandRange(fuzzChaosState *state, uint32_t min,
                                   uint32_t max) {
    if (min >= max) {
        return min;
    }
    return min + (fuzzChaosRand(state) % (max - min + 1));
}

static float fuzzChaosRandFloat(fuzzChaosState *state) {
    return (float)(fuzzChaosRand(state) & 0xFFFFFF) / (float)0xFFFFFF;
}

static bool fuzzChaosRandBool(fuzzChaosState *state, float probability) {
    return fuzzChaosRandFloat(state) < probability;
}

/* =========================================================================
 * Scenario Configuration
 * ========================================================================= */

kraftFuzzChaosConfig kraftFuzzChaosConfigDefault(void) {
    kraftFuzzChaosConfig config = {0};

    config.nodeCount = 5;
    config.maxOperations = 100000;       /* 100k ops default */
    config.maxTime = 5 * 60 * 1000000UL; /* 5 minutes default */
    config.seed = (uint32_t)time(NULL);

    config.scenario = FUZZ_CHAOS_MIXED;
    config.useCustomChaos = false;

    config.byzantineNodeCount = 0;
    config.byzantineBehavior = KRAFT_BYZANTINE_NONE;
    config.rotateByzantine = false;
    config.byzantineRotationInterval = 500;

    config.dynamicChaos = true;
    config.chaosPhaseInterval = 500;
    config.escalatingChaos = false;

    config.checkElectionSafety = true;
    config.checkLogMatching = true;
    config.checkLeaderCompleteness = true;
    config.checkStateMachineSafety = true;
    config.checkByzantineResilience = true;
    config.invariantCheckInterval = 50;

    config.verbose = false;
    config.logChaosEvents = false;
    config.reportInterval = 1000;
    config.replayMode = false;

    return config;
}

kraftFuzzChaosConfig
kraftFuzzChaosConfigForScenario(kraftFuzzChaosScenario scenario) {
    kraftFuzzChaosConfig config = kraftFuzzChaosConfigDefault();
    config.scenario = scenario;
    config.useCustomChaos = true;

    switch (scenario) {
    case FUZZ_CHAOS_NONE:
        /* Baseline - truly no chaos at all */
        config.network.dropProbability = 0.0f;
        config.network.delayProbability = 0.0f;
        config.network.duplicateProbability = 0.0f;
        config.network.reorderProbability = 0.0f;
        config.network.corruptProbability = 0.0f;
        config.partition.partitionProbability = 0.0f;
        config.node.crashProbability = 0.0f;
        config.node.pauseProbability = 0.0f;
        config.clock.driftProbability = 0.0f;
        config.clock.jumpProbability = 0.0f;
        config.dynamicChaos = false; /* Disable dynamic chaos changes */
        config.escalatingChaos = false;
        config.byzantineNodeCount = 0;
        break;

    case FUZZ_CHAOS_FLAKY:
        /* Flaky network */
        config.network.dropProbability = 0.15f;
        config.network.delayProbability = 0.20f;
        config.network.minDelayMs = 10;
        config.network.maxDelayMs = 100;
        config.network.duplicateProbability = 0.05f;
        break;

    case FUZZ_CHAOS_DELAY:
        /* High latency */
        config.network.delayProbability = 0.80f;
        config.network.minDelayMs = 50;
        config.network.maxDelayMs = 500;
        break;

    case FUZZ_CHAOS_PARTITION:
        /* Frequent partitions */
        config.partition.partitionProbability = 0.05f;
        config.partition.minDurationMs = 500;
        config.partition.maxDurationMs = 5000;
        config.partition.maxPartitions = 2;
        break;

    case FUZZ_CHAOS_STORM:
        /* Partition storm */
        config.partition.partitionProbability = 0.15f;
        config.partition.minDurationMs = 100;
        config.partition.maxDurationMs = 2000;
        config.partition.asymmetric = true;
        config.partition.maxPartitions = 3;
        config.network.dropProbability = 0.10f;
        break;

    case FUZZ_CHAOS_BYZANTINE_SINGLE:
        /* Single Byzantine node */
        config.byzantineNodeCount = 1;
        config.byzantineBehavior = KRAFT_BYZANTINE_EQUIVOCATE;
        config.rotateByzantine = true;
        config.byzantineRotationInterval = 1000;
        break;

    case FUZZ_CHAOS_BYZANTINE_MULTI:
        /* Multiple Byzantine nodes (up to f for 3f+1) */
        config.byzantineNodeCount = (config.nodeCount - 1) / 3;
        if (config.byzantineNodeCount == 0) {
            config.byzantineNodeCount = 1;
        }
        config.byzantineBehavior = KRAFT_BYZANTINE_EQUIVOCATE;
        config.rotateByzantine = true;
        config.byzantineRotationInterval = 500;
        break;

    case FUZZ_CHAOS_CLOCK_DRIFT:
        /* Clock anomalies */
        config.clock.driftProbability = 0.30f;
        config.clock.jumpProbability = 0.05f;
        config.clock.driftFactor = 1.5f;
        config.clock.maxJumpMs = 5000;
        break;

    case FUZZ_CHAOS_GC_PAUSES:
        /* Simulated GC pauses */
        config.node.pauseProbability = 0.10f;
        config.node.minPauseDurationMs = 100;
        config.node.maxPauseDurationMs = 2000;
        break;

    case FUZZ_CHAOS_MIXED:
        /* Mix of all faults */
        config.network.dropProbability = 0.05f;
        config.network.delayProbability = 0.10f;
        config.network.minDelayMs = 10;
        config.network.maxDelayMs = 100;
        config.network.duplicateProbability = 0.02f;
        config.network.reorderProbability = 0.02f;

        config.partition.partitionProbability = 0.02f;
        config.partition.minDurationMs = 500;
        config.partition.maxDurationMs = 3000;
        config.partition.maxPartitions = 1;

        config.node.crashProbability = 0.01f;
        config.node.pauseProbability = 0.02f;
        config.node.minPauseDurationMs = 50;
        config.node.maxPauseDurationMs = 500;
        config.node.restartProbability = 0.50f;
        config.node.restartDelayMs = 1000;

        config.clock.driftProbability = 0.05f;
        config.clock.driftFactor = 1.2f;
        break;

    case FUZZ_CHAOS_HELLWEEK:
        /* Maximum adversarial conditions */
        config.network.dropProbability = 0.20f;
        config.network.delayProbability = 0.30f;
        config.network.minDelayMs = 20;
        config.network.maxDelayMs = 300;
        config.network.duplicateProbability = 0.10f;
        config.network.reorderProbability = 0.10f;
        config.network.corruptProbability = 0.02f;

        config.partition.partitionProbability = 0.10f;
        config.partition.minDurationMs = 200;
        config.partition.maxDurationMs = 5000;
        config.partition.asymmetric = true;
        config.partition.maxPartitions = 2;

        config.node.crashProbability = 0.05f;
        config.node.pauseProbability = 0.10f;
        config.node.minPauseDurationMs = 100;
        config.node.maxPauseDurationMs = 2000;
        config.node.restartProbability = 0.80f;
        config.node.restartDelayMs = 500;

        config.byzantineNodeCount = 1;
        config.byzantineBehavior = KRAFT_BYZANTINE_EQUIVOCATE;
        config.rotateByzantine = true;
        config.byzantineRotationInterval = 300;

        config.clock.driftProbability = 0.20f;
        config.clock.jumpProbability = 0.05f;
        config.clock.driftFactor = 2.0f;
        config.clock.maxJumpMs = 10000;

        config.escalatingChaos = true;
        config.chaosPhaseInterval = 200;
        break;

    default:
        break;
    }

    return config;
}

const char *kraftFuzzChaosScenarioName(kraftFuzzChaosScenario scenario) {
    static const char *names[] = {"none",
                                  "flaky",
                                  "delay",
                                  "partition",
                                  "storm",
                                  "byzantine-single",
                                  "byzantine-multi",
                                  "clock-drift",
                                  "gc-pauses",
                                  "mixed",
                                  "hellweek"};
    if (scenario >= FUZZ_CHAOS_SCENARIO_COUNT) {
        return "unknown";
    }
    return names[scenario];
}

/* =========================================================================
 * Chaos Application
 * ========================================================================= */

static void applyChaosConfig(fuzzChaosState *state) {
    kraftChaosEngine *chaos = state->chaos;

    if (state->config.useCustomChaos) {
        kraftChaosConfigureNetwork(chaos, &state->config.network);
        kraftChaosConfigurePartitions(chaos, &state->config.partition);
        kraftChaosConfigureNodes(chaos, &state->config.node);
        kraftChaosConfigureClock(chaos, &state->config.clock);
    }

    /* Setup Byzantine nodes */
    if (state->config.byzantineNodeCount > 0) {
        state->byzantineNodes =
            zcalloc(state->config.nodeCount, sizeof(uint32_t));
        state->activeByzantineCount = 0;

        /* Initially assign Byzantine nodes randomly */
        for (uint32_t i = 0; i < state->config.byzantineNodeCount &&
                             i < state->config.nodeCount;
             i++) {
            uint32_t nodeId;
            bool unique;
            do {
                nodeId =
                    fuzzChaosRandRange(state, 0, state->config.nodeCount - 1);
                unique = true;
                for (uint32_t j = 0; j < state->activeByzantineCount; j++) {
                    if (state->byzantineNodes[j] == nodeId) {
                        unique = false;
                        break;
                    }
                }
            } while (!unique &&
                     state->activeByzantineCount < state->config.nodeCount);

            state->byzantineNodes[state->activeByzantineCount++] = nodeId;
            kraftChaosSetByzantineNode(chaos, nodeId,
                                       state->config.byzantineBehavior);
        }
    }
}

static void rotateByzantineNodes(fuzzChaosState *state) {
    if (!state->config.rotateByzantine ||
        state->config.byzantineNodeCount == 0) {
        return;
    }

    /* Clear current Byzantine nodes */
    kraftChaosClearAllByzantine(state->chaos);
    state->activeByzantineCount = 0;

    /* Assign new random Byzantine nodes */
    for (uint32_t i = 0;
         i < state->config.byzantineNodeCount && i < state->config.nodeCount;
         i++) {
        uint32_t nodeId;
        bool unique;
        do {
            nodeId = fuzzChaosRandRange(state, 0, state->config.nodeCount - 1);
            unique = true;
            for (uint32_t j = 0; j < state->activeByzantineCount; j++) {
                if (state->byzantineNodes[j] == nodeId) {
                    unique = false;
                    break;
                }
            }
        } while (!unique);

        state->byzantineNodes[state->activeByzantineCount++] = nodeId;

        /* Vary Byzantine behavior */
        kraftByzantineBehavior behavior = state->config.byzantineBehavior;
        if (fuzzChaosRandBool(state, 0.3f)) {
            behavior = (kraftByzantineBehavior)fuzzChaosRandRange(
                state, KRAFT_BYZANTINE_SILENT, KRAFT_BYZANTINE_DELAY_VOTES);
        }

        kraftChaosSetByzantineNode(state->chaos, nodeId, behavior);
    }

    state->stats.byzantineActionsTotal++;
}

static void advanceChaosPhase(fuzzChaosState *state) {
    state->currentPhase++;
    state->stats.chaosPhases++;
    state->stats.currentPhase = state->currentPhase;
    state->phaseStartOp = state->stats.totalOperations;

    if (!state->config.escalatingChaos) {
        /* Just rotate parameters randomly */
        if (state->config.dynamicChaos) {
            /* Modify chaos probabilities randomly */
            kraftChaosNetworkConfig net = state->config.network;
            net.dropProbability *= (0.5f + fuzzChaosRandFloat(state));
            if (net.dropProbability > 0.5f) {
                net.dropProbability = 0.5f;
            }
            net.delayProbability *= (0.5f + fuzzChaosRandFloat(state));
            if (net.delayProbability > 0.8f) {
                net.delayProbability = 0.8f;
            }
            kraftChaosConfigureNetwork(state->chaos, &net);
        }
    } else {
        /* Escalate chaos over time */
        float escalation = 1.0f + (float)state->currentPhase * 0.1f;
        if (escalation > 3.0f) {
            escalation = 3.0f;
        }

        kraftChaosNetworkConfig net = state->config.network;
        net.dropProbability =
            state->config.network.dropProbability * escalation;
        if (net.dropProbability > 0.5f) {
            net.dropProbability = 0.5f;
        }
        net.delayProbability =
            state->config.network.delayProbability * escalation;
        if (net.delayProbability > 0.9f) {
            net.delayProbability = 0.9f;
        }
        kraftChaosConfigureNetwork(state->chaos, &net);

        kraftChaosPartitionConfig part = state->config.partition;
        part.partitionProbability =
            state->config.partition.partitionProbability * escalation;
        if (part.partitionProbability > 0.3f) {
            part.partitionProbability = 0.3f;
        }
        kraftChaosConfigurePartitions(state->chaos, &part);
    }

    if (state->config.verbose) {
        printf("  [Phase %u] Chaos parameters updated\n", state->currentPhase);
    }
}

/* =========================================================================
 * Invariant Checking - uses test framework's invariant checkers
 * ========================================================================= */

static bool checkElectionSafety(fuzzChaosState *state) {
    /* Use test framework's election safety invariant */
    if (!testInvariantElectionSafety(state->cluster)) {
        snprintf(state->stats.lastViolation, sizeof(state->stats.lastViolation),
                 "Election safety: multiple leaders in same term detected");
        return false;
    }
    return true;
}

static bool checkLogMatching(fuzzChaosState *state) {
    /* Use test framework's log matching invariant */
    if (!testInvariantLogMatching(state->cluster)) {
        snprintf(state->stats.lastViolation, sizeof(state->stats.lastViolation),
                 "Log matching: log entries diverged");
        return false;
    }
    return true;
}

static bool checkLeaderCompleteness(fuzzChaosState *state) {
    /* Use test framework's leader completeness invariant */
    if (!testInvariantLeaderCompleteness(state->cluster)) {
        snprintf(state->stats.lastViolation, sizeof(state->stats.lastViolation),
                 "Leader completeness: committed entry missing from leader");
        return false;
    }
    return true;
}

static bool checkStateMachineSafety(fuzzChaosState *state) {
    /* Use test framework's state machine safety invariant */
    if (!testInvariantStateMachineSafety(state->cluster)) {
        snprintf(
            state->stats.lastViolation, sizeof(state->stats.lastViolation),
            "State machine safety: different entries applied at same index");
        return false;
    }
    return true;
}

static bool checkInvariants(fuzzChaosState *state) {
    state->stats.invariantChecks++;
    bool passed = true;

    if (state->config.checkElectionSafety && !checkElectionSafety(state)) {
        state->stats.invariantViolations++;
        passed = false;
    }

    if (passed && state->config.checkLogMatching && !checkLogMatching(state)) {
        state->stats.invariantViolations++;
        passed = false;
    }

    if (passed && state->config.checkLeaderCompleteness &&
        !checkLeaderCompleteness(state)) {
        state->stats.invariantViolations++;
        passed = false;
    }

    if (passed && state->config.checkStateMachineSafety &&
        !checkStateMachineSafety(state)) {
        state->stats.invariantViolations++;
        passed = false;
    }

    return passed;
}

/* =========================================================================
 * Fuzzing Operations
 * ========================================================================= */

typedef enum {
    OP_TICK,
    OP_SUBMIT_COMMAND,
    OP_INJECT_PARTITION,
    OP_HEAL_PARTITION,
    OP_CRASH_NODE,
    OP_RECOVER_NODE,
    OP_PAUSE_NODE,
    OP_CLOCK_DRIFT,
    OP_DELIVER_MESSAGES,
    OP_COUNT
} fuzzChaosOp;

static fuzzChaosOp selectOperation(fuzzChaosState *state) {
    uint32_t weights[OP_COUNT] = {
        200, /* TICK */
        150, /* SUBMIT_COMMAND */
        30,  /* INJECT_PARTITION */
        20,  /* HEAL_PARTITION */
        20,  /* CRASH_NODE */
        30,  /* RECOVER_NODE */
        25,  /* PAUSE_NODE */
        15,  /* CLOCK_DRIFT */
        300, /* DELIVER_MESSAGES */
    };

    /* For NONE scenario, disable chaos operations entirely */
    if (state->config.scenario == FUZZ_CHAOS_NONE) {
        weights[OP_INJECT_PARTITION] = 0;
        weights[OP_HEAL_PARTITION] = 0;
        weights[OP_CRASH_NODE] = 0;
        weights[OP_RECOVER_NODE] = 0;
        weights[OP_PAUSE_NODE] = 0;
        weights[OP_CLOCK_DRIFT] = 0;
    }

    uint32_t total = 0;
    for (int i = 0; i < OP_COUNT; i++) {
        total += weights[i];
    }

    uint32_t r = fuzzChaosRandRange(state, 0, total - 1);
    uint32_t cumulative = 0;

    for (int i = 0; i < OP_COUNT; i++) {
        cumulative += weights[i];
        if (r < cumulative) {
            return (fuzzChaosOp)i;
        }
    }

    return OP_TICK;
}

static void executeOperation(fuzzChaosState *state, fuzzChaosOp op) {
    testCluster *cluster = state->cluster;
    uint32_t nodeCount = state->config.nodeCount;

    switch (op) {
    case OP_TICK: {
        uint32_t ticks = fuzzChaosRandRange(state, 1, 5);
        uint64_t tickUs = 1000; /* 1ms per tick */
        for (uint32_t t = 0; t < ticks; t++) {
            testClusterTick(cluster, tickUs);
        }
        state->stats.simulatedTimeUs += ticks * tickUs;
        break;
    }

    case OP_SUBMIT_COMMAND: {
        /* Use test framework to find leader and submit */
        uint32_t leaderId = testClusterGetLeader(cluster);
        if (leaderId != UINT32_MAX) {
            char cmd[64];
            snprintf(cmd, sizeof(cmd), "cmd-%" PRIu64 "",
                     (unsigned long long)state->stats.commandsSubmitted);
            testClusterSubmitCommand(cluster, cmd, strlen(cmd) + 1);
            state->stats.commandsSubmitted++;

            /* Advance time slightly to allow for RPC processing and heartbeats
             */
            testClusterTick(cluster, 500); /* 0.5ms per command */
            state->stats.simulatedTimeUs += 500;
        }
        break;
    }

    case OP_INJECT_PARTITION: {
        if (state->stats.partitionsCreated - state->stats.partitionsHealed >=
            state->config.partition.maxPartitions) {
            break;
        }

        /* Random partition of 1-2 nodes */
        uint32_t partSize = fuzzChaosRandRange(state, 1, 2);
        uint32_t *partNodes = zcalloc(partSize, sizeof(uint32_t));

        for (uint32_t i = 0; i < partSize; i++) {
            partNodes[i] = fuzzChaosRandRange(state, 0, nodeCount - 1);
        }

        uint32_t duration =
            fuzzChaosRandRange(state, state->config.partition.minDurationMs,
                               state->config.partition.maxDurationMs);

        kraftChaosCreatePartition(state->chaos, partNodes, partSize, duration);
        state->stats.partitionsCreated++;

        zfree(partNodes);
        break;
    }

    case OP_HEAL_PARTITION:
        kraftChaosHealAllPartitions(state->chaos);
        state->stats.partitionsHealed++;
        break;

    case OP_CRASH_NODE: {
        /* Don't crash more than minority */
        uint32_t maxCrash = (nodeCount - 1) / 2;
        uint32_t crashedCount = 0;
        for (uint32_t i = 0; i < nodeCount; i++) {
            if (cluster->nodes[i].crashed) {
                crashedCount++;
            }
        }

        if (crashedCount < maxCrash) {
            uint32_t nodeId;
            int attempts = 0;
            do {
                nodeId = fuzzChaosRandRange(state, 0, nodeCount - 1);
                attempts++;
            } while (cluster->nodes[nodeId].crashed && attempts < 10);

            if (!cluster->nodes[nodeId].crashed) {
                kraftChaosCrashNode(state->chaos, nodeId);
                cluster->nodes[nodeId].crashed = true;
                state->stats.nodesCrashed++;
            }
        }
        break;
    }

    case OP_RECOVER_NODE: {
        /* Find a crashed node to recover */
        for (uint32_t i = 0; i < nodeCount; i++) {
            if (cluster->nodes[i].crashed) {
                kraftChaosRestartNode(state->chaos, i);
                cluster->nodes[i].crashed = false;
                state->stats.nodesRecovered++;
                break;
            }
        }
        break;
    }

    case OP_PAUSE_NODE: {
        uint32_t nodeId = fuzzChaosRandRange(state, 0, nodeCount - 1);
        if (!cluster->nodes[nodeId].crashed) {
            uint32_t duration =
                fuzzChaosRandRange(state, state->config.node.minPauseDurationMs,
                                   state->config.node.maxPauseDurationMs);
            kraftChaosPauseNode(state->chaos, nodeId, duration);
            state->stats.nodesPaused++;
        }
        break;
    }

    case OP_CLOCK_DRIFT: {
        uint32_t nodeId = fuzzChaosRandRange(state, 0, nodeCount - 1);
        float drift = 0.8f + fuzzChaosRandFloat(state) * 0.4f;
        kraftChaosSetClockDrift(state->chaos, nodeId, drift);
        state->stats.clockDrifts++;
        break;
    }

    case OP_DELIVER_MESSAGES: {
        /* Tick to process pending messages and allow heartbeats */
        testClusterTick(cluster, 1000); /* 1ms to process messages */
        state->stats.simulatedTimeUs += 1000;
        state->stats.messagesDelivered += cluster->network.totalDelivered;
        state->stats.messagesDropped += cluster->network.totalDropped;
        break;
    }

    default:
        break;
    }

    state->stats.totalOperations++;
}

/* =========================================================================
 * Main Fuzzer Loop
 * ========================================================================= */

kraftFuzzChaosResult kraftFuzzChaosRun(const kraftFuzzChaosConfig *config,
                                       kraftFuzzChaosStats *stats) {
    fuzzChaosState state = {0};
    state.config = *config;
    state.rngState = config->seed;
    if (state.rngState == 0) {
        state.rngState = 1;
    }

    /* Initialize cluster */
    state.cluster = testClusterNew(config->nodeCount);
    if (!state.cluster) {
        return FUZZ_CHAOS_ERROR;
    }

    /* Start the cluster - this initiates election timers */
    testClusterStart(state.cluster);

    /* Wait for initial leader election before starting chaos */
    testClusterRunUntilLeaderElected(state.cluster,
                                     5000000UL); /* 5 second timeout */

    /* Initialize chaos engine */
    state.chaos = kraftChaosEngineNewWithSeed(config->seed);
    if (!state.chaos) {
        testClusterFree(state.cluster);
        return FUZZ_CHAOS_ERROR;
    }

    /* Apply chaos configuration */
    applyChaosConfig(&state);

    if (config->logChaosEvents) {
        kraftChaosEnableLogging(state.chaos, 1000);
    }

    kraftChaosSetEnabled(state.chaos, true);

    /* Tracking arrays */
    state.lastCommittedByNode = zcalloc(config->nodeCount, sizeof(uint64_t));

    if (config->verbose) {
        printf("Starting chaos fuzzer:\n");
        printf("  Nodes: %u\n", config->nodeCount);
        printf("  Scenario: %s\n",
               kraftFuzzChaosScenarioName(config->scenario));
        printf("  Max ops: %" PRIu64 "\n",
               (unsigned long long)config->maxOperations);
        printf("  Seed: %u\n", config->seed);
        if (config->byzantineNodeCount > 0) {
            printf("  Byzantine nodes: %u\n", config->byzantineNodeCount);
        }
        printf("\n");
    }

    kraftFuzzChaosResult result = FUZZ_CHAOS_SUCCESS;

    /* Track leader changes */
    uint32_t previousLeader = testClusterGetLeader(state.cluster);

    /* Main fuzzing loop */
    while (state.stats.totalOperations < config->maxOperations &&
           state.stats.simulatedTimeUs < config->maxTime) {
        /* Select and execute operation */
        fuzzChaosOp op = selectOperation(&state);
        executeOperation(&state, op);

        /* Track leader changes */
        uint32_t newLeader = testClusterGetLeader(state.cluster);
        if (newLeader != previousLeader && newLeader != UINT32_MAX) {
            state.stats.leaderChanges++;
            if (previousLeader == UINT32_MAX) {
                /* Recovered from no-leader state */
                state.stats.electionsSuccessful++;
            }
            previousLeader = newLeader;
        } else if (newLeader == UINT32_MAX && previousLeader != UINT32_MAX) {
            /* Lost the leader (to crash or partition) */
            previousLeader = newLeader;
        }

        /* Check for chaos phase advancement */
        if (config->dynamicChaos &&
            state.stats.totalOperations - state.phaseStartOp >=
                config->chaosPhaseInterval) {
            advanceChaosPhase(&state);
        }

        /* Rotate Byzantine nodes if configured */
        if (config->rotateByzantine && config->byzantineNodeCount > 0 &&
            state.stats.totalOperations % config->byzantineRotationInterval ==
                0) {
            rotateByzantineNodes(&state);
        }

        /* Periodic invariant checking */
        if (state.stats.totalOperations - state.lastInvariantCheck >=
            config->invariantCheckInterval) {
            state.lastInvariantCheck = state.stats.totalOperations;

            if (!checkInvariants(&state)) {
                result = FUZZ_CHAOS_INVARIANT_VIOLATION;
                break;
            }
        }

        /* Progress reporting */
        if (config->verbose &&
            state.stats.totalOperations % config->reportInterval == 0) {
            printf("  [%" PRIu64 " ops] cmds=%" PRIu64 " elected=%" PRIu64
                   " msgs=%" PRIu64 "/%" PRIu64 " crashes=%" PRIu64 "\n",
                   state.stats.totalOperations, state.stats.commandsSubmitted,
                   state.stats.electionsTotal, state.stats.messagesDelivered,
                   state.stats.messagesDropped, state.stats.nodesCrashed);
        }
    }

    /* Final invariant check */
    if (result == FUZZ_CHAOS_SUCCESS && !checkInvariants(&state)) {
        result = FUZZ_CHAOS_INVARIANT_VIOLATION;
    }

    /* Check if we hit time limit */
    if (result == FUZZ_CHAOS_SUCCESS &&
        state.stats.simulatedTimeUs >= config->maxTime) {
        result = FUZZ_CHAOS_TIMEOUT;
    }

    /* Copy cluster stats to final stats */
    state.stats.electionsTotal = state.cluster->totalElections;
    /* leaderChanges and electionsSuccessful are tracked during the main loop */
    state.stats.messagesTotal = state.cluster->network.totalSent;
    state.stats.messagesDelivered = state.cluster->network.totalDelivered;
    state.stats.messagesDropped = state.cluster->network.totalDropped;

    /* If we have a leader at the end, count the initial election */
    uint32_t finalLeader = testClusterGetLeader(state.cluster);
    if (finalLeader != UINT32_MAX || state.stats.leaderChanges > 0) {
        /* At least one successful election happened (the initial one) */
        state.stats.electionsSuccessful += 1;
    }

    /* Count committed commands by checking commit indices */
    uint64_t maxCommit = 0;
    for (uint32_t i = 0; i < state.cluster->nodeCount; i++) {
        if (!state.cluster->nodes[i].crashed && state.cluster->nodes[i].state) {
            uint64_t commit = state.cluster->nodes[i].state->commitIndex;
            if (commit > maxCommit) {
                maxCommit = commit;
            }
        }
    }
    state.stats.commandsCommitted = maxCommit;

    /* Copy stats if requested */
    if (stats) {
        *stats = state.stats;
    }

    /* Cleanup */
    zfree(state.lastCommittedByNode);
    zfree(state.byzantineNodes);
    kraftChaosEngineFree(state.chaos);
    testClusterFree(state.cluster);

    return result;
}

kraftFuzzChaosResult kraftFuzzChaosRunScenario(kraftFuzzChaosScenario scenario,
                                               uint32_t seed,
                                               kraftFuzzChaosStats *stats) {
    kraftFuzzChaosConfig config = kraftFuzzChaosConfigForScenario(scenario);
    config.seed = seed;
    return kraftFuzzChaosRun(&config, stats);
}

/* =========================================================================
 * Reporting
 * ========================================================================= */

void kraftFuzzChaosPrintStats(const kraftFuzzChaosStats *stats) {
    printf("\n--- Chaos Fuzzer Statistics ---\n");
    printf("Operations:     %" PRIu64 "\n", stats->totalOperations);
    printf("Simulated time: %" PRIu64 " us (%.2f s)\n", stats->simulatedTimeUs,
           stats->simulatedTimeUs / 1000000.0);
    printf("Commands:       %" PRIu64 " submitted, %" PRIu64 " committed\n",
           stats->commandsSubmitted, stats->commandsCommitted);
    printf("\n");

    printf("Elections:      %" PRIu64 " total, %" PRIu64 " successful\n",
           stats->electionsTotal, stats->electionsSuccessful);
    printf("Leader changes: %" PRIu64 "\n", stats->leaderChanges);
    printf("\n");

    printf("Messages:       %" PRIu64 " total\n", stats->messagesTotal);
    printf("  Delivered:    %" PRIu64 "\n", stats->messagesDelivered);
    printf("  Dropped:      %" PRIu64 "\n", stats->messagesDropped);
    printf("  Delayed:      %" PRIu64 "\n", stats->messagesDelayed);
    printf("  Duplicated:   %" PRIu64 "\n", stats->messagesDuplicated);
    printf("  Corrupted:    %" PRIu64 "\n", stats->messagesCorrupted);
    printf("\n");

    printf("Partitions:     %" PRIu64 " created, %" PRIu64 " healed\n",
           stats->partitionsCreated, stats->partitionsHealed);
    printf("Node crashes:   %" PRIu64 " crashed, %" PRIu64 " recovered\n",
           stats->nodesCrashed, stats->nodesRecovered);
    printf("Node pauses:    %" PRIu64 " paused, %" PRIu64 " resumed\n",
           stats->nodesPaused, stats->nodesResumed);
    printf("\n");

    if (stats->byzantineActionsTotal > 0) {
        printf("Byzantine:      %" PRIu64 " actions\n",
               stats->byzantineActionsTotal);
        printf("  Lie votes:    %" PRIu64 "\n", stats->byzantineLieVotes);
        printf("  Equivocate:   %" PRIu64 "\n", stats->byzantineEquivocations);
        printf("  Fake leader:  %" PRIu64 "\n", stats->byzantineFakeLeaders);
        printf("  Corrupt log:  %" PRIu64 "\n", stats->byzantineCorruptLogs);
        printf("\n");
    }

    printf("Clock drifts:   %" PRIu64 "\n", stats->clockDrifts);
    printf("Clock jumps:    %" PRIu64 "\n", stats->clockJumps);
    printf("\n");

    printf("Invariants:     %" PRIu64 " checks, %" PRIu64 " violations\n",
           stats->invariantChecks, stats->invariantViolations);
    if (stats->invariantViolations > 0) {
        printf("Last violation: %s\n", stats->lastViolation);
    }

    printf("Chaos phases:   %" PRIu32 "\n", stats->chaosPhases);
    printf("-------------------------------\n");
}

bool kraftFuzzChaosSaveFailure(const kraftFuzzChaosConfig *config,
                               const kraftFuzzChaosStats *stats,
                               const char *filename) {
    FILE *f = fopen(filename, "w");
    if (!f) {
        return false;
    }

    fprintf(f, "# Kraft Chaos Fuzzer Failure Report\n");
    fprintf(f, "seed=%u\n", config->seed);
    fprintf(f, "nodes=%u\n", config->nodeCount);
    fprintf(f, "scenario=%s\n", kraftFuzzChaosScenarioName(config->scenario));
    fprintf(f, "operations=%" PRIu64 "\n", stats->totalOperations);
    fprintf(f, "violation=%s\n", stats->lastViolation);

    fclose(f);
    return true;
}
