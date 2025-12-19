#include "batching.h"
#include "kraft.h"
#include "metrics.h"
#include "rpcLeader.h"
#include <stdlib.h>
#include <string.h>

/* =========================================================================
 * Internal Helpers
 * ========================================================================= */

static uint64_t batchNowUs(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

static bool batchGrow(kraftBatch *batch) {
    uint32_t newCapacity = batch->capacity == 0 ? 16 : batch->capacity * 2;
    kraftBatchEntry *newEntries =
        realloc(batch->entries, newCapacity * sizeof(kraftBatchEntry));
    if (!newEntries) {
        return false;
    }
    batch->entries = newEntries;
    batch->capacity = newCapacity;
    return true;
}

/* =========================================================================
 * Configuration
 * ========================================================================= */

kraftBatchConfig kraftBatchConfigDefault(void) {
    return (kraftBatchConfig){.maxBatchSize = KRAFT_BATCH_DEFAULT_SIZE,
                              .maxBatchBytes = KRAFT_BATCH_DEFAULT_BYTES,
                              .flushIntervalUs = KRAFT_BATCH_DEFAULT_FLUSH_US,
                              .maxInFlight = KRAFT_BATCH_DEFAULT_IN_FLIGHT};
}

/* =========================================================================
 * Initialization & Lifecycle
 * ========================================================================= */

void kraftBatchInit(kraftBatch *batch) {
    kraftBatchConfig config = kraftBatchConfigDefault();
    kraftBatchInitWithConfig(batch, &config);
}

void kraftBatchInitWithConfig(kraftBatch *batch,
                              const kraftBatchConfig *config) {
    memset(batch, 0, sizeof(*batch));
    batch->config = *config;
    batch->lastFlushTime = batchNowUs();
}

void kraftBatchFree(kraftBatch *batch) {
    if (batch->entries) {
        free(batch->entries);
        batch->entries = NULL;
    }
    batch->count = 0;
    batch->capacity = 0;
}

void kraftBatchReset(kraftBatch *batch) {
    batch->count = 0;
    batch->currentBytes = 0;
    batch->oldestEntryTime = 0;
}

void kraftBatchSetConfig(kraftBatch *batch, const kraftBatchConfig *config) {
    batch->config = *config;
}

/* =========================================================================
 * Command Submission
 * ========================================================================= */

bool kraftBatchAppend(kraftBatch *batch, uint64_t cmd, void *data, size_t len) {
    /* Check if we're at capacity limits */
    if (batch->count >= batch->config.maxBatchSize) {
        return false; /* Caller should flush first */
    }

    if (batch->currentBytes + len > batch->config.maxBatchBytes) {
        return false; /* Would exceed byte limit */
    }

    /* Grow array if needed */
    if (batch->count >= batch->capacity) {
        if (!batchGrow(batch)) {
            return false; /* Allocation failed */
        }
    }

    uint64_t now = batchNowUs();

    /* Record first entry time for timeout tracking */
    if (batch->count == 0) {
        batch->oldestEntryTime = now;
    }

    /* Add entry */
    kraftBatchEntry *entry = &batch->entries[batch->count];
    entry->cmd = cmd;
    entry->data = data;
    entry->len = len;
    entry->submitTime = now;

    batch->count++;
    batch->currentBytes += len;
    batch->totalBytesQueued += len;

    return true;
}

bool kraftBatchAppendCopy(kraftBatch *batch, uint64_t cmd, const void *data,
                          size_t len) {
    /* Allocate copy of data */
    void *copy = NULL;
    if (data && len > 0) {
        copy = malloc(len);
        if (!copy) {
            return false;
        }
        memcpy(copy, data, len);
    }

    if (!kraftBatchAppend(batch, cmd, copy, len)) {
        free(copy);
        return false;
    }

    return true;
}

bool kraftBatchShouldFlush(const kraftBatch *batch, uint64_t nowUs) {
    if (batch->count == 0) {
        return false;
    }

    /* Check size limit */
    if (batch->count >= batch->config.maxBatchSize) {
        return true;
    }

    /* Check byte limit */
    if (batch->currentBytes >= batch->config.maxBatchBytes) {
        return true;
    }

    /* Check timeout */
    if (batch->oldestEntryTime > 0) {
        uint64_t age = nowUs - batch->oldestEntryTime;
        if (age >= batch->config.flushIntervalUs) {
            return true;
        }
    }

    return false;
}

bool kraftBatchIsEmpty(const kraftBatch *batch) {
    return batch->count == 0;
}

uint32_t kraftBatchSize(const kraftBatch *batch) {
    return batch->count;
}

/* =========================================================================
 * Pipelining Control
 * ========================================================================= */

bool kraftBatchCanSendMore(const kraftBatch *batch) {
    return batch->inFlightCount < batch->config.maxInFlight;
}

void kraftBatchAckCommit(kraftBatch *batch) {
    if (batch->inFlightCount > 0) {
        batch->inFlightCount--;
    }
}

/* =========================================================================
 * Batch Flushing - Internal callback for kraftRpcLeaderSendRpcCustom
 * ========================================================================= */

/* Callback context for custom RPC sender */
typedef struct {
    kraftBatch *batch;
} batchFlushContext;

static bool batchGetEntry(void *customSource, size_t i, void **data,
                          size_t *len) {
    batchFlushContext *ctx = customSource;

    if (i >= ctx->batch->count) {
        return false;
    }

    *data = ctx->batch->entries[i].data;
    *len = ctx->batch->entries[i].len;
    return true;
}

static void batchFreeEntry(void *entry) {
    /* Entries are owned by caller, don't free */
    (void)entry;
}

/* =========================================================================
 * Batch Flushing
 * ========================================================================= */

int32_t kraftBatchFlush(kraftState *state, kraftBatch *batch) {
    if (!state || batch->count == 0) {
        return 0;
    }

    /* Check if we're the leader */
    if (!IS_LEADER(state->self)) {
        return 0;
    }

    /* Check pipelining limit */
    if (!kraftBatchCanSendMore(batch)) {
        return 0; /* At max in-flight, caller should wait */
    }

    return kraftBatchFlushForce(state, batch);
}

int32_t kraftBatchFlushForce(kraftState *state, kraftBatch *batch) {
    if (!state || batch->count == 0) {
        return 0;
    }

    if (!IS_LEADER(state->self)) {
        return 0;
    }

    uint32_t entriesToFlush = batch->count;

    /* Create context for callback */
    batchFlushContext ctx = {.batch = batch};

    /* Use existing custom RPC sender which already handles batching */
    kraftRpcLeaderSendRpcCustom(state, &ctx, entriesToFlush, batchGetEntry,
                                batchFreeEntry);

    /* Update statistics */
    batch->totalBatchesSent++;
    batch->totalCommandsBatched += entriesToFlush;
    batch->inFlightCount++;
    batch->lastFlushTime = batchNowUs();

    /* Reset batch for next use */
    kraftBatchReset(batch);

    return (int32_t)entriesToFlush;
}

void kraftBatchDrain(kraftState *state, kraftBatch *batch) {
    /* In a real implementation, this would wait for commit callbacks.
     * For now, we just flush any pending entries. */
    if (batch->count > 0) {
        kraftBatchFlushForce(state, batch);
    }

    /* Note: Actual waiting for commits would require callback integration
     * with the commit path, which is future work. */
}

/* =========================================================================
 * Convenience: Auto-batching Submit
 * ========================================================================= */

uint64_t kraftBatchSubmit(kraftState *state, kraftBatch *batch, uint64_t cmd,
                          const void *data, size_t len) {
    if (!state) {
        return 0;
    }

    /* Copy data since we don't know when it will be flushed */
    if (!kraftBatchAppendCopy(batch, cmd, data, len)) {
        /* Batch full, flush first */
        if (kraftBatchFlush(state, batch) < 0) {
            return 0; /* Flush failed */
        }

        /* Try append again */
        if (!kraftBatchAppendCopy(batch, cmd, data, len)) {
            return 0; /* Still failed after flush */
        }
    }

    /* Check if we should auto-flush */
    uint64_t now = batchNowUs();
    if (kraftBatchShouldFlush(batch, now)) {
        kraftBatchFlush(state, batch);
    }

    /* Return a synthetic index (actual index assigned during flush) */
    /* For precise tracking, caller should use kraftBatchFlush directly */
    return batch->totalCommandsBatched;
}
