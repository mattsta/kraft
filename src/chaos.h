#ifndef KRAFT_CHAOS_H
#define KRAFT_CHAOS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Kraft Chaos Testing Framework
 * ==========================================
 * Comprehensive fault injection for testing Raft robustness under:
 * - Network failures (partitions, delays, packet loss)
 * - Node failures (crashes, hangs, restarts)
 * - Byzantine faults (message corruption, lying, equivocation)
 * - Clock drift and timing anomalies
 *
 * Quick Start:
 *   kraftChaosEngine *chaos = kraftChaosEngineNew();
 *   kraftChaosEnablePartitions(chaos, 0.1);  // 10% partition chance
 *   kraftChaosAttach(chaos, state);
 *   // ... run tests ...
 *   kraftChaosEngineFree(chaos);
 *
 * Byzantine Mode:
 *   kraftChaosEngine *chaos = kraftChaosEngineNew();
 *   kraftChaosSetByzantineNode(chaos, nodeId, KRAFT_BYZANTINE_EQUIVOCATE);
 *   kraftChaosAttach(chaos, state);
 */

/* Forward declarations */
struct kraftState;
struct kraftNode;

/* =========================================================================
 * Fault Types
 * ========================================================================= */

/* Network fault types */
typedef enum kraftNetworkFault {
    KRAFT_FAULT_NONE = 0,
    KRAFT_FAULT_DROP,          /* Drop the message entirely */
    KRAFT_FAULT_DELAY,         /* Add latency to delivery */
    KRAFT_FAULT_DUPLICATE,     /* Deliver message multiple times */
    KRAFT_FAULT_REORDER,       /* Deliver out of order */
    KRAFT_FAULT_CORRUPT_DATA,  /* Corrupt message payload */
    KRAFT_FAULT_CORRUPT_TERM,  /* Corrupt term number (Byzantine) */
    KRAFT_FAULT_CORRUPT_INDEX, /* Corrupt log index (Byzantine) */
    KRAFT_FAULT_COUNT
} kraftNetworkFault;

/* Node fault types */
typedef enum kraftNodeFault {
    KRAFT_NODE_HEALTHY = 0,
    KRAFT_NODE_CRASHED,     /* Node is completely unresponsive */
    KRAFT_NODE_PARTITIONED, /* Node is isolated from others */
    KRAFT_NODE_SLOW,        /* Node has slow responses */
    KRAFT_NODE_PAUSED,      /* Node is paused (GC simulation) */
    KRAFT_NODE_BYZANTINE,   /* Node is actively malicious */
    KRAFT_NODE_FAULT_COUNT
} kraftNodeFault;

/* Byzantine behavior types */
typedef enum kraftByzantineBehavior {
    KRAFT_BYZANTINE_NONE = 0,
    KRAFT_BYZANTINE_SILENT,      /* Never respond */
    KRAFT_BYZANTINE_SELECTIVE,   /* Only respond to some nodes */
    KRAFT_BYZANTINE_LIE_VOTE,    /* Claim to have voted when didn't */
    KRAFT_BYZANTINE_EQUIVOCATE,  /* Send different votes to different nodes */
    KRAFT_BYZANTINE_FAKE_LEADER, /* Claim leadership falsely */
    KRAFT_BYZANTINE_FUTURE_TERM, /* Use artificially high term numbers */
    KRAFT_BYZANTINE_CORRUPT_LOG, /* Send incorrect log entries */
    KRAFT_BYZANTINE_DELAY_VOTES, /* Withhold votes strategically */
    KRAFT_BYZANTINE_COUNT
} kraftByzantineBehavior;

/* Clock drift types */
typedef enum kraftClockFault {
    KRAFT_CLOCK_NORMAL = 0,
    KRAFT_CLOCK_FAST,          /* Clock runs faster than real time */
    KRAFT_CLOCK_SLOW,          /* Clock runs slower than real time */
    KRAFT_CLOCK_JUMP_FORWARD,  /* Clock suddenly jumps ahead */
    KRAFT_CLOCK_JUMP_BACKWARD, /* Clock suddenly jumps back */
    KRAFT_CLOCK_PAUSE,         /* Clock temporarily stops */
    KRAFT_CLOCK_FAULT_COUNT
} kraftClockFault;

/* =========================================================================
 * Configuration Structures
 * ========================================================================= */

/* Network chaos configuration */
typedef struct kraftChaosNetworkConfig {
    float dropProbability;      /* 0.0 - 1.0: chance to drop message */
    float delayProbability;     /* 0.0 - 1.0: chance to delay message */
    float duplicateProbability; /* 0.0 - 1.0: chance to duplicate */
    float reorderProbability;   /* 0.0 - 1.0: chance to reorder */
    float corruptProbability;   /* 0.0 - 1.0: chance to corrupt */
    uint32_t minDelayMs;        /* Minimum delay when delaying */
    uint32_t maxDelayMs;        /* Maximum delay when delaying */
    uint32_t maxDuplicates;     /* Max duplicates to create */
} kraftChaosNetworkConfig;

/* Partition configuration */
typedef struct kraftChaosPartitionConfig {
    float partitionProbability; /* 0.0 - 1.0: chance to create partition */
    uint32_t minDurationMs;     /* Minimum partition duration */
    uint32_t maxDurationMs;     /* Maximum partition duration */
    bool asymmetric;            /* Allow asymmetric partitions */
    uint32_t maxPartitions;     /* Max simultaneous partitions */
} kraftChaosPartitionConfig;

/* Node chaos configuration */
typedef struct kraftChaosNodeConfig {
    float crashProbability;      /* 0.0 - 1.0: chance to crash */
    float pauseProbability;      /* 0.0 - 1.0: chance to pause (GC sim) */
    uint32_t minPauseDurationMs; /* Minimum pause duration */
    uint32_t maxPauseDurationMs; /* Maximum pause duration */
    float restartProbability;    /* 0.0 - 1.0: chance crashed node restarts */
    uint32_t restartDelayMs;     /* Delay before restart */
} kraftChaosNodeConfig;

/* Clock chaos configuration */
typedef struct kraftChaosClockConfig {
    float driftProbability; /* 0.0 - 1.0: chance of clock drift */
    float jumpProbability;  /* 0.0 - 1.0: chance of clock jump */
    float driftFactor;      /* 0.5 - 2.0: clock speed factor */
    int64_t maxJumpMs;      /* Maximum jump size (positive or negative) */
} kraftChaosClockConfig;

/* Per-node Byzantine configuration */
typedef struct kraftChaosByzantineNode {
    uint32_t nodeId;                 /* Which node is Byzantine */
    kraftByzantineBehavior behavior; /* Type of Byzantine behavior */
    uint32_t targetNodeMask;         /* Bitmask of nodes to target (0 = all) */
    float activationProbability;     /* How often to exhibit behavior */
} kraftChaosByzantineNode;

/* =========================================================================
 * Event Recording
 * ========================================================================= */

/* Recorded fault event for analysis */
typedef struct kraftChaosEvent {
    uint64_t timestamp;               /* When fault occurred */
    kraftNetworkFault networkFault;   /* Type of network fault if any */
    kraftNodeFault nodeFault;         /* Type of node fault if any */
    kraftByzantineBehavior byzantine; /* Byzantine behavior if any */
    uint32_t sourceNode;              /* Source node ID */
    uint32_t targetNode;              /* Target node ID (if applicable) */
    uint64_t term;                    /* Term when fault occurred */
    uint64_t index;                   /* Log index when fault occurred */
    char details[128];                /* Human-readable description */
} kraftChaosEvent;

/* Event ring buffer */
typedef struct kraftChaosEventLog {
    kraftChaosEvent *events; /* Ring buffer of events */
    uint32_t capacity;       /* Total capacity */
    uint32_t head;           /* Write position */
    uint32_t count;          /* Current count */
} kraftChaosEventLog;

/* =========================================================================
 * Partition State
 * ========================================================================= */

/* Represents a network partition */
typedef struct kraftChaosPartition {
    uint32_t id;             /* Partition identifier */
    uint64_t startTime;      /* When partition started */
    uint64_t endTime;        /* When partition ends (0 = indefinite) */
    uint32_t *isolatedNodes; /* Array of isolated node IDs */
    uint32_t isolatedCount;  /* Number of isolated nodes */
    bool active;             /* Is partition currently active */
    bool asymmetric;         /* Is partition asymmetric */
    uint32_t asymmetricDir;  /* Direction for asymmetric: 0=A->B, 1=B->A */
} kraftChaosPartition;

/* =========================================================================
 * Chaos Engine
 * ========================================================================= */

/* Main chaos engine structure */
typedef struct kraftChaosEngine {
    /* Configuration */
    kraftChaosNetworkConfig network;
    kraftChaosPartitionConfig partition;
    kraftChaosNodeConfig node;
    kraftChaosClockConfig clock;

    /* Byzantine nodes */
    kraftChaosByzantineNode *byzantineNodes;
    uint32_t byzantineNodeCount;
    uint32_t byzantineNodeCapacity;

    /* Active partitions */
    kraftChaosPartition *partitions;
    uint32_t partitionCount;
    uint32_t partitionCapacity;

    /* Node states */
    kraftNodeFault *nodeStates; /* Per-node fault state */
    uint64_t *nodePauseEnd;     /* When paused nodes resume */
    uint32_t nodeCount;

    /* Clock state per node */
    int64_t *clockOffsets;    /* Per-node clock offset */
    float *clockDriftFactors; /* Per-node clock drift factor */

    /* Event logging */
    kraftChaosEventLog eventLog;
    bool logEnabled;

    /* Statistics */
    uint64_t messagesDropped;
    uint64_t messagesDelayed;
    uint64_t messagesDuplicated;
    uint64_t messagesCorrupted;
    uint64_t partitionsCreated;
    uint64_t nodesCrashed;
    uint64_t nodesPaused;
    uint64_t byzantineActions;

    /* Random seed for reproducibility */
    uint64_t seed;
    uint64_t rngState;

    /* Attached state (for callbacks) */
    struct kraftState *attachedState;
    bool enabled;
} kraftChaosEngine;

/* Statistics snapshot */
typedef struct kraftChaosStats {
    uint64_t messagesDropped;
    uint64_t messagesDelayed;
    uint64_t messagesDuplicated;
    uint64_t messagesCorrupted;
    uint64_t partitionsCreated;
    uint64_t activePartitions;
    uint64_t nodesCrashed;
    uint64_t activeCrashes;
    uint64_t nodesPaused;
    uint64_t byzantineActions;
    uint64_t totalEventsLogged;
} kraftChaosStats;

/* =========================================================================
 * API Functions
 * ========================================================================= */

/* --- Engine Lifecycle --- */

/**
 * Create a new chaos engine
 *
 * @return New chaos engine or NULL on failure
 */
kraftChaosEngine *kraftChaosEngineNew(void);

/**
 * Create chaos engine with specific seed for reproducibility
 *
 * @param seed Random seed for deterministic replay
 * @return New chaos engine or NULL on failure
 */
kraftChaosEngine *kraftChaosEngineNewWithSeed(uint64_t seed);

/**
 * Free chaos engine and all resources
 *
 * @param chaos Engine to free
 */
void kraftChaosEngineFree(kraftChaosEngine *chaos);

/**
 * Attach chaos engine to a kraft state
 * The engine will intercept RPC messages and inject faults
 *
 * @param chaos Chaos engine
 * @param state Kraft state to attach to
 * @return true on success
 */
bool kraftChaosAttach(kraftChaosEngine *chaos, struct kraftState *state);

/**
 * Detach chaos engine from kraft state
 *
 * @param chaos Chaos engine
 */
void kraftChaosDetach(kraftChaosEngine *chaos);

/**
 * Enable/disable chaos injection (can be toggled during test)
 *
 * @param chaos Chaos engine
 * @param enabled true to enable fault injection
 */
void kraftChaosSetEnabled(kraftChaosEngine *chaos, bool enabled);

/* --- Network Configuration --- */

/**
 * Configure network fault probabilities
 */
void kraftChaosConfigureNetwork(kraftChaosEngine *chaos,
                                const kraftChaosNetworkConfig *config);

/**
 * Set packet drop probability
 */
void kraftChaosSetDropProbability(kraftChaosEngine *chaos, float probability);

/**
 * Set packet delay configuration
 */
void kraftChaosSetDelay(kraftChaosEngine *chaos, float probability,
                        uint32_t minMs, uint32_t maxMs);

/**
 * Set packet duplication probability
 */
void kraftChaosSetDuplicateProbability(kraftChaosEngine *chaos,
                                       float probability);

/**
 * Set packet corruption probability (Byzantine)
 */
void kraftChaosSetCorruptProbability(kraftChaosEngine *chaos,
                                     float probability);

/* --- Partition Management --- */

/**
 * Configure partition behavior
 */
void kraftChaosConfigurePartitions(kraftChaosEngine *chaos,
                                   const kraftChaosPartitionConfig *config);

/**
 * Create a network partition isolating specified nodes
 *
 * @param chaos Chaos engine
 * @param nodeIds Array of node IDs to isolate
 * @param count Number of nodes to isolate
 * @param durationMs Duration of partition (0 = indefinite)
 * @return Partition ID (0 on failure)
 */
uint32_t kraftChaosCreatePartition(kraftChaosEngine *chaos,
                                   const uint32_t *nodeIds, uint32_t count,
                                   uint32_t durationMs);

/**
 * Create asymmetric partition (A can reach B, but B cannot reach A)
 *
 * @param chaos Chaos engine
 * @param sideA Array of node IDs on side A
 * @param countA Number of nodes on side A
 * @param sideB Array of node IDs on side B
 * @param countB Number of nodes on side B
 * @param durationMs Duration of partition
 * @return Partition ID (0 on failure)
 */
uint32_t kraftChaosCreateAsymmetricPartition(
    kraftChaosEngine *chaos, const uint32_t *sideA, uint32_t countA,
    const uint32_t *sideB, uint32_t countB, uint32_t durationMs);

/**
 * Heal a specific partition
 *
 * @param chaos Chaos engine
 * @param partitionId ID returned from CreatePartition
 * @return true if partition was healed
 */
bool kraftChaosHealPartition(kraftChaosEngine *chaos, uint32_t partitionId);

/**
 * Heal all active partitions
 *
 * @param chaos Chaos engine
 */
void kraftChaosHealAllPartitions(kraftChaosEngine *chaos);

/* --- Node Fault Injection --- */

/**
 * Configure node fault probabilities
 */
void kraftChaosConfigureNodes(kraftChaosEngine *chaos,
                              const kraftChaosNodeConfig *config);

/**
 * Crash a specific node
 *
 * @param chaos Chaos engine
 * @param nodeId Node to crash
 */
void kraftChaosCrashNode(kraftChaosEngine *chaos, uint32_t nodeId);

/**
 * Pause a node (simulates GC pause)
 *
 * @param chaos Chaos engine
 * @param nodeId Node to pause
 * @param durationMs Pause duration
 */
void kraftChaosPauseNode(kraftChaosEngine *chaos, uint32_t nodeId,
                         uint32_t durationMs);

/**
 * Restart a crashed node
 *
 * @param chaos Chaos engine
 * @param nodeId Node to restart
 */
void kraftChaosRestartNode(kraftChaosEngine *chaos, uint32_t nodeId);

/**
 * Resume a paused node immediately
 *
 * @param chaos Chaos engine
 * @param nodeId Node to resume
 */
void kraftChaosResumeNode(kraftChaosEngine *chaos, uint32_t nodeId);

/**
 * Make a node slow (high latency responses)
 *
 * @param chaos Chaos engine
 * @param nodeId Node to make slow
 * @param latencyMs Additional latency in ms
 */
void kraftChaosSlowNode(kraftChaosEngine *chaos, uint32_t nodeId,
                        uint32_t latencyMs);

/**
 * Restore node to healthy state
 *
 * @param chaos Chaos engine
 * @param nodeId Node to restore
 */
void kraftChaosRestoreNode(kraftChaosEngine *chaos, uint32_t nodeId);

/**
 * Restore all nodes to healthy state
 *
 * @param chaos Chaos engine
 */
void kraftChaosRestoreAllNodes(kraftChaosEngine *chaos);

/* --- Byzantine Fault Injection --- */

/**
 * Mark a node as Byzantine with specific behavior
 *
 * @param chaos Chaos engine
 * @param nodeId Node to mark as Byzantine
 * @param behavior Type of Byzantine behavior
 * @return true on success
 */
bool kraftChaosSetByzantineNode(kraftChaosEngine *chaos, uint32_t nodeId,
                                kraftByzantineBehavior behavior);

/**
 * Configure Byzantine node with detailed settings
 *
 * @param chaos Chaos engine
 * @param config Byzantine node configuration
 * @return true on success
 */
bool kraftChaosConfigureByzantineNode(kraftChaosEngine *chaos,
                                      const kraftChaosByzantineNode *config);

/**
 * Remove Byzantine behavior from a node
 *
 * @param chaos Chaos engine
 * @param nodeId Node to restore
 */
void kraftChaosClearByzantineNode(kraftChaosEngine *chaos, uint32_t nodeId);

/**
 * Clear all Byzantine nodes
 *
 * @param chaos Chaos engine
 */
void kraftChaosClearAllByzantine(kraftChaosEngine *chaos);

/* --- Clock Manipulation --- */

/**
 * Configure clock chaos
 */
void kraftChaosConfigureClock(kraftChaosEngine *chaos,
                              const kraftChaosClockConfig *config);

/**
 * Set clock offset for a specific node
 *
 * @param chaos Chaos engine
 * @param nodeId Node to affect
 * @param offsetMs Clock offset in milliseconds (positive = ahead, negative =
 * behind)
 */
void kraftChaosSetClockOffset(kraftChaosEngine *chaos, uint32_t nodeId,
                              int64_t offsetMs);

/**
 * Set clock drift factor for a node
 *
 * @param chaos Chaos engine
 * @param nodeId Node to affect
 * @param factor Drift factor (1.0 = normal, 2.0 = twice as fast, 0.5 = half
 * speed)
 */
void kraftChaosSetClockDrift(kraftChaosEngine *chaos, uint32_t nodeId,
                             float factor);

/**
 * Simulate a clock jump for a node
 *
 * @param chaos Chaos engine
 * @param nodeId Node to affect
 * @param jumpMs Jump amount (positive = forward, negative = backward)
 */
void kraftChaosClockJump(kraftChaosEngine *chaos, uint32_t nodeId,
                         int64_t jumpMs);

/* --- Event Logging --- */

/**
 * Enable event logging
 *
 * @param chaos Chaos engine
 * @param capacity Maximum events to store (ring buffer)
 */
void kraftChaosEnableLogging(kraftChaosEngine *chaos, uint32_t capacity);

/**
 * Disable event logging
 *
 * @param chaos Chaos engine
 */
void kraftChaosDisableLogging(kraftChaosEngine *chaos);

/**
 * Get logged events
 *
 * @param chaos Chaos engine
 * @param events Buffer to receive events
 * @param maxEvents Maximum events to retrieve
 * @return Number of events copied
 */
uint32_t kraftChaosGetEvents(const kraftChaosEngine *chaos,
                             kraftChaosEvent *events, uint32_t maxEvents);

/**
 * Clear event log
 *
 * @param chaos Chaos engine
 */
void kraftChaosClearEvents(kraftChaosEngine *chaos);

/* --- Statistics --- */

/**
 * Get chaos statistics
 *
 * @param chaos Chaos engine
 * @param stats Output statistics structure
 */
void kraftChaosGetStats(const kraftChaosEngine *chaos, kraftChaosStats *stats);

/**
 * Reset statistics counters
 *
 * @param chaos Chaos engine
 */
void kraftChaosResetStats(kraftChaosEngine *chaos);

/* --- Testing Helpers --- */

/**
 * Process time advancement (check for partition/pause expirations)
 * Call this periodically during tests
 *
 * @param chaos Chaos engine
 * @param currentTimeMs Current time in milliseconds
 */
void kraftChaosTick(kraftChaosEngine *chaos, uint64_t currentTimeMs);

/**
 * Check if a message should be delivered between two nodes
 * Returns the fault to apply (KRAFT_FAULT_NONE if message should be delivered)
 *
 * @param chaos Chaos engine
 * @param srcNodeId Source node
 * @param dstNodeId Destination node
 * @return Fault to apply
 */
kraftNetworkFault kraftChaosCheckDelivery(kraftChaosEngine *chaos,
                                          uint32_t srcNodeId,
                                          uint32_t dstNodeId);

/**
 * Get current node state
 *
 * @param chaos Chaos engine
 * @param nodeId Node to check
 * @return Current fault state of node
 */
kraftNodeFault kraftChaosGetNodeState(const kraftChaosEngine *chaos,
                                      uint32_t nodeId);

/**
 * Check if node is Byzantine
 *
 * @param chaos Chaos engine
 * @param nodeId Node to check
 * @return Byzantine behavior type (KRAFT_BYZANTINE_NONE if not Byzantine)
 */
kraftByzantineBehavior
kraftChaosGetByzantineBehavior(const kraftChaosEngine *chaos, uint32_t nodeId);

/**
 * Get adjusted time for a node (accounting for clock chaos)
 *
 * @param chaos Chaos engine
 * @param nodeId Node to get time for
 * @param realTimeMs Actual current time
 * @return Adjusted time as perceived by the node
 */
uint64_t kraftChaosGetNodeTime(const kraftChaosEngine *chaos, uint32_t nodeId,
                               uint64_t realTimeMs);

/* --- Scenario Presets --- */

/**
 * Apply preset: Flaky Network
 * High packet loss and delays, no Byzantine
 */
void kraftChaosPresetFlakyNetwork(kraftChaosEngine *chaos);

/**
 * Apply preset: Partition Storm
 * Frequent partitions forming and healing
 */
void kraftChaosPresetPartitionStorm(kraftChaosEngine *chaos);

/**
 * Apply preset: Byzantine Single
 * One Byzantine node with equivocation behavior
 */
void kraftChaosPresetByzantineSingle(kraftChaosEngine *chaos, uint32_t nodeId);

/**
 * Apply preset: Byzantine Minority
 * f Byzantine nodes (where n=3f+1 for safety)
 */
void kraftChaosPresetByzantineMinority(kraftChaosEngine *chaos,
                                       const uint32_t *nodeIds, uint32_t count);

/**
 * Apply preset: Clock Chaos
 * Various clock drifts and jumps across nodes
 */
void kraftChaosPresetClockChaos(kraftChaosEngine *chaos);

/**
 * Apply preset: Total Chaos
 * All fault types enabled with moderate probabilities
 */
void kraftChaosPresetTotalChaos(kraftChaosEngine *chaos);

/* --- String Helpers --- */

/**
 * Get human-readable name for network fault
 */
const char *kraftNetworkFaultName(kraftNetworkFault fault);

/**
 * Get human-readable name for node fault
 */
const char *kraftNodeFaultName(kraftNodeFault fault);

/**
 * Get human-readable name for Byzantine behavior
 */
const char *kraftByzantineBehaviorName(kraftByzantineBehavior behavior);

/**
 * Get human-readable name for clock fault
 */
const char *kraftClockFaultName(kraftClockFault fault);

#endif /* KRAFT_CHAOS_H */
