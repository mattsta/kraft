/* Kraft Client API
 * ==================
 *
 * This header defines the client-facing API for submitting commands to a
 * Kraft cluster and receiving responses. It handles:
 *
 *   - Command submission (sync and async)
 *   - Response handling with callbacks
 *   - Leader discovery and redirect handling
 *   - Request pipelining and batching
 *   - Timeout and retry logic
 *   - Session management for linearizable reads
 *
 * Architecture:
 *   Application -> kraftClientAPI -> kraftAdapter -> Network
 *                                 -> kraftProtocol -> Wire Format
 *
 * Usage Patterns:
 *
 *   1. Fire-and-forget (async):
 *      kraftClientSubmit(client, cmd, len, NULL, NULL);
 *
 *   2. Async with callback:
 *      kraftClientSubmit(client, cmd, len, onResponse, userData);
 *
 *   3. Synchronous (blocks):
 *      result = kraftClientSubmitSync(client, cmd, len, &response, timeout);
 *
 *   4. Linearizable read:
 *      kraftClientRead(client, key, len, onRead, userData);
 *
 * Thread Safety:
 *   - kraftClient instances are NOT thread-safe
 *   - Use one client per thread, or protect with external mutex
 *   - Callbacks are invoked from the adapter's event loop thread
 */

#ifndef KRAFT_CLIENT_API_H
#define KRAFT_CLIENT_API_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Forward declarations */
struct kraftAdapter;
struct kraftProtocol;
struct kraftClientSession;
struct kraftClientRequest;

/* =========================================================================
 * Client Configuration
 * ========================================================================= */

typedef struct kraftClientConfig {
    /* Timeouts (milliseconds) */
    uint64_t connectTimeoutMs; /* Connection establishment timeout */
    uint64_t requestTimeoutMs; /* Per-request timeout */
    uint64_t sessionTimeoutMs; /* Session keepalive timeout */

    /* Retry behavior */
    uint32_t maxRetries;           /* Max retries per request (0 = no retry) */
    uint64_t retryBackoffMs;       /* Initial backoff between retries */
    uint64_t retryBackoffMaxMs;    /* Maximum backoff */
    double retryBackoffMultiplier; /* Backoff multiplier (e.g., 2.0) */

    /* Connection management */
    uint32_t maxPendingRequests; /* Max in-flight requests per connection */
    uint32_t connectionPoolSize; /* Number of connections to maintain */
    bool autoReconnect;          /* Auto-reconnect on disconnect */
    bool followRedirects;        /* Auto-follow leader redirects */

    /* Request options */
    bool enableBatching;   /* Batch multiple requests */
    uint32_t batchMaxSize; /* Max requests per batch */
    uint64_t batchDelayMs; /* Max delay before sending batch */

    /* Session options (for linearizable semantics) */
    bool enableSession; /* Enable session tracking */
    uint64_t clientId;  /* Client identifier (0 = auto-generate) */
} kraftClientConfig;

/* Default configuration values */
#define KRAFT_CLIENT_DEFAULT_CONNECT_TIMEOUT_MS 5000
#define KRAFT_CLIENT_DEFAULT_REQUEST_TIMEOUT_MS 10000
#define KRAFT_CLIENT_DEFAULT_SESSION_TIMEOUT_MS 30000
#define KRAFT_CLIENT_DEFAULT_MAX_RETRIES 3
#define KRAFT_CLIENT_DEFAULT_RETRY_BACKOFF_MS 100
#define KRAFT_CLIENT_DEFAULT_RETRY_BACKOFF_MAX_MS 5000
#define KRAFT_CLIENT_DEFAULT_RETRY_MULTIPLIER 2.0
#define KRAFT_CLIENT_DEFAULT_MAX_PENDING 256
#define KRAFT_CLIENT_DEFAULT_POOL_SIZE 1
#define KRAFT_CLIENT_DEFAULT_BATCH_SIZE 64
#define KRAFT_CLIENT_DEFAULT_BATCH_DELAY_MS 1

/* =========================================================================
 * Request/Response Types
 * ========================================================================= */

typedef enum kraftClientStatus {
    KRAFT_CLIENT_OK = 0,       /* Success */
    KRAFT_CLIENT_ERROR,        /* Generic error */
    KRAFT_CLIENT_TIMEOUT,      /* Request timed out */
    KRAFT_CLIENT_NOT_LEADER,   /* Node is not leader (redirect) */
    KRAFT_CLIENT_NO_LEADER,    /* No leader currently elected */
    KRAFT_CLIENT_DISCONNECTED, /* Connection lost */
    KRAFT_CLIENT_REJECTED,     /* Request rejected (e.g., cluster busy) */
    KRAFT_CLIENT_INVALID,      /* Invalid request */
    KRAFT_CLIENT_SHUTDOWN,     /* Client is shutting down */
} kraftClientStatus;

/* Convert status to string */
const char *kraftClientStatusString(kraftClientStatus status);

/* Request handle for tracking async requests */
typedef struct kraftClientRequest {
    uint64_t requestId;       /* Unique request ID */
    uint64_t clientSeq;       /* Client sequence number */
    uint64_t submitTimeUs;    /* When request was submitted */
    uint64_t timeoutUs;       /* Absolute timeout time */
    uint32_t retryCount;      /* Number of retries so far */
    kraftClientStatus status; /* Current status */
    void *userData;           /* User-attached data */

    /* For tracking in client's pending list */
    struct kraftClientRequest *next;
    struct kraftClientRequest *prev;
} kraftClientRequest;

/* Response data */
typedef struct kraftClientResponse {
    kraftClientStatus status; /* Response status */
    uint64_t requestId;       /* Original request ID */
    uint64_t logIndex; /* Index where command was logged (if committed) */
    uint64_t term;     /* Term of the log entry */

    /* Response payload */
    const uint8_t *data; /* Response data (may be NULL) */
    size_t dataLen;      /* Response data length */

    /* Leader hint (if NOT_LEADER) */
    uint64_t leaderNodeId;   /* Current leader's node ID (0 = unknown) */
    char leaderAddress[256]; /* Current leader's address (if known) */
    uint16_t leaderPort;     /* Current leader's port */

    /* Timing */
    uint64_t latencyUs; /* Request round-trip time */
} kraftClientResponse;

/* =========================================================================
 * Callback Types
 * ========================================================================= */

/* Called when a command submission completes */
typedef void (*kraftClientResponseFn)(struct kraftClientSession *session,
                                      const kraftClientRequest *request,
                                      const kraftClientResponse *response,
                                      void *userData);

/* Called when a read completes */
typedef void (*kraftClientReadFn)(struct kraftClientSession *session,
                                  const kraftClientRequest *request,
                                  const kraftClientResponse *response,
                                  const uint8_t *value, size_t valueLen,
                                  void *userData);

/* Called when connection state changes */
typedef void (*kraftClientConnectFn)(struct kraftClientSession *session,
                                     bool connected, const char *address,
                                     uint16_t port,
                                     const char *errorMsg /* NULL on success */
);

/* Called when leader changes (for redirect handling) */
typedef void (*kraftClientLeaderChangeFn)(struct kraftClientSession *session,
                                          uint64_t newLeaderId,
                                          const char *newLeaderAddress,
                                          uint16_t newLeaderPort);

/* =========================================================================
 * Cluster Endpoint Configuration
 * ========================================================================= */

typedef struct kraftClusterEndpoint {
    char address[256]; /* Node address (hostname or IP) */
    uint16_t port;     /* Node port */
    uint64_t nodeId;   /* Node ID (0 if unknown) */
    bool isLeader;     /* True if this is the known leader */
} kraftClusterEndpoint;

typedef struct kraftClusterConfig {
    kraftClusterEndpoint *endpoints;
    uint32_t endpointCount;
    uint64_t clusterId; /* Expected cluster ID (0 = any) */
} kraftClusterConfig;

/* =========================================================================
 * Client Session Handle
 * ========================================================================= */

typedef struct kraftClientSession {
    /* Configuration */
    kraftClientConfig config;
    kraftClusterConfig cluster;

    /* Network layer */
    struct kraftAdapter *adapter;
    struct kraftProtocol *protocol;

    /* Session state */
    uint64_t clientId;  /* Our client ID */
    uint64_t nextSeq;   /* Next sequence number */
    uint64_t sessionId; /* Server-assigned session ID */
    bool connected;     /* Currently connected */

    /* Leader tracking */
    uint64_t knownLeaderId;       /* Current known leader */
    char knownLeaderAddress[256]; /* Current leader address */
    uint16_t knownLeaderPort;     /* Current leader port */

    /* Pending requests */
    kraftClientRequest *pendingHead;
    kraftClientRequest *pendingTail;
    uint32_t pendingCount;

    /* Callbacks */
    kraftClientConnectFn onConnect;
    kraftClientLeaderChangeFn onLeaderChange;
    void *callbackUserData;

    /* Statistics */
    struct {
        uint64_t requestsSent;
        uint64_t responsesReceived;
        uint64_t timeouts;
        uint64_t retries;
        uint64_t redirects;
        uint64_t errors;
        uint64_t totalLatencyUs; /* Sum of all latencies */
    } stats;

    /* Implementation data */
    void *impl;

} kraftClientSession;

/* =========================================================================
 * Client Lifecycle
 * ========================================================================= */

/* Initialize default configuration */
void kraftClientConfigDefault(kraftClientConfig *config);

/* Create client session with given adapter and protocol */
kraftClientSession *kraftClientCreate(struct kraftAdapter *adapter,
                                      struct kraftProtocol *protocol,
                                      const kraftClientConfig *config);

/* Configure cluster endpoints */
bool kraftClientSetCluster(kraftClientSession *session,
                           const kraftClusterConfig *cluster);

/* Add a single endpoint to cluster configuration */
bool kraftClientAddEndpoint(kraftClientSession *session, const char *address,
                            uint16_t port);

/* Set connection callbacks */
void kraftClientSetCallbacks(kraftClientSession *session,
                             kraftClientConnectFn onConnect,
                             kraftClientLeaderChangeFn onLeaderChange,
                             void *userData);

/* Connect to cluster (async - use callback for result) */
bool kraftClientConnect(kraftClientSession *session);

/* Disconnect from cluster */
void kraftClientDisconnect(kraftClientSession *session);

/* Destroy client session and free resources */
void kraftClientDestroy(kraftClientSession *session);

/* =========================================================================
 * Command Submission
 * ========================================================================= */

/* Submit command asynchronously
 * Returns request handle on success, NULL on failure
 * Response delivered via callback */
kraftClientRequest *kraftClientSubmit(kraftClientSession *session,
                                      const uint8_t *command, size_t commandLen,
                                      kraftClientResponseFn callback,
                                      void *userData);

/* Submit command with specific type/options */
kraftClientRequest *kraftClientSubmitEx(
    kraftClientSession *session,
    uint32_t commandType, /* Application-specific command type */
    const uint8_t *command, size_t commandLen,
    uint64_t timeoutMs, /* 0 = use default */
    kraftClientResponseFn callback, void *userData);

/* Submit command synchronously (blocks until response or timeout) */
kraftClientStatus
kraftClientSubmitSync(kraftClientSession *session, const uint8_t *command,
                      size_t commandLen,
                      kraftClientResponse *response, /* OUT: response data */
                      uint64_t timeoutMs             /* 0 = use default */
);

/* Cancel a pending request */
bool kraftClientCancel(kraftClientSession *session,
                       kraftClientRequest *request);

/* Cancel all pending requests */
void kraftClientCancelAll(kraftClientSession *session);

/* =========================================================================
 * Linearizable Reads (ReadIndex)
 * ========================================================================= */

/* Perform linearizable read
 * This ensures the read sees all previously committed writes
 * Response delivered via callback */
kraftClientRequest *kraftClientRead(
    kraftClientSession *session,
    const uint8_t *key, /* Key/query to read (application-specific) */
    size_t keyLen, kraftClientReadFn callback, void *userData);

/* Synchronous linearizable read */
kraftClientStatus
kraftClientReadSync(kraftClientSession *session, const uint8_t *key,
                    size_t keyLen,
                    uint8_t **value,  /* OUT: value data (caller must free) */
                    size_t *valueLen, /* OUT: value length */
                    uint64_t timeoutMs);

/* =========================================================================
 * Batching Support
 * ========================================================================= */

/* Start a batch of commands
 * Commands submitted after this are queued until kraftClientBatchEnd() */
bool kraftClientBatchBegin(kraftClientSession *session);

/* End batch and send all queued commands */
bool kraftClientBatchEnd(kraftClientSession *session);

/* Discard current batch without sending */
void kraftClientBatchDiscard(kraftClientSession *session);

/* =========================================================================
 * Leader Discovery
 * ========================================================================= */

/* Get current known leader
 * Returns false if no leader known */
bool kraftClientGetLeader(kraftClientSession *session, uint64_t *nodeId,
                          char *address, size_t addressLen, uint16_t *port);

/* Force leader discovery (useful after long idle) */
bool kraftClientRefreshLeader(kraftClientSession *session);

/* Check if currently connected to leader */
bool kraftClientIsConnectedToLeader(kraftClientSession *session);

/* =========================================================================
 * Connection Status
 * ========================================================================= */

/* Check if connected to any node */
bool kraftClientIsConnected(kraftClientSession *session);

/* Get number of pending requests */
uint32_t kraftClientPendingCount(kraftClientSession *session);

/* Get connection latency statistics */
void kraftClientGetLatencyStats(kraftClientSession *session, uint64_t *minUs,
                                uint64_t *maxUs, uint64_t *avgUs,
                                uint64_t *p99Us);

/* =========================================================================
 * Statistics
 * ========================================================================= */

typedef struct kraftClientStats {
    uint64_t requestsSent;
    uint64_t responsesReceived;
    uint64_t timeouts;
    uint64_t retries;
    uint64_t redirects;
    uint64_t errors;
    uint64_t avgLatencyUs;
    uint64_t pendingRequests;
    bool connected;
    uint64_t knownLeaderId;
} kraftClientStats;

void kraftClientGetStats(const kraftClientSession *session,
                         kraftClientStats *stats);

/* Format stats as string (returns bytes written) */
int kraftClientFormatStats(const kraftClientStats *stats, char *buf,
                           size_t bufLen);

/* Reset statistics counters */
void kraftClientResetStats(kraftClientSession *session);

/* =========================================================================
 * Event Loop Integration
 * ========================================================================= */

/* Process pending events (for non-blocking integration)
 * Returns number of events processed, or -1 on error */
int kraftClientPoll(kraftClientSession *session, uint64_t timeoutMs);

/* Run client event loop (blocks until disconnect or error) */
void kraftClientRun(kraftClientSession *session);

/* Stop event loop (safe to call from any thread) */
void kraftClientStop(kraftClientSession *session);

/* =========================================================================
 * Multi-Raft Support
 * ========================================================================= */

/* Submit command to specific raft group */
kraftClientRequest *
kraftClientSubmitToGroup(kraftClientSession *session, uint32_t groupId,
                         const uint8_t *command, size_t commandLen,
                         kraftClientResponseFn callback, void *userData);

/* Get leader for specific group */
bool kraftClientGetGroupLeader(kraftClientSession *session, uint32_t groupId,
                               uint64_t *nodeId, char *address,
                               size_t addressLen, uint16_t *port);

/* =========================================================================
 * Request Utilities
 * ========================================================================= */

/* Get request by ID */
kraftClientRequest *kraftClientGetRequest(kraftClientSession *session,
                                          uint64_t requestId);

/* Get elapsed time for pending request (microseconds) */
uint64_t kraftClientRequestElapsed(const kraftClientRequest *request);

/* Check if request has timed out */
bool kraftClientRequestTimedOut(const kraftClientRequest *request);

#endif /* KRAFT_CLIENT_API_H */
