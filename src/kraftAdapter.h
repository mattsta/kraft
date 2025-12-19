/* Kraft Pluggable Network Adapter Interface
 * ==========================================
 *
 * This header defines the abstract interface for network I/O adapters.
 * Implementations can use any event loop (libuv, epoll, kqueue, io_uring, etc.)
 *
 * The adapter is responsible for:
 *   - Listening for incoming connections (Raft peers and clients)
 *   - Establishing outbound connections to peers
 *   - Reading/writing data on connections
 *   - Timer management (election, heartbeat, maintenance)
 *   - Running the event loop
 *
 * Usage:
 *   1. Create adapter: kraftAdapterCreate(&ops, userData)
 *   2. Configure kraft: kraftSetAdapter(state, adapter)
 *   3. Start server: kraftAdapterStart(adapter, "0.0.0.0", 5000)
 *   4. Run event loop: kraftAdapterRun(adapter)  // blocks
 *   5. Cleanup: kraftAdapterDestroy(adapter)
 *
 * Thread Safety:
 *   - All callbacks are invoked from the event loop thread
 *   - kraftAdapterStop() is safe to call from any thread
 */

#ifndef KRAFT_ADAPTER_H
#define KRAFT_ADAPTER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declarations */
struct kraftState;
struct kraftAdapter;
struct kraftConnection;

/* =========================================================================
 * Connection Types
 * ========================================================================= */

typedef enum kraftConnectionType {
    KRAFT_CONN_PEER_INBOUND = 1, /* Incoming Raft peer connection */
    KRAFT_CONN_PEER_OUTBOUND,    /* Outgoing Raft peer connection */
    KRAFT_CONN_CLIENT,           /* External client connection */
} kraftConnectionType;

typedef enum kraftConnectionState {
    KRAFT_CONN_STATE_CONNECTING = 1,
    KRAFT_CONN_STATE_CONNECTED,
    KRAFT_CONN_STATE_VERIFIED, /* HELLO handshake complete (peers only) */
    KRAFT_CONN_STATE_CLOSING,
    KRAFT_CONN_STATE_CLOSED,
} kraftConnectionState;

/* =========================================================================
 * Connection Handle
 * ========================================================================= */

/* Opaque connection handle - implementation-specific */
typedef struct kraftConnection {
    uint64_t id;                /* Unique connection ID */
    kraftConnectionType type;   /* Peer or client */
    kraftConnectionState state; /* Current state */
    void *adapterData;          /* Adapter-specific data (e.g., uv_tcp_t*) */
    void *userData;             /* User-attached data */

    /* Peer info (filled after connection/accept) */
    char remoteIp[46]; /* IPv4 or IPv6 */
    uint16_t remotePort;
    char localIp[46];
    uint16_t localPort;

    /* Statistics */
    uint64_t bytesReceived;
    uint64_t bytesSent;
    uint64_t messagesReceived;
    uint64_t messagesSent;
    uint64_t connectTimeUs; /* When connection was established */
} kraftConnection;

/* =========================================================================
 * Timer Handle
 * ========================================================================= */

typedef enum kraftTimerType {
    KRAFT_TIMER_ELECTION = 1,     /* Follower: no heartbeat timeout */
    KRAFT_TIMER_HEARTBEAT,        /* Leader: send heartbeats */
    KRAFT_TIMER_ELECTION_BACKOFF, /* Candidate: retry delay */
    KRAFT_TIMER_MAINTENANCE,    /* Periodic maintenance (reconnection, etc.) */
    KRAFT_TIMER_SNAPSHOT,       /* Snapshot creation timer */
    KRAFT_TIMER_CLIENT_TIMEOUT, /* Client request timeout */
    KRAFT_TIMER_CUSTOM,         /* User-defined timer */
} kraftTimerType;

/* Opaque timer handle */
typedef struct kraftTimer {
    uint64_t id;         /* Unique timer ID */
    kraftTimerType type; /* Timer type */
    uint64_t intervalMs; /* Timer interval in milliseconds */
    bool repeating;      /* One-shot or repeating */
    bool active;         /* Currently running */
    void *adapterData;   /* Adapter-specific data */
    void *userData;      /* User-attached data */
} kraftTimer;

/* =========================================================================
 * Adapter Callbacks (from adapter TO kraft core)
 * ========================================================================= */

/* Called when a new peer/client connection is accepted */
typedef void (*kraftOnAcceptFn)(struct kraftAdapter *adapter,
                                kraftConnection *conn,
                                kraftConnectionType type);

/* Called when outbound connection completes (success or failure) */
typedef void (*kraftOnConnectFn)(struct kraftAdapter *adapter,
                                 kraftConnection *conn, bool success,
                                 const char *errorMsg /* NULL on success */
);

/* Called when data is received on a connection */
typedef void (*kraftOnDataFn)(struct kraftAdapter *adapter,
                              kraftConnection *conn, const void *data,
                              size_t len);

/* Called when connection is closed (by peer or error) */
typedef void (*kraftOnCloseFn)(struct kraftAdapter *adapter,
                               kraftConnection *conn,
                               const char *reason /* NULL for normal close */
);

/* Called when write completes (for flow control) */
typedef void (*kraftOnWriteCompleteFn)(struct kraftAdapter *adapter,
                                       kraftConnection *conn,
                                       size_t bytesWritten, bool success);

/* Called when timer fires */
typedef void (*kraftOnTimerFn)(struct kraftAdapter *adapter, kraftTimer *timer);

/* =========================================================================
 * Adapter Operations (from kraft core TO adapter implementation)
 * ========================================================================= */

typedef struct kraftAdapterOps {
    /* ===== Lifecycle ===== */

    /* Initialize adapter (called once at startup) */
    bool (*init)(struct kraftAdapter *adapter);

    /* Cleanup adapter (called once at shutdown) */
    void (*destroy)(struct kraftAdapter *adapter);

    /* Start listening on address:port
     * Returns true on success, false on failure */
    bool (*listen)(struct kraftAdapter *adapter, const char *address,
                   uint16_t port,
                   kraftConnectionType acceptAs); /* PEER or CLIENT */

    /* Stop listening (but don't destroy adapter) */
    void (*stopListening)(struct kraftAdapter *adapter);

    /* Run event loop (blocks until stopped) */
    void (*run)(struct kraftAdapter *adapter);

    /* Stop event loop (can be called from any thread) */
    void (*stop)(struct kraftAdapter *adapter);

    /* Process pending events without blocking (for integration with other
     * loops) */
    int (*poll)(struct kraftAdapter *adapter, uint64_t timeoutMs);

    /* ===== Connection Management ===== */

    /* Initiate outbound connection */
    kraftConnection *(*connect)(struct kraftAdapter *adapter,
                                const char *address, uint16_t port,
                                kraftConnectionType type);

    /* Write data to connection (async, non-blocking) */
    bool (*write)(struct kraftAdapter *adapter, kraftConnection *conn,
                  const void *data, size_t len);

    /* Close connection gracefully */
    void (*close)(struct kraftAdapter *adapter, kraftConnection *conn);

    /* Get connection info */
    bool (*getConnectionInfo)(kraftConnection *conn, char *remoteIp,
                              size_t ipLen, uint16_t *remotePort);

    /* ===== Timer Management ===== */

    /* Create and start a timer */
    kraftTimer *(*timerCreate)(struct kraftAdapter *adapter,
                               kraftTimerType type, uint64_t intervalMs,
                               bool repeating, void *userData);

    /* Start/restart timer with new interval */
    bool (*timerStart)(struct kraftAdapter *adapter, kraftTimer *timer,
                       uint64_t intervalMs);

    /* Stop timer (can be restarted) */
    void (*timerStop)(struct kraftAdapter *adapter, kraftTimer *timer);

    /* Destroy timer (cannot be restarted) */
    void (*timerDestroy)(struct kraftAdapter *adapter, kraftTimer *timer);

    /* Reset timer to start counting from now */
    void (*timerReset)(struct kraftAdapter *adapter, kraftTimer *timer);

    /* ===== Utilities ===== */

    /* Get current time in microseconds (monotonic) */
    uint64_t (*now)(struct kraftAdapter *adapter);

    /* Schedule callback to run on next event loop iteration */
    bool (*scheduleCallback)(struct kraftAdapter *adapter,
                             void (*callback)(void *), void *data);

    /* Get number of active connections */
    uint32_t (*connectionCount)(struct kraftAdapter *adapter);

} kraftAdapterOps;

/* =========================================================================
 * Adapter Handle
 * ========================================================================= */

typedef struct kraftAdapter {
    /* Adapter implementation operations */
    const kraftAdapterOps *ops;

    /* Callbacks into kraft core */
    kraftOnAcceptFn onAccept;
    kraftOnConnectFn onConnect;
    kraftOnDataFn onData;
    kraftOnCloseFn onClose;
    kraftOnWriteCompleteFn onWriteComplete;
    kraftOnTimerFn onTimer;

    /* Back-reference to kraft state */
    struct kraftState *state;

    /* Implementation-specific data */
    void *impl;

    /* User data */
    void *userData;

    /* Configuration */
    struct {
        uint32_t maxConnections;     /* Max simultaneous connections */
        uint32_t recvBufferSize;     /* Per-connection receive buffer */
        uint32_t sendBufferSize;     /* Per-connection send buffer */
        uint64_t connectTimeoutMs;   /* Outbound connection timeout */
        uint64_t writeTimeoutMs;     /* Write operation timeout */
        bool enableTcpNodelay;       /* Disable Nagle's algorithm */
        bool enableKeepalive;        /* Enable TCP keepalive */
        uint32_t keepaliveIntervalS; /* Keepalive probe interval */
    } config;

    /* Statistics */
    struct {
        uint64_t connectionsAccepted;
        uint64_t connectionsInitiated;
        uint64_t connectionsFailed;
        uint64_t connectionsClosed;
        uint64_t bytesReceived;
        uint64_t bytesSent;
        uint64_t timersCreated;
        uint64_t timersFired;
    } stats;

} kraftAdapter;

/* =========================================================================
 * Adapter Lifecycle Functions
 * ========================================================================= */

/* Create adapter with given operations and callbacks */
kraftAdapter *kraftAdapterCreate(const kraftAdapterOps *ops, void *userData);

/* Set default configuration values */
void kraftAdapterConfigDefault(kraftAdapter *adapter);

/* Destroy adapter and free resources */
void kraftAdapterDestroy(kraftAdapter *adapter);

/* Associate adapter with kraft state */
void kraftAdapterSetState(kraftAdapter *adapter, struct kraftState *state);

/* =========================================================================
 * Convenience Macros for Adapter Implementation
 * ========================================================================= */

/* Invoke callback if set */
#define KRAFT_ADAPTER_CALLBACK(adapter, cb, ...)                               \
    do {                                                                       \
        if ((adapter)->cb)                                                     \
            (adapter)->cb((adapter), ##__VA_ARGS__);                           \
    } while (0)

/* Check if adapter is valid */
#define KRAFT_ADAPTER_VALID(adapter) ((adapter) && (adapter)->ops)

/* =========================================================================
 * Built-in Timer Intervals (milliseconds)
 * ========================================================================= */

#define KRAFT_TIMER_ELECTION_MIN_MS 150
#define KRAFT_TIMER_ELECTION_MAX_MS 300
#define KRAFT_TIMER_HEARTBEAT_MS 50
#define KRAFT_TIMER_MAINTENANCE_MS 1000
#define KRAFT_TIMER_SNAPSHOT_CHECK_MS 5000
#define KRAFT_TIMER_CLIENT_TIMEOUT_MS 30000

/* Random jitter for election timeout */
#define KRAFT_TIMER_ELECTION_JITTER_MS 150

/* =========================================================================
 * Adapter Statistics
 * ========================================================================= */

typedef struct kraftAdapterStats {
    uint64_t connectionsAccepted;
    uint64_t connectionsInitiated;
    uint64_t connectionsFailed;
    uint64_t connectionsClosed;
    uint64_t activeConnections;
    uint64_t bytesReceived;
    uint64_t bytesSent;
    uint64_t timersActive;
    uint64_t timersFired;
    uint64_t uptimeUs;
} kraftAdapterStats;

void kraftAdapterGetStats(const kraftAdapter *adapter,
                          kraftAdapterStats *stats);

/* Format stats as string (returns bytes written) */
int kraftAdapterFormatStats(const kraftAdapterStats *stats, char *buf,
                            size_t bufLen);

#endif /* KRAFT_ADAPTER_H */
