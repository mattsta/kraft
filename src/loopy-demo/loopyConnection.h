/* loopyConnection - Per-connection I/O abstraction
 *
 * Wraps loopyStream to provide clean TCP connection management with
 * automatic buffer handling and lifecycle management.
 *
 * This is part of loopy-demo, a consumer of kraft (not part of kraft itself).
 */

#ifndef LOOPY_CONNECTION_H
#define LOOPY_CONNECTION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declarations */
struct loopyLoop;
struct loopyStream;

/* ============================================================================
 * Types
 * ============================================================================
 */

/**
 * Connection handle.
 */
typedef struct loopyConn loopyConn;

/**
 * Connection state.
 */
typedef enum loopyConnState {
    LOOPY_CONN_CONNECTING, /* Outbound connection in progress */
    LOOPY_CONN_CONNECTED,  /* Ready for I/O */
    LOOPY_CONN_CLOSING,    /* Shutdown initiated */
    LOOPY_CONN_CLOSED      /* Fully closed */
} loopyConnState;

/**
 * Connection event types for callbacks.
 */
typedef enum loopyConnEvent {
    LOOPY_CONN_EVENT_CONNECTED, /* Connection established (outbound) */
    LOOPY_CONN_EVENT_DATA,      /* Data received */
    LOOPY_CONN_EVENT_CLOSE,     /* Connection closed (graceful or error) */
    LOOPY_CONN_EVENT_ERROR      /* Error occurred */
} loopyConnEvent;

/**
 * Event callback.
 *
 * @param conn     Connection handle
 * @param event    Event type
 * @param data     Data buffer (only valid for LOOPY_CONN_EVENT_DATA)
 * @param len      Data length (only valid for LOOPY_CONN_EVENT_DATA)
 * @param userData User context
 */
typedef void loopyConnCallback(loopyConn *conn, loopyConnEvent event,
                               const void *data, size_t len, void *userData);

/**
 * Connection configuration.
 */
typedef struct loopyConnConfig {
    size_t readBufSize;    /* Initial read buffer size (default: 4KB) */
    size_t maxReadBufSize; /* Max read buffer size (default: 64KB) */
    size_t writeBufSize;   /* Initial write buffer size (default: 4KB) */
    bool tcpNoDelay;       /* Enable TCP_NODELAY (default: true) */
    bool tcpKeepAlive;     /* Enable TCP keepalive (default: true) */
} loopyConnConfig;

/* ============================================================================
 * Configuration
 * ============================================================================
 */

/**
 * Initialize config with defaults.
 */
void loopyConnConfigInit(loopyConnConfig *cfg);

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

/**
 * Create connection from accepted client stream.
 *
 * Takes ownership of the stream. Used by server code after accept.
 *
 * @param stream   Accepted loopyStream (ownership transferred)
 * @param cb       Event callback
 * @param userData User context for callback
 * @param cfg      Configuration (NULL for defaults)
 * @return Connection handle, or NULL on error
 */
loopyConn *loopyConnFromStream(struct loopyStream *stream,
                               loopyConnCallback *cb, void *userData,
                               const loopyConnConfig *cfg);

/**
 * Create outbound connection.
 *
 * Initiates a TCP connection to the specified address. The callback
 * will receive LOOPY_CONN_EVENT_CONNECTED on success or LOOPY_CONN_EVENT_ERROR
 * on failure.
 *
 * @param loop     Event loop
 * @param addr     Remote address (e.g., "127.0.0.1", "::1")
 * @param port     Remote port
 * @param cb       Event callback
 * @param userData User context for callback
 * @param cfg      Configuration (NULL for defaults)
 * @return Connection handle, or NULL on immediate error
 */
loopyConn *loopyConnConnect(struct loopyLoop *loop, const char *addr, int port,
                            loopyConnCallback *cb, void *userData,
                            const loopyConnConfig *cfg);

/**
 * Close connection gracefully.
 *
 * Initiates a graceful shutdown. Pending writes will be flushed.
 * The callback will receive LOOPY_CONN_EVENT_CLOSE when complete.
 *
 * @param conn Connection to close (may be NULL)
 */
void loopyConnClose(loopyConn *conn);

/**
 * Close connection immediately.
 *
 * Forces immediate close without flushing pending writes.
 *
 * @param conn Connection to close (may be NULL)
 */
void loopyConnCloseImmediate(loopyConn *conn);

/* ============================================================================
 * I/O Operations
 * ============================================================================
 */

/**
 * Write data to connection.
 *
 * Data is copied and queued. Returns immediately; actual send is asynchronous.
 *
 * @param conn Connection
 * @param data Data to write
 * @param len  Data length
 * @return true if queued successfully, false on error
 */
bool loopyConnWrite(loopyConn *conn, const void *data, size_t len);

/**
 * Get pending write queue size.
 *
 * @param conn Connection
 * @return Bytes queued for writing
 */
size_t loopyConnGetWriteQueueSize(const loopyConn *conn);

/**
 * Pause reading from connection.
 *
 * Useful for backpressure when processing can't keep up.
 *
 * @param conn Connection
 */
void loopyConnPauseRead(loopyConn *conn);

/**
 * Resume reading from connection.
 *
 * @param conn Connection
 */
void loopyConnResumeRead(loopyConn *conn);

/* ============================================================================
 * Properties
 * ============================================================================
 */

/**
 * Get connection state.
 */
loopyConnState loopyConnGetState(const loopyConn *conn);

/**
 * Get underlying event loop.
 */
struct loopyLoop *loopyConnGetLoop(const loopyConn *conn);

/**
 * Get underlying stream (for advanced use).
 */
struct loopyStream *loopyConnGetStream(const loopyConn *conn);

/**
 * Get/set user data.
 */
void *loopyConnGetUserData(const loopyConn *conn);
void loopyConnSetUserData(loopyConn *conn, void *userData);

/**
 * Change the event callback.
 *
 * Allows changing the callback after creation (e.g., when ownership
 * is transferred to a session layer).
 *
 * @param conn     Connection
 * @param cb       New callback
 * @param userData New user context
 */
void loopyConnSetCallback(loopyConn *conn, loopyConnCallback *cb,
                          void *userData);

/**
 * Get local address.
 *
 * @param conn    Connection
 * @param addr    Buffer for address string
 * @param addrLen Buffer length
 * @param port    OUT: port number (optional, may be NULL)
 * @return true on success
 */
bool loopyConnGetLocalAddr(loopyConn *conn, char *addr, size_t addrLen,
                           int *port);

/**
 * Get peer address.
 *
 * @param conn    Connection
 * @param addr    Buffer for address string
 * @param addrLen Buffer length
 * @param port    OUT: port number (optional, may be NULL)
 * @return true on success
 */
bool loopyConnGetPeerAddr(loopyConn *conn, char *addr, size_t addrLen,
                          int *port);

/**
 * Get connection statistics.
 */
typedef struct loopyConnStats {
    uint64_t bytesRead;
    uint64_t bytesWritten;
    uint64_t readCallbacks;
    uint64_t writeCallbacks;
} loopyConnStats;

void loopyConnGetStats(const loopyConn *conn, loopyConnStats *stats);

#endif /* LOOPY_CONNECTION_H */
