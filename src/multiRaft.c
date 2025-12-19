/* Kraft Multi-Raft Groups Implementation
 *
 * Manages multiple independent Raft consensus groups within a single process.
 * Provides centralized group registry, message routing, and load balancing.
 */

#include "multiRaft.h"
#include "../deps/datakit/src/datakit.h"
#include "kraft.h"
#include "leadershipTransfer.h"
#include "rpc.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

/* =========================================================================
 * Internal Helpers
 * ========================================================================= */

/* Simple hash function for group ID lookup */
static uint32_t groupIdHash(kraftGroupId groupId) {
    /* FNV-1a hash */
    uint32_t hash = 2166136261u;
    for (int i = 0; i < 8; i++) {
        hash ^= (groupId >> (i * 8)) & 0xFF;
        hash *= 16777619u;
    }
    return hash;
}

/* Simple hash table entry for group lookup */
typedef struct groupIndexEntry {
    kraftGroupId groupId;
    kraftGroup *group;
    struct groupIndexEntry *next;
} groupIndexEntry;

typedef struct groupIndex {
    groupIndexEntry **buckets;
    uint32_t bucketCount;
    uint32_t entryCount;
} groupIndex;

/* Initialize group index */
static bool groupIndexInit(groupIndex *idx, uint32_t initialBuckets) {
    idx->bucketCount = initialBuckets > 0 ? initialBuckets : 16;
    idx->buckets = zcalloc(idx->bucketCount, sizeof(groupIndexEntry *));
    if (!idx->buckets) {
        return false;
    }
    idx->entryCount = 0;
    return true;
}

/* Free group index */
static void groupIndexFree(groupIndex *idx) {
    if (!idx || !idx->buckets) {
        return;
    }

    for (uint32_t i = 0; i < idx->bucketCount; i++) {
        groupIndexEntry *entry = idx->buckets[i];
        while (entry) {
            groupIndexEntry *next = entry->next;
            zfree(entry);
            entry = next;
        }
    }
    zfree(idx->buckets);
    idx->buckets = NULL;
    idx->bucketCount = 0;
    idx->entryCount = 0;
}

/* Insert into group index */
static bool groupIndexInsert(groupIndex *idx, kraftGroupId groupId,
                             kraftGroup *group) {
    uint32_t bucket = groupIdHash(groupId) % idx->bucketCount;

    /* Check for existing entry */
    groupIndexEntry *entry = idx->buckets[bucket];
    while (entry) {
        if (entry->groupId == groupId) {
            entry->group = group; /* Update existing */
            return true;
        }
        entry = entry->next;
    }

    /* Create new entry */
    entry = zmalloc(sizeof(groupIndexEntry));
    if (!entry) {
        return false;
    }

    entry->groupId = groupId;
    entry->group = group;
    entry->next = idx->buckets[bucket];
    idx->buckets[bucket] = entry;
    idx->entryCount++;

    return true;
}

/* Lookup in group index */
static kraftGroup *groupIndexLookup(groupIndex *idx, kraftGroupId groupId) {
    uint32_t bucket = groupIdHash(groupId) % idx->bucketCount;

    groupIndexEntry *entry = idx->buckets[bucket];
    while (entry) {
        if (entry->groupId == groupId) {
            return entry->group;
        }
        entry = entry->next;
    }
    return NULL;
}

/* Remove from group index */
static bool groupIndexRemove(groupIndex *idx, kraftGroupId groupId) {
    uint32_t bucket = groupIdHash(groupId) % idx->bucketCount;

    groupIndexEntry **ptr = &idx->buckets[bucket];
    while (*ptr) {
        if ((*ptr)->groupId == groupId) {
            groupIndexEntry *entry = *ptr;
            *ptr = entry->next;
            zfree(entry);
            idx->entryCount--;
            return true;
        }
        ptr = &(*ptr)->next;
    }
    return false;
}

/* Compare keys (for range lookup) */
static int keyCompare(const uint8_t *a, size_t aLen, const uint8_t *b,
                      size_t bLen) {
    size_t minLen = aLen < bLen ? aLen : bLen;
    int cmp = memcmp(a, b, minLen);
    if (cmp != 0) {
        return cmp;
    }
    if (aLen < bLen) {
        return -1;
    }
    if (aLen > bLen) {
        return 1;
    }
    return 0;
}

/* =========================================================================
 * Configuration Functions
 * ========================================================================= */

kraftMultiRaftConfig kraftMultiRaftConfigDefault(void) {
    kraftMultiRaftConfig config = {
        .maxGroups = KRAFT_MULTI_DEFAULT_MAX_GROUPS,
        .initialCapacity = KRAFT_MULTI_DEFAULT_INITIAL_CAPACITY,
        .tickIntervalUs = KRAFT_MULTI_DEFAULT_TICK_INTERVAL,
        .electionTimeoutUs = KRAFT_MULTI_DEFAULT_ELECTION_TIMEOUT,
        .heartbeatIntervalUs = KRAFT_MULTI_DEFAULT_HEARTBEAT_INTERVAL,
        .maxBatchSize = KRAFT_MULTI_DEFAULT_MAX_BATCH,
        .maxPendingMessages = KRAFT_MULTI_DEFAULT_MAX_PENDING,
        .enableLoadBalancing = true,
        .targetLeadersPerNode = 0, /* Equal distribution */
        .rebalanceIntervalUs = KRAFT_MULTI_DEFAULT_REBALANCE_INTERVAL,
        .shareConnections = true,
        .shareTimers = true};
    return config;
}

/* =========================================================================
 * Initialization & Lifecycle
 * ========================================================================= */

bool kraftMultiRaftInit(kraftMultiRaft *mr, const kraftMultiRaftConfig *config,
                        uint64_t localNodeId) {
    if (!mr) {
        return false;
    }

    memset(mr, 0, sizeof(*mr));

    /* Apply configuration */
    if (config) {
        mr->config = *config;
    } else {
        mr->config = kraftMultiRaftConfigDefault();
    }

    mr->localNodeId = localNodeId;

    /* Allocate group array */
    mr->groupCapacity = mr->config.initialCapacity;
    mr->groups = zcalloc(mr->groupCapacity, sizeof(kraftGroup *));
    if (!mr->groups) {
        return false;
    }

    /* Initialize group index */
    groupIndex *idx = zmalloc(sizeof(groupIndex));
    if (!idx) {
        zfree(mr->groups);
        mr->groups = NULL;
        return false;
    }

    if (!groupIndexInit(idx, mr->groupCapacity * 2)) {
        zfree(idx);
        zfree(mr->groups);
        mr->groups = NULL;
        return false;
    }

    mr->groupIndex = idx;

    return true;
}

void kraftMultiRaftFree(kraftMultiRaft *mr) {
    if (!mr) {
        return;
    }

    /* Free all groups */
    for (uint32_t i = 0; i < mr->groupCount; i++) {
        kraftGroup *group = mr->groups[i];
        if (group) {
            /* Free group descriptor resources */
            zfree(group->descriptor.startKey);
            zfree(group->descriptor.endKey);
            zfree(group->descriptor.replicas);

            /* Free Raft state */
            if (group->raft) {
                kraftStateFree(group->raft);
                zfree(group->raft);
            }

            zfree(group);
        }
    }

    /* Free group array */
    zfree(mr->groups);
    mr->groups = NULL;

    /* Free group index */
    if (mr->groupIndex) {
        groupIndexFree((groupIndex *)mr->groupIndex);
        zfree(mr->groupIndex);
        mr->groupIndex = NULL;
    }

    mr->groupCount = 0;
    mr->groupCapacity = 0;
}

/* =========================================================================
 * Group Management
 * ========================================================================= */

/* Grow group array if needed */
static bool ensureGroupCapacity(kraftMultiRaft *mr) {
    if (mr->groupCount < mr->groupCapacity) {
        return true;
    }

    if (mr->groupCapacity >= mr->config.maxGroups) {
        return false; /* At maximum */
    }

    uint32_t newCapacity = mr->groupCapacity * 2;
    if (newCapacity > mr->config.maxGroups) {
        newCapacity = mr->config.maxGroups;
    }

    kraftGroup **newGroups =
        zrealloc(mr->groups, newCapacity * sizeof(kraftGroup *));
    if (!newGroups) {
        return false;
    }

    /* Zero new slots */
    memset(newGroups + mr->groupCapacity, 0,
           (newCapacity - mr->groupCapacity) * sizeof(kraftGroup *));

    mr->groups = newGroups;
    mr->groupCapacity = newCapacity;

    return true;
}

kraftGroup *kraftMultiRaftCreateGroup(kraftMultiRaft *mr, kraftGroupId groupId,
                                      const kraftReplicaId *replicas,
                                      uint32_t replicaCount) {
    return kraftMultiRaftCreateGroupWithRange(mr, groupId, replicas,
                                              replicaCount, NULL, 0, NULL, 0);
}

kraftGroup *kraftMultiRaftCreateGroupWithRange(
    kraftMultiRaft *mr, kraftGroupId groupId, const kraftReplicaId *replicas,
    uint32_t replicaCount, const uint8_t *startKey, size_t startKeyLen,
    const uint8_t *endKey, size_t endKeyLen) {
    if (!mr || replicaCount == 0) {
        return NULL;
    }

    /* Check if group already exists */
    if (kraftMultiRaftGetGroup(mr, groupId)) {
        return NULL; /* Already exists */
    }

    /* Ensure capacity */
    if (!ensureGroupCapacity(mr)) {
        return NULL;
    }

    /* Allocate group */
    kraftGroup *group = zcalloc(1, sizeof(kraftGroup));
    if (!group) {
        return NULL;
    }

    /* Initialize descriptor */
    group->descriptor.groupId = groupId;
    group->descriptor.state = KRAFT_GROUP_CREATING;
    group->descriptor.createdAt = 0; /* Will be set on first tick */
    group->descriptor.epoch = 1;

    /* Copy replicas */
    group->descriptor.replicas = zmalloc(replicaCount * sizeof(kraftReplicaId));
    if (!group->descriptor.replicas) {
        zfree(group);
        return NULL;
    }
    memcpy(group->descriptor.replicas, replicas,
           replicaCount * sizeof(kraftReplicaId));
    group->descriptor.replicaCount = replicaCount;

    /* Copy key range if provided */
    if (startKey && startKeyLen > 0) {
        group->descriptor.startKey = zmalloc(startKeyLen);
        if (!group->descriptor.startKey) {
            zfree(group->descriptor.replicas);
            zfree(group);
            return NULL;
        }
        memcpy(group->descriptor.startKey, startKey, startKeyLen);
        group->descriptor.startKeyLen = startKeyLen;
    }

    if (endKey && endKeyLen > 0) {
        group->descriptor.endKey = zmalloc(endKeyLen);
        if (!group->descriptor.endKey) {
            zfree(group->descriptor.startKey);
            zfree(group->descriptor.replicas);
            zfree(group);
            return NULL;
        }
        memcpy(group->descriptor.endKey, endKey, endKeyLen);
        group->descriptor.endKeyLen = endKeyLen;
    }

    /* Note: Raft state is NOT initialized here - it's set externally via
     * kraftMultiRaftSetGroupRaft() or during integration with testCluster.
     * This allows the group registry to work standalone for unit testing. */
    group->raft = NULL;

    group->active = true;
    group->descriptor.state = KRAFT_GROUP_ACTIVE;

    /* Add to registry */
    mr->groups[mr->groupCount++] = group;
    groupIndexInsert((groupIndex *)mr->groupIndex, groupId, group);

    mr->totalGroups++;
    mr->activeGroups++;

    D("Created group %" PRIu64 ", active=true, activeGroups=%" PRIu64, groupId,
      mr->activeGroups);

    return group;
}

kraftGroup *kraftMultiRaftGetGroup(kraftMultiRaft *mr, kraftGroupId groupId) {
    if (!mr || !mr->groupIndex) {
        return NULL;
    }
    return groupIndexLookup((groupIndex *)mr->groupIndex, groupId);
}

bool kraftMultiRaftRemoveGroup(kraftMultiRaft *mr, kraftGroupId groupId) {
    if (!mr) {
        return false;
    }

    kraftGroup *group = kraftMultiRaftGetGroup(mr, groupId);
    if (!group) {
        D("RemoveGroup(%" PRIu64 "): group not found", groupId);
        return false;
    }

    /* Save active state BEFORE marking as removing */
    bool wasActive = group->active;
    D("RemoveGroup(%" PRIu64 "): wasActive=%d, activeGroups=%" PRIu64, groupId,
      wasActive, mr->activeGroups);

    /* Mark as removing */
    group->descriptor.state = KRAFT_GROUP_REMOVING;
    group->active = false;

    /* Remove from index */
    groupIndexRemove((groupIndex *)mr->groupIndex, groupId);

    /* Find and remove from array */
    for (uint32_t i = 0; i < mr->groupCount; i++) {
        if (mr->groups[i] == group) {
            /* Move last element to this slot */
            mr->groups[i] = mr->groups[mr->groupCount - 1];
            mr->groups[mr->groupCount - 1] = NULL;
            mr->groupCount--;
            break;
        }
    }

    /* Free resources */
    zfree(group->descriptor.startKey);
    zfree(group->descriptor.endKey);
    zfree(group->descriptor.replicas);
    if (group->raft) {
        kraftStateFree(group->raft);
        zfree(group->raft);
    }
    zfree(group);

    if (wasActive) {
        mr->activeGroups--;
        D("RemoveGroup: decremented activeGroups to %" PRIu64,
          mr->activeGroups);
    } else {
        D("RemoveGroup: group was not active, activeGroups unchanged at "
          "%" PRIu64,
          mr->activeGroups);
    }

    return true;
}

bool kraftMultiRaftStopGroup(kraftMultiRaft *mr, kraftGroupId groupId) {
    kraftGroup *group = kraftMultiRaftGetGroup(mr, groupId);
    if (!group) {
        return false;
    }

    /* Only decrement if group was active */
    if (group->active) {
        group->active = false;
        group->descriptor.state = KRAFT_GROUP_STOPPED;
        mr->activeGroups--;
        D("StopGroup(%" PRIu64 "): was active, activeGroups now %" PRIu64,
          groupId, mr->activeGroups);
    } else {
        D("StopGroup(%" PRIu64
          "): already inactive, activeGroups unchanged at %" PRIu64,
          groupId, mr->activeGroups);
    }

    return true;
}

bool kraftMultiRaftResumeGroup(kraftMultiRaft *mr, kraftGroupId groupId) {
    kraftGroup *group = kraftMultiRaftGetGroup(mr, groupId);
    if (!group) {
        return false;
    }

    if (group->descriptor.state != KRAFT_GROUP_STOPPED) {
        D("ResumeGroup(%" PRIu64 "): not stopped (state=%d), cannot resume",
          groupId, group->descriptor.state);
        return false; /* Can only resume stopped groups */
    }

    group->active = true;
    group->descriptor.state = KRAFT_GROUP_ACTIVE;
    mr->activeGroups++;
    D("ResumeGroup(%" PRIu64 "): resumed, activeGroups now %" PRIu64, groupId,
      mr->activeGroups);

    return true;
}

/* =========================================================================
 * Message Routing
 * ========================================================================= */

bool kraftMultiRaftRouteMessage(kraftMultiRaft *mr, kraftGroupId groupId,
                                uint64_t fromNodeId, const void *data,
                                size_t dataLen) {
    if (!mr || !data || dataLen == 0) {
        return false;
    }

    kraftGroup *group = kraftMultiRaftGetGroup(mr, groupId);
    if (!group || !group->active) {
        return false;
    }

    /* Find sender's node index */
    uint32_t fromIdx = 0;
    for (uint32_t i = 0; i < group->descriptor.replicaCount; i++) {
        if (group->descriptor.replicas[i] == fromNodeId) {
            fromIdx = i;
            break;
        }
    }

    /* Route to underlying Raft - requires raft integration.
     * For now, just track the message. Real routing happens through
     * the test framework or production RPC layer. */
    if (!group->raft) {
        return false; /* No raft instance attached */
    }

    (void)fromIdx;
    (void)data;
    (void)dataLen;

    /* TODO: Integrate with actual RPC message processing */
    group->messagesReceived++;
    mr->totalMessages++;

    return true;
}

uint32_t kraftMultiRaftGetOutbound(kraftMultiRaft *mr,
                                   kraftMultiRaftMessage **messages,
                                   uint32_t maxMessages) {
    if (!mr || !messages || maxMessages == 0) {
        return 0;
    }

    /* This is a simplified implementation - in production you'd batch
     * messages across all groups more efficiently */
    *messages = NULL;
    return 0; /* Placeholder - actual implementation depends on RPC layer */
}

void kraftMultiRaftFreeMessages(kraftMultiRaftMessage *messages,
                                uint32_t count) {
    if (!messages) {
        return;
    }

    for (uint32_t i = 0; i < count; i++) {
        zfree(messages[i].data);
    }
    zfree(messages);
}

/* =========================================================================
 * Tick & Processing
 * ========================================================================= */

void kraftMultiRaftTick(kraftMultiRaft *mr, uint64_t nowUs) {
    if (!mr) {
        return;
    }

    mr->lastTickTime = nowUs;
    mr->totalTicks++;

    /* Count leaders before tick */
    uint32_t leadersBeforeTick = 0;

    /* Tick all active groups */
    for (uint32_t i = 0; i < mr->groupCount; i++) {
        kraftGroup *group = mr->groups[i];
        if (group && group->active) {
            kraftMultiRaftTickGroup(mr, group, nowUs);

            /* Track leadership - use IS_LEADER macro from rpc.h on raft->self
             */
            if (group->raft && group->raft->self &&
                IS_LEADER(group->raft->self)) {
                leadersBeforeTick++;
            }
        }
    }

    mr->groupsAsLeader = leadersBeforeTick;

    /* Periodic rebalancing */
    if (mr->config.enableLoadBalancing &&
        nowUs - mr->lastRebalanceTime >= mr->config.rebalanceIntervalUs) {
        kraftMultiRaftRebalance(mr);
        mr->lastRebalanceTime = nowUs;
    }
}

void kraftMultiRaftTickGroup(kraftMultiRaft *mr, kraftGroup *group,
                             uint64_t nowUs) {
    if (!group) {
        return;
    }

    (void)mr; /* May be used for shared resources */

    /* Set creation time on first tick */
    if (group->descriptor.createdAt == 0) {
        group->descriptor.createdAt = nowUs;
    }

    group->lastTickTime = nowUs;

    /* Note: Actual Raft ticking happens through the test framework or
     * production timer callbacks. This function just updates group metadata. */

    /* Update leader in descriptor if raft instance is attached */
    if (group->raft && group->raft->self && IS_LEADER(group->raft->self)) {
        /* The local node is the leader for this group */
        group->descriptor.leaderReplica = mr->localNodeId;
    }
}

bool kraftMultiRaftHasPendingWork(const kraftMultiRaft *mr) {
    if (!mr) {
        return false;
    }

    for (uint32_t i = 0; i < mr->groupCount; i++) {
        kraftGroup *group = mr->groups[i];
        if (group && group->active && group->pendingMessages > 0) {
            return true;
        }
    }

    return false;
}

/* =========================================================================
 * Command Submission
 * ========================================================================= */

bool kraftMultiRaftSubmitCommand(kraftMultiRaft *mr, kraftGroupId groupId,
                                 const void *command, size_t commandLen) {
    if (!mr || !command || commandLen == 0) {
        return false;
    }

    kraftGroup *group = kraftMultiRaftGetGroup(mr, groupId);
    if (!group || !group->active || !group->raft) {
        return false;
    }

    /* Must be leader to accept commands */
    if (!group->raft->self || !IS_LEADER(group->raft->self)) {
        return false;
    }

    /* TODO: Integrate with actual command submission API.
     * For now, return success if we're the leader. */
    (void)command;
    (void)commandLen;
    return true;
}

bool kraftMultiRaftSubmitCommandForKey(kraftMultiRaft *mr, const uint8_t *key,
                                       size_t keyLen, const void *command,
                                       size_t commandLen) {
    if (!mr || !key || keyLen == 0) {
        return false;
    }

    kraftGroup *group = kraftMultiRaftFindGroupForKey(mr, key, keyLen);
    if (!group) {
        return false;
    }

    return kraftMultiRaftSubmitCommand(mr, group->descriptor.groupId, command,
                                       commandLen);
}

/* =========================================================================
 * Leadership
 * ========================================================================= */

bool kraftMultiRaftIsLeader(kraftMultiRaft *mr, kraftGroupId groupId) {
    kraftGroup *group = kraftMultiRaftGetGroup(mr, groupId);
    if (!group || !group->raft || !group->raft->self) {
        return false;
    }

    return IS_LEADER(group->raft->self);
}

kraftReplicaId kraftMultiRaftGetLeader(kraftMultiRaft *mr,
                                       kraftGroupId groupId) {
    kraftGroup *group = kraftMultiRaftGetGroup(mr, groupId);
    if (!group) {
        return 0;
    }

    return group->descriptor.leaderReplica;
}

bool kraftMultiRaftTransferLeadership(kraftMultiRaft *mr, kraftGroupId groupId,
                                      kraftReplicaId targetReplica) {
    kraftGroup *group = kraftMultiRaftGetGroup(mr, groupId);
    if (!group || !group->raft) {
        return false;
    }

    /* Find target's index */
    uint32_t targetIdx = UINT32_MAX;
    for (uint32_t i = 0; i < group->descriptor.replicaCount; i++) {
        if (group->descriptor.replicas[i] == targetReplica) {
            targetIdx = i;
            break;
        }
    }

    if (targetIdx == UINT32_MAX) {
        return false; /* Target not in group */
    }

    /* TODO: Integrate with kraftTransferLeadershipBegin() which requires
     * a kraftNode* pointer. For now, return false. */
    (void)targetIdx;
    return false;
}

/* =========================================================================
 * Load Balancing
 * ========================================================================= */

void kraftMultiRaftRebalance(kraftMultiRaft *mr) {
    if (!mr || !mr->config.enableLoadBalancing) {
        return;
    }

    /* Simple rebalancing: if we lead too many groups, transfer some away.
     * A production implementation would be more sophisticated. */

    uint32_t totalGroups = mr->groupCount;
    if (totalGroups == 0) {
        return;
    }

    /* Find unique node count from all groups */
    uint64_t uniqueNodes[64];
    uint32_t nodeCount = 0;

    for (uint32_t i = 0; i < mr->groupCount; i++) {
        kraftGroup *group = mr->groups[i];
        if (!group) {
            continue;
        }

        for (uint32_t j = 0; j < group->descriptor.replicaCount; j++) {
            uint64_t nodeId = group->descriptor.replicas[j];
            bool found = false;
            for (uint32_t k = 0; k < nodeCount; k++) {
                if (uniqueNodes[k] == nodeId) {
                    found = true;
                    break;
                }
            }
            if (!found && nodeCount < 64) {
                uniqueNodes[nodeCount++] = nodeId;
            }
        }
    }

    if (nodeCount == 0) {
        return;
    }

    /* Target leaders per node */
    uint32_t targetPerNode = mr->config.targetLeadersPerNode;
    if (targetPerNode == 0) {
        targetPerNode = (totalGroups + nodeCount - 1) / nodeCount;
    }

    /* If we're leading too many, transfer some */
    if (mr->groupsAsLeader > targetPerNode + 1) {
        for (uint32_t i = 0;
             i < mr->groupCount && mr->groupsAsLeader > targetPerNode; i++) {
            kraftGroup *group = mr->groups[i];
            if (!group || !group->raft || !group->raft->self ||
                !IS_LEADER(group->raft->self)) {
                continue;
            }

            /* Find a different replica to transfer to */
            for (uint32_t j = 0; j < group->descriptor.replicaCount; j++) {
                if (group->descriptor.replicas[j] != mr->localNodeId) {
                    /* TODO: Integrate with kraftTransferLeadershipBegin() */
                    /* kraftTransferLeadership(group->raft, j); */
                    mr->groupsAsLeader--;
                    break;
                }
            }
        }
    }
}

uint32_t kraftMultiRaftGetLeadershipDistribution(
    kraftMultiRaft *mr, kraftLeadershipDistribution **distribution) {
    if (!mr || !distribution) {
        return 0;
    }

    /* Simplified implementation - count leaders per node */
    /* In production, you'd track this more efficiently */

    *distribution = NULL;
    return 0; /* Placeholder */
}

/* =========================================================================
 * Range Operations
 * ========================================================================= */

kraftGroup *kraftMultiRaftFindGroupForKey(kraftMultiRaft *mr,
                                          const uint8_t *key, size_t keyLen) {
    if (!mr || !key || keyLen == 0) {
        return NULL;
    }

    /* Linear scan - in production you'd use an interval tree or similar */
    for (uint32_t i = 0; i < mr->groupCount; i++) {
        kraftGroup *group = mr->groups[i];
        if (!group || !group->active) {
            continue;
        }

        /* Check if key is in range [startKey, endKey) */
        if (group->descriptor.startKey && group->descriptor.startKeyLen > 0) {
            if (keyCompare(key, keyLen, group->descriptor.startKey,
                           group->descriptor.startKeyLen) < 0) {
                continue; /* Key is before range start */
            }
        }

        if (group->descriptor.endKey && group->descriptor.endKeyLen > 0) {
            if (keyCompare(key, keyLen, group->descriptor.endKey,
                           group->descriptor.endKeyLen) >= 0) {
                continue; /* Key is at or after range end */
            }
        }

        return group;
    }

    return NULL;
}

bool kraftMultiRaftSplitGroup(kraftMultiRaft *mr, kraftGroupId groupId,
                              const uint8_t *splitKey, size_t splitKeyLen,
                              kraftGroupId *leftGroupId,
                              kraftGroupId *rightGroupId) {
    if (!mr || !splitKey || splitKeyLen == 0) {
        return false;
    }

    kraftGroup *group = kraftMultiRaftGetGroup(mr, groupId);
    if (!group || !group->active) {
        return false;
    }

    /* Mark as splitting */
    group->descriptor.state = KRAFT_GROUP_SPLITTING;

    /* Generate new group IDs */
    kraftGroupId newLeftId = groupId;      /* Keep original for left */
    kraftGroupId newRightId = groupId + 1; /* Simple increment */

    /* Find unused ID for right group */
    while (kraftMultiRaftGetGroup(mr, newRightId)) {
        newRightId++;
    }

    /* Create right group with same replicas, range [splitKey, endKey) */
    kraftGroup *rightGroup = kraftMultiRaftCreateGroupWithRange(
        mr, newRightId, group->descriptor.replicas,
        group->descriptor.replicaCount, splitKey, splitKeyLen,
        group->descriptor.endKey, group->descriptor.endKeyLen);

    if (!rightGroup) {
        group->descriptor.state = KRAFT_GROUP_ACTIVE;
        return false;
    }

    /* Update left group's end key to splitKey */
    zfree(group->descriptor.endKey);
    group->descriptor.endKey = zmalloc(splitKeyLen);
    if (group->descriptor.endKey) {
        memcpy(group->descriptor.endKey, splitKey, splitKeyLen);
        group->descriptor.endKeyLen = splitKeyLen;
    }

    group->descriptor.state = KRAFT_GROUP_ACTIVE;
    group->descriptor.epoch++;

    if (leftGroupId) {
        *leftGroupId = newLeftId;
    }
    if (rightGroupId) {
        *rightGroupId = newRightId;
    }

    return true;
}

bool kraftMultiRaftMergeGroups(kraftMultiRaft *mr, kraftGroupId leftGroupId,
                               kraftGroupId rightGroupId,
                               kraftGroupId *mergedGroupId) {
    if (!mr) {
        return false;
    }

    kraftGroup *leftGroup = kraftMultiRaftGetGroup(mr, leftGroupId);
    kraftGroup *rightGroup = kraftMultiRaftGetGroup(mr, rightGroupId);

    if (!leftGroup || !rightGroup) {
        return false;
    }
    if (!leftGroup->active || !rightGroup->active) {
        return false;
    }

    /* Mark as merging */
    leftGroup->descriptor.state = KRAFT_GROUP_MERGING;
    rightGroup->descriptor.state = KRAFT_GROUP_MERGING;

    /* Extend left group's range to include right */
    zfree(leftGroup->descriptor.endKey);
    if (rightGroup->descriptor.endKey) {
        leftGroup->descriptor.endKey =
            zmalloc(rightGroup->descriptor.endKeyLen);
        if (leftGroup->descriptor.endKey) {
            memcpy(leftGroup->descriptor.endKey, rightGroup->descriptor.endKey,
                   rightGroup->descriptor.endKeyLen);
            leftGroup->descriptor.endKeyLen = rightGroup->descriptor.endKeyLen;
        }
    } else {
        leftGroup->descriptor.endKey = NULL;
        leftGroup->descriptor.endKeyLen = 0;
    }

    /* Remove right group */
    kraftMultiRaftRemoveGroup(mr, rightGroupId);

    leftGroup->descriptor.state = KRAFT_GROUP_ACTIVE;
    leftGroup->descriptor.epoch++;

    if (mergedGroupId) {
        *mergedGroupId = leftGroupId;
    }

    return true;
}

/* =========================================================================
 * Query Functions
 * ========================================================================= */

uint32_t kraftMultiRaftGroupCount(const kraftMultiRaft *mr) {
    return mr ? mr->groupCount : 0;
}

uint32_t kraftMultiRaftGetGroupIds(const kraftMultiRaft *mr,
                                   kraftGroupId *groupIds, uint32_t maxGroups) {
    if (!mr || !groupIds || maxGroups == 0) {
        return 0;
    }

    uint32_t count = 0;
    for (uint32_t i = 0; i < mr->groupCount && count < maxGroups; i++) {
        if (mr->groups[i]) {
            groupIds[count++] = mr->groups[i]->descriptor.groupId;
        }
    }

    return count;
}

void kraftMultiRaftForEachGroup(kraftMultiRaft *mr, kraftGroupIterator iterator,
                                void *userData) {
    if (!mr || !iterator) {
        return;
    }

    for (uint32_t i = 0; i < mr->groupCount; i++) {
        kraftGroup *group = mr->groups[i];
        if (group) {
            if (!iterator(group, userData)) {
                break; /* Iterator requested stop */
            }
        }
    }
}

/* =========================================================================
 * Statistics
 * ========================================================================= */

void kraftMultiRaftGetStats(const kraftMultiRaft *mr,
                            kraftMultiRaftStats *stats) {
    if (!mr || !stats) {
        return;
    }

    memset(stats, 0, sizeof(*stats));

    stats->totalGroups = mr->totalGroups;
    stats->activeGroups = mr->activeGroups;
    stats->groupsAsLeader = mr->groupsAsLeader;
    stats->totalTicks = mr->totalTicks;
    stats->totalMessages = mr->totalMessages;

    if (mr->totalTicks > 0) {
        stats->avgMessagesPerTick = mr->totalMessages / mr->totalTicks;
        stats->avgGroupsPerTick =
            (mr->activeGroups * mr->totalTicks) / mr->totalTicks;
    }
}

int kraftMultiRaftFormatStats(const kraftMultiRaftStats *stats, char *buf,
                              size_t bufLen) {
    if (!stats || !buf || bufLen == 0) {
        return 0;
    }

    return snprintf(buf, bufLen,
                    "Multi-Raft Stats:\n"
                    "  Total groups:     %llu\n"
                    "  Active groups:    %llu\n"
                    "  Groups as leader: %llu\n"
                    "  Total ticks:      %llu\n"
                    "  Total messages:   %llu\n"
                    "  Avg msgs/tick:    %llu\n",
                    (unsigned long long)stats->totalGroups,
                    (unsigned long long)stats->activeGroups,
                    (unsigned long long)stats->groupsAsLeader,
                    (unsigned long long)stats->totalTicks,
                    (unsigned long long)stats->totalMessages,
                    (unsigned long long)stats->avgMessagesPerTick);
}

/* =========================================================================
 * Callbacks
 * ========================================================================= */

/* Callback storage - would be in the struct in production */
static kraftMultiRaftLeaderChangeCallback g_leaderChangeCb = NULL;
static void *g_leaderChangeUserData = NULL;
static kraftMultiRaftCommitCallback g_commitCb = NULL;
static void *g_commitUserData = NULL;

void kraftMultiRaftSetLeaderChangeCallback(
    kraftMultiRaft *mr, kraftMultiRaftLeaderChangeCallback cb, void *userData) {
    (void)mr;
    g_leaderChangeCb = cb;
    g_leaderChangeUserData = userData;
}

void kraftMultiRaftSetCommitCallback(kraftMultiRaft *mr,
                                     kraftMultiRaftCommitCallback cb,
                                     void *userData) {
    (void)mr;
    g_commitCb = cb;
    g_commitUserData = userData;
}
