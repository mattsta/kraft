/* Kraft Network Integration Header
 * ==================================
 *
 * This header provides the unified interface for pluggable network I/O.
 * It combines the adapter, protocol, and client APIs into a cohesive
 * networking layer that can be used with any event loop and wire format.
 *
 * Component Hierarchy:
 *
 *   kraftNetwork (this file) - Integration layer
 *       |
 *       +-- kraftAdapter.h   - Event loop abstraction (timers, I/O)
 *       +-- kraftProtocol.h  - Wire protocol encoding/decoding
 *       +-- kraftClientAPI.h - Client command submission
 *
 * Usage Example:
 *
 *   // Create components
 *   kraftProtocol *proto = kraftProtocolCreate(&binaryOps, NULL);
 *   kraftAdapter *adapter = kraftAdapterCreate(&libuvOps, NULL);
 *   kraftClientSession *client = kraftClientCreate(adapter, proto, NULL);
 *
 *   // Configure cluster
 *   kraftClientAddEndpoint(client, "127.0.0.1", 7001);
 *   kraftClientAddEndpoint(client, "127.0.0.1", 7002);
 *   kraftClientAddEndpoint(client, "127.0.0.1", 7003);
 *
 *   // Connect and submit commands
 *   kraftClientConnect(client);
 *   kraftClientSubmit(client, cmd, len, onResponse, userData);
 *   kraftClientRun(client);  // or integrate with your event loop
 *
 * Server-Side Usage:
 *
 *   // Create kraft server with pluggable adapter
 *   kraftServer *server = kraftServerCreate(adapter, proto);
 *   kraftServerSetState(server, kraftState);
 *   kraftServerListen(server, "0.0.0.0", 7001);
 *   kraftServerRun(server);
 *
 * Thread Safety:
 *   - All components are single-threaded by design
 *   - Use one set per thread, or protect with external mutex
 *   - kraftAdapterStop() is safe to call from any thread
 */

#ifndef KRAFT_NETWORK_H
#define KRAFT_NETWORK_H

#include "kraftAdapter.h"
#include "kraftClientAPI.h"
#include "kraftProtocol.h"

/* Forward declarations */
struct kraftState;

/* =========================================================================
 * Server-Side Integration
 * ========================================================================= */

/* Server configuration */
typedef struct kraftServerConfig {
    /* Listen addresses */
    char peerListenAddress[256];   /* Address for peer connections */
    uint16_t peerListenPort;       /* Port for peer connections */
    char clientListenAddress[256]; /* Address for client connections */
    uint16_t clientListenPort;     /* Port for client connections */

    /* Separate ports for peer/client traffic (optional) */
    bool separatePorts; /* Use different ports for peer/client */

    /* Connection limits */
    uint32_t maxPeerConnections;   /* Maximum peer connections */
    uint32_t maxClientConnections; /* Maximum client connections */

    /* Timeouts */
    uint64_t peerConnectTimeoutMs; /* Peer connection timeout */
    uint64_t clientIdleTimeoutMs;  /* Client idle disconnect timeout */

    /* TLS (future) */
    bool enableTls;          /* Enable TLS encryption */
    const char *tlsCertPath; /* Path to TLS certificate */
    const char *tlsKeyPath;  /* Path to TLS private key */
} kraftServerConfig;

/* Server handle */
typedef struct kraftServer {
    /* Components */
    kraftAdapter *adapter;
    kraftProtocol *protocol;
    struct kraftState *state;

    /* Configuration */
    kraftServerConfig config;

    /* State */
    bool running;
    bool listening;

    /* Statistics */
    struct {
        uint64_t peerConnectionsAccepted;
        uint64_t clientConnectionsAccepted;
        uint64_t messagesProcessed;
        uint64_t bytesReceived;
        uint64_t bytesSent;
        uint64_t uptimeUs;
    } stats;

    /* Implementation data */
    void *impl;

} kraftServer;

/* =========================================================================
 * Server Lifecycle
 * ========================================================================= */

/* Initialize default server configuration */
void kraftServerConfigDefault(kraftServerConfig *config);

/* Create server with adapter and protocol */
kraftServer *kraftServerCreate(kraftAdapter *adapter, kraftProtocol *protocol);

/* Associate server with kraft state */
void kraftServerSetState(kraftServer *server, struct kraftState *state);

/* Configure server */
void kraftServerConfigure(kraftServer *server, const kraftServerConfig *config);

/* Start listening for connections
 * Returns true on success */
bool kraftServerListen(kraftServer *server);

/* Start listening on specific address/port (convenience) */
bool kraftServerListenOn(kraftServer *server, const char *address,
                         uint16_t port);

/* Stop listening (but keep processing existing connections) */
void kraftServerStopListening(kraftServer *server);

/* Run server event loop (blocks) */
void kraftServerRun(kraftServer *server);

/* Stop server (safe from any thread) */
void kraftServerStop(kraftServer *server);

/* Destroy server and free resources */
void kraftServerDestroy(kraftServer *server);

/* =========================================================================
 * Server Statistics
 * ========================================================================= */

typedef struct kraftServerStats {
    uint64_t peerConnectionsAccepted;
    uint64_t clientConnectionsAccepted;
    uint64_t activeConnections;
    uint64_t messagesProcessed;
    uint64_t bytesReceived;
    uint64_t bytesSent;
    uint64_t uptimeUs;
} kraftServerStats;

void kraftServerGetStats(const kraftServer *server, kraftServerStats *stats);

/* Format stats as string */
int kraftServerFormatStats(const kraftServerStats *stats, char *buf,
                           size_t len);

/* =========================================================================
 * Integration with kraftState
 * ========================================================================= */

/* Set adapter on kraft state (connects kraft to network layer) */
void kraftStateSetAdapter(struct kraftState *state, kraftAdapter *adapter);

/* Set protocol on kraft state */
void kraftStateSetProtocol(struct kraftState *state, kraftProtocol *protocol);

/* Get adapter from kraft state */
kraftAdapter *kraftStateGetAdapter(struct kraftState *state);

/* Get protocol from kraft state */
kraftProtocol *kraftStateGetProtocol(struct kraftState *state);

/* =========================================================================
 * Convenience: Built-in Adapters
 * ========================================================================= */

/* Get libuv adapter operations (if available) */
const kraftAdapterOps *kraftAdapterGetLibuvOps(void);

/* Get poll/select adapter operations (portable fallback) */
const kraftAdapterOps *kraftAdapterGetPollOps(void);

/* =========================================================================
 * Convenience: Create Fully Configured Server
 * ========================================================================= */

/* Create server with default libuv adapter and binary protocol */
kraftServer *kraftServerCreateDefault(struct kraftState *state);

/* =========================================================================
 * Multi-Raft Server Support
 * ========================================================================= */

/* Handle for multi-raft server (multiple raft groups) */
typedef struct kraftMultiRaftServer {
    kraftServer *server; /* Underlying server */
    void *groups;        /* Hash map: groupId -> kraftState* */
    uint32_t groupCount; /* Number of active groups */
} kraftMultiRaftServer;

/* Create multi-raft server */
kraftMultiRaftServer *kraftMultiRaftServerCreate(kraftAdapter *adapter,
                                                 kraftProtocol *protocol);

/* Add a raft group to multi-raft server */
bool kraftMultiRaftServerAddGroup(kraftMultiRaftServer *server,
                                  uint32_t groupId, struct kraftState *state);

/* Remove a raft group */
bool kraftMultiRaftServerRemoveGroup(kraftMultiRaftServer *server,
                                     uint32_t groupId);

/* Get raft state for a group */
struct kraftState *kraftMultiRaftServerGetGroup(kraftMultiRaftServer *server,
                                                uint32_t groupId);

/* Destroy multi-raft server */
void kraftMultiRaftServerDestroy(kraftMultiRaftServer *server);

#endif /* KRAFT_NETWORK_H */
