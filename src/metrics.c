#include "metrics.h"
#include "kraft.h"
#include "rpc.h"
#include <stdio.h>
#include <string.h>
#include <sys/time.h>

/* Get current time in microseconds */
static uint64_t nowUs(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

/* =========================================================================
 * Initialization & Lifecycle
 * ========================================================================= */

void kraftMetricsInit(kraftMetrics *m) {
    memset(m, 0, sizeof(*m));
    m->startTime = nowUs();
    m->lastResetTime = m->startTime;
    m->enabled = true;

    /* Initialize histogram min values to max */
    m->histograms.commitLatency.min = UINT64_MAX;
    m->histograms.replicationLatency.min = UINT64_MAX;
    m->histograms.electionDuration.min = UINT64_MAX;
    m->histograms.heartbeatRtt.min = UINT64_MAX;
}

void kraftMetricsReset(kraftMetrics *m) {
    bool wasEnabled = m->enabled;
    memset(m, 0, sizeof(*m));
    m->startTime = nowUs();
    m->lastResetTime = m->startTime;
    m->enabled = wasEnabled;

    m->histograms.commitLatency.min = UINT64_MAX;
    m->histograms.replicationLatency.min = UINT64_MAX;
    m->histograms.electionDuration.min = UINT64_MAX;
    m->histograms.heartbeatRtt.min = UINT64_MAX;
}

/* =========================================================================
 * Histogram Operations
 * ========================================================================= */

void kraftHistogramRecord(kraftHistogram *h, uint64_t valueUs) {
    /* Find the appropriate bucket */
    for (int i = 0; i < KRAFT_HISTOGRAM_BUCKETS; i++) {
        if (valueUs <= kraftHistogramBoundaries[i]) {
            h->buckets[i]++;
            break;
        }
    }

    h->count++;
    h->sum += valueUs;

    if (valueUs < h->min) {
        h->min = valueUs;
    }
    if (valueUs > h->max) {
        h->max = valueUs;
    }
}

uint64_t kraftHistogramPercentile(const kraftHistogram *h, int percentile) {
    if (h->count == 0) {
        return 0;
    }

    uint64_t targetCount = (h->count * percentile) / 100;
    uint64_t cumulative = 0;

    for (int i = 0; i < KRAFT_HISTOGRAM_BUCKETS; i++) {
        cumulative += h->buckets[i];
        if (cumulative >= targetCount) {
            return kraftHistogramBoundaries[i];
        }
    }

    return h->max;
}

/* =========================================================================
 * Snapshot & Export
 * ========================================================================= */

void kraftMetricsTakeSnapshot(const kraftMetrics *m,
                              kraftMetricsSnapshot *snap) {
    uint64_t now = nowUs();

    /* Copy counters and gauges */
    memcpy(&snap->counters, &m->counters, sizeof(snap->counters));
    memcpy(&snap->gauges, &m->gauges, sizeof(snap->gauges));

    /* Compute histogram summaries */
    snap->commitLatency.p50 =
        kraftHistogramPercentile(&m->histograms.commitLatency, 50);
    snap->commitLatency.p95 =
        kraftHistogramPercentile(&m->histograms.commitLatency, 95);
    snap->commitLatency.p99 =
        kraftHistogramPercentile(&m->histograms.commitLatency, 99);
    snap->commitLatency.count = m->histograms.commitLatency.count;

    snap->replicationLatency.p50 =
        kraftHistogramPercentile(&m->histograms.replicationLatency, 50);
    snap->replicationLatency.p95 =
        kraftHistogramPercentile(&m->histograms.replicationLatency, 95);
    snap->replicationLatency.p99 =
        kraftHistogramPercentile(&m->histograms.replicationLatency, 99);
    snap->replicationLatency.count = m->histograms.replicationLatency.count;

    snap->electionDuration.p50 =
        kraftHistogramPercentile(&m->histograms.electionDuration, 50);
    snap->electionDuration.p95 =
        kraftHistogramPercentile(&m->histograms.electionDuration, 95);
    snap->electionDuration.p99 =
        kraftHistogramPercentile(&m->histograms.electionDuration, 99);
    snap->electionDuration.count = m->histograms.electionDuration.count;

    snap->heartbeatRtt.p50 =
        kraftHistogramPercentile(&m->histograms.heartbeatRtt, 50);
    snap->heartbeatRtt.p95 =
        kraftHistogramPercentile(&m->histograms.heartbeatRtt, 95);
    snap->heartbeatRtt.p99 =
        kraftHistogramPercentile(&m->histograms.heartbeatRtt, 99);
    snap->heartbeatRtt.count = m->histograms.heartbeatRtt.count;

    /* Compute rates */
    snap->snapshotTime = now;
    snap->uptimeUs = now - m->startTime;
    double uptimeSec = (double)snap->uptimeUs / 1000000.0;

    if (uptimeSec > 0) {
        snap->messagesPerSec =
            (double)(m->counters.messagesSent + m->counters.messagesReceived) /
            uptimeSec;
        snap->commitsPerSec = (double)m->counters.entriesCommitted / uptimeSec;
        snap->electionsPerSec =
            (double)m->counters.electionsStarted / uptimeSec;
    } else {
        snap->messagesPerSec = 0;
        snap->commitsPerSec = 0;
        snap->electionsPerSec = 0;
    }
}

int kraftMetricsFormat(const kraftMetricsSnapshot *snap, char *buf,
                       size_t bufLen) {
    const char *roleStr;
    switch (snap->gauges.currentRole) {
    case KRAFT_LEADER:
        roleStr = "LEADER";
        break;
    case KRAFT_CANDIDATE:
        roleStr = "CANDIDATE";
        break;
    case KRAFT_FOLLOWER:
        roleStr = "FOLLOWER";
        break;
    case KRAFT_PRE_CANDIDATE:
        roleStr = "PRE_CANDIDATE";
        break;
    default:
        roleStr = "UNKNOWN";
        break;
    }

    return snprintf(
        buf, bufLen,
        "Kraft Metrics:\n"
        "  Role: %s | Term: %llu | Commit: %llu | Applied: %llu | Log: %llu\n"
        "  Messages: sent=%llu recv=%llu (%.1f/s)\n"
        "  Elections: started=%llu won=%llu (%.3f/s)\n"
        "  Commits: %llu (%.1f/s)\n"
        "  Latency [commit]: p50=%lluus p95=%lluus p99=%lluus (n=%llu)\n"
        "  Latency [repl]:   p50=%lluus p95=%lluus p99=%lluus (n=%llu)\n"
        "  Cluster: size=%llu healthy=%llu lag=%llu\n"
        "  Uptime: %.1fs\n",
        roleStr, (unsigned long long)snap->gauges.currentTerm,
        (unsigned long long)snap->gauges.commitIndex,
        (unsigned long long)snap->gauges.lastApplied,
        (unsigned long long)snap->gauges.logLength,
        (unsigned long long)snap->counters.messagesSent,
        (unsigned long long)snap->counters.messagesReceived,
        snap->messagesPerSec,
        (unsigned long long)snap->counters.electionsStarted,
        (unsigned long long)snap->counters.electionsWon, snap->electionsPerSec,
        (unsigned long long)snap->counters.entriesCommitted,
        snap->commitsPerSec, (unsigned long long)snap->commitLatency.p50,
        (unsigned long long)snap->commitLatency.p95,
        (unsigned long long)snap->commitLatency.p99,
        (unsigned long long)snap->commitLatency.count,
        (unsigned long long)snap->replicationLatency.p50,
        (unsigned long long)snap->replicationLatency.p95,
        (unsigned long long)snap->replicationLatency.p99,
        (unsigned long long)snap->replicationLatency.count,
        (unsigned long long)snap->gauges.clusterSize,
        (unsigned long long)snap->gauges.healthyPeers,
        (unsigned long long)snap->gauges.replicationLag,
        (double)snap->uptimeUs / 1000000.0);
}

/* =========================================================================
 * Convenience Helpers
 * ========================================================================= */

void kraftMetricsUpdateGauges(kraftState *state) {
    if (!state->metrics.enabled) {
        return;
    }

    kraftMetrics *m = &state->metrics;

    /* Update term and indices */
    m->gauges.currentTerm = state->self->consensus.term;
    m->gauges.commitIndex = state->commitIndex;
    m->gauges.lastApplied = state->lastApplied;
    m->gauges.currentRole = state->self->consensus.role;

    /* Get log length */
    kraftRpcUniqueIndex maxIdx = 0;
    kvidxMaxKey(KRAFT_ENTRIES(state), &maxIdx);
    m->gauges.logLength = maxIdx;

    /* Cluster size */
    m->gauges.clusterSize = vecPtrCount(NODES_OLD_NODES(state));

    /* Leader-specific metrics */
    if (IS_LEADER(state->self)) {
        uint64_t minMatch = UINT64_MAX;
        uint64_t maxMatch = 0;
        uint64_t healthyCount = 0;
        uint32_t nodeCount = vecPtrCount(NODES_OLD_NODES(state));

        for (uint32_t i = 0; i < nodeCount; i++) {
            kraftNode *node;
            vecPtrGet(NODES_OLD_NODES(state), i, (void **)&node);

            if (node == state->self) {
                continue;
            }

            uint64_t match = node->consensus.leaderData.matchIndex;
            if (match > 0) {
                healthyCount++;
                if (match < minMatch) {
                    minMatch = match;
                }
                if (match > maxMatch) {
                    maxMatch = match;
                }
            }
        }

        m->gauges.healthyPeers = healthyCount;
        m->gauges.minMatchIndex = (minMatch == UINT64_MAX) ? 0 : minMatch;
        m->gauges.maxMatchIndex = maxMatch;
        m->gauges.replicationLag =
            state->self->consensus.currentIndex - m->gauges.minMatchIndex;
    } else {
        m->gauges.healthyPeers = state->leader ? 1 : 0;
        m->gauges.minMatchIndex = 0;
        m->gauges.maxMatchIndex = 0;
        m->gauges.replicationLag = 0;
    }
}

void kraftMetricsRecordSend(kraftState *state, int rpcCmd) {
    if (!state->metrics.enabled) {
        return;
    }

    KRAFT_METRIC_INC(state, messagesSent);

    switch (rpcCmd) {
    case KRAFT_RPC_APPEND_ENTRIES:
        KRAFT_METRIC_INC(state, appendEntriesSent);
        /* Heartbeats are empty AppendEntries */
        KRAFT_METRIC_INC(state, heartbeatsSent);
        break;
    case KRAFT_RPC_REQUEST_VOTE:
        KRAFT_METRIC_INC(state, voteRequestsSent);
        break;
    case KRAFT_RPC_REQUEST_PREVOTE:
        KRAFT_METRIC_INC(state, preVoteRequestsSent);
        break;
    default:
        break;
    }
}

void kraftMetricsRecordReceive(kraftState *state, int rpcCmd) {
    if (!state->metrics.enabled) {
        return;
    }

    KRAFT_METRIC_INC(state, messagesReceived);

    switch (rpcCmd) {
    case KRAFT_RPC_APPEND_ENTRIES:
        KRAFT_METRIC_INC(state, appendEntriesReceived);
        KRAFT_METRIC_INC(state, heartbeatsReceived);
        break;
    case KRAFT_RPC_REQUEST_VOTE:
        KRAFT_METRIC_INC(state, voteRequestsReceived);
        break;
    case KRAFT_RPC_REQUEST_PREVOTE:
        KRAFT_METRIC_INC(state, preVoteRequestsReceived);
        break;
    default:
        break;
    }
}
