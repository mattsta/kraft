/* chaos.c - Kraft Chaos Testing Framework Implementation
 *
 * Provides comprehensive fault injection for testing Raft robustness:
 * - Network faults: drops, delays, duplicates, corruption
 * - Partitions: symmetric, asymmetric, timed
 * - Node faults: crashes, pauses, slow nodes
 * - Byzantine behaviors: equivocation, lying, selective responses
 * - Clock anomalies: drift, jumps, pauses
 */

#include "chaos.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* =========================================================================
 * Random Number Generation (xorshift64)
 * ========================================================================= */

static uint64_t chaosRandom(kraftChaosEngine *chaos) {
    uint64_t x = chaos->rngState;
    x ^= x << 13;
    x ^= x >> 7;
    x ^= x << 17;
    chaos->rngState = x;
    return x;
}

static float chaosRandomFloat(kraftChaosEngine *chaos) {
    return (float)(chaosRandom(chaos) & 0xFFFFFFFF) / (float)0xFFFFFFFF;
}

static uint32_t chaosRandomRange(kraftChaosEngine *chaos, uint32_t min,
                                 uint32_t max) {
    if (max <= min) {
        return min;
    }
    return min + (chaosRandom(chaos) % (max - min + 1));
}

/* =========================================================================
 * Event Logging
 * ========================================================================= */

static void logEvent(kraftChaosEngine *chaos, kraftNetworkFault netFault,
                     kraftNodeFault nodeFault, kraftByzantineBehavior byz,
                     uint32_t srcNode, uint32_t dstNode, const char *details) {
    if (!chaos->logEnabled || chaos->eventLog.events == NULL) {
        return;
    }

    kraftChaosEvent *event = &chaos->eventLog.events[chaos->eventLog.head];
    event->timestamp = (uint64_t)time(NULL);
    event->networkFault = netFault;
    event->nodeFault = nodeFault;
    event->byzantine = byz;
    event->sourceNode = srcNode;
    event->targetNode = dstNode;
    event->term = 0; /* Could be filled by caller if needed */
    event->index = 0;

    if (details) {
        strncpy(event->details, details, sizeof(event->details) - 1);
        event->details[sizeof(event->details) - 1] = '\0';
    } else {
        event->details[0] = '\0';
    }

    chaos->eventLog.head =
        (chaos->eventLog.head + 1) % chaos->eventLog.capacity;
    if (chaos->eventLog.count < chaos->eventLog.capacity) {
        chaos->eventLog.count++;
    }
}

/* =========================================================================
 * Engine Lifecycle
 * ========================================================================= */

kraftChaosEngine *kraftChaosEngineNew(void) {
    return kraftChaosEngineNewWithSeed((uint64_t)time(NULL) ^
                                       (uint64_t)clock());
}

kraftChaosEngine *kraftChaosEngineNewWithSeed(uint64_t seed) {
    kraftChaosEngine *chaos = calloc(1, sizeof(*chaos));
    if (!chaos) {
        return NULL;
    }

    chaos->seed = seed;
    chaos->rngState = seed ? seed : 1;
    chaos->enabled = true;

    /* Initialize default configurations */
    chaos->network.minDelayMs = 1;
    chaos->network.maxDelayMs = 100;
    chaos->network.maxDuplicates = 3;

    chaos->partition.minDurationMs = 100;
    chaos->partition.maxDurationMs = 5000;
    chaos->partition.maxPartitions = 16;

    chaos->node.minPauseDurationMs = 50;
    chaos->node.maxPauseDurationMs = 1000;
    chaos->node.restartDelayMs = 100;

    chaos->clock.driftFactor = 1.0f;
    chaos->clock.maxJumpMs = 10000;

    /* Allocate initial Byzantine node array */
    chaos->byzantineNodeCapacity = 8;
    chaos->byzantineNodes =
        calloc(chaos->byzantineNodeCapacity, sizeof(kraftChaosByzantineNode));
    if (!chaos->byzantineNodes) {
        free(chaos);
        return NULL;
    }

    /* Allocate initial partition array */
    chaos->partitionCapacity = 16;
    chaos->partitions =
        calloc(chaos->partitionCapacity, sizeof(kraftChaosPartition));
    if (!chaos->partitions) {
        free(chaos->byzantineNodes);
        free(chaos);
        return NULL;
    }

    return chaos;
}

void kraftChaosEngineFree(kraftChaosEngine *chaos) {
    if (!chaos) {
        return;
    }

    /* Free partition isolated node arrays */
    for (uint32_t i = 0; i < chaos->partitionCount; i++) {
        free(chaos->partitions[i].isolatedNodes);
    }
    free(chaos->partitions);

    free(chaos->byzantineNodes);
    free(chaos->nodeStates);
    free(chaos->nodePauseEnd);
    free(chaos->clockOffsets);
    free(chaos->clockDriftFactors);
    free(chaos->eventLog.events);
    free(chaos);
}

bool kraftChaosAttach(kraftChaosEngine *chaos, struct kraftState *state) {
    if (!chaos) {
        return false;
    }

    chaos->attachedState = state;

    /* Allocate per-node state arrays - assume reasonable max for testing */
    uint32_t maxNodes = 64;
    chaos->nodeCount = maxNodes;

    chaos->nodeStates = calloc(maxNodes, sizeof(kraftNodeFault));
    chaos->nodePauseEnd = calloc(maxNodes, sizeof(uint64_t));
    chaos->clockOffsets = calloc(maxNodes, sizeof(int64_t));
    chaos->clockDriftFactors = calloc(maxNodes, sizeof(float));

    if (!chaos->nodeStates || !chaos->nodePauseEnd || !chaos->clockOffsets ||
        !chaos->clockDriftFactors) {
        return false;
    }

    /* Initialize clock drift factors to 1.0 (normal speed) */
    for (uint32_t i = 0; i < maxNodes; i++) {
        chaos->clockDriftFactors[i] = 1.0f;
    }

    return true;
}

void kraftChaosDetach(kraftChaosEngine *chaos) {
    if (!chaos) {
        return;
    }
    chaos->attachedState = NULL;
}

void kraftChaosSetEnabled(kraftChaosEngine *chaos, bool enabled) {
    if (chaos) {
        chaos->enabled = enabled;
    }
}

/* =========================================================================
 * Network Configuration
 * ========================================================================= */

void kraftChaosConfigureNetwork(kraftChaosEngine *chaos,
                                const kraftChaosNetworkConfig *config) {
    if (!chaos || !config) {
        return;
    }
    chaos->network = *config;
}

void kraftChaosSetDropProbability(kraftChaosEngine *chaos, float probability) {
    if (chaos) {
        chaos->network.dropProbability = probability;
    }
}

void kraftChaosSetDelay(kraftChaosEngine *chaos, float probability,
                        uint32_t minMs, uint32_t maxMs) {
    if (!chaos) {
        return;
    }
    chaos->network.delayProbability = probability;
    chaos->network.minDelayMs = minMs;
    chaos->network.maxDelayMs = maxMs;
}

void kraftChaosSetDuplicateProbability(kraftChaosEngine *chaos,
                                       float probability) {
    if (chaos) {
        chaos->network.duplicateProbability = probability;
    }
}

void kraftChaosSetCorruptProbability(kraftChaosEngine *chaos,
                                     float probability) {
    if (chaos) {
        chaos->network.corruptProbability = probability;
    }
}

/* =========================================================================
 * Partition Management
 * ========================================================================= */

void kraftChaosConfigurePartitions(kraftChaosEngine *chaos,
                                   const kraftChaosPartitionConfig *config) {
    if (!chaos || !config) {
        return;
    }
    chaos->partition = *config;
}

static uint32_t findFreePartitionSlot(kraftChaosEngine *chaos) {
    for (uint32_t i = 0; i < chaos->partitionCapacity; i++) {
        if (!chaos->partitions[i].active) {
            return i;
        }
    }

    /* Grow array if needed */
    uint32_t newCap = chaos->partitionCapacity * 2;
    kraftChaosPartition *newPartitions =
        realloc(chaos->partitions, newCap * sizeof(kraftChaosPartition));
    if (!newPartitions) {
        return UINT32_MAX;
    }

    memset(&newPartitions[chaos->partitionCapacity], 0,
           (newCap - chaos->partitionCapacity) * sizeof(kraftChaosPartition));

    chaos->partitions = newPartitions;
    uint32_t slot = chaos->partitionCapacity;
    chaos->partitionCapacity = newCap;
    return slot;
}

uint32_t kraftChaosCreatePartition(kraftChaosEngine *chaos,
                                   const uint32_t *nodeIds, uint32_t count,
                                   uint32_t durationMs) {
    if (!chaos || !nodeIds || count == 0) {
        return 0;
    }
    if (chaos->partitionCount >= chaos->partition.maxPartitions) {
        return 0;
    }

    uint32_t slot = findFreePartitionSlot(chaos);
    if (slot == UINT32_MAX) {
        return 0;
    }

    kraftChaosPartition *part = &chaos->partitions[slot];
    part->id = slot + 1;                           /* IDs start at 1 */
    part->startTime = (uint64_t)time(NULL) * 1000; /* Convert to ms */
    part->endTime = durationMs ? (part->startTime + durationMs) : 0;
    part->active = true;
    part->asymmetric = false;

    part->isolatedNodes = malloc(count * sizeof(uint32_t));
    if (!part->isolatedNodes) {
        part->active = false;
        return 0;
    }
    memcpy(part->isolatedNodes, nodeIds, count * sizeof(uint32_t));
    part->isolatedCount = count;

    chaos->partitionCount++;
    chaos->partitionsCreated++;

    logEvent(chaos, KRAFT_FAULT_NONE, KRAFT_NODE_PARTITIONED,
             KRAFT_BYZANTINE_NONE, nodeIds[0], 0, "Partition created");

    return part->id;
}

uint32_t kraftChaosCreateAsymmetricPartition(
    kraftChaosEngine *chaos, const uint32_t *sideA, uint32_t countA,
    const uint32_t *sideB, uint32_t countB, uint32_t durationMs) {
    if (!chaos || !sideA || !sideB || countA == 0 || countB == 0) {
        return 0;
    }

    /* Create partition with sideA as isolated (they can't reach sideB) */
    uint32_t id = kraftChaosCreatePartition(chaos, sideA, countA, durationMs);
    if (id == 0) {
        return 0;
    }

    /* Mark as asymmetric */
    chaos->partitions[id - 1].asymmetric = true;
    chaos->partitions[id - 1].asymmetricDir = 0; /* A -> B blocked */

    return id;
}

bool kraftChaosHealPartition(kraftChaosEngine *chaos, uint32_t partitionId) {
    if (!chaos || partitionId == 0 || partitionId > chaos->partitionCapacity) {
        return false;
    }

    kraftChaosPartition *part = &chaos->partitions[partitionId - 1];
    if (!part->active) {
        return false;
    }

    free(part->isolatedNodes);
    part->isolatedNodes = NULL;
    part->isolatedCount = 0;
    part->active = false;
    chaos->partitionCount--;

    logEvent(chaos, KRAFT_FAULT_NONE, KRAFT_NODE_HEALTHY, KRAFT_BYZANTINE_NONE,
             0, 0, "Partition healed");

    return true;
}

void kraftChaosHealAllPartitions(kraftChaosEngine *chaos) {
    if (!chaos) {
        return;
    }

    for (uint32_t i = 0; i < chaos->partitionCapacity; i++) {
        if (chaos->partitions[i].active) {
            kraftChaosHealPartition(chaos, i + 1);
        }
    }
}

/* Check if src can reach dst given active partitions */
static bool isPartitioned(kraftChaosEngine *chaos, uint32_t srcNode,
                          uint32_t dstNode) {
    for (uint32_t i = 0; i < chaos->partitionCapacity; i++) {
        kraftChaosPartition *part = &chaos->partitions[i];
        if (!part->active) {
            continue;
        }

        bool srcIsolated = false;
        bool dstIsolated = false;

        for (uint32_t j = 0; j < part->isolatedCount; j++) {
            if (part->isolatedNodes[j] == srcNode) {
                srcIsolated = true;
            }
            if (part->isolatedNodes[j] == dstNode) {
                dstIsolated = true;
            }
        }

        /* If symmetric partition: isolated nodes can't reach non-isolated and
         * vice versa */
        if (!part->asymmetric) {
            if (srcIsolated != dstIsolated) {
                return true;
            }
        } else {
            /* Asymmetric: only one direction blocked */
            if (srcIsolated && !dstIsolated) {
                return true;
            }
        }
    }
    return false;
}

/* =========================================================================
 * Node Fault Injection
 * ========================================================================= */

void kraftChaosConfigureNodes(kraftChaosEngine *chaos,
                              const kraftChaosNodeConfig *config) {
    if (!chaos || !config) {
        return;
    }
    chaos->node = *config;
}

void kraftChaosCrashNode(kraftChaosEngine *chaos, uint32_t nodeId) {
    if (!chaos || nodeId >= chaos->nodeCount) {
        return;
    }
    chaos->nodeStates[nodeId] = KRAFT_NODE_CRASHED;
    chaos->nodesCrashed++;
    logEvent(chaos, KRAFT_FAULT_NONE, KRAFT_NODE_CRASHED, KRAFT_BYZANTINE_NONE,
             nodeId, 0, "Node crashed");
}

void kraftChaosPauseNode(kraftChaosEngine *chaos, uint32_t nodeId,
                         uint32_t durationMs) {
    if (!chaos || nodeId >= chaos->nodeCount) {
        return;
    }
    chaos->nodeStates[nodeId] = KRAFT_NODE_PAUSED;
    chaos->nodePauseEnd[nodeId] = (uint64_t)time(NULL) * 1000 + durationMs;
    chaos->nodesPaused++;
    logEvent(chaos, KRAFT_FAULT_NONE, KRAFT_NODE_PAUSED, KRAFT_BYZANTINE_NONE,
             nodeId, 0, "Node paused (GC simulation)");
}

void kraftChaosRestartNode(kraftChaosEngine *chaos, uint32_t nodeId) {
    if (!chaos || nodeId >= chaos->nodeCount) {
        return;
    }
    if (chaos->nodeStates[nodeId] == KRAFT_NODE_CRASHED) {
        chaos->nodeStates[nodeId] = KRAFT_NODE_HEALTHY;
        logEvent(chaos, KRAFT_FAULT_NONE, KRAFT_NODE_HEALTHY,
                 KRAFT_BYZANTINE_NONE, nodeId, 0, "Node restarted");
    }
}

void kraftChaosResumeNode(kraftChaosEngine *chaos, uint32_t nodeId) {
    if (!chaos || nodeId >= chaos->nodeCount) {
        return;
    }
    if (chaos->nodeStates[nodeId] == KRAFT_NODE_PAUSED) {
        chaos->nodeStates[nodeId] = KRAFT_NODE_HEALTHY;
        chaos->nodePauseEnd[nodeId] = 0;
        logEvent(chaos, KRAFT_FAULT_NONE, KRAFT_NODE_HEALTHY,
                 KRAFT_BYZANTINE_NONE, nodeId, 0, "Node resumed");
    }
}

void kraftChaosSlowNode(kraftChaosEngine *chaos, uint32_t nodeId,
                        uint32_t latencyMs) {
    if (!chaos || nodeId >= chaos->nodeCount) {
        return;
    }
    (void)latencyMs; /* Stored elsewhere if needed */
    chaos->nodeStates[nodeId] = KRAFT_NODE_SLOW;
    logEvent(chaos, KRAFT_FAULT_NONE, KRAFT_NODE_SLOW, KRAFT_BYZANTINE_NONE,
             nodeId, 0, "Node slowed");
}

void kraftChaosRestoreNode(kraftChaosEngine *chaos, uint32_t nodeId) {
    if (!chaos || nodeId >= chaos->nodeCount) {
        return;
    }
    chaos->nodeStates[nodeId] = KRAFT_NODE_HEALTHY;
    chaos->nodePauseEnd[nodeId] = 0;
}

void kraftChaosRestoreAllNodes(kraftChaosEngine *chaos) {
    if (!chaos) {
        return;
    }
    for (uint32_t i = 0; i < chaos->nodeCount; i++) {
        chaos->nodeStates[i] = KRAFT_NODE_HEALTHY;
        chaos->nodePauseEnd[i] = 0;
    }
}

/* =========================================================================
 * Byzantine Fault Injection
 * ========================================================================= */

bool kraftChaosSetByzantineNode(kraftChaosEngine *chaos, uint32_t nodeId,
                                kraftByzantineBehavior behavior) {
    kraftChaosByzantineNode config = {.nodeId = nodeId,
                                      .behavior = behavior,
                                      .targetNodeMask = 0, /* All nodes */
                                      .activationProbability = 1.0f};
    return kraftChaosConfigureByzantineNode(chaos, &config);
}

bool kraftChaosConfigureByzantineNode(kraftChaosEngine *chaos,
                                      const kraftChaosByzantineNode *config) {
    if (!chaos || !config) {
        return false;
    }

    /* Check if node already exists */
    for (uint32_t i = 0; i < chaos->byzantineNodeCount; i++) {
        if (chaos->byzantineNodes[i].nodeId == config->nodeId) {
            chaos->byzantineNodes[i] = *config;
            return true;
        }
    }

    /* Add new Byzantine node */
    if (chaos->byzantineNodeCount >= chaos->byzantineNodeCapacity) {
        uint32_t newCap = chaos->byzantineNodeCapacity * 2;
        kraftChaosByzantineNode *newNodes = realloc(
            chaos->byzantineNodes, newCap * sizeof(kraftChaosByzantineNode));
        if (!newNodes) {
            return false;
        }
        chaos->byzantineNodes = newNodes;
        chaos->byzantineNodeCapacity = newCap;
    }

    chaos->byzantineNodes[chaos->byzantineNodeCount++] = *config;

    if (config->nodeId < chaos->nodeCount) {
        chaos->nodeStates[config->nodeId] = KRAFT_NODE_BYZANTINE;
    }

    logEvent(chaos, KRAFT_FAULT_NONE, KRAFT_NODE_BYZANTINE, config->behavior,
             config->nodeId, 0, "Node marked Byzantine");

    return true;
}

void kraftChaosClearByzantineNode(kraftChaosEngine *chaos, uint32_t nodeId) {
    if (!chaos) {
        return;
    }

    for (uint32_t i = 0; i < chaos->byzantineNodeCount; i++) {
        if (chaos->byzantineNodes[i].nodeId == nodeId) {
            /* Shift remaining nodes */
            memmove(&chaos->byzantineNodes[i], &chaos->byzantineNodes[i + 1],
                    (chaos->byzantineNodeCount - i - 1) *
                        sizeof(kraftChaosByzantineNode));
            chaos->byzantineNodeCount--;

            if (nodeId < chaos->nodeCount) {
                chaos->nodeStates[nodeId] = KRAFT_NODE_HEALTHY;
            }
            return;
        }
    }
}

void kraftChaosClearAllByzantine(kraftChaosEngine *chaos) {
    if (!chaos) {
        return;
    }

    for (uint32_t i = 0; i < chaos->byzantineNodeCount; i++) {
        uint32_t nodeId = chaos->byzantineNodes[i].nodeId;
        if (nodeId < chaos->nodeCount) {
            chaos->nodeStates[nodeId] = KRAFT_NODE_HEALTHY;
        }
    }
    chaos->byzantineNodeCount = 0;
}

/* =========================================================================
 * Clock Manipulation
 * ========================================================================= */

void kraftChaosConfigureClock(kraftChaosEngine *chaos,
                              const kraftChaosClockConfig *config) {
    if (!chaos || !config) {
        return;
    }
    chaos->clock = *config;
}

void kraftChaosSetClockOffset(kraftChaosEngine *chaos, uint32_t nodeId,
                              int64_t offsetMs) {
    if (!chaos || nodeId >= chaos->nodeCount) {
        return;
    }
    chaos->clockOffsets[nodeId] = offsetMs;
}

void kraftChaosSetClockDrift(kraftChaosEngine *chaos, uint32_t nodeId,
                             float factor) {
    if (!chaos || nodeId >= chaos->nodeCount) {
        return;
    }
    chaos->clockDriftFactors[nodeId] = factor;
}

void kraftChaosClockJump(kraftChaosEngine *chaos, uint32_t nodeId,
                         int64_t jumpMs) {
    if (!chaos || nodeId >= chaos->nodeCount) {
        return;
    }
    chaos->clockOffsets[nodeId] += jumpMs;
    logEvent(chaos, KRAFT_FAULT_NONE, KRAFT_NODE_HEALTHY, KRAFT_BYZANTINE_NONE,
             nodeId, 0,
             jumpMs > 0 ? "Clock jumped forward" : "Clock jumped backward");
}

/* =========================================================================
 * Event Logging
 * ========================================================================= */

void kraftChaosEnableLogging(kraftChaosEngine *chaos, uint32_t capacity) {
    if (!chaos || capacity == 0) {
        return;
    }

    free(chaos->eventLog.events);
    chaos->eventLog.events = calloc(capacity, sizeof(kraftChaosEvent));
    if (chaos->eventLog.events) {
        chaos->eventLog.capacity = capacity;
        chaos->eventLog.head = 0;
        chaos->eventLog.count = 0;
        chaos->logEnabled = true;
    }
}

void kraftChaosDisableLogging(kraftChaosEngine *chaos) {
    if (!chaos) {
        return;
    }
    chaos->logEnabled = false;
}

uint32_t kraftChaosGetEvents(const kraftChaosEngine *chaos,
                             kraftChaosEvent *events, uint32_t maxEvents) {
    if (!chaos || !events || !chaos->eventLog.events) {
        return 0;
    }

    uint32_t toCopy =
        (maxEvents < chaos->eventLog.count) ? maxEvents : chaos->eventLog.count;
    uint32_t start = (chaos->eventLog.head + chaos->eventLog.capacity -
                      chaos->eventLog.count) %
                     chaos->eventLog.capacity;

    for (uint32_t i = 0; i < toCopy; i++) {
        events[i] =
            chaos->eventLog.events[(start + i) % chaos->eventLog.capacity];
    }

    return toCopy;
}

void kraftChaosClearEvents(kraftChaosEngine *chaos) {
    if (!chaos) {
        return;
    }
    chaos->eventLog.head = 0;
    chaos->eventLog.count = 0;
}

/* =========================================================================
 * Statistics
 * ========================================================================= */

void kraftChaosGetStats(const kraftChaosEngine *chaos, kraftChaosStats *stats) {
    if (!chaos || !stats) {
        return;
    }

    stats->messagesDropped = chaos->messagesDropped;
    stats->messagesDelayed = chaos->messagesDelayed;
    stats->messagesDuplicated = chaos->messagesDuplicated;
    stats->messagesCorrupted = chaos->messagesCorrupted;
    stats->partitionsCreated = chaos->partitionsCreated;
    stats->activePartitions = chaos->partitionCount;
    stats->nodesCrashed = chaos->nodesCrashed;
    stats->nodesPaused = chaos->nodesPaused;
    stats->byzantineActions = chaos->byzantineActions;
    stats->totalEventsLogged = chaos->eventLog.count;

    /* Count active crashes */
    stats->activeCrashes = 0;
    for (uint32_t i = 0; i < chaos->nodeCount; i++) {
        if (chaos->nodeStates[i] == KRAFT_NODE_CRASHED) {
            stats->activeCrashes++;
        }
    }
}

void kraftChaosResetStats(kraftChaosEngine *chaos) {
    if (!chaos) {
        return;
    }

    chaos->messagesDropped = 0;
    chaos->messagesDelayed = 0;
    chaos->messagesDuplicated = 0;
    chaos->messagesCorrupted = 0;
    chaos->partitionsCreated = 0;
    chaos->nodesCrashed = 0;
    chaos->nodesPaused = 0;
    chaos->byzantineActions = 0;
}

/* =========================================================================
 * Testing Helpers
 * ========================================================================= */

void kraftChaosTick(kraftChaosEngine *chaos, uint64_t currentTimeMs) {
    if (!chaos) {
        return;
    }

    /* Check for partition expirations */
    for (uint32_t i = 0; i < chaos->partitionCapacity; i++) {
        kraftChaosPartition *part = &chaos->partitions[i];
        if (part->active && part->endTime > 0 &&
            currentTimeMs >= part->endTime) {
            kraftChaosHealPartition(chaos, i + 1);
        }
    }

    /* Check for pause expirations */
    for (uint32_t i = 0; i < chaos->nodeCount; i++) {
        if (chaos->nodeStates[i] == KRAFT_NODE_PAUSED &&
            chaos->nodePauseEnd[i] > 0 &&
            currentTimeMs >= chaos->nodePauseEnd[i]) {
            kraftChaosResumeNode(chaos, i);
        }
    }

    /* Random fault injection based on probabilities */
    if (chaos->enabled) {
        /* Random partition creation */
        if (chaos->partition.partitionProbability > 0 &&
            chaosRandomFloat(chaos) <
                chaos->partition.partitionProbability / 100.0f) {
            /* Create random partition - pick random node to isolate */
            uint32_t nodeToIsolate = chaosRandom(chaos) % chaos->nodeCount;
            uint32_t duration =
                chaosRandomRange(chaos, chaos->partition.minDurationMs,
                                 chaos->partition.maxDurationMs);
            kraftChaosCreatePartition(chaos, &nodeToIsolate, 1, duration);
        }

        /* Random node crashes */
        if (chaos->node.crashProbability > 0 &&
            chaosRandomFloat(chaos) < chaos->node.crashProbability / 100.0f) {
            uint32_t nodeToCrash = chaosRandom(chaos) % chaos->nodeCount;
            if (chaos->nodeStates[nodeToCrash] == KRAFT_NODE_HEALTHY) {
                kraftChaosCrashNode(chaos, nodeToCrash);
            }
        }

        /* Random node pauses */
        if (chaos->node.pauseProbability > 0 &&
            chaosRandomFloat(chaos) < chaos->node.pauseProbability / 100.0f) {
            uint32_t nodeToPause = chaosRandom(chaos) % chaos->nodeCount;
            if (chaos->nodeStates[nodeToPause] == KRAFT_NODE_HEALTHY) {
                uint32_t duration =
                    chaosRandomRange(chaos, chaos->node.minPauseDurationMs,
                                     chaos->node.maxPauseDurationMs);
                kraftChaosPauseNode(chaos, nodeToPause, duration);
            }
        }

        /* Random restarts */
        if (chaos->node.restartProbability > 0 &&
            chaosRandomFloat(chaos) < chaos->node.restartProbability / 100.0f) {
            for (uint32_t i = 0; i < chaos->nodeCount; i++) {
                if (chaos->nodeStates[i] == KRAFT_NODE_CRASHED) {
                    kraftChaosRestartNode(chaos, i);
                    break;
                }
            }
        }
    }
}

kraftNetworkFault kraftChaosCheckDelivery(kraftChaosEngine *chaos,
                                          uint32_t srcNodeId,
                                          uint32_t dstNodeId) {
    if (!chaos || !chaos->enabled) {
        return KRAFT_FAULT_NONE;
    }

    /* Check if source or destination is crashed/paused */
    if (srcNodeId < chaos->nodeCount) {
        kraftNodeFault srcState = chaos->nodeStates[srcNodeId];
        if (srcState == KRAFT_NODE_CRASHED || srcState == KRAFT_NODE_PAUSED) {
            return KRAFT_FAULT_DROP;
        }
    }

    if (dstNodeId < chaos->nodeCount) {
        kraftNodeFault dstState = chaos->nodeStates[dstNodeId];
        if (dstState == KRAFT_NODE_CRASHED || dstState == KRAFT_NODE_PAUSED) {
            return KRAFT_FAULT_DROP;
        }
    }

    /* Check partitions */
    if (isPartitioned(chaos, srcNodeId, dstNodeId)) {
        chaos->messagesDropped++;
        logEvent(chaos, KRAFT_FAULT_DROP, KRAFT_NODE_PARTITIONED,
                 KRAFT_BYZANTINE_NONE, srcNodeId, dstNodeId,
                 "Message blocked by partition");
        return KRAFT_FAULT_DROP;
    }

    /* Random drop */
    if (chaos->network.dropProbability > 0 &&
        chaosRandomFloat(chaos) < chaos->network.dropProbability) {
        chaos->messagesDropped++;
        logEvent(chaos, KRAFT_FAULT_DROP, KRAFT_NODE_HEALTHY,
                 KRAFT_BYZANTINE_NONE, srcNodeId, dstNodeId,
                 "Message randomly dropped");
        return KRAFT_FAULT_DROP;
    }

    /* Random corruption (Byzantine) */
    if (chaos->network.corruptProbability > 0 &&
        chaosRandomFloat(chaos) < chaos->network.corruptProbability) {
        chaos->messagesCorrupted++;
        uint32_t corruptType = chaosRandom(chaos) % 3;
        kraftNetworkFault fault = KRAFT_FAULT_CORRUPT_DATA;
        if (corruptType == 1) {
            fault = KRAFT_FAULT_CORRUPT_TERM;
        } else if (corruptType == 2) {
            fault = KRAFT_FAULT_CORRUPT_INDEX;
        }
        logEvent(chaos, fault, KRAFT_NODE_HEALTHY, KRAFT_BYZANTINE_NONE,
                 srcNodeId, dstNodeId, "Message corrupted");
        return fault;
    }

    /* Random delay */
    if (chaos->network.delayProbability > 0 &&
        chaosRandomFloat(chaos) < chaos->network.delayProbability) {
        chaos->messagesDelayed++;
        return KRAFT_FAULT_DELAY;
    }

    /* Random duplicate */
    if (chaos->network.duplicateProbability > 0 &&
        chaosRandomFloat(chaos) < chaos->network.duplicateProbability) {
        chaos->messagesDuplicated++;
        return KRAFT_FAULT_DUPLICATE;
    }

    /* Random reorder */
    if (chaos->network.reorderProbability > 0 &&
        chaosRandomFloat(chaos) < chaos->network.reorderProbability) {
        return KRAFT_FAULT_REORDER;
    }

    return KRAFT_FAULT_NONE;
}

kraftNodeFault kraftChaosGetNodeState(const kraftChaosEngine *chaos,
                                      uint32_t nodeId) {
    if (!chaos || nodeId >= chaos->nodeCount) {
        return KRAFT_NODE_HEALTHY;
    }
    return chaos->nodeStates[nodeId];
}

kraftByzantineBehavior
kraftChaosGetByzantineBehavior(const kraftChaosEngine *chaos, uint32_t nodeId) {
    if (!chaos) {
        return KRAFT_BYZANTINE_NONE;
    }

    for (uint32_t i = 0; i < chaos->byzantineNodeCount; i++) {
        if (chaos->byzantineNodes[i].nodeId == nodeId) {
            return chaos->byzantineNodes[i].behavior;
        }
    }
    return KRAFT_BYZANTINE_NONE;
}

uint64_t kraftChaosGetNodeTime(const kraftChaosEngine *chaos, uint32_t nodeId,
                               uint64_t realTimeMs) {
    if (!chaos || nodeId >= chaos->nodeCount) {
        return realTimeMs;
    }

    int64_t offset = chaos->clockOffsets[nodeId];
    float drift = chaos->clockDriftFactors[nodeId];

    /* Apply drift factor to elapsed time and add offset */
    return (uint64_t)((int64_t)(realTimeMs * drift) + offset);
}

/* =========================================================================
 * Scenario Presets
 * ========================================================================= */

void kraftChaosPresetFlakyNetwork(kraftChaosEngine *chaos) {
    if (!chaos) {
        return;
    }

    chaos->network.dropProbability = 0.1f;       /* 10% packet loss */
    chaos->network.delayProbability = 0.2f;      /* 20% delayed */
    chaos->network.duplicateProbability = 0.05f; /* 5% duplicates */
    chaos->network.reorderProbability = 0.1f;    /* 10% reordered */
    chaos->network.corruptProbability = 0.0f;    /* No corruption */
    chaos->network.minDelayMs = 10;
    chaos->network.maxDelayMs = 500;
}

void kraftChaosPresetPartitionStorm(kraftChaosEngine *chaos) {
    if (!chaos) {
        return;
    }

    chaos->partition.partitionProbability = 0.05f; /* 5% chance per tick */
    chaos->partition.minDurationMs = 500;
    chaos->partition.maxDurationMs = 3000;
    chaos->partition.asymmetric = true;
    chaos->partition.maxPartitions = 8;
}

void kraftChaosPresetByzantineSingle(kraftChaosEngine *chaos, uint32_t nodeId) {
    if (!chaos) {
        return;
    }

    kraftChaosByzantineNode config = {
        .nodeId = nodeId,
        .behavior = KRAFT_BYZANTINE_EQUIVOCATE,
        .targetNodeMask = 0,
        .activationProbability = 0.5f /* 50% of messages are Byzantine */
    };
    kraftChaosConfigureByzantineNode(chaos, &config);
}

void kraftChaosPresetByzantineMinority(kraftChaosEngine *chaos,
                                       const uint32_t *nodeIds,
                                       uint32_t count) {
    if (!chaos || !nodeIds) {
        return;
    }

    kraftByzantineBehavior behaviors[] = {
        KRAFT_BYZANTINE_EQUIVOCATE, KRAFT_BYZANTINE_LIE_VOTE,
        KRAFT_BYZANTINE_FUTURE_TERM, KRAFT_BYZANTINE_SELECTIVE};

    for (uint32_t i = 0; i < count; i++) {
        kraftChaosByzantineNode config = {.nodeId = nodeIds[i],
                                          .behavior = behaviors[i % 4],
                                          .targetNodeMask = 0,
                                          .activationProbability = 0.7f};
        kraftChaosConfigureByzantineNode(chaos, &config);
    }
}

void kraftChaosPresetClockChaos(kraftChaosEngine *chaos) {
    if (!chaos) {
        return;
    }

    chaos->clock.driftProbability = 0.3f;
    chaos->clock.jumpProbability = 0.1f;
    chaos->clock.driftFactor = 1.5f; /* Up to 1.5x speed */
    chaos->clock.maxJumpMs = 30000;  /* Up to 30 second jumps */

    /* Apply varying drift to each node */
    for (uint32_t i = 0; i < chaos->nodeCount; i++) {
        float drift = 0.8f + chaosRandomFloat(chaos) * 0.4f; /* 0.8 - 1.2x */
        chaos->clockDriftFactors[i] = drift;
    }
}

void kraftChaosPresetTotalChaos(kraftChaosEngine *chaos) {
    if (!chaos) {
        return;
    }

    /* Network chaos */
    chaos->network.dropProbability = 0.05f;
    chaos->network.delayProbability = 0.1f;
    chaos->network.duplicateProbability = 0.03f;
    chaos->network.reorderProbability = 0.05f;
    chaos->network.corruptProbability = 0.01f;

    /* Partition chaos */
    chaos->partition.partitionProbability = 0.02f;
    chaos->partition.minDurationMs = 200;
    chaos->partition.maxDurationMs = 2000;
    chaos->partition.asymmetric = true;

    /* Node chaos */
    chaos->node.crashProbability = 0.01f;
    chaos->node.pauseProbability = 0.02f;
    chaos->node.restartProbability = 0.1f;

    /* Clock chaos */
    kraftChaosPresetClockChaos(chaos);
}

/* =========================================================================
 * String Helpers
 * ========================================================================= */

const char *kraftNetworkFaultName(kraftNetworkFault fault) {
    switch (fault) {
    case KRAFT_FAULT_NONE:
        return "None";
    case KRAFT_FAULT_DROP:
        return "Drop";
    case KRAFT_FAULT_DELAY:
        return "Delay";
    case KRAFT_FAULT_DUPLICATE:
        return "Duplicate";
    case KRAFT_FAULT_REORDER:
        return "Reorder";
    case KRAFT_FAULT_CORRUPT_DATA:
        return "CorruptData";
    case KRAFT_FAULT_CORRUPT_TERM:
        return "CorruptTerm";
    case KRAFT_FAULT_CORRUPT_INDEX:
        return "CorruptIndex";
    default:
        return "Unknown";
    }
}

const char *kraftNodeFaultName(kraftNodeFault fault) {
    switch (fault) {
    case KRAFT_NODE_HEALTHY:
        return "Healthy";
    case KRAFT_NODE_CRASHED:
        return "Crashed";
    case KRAFT_NODE_PARTITIONED:
        return "Partitioned";
    case KRAFT_NODE_SLOW:
        return "Slow";
    case KRAFT_NODE_PAUSED:
        return "Paused";
    case KRAFT_NODE_BYZANTINE:
        return "Byzantine";
    default:
        return "Unknown";
    }
}

const char *kraftByzantineBehaviorName(kraftByzantineBehavior behavior) {
    switch (behavior) {
    case KRAFT_BYZANTINE_NONE:
        return "None";
    case KRAFT_BYZANTINE_SILENT:
        return "Silent";
    case KRAFT_BYZANTINE_SELECTIVE:
        return "Selective";
    case KRAFT_BYZANTINE_LIE_VOTE:
        return "LieVote";
    case KRAFT_BYZANTINE_EQUIVOCATE:
        return "Equivocate";
    case KRAFT_BYZANTINE_FAKE_LEADER:
        return "FakeLeader";
    case KRAFT_BYZANTINE_FUTURE_TERM:
        return "FutureTerm";
    case KRAFT_BYZANTINE_CORRUPT_LOG:
        return "CorruptLog";
    case KRAFT_BYZANTINE_DELAY_VOTES:
        return "DelayVotes";
    default:
        return "Unknown";
    }
}

const char *kraftClockFaultName(kraftClockFault fault) {
    switch (fault) {
    case KRAFT_CLOCK_NORMAL:
        return "Normal";
    case KRAFT_CLOCK_FAST:
        return "Fast";
    case KRAFT_CLOCK_SLOW:
        return "Slow";
    case KRAFT_CLOCK_JUMP_FORWARD:
        return "JumpForward";
    case KRAFT_CLOCK_JUMP_BACKWARD:
        return "JumpBackward";
    case KRAFT_CLOCK_PAUSE:
        return "Pause";
    default:
        return "Unknown";
    }
}
