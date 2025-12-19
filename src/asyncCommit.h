#ifndef KRAFT_ASYNC_COMMIT_H
#define KRAFT_ASYNC_COMMIT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Kraft Async Commit Modes
 * =====================================
 * Configurable durability levels that trade consistency for latency/throughput.
 * Allows applications to choose the right durability guarantees for their use
 * case.
 *
 * Commit Modes:
 * 1. STRONG (default) - Standard Raft: Wait for majority commit
 *    - Guarantees: Entry survives any minority failure
 *    - Latency: 1 RTT to slowest majority member
 *    - Use when: Data must not be lost
 *
 * 2. LEADER_ONLY - Acknowledge after leader persistence
 *    - Guarantees: Entry survives leader restart (if durable)
 *    - Latency: Local disk fsync only
 *    - Use when: Speed critical, can replay from source on leader failure
 *
 * 3. ASYNC - Acknowledge immediately, replicate in background
 *    - Guarantees: None - entry may be lost on any failure
 *    - Latency: Near-zero (just memory write)
 *    - Use when: Metrics, logging, or data that can be reconstructed
 *
 * 4. FLEXIBLE - Configurable number of acks required
 *    - Guarantees: Entry survives up to (acks-1) failures
 *    - Latency: 1 RTT to fastest (acks) members
 *    - Use when: Custom durability/latency tradeoff needed
 *
 * Per-Request Modes:
 * Applications can also specify commit mode per-request for fine-grained
 * control. e.g., metadata writes use STRONG, data writes use ASYNC
 *
 * Based on:
 * - Apache Kafka's acks parameter (0, 1, all)
 * - etcd's consistency levels
 * - CockroachDB's durability settings
 */

/* Forward declarations */
struct kraftState;

/* Commit mode options */
typedef enum kraftCommitMode {
    KRAFT_COMMIT_STRONG = 1,  /* Wait for majority (default Raft) */
    KRAFT_COMMIT_LEADER_ONLY, /* Wait for leader persistence only */
    KRAFT_COMMIT_ASYNC,       /* Acknowledge immediately */
    KRAFT_COMMIT_FLEXIBLE     /* Wait for N acks (configurable) */
} kraftCommitMode;

/* Commit mode names for logging/debugging */
static const char *kraftCommitModeNames[] = {
    "_UNDEFINED_", "STRONG", "LEADER_ONLY", "ASYNC", "FLEXIBLE"};

/* Async commit configuration */
typedef struct kraftAsyncCommitConfig {
    kraftCommitMode defaultMode; /* Default mode for all writes */
    uint32_t flexibleAcks;       /* Number of acks for FLEXIBLE mode (1 to N) */
    uint64_t asyncFlushIntervalUs; /* How often to flush async writes (default:
                                      10ms) */
    uint64_t asyncMaxPendingBytes; /* Max pending async bytes before blocking */
    bool allowModeOverride;        /* Allow per-request mode override */
} kraftAsyncCommitConfig;

/* Default configuration values */
#define KRAFT_ASYNC_DEFAULT_MODE KRAFT_COMMIT_STRONG
#define KRAFT_ASYNC_DEFAULT_FLEXIBLE_ACKS 1
#define KRAFT_ASYNC_DEFAULT_FLUSH_US (10 * 1000ULL)        /* 10 ms */
#define KRAFT_ASYNC_DEFAULT_MAX_PENDING (16 * 1024 * 1024) /* 16 MB */

/* Pending async write tracking */
typedef struct kraftPendingWrite {
    uint64_t index;        /* Log index of this write */
    uint64_t timestamp;    /* When write was submitted */
    kraftCommitMode mode;  /* Commit mode for this write */
    uint32_t acksRequired; /* Acks required (for FLEXIBLE) */
    uint32_t acksReceived; /* Acks received so far */
    void *callback;        /* Completion callback (optional) */
    void *callbackData;    /* Callback user data */
    bool acknowledged;     /* True once acknowledged to client */
} kraftPendingWrite;

/* Async commit manager */
typedef struct kraftAsyncCommitManager {
    kraftAsyncCommitConfig config; /* Configuration */

    /* Pending writes tracking */
    kraftPendingWrite *pending; /* Array of pending writes */
    uint32_t pendingCount;      /* Number of pending writes */
    uint32_t pendingCapacity;   /* Allocated capacity */
    uint64_t pendingBytes;      /* Total bytes in pending writes */

    /* Index tracking */
    uint64_t lastAckedIndex;   /* Last index acknowledged to client */
    uint64_t lastDurableIndex; /* Last index known durable (majority) */

    /* Statistics */
    uint64_t totalWrites;       /* Total writes submitted */
    uint64_t strongWrites;      /* Writes with STRONG mode */
    uint64_t leaderOnlyWrites;  /* Writes with LEADER_ONLY mode */
    uint64_t asyncWrites;       /* Writes with ASYNC mode */
    uint64_t flexibleWrites;    /* Writes with FLEXIBLE mode */
    uint64_t totalAckLatencyUs; /* Sum of ack latencies for avg calculation */
} kraftAsyncCommitManager;

/* =========================================================================
 * Configuration
 * ========================================================================= */

/* Get default configuration */
kraftAsyncCommitConfig kraftAsyncCommitConfigDefault(void);

/* =========================================================================
 * Initialization & Lifecycle
 * ========================================================================= */

/* Initialize async commit manager with configuration */
bool kraftAsyncCommitInit(kraftAsyncCommitManager *mgr,
                          const kraftAsyncCommitConfig *config);

/* Free async commit manager */
void kraftAsyncCommitFree(kraftAsyncCommitManager *mgr);

/* =========================================================================
 * Write Submission
 * ========================================================================= */

/* Submit a write with default commit mode.
 * Returns:
 *   - For STRONG: Blocks until majority commit, then returns true
 *   - For LEADER_ONLY: Returns true after leader persistence
 *   - For ASYNC: Returns true immediately
 *   - For FLEXIBLE: Blocks until required acks received */
bool kraftAsyncCommitSubmit(kraftAsyncCommitManager *mgr,
                            struct kraftState *state, uint64_t index,
                            uint64_t size);

/* Submit a write with specific commit mode (if override allowed) */
bool kraftAsyncCommitSubmitWithMode(kraftAsyncCommitManager *mgr,
                                    struct kraftState *state, uint64_t index,
                                    uint64_t size, kraftCommitMode mode);

/* Check if a write should be acknowledged to client.
 * Call this after receiving replication acks. */
bool kraftAsyncCommitShouldAck(kraftAsyncCommitManager *mgr, uint64_t index);

/* =========================================================================
 * Replication Tracking
 * ========================================================================= */

/* Called when an ack is received for an index.
 * Updates pending writes and may trigger acknowledgments. */
void kraftAsyncCommitRecordAck(kraftAsyncCommitManager *mgr, uint64_t index);

/* Called when index is known to be durable (majority replicated).
 * Updates lastDurableIndex and acknowledges pending STRONG writes. */
void kraftAsyncCommitRecordDurable(kraftAsyncCommitManager *mgr,
                                   uint64_t durableIndex);

/* Flush pending async writes (call periodically or on shutdown).
 * Returns number of writes flushed. */
uint32_t kraftAsyncCommitFlush(kraftAsyncCommitManager *mgr);

/* =========================================================================
 * Query Functions
 * ========================================================================= */

/* Get effective commit mode for a write (considering overrides) */
kraftCommitMode kraftAsyncCommitGetMode(const kraftAsyncCommitManager *mgr,
                                        kraftCommitMode requestedMode);

/* Get number of acks required for current mode */
uint32_t kraftAsyncCommitGetAcksRequired(const kraftAsyncCommitManager *mgr,
                                         uint32_t clusterSize);

/* Check if manager is blocked due to too many pending async writes */
bool kraftAsyncCommitIsBlocked(const kraftAsyncCommitManager *mgr);

/* Get last acknowledged index */
uint64_t kraftAsyncCommitGetLastAcked(const kraftAsyncCommitManager *mgr);

/* Get last durable index */
uint64_t kraftAsyncCommitGetLastDurable(const kraftAsyncCommitManager *mgr);

/* =========================================================================
 * Statistics
 * ========================================================================= */

typedef struct kraftAsyncCommitStats {
    uint64_t totalWrites;
    uint64_t strongWrites;
    uint64_t leaderOnlyWrites;
    uint64_t asyncWrites;
    uint64_t flexibleWrites;
    uint64_t pendingWrites;
    uint64_t pendingBytes;
    uint64_t avgAckLatencyUs;
    kraftCommitMode defaultMode;
} kraftAsyncCommitStats;

/* Get async commit statistics */
void kraftAsyncCommitGetStats(const kraftAsyncCommitManager *mgr,
                              kraftAsyncCommitStats *stats);

/* Format statistics as string */
int kraftAsyncCommitFormatStats(const kraftAsyncCommitStats *stats, char *buf,
                                size_t bufLen);

#endif /* KRAFT_ASYNC_COMMIT_H */
