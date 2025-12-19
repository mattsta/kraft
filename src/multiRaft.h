#ifndef KRAFT_MULTI_RAFT_H
#define KRAFT_MULTI_RAFT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Kraft Multi-Raft Groups
 * =====================================
 * Manage multiple independent Raft consensus groups within a single process.
 * Essential for distributed databases that partition data across Raft groups.
 *
 * Features:
 * 1. Group Registry - Central management of all Raft groups
 * 2. Shared Resources - Network layer, timers, memory pools
 * 3. Group Isolation - Each group operates independently
 * 4. Efficient Multiplexing - Share connections across groups
 * 5. Load Balancing - Spread leadership across nodes
 * 6. Cross-Group Coordination - Support for multi-group transactions
 *
 * Architecture:
 * - MultiRaft acts as a container for multiple kraftState instances
 * - Each group has a unique groupId (typically a range/partition ID)
 * - Messages are routed by (nodeId, groupId) tuple
 * - Ticks and I/O are batched for efficiency
 *
 * Based on:
 * - CockroachDB MultiRaft implementation
 * - etcd/raft multi-group design
 * - TiKV Raftstore architecture
 */

/* Forward declarations */
struct kraftState;
struct kraftNode;

/* =========================================================================
 * Multi-Raft Types
 * ========================================================================= */

/* Group identifier (e.g., range ID, partition ID) */
typedef uint64_t kraftGroupId;

/* Replica identifier within a group */
typedef uint64_t kraftReplicaId;

/* Group state */
typedef enum kraftGroupState {
    KRAFT_GROUP_CREATING = 0, /* Being initialized */
    KRAFT_GROUP_ACTIVE,       /* Normal operation */
    KRAFT_GROUP_SPLITTING,    /* Undergoing split */
    KRAFT_GROUP_MERGING,      /* Undergoing merge */
    KRAFT_GROUP_REMOVING,     /* Being removed */
    KRAFT_GROUP_STOPPED       /* Stopped/inactive */
} kraftGroupState;

/* Group descriptor - metadata about a Raft group */
typedef struct kraftGroupDescriptor {
    kraftGroupId groupId;  /* Unique group identifier */
    kraftGroupState state; /* Current group state */
    uint64_t createdAt;    /* Creation timestamp (us) */
    uint64_t epoch;        /* Configuration epoch (incremented on changes) */

    /* Key range (for range-partitioned systems) */
    uint8_t *startKey; /* Start of key range (inclusive) */
    size_t startKeyLen;
    uint8_t *endKey; /* End of key range (exclusive) */
    size_t endKeyLen;

    /* Membership */
    kraftReplicaId *replicas;     /* Array of replica IDs in this group */
    uint32_t replicaCount;        /* Number of replicas */
    kraftReplicaId leaderReplica; /* Current leader (0 if unknown) */
} kraftGroupDescriptor;

/* Per-group handle */
typedef struct kraftGroup {
    kraftGroupDescriptor descriptor;
    struct kraftState *raft;  /* Underlying Raft state machine */
    void *appData;            /* Application-specific data */
    bool active;              /* Is this group slot active */
    uint64_t lastTickTime;    /* Last time this group was ticked */
    uint64_t pendingMessages; /* Messages waiting to be processed */

    /* Statistics */
    uint64_t messagesReceived;
    uint64_t messagesSent;
    uint64_t entriesApplied;
    uint64_t leaderChanges;
} kraftGroup;

/* =========================================================================
 * Configuration
 * ========================================================================= */

/* Multi-Raft configuration */
typedef struct kraftMultiRaftConfig {
    /* Capacity limits */
    uint32_t maxGroups;       /* Maximum number of groups */
    uint32_t initialCapacity; /* Initial group array capacity */

    /* Timing */
    uint64_t tickIntervalUs;      /* How often to tick groups (us) */
    uint64_t electionTimeoutUs;   /* Default election timeout for new groups */
    uint64_t heartbeatIntervalUs; /* Default heartbeat interval */

    /* Batching */
    uint32_t maxBatchSize;       /* Max messages to batch per tick */
    uint32_t maxPendingMessages; /* Max pending messages per group */

    /* Load balancing */
    bool enableLoadBalancing;      /* Enable automatic leader balancing */
    uint32_t targetLeadersPerNode; /* Target leaders per node (0 = equal) */
    uint64_t rebalanceIntervalUs;  /* How often to rebalance (us) */

    /* Resource sharing */
    bool shareConnections; /* Share network connections across groups */
    bool shareTimers;      /* Share timer infrastructure */
} kraftMultiRaftConfig;

/* Default configuration values */
#define KRAFT_MULTI_DEFAULT_MAX_GROUPS 1024
#define KRAFT_MULTI_DEFAULT_INITIAL_CAPACITY 16
#define KRAFT_MULTI_DEFAULT_TICK_INTERVAL (10 * 1000)      /* 10ms */
#define KRAFT_MULTI_DEFAULT_ELECTION_TIMEOUT (150 * 1000)  /* 150ms */
#define KRAFT_MULTI_DEFAULT_HEARTBEAT_INTERVAL (50 * 1000) /* 50ms */
#define KRAFT_MULTI_DEFAULT_MAX_BATCH 100
#define KRAFT_MULTI_DEFAULT_MAX_PENDING 1000
#define KRAFT_MULTI_DEFAULT_REBALANCE_INTERVAL (60 * 1000000) /* 60s */

/* =========================================================================
 * Multi-Raft Manager
 * ========================================================================= */

typedef struct kraftMultiRaft {
    kraftMultiRaftConfig config; /* Configuration */

    /* Group registry */
    kraftGroup **groups;    /* Array of group pointers */
    uint32_t groupCount;    /* Number of active groups */
    uint32_t groupCapacity; /* Allocated capacity */

    /* Group lookup by ID */
    void *groupIndex; /* Hash table: groupId -> kraftGroup* */

    /* Local node identity */
    uint64_t localNodeId; /* This node's ID */

    /* Timing */
    uint64_t lastTickTime;      /* Last tick timestamp */
    uint64_t lastRebalanceTime; /* Last rebalance attempt */

    /* Statistics */
    uint64_t totalGroups;    /* Total groups ever created */
    uint64_t activeGroups;   /* Currently active groups */
    uint64_t groupsAsLeader; /* Groups where we are leader */
    uint64_t totalTicks;     /* Total tick cycles */
    uint64_t totalMessages;  /* Total messages processed */
} kraftMultiRaft;

/* =========================================================================
 * Configuration Functions
 * ========================================================================= */

/* Get default configuration */
kraftMultiRaftConfig kraftMultiRaftConfigDefault(void);

/* =========================================================================
 * Initialization & Lifecycle
 * ========================================================================= */

/* Initialize Multi-Raft manager */
bool kraftMultiRaftInit(kraftMultiRaft *mr, const kraftMultiRaftConfig *config,
                        uint64_t localNodeId);

/* Free Multi-Raft manager and all groups */
void kraftMultiRaftFree(kraftMultiRaft *mr);

/* =========================================================================
 * Group Management
 * ========================================================================= */

/* Create a new Raft group.
 * Returns the group handle or NULL on failure. */
kraftGroup *kraftMultiRaftCreateGroup(kraftMultiRaft *mr, kraftGroupId groupId,
                                      const kraftReplicaId *replicas,
                                      uint32_t replicaCount);

/* Create a group with key range (for range-partitioned systems) */
kraftGroup *kraftMultiRaftCreateGroupWithRange(
    kraftMultiRaft *mr, kraftGroupId groupId, const kraftReplicaId *replicas,
    uint32_t replicaCount, const uint8_t *startKey, size_t startKeyLen,
    const uint8_t *endKey, size_t endKeyLen);

/* Get existing group by ID */
kraftGroup *kraftMultiRaftGetGroup(kraftMultiRaft *mr, kraftGroupId groupId);

/* Remove a group (stops and cleans up) */
bool kraftMultiRaftRemoveGroup(kraftMultiRaft *mr, kraftGroupId groupId);

/* Stop a group (but keep it registered) */
bool kraftMultiRaftStopGroup(kraftMultiRaft *mr, kraftGroupId groupId);

/* Resume a stopped group */
bool kraftMultiRaftResumeGroup(kraftMultiRaft *mr, kraftGroupId groupId);

/* =========================================================================
 * Message Routing
 * ========================================================================= */

/* Route an incoming message to the appropriate group */
bool kraftMultiRaftRouteMessage(kraftMultiRaft *mr, kraftGroupId groupId,
                                uint64_t fromNodeId, const void *data,
                                size_t dataLen);

/* Get pending outbound messages for a node (batched across groups) */
typedef struct kraftMultiRaftMessage {
    kraftGroupId groupId;
    uint64_t toNodeId;
    void *data;
    size_t dataLen;
} kraftMultiRaftMessage;

/* Get batched outbound messages. Returns count, caller frees messages. */
uint32_t kraftMultiRaftGetOutbound(kraftMultiRaft *mr,
                                   kraftMultiRaftMessage **messages,
                                   uint32_t maxMessages);

/* Free a batch of messages */
void kraftMultiRaftFreeMessages(kraftMultiRaftMessage *messages,
                                uint32_t count);

/* =========================================================================
 * Tick & Processing
 * ========================================================================= */

/* Tick all groups (call periodically) */
void kraftMultiRaftTick(kraftMultiRaft *mr, uint64_t nowUs);

/* Process pending work for a specific group */
void kraftMultiRaftTickGroup(kraftMultiRaft *mr, kraftGroup *group,
                             uint64_t nowUs);

/* Check if any groups have pending work */
bool kraftMultiRaftHasPendingWork(const kraftMultiRaft *mr);

/* =========================================================================
 * Command Submission
 * ========================================================================= */

/* Submit a command to a specific group */
bool kraftMultiRaftSubmitCommand(kraftMultiRaft *mr, kraftGroupId groupId,
                                 const void *command, size_t commandLen);

/* Submit a command to the group responsible for a key */
bool kraftMultiRaftSubmitCommandForKey(kraftMultiRaft *mr, const uint8_t *key,
                                       size_t keyLen, const void *command,
                                       size_t commandLen);

/* =========================================================================
 * Leadership
 * ========================================================================= */

/* Check if we are leader for a group */
bool kraftMultiRaftIsLeader(kraftMultiRaft *mr, kraftGroupId groupId);

/* Get leader for a group (0 if unknown) */
kraftReplicaId kraftMultiRaftGetLeader(kraftMultiRaft *mr,
                                       kraftGroupId groupId);

/* Trigger leadership transfer for a group */
bool kraftMultiRaftTransferLeadership(kraftMultiRaft *mr, kraftGroupId groupId,
                                      kraftReplicaId targetReplica);

/* =========================================================================
 * Load Balancing
 * ========================================================================= */

/* Trigger leadership rebalancing across all groups */
void kraftMultiRaftRebalance(kraftMultiRaft *mr);

/* Get leadership distribution (groups led by each node) */
typedef struct kraftLeadershipDistribution {
    uint64_t nodeId;
    uint32_t groupsLed;
} kraftLeadershipDistribution;

uint32_t kraftMultiRaftGetLeadershipDistribution(
    kraftMultiRaft *mr, kraftLeadershipDistribution **distribution);

/* =========================================================================
 * Range Operations (for range-partitioned systems)
 * ========================================================================= */

/* Find group responsible for a key */
kraftGroup *kraftMultiRaftFindGroupForKey(kraftMultiRaft *mr,
                                          const uint8_t *key, size_t keyLen);

/* Split a group at a key (creates two new groups) */
bool kraftMultiRaftSplitGroup(kraftMultiRaft *mr, kraftGroupId groupId,
                              const uint8_t *splitKey, size_t splitKeyLen,
                              kraftGroupId *leftGroupId,
                              kraftGroupId *rightGroupId);

/* Merge two adjacent groups */
bool kraftMultiRaftMergeGroups(kraftMultiRaft *mr, kraftGroupId leftGroupId,
                               kraftGroupId rightGroupId,
                               kraftGroupId *mergedGroupId);

/* =========================================================================
 * Query Functions
 * ========================================================================= */

/* Get total group count */
uint32_t kraftMultiRaftGroupCount(const kraftMultiRaft *mr);

/* Get all group IDs */
uint32_t kraftMultiRaftGetGroupIds(const kraftMultiRaft *mr,
                                   kraftGroupId *groupIds, uint32_t maxGroups);

/* Iterate over all groups */
typedef bool (*kraftGroupIterator)(kraftGroup *group, void *userData);
void kraftMultiRaftForEachGroup(kraftMultiRaft *mr, kraftGroupIterator iterator,
                                void *userData);

/* =========================================================================
 * Statistics
 * ========================================================================= */

typedef struct kraftMultiRaftStats {
    uint64_t totalGroups;        /* Total groups ever created */
    uint64_t activeGroups;       /* Currently active groups */
    uint64_t groupsAsLeader;     /* Groups where local node is leader */
    uint64_t totalTicks;         /* Total tick cycles */
    uint64_t totalMessages;      /* Total messages processed */
    uint64_t totalCommands;      /* Total commands submitted */
    uint64_t avgMessagesPerTick; /* Average messages per tick */
    uint64_t avgGroupsPerTick;   /* Average groups ticked per cycle */
} kraftMultiRaftStats;

/* Get Multi-Raft statistics */
void kraftMultiRaftGetStats(const kraftMultiRaft *mr,
                            kraftMultiRaftStats *stats);

/* Format statistics as string */
int kraftMultiRaftFormatStats(const kraftMultiRaftStats *stats, char *buf,
                              size_t bufLen);

/* =========================================================================
 * Callbacks
 * ========================================================================= */

/* Callback when a group's leadership changes */
typedef void (*kraftMultiRaftLeaderChangeCallback)(kraftMultiRaft *mr,
                                                   kraftGroupId groupId,
                                                   kraftReplicaId newLeader,
                                                   void *userData);

/* Callback when entries are committed in a group */
typedef void (*kraftMultiRaftCommitCallback)(kraftMultiRaft *mr,
                                             kraftGroupId groupId,
                                             uint64_t index, const void *data,
                                             size_t dataLen, void *userData);

/* Set callbacks */
void kraftMultiRaftSetLeaderChangeCallback(
    kraftMultiRaft *mr, kraftMultiRaftLeaderChangeCallback cb, void *userData);

void kraftMultiRaftSetCommitCallback(kraftMultiRaft *mr,
                                     kraftMultiRaftCommitCallback cb,
                                     void *userData);

#endif /* KRAFT_MULTI_RAFT_H */
