#ifndef KRAFT_SNAPSHOT_H
#define KRAFT_SNAPSHOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Kraft Intelligent Snapshots
 * ========================================
 * Log compaction through intelligent snapshot management with automatic
 * triggering, incremental snapshots, and parallel transfer support.
 *
 * Features:
 * 1. Automatic Triggering - Snapshot based on log size, entry count, or time
 * 2. Incremental Snapshots - Only snapshot changed state since last snapshot
 * 3. Parallel Transfer - Stream snapshots to followers without blocking
 * 4. Snapshot Scheduling - Control when snapshots occur (low-load periods)
 * 5. Compaction Policy - Configure retention and compaction behavior
 *
 * Snapshot Lifecycle:
 * 1. Trigger: Automatic (size/count threshold) or manual
 * 2. Create: State machine serializes current state
 * 3. Persist: Write snapshot to durable storage
 * 4. Compact: Truncate log entries up to snapshot index
 * 5. Transfer: Stream to followers needing catch-up
 * 6. Install: Follower applies snapshot and resets state
 *
 * Based on:
 * - Raft thesis Section 7 (Log compaction)
 * - etcd snapshot implementation
 * - CockroachDB range snapshots
 */

/* Forward declarations */
struct kraftState;
struct kraftNode;

/* =========================================================================
 * Snapshot Types
 * ========================================================================= */

/* Snapshot format types */
typedef enum kraftSnapshotFormat {
    KRAFT_SNAPSHOT_FULL = 1,    /* Full state snapshot */
    KRAFT_SNAPSHOT_INCREMENTAL, /* Delta from previous snapshot */
    KRAFT_SNAPSHOT_STREAMING    /* Streamed in chunks */
} kraftSnapshotFormat;

/* Snapshot state */
typedef enum kraftSnapshotState {
    KRAFT_SNAPSHOT_IDLE = 0,     /* No snapshot in progress */
    KRAFT_SNAPSHOT_CREATING,     /* Creating snapshot */
    KRAFT_SNAPSHOT_PERSISTING,   /* Writing to storage */
    KRAFT_SNAPSHOT_TRANSFERRING, /* Sending to follower */
    KRAFT_SNAPSHOT_INSTALLING,   /* Follower installing snapshot */
    KRAFT_SNAPSHOT_COMPLETE,     /* Snapshot complete */
    KRAFT_SNAPSHOT_FAILED        /* Snapshot failed */
} kraftSnapshotState;

/* Transfer state for streaming to followers */
typedef enum kraftSnapshotTransferState {
    KRAFT_TRANSFER_IDLE = 0,
    KRAFT_TRANSFER_SENDING,
    KRAFT_TRANSFER_WAITING_ACK,
    KRAFT_TRANSFER_COMPLETE,
    KRAFT_TRANSFER_FAILED
} kraftSnapshotTransferState;

/* Snapshot metadata */
typedef struct kraftSnapshotMeta {
    uint64_t snapshotId;        /* Unique snapshot identifier */
    uint64_t lastIncludedIndex; /* Last log index in snapshot */
    uint64_t lastIncludedTerm;  /* Term of last log index */
    uint64_t createdAt;         /* Creation timestamp (us) */
    uint64_t sizeBytes;         /* Snapshot size in bytes */
    uint32_t chunkCount;        /* Number of chunks (for streaming) */
    kraftSnapshotFormat format; /* Snapshot format */
    uint8_t checksum[32];       /* SHA-256 checksum */
} kraftSnapshotMeta;

/* Snapshot chunk for streaming transfer */
typedef struct kraftSnapshotChunk {
    uint64_t snapshotId;  /* Parent snapshot ID */
    uint32_t chunkIndex;  /* Chunk sequence number */
    uint32_t totalChunks; /* Total chunks in snapshot */
    uint64_t offset;      /* Byte offset in snapshot */
    uint32_t length;      /* Chunk data length */
    uint8_t *data;        /* Chunk data (caller owns) */
    bool isLast;          /* True if this is the final chunk */
} kraftSnapshotChunk;

/* =========================================================================
 * Configuration
 * ========================================================================= */

/* Snapshot trigger policy */
typedef enum kraftSnapshotTriggerPolicy {
    KRAFT_TRIGGER_MANUAL = 0,    /* Manual only, no automatic */
    KRAFT_TRIGGER_LOG_SIZE,      /* Trigger on log size threshold */
    KRAFT_TRIGGER_ENTRY_COUNT,   /* Trigger on entry count threshold */
    KRAFT_TRIGGER_TIME_INTERVAL, /* Trigger on time interval */
    KRAFT_TRIGGER_COMBINED       /* Trigger on any threshold */
} kraftSnapshotTriggerPolicy;

/* Snapshot configuration */
typedef struct kraftSnapshotConfig {
    kraftSnapshotTriggerPolicy triggerPolicy;
    uint64_t logSizeThresholdBytes; /* Trigger when log exceeds this size */
    uint64_t entryCountThreshold;   /* Trigger when entries exceed this */
    uint64_t timeIntervalUs;        /* Trigger interval (microseconds) */

    /* Transfer configuration */
    uint32_t chunkSizeBytes;         /* Size of each streaming chunk */
    uint32_t maxConcurrentTransfers; /* Max parallel transfers */
    uint64_t transferTimeoutUs;      /* Timeout per chunk */
    uint32_t maxRetries;             /* Max retries per transfer */

    /* Retention configuration */
    uint32_t maxSnapshots;       /* Max snapshots to retain */
    uint64_t retentionUs;        /* How long to keep old snapshots */
    bool compactAfterSnapshot;   /* Compact log after snapshot */
    uint64_t minLogEntriesAfter; /* Keep at least N entries after compact */

    /* Scheduling */
    bool preferLowLoad;            /* Prefer low-load periods */
    uint32_t loadThresholdPercent; /* Consider "loaded" above this % */
} kraftSnapshotConfig;

/* Default configuration values */
#define KRAFT_SNAPSHOT_DEFAULT_LOG_SIZE (100 * 1024 * 1024) /* 100 MB */
#define KRAFT_SNAPSHOT_DEFAULT_ENTRY_COUNT 10000
#define KRAFT_SNAPSHOT_DEFAULT_TIME_INTERVAL                                   \
    (60 * 60 * 1000000ULL)                                  /* 1 hour          \
                                                             */
#define KRAFT_SNAPSHOT_DEFAULT_CHUNK_SIZE (1 * 1024 * 1024) /* 1 MB */
#define KRAFT_SNAPSHOT_DEFAULT_MAX_TRANSFERS 3
#define KRAFT_SNAPSHOT_DEFAULT_TRANSFER_TIMEOUT (30 * 1000000ULL) /* 30 sec */
#define KRAFT_SNAPSHOT_DEFAULT_MAX_RETRIES 3
#define KRAFT_SNAPSHOT_DEFAULT_MAX_SNAPSHOTS 3
#define KRAFT_SNAPSHOT_DEFAULT_RETENTION (24 * 60 * 60 * 1000000ULL) /* 24h */
#define KRAFT_SNAPSHOT_DEFAULT_MIN_LOG_ENTRIES 100

/* =========================================================================
 * Transfer Tracking (per follower)
 * ========================================================================= */

typedef struct kraftSnapshotTransfer {
    struct kraftNode *node;           /* Target node */
    uint64_t snapshotId;              /* Snapshot being transferred */
    kraftSnapshotTransferState state; /* Current state */
    uint32_t currentChunk;            /* Current chunk being sent */
    uint32_t totalChunks;             /* Total chunks to send */
    uint64_t bytesSent;               /* Total bytes sent */
    uint64_t startTime;               /* When transfer started */
    uint64_t lastAckTime;             /* When last ack received */
    uint32_t retryCount;              /* Current retry count */
    bool active;                      /* Is this slot in use */
} kraftSnapshotTransfer;

/* =========================================================================
 * Snapshot Manager
 * ========================================================================= */

typedef struct kraftSnapshotManager {
    kraftSnapshotConfig config; /* Configuration */

    /* Current snapshot state */
    kraftSnapshotState state;          /* Current operation state */
    kraftSnapshotMeta currentSnapshot; /* Metadata of current/last snapshot */
    uint8_t *snapshotBuffer;           /* Buffer for snapshot data */
    uint64_t snapshotBufferSize;       /* Size of snapshot buffer */
    uint64_t snapshotBufferCapacity;   /* Capacity of buffer */

    /* Incremental snapshot support */
    uint64_t lastFullSnapshotId;   /* ID of last full snapshot */
    uint64_t incrementalBaseIndex; /* Base index for incremental */

    /* Active transfers */
    kraftSnapshotTransfer *transfers; /* Array of active transfers */
    uint32_t transferCount;           /* Number of active transfers */
    uint32_t transferCapacity;        /* Allocated capacity */

    /* Trigger tracking */
    uint64_t lastSnapshotTime;      /* When last snapshot was taken */
    uint64_t logSizeAtLastSnapshot; /* Log size when last snapshot taken */
    uint64_t entriesAtLastSnapshot; /* Entry count at last snapshot */

    /* Statistics */
    uint64_t totalSnapshots;    /* Total snapshots created */
    uint64_t totalBytes;        /* Total bytes in all snapshots */
    uint64_t totalTransfers;    /* Total transfers completed */
    uint64_t failedTransfers;   /* Failed transfers */
    uint64_t avgSnapshotTimeUs; /* Average snapshot creation time */
    uint64_t avgTransferTimeUs; /* Average transfer time */
} kraftSnapshotManager;

/* =========================================================================
 * Configuration Functions
 * ========================================================================= */

/* Get default configuration */
kraftSnapshotConfig kraftSnapshotConfigDefault(void);

/* =========================================================================
 * Initialization & Lifecycle
 * ========================================================================= */

/* Initialize snapshot manager with configuration */
bool kraftSnapshotManagerInit(kraftSnapshotManager *mgr,
                              const kraftSnapshotConfig *config);

/* Free snapshot manager */
void kraftSnapshotManagerFree(kraftSnapshotManager *mgr);

/* =========================================================================
 * Trigger Checking
 * ========================================================================= */

/* Check if snapshot should be triggered based on policy.
 * Call periodically (e.g., on heartbeat timer). */
bool kraftSnapshotShouldTrigger(kraftSnapshotManager *mgr,
                                uint64_t logSizeBytes, uint64_t entryCount,
                                uint64_t nowUs);

/* Update log metrics (call after log changes) */
void kraftSnapshotUpdateLogMetrics(kraftSnapshotManager *mgr,
                                   uint64_t logSizeBytes, uint64_t entryCount);

/* =========================================================================
 * Snapshot Creation
 * ========================================================================= */

/* Begin creating a snapshot. Returns snapshot ID or 0 on failure.
 * The state machine callback will be invoked to serialize state. */
uint64_t kraftSnapshotBegin(kraftSnapshotManager *mgr, struct kraftState *state,
                            uint64_t lastIncludedIndex,
                            uint64_t lastIncludedTerm);

/* Add data to the current snapshot buffer.
 * Called by state machine during serialization. */
bool kraftSnapshotWrite(kraftSnapshotManager *mgr, const void *data,
                        size_t len);

/* Complete snapshot creation. Finalizes metadata and checksum. */
bool kraftSnapshotFinish(kraftSnapshotManager *mgr);

/* Abort snapshot creation */
void kraftSnapshotAbort(kraftSnapshotManager *mgr);

/* =========================================================================
 * Log Compaction
 * ========================================================================= */

/* Compact log entries up to the last snapshot.
 * Returns number of entries compacted. */
uint64_t kraftSnapshotCompactLog(kraftSnapshotManager *mgr,
                                 struct kraftState *state);

/* Get the index up to which the log can be compacted */
uint64_t kraftSnapshotGetCompactableIndex(const kraftSnapshotManager *mgr);

/* =========================================================================
 * Snapshot Transfer (Leader -> Follower)
 * ========================================================================= */

/* Begin transferring snapshot to a follower.
 * Returns transfer handle or NULL on failure. */
kraftSnapshotTransfer *kraftSnapshotTransferBegin(kraftSnapshotManager *mgr,
                                                  struct kraftNode *node);

/* Get next chunk to send. Returns false if no more chunks. */
bool kraftSnapshotTransferNextChunk(kraftSnapshotManager *mgr,
                                    kraftSnapshotTransfer *transfer,
                                    kraftSnapshotChunk *chunk);

/* Record acknowledgment of a chunk */
void kraftSnapshotTransferRecordAck(kraftSnapshotManager *mgr,
                                    kraftSnapshotTransfer *transfer,
                                    uint32_t chunkIndex);

/* Check if transfer is complete */
bool kraftSnapshotTransferIsComplete(const kraftSnapshotTransfer *transfer);

/* Abort a transfer */
void kraftSnapshotTransferAbort(kraftSnapshotManager *mgr,
                                kraftSnapshotTransfer *transfer);

/* Process timeouts for all active transfers.
 * Returns number of transfers that timed out. */
uint32_t kraftSnapshotTransferProcessTimeouts(kraftSnapshotManager *mgr,
                                              uint64_t nowUs);

/* =========================================================================
 * Snapshot Installation (Follower)
 * ========================================================================= */

/* Begin installing a snapshot (as follower).
 * Called when receiving InstallSnapshot RPC. */
bool kraftSnapshotInstallBegin(kraftSnapshotManager *mgr,
                               const kraftSnapshotMeta *meta);

/* Write a received chunk to the snapshot being installed */
bool kraftSnapshotInstallWriteChunk(kraftSnapshotManager *mgr,
                                    const kraftSnapshotChunk *chunk);

/* Finalize snapshot installation. Applies to state machine.
 * Returns true if installation was successful. */
bool kraftSnapshotInstallFinish(kraftSnapshotManager *mgr,
                                struct kraftState *state);

/* Abort snapshot installation */
void kraftSnapshotInstallAbort(kraftSnapshotManager *mgr);

/* =========================================================================
 * Query Functions
 * ========================================================================= */

/* Get current snapshot metadata (NULL if none) */
const kraftSnapshotMeta *
kraftSnapshotGetCurrent(const kraftSnapshotManager *mgr);

/* Get snapshot state */
kraftSnapshotState kraftSnapshotGetState(const kraftSnapshotManager *mgr);

/* Check if a snapshot is in progress */
bool kraftSnapshotInProgress(const kraftSnapshotManager *mgr);

/* Check if node needs a snapshot (far behind leader) */
bool kraftSnapshotNodeNeedsSnapshot(const kraftSnapshotManager *mgr,
                                    struct kraftNode *node,
                                    uint64_t firstLogIndex);

/* =========================================================================
 * Statistics
 * ========================================================================= */

typedef struct kraftSnapshotStats {
    uint64_t totalSnapshots;         /* Total snapshots created */
    uint64_t totalBytes;             /* Total bytes snapshotted */
    uint64_t totalTransfers;         /* Total transfers completed */
    uint64_t failedTransfers;        /* Failed transfers */
    uint64_t activeTransfers;        /* Currently active transfers */
    uint64_t avgSnapshotTimeUs;      /* Average creation time */
    uint64_t avgTransferTimeUs;      /* Average transfer time */
    uint64_t currentSnapshotId;      /* Current/last snapshot ID */
    uint64_t lastIncludedIndex;      /* Last included log index */
    kraftSnapshotState currentState; /* Current state */
} kraftSnapshotStats;

/* Get snapshot statistics */
void kraftSnapshotGetStats(const kraftSnapshotManager *mgr,
                           kraftSnapshotStats *stats);

/* Format statistics as string */
int kraftSnapshotFormatStats(const kraftSnapshotStats *stats, char *buf,
                             size_t bufLen);

/* =========================================================================
 * Callbacks (to be implemented by application)
 * ========================================================================= */

/* Callback type for state machine serialization.
 * Called during snapshot creation to serialize state.
 * Implementation should call kraftSnapshotWrite() with serialized data. */
typedef bool (*kraftSnapshotSerializeCallback)(kraftSnapshotManager *mgr,
                                               void *appState);

/* Callback type for state machine deserialization.
 * Called during snapshot installation to restore state.
 * Implementation receives the snapshot data buffer. */
typedef bool (*kraftSnapshotDeserializeCallback)(const uint8_t *data,
                                                 size_t len, void *appState);

#endif /* KRAFT_SNAPSHOT_H */
