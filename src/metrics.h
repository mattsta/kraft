#ifndef KRAFT_METRICS_H
#define KRAFT_METRICS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Kraft Metrics Module
 * ================================
 * Provides observability into Raft consensus behavior for:
 * - Performance monitoring (latency, throughput)
 * - Debugging (elections, replication lag)
 * - Operational awareness (role changes, commit progress)
 *
 * Design Principles:
 * - Zero allocation after init
 * - Lock-free counters (single writer assumed)
 * - Minimal overhead in hot path
 * - Export-agnostic (can be exported to Prometheus, StatsD, etc.)
 */

/* Histogram bucket configuration for latency tracking */
#define KRAFT_HISTOGRAM_BUCKETS 12
static const uint64_t kraftHistogramBoundaries[KRAFT_HISTOGRAM_BUCKETS] = {
    100,       /* 100 us */
    500,       /* 500 us */
    1000,      /* 1 ms */
    5000,      /* 5 ms */
    10000,     /* 10 ms */
    25000,     /* 25 ms */
    50000,     /* 50 ms */
    100000,    /* 100 ms */
    250000,    /* 250 ms */
    500000,    /* 500 ms */
    1000000,   /* 1 sec */
    UINT64_MAX /* overflow bucket */
};

/* Histogram for latency tracking */
typedef struct kraftHistogram {
    uint64_t buckets[KRAFT_HISTOGRAM_BUCKETS];
    uint64_t count;
    uint64_t sum;
    uint64_t min;
    uint64_t max;
} kraftHistogram;

/* Counter metrics (monotonically increasing) */
typedef struct kraftCounterMetrics {
    /* Message counters */
    uint64_t messagesSent;
    uint64_t messagesReceived;
    uint64_t messagesDropped;

    /* RPC-specific counters */
    uint64_t appendEntriesSent;
    uint64_t appendEntriesReceived;
    uint64_t voteRequestsSent;
    uint64_t voteRequestsReceived;
    uint64_t preVoteRequestsSent;
    uint64_t preVoteRequestsReceived;
    uint64_t heartbeatsSent;
    uint64_t heartbeatsReceived;

    /* Election counters */
    uint64_t electionsStarted;
    uint64_t electionsWon;
    uint64_t preVotesStarted;
    uint64_t preVotesWon;

    /* Commit counters */
    uint64_t entriesCommitted;
    uint64_t entriesApplied;

    /* Error counters */
    uint64_t logMismatches;
    uint64_t staleRpcsRejected;
    uint64_t termBumps;

    /* Leadership transfer */
    uint64_t transfersInitiated;
    uint64_t transfersCompleted;
    uint64_t transfersTimedOut;

    /* ReadIndex */
    uint64_t readIndexRequests;
    uint64_t readIndexCompleted;
} kraftCounterMetrics;

/* Gauge metrics (current values, can go up or down) */
typedef struct kraftGaugeMetrics {
    uint64_t currentTerm;
    uint64_t commitIndex;
    uint64_t lastApplied;
    uint64_t logLength;
    int32_t currentRole; /* FOLLOWER=2, CANDIDATE=1, LEADER=3 */
    uint64_t clusterSize;
    uint64_t healthyPeers; /* Number of peers responding to heartbeats */

    /* Replication state (leader only) */
    uint64_t minMatchIndex;  /* Lowest matchIndex among followers */
    uint64_t maxMatchIndex;  /* Highest matchIndex among followers */
    uint64_t replicationLag; /* currentIndex - minMatchIndex */
} kraftGaugeMetrics;

/* Histogram metrics (latency distributions) */
typedef struct kraftHistogramMetrics {
    kraftHistogram commitLatency;      /* Time from submit to commit */
    kraftHistogram replicationLatency; /* Time to replicate to majority */
    kraftHistogram electionDuration;   /* Time to win election */
    kraftHistogram heartbeatRtt;       /* Heartbeat round-trip time */
} kraftHistogramMetrics;

/* Complete metrics structure */
typedef struct kraftMetrics {
    kraftCounterMetrics counters;
    kraftGaugeMetrics gauges;
    kraftHistogramMetrics histograms;

    /* Timestamps for rate calculation */
    uint64_t startTime;
    uint64_t lastResetTime;

    /* Enable flag */
    bool enabled;
} kraftMetrics;

/* Forward declaration */
struct kraftState;

/* =========================================================================
 * Initialization & Lifecycle
 * ========================================================================= */

/* Initialize metrics structure */
void kraftMetricsInit(kraftMetrics *m);

/* Reset all metrics to zero (useful for testing) */
void kraftMetricsReset(kraftMetrics *m);

/* =========================================================================
 * Counter Operations (increment only)
 * ========================================================================= */

/* Increment a counter by 1 */
#define KRAFT_METRIC_INC(state, counter)                                       \
    do {                                                                       \
        if ((state)->metrics.enabled)                                          \
            (state)->metrics.counters.counter++;                               \
    } while (0)

/* Increment a counter by N */
#define KRAFT_METRIC_ADD(state, counter, n)                                    \
    do {                                                                       \
        if ((state)->metrics.enabled)                                          \
            (state)->metrics.counters.counter += (n);                          \
    } while (0)

/* =========================================================================
 * Gauge Operations (set to current value)
 * ========================================================================= */

/* Set a gauge to a specific value */
#define KRAFT_GAUGE_SET(state, gauge, value)                                   \
    do {                                                                       \
        if ((state)->metrics.enabled)                                          \
            (state)->metrics.gauges.gauge = (value);                           \
    } while (0)

/* =========================================================================
 * Histogram Operations (record latency samples)
 * ========================================================================= */

/* Record a latency sample in microseconds */
void kraftHistogramRecord(kraftHistogram *h, uint64_t valueUs);

/* Get percentile value (0-100) from histogram */
uint64_t kraftHistogramPercentile(const kraftHistogram *h, int percentile);

/* Convenience macros for recording latencies */
#define KRAFT_LATENCY_RECORD(state, histogram, valueUs)                        \
    do {                                                                       \
        if ((state)->metrics.enabled)                                          \
            kraftHistogramRecord(&(state)->metrics.histograms.histogram,       \
                                 valueUs);                                     \
    } while (0)

/* =========================================================================
 * Snapshot & Export
 * ========================================================================= */

/* Snapshot of metrics for export (copy to avoid race conditions) */
typedef struct kraftMetricsSnapshot {
    kraftCounterMetrics counters;
    kraftGaugeMetrics gauges;

    /* Histogram summaries (percentiles) */
    struct {
        uint64_t p50;
        uint64_t p95;
        uint64_t p99;
        uint64_t count;
    } commitLatency, replicationLatency, electionDuration, heartbeatRtt;

    /* Computed rates (per second) */
    double messagesPerSec;
    double commitsPerSec;
    double electionsPerSec;

    /* Timestamp */
    uint64_t snapshotTime;
    uint64_t uptimeUs;
} kraftMetricsSnapshot;

/* Take a snapshot of current metrics */
void kraftMetricsTakeSnapshot(const kraftMetrics *m,
                              kraftMetricsSnapshot *snap);

/* Format metrics as human-readable string (for logging) */
int kraftMetricsFormat(const kraftMetricsSnapshot *snap, char *buf,
                       size_t bufLen);

/* =========================================================================
 * Convenience Helpers
 * ========================================================================= */

/* Update gauge metrics from state (call periodically or after state changes) */
void kraftMetricsUpdateGauges(struct kraftState *state);

/* Record a message sent (updates multiple counters based on RPC type) */
void kraftMetricsRecordSend(struct kraftState *state, int rpcCmd);

/* Record a message received */
void kraftMetricsRecordReceive(struct kraftState *state, int rpcCmd);

#endif /* KRAFT_METRICS_H */
