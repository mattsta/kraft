/* loopyCluster - Cluster membership and discovery
 *
 * Manages dynamic cluster membership for loopy-demo:
 *   - Membership tracking (which nodes are in the cluster)
 *   - Node discovery (learn about peers via gossip)
 *   - Bootstrap flow (first node becomes leader, others join via --join)
 *   - Membership changes (add/remove nodes via Raft joint consensus)
 *   - Configuration persistence (survive restarts)
 *
 * This is part of loopy-demo, a consumer of kraft (not part of kraft itself).
 *
 * Architecture:
 *
 *   loopyCluster maintains a view of cluster membership that is:
 *   - Consistent: All nodes eventually converge to the same view
 *   - Durable: Membership is persisted and recovered on restart
 *   - Dynamic: Nodes can be added/removed at runtime
 *
 * Bootstrap Flow:
 *
 *   1. First node starts with no peers -> becomes single-node cluster leader
 *   2. Second node starts with --join <first-node>:
 *      a. Connects to seed node
 *      b. Sends JOIN request
 *      c. Seed (if leader) initiates joint consensus
 *      d. New node receives full cluster membership
 *      e. New node connects to all cluster members
 *   3. Subsequent nodes follow the same join flow
 *
 * Membership Protocol Messages:
 *
 *   CLUSTER_JOIN_REQUEST  - "I want to join the cluster"
 *   CLUSTER_JOIN_RESPONSE - "Here's the full membership"
 *   CLUSTER_GOSSIP        - "Here's what I know about the cluster"
 *   CLUSTER_CONFIG_CHANGE - "Membership is changing (joint consensus)"
 *
 * Usage:
 *
 *   loopyClusterConfig cfg;
 *   loopyClusterConfigInit(&cfg);
 *   cfg.localId = myNodeId;
 *   cfg.dataDir = "/var/lib/kraft";
 *
 *   loopyCluster *cluster = loopyClusterCreate(&cfg);
 *   loopyClusterSetCallback(cluster, onMembershipChange, userData);
 *
 *   // Bootstrap or join
 *   if (seedAddr) {
 *       loopyClusterJoin(cluster, seedAddr, seedPort);
 *   } else {
 *       loopyClusterBootstrap(cluster);  // Become first node
 *   }
 *
 *   // Later, add more nodes
 *   loopyClusterAddMember(cluster, nodeId, addr, port);
 */

#ifndef LOOPY_CLUSTER_H
#define LOOPY_CLUSTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declarations */
struct loopyLoop;

/* ============================================================================
 * Types
 * ============================================================================
 */

/**
 * Cluster handle.
 *
 * Manages membership state and discovery for a Raft cluster.
 */
typedef struct loopyCluster loopyCluster;

/**
 * Member information.
 *
 * Describes a single node in the cluster.
 */
typedef struct loopyMember {
    uint64_t nodeId; /* Unique node identifier */
    char addr[64];   /* Network address */
    int port;        /* Network port */
    uint64_t runId;  /* Instance ID (changes on restart) */
    bool isVoter;    /* Can participate in elections */
    bool isLearner;  /* Non-voting member (catching up) */
} loopyMember;

/**
 * Cluster state.
 */
typedef enum loopyClusterState {
    LOOPY_CLUSTER_INIT,          /* Created but not started */
    LOOPY_CLUSTER_JOINING,       /* Join in progress */
    LOOPY_CLUSTER_BOOTSTRAPPING, /* Bootstrap in progress */
    LOOPY_CLUSTER_ACTIVE,        /* Normal operation */
    LOOPY_CLUSTER_CONFIG_CHANGE, /* Membership change in progress */
    LOOPY_CLUSTER_STOPPED        /* Shut down */
} loopyClusterState;

/**
 * Cluster event types.
 */
typedef enum loopyClusterEvent {
    LOOPY_CLUSTER_EVENT_JOINED,         /* Successfully joined cluster */
    LOOPY_CLUSTER_EVENT_BOOTSTRAPPED,   /* Successfully bootstrapped */
    LOOPY_CLUSTER_EVENT_MEMBER_ADDED,   /* New member added */
    LOOPY_CLUSTER_EVENT_MEMBER_REMOVED, /* Member removed */
    LOOPY_CLUSTER_EVENT_MEMBER_UPDATED, /* Member info changed */
    LOOPY_CLUSTER_EVENT_CONFIG_STABLE,  /* Config change completed */
    LOOPY_CLUSTER_EVENT_JOIN_FAILED,    /* Join request rejected */
    LOOPY_CLUSTER_EVENT_GOSSIP_RECEIVED /* Received gossip update */
} loopyClusterEvent;

/**
 * Cluster event callback.
 *
 * @param cluster   Cluster handle
 * @param event     Event type
 * @param member    Member affected (for member events)
 * @param userData  User context
 */
typedef void loopyClusterCallback(loopyCluster *cluster,
                                  loopyClusterEvent event,
                                  const loopyMember *member, void *userData);

/**
 * Join response handler.
 *
 * Called when a JOIN request is received by the leader.
 * The handler should validate the request and return whether to accept.
 *
 * @param cluster   Cluster handle
 * @param member    Requesting member info
 * @param userData  User context
 * @return true to accept join request
 */
typedef bool loopyClusterJoinHandler(loopyCluster *cluster,
                                     const loopyMember *member, void *userData);

/**
 * Send handler for broadcasting messages.
 *
 * Called when the cluster needs to broadcast a message to all peers.
 * The implementation should send the message to all known cluster members.
 *
 * @param cluster   Cluster handle
 * @param data      Message data
 * @param len       Message length
 * @param userData  User context
 * @return Number of peers message was sent to
 */
typedef size_t loopyClusterSendHandler(loopyCluster *cluster, const void *data,
                                       size_t len, void *userData);

/**
 * Cluster configuration.
 */
typedef struct loopyClusterConfig {
    /* Local node identity */
    uint64_t localNodeId; /* Our node ID (required, > 0) */
    uint64_t clusterId;   /* Cluster ID (default: 1) */
    char localAddr[64];   /* Our advertised address */
    int localPort;        /* Our advertised port */

    /* Persistence */
    const char *dataDir; /* Directory for config files (optional) */

    /* Timing (milliseconds) */
    uint32_t gossipIntervalMs; /* Gossip broadcast interval (default: 1000) */
    uint32_t joinTimeoutMs;    /* Join request timeout (default: 5000) */
    uint32_t configChangeTimeoutMs; /* Config change timeout (default: 10000) */

    /* Callbacks */
    loopyClusterCallback *callback;
    loopyClusterJoinHandler *joinHandler;
    loopyClusterSendHandler *sendHandler; /* For gossip broadcast */
    void *userData;
} loopyClusterConfig;

/* ============================================================================
 * Configuration
 * ============================================================================
 */

/**
 * Initialize config with defaults.
 */
void loopyClusterConfigInit(loopyClusterConfig *cfg);

/**
 * Set local node identity.
 *
 * @param cfg      Configuration
 * @param nodeId   Node ID
 * @param addr     Advertised address
 * @param port     Advertised port
 */
void loopyClusterConfigSetLocal(loopyClusterConfig *cfg, uint64_t nodeId,
                                const char *addr, int port);

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

/**
 * Create a cluster manager.
 *
 * Creates the cluster manager but does not start any operations.
 * Call loopyClusterBootstrap() or loopyClusterJoin() to begin.
 *
 * @param cfg Configuration (required)
 * @return Cluster handle, or NULL on error
 */
loopyCluster *loopyClusterCreate(const loopyClusterConfig *cfg);

/**
 * Destroy the cluster manager.
 *
 * Frees all resources.
 *
 * @param cluster Cluster handle (may be NULL)
 */
void loopyClusterDestroy(loopyCluster *cluster);

/**
 * Bootstrap a new cluster.
 *
 * Initializes this node as the first and only member of a new cluster.
 * This node will immediately become leader (single-node cluster).
 *
 * Only call this when starting a new cluster. To join an existing
 * cluster, use loopyClusterJoin() instead.
 *
 * @param cluster Cluster handle
 * @return true if bootstrap initiated
 */
bool loopyClusterBootstrap(loopyCluster *cluster);

/**
 * Join an existing cluster.
 *
 * Connects to a seed node and requests to join the cluster.
 * On success, receives full membership and connects to all peers.
 *
 * @param cluster  Cluster handle
 * @param seedAddr Seed node address
 * @param seedPort Seed node port
 * @return true if join initiated
 */
bool loopyClusterJoin(loopyCluster *cluster, const char *seedAddr,
                      int seedPort);

/**
 * Start cluster operations.
 *
 * Must be called after bootstrap or successful join.
 * Starts gossip protocol and membership maintenance.
 *
 * @param cluster Cluster handle
 * @param loop    Event loop to use
 * @return true if started
 */
bool loopyClusterStart(loopyCluster *cluster, struct loopyLoop *loop);

/**
 * Stop cluster operations.
 *
 * Stops gossip and prepares for shutdown.
 *
 * @param cluster Cluster handle
 */
void loopyClusterStop(loopyCluster *cluster);

/* ============================================================================
 * Membership Management
 * ============================================================================
 */

/**
 * Add a member to the cluster.
 *
 * Initiates a configuration change to add the new member.
 * Only the leader can add members (uses Raft joint consensus).
 *
 * @param cluster  Cluster handle
 * @param nodeId   New member's node ID
 * @param addr     New member's address
 * @param port     New member's port
 * @param isVoter  true for voting member, false for learner
 * @return true if change initiated
 */
bool loopyClusterAddMember(loopyCluster *cluster, uint64_t nodeId,
                           const char *addr, int port, bool isVoter);

/**
 * Remove a member from the cluster.
 *
 * Initiates a configuration change to remove the member.
 * Only the leader can remove members.
 *
 * @param cluster Cluster handle
 * @param nodeId  Member to remove
 * @return true if change initiated
 */
bool loopyClusterRemoveMember(loopyCluster *cluster, uint64_t nodeId);

/**
 * Promote a learner to voter.
 *
 * @param cluster Cluster handle
 * @param nodeId  Learner to promote
 * @return true if change initiated
 */
bool loopyClusterPromoteMember(loopyCluster *cluster, uint64_t nodeId);

/**
 * Demote a voter to learner.
 *
 * @param cluster Cluster handle
 * @param nodeId  Voter to demote
 * @return true if change initiated
 */
bool loopyClusterDemoteMember(loopyCluster *cluster, uint64_t nodeId);

/* ============================================================================
 * Membership Queries
 * ============================================================================
 */

/**
 * Get cluster state.
 */
loopyClusterState loopyClusterGetState(const loopyCluster *cluster);

/**
 * Get state name string.
 */
const char *loopyClusterStateName(loopyClusterState state);

/**
 * Get number of members.
 */
size_t loopyClusterGetMemberCount(const loopyCluster *cluster);

/**
 * Get number of voting members.
 */
size_t loopyClusterGetVoterCount(const loopyCluster *cluster);

/**
 * Get member by node ID.
 *
 * @param cluster Cluster handle
 * @param nodeId  Node ID to look up
 * @param member  OUT: Member info (copied)
 * @return true if found
 */
bool loopyClusterGetMember(const loopyCluster *cluster, uint64_t nodeId,
                           loopyMember *member);

/**
 * Check if a node is a member.
 */
bool loopyClusterIsMember(const loopyCluster *cluster, uint64_t nodeId);

/**
 * Check if a node is a voter.
 */
bool loopyClusterIsVoter(const loopyCluster *cluster, uint64_t nodeId);

/**
 * Get local member info.
 */
const loopyMember *loopyClusterGetLocal(const loopyCluster *cluster);

/**
 * Member iterator callback.
 *
 * @param cluster  Cluster handle
 * @param member   Current member
 * @param userData User context
 * @return true to continue, false to stop
 */
typedef bool loopyClusterMemberIter(loopyCluster *cluster,
                                    const loopyMember *member, void *userData);

/**
 * Iterate over all members.
 */
void loopyClusterForEachMember(loopyCluster *cluster,
                               loopyClusterMemberIter *iter, void *userData);

/**
 * Iterate over voters only.
 */
void loopyClusterForEachVoter(loopyCluster *cluster,
                              loopyClusterMemberIter *iter, void *userData);

/* ============================================================================
 * Configuration Persistence
 * ============================================================================
 */

/**
 * Save cluster configuration to disk.
 *
 * Persists current membership for recovery on restart.
 *
 * @param cluster Cluster handle
 * @return true if saved
 */
bool loopyClusterSave(loopyCluster *cluster);

/**
 * Load cluster configuration from disk.
 *
 * Restores membership from previous run.
 *
 * @param cluster Cluster handle
 * @return true if loaded (false if no saved config)
 */
bool loopyClusterLoad(loopyCluster *cluster);

/* ============================================================================
 * Gossip Protocol
 * ============================================================================
 */

/**
 * Process incoming gossip message.
 *
 * Called by the platform when receiving a CLUSTER_GOSSIP message.
 *
 * @param cluster Cluster handle
 * @param data    Message data
 * @param len     Message length
 * @return true if processed
 */
bool loopyClusterProcessGossip(loopyCluster *cluster, const void *data,
                               size_t len);

/**
 * Process incoming join request.
 *
 * Called by the leader when receiving a CLUSTER_JOIN_REQUEST.
 *
 * @param cluster Cluster handle
 * @param data    Request data
 * @param len     Request length
 * @param response OUT: Response to send back (callee allocates, caller frees)
 * @param responseLen OUT: Response length
 * @return true if request accepted
 */
bool loopyClusterProcessJoinRequest(loopyCluster *cluster, const void *data,
                                    size_t len, void **response,
                                    size_t *responseLen);

/**
 * Process incoming join response.
 *
 * Called when receiving a CLUSTER_JOIN_RESPONSE after sending join request.
 *
 * @param cluster Cluster handle
 * @param data    Response data
 * @param len     Response length
 * @return true if processed
 */
bool loopyClusterProcessJoinResponse(loopyCluster *cluster, const void *data,
                                     size_t len);

/**
 * Create gossip message for broadcast.
 *
 * @param cluster Cluster handle
 * @param data    OUT: Message data (callee allocates, caller frees)
 * @param len     OUT: Message length
 * @return true if created
 */
bool loopyClusterCreateGossip(loopyCluster *cluster, void **data, size_t *len);

/* ============================================================================
 * Statistics
 * ============================================================================
 */

/**
 * Cluster statistics.
 */
typedef struct loopyClusterStats {
    uint64_t gossipSent;           /* Gossip messages sent */
    uint64_t gossipReceived;       /* Gossip messages received */
    uint64_t joinRequestsSent;     /* Join requests sent */
    uint64_t joinRequestsReceived; /* Join requests received */
    uint64_t configChanges;        /* Configuration changes */
    uint64_t memberAdded;          /* Members added */
    uint64_t memberRemoved;        /* Members removed */
} loopyClusterStats;

void loopyClusterGetStats(const loopyCluster *cluster,
                          loopyClusterStats *stats);

/* ============================================================================
 * Cluster Protocol Message Types
 *
 * These are the message types used by the cluster membership protocol.
 * They are separate from the Raft RPC messages.
 * ============================================================================
 */

/**
 * Cluster protocol message types.
 */
typedef enum loopyClusterMsgType {
    LOOPY_CLUSTER_MSG_JOIN_REQUEST = 0x01,  /* Request to join cluster */
    LOOPY_CLUSTER_MSG_JOIN_RESPONSE = 0x02, /* Response to join request */
    LOOPY_CLUSTER_MSG_GOSSIP = 0x03,        /* Membership gossip */
    LOOPY_CLUSTER_MSG_CONFIG_CHANGE =
        0x04 /* Configuration change notification */
} loopyClusterMsgType;

/**
 * Check if a message is a cluster protocol message.
 *
 * @param data Message data
 * @param len  Message length
 * @return true if this is a cluster protocol message
 */
bool loopyClusterIsClusterMessage(const void *data, size_t len);

/**
 * Get cluster message type.
 *
 * @param data Message data
 * @param len  Message length
 * @return Message type, or -1 if not a cluster message
 */
int loopyClusterGetMessageType(const void *data, size_t len);

#endif /* LOOPY_CLUSTER_H */
