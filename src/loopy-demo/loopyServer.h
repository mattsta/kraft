/* loopyServer - High-level Raft server API
 *
 * Provides a simple, high-level API for building Raft-based servers.
 * Wraps loopyPlatform and kraft to provide:
 *   - Simple server creation with sensible defaults
 *   - State machine interface for application logic
 *   - Lifecycle callbacks (onBecomeLeader, etc.)
 *   - Command submission and read queries
 *
 * This is part of loopy-demo, a consumer of kraft (not part of kraft itself).
 *
 * Minimal Example (3-node cluster):
 *
 *   loopyServerConfig cfg;
 *   loopyServerConfigInit(&cfg);
 *   cfg.nodeId = 1;
 *   cfg.listenAddr = "127.0.0.1";
 *   cfg.listenPort = 5001;
 *   loopyServerConfigAddPeer(&cfg, 2, "127.0.0.1", 5002);
 *   loopyServerConfigAddPeer(&cfg, 3, "127.0.0.1", 5003);
 *
 *   loopyServer *srv = loopyServerCreate(&cfg);
 *   loopyServerSetStateMachine(srv, &myStateMachine);
 *   loopyServerRun(srv);  // Blocks until shutdown
 *   loopyServerDestroy(srv);
 *
 * Single-Node Example:
 *
 *   loopyServer *srv = loopyServerCreateSingle(1, "127.0.0.1", 5001);
 *   loopyServerRun(srv);
 *   loopyServerDestroy(srv);
 */

#ifndef LOOPY_SERVER_H
#define LOOPY_SERVER_H

#include "loopyPlatform.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declarations */
struct kraftState;

/* ============================================================================
 * Types
 * ============================================================================
 */

/**
 * Server handle.
 */
typedef struct loopyServer loopyServer;

/**
 * Server role in the Raft cluster.
 */
typedef enum loopyServerRole {
    LOOPY_SERVER_ROLE_FOLLOWER,
    LOOPY_SERVER_ROLE_CANDIDATE,
    LOOPY_SERVER_ROLE_LEADER
} loopyServerRole;

/**
 * State machine interface.
 *
 * Implement this interface to provide application-specific logic.
 * The state machine applies committed log entries to produce results.
 */
typedef struct loopyStateMachine {
    /**
     * Apply a committed command.
     *
     * Called when a log entry is committed and should be applied to
     * the application state. Must be deterministic - same input must
     * produce same output on all nodes.
     *
     * @param sm       State machine context
     * @param index    Log index of the command
     * @param data     Command data
     * @param len      Command length
     * @param result   OUT: Optional result data (server will copy)
     * @param resultLen OUT: Result length
     * @return true on success
     */
    bool (*apply)(struct loopyStateMachine *sm, uint64_t index,
                  const void *data, size_t len, void **result,
                  size_t *resultLen);

    /**
     * Create a snapshot of current state.
     *
     * Called periodically to compact the log.
     *
     * @param sm       State machine context
     * @param data     OUT: Snapshot data (callee allocates, caller frees)
     * @param len      OUT: Snapshot length
     * @param lastIndex OUT: Last applied index included in snapshot
     * @param lastTerm OUT: Term of last applied entry
     * @return true on success
     */
    bool (*snapshot)(struct loopyStateMachine *sm, void **data, size_t *len,
                     uint64_t *lastIndex, uint64_t *lastTerm);

    /**
     * Restore state from a snapshot.
     *
     * Called when receiving a snapshot from the leader.
     *
     * @param sm       State machine context
     * @param data     Snapshot data
     * @param len      Snapshot length
     * @param lastIndex Last applied index in snapshot
     * @param lastTerm Term of last applied entry
     * @return true on success
     */
    bool (*restore)(struct loopyStateMachine *sm, const void *data, size_t len,
                    uint64_t lastIndex, uint64_t lastTerm);

    /**
     * User context.
     */
    void *userData;
} loopyStateMachine;

/**
 * Server event types for lifecycle callbacks.
 */
typedef enum loopyServerEvent {
    LOOPY_SERVER_EVENT_STARTED,      /* Server started */
    LOOPY_SERVER_EVENT_STOPPED,      /* Server stopped */
    LOOPY_SERVER_EVENT_ROLE_CHANGED, /* Role changed (follower/candidate/leader)
                                      */
    LOOPY_SERVER_EVENT_LEADER_CHANGED, /* New leader elected */
    LOOPY_SERVER_EVENT_PEER_ADDED,     /* Peer joined cluster */
    LOOPY_SERVER_EVENT_PEER_REMOVED,   /* Peer left cluster */
    LOOPY_SERVER_EVENT_COMMAND_APPLIED /* Command was applied */
} loopyServerEvent;

/**
 * Server event callback.
 *
 * @param server   Server handle
 * @param event    Event type
 * @param data     Event-specific data (role, leader ID, etc.)
 * @param userData User context
 */
typedef void loopyServerCallback(loopyServer *server, loopyServerEvent event,
                                 const void *data, void *userData);

/**
 * Server configuration.
 */
typedef struct loopyServerConfig {
    /* Node identity */
    uint64_t nodeId;    /* Our node ID (required, must be > 0) */
    uint64_t clusterId; /* Cluster ID (default: 1) */

    /* Networking */
    const char *listenAddr; /* Address to bind (default: "0.0.0.0") */
    int listenPort;         /* Port to listen on (required) */

    /* Cluster peers (for static membership) */
    loopyPeerConfig *peers; /* Array of peer configs */
    size_t peerCount;       /* Number of peers */

    /* Raft timing (milliseconds) */
    uint32_t electionTimeoutMinMs; /* Min election timeout (default: 150) */
    uint32_t electionTimeoutMaxMs; /* Max election timeout (default: 300) */
    uint32_t heartbeatIntervalMs;  /* Heartbeat interval (default: 50) */

    /* Features */
    bool preVoteEnabled;   /* Enable PreVote (default: true) */
    bool readIndexEnabled; /* Enable ReadIndex (default: true) */

    /* Callbacks */
    loopyServerCallback *callback;
    void *userData;
} loopyServerConfig;

/* ============================================================================
 * Configuration
 * ============================================================================
 */

/**
 * Initialize config with defaults.
 */
void loopyServerConfigInit(loopyServerConfig *cfg);

/**
 * Add a peer to the configuration.
 *
 * @param cfg    Configuration
 * @param nodeId Peer's node ID
 * @param addr   Peer's address
 * @param port   Peer's port
 * @return true if added
 */
bool loopyServerConfigAddPeer(loopyServerConfig *cfg, uint64_t nodeId,
                              const char *addr, int port);

/**
 * Free peer array in configuration.
 */
void loopyServerConfigCleanup(loopyServerConfig *cfg);

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

/**
 * Create a server.
 *
 * Creates server with the specified configuration.
 * Server is not started until loopyServerRun() is called.
 *
 * @param cfg Configuration (required)
 * @return Server handle, or NULL on error
 */
loopyServer *loopyServerCreate(const loopyServerConfig *cfg);

/**
 * Create a single-node server.
 *
 * Convenience function for development/testing.
 * The single node immediately becomes leader.
 *
 * @param nodeId     Node ID
 * @param listenAddr Address to bind
 * @param listenPort Port to listen on
 * @return Server handle, or NULL on error
 */
loopyServer *loopyServerCreateSingle(uint64_t nodeId, const char *listenAddr,
                                     int listenPort);

/**
 * Set the state machine.
 *
 * Must be called before loopyServerRun().
 *
 * @param server Server handle
 * @param sm     State machine interface
 */
void loopyServerSetStateMachine(loopyServer *server, loopyStateMachine *sm);

/**
 * Run the server.
 *
 * Starts the server and blocks until shutdown.
 * Call loopyServerStop() from a callback to initiate shutdown.
 *
 * @param server Server handle
 * @return 0 on success, -1 on error
 */
int loopyServerRun(loopyServer *server);

/**
 * Stop the server.
 *
 * Initiates graceful shutdown. Safe to call from callbacks.
 *
 * @param server Server handle
 */
void loopyServerStop(loopyServer *server);

/**
 * Destroy the server.
 *
 * Frees all resources.
 *
 * @param server Server handle (may be NULL)
 */
void loopyServerDestroy(loopyServer *server);

/* ============================================================================
 * Command Submission
 * ============================================================================
 */

/**
 * Command result callback.
 *
 * Called when a submitted command is committed and applied.
 *
 * @param server   Server handle
 * @param index    Log index of the command
 * @param success  true if committed, false if failed (e.g., not leader)
 * @param result   Result from state machine (may be NULL)
 * @param resultLen Result length
 * @param userData User context from submit call
 */
typedef void loopyCommandCallback(loopyServer *server, uint64_t index,
                                  bool success, const void *result,
                                  size_t resultLen, void *userData);

/**
 * Submit a command to the cluster.
 *
 * The command will be replicated to all nodes and applied once committed.
 * Only the leader can accept commands.
 *
 * @param server   Server handle
 * @param data     Command data
 * @param len      Command length
 * @param cb       Result callback (optional)
 * @param userData User context for callback
 * @return Command index if accepted, 0 if rejected (not leader)
 */
uint64_t loopyServerSubmit(loopyServer *server, const void *data, size_t len,
                           loopyCommandCallback *cb, void *userData);

/**
 * Check if we are the leader.
 */
bool loopyServerIsLeader(const loopyServer *server);

/**
 * Get current role.
 */
loopyServerRole loopyServerGetRole(const loopyServer *server);

/**
 * Get current leader ID (0 if unknown).
 */
uint64_t loopyServerGetLeaderId(const loopyServer *server);

/* ============================================================================
 * Read Queries
 * ============================================================================
 */

/**
 * Read callback.
 *
 * Called when a read query can be safely executed.
 *
 * @param server   Server handle
 * @param success  true if read is safe, false if stale
 * @param userData User context
 */
typedef void loopyReadCallback(loopyServer *server, bool success,
                               void *userData);

/**
 * Execute a linearizable read.
 *
 * Ensures the read sees all committed writes up to this point.
 * Uses ReadIndex for efficiency when available.
 *
 * @param server   Server handle
 * @param cb       Callback when read is safe
 * @param userData User context
 * @return true if request accepted
 */
bool loopyServerReadLinearizable(loopyServer *server, loopyReadCallback *cb,
                                 void *userData);

/* ============================================================================
 * Properties
 * ============================================================================
 */

/**
 * Get our node ID.
 */
uint64_t loopyServerGetNodeId(const loopyServer *server);

/**
 * Get cluster ID.
 */
uint64_t loopyServerGetClusterId(const loopyServer *server);

/**
 * Get current term.
 */
uint64_t loopyServerGetTerm(const loopyServer *server);

/**
 * Get commit index.
 */
uint64_t loopyServerGetCommitIndex(const loopyServer *server);

/**
 * Get last applied index.
 */
uint64_t loopyServerGetLastApplied(const loopyServer *server);

/**
 * Get underlying platform.
 */
loopyPlatform *loopyServerGetPlatform(const loopyServer *server);

/**
 * Get/set user data.
 */
void *loopyServerGetUserData(const loopyServer *server);
void loopyServerSetUserData(loopyServer *server, void *userData);

/**
 * Set event callback.
 *
 * Can be called after creation to set or change the callback.
 */
void loopyServerSetCallback(loopyServer *server, loopyServerCallback *callback,
                            void *userData);

/**
 * Server statistics.
 */
typedef struct loopyServerStats {
    uint64_t commandsSubmitted;
    uint64_t commandsCommitted;
    uint64_t commandsRejected;
    uint64_t readsExecuted;
    uint64_t electionTimeouts;
    uint64_t termsServed;
} loopyServerStats;

void loopyServerGetStats(const loopyServer *server, loopyServerStats *stats);

/* ============================================================================
 * Cluster Management
 * ============================================================================
 */

/**
 * Add a node to the cluster (dynamic membership).
 *
 * Only the leader can add nodes. Uses joint consensus.
 *
 * @param server Server handle
 * @param nodeId New node's ID
 * @param addr   New node's address
 * @param port   New node's port
 * @return true if membership change initiated
 */
bool loopyServerAddNode(loopyServer *server, uint64_t nodeId, const char *addr,
                        int port);

/**
 * Remove a node from the cluster.
 *
 * Only the leader can remove nodes.
 *
 * @param server Server handle
 * @param nodeId Node to remove
 * @return true if membership change initiated
 */
bool loopyServerRemoveNode(loopyServer *server, uint64_t nodeId);

/**
 * Transfer leadership to another node.
 *
 * @param server  Server handle
 * @param nodeId  Target node (0 for automatic selection)
 * @return true if transfer initiated
 */
bool loopyServerTransferLeadership(loopyServer *server, uint64_t nodeId);

#endif /* LOOPY_SERVER_H */
