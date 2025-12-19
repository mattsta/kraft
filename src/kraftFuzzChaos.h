#ifndef KRAFT_FUZZ_CHAOS_H
#define KRAFT_FUZZ_CHAOS_H

/* Kraft Chaos-Enhanced Fuzzer
 * ============================
 * Combines the chaos testing framework with fuzzing for comprehensive
 * adversarial testing of Raft consensus under Byzantine and network faults.
 *
 * This fuzzer exercises:
 *   - Network faults: packet loss, delays, corruption, reordering
 *   - Partitions: symmetric and asymmetric network splits
 *   - Node faults: crashes, pauses (GC simulation), slow nodes
 *   - Byzantine faults: equivocation, lying, fake leadership
 *   - Clock anomalies: drift, jumps, pauses
 *
 * Usage:
 *   ./kraft-fuzz-chaos                    # Quick chaos fuzz
 *   ./kraft-fuzz-chaos -n 7 -o 10000      # 7 nodes, 10k ops
 *   ./kraft-fuzz-chaos --byzantine 2      # 2 Byzantine nodes
 *   ./kraft-fuzz-chaos --scenario storm   # Pre-defined chaos scenario
 *
 * Scenarios:
 *   flaky     - Intermittent packet loss and delays
 *   storm     - Rapid partition changes
 *   byzantine - Byzantine nodes with equivocation
 *   chaos     - All fault types combined
 *   hellweek  - Maximum adversarial conditions
 */

#include "chaos.h"
#include "kraftTest.h"
#include <stdbool.h>
#include <stdint.h>

/* =========================================================================
 * Chaos Scenario Presets
 * ========================================================================= */

typedef enum kraftFuzzChaosScenario {
    FUZZ_CHAOS_NONE = 0,         /* No chaos (baseline) */
    FUZZ_CHAOS_FLAKY,            /* Flaky network (10-20% loss) */
    FUZZ_CHAOS_DELAY,            /* High latency network */
    FUZZ_CHAOS_PARTITION,        /* Frequent partitions */
    FUZZ_CHAOS_STORM,            /* Partition storms */
    FUZZ_CHAOS_BYZANTINE_SINGLE, /* Single Byzantine node */
    FUZZ_CHAOS_BYZANTINE_MULTI,  /* Multiple Byzantine nodes */
    FUZZ_CHAOS_CLOCK_DRIFT,      /* Clock drift across nodes */
    FUZZ_CHAOS_GC_PAUSES,        /* Simulated GC pauses */
    FUZZ_CHAOS_MIXED,            /* Mix of all fault types */
    FUZZ_CHAOS_HELLWEEK,         /* Maximum adversarial conditions */
    FUZZ_CHAOS_SCENARIO_COUNT
} kraftFuzzChaosScenario;

/* =========================================================================
 * Configuration
 * ========================================================================= */

typedef struct kraftFuzzChaosConfig {
    /* Basic parameters */
    uint32_t nodeCount;     /* Number of nodes (default: 5) */
    uint64_t maxOperations; /* Max operations per run (default: 5000) */
    uint64_t maxTime;       /* Max simulated time in us (default: 60s) */
    uint32_t seed;          /* Random seed for reproducibility */

    /* Chaos configuration */
    kraftFuzzChaosScenario scenario;     /* Preset scenario */
    bool useCustomChaos;                 /* Use custom chaos config below */
    kraftChaosNetworkConfig network;     /* Custom network config */
    kraftChaosPartitionConfig partition; /* Custom partition config */
    kraftChaosNodeConfig node;           /* Custom node config */
    kraftChaosClockConfig clock;         /* Custom clock config */

    /* Byzantine configuration */
    uint32_t byzantineNodeCount;              /* Number of Byzantine nodes */
    kraftByzantineBehavior byzantineBehavior; /* Default behavior */
    bool rotateByzantine;               /* Rotate which nodes are Byzantine */
    uint32_t byzantineRotationInterval; /* Rotation interval (ops) */

    /* Chaos dynamics */
    bool dynamicChaos;           /* Change chaos parameters over time */
    uint32_t chaosPhaseInterval; /* Operations between chaos phase changes */
    bool escalatingChaos;        /* Increase chaos intensity over time */

    /* Invariant checking */
    bool checkElectionSafety;        /* At most one leader per term */
    bool checkLogMatching;           /* Logs match when they should */
    bool checkLeaderCompleteness;    /* Committed entries persist */
    bool checkStateMachineSafety;    /* Consistent state machine */
    bool checkByzantineResilience;   /* System works despite Byzantine */
    uint32_t invariantCheckInterval; /* Check invariants every N ops */

    /* Reporting */
    bool verbose;
    bool logChaosEvents;     /* Log each chaos event */
    uint64_t reportInterval; /* Report stats every N ops */
    bool replayMode;         /* Step-by-step output */
} kraftFuzzChaosConfig;

/* =========================================================================
 * Statistics
 * ========================================================================= */

typedef struct kraftFuzzChaosStats {
    /* Basic stats */
    uint64_t totalOperations;
    uint64_t simulatedTimeUs;
    uint64_t commandsSubmitted;
    uint64_t commandsCommitted;

    /* Election stats */
    uint64_t electionsTotal;
    uint64_t electionsSuccessful;
    uint64_t electionsFailed;
    uint64_t leaderChanges;

    /* Message stats */
    uint64_t messagesTotal;
    uint64_t messagesDelivered;
    uint64_t messagesDropped;
    uint64_t messagesDelayed;
    uint64_t messagesDuplicated;
    uint64_t messagesCorrupted;

    /* Partition stats */
    uint64_t partitionsCreated;
    uint64_t partitionsHealed;
    uint64_t partitionDurationTotal;
    uint64_t maxSimultaneousPartitions;

    /* Node fault stats */
    uint64_t nodesCrashed;
    uint64_t nodesRecovered;
    uint64_t nodesPaused;
    uint64_t nodesResumed;
    uint64_t maxSimultaneousCrashes;

    /* Byzantine stats */
    uint64_t byzantineActionsTotal;
    uint64_t byzantineLieVotes;
    uint64_t byzantineEquivocations;
    uint64_t byzantineFakeLeaders;
    uint64_t byzantineCorruptLogs;
    uint64_t byzantineDetected; /* Times Byzantine behavior was detected */

    /* Clock stats */
    uint64_t clockDrifts;
    uint64_t clockJumps;
    int64_t maxClockSkew;

    /* Invariant stats */
    uint64_t invariantChecks;
    uint64_t invariantViolations;
    char lastViolation[256];

    /* Chaos phase tracking */
    uint32_t chaosPhases;
    uint32_t currentPhase;
} kraftFuzzChaosStats;

/* =========================================================================
 * Result
 * ========================================================================= */

typedef enum kraftFuzzChaosResult {
    FUZZ_CHAOS_SUCCESS = 0,         /* Completed without violations */
    FUZZ_CHAOS_INVARIANT_VIOLATION, /* Raft invariant violated */
    FUZZ_CHAOS_BYZANTINE_BREACH,    /* Byzantine fault compromised safety */
    FUZZ_CHAOS_TIMEOUT,             /* Hit time limit (not a failure) */
    FUZZ_CHAOS_ERROR                /* Internal error */
} kraftFuzzChaosResult;

/* =========================================================================
 * API Functions
 * ========================================================================= */

/**
 * Get default chaos fuzzer configuration
 */
kraftFuzzChaosConfig kraftFuzzChaosConfigDefault(void);

/**
 * Get configuration for a specific scenario
 */
kraftFuzzChaosConfig
kraftFuzzChaosConfigForScenario(kraftFuzzChaosScenario scenario);

/**
 * Run chaos fuzzer
 *
 * @param config Fuzzer configuration
 * @param stats Output statistics (can be NULL)
 * @return Result code
 */
kraftFuzzChaosResult kraftFuzzChaosRun(const kraftFuzzChaosConfig *config,
                                       kraftFuzzChaosStats *stats);

/**
 * Run chaos fuzzer with a specific scenario
 */
kraftFuzzChaosResult kraftFuzzChaosRunScenario(kraftFuzzChaosScenario scenario,
                                               uint32_t seed,
                                               kraftFuzzChaosStats *stats);

/**
 * Get scenario name
 */
const char *kraftFuzzChaosScenarioName(kraftFuzzChaosScenario scenario);

/**
 * Print statistics summary
 */
void kraftFuzzChaosPrintStats(const kraftFuzzChaosStats *stats);

/**
 * Save failure case for replay
 */
bool kraftFuzzChaosSaveFailure(const kraftFuzzChaosConfig *config,
                               const kraftFuzzChaosStats *stats,
                               const char *filename);

#endif /* KRAFT_FUZZ_CHAOS_H */
