/* loopyTransport - TCP transport layer for Raft networking
 *
 * Manages TCP server (accepting incoming connections) and outbound
 * connections to other Raft nodes. Provides connection lifecycle
 * management and reconnection logic.
 *
 * This is part of loopy-demo, a consumer of kraft (not part of kraft itself).
 */

#ifndef LOOPY_TRANSPORT_H
#define LOOPY_TRANSPORT_H

#include "loopyConnection.h"

#include <stdbool.h>
#include <stdint.h>

/* Forward declarations */
struct loopyLoop;

/* ============================================================================
 * Types
 * ============================================================================
 */

/**
 * Transport handle.
 *
 * Manages all TCP networking for a single Raft node: both inbound
 * (server) and outbound (client) connections.
 */
typedef struct loopyTransport loopyTransport;

/**
 * Connection type (direction).
 */
typedef enum loopyTransportConnType {
    LOOPY_CONN_INBOUND, /* Accepted from server */
    LOOPY_CONN_OUTBOUND /* Connected to peer */
} loopyTransportConnType;

/**
 * Transport event types.
 */
typedef enum loopyTransportEvent {
    LOOPY_TRANSPORT_NEW_CONNECTION, /* New inbound connection */
    LOOPY_TRANSPORT_CONNECTED,      /* Outbound connection established */
    LOOPY_TRANSPORT_DATA,           /* Data received on a connection */
    LOOPY_TRANSPORT_DISCONNECTED,   /* Connection closed or errored */
    LOOPY_TRANSPORT_RECONNECTING    /* Starting reconnection attempt */
} loopyTransportEvent;

/**
 * Transport event callback.
 *
 * @param transport Transport handle
 * @param event     Event type
 * @param conn      Connection handle (valid for all events)
 * @param connType  Whether connection is inbound or outbound
 * @param data      Data buffer (only valid for LOOPY_TRANSPORT_DATA)
 * @param len       Data length (only valid for LOOPY_TRANSPORT_DATA)
 * @param userData  User context
 */
typedef void loopyTransportCallback(loopyTransport *transport,
                                    loopyTransportEvent event, loopyConn *conn,
                                    loopyTransportConnType connType,
                                    const void *data, size_t len,
                                    void *userData);

/**
 * Transport configuration.
 */
typedef struct loopyTransportConfig {
    /* Server settings */
    const char *bindAddr; /* Address to bind (default: "0.0.0.0") */
    int port;             /* Port to listen on (required) */
    int backlog;          /* Listen backlog (default: 128) */

    /* Connection settings */
    loopyConnConfig connConfig; /* Passed to loopyConn */

    /* Reconnection settings */
    bool autoReconnect;           /* Auto-reconnect outbound (default: true) */
    uint32_t reconnectDelayMs;    /* Initial delay (default: 100ms) */
    uint32_t reconnectMaxDelayMs; /* Max delay with backoff (default: 5000ms) */
    uint32_t
        reconnectMaxAttempts; /* Max attempts, 0 = unlimited (default: 0) */
} loopyTransportConfig;

/* ============================================================================
 * Configuration
 * ============================================================================
 */

/**
 * Initialize config with defaults.
 *
 * Note: You must set cfg->port before creating the transport.
 */
void loopyTransportConfigInit(loopyTransportConfig *cfg);

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

/**
 * Create a transport.
 *
 * Creates the transport but does not start listening. Call
 * loopyTransportStart() to begin accepting connections.
 *
 * @param loop     Event loop
 * @param cb       Event callback
 * @param userData User context for callback
 * @param cfg      Configuration (required, must have port set)
 * @return Transport handle, or NULL on error
 */
loopyTransport *loopyTransportCreate(struct loopyLoop *loop,
                                     loopyTransportCallback *cb, void *userData,
                                     const loopyTransportConfig *cfg);

/**
 * Start the transport.
 *
 * Begins listening for incoming connections.
 *
 * @param transport Transport handle
 * @return true on success
 */
bool loopyTransportStart(loopyTransport *transport);

/**
 * Stop the transport.
 *
 * Stops accepting new connections but keeps existing connections open.
 *
 * @param transport Transport handle
 */
void loopyTransportStop(loopyTransport *transport);

/**
 * Destroy the transport.
 *
 * Closes all connections and frees resources.
 *
 * @param transport Transport handle (may be NULL)
 */
void loopyTransportDestroy(loopyTransport *transport);

/* ============================================================================
 * Outbound Connections
 *
 * For connecting to other Raft nodes in the cluster.
 * ============================================================================
 */

/**
 * Peer identifier for outbound connections.
 */
typedef uint64_t loopyPeerId;

/**
 * Connect to a peer.
 *
 * Initiates an outbound connection to the specified address.
 * The callback will receive LOOPY_TRANSPORT_CONNECTED on success
 * or LOOPY_TRANSPORT_DISCONNECTED on failure.
 *
 * If autoReconnect is enabled and the connection fails or is later
 * disconnected, automatic reconnection will be attempted.
 *
 * @param transport Transport handle
 * @param peerId    Unique ID for this peer (for tracking)
 * @param addr      Remote address
 * @param port      Remote port
 * @return true if connection initiated
 */
bool loopyTransportConnect(loopyTransport *transport, loopyPeerId peerId,
                           const char *addr, int port);

/**
 * Disconnect from a peer.
 *
 * Closes the connection and disables auto-reconnect for this peer.
 *
 * @param transport Transport handle
 * @param peerId    Peer to disconnect
 */
void loopyTransportDisconnect(loopyTransport *transport, loopyPeerId peerId);

/**
 * Get connection for a peer.
 *
 * @param transport Transport handle
 * @param peerId    Peer ID
 * @return Connection handle, or NULL if not connected
 */
loopyConn *loopyTransportGetPeerConn(loopyTransport *transport,
                                     loopyPeerId peerId);

/**
 * Check if connected to a peer.
 */
bool loopyTransportIsPeerConnected(loopyTransport *transport,
                                   loopyPeerId peerId);

/* ============================================================================
 * Connection Management
 * ============================================================================
 */

/**
 * Get number of active connections.
 */
size_t loopyTransportGetConnectionCount(const loopyTransport *transport);

/**
 * Get number of inbound connections.
 */
size_t loopyTransportGetInboundCount(const loopyTransport *transport);

/**
 * Get number of outbound connections.
 */
size_t loopyTransportGetOutboundCount(const loopyTransport *transport);

/**
 * Close all inbound connections.
 *
 * Useful when stepping down as a node or during shutdown.
 */
void loopyTransportCloseInbound(loopyTransport *transport);

/**
 * Close all outbound connections.
 *
 * Also disables auto-reconnect for all peers.
 */
void loopyTransportCloseOutbound(loopyTransport *transport);

/* ============================================================================
 * Properties
 * ============================================================================
 */

/**
 * Get the event loop.
 */
struct loopyLoop *loopyTransportGetLoop(const loopyTransport *transport);

/**
 * Get the listening port.
 */
int loopyTransportGetPort(const loopyTransport *transport);

/**
 * Check if transport is running.
 */
bool loopyTransportIsRunning(const loopyTransport *transport);

/**
 * Get/set user data.
 */
void *loopyTransportGetUserData(const loopyTransport *transport);
void loopyTransportSetUserData(loopyTransport *transport, void *userData);

/**
 * Transport statistics.
 */
typedef struct loopyTransportStats {
    uint64_t totalAccepted;     /* Total inbound connections accepted */
    uint64_t totalConnected;    /* Total outbound connections established */
    uint64_t totalDisconnected; /* Total disconnections (both directions) */
    uint64_t reconnectAttempts; /* Total reconnection attempts */
    uint64_t bytesReceived;     /* Total bytes received */
    uint64_t bytesSent;         /* Total bytes sent */
} loopyTransportStats;

void loopyTransportGetStats(const loopyTransport *transport,
                            loopyTransportStats *stats);

/* ============================================================================
 * Iteration
 * ============================================================================
 */

/**
 * Connection iterator callback.
 *
 * @param transport Transport handle
 * @param conn      Connection handle
 * @param connType  Connection type
 * @param userData  User context
 * @return true to continue, false to stop iteration
 */
typedef bool loopyTransportIterCallback(loopyTransport *transport,
                                        loopyConn *conn,
                                        loopyTransportConnType connType,
                                        void *userData);

/**
 * Iterate over all connections.
 *
 * @param transport Transport handle
 * @param cb        Iterator callback
 * @param userData  User context
 */
void loopyTransportForEach(loopyTransport *transport,
                           loopyTransportIterCallback *cb, void *userData);

#endif /* LOOPY_TRANSPORT_H */
