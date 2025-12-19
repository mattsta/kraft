/* loopyPlatform - Server lifecycle and orchestration
 *
 * Central orchestrator for loopy-demo that owns and coordinates:
 *   - Event loop (loopyLoop)
 *   - Timer management (loopyTimerMgr)
 *   - Transport layer (loopyTransport)
 *   - Session management
 *   - kraft integration
 *
 * This is part of loopy-demo, a consumer of kraft (not part of kraft itself).
 *
 * Usage:
 *   loopyPlatformConfig cfg;
 *   loopyPlatformConfigInit(&cfg);
 *   cfg.nodeId = 1;
 *   cfg.listenPort = 5001;
 *
 *   loopyPlatform *platform = loopyPlatformCreate(&cfg);
 *   loopyPlatformRun(platform);  // Blocks until shutdown
 *   loopyPlatformDestroy(platform);
 */

#ifndef LOOPY_PLATFORM_H
#define LOOPY_PLATFORM_H

#include "loopyCluster.h"
#include "loopyConnection.h"
#include "loopySession.h"
#include "loopyTimers.h"
#include "loopyTransport.h"

#include <stdbool.h>
#include <stdint.h>

/* Forward declarations */
struct loopyLoop;
struct kraftState;

/* ============================================================================
 * Types
 * ============================================================================
 */

/**
 * Platform handle.
 *
 * The central orchestrator for a loopy-demo Raft node.
 */
typedef struct loopyPlatform loopyPlatform;

/**
 * Platform state.
 */
typedef enum loopyPlatformState {
    LOOPY_PLATFORM_CREATED,  /* Created but not started */
    LOOPY_PLATFORM_STARTING, /* Startup in progress */
    LOOPY_PLATFORM_RUNNING,  /* Event loop running */
    LOOPY_PLATFORM_STOPPING, /* Shutdown in progress */
    LOOPY_PLATFORM_STOPPED   /* Fully stopped */
} loopyPlatformState;

/**
 * Platform event types.
 */
typedef enum loopyPlatformEvent {
    LOOPY_PLATFORM_EVENT_STARTED,     /* Platform started */
    LOOPY_PLATFORM_EVENT_STOPPED,     /* Platform stopped */
    LOOPY_PLATFORM_EVENT_PEER_JOINED, /* Peer session verified */
    LOOPY_PLATFORM_EVENT_PEER_LEFT,   /* Peer session closed */
    LOOPY_PLATFORM_EVENT_MESSAGE,     /* RPC message received */
    LOOPY_PLATFORM_EVENT_TIMER        /* Timer fired (election/heartbeat) */
} loopyPlatformEvent;

/**
 * Platform event callback.
 *
 * @param platform Platform handle
 * @param event    Event type
 * @param session  Session handle (for peer events)
 * @param data     Message data (for MESSAGE event)
 * @param len      Message length (for MESSAGE event)
 * @param userData User context
 */
typedef void loopyPlatformCallback(loopyPlatform *platform,
                                   loopyPlatformEvent event,
                                   loopySession *session, const void *data,
                                   size_t len, void *userData);

/**
 * Peer configuration for static cluster membership.
 */
typedef struct loopyPeerConfig {
    uint64_t nodeId; /* Peer's node ID */
    char addr[64];   /* Peer's address */
    int port;        /* Peer's port */
} loopyPeerConfig;

/**
 * Platform configuration.
 */
typedef struct loopyPlatformConfig {
    /* Node identity */
    uint64_t nodeId;    /* Our node ID (required) */
    uint64_t clusterId; /* Cluster ID (default: 1) */

    /* Networking */
    const char *listenAddr; /* Address to bind (default: "0.0.0.0") */
    int listenPort;         /* Port to listen on (required) */

    /* Static cluster membership (for bootstrap) */
    loopyPeerConfig *peers; /* Array of peer configs */
    size_t peerCount;       /* Number of peers */

    /* Timer configuration */
    loopyTimerConfig timerConfig;

    /* Transport configuration */
    loopyTransportConfig transportConfig;

    /* Cluster configuration (optional - for dynamic membership) */
    bool enableCluster;               /* Enable cluster membership manager */
    loopyClusterConfig clusterConfig; /* Cluster config (if enableCluster) */

    /* Raft feature configuration */
    bool preVoteEnabled;   /* Enable PreVote extension (default: true) */
    bool readIndexEnabled; /* Enable ReadIndex optimization (default: true) */

    /* Callbacks */
    loopyPlatformCallback *callback;
    void *userData;
} loopyPlatformConfig;

/* ============================================================================
 * Configuration
 * ============================================================================
 */

/**
 * Initialize config with defaults.
 */
void loopyPlatformConfigInit(loopyPlatformConfig *cfg);

/**
 * Add a peer to the configuration.
 *
 * @param cfg    Configuration
 * @param nodeId Peer's node ID
 * @param addr   Peer's address
 * @param port   Peer's port
 * @return true if added
 */
bool loopyPlatformConfigAddPeer(loopyPlatformConfig *cfg, uint64_t nodeId,
                                const char *addr, int port);

/**
 * Free peer array in configuration.
 * Call after loopyPlatformCreate() or if create fails.
 */
void loopyPlatformConfigCleanup(loopyPlatformConfig *cfg);

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

/**
 * Create a platform.
 *
 * Creates all subsystems but does not start the event loop.
 *
 * @param cfg Configuration (required)
 * @return Platform handle, or NULL on error
 */
loopyPlatform *loopyPlatformCreate(const loopyPlatformConfig *cfg);

/**
 * Run the platform.
 *
 * Starts the event loop and blocks until shutdown.
 * Call loopyPlatformStop() from a callback to initiate shutdown.
 *
 * @param platform Platform handle
 * @return 0 on success, -1 on error
 */
int loopyPlatformRun(loopyPlatform *platform);

/**
 * Stop the platform.
 *
 * Initiates graceful shutdown. Safe to call from callbacks.
 *
 * @param platform Platform handle
 */
void loopyPlatformStop(loopyPlatform *platform);

/**
 * Destroy the platform.
 *
 * Frees all resources. Must be called after run completes.
 *
 * @param platform Platform handle (may be NULL)
 */
void loopyPlatformDestroy(loopyPlatform *platform);

/* ============================================================================
 * Connection Management
 * ============================================================================
 */

/**
 * Connect to a peer.
 *
 * Initiates outbound connection. Session will be created when connected.
 *
 * @param platform Platform handle
 * @param nodeId   Peer's node ID
 * @param addr     Peer's address
 * @param port     Peer's port
 * @return true if connection initiated
 */
bool loopyPlatformConnectPeer(loopyPlatform *platform, uint64_t nodeId,
                              const char *addr, int port);

/**
 * Disconnect from a peer.
 *
 * @param platform Platform handle
 * @param nodeId   Peer's node ID
 */
void loopyPlatformDisconnectPeer(loopyPlatform *platform, uint64_t nodeId);

/**
 * Get session for a peer.
 *
 * @param platform Platform handle
 * @param nodeId   Peer's node ID
 * @return Session handle, or NULL if not connected
 */
loopySession *loopyPlatformGetPeerSession(loopyPlatform *platform,
                                          uint64_t nodeId);

/**
 * Send message to a peer.
 *
 * @param platform Platform handle
 * @param nodeId   Peer's node ID
 * @param data     Message data
 * @param len      Message length
 * @return true if sent
 */
bool loopyPlatformSendToPeer(loopyPlatform *platform, uint64_t nodeId,
                             const void *data, size_t len);

/**
 * Broadcast message to all verified peers.
 *
 * @param platform Platform handle
 * @param data     Message data
 * @param len      Message length
 * @return Number of peers message was sent to
 */
size_t loopyPlatformBroadcast(loopyPlatform *platform, const void *data,
                              size_t len);

/* ============================================================================
 * Timer Control
 * ============================================================================
 */

/**
 * Start election timer.
 *
 * Fires LOOPY_PLATFORM_EVENT_TIMER with timer type = ELECTION.
 */
void loopyPlatformStartElectionTimer(loopyPlatform *platform);

/**
 * Stop election timer.
 */
void loopyPlatformStopElectionTimer(loopyPlatform *platform);

/**
 * Reset election timer (restart with new random timeout).
 */
void loopyPlatformResetElectionTimer(loopyPlatform *platform);

/**
 * Start heartbeat timer.
 *
 * Fires LOOPY_PLATFORM_EVENT_TIMER with timer type = HEARTBEAT.
 */
void loopyPlatformStartHeartbeatTimer(loopyPlatform *platform);

/**
 * Stop heartbeat timer.
 */
void loopyPlatformStopHeartbeatTimer(loopyPlatform *platform);

/* ============================================================================
 * Properties
 * ============================================================================
 */

/**
 * Get platform state.
 */
loopyPlatformState loopyPlatformGetState(const loopyPlatform *platform);

/**
 * Get state name string.
 */
const char *loopyPlatformStateName(loopyPlatformState state);

/**
 * Get the event loop.
 */
struct loopyLoop *loopyPlatformGetLoop(const loopyPlatform *platform);

/**
 * Get the timer manager.
 */
loopyTimerMgr *loopyPlatformGetTimerMgr(const loopyPlatform *platform);

/**
 * Get the transport.
 */
loopyTransport *loopyPlatformGetTransport(const loopyPlatform *platform);

/**
 * Get the cluster manager (if enabled).
 */
loopyCluster *loopyPlatformGetCluster(const loopyPlatform *platform);

/**
 * Get our node identity.
 */
const loopyNodeId *loopyPlatformGetLocalId(const loopyPlatform *platform);

/**
 * Get/set user data.
 */
void *loopyPlatformGetUserData(const loopyPlatform *platform);
void loopyPlatformSetUserData(loopyPlatform *platform, void *userData);

/**
 * Get number of verified peer sessions.
 */
size_t loopyPlatformGetPeerCount(const loopyPlatform *platform);

/**
 * Check if PreVote is enabled.
 */
bool loopyPlatformIsPreVoteEnabled(const loopyPlatform *platform);

/**
 * Check if ReadIndex is enabled.
 */
bool loopyPlatformIsReadIndexEnabled(const loopyPlatform *platform);

/**
 * Platform statistics.
 */
typedef struct loopyPlatformStats {
    uint64_t messagesReceived;
    uint64_t messagesSent;
    uint64_t peersConnected;
    uint64_t peersDisconnected;
    uint64_t electionTimeouts;
    uint64_t heartbeatsSent;
} loopyPlatformStats;

void loopyPlatformGetStats(const loopyPlatform *platform,
                           loopyPlatformStats *stats);

/* ============================================================================
 * Session Iteration
 * ============================================================================
 */

/**
 * Session iterator callback.
 *
 * @param platform Platform handle
 * @param session  Session handle
 * @param userData User context
 * @return true to continue, false to stop
 */
typedef bool loopyPlatformSessionIter(loopyPlatform *platform,
                                      loopySession *session, void *userData);

/**
 * Iterate over all verified sessions.
 */
void loopyPlatformForEachSession(loopyPlatform *platform,
                                 loopyPlatformSessionIter *iter,
                                 void *userData);

/* ============================================================================
 * Cluster Operations (requires enableCluster = true)
 * ============================================================================
 */

/**
 * Bootstrap a new cluster.
 *
 * This node becomes the first and only member of a new cluster.
 * Only call when starting a new cluster (not joining existing one).
 *
 * @param platform Platform handle
 * @return true if bootstrap initiated
 */
bool loopyPlatformBootstrap(loopyPlatform *platform);

/**
 * Join an existing cluster.
 *
 * Connects to a seed node and requests to join.
 *
 * @param platform Platform handle
 * @param seedAddr Seed node address
 * @param seedPort Seed node port
 * @return true if join initiated
 */
bool loopyPlatformJoinCluster(loopyPlatform *platform, const char *seedAddr,
                              int seedPort);

#endif /* LOOPY_PLATFORM_H */
