#include "snapshot.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

/* Forward declarations */
struct kraftState;
struct kraftNode;

/* =========================================================================
 * Internal Helpers
 * ========================================================================= */

static uint64_t snapshotNowUs(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

static uint64_t snapshotGenerateId(void) {
    /* Simple ID generation using timestamp + random */
    return snapshotNowUs() ^ ((uint64_t)rand() << 32);
}

/* Find transfer slot for a node */
static kraftSnapshotTransfer *findTransfer(kraftSnapshotManager *mgr,
                                           struct kraftNode *node) {
    if (!mgr->transfers || !node) {
        return NULL;
    }

    for (uint32_t i = 0; i < mgr->transferCapacity; i++) {
        if (mgr->transfers[i].active && mgr->transfers[i].node == node) {
            return &mgr->transfers[i];
        }
    }

    return NULL;
}

/* Find empty transfer slot */
static kraftSnapshotTransfer *findEmptyTransferSlot(kraftSnapshotManager *mgr) {
    if (!mgr->transfers) {
        return NULL;
    }

    for (uint32_t i = 0; i < mgr->transferCapacity; i++) {
        if (!mgr->transfers[i].active) {
            return &mgr->transfers[i];
        }
    }

    return NULL;
}

/* Simple checksum calculation (placeholder - replace with SHA-256 in prod) */
static void calculateChecksum(const uint8_t *data, size_t len, uint8_t *out) {
    memset(out, 0, 32);
    /* Simple XOR-based checksum for now */
    for (size_t i = 0; i < len; i++) {
        out[i % 32] ^= data[i];
    }
}

/* =========================================================================
 * Configuration
 * ========================================================================= */

kraftSnapshotConfig kraftSnapshotConfigDefault(void) {
    return (kraftSnapshotConfig){
        .triggerPolicy = KRAFT_TRIGGER_COMBINED,
        .logSizeThresholdBytes = KRAFT_SNAPSHOT_DEFAULT_LOG_SIZE,
        .entryCountThreshold = KRAFT_SNAPSHOT_DEFAULT_ENTRY_COUNT,
        .timeIntervalUs = KRAFT_SNAPSHOT_DEFAULT_TIME_INTERVAL,
        .chunkSizeBytes = KRAFT_SNAPSHOT_DEFAULT_CHUNK_SIZE,
        .maxConcurrentTransfers = KRAFT_SNAPSHOT_DEFAULT_MAX_TRANSFERS,
        .transferTimeoutUs = KRAFT_SNAPSHOT_DEFAULT_TRANSFER_TIMEOUT,
        .maxRetries = KRAFT_SNAPSHOT_DEFAULT_MAX_RETRIES,
        .maxSnapshots = KRAFT_SNAPSHOT_DEFAULT_MAX_SNAPSHOTS,
        .retentionUs = KRAFT_SNAPSHOT_DEFAULT_RETENTION,
        .compactAfterSnapshot = true,
        .minLogEntriesAfter = KRAFT_SNAPSHOT_DEFAULT_MIN_LOG_ENTRIES,
        .preferLowLoad = true,
        .loadThresholdPercent = 70};
}

/* =========================================================================
 * Initialization & Lifecycle
 * ========================================================================= */

bool kraftSnapshotManagerInit(kraftSnapshotManager *mgr,
                              const kraftSnapshotConfig *config) {
    memset(mgr, 0, sizeof(*mgr));

    if (config) {
        mgr->config = *config;
    } else {
        mgr->config = kraftSnapshotConfigDefault();
    }

    /* Allocate transfer slots */
    mgr->transferCapacity = mgr->config.maxConcurrentTransfers;
    if (mgr->transferCapacity < 4) {
        mgr->transferCapacity = 4;
    }

    mgr->transfers =
        calloc(mgr->transferCapacity, sizeof(kraftSnapshotTransfer));
    if (!mgr->transfers) {
        return false;
    }

    /* Initial snapshot buffer (will grow as needed) */
    mgr->snapshotBufferCapacity = 1024 * 1024; /* 1 MB initial */
    mgr->snapshotBuffer = malloc(mgr->snapshotBufferCapacity);
    if (!mgr->snapshotBuffer) {
        free(mgr->transfers);
        return false;
    }

    mgr->state = KRAFT_SNAPSHOT_IDLE;
    mgr->lastSnapshotTime = snapshotNowUs();

    return true;
}

void kraftSnapshotManagerFree(kraftSnapshotManager *mgr) {
    if (mgr->transfers) {
        free(mgr->transfers);
    }
    if (mgr->snapshotBuffer) {
        free(mgr->snapshotBuffer);
    }
    memset(mgr, 0, sizeof(*mgr));
}

/* =========================================================================
 * Trigger Checking
 * ========================================================================= */

bool kraftSnapshotShouldTrigger(kraftSnapshotManager *mgr,
                                uint64_t logSizeBytes, uint64_t entryCount,
                                uint64_t nowUs) {
    /* Don't trigger if snapshot already in progress */
    if (mgr->state != KRAFT_SNAPSHOT_IDLE) {
        return false;
    }

    switch (mgr->config.triggerPolicy) {
    case KRAFT_TRIGGER_MANUAL:
        return false;

    case KRAFT_TRIGGER_LOG_SIZE:
        return logSizeBytes >= mgr->config.logSizeThresholdBytes;

    case KRAFT_TRIGGER_ENTRY_COUNT:
        return entryCount >= mgr->config.entryCountThreshold;

    case KRAFT_TRIGGER_TIME_INTERVAL:
        return (nowUs - mgr->lastSnapshotTime) >= mgr->config.timeIntervalUs;

    case KRAFT_TRIGGER_COMBINED:
        /* Trigger if any threshold is exceeded */
        if (logSizeBytes >= mgr->config.logSizeThresholdBytes) {
            return true;
        }
        if (entryCount >= mgr->config.entryCountThreshold) {
            return true;
        }
        if ((nowUs - mgr->lastSnapshotTime) >= mgr->config.timeIntervalUs) {
            return true;
        }
        return false;

    default:
        return false;
    }
}

void kraftSnapshotUpdateLogMetrics(kraftSnapshotManager *mgr,
                                   uint64_t logSizeBytes, uint64_t entryCount) {
    /* Just track for trigger checking - actual trigger check done in
     * ShouldTrigger */
    (void)mgr;
    (void)logSizeBytes;
    (void)entryCount;
}

/* =========================================================================
 * Snapshot Creation
 * ========================================================================= */

uint64_t kraftSnapshotBegin(kraftSnapshotManager *mgr, struct kraftState *state,
                            uint64_t lastIncludedIndex,
                            uint64_t lastIncludedTerm) {
    (void)state;

    if (mgr->state != KRAFT_SNAPSHOT_IDLE) {
        return 0; /* Snapshot already in progress */
    }

    /* Generate snapshot ID */
    uint64_t snapshotId = snapshotGenerateId();

    /* Reset buffer */
    mgr->snapshotBufferSize = 0;

    /* Initialize metadata */
    memset(&mgr->currentSnapshot, 0, sizeof(mgr->currentSnapshot));
    mgr->currentSnapshot.snapshotId = snapshotId;
    mgr->currentSnapshot.lastIncludedIndex = lastIncludedIndex;
    mgr->currentSnapshot.lastIncludedTerm = lastIncludedTerm;
    mgr->currentSnapshot.createdAt = snapshotNowUs();
    mgr->currentSnapshot.format = KRAFT_SNAPSHOT_FULL;

    mgr->state = KRAFT_SNAPSHOT_CREATING;

    return snapshotId;
}

bool kraftSnapshotWrite(kraftSnapshotManager *mgr, const void *data,
                        size_t len) {
    if (mgr->state != KRAFT_SNAPSHOT_CREATING) {
        return false;
    }

    /* Grow buffer if needed */
    if (mgr->snapshotBufferSize + len > mgr->snapshotBufferCapacity) {
        size_t newCapacity = mgr->snapshotBufferCapacity * 2;
        while (newCapacity < mgr->snapshotBufferSize + len) {
            newCapacity *= 2;
        }

        uint8_t *newBuffer = realloc(mgr->snapshotBuffer, newCapacity);
        if (!newBuffer) {
            return false;
        }

        mgr->snapshotBuffer = newBuffer;
        mgr->snapshotBufferCapacity = newCapacity;
    }

    /* Copy data */
    memcpy(mgr->snapshotBuffer + mgr->snapshotBufferSize, data, len);
    mgr->snapshotBufferSize += len;

    return true;
}

bool kraftSnapshotFinish(kraftSnapshotManager *mgr) {
    if (mgr->state != KRAFT_SNAPSHOT_CREATING) {
        return false;
    }

    /* Finalize metadata */
    mgr->currentSnapshot.sizeBytes = mgr->snapshotBufferSize;
    mgr->currentSnapshot.chunkCount =
        (mgr->snapshotBufferSize + mgr->config.chunkSizeBytes - 1) /
        mgr->config.chunkSizeBytes;

    /* Calculate checksum */
    calculateChecksum(mgr->snapshotBuffer, mgr->snapshotBufferSize,
                      mgr->currentSnapshot.checksum);

    /* Update statistics */
    uint64_t creationTime = snapshotNowUs() - mgr->currentSnapshot.createdAt;
    if (mgr->totalSnapshots == 0) {
        mgr->avgSnapshotTimeUs = creationTime;
    } else {
        mgr->avgSnapshotTimeUs =
            (mgr->avgSnapshotTimeUs * mgr->totalSnapshots + creationTime) /
            (mgr->totalSnapshots + 1);
    }

    mgr->totalSnapshots++;
    mgr->totalBytes += mgr->snapshotBufferSize;
    mgr->lastSnapshotTime = snapshotNowUs();
    mgr->logSizeAtLastSnapshot = mgr->snapshotBufferSize;

    /* Transition to COMPLETE, then back to IDLE so we can start another */
    mgr->state = KRAFT_SNAPSHOT_IDLE;

    return true;
}

void kraftSnapshotAbort(kraftSnapshotManager *mgr) {
    if (mgr->state == KRAFT_SNAPSHOT_CREATING ||
        mgr->state == KRAFT_SNAPSHOT_PERSISTING) {
        mgr->state = KRAFT_SNAPSHOT_IDLE;
        mgr->snapshotBufferSize = 0;
    }
}

/* =========================================================================
 * Log Compaction
 * ========================================================================= */

uint64_t kraftSnapshotCompactLog(kraftSnapshotManager *mgr,
                                 struct kraftState *state) {
    (void)state;

    /* In a real implementation, this would truncate log entries
     * up to currentSnapshot.lastIncludedIndex */
    if (mgr->state != KRAFT_SNAPSHOT_COMPLETE) {
        return 0;
    }

    /* Return the index up to which we compacted */
    return mgr->currentSnapshot.lastIncludedIndex;
}

uint64_t kraftSnapshotGetCompactableIndex(const kraftSnapshotManager *mgr) {
    if (mgr->currentSnapshot.snapshotId == 0) {
        return 0;
    }
    return mgr->currentSnapshot.lastIncludedIndex;
}

/* =========================================================================
 * Snapshot Transfer (Leader -> Follower)
 * ========================================================================= */

kraftSnapshotTransfer *kraftSnapshotTransferBegin(kraftSnapshotManager *mgr,
                                                  struct kraftNode *node) {
    if (!node || mgr->currentSnapshot.snapshotId == 0) {
        return NULL;
    }

    /* Check if already transferring to this node */
    if (findTransfer(mgr, node)) {
        return NULL;
    }

    /* Check concurrent transfer limit */
    if (mgr->transferCount >= mgr->config.maxConcurrentTransfers) {
        return NULL;
    }

    /* Find empty slot */
    kraftSnapshotTransfer *transfer = findEmptyTransferSlot(mgr);
    if (!transfer) {
        return NULL;
    }

    /* Initialize transfer */
    memset(transfer, 0, sizeof(*transfer));
    transfer->node = node;
    transfer->snapshotId = mgr->currentSnapshot.snapshotId;
    transfer->state = KRAFT_TRANSFER_SENDING;
    transfer->totalChunks = mgr->currentSnapshot.chunkCount;
    transfer->startTime = snapshotNowUs();
    transfer->lastAckTime = transfer->startTime;
    transfer->active = true;

    mgr->transferCount++;

    return transfer;
}

bool kraftSnapshotTransferNextChunk(kraftSnapshotManager *mgr,
                                    kraftSnapshotTransfer *transfer,
                                    kraftSnapshotChunk *chunk) {
    if (!transfer || !transfer->active ||
        transfer->state != KRAFT_TRANSFER_SENDING) {
        return false;
    }

    if (transfer->currentChunk >= transfer->totalChunks) {
        return false; /* All chunks sent */
    }

    /* Calculate chunk offset and size */
    uint64_t offset =
        (uint64_t)transfer->currentChunk * mgr->config.chunkSizeBytes;
    uint32_t remaining = mgr->snapshotBufferSize - offset;
    uint32_t chunkSize = remaining < mgr->config.chunkSizeBytes
                             ? remaining
                             : mgr->config.chunkSizeBytes;

    /* Populate chunk */
    chunk->snapshotId = transfer->snapshotId;
    chunk->chunkIndex = transfer->currentChunk;
    chunk->totalChunks = transfer->totalChunks;
    chunk->offset = offset;
    chunk->length = chunkSize;
    chunk->data = mgr->snapshotBuffer + offset;
    chunk->isLast = (transfer->currentChunk + 1 >= transfer->totalChunks);

    transfer->state = KRAFT_TRANSFER_WAITING_ACK;

    return true;
}

void kraftSnapshotTransferRecordAck(kraftSnapshotManager *mgr,
                                    kraftSnapshotTransfer *transfer,
                                    uint32_t chunkIndex) {
    (void)mgr;

    if (!transfer || !transfer->active) {
        return;
    }

    if (chunkIndex == transfer->currentChunk) {
        transfer->currentChunk++;
        transfer->bytesSent += mgr->config.chunkSizeBytes;
        transfer->lastAckTime = snapshotNowUs();
        transfer->retryCount = 0;

        if (transfer->currentChunk >= transfer->totalChunks) {
            transfer->state = KRAFT_TRANSFER_COMPLETE;
        } else {
            transfer->state = KRAFT_TRANSFER_SENDING;
        }
    }
}

bool kraftSnapshotTransferIsComplete(const kraftSnapshotTransfer *transfer) {
    return transfer && transfer->state == KRAFT_TRANSFER_COMPLETE;
}

void kraftSnapshotTransferAbort(kraftSnapshotManager *mgr,
                                kraftSnapshotTransfer *transfer) {
    if (!transfer || !transfer->active) {
        return;
    }

    transfer->active = false;
    transfer->state = KRAFT_TRANSFER_FAILED;
    mgr->transferCount--;
    mgr->failedTransfers++;
}

uint32_t kraftSnapshotTransferProcessTimeouts(kraftSnapshotManager *mgr,
                                              uint64_t nowUs) {
    uint32_t timedOut = 0;

    for (uint32_t i = 0; i < mgr->transferCapacity; i++) {
        kraftSnapshotTransfer *t = &mgr->transfers[i];
        if (!t->active) {
            continue;
        }

        if (t->state == KRAFT_TRANSFER_WAITING_ACK) {
            uint64_t elapsed = nowUs - t->lastAckTime;
            if (elapsed > mgr->config.transferTimeoutUs) {
                t->retryCount++;
                if (t->retryCount >= mgr->config.maxRetries) {
                    kraftSnapshotTransferAbort(mgr, t);
                    timedOut++;
                } else {
                    /* Retry - go back to sending */
                    t->state = KRAFT_TRANSFER_SENDING;
                }
            }
        }
    }

    return timedOut;
}

/* =========================================================================
 * Snapshot Installation (Follower)
 * ========================================================================= */

bool kraftSnapshotInstallBegin(kraftSnapshotManager *mgr,
                               const kraftSnapshotMeta *meta) {
    if (mgr->state == KRAFT_SNAPSHOT_INSTALLING) {
        /* Already installing - check if same snapshot */
        if (mgr->currentSnapshot.snapshotId == meta->snapshotId) {
            return true; /* Continue existing installation */
        }
        /* Different snapshot - abort current and start new */
        kraftSnapshotInstallAbort(mgr);
    }

    /* Reset buffer */
    mgr->snapshotBufferSize = 0;

    /* Copy metadata */
    mgr->currentSnapshot = *meta;
    mgr->state = KRAFT_SNAPSHOT_INSTALLING;

    return true;
}

bool kraftSnapshotInstallWriteChunk(kraftSnapshotManager *mgr,
                                    const kraftSnapshotChunk *chunk) {
    if (mgr->state != KRAFT_SNAPSHOT_INSTALLING) {
        return false;
    }

    if (chunk->snapshotId != mgr->currentSnapshot.snapshotId) {
        return false;
    }

    /* Ensure buffer is large enough */
    size_t required = chunk->offset + chunk->length;
    if (required > mgr->snapshotBufferCapacity) {
        size_t newCapacity = mgr->snapshotBufferCapacity;
        while (newCapacity < required) {
            newCapacity *= 2;
        }

        uint8_t *newBuffer = realloc(mgr->snapshotBuffer, newCapacity);
        if (!newBuffer) {
            return false;
        }

        mgr->snapshotBuffer = newBuffer;
        mgr->snapshotBufferCapacity = newCapacity;
    }

    /* Copy chunk data */
    memcpy(mgr->snapshotBuffer + chunk->offset, chunk->data, chunk->length);

    if (chunk->offset + chunk->length > mgr->snapshotBufferSize) {
        mgr->snapshotBufferSize = chunk->offset + chunk->length;
    }

    return true;
}

bool kraftSnapshotInstallFinish(kraftSnapshotManager *mgr,
                                struct kraftState *state) {
    (void)state;

    if (mgr->state != KRAFT_SNAPSHOT_INSTALLING) {
        return false;
    }

    /* Verify checksum */
    uint8_t checksum[32];
    calculateChecksum(mgr->snapshotBuffer, mgr->snapshotBufferSize, checksum);
    if (memcmp(checksum, mgr->currentSnapshot.checksum, 32) != 0) {
        mgr->state = KRAFT_SNAPSHOT_FAILED;
        return false;
    }

    /* In a real implementation, this would:
     * 1. Call the deserialize callback to restore state machine
     * 2. Update state->lastApplied and state->commitIndex
     * 3. Truncate the log
     */

    mgr->state = KRAFT_SNAPSHOT_COMPLETE;
    return true;
}

void kraftSnapshotInstallAbort(kraftSnapshotManager *mgr) {
    if (mgr->state == KRAFT_SNAPSHOT_INSTALLING) {
        mgr->state = KRAFT_SNAPSHOT_IDLE;
        mgr->snapshotBufferSize = 0;
    }
}

/* =========================================================================
 * Query Functions
 * ========================================================================= */

const kraftSnapshotMeta *
kraftSnapshotGetCurrent(const kraftSnapshotManager *mgr) {
    if (mgr->currentSnapshot.snapshotId == 0) {
        return NULL;
    }
    return &mgr->currentSnapshot;
}

kraftSnapshotState kraftSnapshotGetState(const kraftSnapshotManager *mgr) {
    return mgr->state;
}

bool kraftSnapshotInProgress(const kraftSnapshotManager *mgr) {
    return mgr->state != KRAFT_SNAPSHOT_IDLE &&
           mgr->state != KRAFT_SNAPSHOT_COMPLETE &&
           mgr->state != KRAFT_SNAPSHOT_FAILED;
}

bool kraftSnapshotNodeNeedsSnapshot(const kraftSnapshotManager *mgr,
                                    struct kraftNode *node,
                                    uint64_t firstLogIndex) {
    (void)node;

    /* Node needs snapshot if our first log index is after their match index */
    /* This means the log entries they need have been compacted */
    if (mgr->currentSnapshot.snapshotId == 0) {
        return false;
    }

    return firstLogIndex > mgr->currentSnapshot.lastIncludedIndex;
}

/* =========================================================================
 * Statistics
 * ========================================================================= */

void kraftSnapshotGetStats(const kraftSnapshotManager *mgr,
                           kraftSnapshotStats *stats) {
    memset(stats, 0, sizeof(*stats));

    stats->totalSnapshots = mgr->totalSnapshots;
    stats->totalBytes = mgr->totalBytes;
    stats->totalTransfers = mgr->totalTransfers;
    stats->failedTransfers = mgr->failedTransfers;
    stats->activeTransfers = mgr->transferCount;
    stats->avgSnapshotTimeUs = mgr->avgSnapshotTimeUs;
    stats->avgTransferTimeUs = mgr->avgTransferTimeUs;
    stats->currentSnapshotId = mgr->currentSnapshot.snapshotId;
    stats->lastIncludedIndex = mgr->currentSnapshot.lastIncludedIndex;
    stats->currentState = mgr->state;
}

int kraftSnapshotFormatStats(const kraftSnapshotStats *stats, char *buf,
                             size_t bufLen) {
    const char *stateNames[] = {"IDLE",         "CREATING",   "PERSISTING",
                                "TRANSFERRING", "INSTALLING", "COMPLETE",
                                "FAILED"};

    const char *stateName = "UNKNOWN";
    if (stats->currentState <= KRAFT_SNAPSHOT_FAILED) {
        stateName = stateNames[stats->currentState];
    }

    return snprintf(buf, bufLen,
                    "Snapshot Stats:\n"
                    "  State: %s\n"
                    "  Current Snapshot ID: %llu\n"
                    "  Last Included Index: %llu\n"
                    "  Total Snapshots: %llu\n"
                    "  Total Bytes: %llu\n"
                    "  Total Transfers: %llu\n"
                    "  Failed Transfers: %llu\n"
                    "  Active Transfers: %llu\n"
                    "  Avg Snapshot Time: %llu us\n"
                    "  Avg Transfer Time: %llu us\n",
                    stateName, (unsigned long long)stats->currentSnapshotId,
                    (unsigned long long)stats->lastIncludedIndex,
                    (unsigned long long)stats->totalSnapshots,
                    (unsigned long long)stats->totalBytes,
                    (unsigned long long)stats->totalTransfers,
                    (unsigned long long)stats->failedTransfers,
                    (unsigned long long)stats->activeTransfers,
                    (unsigned long long)stats->avgSnapshotTimeUs,
                    (unsigned long long)stats->avgTransferTimeUs);
}
