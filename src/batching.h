#ifndef KRAFT_BATCHING_H
#define KRAFT_BATCHING_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Kraft Command Batching & Pipelining
 * ================================================
 * Provides high-throughput command submission by:
 * - Batching multiple commands into single AppendEntries RPCs
 * - Pipelining: allowing multiple in-flight RPCs before waiting for ACKs
 *
 * Design:
 * - Commands are queued until batch is full or flush is called
 * - Batches are sent when: maxBatchSize reached, flushIntervalUs elapsed, or
 * explicit flush
 * - Pipelining allows up to maxInFlight concurrent replication RPCs
 *
 * Performance benefits:
 * - Reduces RPC overhead (fewer round-trips)
 * - Amortizes commit latency across multiple commands
 * - Better network utilization
 */

/* Forward declarations */
struct kraftState;

/* Configuration for batching behavior */
typedef struct kraftBatchConfig {
    uint32_t maxBatchSize;    /* Max commands per batch (default: 100) */
    uint32_t maxBatchBytes;   /* Max bytes per batch (default: 1MB) */
    uint64_t flushIntervalUs; /* Auto-flush interval in microseconds (default:
                                 1000us = 1ms) */
    uint32_t
        maxInFlight; /* Max concurrent batches awaiting commit (default: 4) */
} kraftBatchConfig;

/* Default configuration values */
#define KRAFT_BATCH_DEFAULT_SIZE 100
#define KRAFT_BATCH_DEFAULT_BYTES (1024 * 1024) /* 1 MB */
#define KRAFT_BATCH_DEFAULT_FLUSH_US 1000       /* 1 ms */
#define KRAFT_BATCH_DEFAULT_IN_FLIGHT 4

/* Single pending command in a batch */
typedef struct kraftBatchEntry {
    uint64_t cmd;        /* Storage command type */
    void *data;          /* Command data (owned by caller) */
    size_t len;          /* Data length */
    uint64_t submitTime; /* Timestamp when submitted (for latency tracking) */
} kraftBatchEntry;

/* Batch state for accumulating commands */
typedef struct kraftBatch {
    kraftBatchEntry *entries; /* Array of pending entries */
    uint32_t count;           /* Current number of entries */
    uint32_t capacity;        /* Allocated capacity */
    size_t currentBytes;      /* Current accumulated bytes */
    uint64_t oldestEntryTime; /* Timestamp of first entry in batch */

    /* Configuration */
    kraftBatchConfig config;

    /* Pipelining state */
    uint32_t inFlightCount; /* Number of batches sent but not yet committed */
    uint64_t lastFlushTime; /* Last flush timestamp */

    /* Statistics */
    uint64_t totalBatchesSent;
    uint64_t totalCommandsBatched;
    uint64_t totalBytesQueued;
} kraftBatch;

/* =========================================================================
 * Initialization & Lifecycle
 * ========================================================================= */

/* Initialize batch with default configuration */
void kraftBatchInit(kraftBatch *batch);

/* Initialize batch with custom configuration */
void kraftBatchInitWithConfig(kraftBatch *batch,
                              const kraftBatchConfig *config);

/* Free batch resources */
void kraftBatchFree(kraftBatch *batch);

/* Reset batch (clear pending entries but keep config) */
void kraftBatchReset(kraftBatch *batch);

/* =========================================================================
 * Configuration
 * ========================================================================= */

/* Get default configuration */
kraftBatchConfig kraftBatchConfigDefault(void);

/* Update batch configuration (takes effect on next batch) */
void kraftBatchSetConfig(kraftBatch *batch, const kraftBatchConfig *config);

/* =========================================================================
 * Command Submission
 * ========================================================================= */

/* Add command to batch. Returns false if batch is full and needs flush.
 * Data is NOT copied - caller must keep data valid until batch is flushed.
 * For automatic copy, use kraftBatchAppendCopy(). */
bool kraftBatchAppend(kraftBatch *batch, uint64_t cmd, void *data, size_t len);

/* Add command to batch, copying the data internally.
 * Returns false if batch is full and needs flush. */
bool kraftBatchAppendCopy(kraftBatch *batch, uint64_t cmd, const void *data,
                          size_t len);

/* Check if batch should be flushed (size limit reached or timeout) */
bool kraftBatchShouldFlush(const kraftBatch *batch, uint64_t nowUs);

/* Check if batch is empty */
bool kraftBatchIsEmpty(const kraftBatch *batch);

/* Get current batch size (number of entries) */
uint32_t kraftBatchSize(const kraftBatch *batch);

/* =========================================================================
 * Batch Flushing (Sends to Raft)
 * ========================================================================= */

/* Flush pending batch to Raft leader.
 * Returns number of entries flushed, or -1 on error.
 * If state is NULL or not leader, returns 0 without error. */
int32_t kraftBatchFlush(struct kraftState *state, kraftBatch *batch);

/* Force flush even if batch is small or pipelining limit reached.
 * Use for synchronous semantics when caller needs immediate commit. */
int32_t kraftBatchFlushForce(struct kraftState *state, kraftBatch *batch);

/* =========================================================================
 * Pipelining Control
 * ========================================================================= */

/* Check if we can send more batches (under maxInFlight limit) */
bool kraftBatchCanSendMore(const kraftBatch *batch);

/* Called when a batch commits to decrement in-flight counter */
void kraftBatchAckCommit(kraftBatch *batch);

/* Wait for all in-flight batches to commit (blocking in test harness) */
void kraftBatchDrain(struct kraftState *state, kraftBatch *batch);

/* =========================================================================
 * Convenience: Auto-batching Submit
 * ========================================================================= */

/* Submit a command with automatic batching.
 * Command is added to batch; batch is flushed automatically if needed.
 * Returns the assigned log index, or 0 on error. */
uint64_t kraftBatchSubmit(struct kraftState *state, kraftBatch *batch,
                          uint64_t cmd, const void *data, size_t len);

#endif /* KRAFT_BATCHING_H */
