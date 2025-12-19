#ifndef KRAFT_SESSION_H
#define KRAFT_SESSION_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Kraft Client Session Tracking
 * ==========================================
 * Provides exactly-once semantics for client requests via:
 * - Session IDs: Unique identifier per client
 * - Sequence numbers: Monotonically increasing per session
 * - Response caching: Store responses for duplicate detection
 *
 * Design based on Raft paper Section 6.3:
 * "To guarantee exactly-once semantics, the leader must track client
 * state and use this to filter out duplicate requests."
 *
 * Protocol:
 * 1. Client registers with leader, receives sessionId
 * 2. Client includes sessionId + seqNum with each request
 * 3. Leader checks if seqNum already processed for session
 * 4. If duplicate: return cached response
 * 5. If new: process request, cache response, increment expected seqNum
 *
 * Session Expiry:
 * - Sessions expire after inactivity timeout (default: 60s)
 * - Client must re-register after expiry
 * - Expired sessions' cached responses are freed
 */

/* Forward declarations */
struct kraftState;

/* Session identifier (unique per client) */
typedef uint64_t kraftSessionId;

/* Sequence number (unique per request within session) */
typedef uint64_t kraftSeqNum;

/* Session state */
typedef enum kraftSessionStatus {
    KRAFT_SESSION_ACTIVE = 1, /* Session is active */
    KRAFT_SESSION_EXPIRED,    /* Session expired, needs re-registration */
    KRAFT_SESSION_UNKNOWN     /* Unknown session */
} kraftSessionStatus;

/* Cached response for duplicate detection */
typedef struct kraftCachedResponse {
    void *data;         /* Response data (allocated, owned by session) */
    size_t len;         /* Response length */
    kraftSeqNum seqNum; /* Sequence number this response is for */
    uint64_t timestamp; /* When response was cached */
} kraftCachedResponse;

/* Single client session */
typedef struct kraftSession {
    kraftSessionId sessionId;   /* Unique session identifier */
    kraftSeqNum nextSeqNum;     /* Next expected sequence number */
    uint64_t lastActivity;      /* Timestamp of last activity */
    kraftCachedResponse *cache; /* Array of cached responses */
    uint32_t cacheSize;         /* Number of cached responses */
    uint32_t cacheCapacity;     /* Allocated cache capacity */
    bool active;                /* Whether session is active */
} kraftSession;

/* Session manager configuration */
typedef struct kraftSessionConfig {
    uint32_t maxSessions;         /* Max concurrent sessions (default: 1000) */
    uint32_t maxCachedPerSession; /* Max cached responses per session (default:
                                     10) */
    uint64_t sessionTimeoutUs; /* Session inactivity timeout (default: 60s) */
    uint64_t
        cacheRetentionUs; /* How long to keep cached responses (default: 30s) */
} kraftSessionConfig;

/* Default configuration values */
#define KRAFT_SESSION_DEFAULT_MAX 1000
#define KRAFT_SESSION_DEFAULT_CACHE 10
#define KRAFT_SESSION_DEFAULT_TIMEOUT_US (60 * 1000000ULL)   /* 60 seconds */
#define KRAFT_SESSION_DEFAULT_RETENTION_US (30 * 1000000ULL) /* 30 seconds */

/* Session manager (owned by kraftState) */
typedef struct kraftSessionManager {
    kraftSession *sessions;       /* Hash table of sessions */
    uint32_t sessionCount;        /* Current active sessions */
    uint32_t capacity;            /* Hash table capacity */
    kraftSessionId nextSessionId; /* Next session ID to assign */
    kraftSessionConfig config;    /* Configuration */

    /* Statistics */
    uint64_t sessionsCreated;
    uint64_t sessionsExpired;
    uint64_t duplicatesDetected;
    uint64_t requestsProcessed;
} kraftSessionManager;

/* =========================================================================
 * Initialization & Lifecycle
 * ========================================================================= */

/* Get default configuration */
kraftSessionConfig kraftSessionConfigDefault(void);

/* Initialize session manager with configuration */
bool kraftSessionManagerInit(kraftSessionManager *mgr,
                             const kraftSessionConfig *config);

/* Free session manager and all sessions */
void kraftSessionManagerFree(kraftSessionManager *mgr);

/* =========================================================================
 * Session Management
 * ========================================================================= */

/* Register a new client session. Returns sessionId, or 0 on failure. */
kraftSessionId kraftSessionRegister(kraftSessionManager *mgr);

/* Unregister a session (client disconnect) */
bool kraftSessionUnregister(kraftSessionManager *mgr, kraftSessionId sessionId);

/* Get session status */
kraftSessionStatus kraftSessionGetStatus(kraftSessionManager *mgr,
                                         kraftSessionId sessionId);

/* Touch session (update last activity time) */
void kraftSessionTouch(kraftSessionManager *mgr, kraftSessionId sessionId);

/* Expire stale sessions (call periodically, e.g., on heartbeat) */
uint32_t kraftSessionExpireStale(kraftSessionManager *mgr, uint64_t nowUs);

/* =========================================================================
 * Request Processing (Exactly-Once Semantics)
 * ========================================================================= */

/* Check if request is a duplicate.
 * Returns:
 *   - true if duplicate (cached response available)
 *   - false if new request (should be processed)
 * On duplicate, sets *cachedData and *cachedLen to cached response. */
bool kraftSessionIsDuplicate(kraftSessionManager *mgr, kraftSessionId sessionId,
                             kraftSeqNum seqNum, void **cachedData,
                             size_t *cachedLen);

/* Record response for a processed request.
 * Call after successfully processing a new request.
 * Data is copied and cached for duplicate detection. */
bool kraftSessionRecordResponse(kraftSessionManager *mgr,
                                kraftSessionId sessionId, kraftSeqNum seqNum,
                                const void *data, size_t len);

/* Get next expected sequence number for session */
kraftSeqNum kraftSessionGetNextSeqNum(kraftSessionManager *mgr,
                                      kraftSessionId sessionId);

/* =========================================================================
 * Statistics
 * ========================================================================= */

typedef struct kraftSessionStats {
    uint32_t activeSessions;
    uint64_t totalCreated;
    uint64_t totalExpired;
    uint64_t duplicatesDetected;
    uint64_t requestsProcessed;
    double duplicateRate; /* duplicates / total requests */
} kraftSessionStats;

/* Get session manager statistics */
void kraftSessionGetStats(const kraftSessionManager *mgr,
                          kraftSessionStats *stats);

/* Format statistics as string */
int kraftSessionFormatStats(const kraftSessionStats *stats, char *buf,
                            size_t bufLen);

#endif /* KRAFT_SESSION_H */
