/* loopyClientAPI - Client API for external connections
 *
 * Provides a TCP listener for external clients to connect and
 * perform key-value operations against the Raft cluster.
 *
 * The client API uses a simple Redis-like text protocol:
 *   SET key value\r\n
 *   GET key\r\n
 *   DEL key\r\n
 *   INCR key\r\n
 *   KEYS\r\n
 *
 * Responses:
 *   +OK\r\n           - Success (no data)
 *   $N\r\ndata\r\n    - Bulk string response
 *   :N\r\n            - Integer response
 *   -ERR message\r\n  - Error
 *   -REDIRECT N\r\n   - Not leader, redirect to node N
 *
 * This is part of loopy-demo, demonstrating kraft usage.
 */

#ifndef LOOPY_CLIENT_API_H
#define LOOPY_CLIENT_API_H

#include "loopyKV.h"
#include "loopyServer.h"

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
 * Client API handle.
 */
typedef struct loopyClientAPI loopyClientAPI;

/**
 * Client connection handle.
 */
typedef struct loopyClientConn loopyClientConn;

/**
 * Client API statistics.
 */
typedef struct loopyClientAPIStats {
    uint64_t connectionsAccepted; /* Total connections accepted */
    uint64_t connectionsActive;   /* Currently active connections */
    uint64_t commandsReceived;    /* Total commands received */
    uint64_t commandsSucceeded;   /* Commands that succeeded */
    uint64_t commandsFailed;      /* Commands that failed */
    uint64_t redirectsSent;       /* Redirect responses sent */
    uint64_t bytesReceived;       /* Total bytes received */
    uint64_t bytesSent;           /* Total bytes sent */
} loopyClientAPIStats;

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

/**
 * Create a client API.
 *
 * @param loop   Event loop (from loopyPlatformGetLoop)
 * @param port   Port to listen on
 * @param server Server for command submission
 * @param kv     KV store for local reads
 * @return Client API handle, or NULL on error
 */
loopyClientAPI *loopyClientAPICreate(struct loopyLoop *loop, int port,
                                     loopyServer *server, loopyKV *kv);

/**
 * Destroy a client API.
 *
 * Closes all client connections and frees resources.
 *
 * @param api Client API handle (may be NULL)
 */
void loopyClientAPIDestroy(loopyClientAPI *api);

/**
 * Start the client API listener.
 *
 * Binds to the configured port and begins accepting connections.
 *
 * @param api Client API handle
 * @return 0 on success, -1 on error
 */
int loopyClientAPIStart(loopyClientAPI *api);

/**
 * Stop the client API listener.
 *
 * Stops accepting new connections but keeps existing ones.
 *
 * @param api Client API handle
 */
void loopyClientAPIStop(loopyClientAPI *api);

/* ============================================================================
 * Properties
 * ============================================================================
 */

/**
 * Get statistics.
 */
void loopyClientAPIGetStats(const loopyClientAPI *api,
                            loopyClientAPIStats *stats);

/**
 * Get the listening port.
 */
int loopyClientAPIGetPort(const loopyClientAPI *api);

/**
 * Get number of active connections.
 */
size_t loopyClientAPIGetConnectionCount(const loopyClientAPI *api);

/**
 * Check if the API is listening.
 */
bool loopyClientAPIIsListening(const loopyClientAPI *api);

#endif /* LOOPY_CLIENT_API_H */
