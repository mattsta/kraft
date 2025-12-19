#include "asyncCommit.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

/* Forward declaration */
struct kraftState;

/* =========================================================================
 * Internal Helpers
 * ========================================================================= */

static uint64_t asyncNowUs(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

/* Find pending write by index */
static kraftPendingWrite *findPending(kraftAsyncCommitManager *mgr,
                                      uint64_t index) {
    for (uint32_t i = 0; i < mgr->pendingCount; i++) {
        if (mgr->pending[i].index == index) {
            return &mgr->pending[i];
        }
    }
    return NULL;
}

/* Remove acknowledged pending writes */
static void compactPending(kraftAsyncCommitManager *mgr) {
    uint32_t writeIdx = 0;

    for (uint32_t readIdx = 0; readIdx < mgr->pendingCount; readIdx++) {
        if (!mgr->pending[readIdx].acknowledged) {
            if (writeIdx != readIdx) {
                mgr->pending[writeIdx] = mgr->pending[readIdx];
            }
            writeIdx++;
        }
    }

    mgr->pendingCount = writeIdx;
}

/* Calculate majority for a cluster size */
static uint32_t majority(uint32_t clusterSize) {
    return (clusterSize / 2) + 1;
}

/* =========================================================================
 * Configuration
 * ========================================================================= */

kraftAsyncCommitConfig kraftAsyncCommitConfigDefault(void) {
    return (kraftAsyncCommitConfig){
        .defaultMode = KRAFT_ASYNC_DEFAULT_MODE,
        .flexibleAcks = KRAFT_ASYNC_DEFAULT_FLEXIBLE_ACKS,
        .asyncFlushIntervalUs = KRAFT_ASYNC_DEFAULT_FLUSH_US,
        .asyncMaxPendingBytes = KRAFT_ASYNC_DEFAULT_MAX_PENDING,
        .allowModeOverride = true};
}

/* =========================================================================
 * Initialization & Lifecycle
 * ========================================================================= */

bool kraftAsyncCommitInit(kraftAsyncCommitManager *mgr,
                          const kraftAsyncCommitConfig *config) {
    memset(mgr, 0, sizeof(*mgr));

    if (config) {
        mgr->config = *config;
    } else {
        mgr->config = kraftAsyncCommitConfigDefault();
    }

    /* Pre-allocate pending writes array */
    mgr->pendingCapacity = 64;
    mgr->pending = calloc(mgr->pendingCapacity, sizeof(kraftPendingWrite));
    if (!mgr->pending) {
        return false;
    }

    return true;
}

void kraftAsyncCommitFree(kraftAsyncCommitManager *mgr) {
    if (!mgr->pending) {
        return;
    }

    free(mgr->pending);
    memset(mgr, 0, sizeof(*mgr));
}

/* =========================================================================
 * Write Submission
 * ========================================================================= */

bool kraftAsyncCommitSubmit(kraftAsyncCommitManager *mgr,
                            struct kraftState *state, uint64_t index,
                            uint64_t size) {
    return kraftAsyncCommitSubmitWithMode(mgr, state, index, size,
                                          mgr->config.defaultMode);
}

bool kraftAsyncCommitSubmitWithMode(kraftAsyncCommitManager *mgr,
                                    struct kraftState *state, uint64_t index,
                                    uint64_t size, kraftCommitMode mode) {
    (void)state; /* May be used for cluster size lookup */

    /* Check if blocked */
    if (kraftAsyncCommitIsBlocked(mgr)) {
        return false;
    }

    /* Get effective mode */
    kraftCommitMode effectiveMode = kraftAsyncCommitGetMode(mgr, mode);

    /* Update statistics */
    mgr->totalWrites++;
    switch (effectiveMode) {
    case KRAFT_COMMIT_STRONG:
        mgr->strongWrites++;
        break;
    case KRAFT_COMMIT_LEADER_ONLY:
        mgr->leaderOnlyWrites++;
        break;
    case KRAFT_COMMIT_ASYNC:
        mgr->asyncWrites++;
        break;
    case KRAFT_COMMIT_FLEXIBLE:
        mgr->flexibleWrites++;
        break;
    }

    /* For ASYNC mode, acknowledge immediately */
    if (effectiveMode == KRAFT_COMMIT_ASYNC) {
        if (index > mgr->lastAckedIndex) {
            mgr->lastAckedIndex = index;
        }
        return true;
    }

    /* For LEADER_ONLY mode, acknowledge after leader persistence
     * In this simplified implementation, we assume leader persistence
     * happens before this function is called */
    if (effectiveMode == KRAFT_COMMIT_LEADER_ONLY) {
        if (index > mgr->lastAckedIndex) {
            mgr->lastAckedIndex = index;
        }
        return true;
    }

    /* For STRONG and FLEXIBLE modes, track as pending */
    /* Grow pending array if needed */
    if (mgr->pendingCount >= mgr->pendingCapacity) {
        uint32_t newCapacity = mgr->pendingCapacity * 2;
        kraftPendingWrite *newPending =
            realloc(mgr->pending, newCapacity * sizeof(kraftPendingWrite));
        if (!newPending) {
            return false;
        }
        mgr->pending = newPending;
        mgr->pendingCapacity = newCapacity;
    }

    /* Add pending write */
    kraftPendingWrite *pw = &mgr->pending[mgr->pendingCount++];
    memset(pw, 0, sizeof(*pw));
    pw->index = index;
    pw->timestamp = asyncNowUs();
    pw->mode = effectiveMode;
    pw->acksReceived = 1; /* Leader counts as 1 */

    /* Set acks required based on mode */
    if (effectiveMode == KRAFT_COMMIT_FLEXIBLE) {
        pw->acksRequired = mgr->config.flexibleAcks;
    } else {
        /* STRONG mode requires majority - will be checked in RecordAck */
        pw->acksRequired = 0; /* Means "majority" */
    }

    mgr->pendingBytes += size;

    return true;
}

bool kraftAsyncCommitShouldAck(kraftAsyncCommitManager *mgr, uint64_t index) {
    /* Check if this index is already acknowledged */
    if (index <= mgr->lastAckedIndex) {
        return false; /* Already acked */
    }

    /* Check if this index is durable (for STRONG mode) */
    if (index <= mgr->lastDurableIndex) {
        return true;
    }

    /* Check pending writes */
    kraftPendingWrite *pw = findPending(mgr, index);
    if (pw && pw->acknowledged) {
        return false; /* Already acked */
    }

    return false;
}

/* =========================================================================
 * Replication Tracking
 * ========================================================================= */

void kraftAsyncCommitRecordAck(kraftAsyncCommitManager *mgr, uint64_t index) {
    kraftPendingWrite *pw = findPending(mgr, index);
    if (!pw || pw->acknowledged) {
        return;
    }

    pw->acksReceived++;

    /* For FLEXIBLE mode, check if we have enough acks */
    if (pw->mode == KRAFT_COMMIT_FLEXIBLE && pw->acksRequired > 0) {
        if (pw->acksReceived >= pw->acksRequired) {
            pw->acknowledged = true;
            if (index > mgr->lastAckedIndex) {
                mgr->lastAckedIndex = index;
            }

            /* Record latency */
            uint64_t latency = asyncNowUs() - pw->timestamp;
            mgr->totalAckLatencyUs += latency;
        }
    }

    /* For STRONG mode, we wait for RecordDurable instead */
}

void kraftAsyncCommitRecordDurable(kraftAsyncCommitManager *mgr,
                                   uint64_t durableIndex) {
    if (durableIndex <= mgr->lastDurableIndex) {
        return;
    }

    mgr->lastDurableIndex = durableIndex;

    /* Acknowledge all pending STRONG writes up to durableIndex */
    for (uint32_t i = 0; i < mgr->pendingCount; i++) {
        kraftPendingWrite *pw = &mgr->pending[i];
        if (pw->acknowledged) {
            continue;
        }

        if (pw->mode == KRAFT_COMMIT_STRONG && pw->index <= durableIndex) {
            pw->acknowledged = true;
            if (pw->index > mgr->lastAckedIndex) {
                mgr->lastAckedIndex = pw->index;
            }

            /* Record latency */
            uint64_t latency = asyncNowUs() - pw->timestamp;
            mgr->totalAckLatencyUs += latency;
        }
    }

    /* Compact pending array */
    compactPending(mgr);
}

uint32_t kraftAsyncCommitFlush(kraftAsyncCommitManager *mgr) {
    uint32_t flushed = 0;

    /* Acknowledge all remaining pending writes (for shutdown) */
    for (uint32_t i = 0; i < mgr->pendingCount; i++) {
        kraftPendingWrite *pw = &mgr->pending[i];
        if (!pw->acknowledged) {
            pw->acknowledged = true;
            if (pw->index > mgr->lastAckedIndex) {
                mgr->lastAckedIndex = pw->index;
            }
            flushed++;
        }
    }

    mgr->pendingCount = 0;
    mgr->pendingBytes = 0;

    return flushed;
}

/* =========================================================================
 * Query Functions
 * ========================================================================= */

kraftCommitMode kraftAsyncCommitGetMode(const kraftAsyncCommitManager *mgr,
                                        kraftCommitMode requestedMode) {
    /* If override not allowed, always use default */
    if (!mgr->config.allowModeOverride) {
        return mgr->config.defaultMode;
    }

    /* If requested mode is valid, use it */
    if (requestedMode >= KRAFT_COMMIT_STRONG &&
        requestedMode <= KRAFT_COMMIT_FLEXIBLE) {
        return requestedMode;
    }

    /* Otherwise use default */
    return mgr->config.defaultMode;
}

uint32_t kraftAsyncCommitGetAcksRequired(const kraftAsyncCommitManager *mgr,
                                         uint32_t clusterSize) {
    switch (mgr->config.defaultMode) {
    case KRAFT_COMMIT_STRONG:
        return majority(clusterSize);

    case KRAFT_COMMIT_LEADER_ONLY:
        return 1;

    case KRAFT_COMMIT_ASYNC:
        return 0;

    case KRAFT_COMMIT_FLEXIBLE:
        return mgr->config.flexibleAcks;

    default:
        return majority(clusterSize);
    }
}

bool kraftAsyncCommitIsBlocked(const kraftAsyncCommitManager *mgr) {
    return mgr->pendingBytes >= mgr->config.asyncMaxPendingBytes;
}

uint64_t kraftAsyncCommitGetLastAcked(const kraftAsyncCommitManager *mgr) {
    return mgr->lastAckedIndex;
}

uint64_t kraftAsyncCommitGetLastDurable(const kraftAsyncCommitManager *mgr) {
    return mgr->lastDurableIndex;
}

/* =========================================================================
 * Statistics
 * ========================================================================= */

void kraftAsyncCommitGetStats(const kraftAsyncCommitManager *mgr,
                              kraftAsyncCommitStats *stats) {
    memset(stats, 0, sizeof(*stats));

    stats->totalWrites = mgr->totalWrites;
    stats->strongWrites = mgr->strongWrites;
    stats->leaderOnlyWrites = mgr->leaderOnlyWrites;
    stats->asyncWrites = mgr->asyncWrites;
    stats->flexibleWrites = mgr->flexibleWrites;
    stats->pendingWrites = mgr->pendingCount;
    stats->pendingBytes = mgr->pendingBytes;
    stats->defaultMode = mgr->config.defaultMode;

    /* Calculate average ack latency */
    uint64_t ackedWrites = mgr->strongWrites + mgr->flexibleWrites;
    if (ackedWrites > 0) {
        stats->avgAckLatencyUs = mgr->totalAckLatencyUs / ackedWrites;
    }
}

int kraftAsyncCommitFormatStats(const kraftAsyncCommitStats *stats, char *buf,
                                size_t bufLen) {
    const char *modeName = "UNKNOWN";
    if (stats->defaultMode >= KRAFT_COMMIT_STRONG &&
        stats->defaultMode <= KRAFT_COMMIT_FLEXIBLE) {
        modeName = kraftCommitModeNames[stats->defaultMode];
    }

    return snprintf(buf, bufLen,
                    "Async Commit Stats:\n"
                    "  Default Mode: %s\n"
                    "  Total Writes: %llu\n"
                    "    Strong: %llu\n"
                    "    Leader-Only: %llu\n"
                    "    Async: %llu\n"
                    "    Flexible: %llu\n"
                    "  Pending Writes: %llu\n"
                    "  Pending Bytes: %llu\n"
                    "  Avg Ack Latency: %llu us\n",
                    modeName, (unsigned long long)stats->totalWrites,
                    (unsigned long long)stats->strongWrites,
                    (unsigned long long)stats->leaderOnlyWrites,
                    (unsigned long long)stats->asyncWrites,
                    (unsigned long long)stats->flexibleWrites,
                    (unsigned long long)stats->pendingWrites,
                    (unsigned long long)stats->pendingBytes,
                    (unsigned long long)stats->avgAckLatencyUs);
}
