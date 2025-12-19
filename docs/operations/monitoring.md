# Monitoring

This guide covers metrics, observability, and alerting for Kraft clusters.

## Key Metrics

### Raft State Metrics

```
┌─────────────────────────────────────────────────────────────────┐
│                    Critical Raft Metrics                         │
│                                                                  │
│  Metric                    │ Type    │ Description              │
│  ─────────────────────────┼─────────┼─────────────────────────  │
│  kraft_role                │ Gauge   │ 0=follower, 1=candidate, │
│                            │         │ 2=leader                  │
│  kraft_term                │ Gauge   │ Current term number      │
│  kraft_commit_index        │ Gauge   │ Highest committed index  │
│  kraft_last_applied        │ Gauge   │ Highest applied index    │
│  kraft_last_log_index      │ Gauge   │ Last log entry index     │
│  kraft_leader_id           │ Gauge   │ Current leader node ID   │
│  kraft_cluster_size        │ Gauge   │ Number of voters         │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Replication Metrics

```c
typedef struct {
    // Per-follower replication state
    uint64_t matchIndex;          // Follower's match index
    uint64_t nextIndex;           // Next index to send
    uint64_t replicationLag;      // Entries behind leader

    // Aggregate
    uint64_t minMatchIndex;       // Slowest follower
    uint64_t avgMatchIndex;       // Average progress
} ReplicationMetrics;

// Export
exportGauge("kraft_replication_match_index", m->matchIndex,
            "follower", followerId);
exportGauge("kraft_replication_lag_entries", m->replicationLag,
            "follower", followerId);
```

### Latency Metrics

```c
typedef struct {
    histogram *commitLatency;       // Time from submit to commit
    histogram *applyLatency;        // Time from commit to apply
    histogram *replicationLatency;  // Time to replicate to follower
    histogram *electionLatency;     // Time to complete election
    histogram *heartbeatRtt;        // Heartbeat round-trip time
} LatencyMetrics;

// Critical percentiles to track
// P50, P95, P99, P99.9
```

### Throughput Metrics

```c
typedef struct {
    // Counters
    uint64_t entriesCommitted;      // Total entries committed
    uint64_t bytesCommitted;        // Total bytes committed
    uint64_t entriesApplied;        // Total entries applied
    uint64_t requestsReceived;      // Client requests received
    uint64_t requestsCompleted;     // Client requests completed

    // Rates (calculated)
    double entriesPerSecond;
    double bytesPerSecond;
    double requestsPerSecond;
} ThroughputMetrics;
```

---

## Metric Export

### Prometheus Format

```c
void exportPrometheusMetrics(kraftState *state, FILE *out) {
    fprintf(out, "# HELP kraft_role Current node role\n");
    fprintf(out, "# TYPE kraft_role gauge\n");
    fprintf(out, "kraft_role{node=\"%llu\"} %d\n",
            state->self.nodeId, state->self.role);

    fprintf(out, "# HELP kraft_term Current term\n");
    fprintf(out, "# TYPE kraft_term gauge\n");
    fprintf(out, "kraft_term{node=\"%llu\"} %llu\n",
            state->self.nodeId, state->self.currentTerm);

    fprintf(out, "# HELP kraft_commit_index Commit index\n");
    fprintf(out, "# TYPE kraft_commit_index gauge\n");
    fprintf(out, "kraft_commit_index{node=\"%llu\"} %llu\n",
            state->self.nodeId, state->commitIndex);

    // Histograms
    fprintf(out, "# HELP kraft_commit_latency_seconds Commit latency\n");
    fprintf(out, "# TYPE kraft_commit_latency_seconds histogram\n");
    exportHistogramBuckets(out, "kraft_commit_latency_seconds",
                           state->metrics.commitLatency);
}
```

### HTTP Metrics Endpoint

```c
void handleMetricsRequest(httpRequest *req, httpResponse *resp) {
    // Check authorization
    if (!isAuthorized(req, "metrics:read")) {
        httpRespondError(resp, 401, "Unauthorized");
        return;
    }

    // Generate metrics
    char *metrics = generatePrometheusMetrics(globalState);

    httpRespondWithContent(resp, 200, "text/plain", metrics);
    free(metrics);
}

// Register endpoint
httpRegister(server, "/metrics", handleMetricsRequest);
```

### StatsD Format

```c
void exportStatsDMetrics(kraftState *state, int sock) {
    char buf[1024];

    // Gauges
    snprintf(buf, sizeof(buf), "kraft.role:%d|g|#node:%llu",
             state->self.role, state->self.nodeId);
    send(sock, buf, strlen(buf), 0);

    snprintf(buf, sizeof(buf), "kraft.term:%llu|g|#node:%llu",
             state->self.currentTerm, state->self.nodeId);
    send(sock, buf, strlen(buf), 0);

    // Counters
    snprintf(buf, sizeof(buf), "kraft.entries_committed:%llu|c",
             state->metrics.entriesCommitted);
    send(sock, buf, strlen(buf), 0);

    // Timing
    snprintf(buf, sizeof(buf), "kraft.commit_latency:%f|ms",
             state->metrics.lastCommitLatency);
    send(sock, buf, strlen(buf), 0);
}
```

---

## Health Checks

### Liveness Check

```c
// Is the node running?
void handleLivenessCheck(httpRequest *req, httpResponse *resp) {
    httpRespondJSON(resp, 200, "{\"status\":\"alive\"}");
}

// /health/live → 200 if process is running
```

### Readiness Check

```c
// Is the node ready to serve traffic?
void handleReadinessCheck(httpRequest *req, httpResponse *resp) {
    kraftState *state = globalState;

    // Check if we're part of cluster
    if (state->self.nodeId == 0) {
        httpRespondJSON(resp, 503, "{\"status\":\"not_initialized\"}");
        return;
    }

    // Check if we're caught up (within 100 entries of commit)
    if (state->lastApplied < state->commitIndex - 100) {
        httpRespondJSON(resp, 503,
            "{\"status\":\"catching_up\",\"lag\":%llu}",
            state->commitIndex - state->lastApplied);
        return;
    }

    // Check if we know the leader
    if (state->leader == 0 && !kraftStateIsLeader(state)) {
        httpRespondJSON(resp, 503, "{\"status\":\"no_leader\"}");
        return;
    }

    httpRespondJSON(resp, 200, "{\"status\":\"ready\"}");
}

// /health/ready → 200 if ready to serve
```

### Detailed Health

```c
typedef struct {
    bool healthy;
    bool isLeader;
    uint64_t term;
    uint64_t commitIndex;
    uint64_t lastApplied;
    uint64_t lagEntries;
    uint64_t lagMs;
    uint64_t leaderNodeId;
    size_t clusterSize;
    size_t healthyPeers;
} DetailedHealth;

void handleDetailedHealth(httpRequest *req, httpResponse *resp) {
    DetailedHealth h = getDetailedHealth(globalState);

    httpRespondJSON(resp, h.healthy ? 200 : 503,
        "{"
        "\"healthy\":%s,"
        "\"is_leader\":%s,"
        "\"term\":%llu,"
        "\"commit_index\":%llu,"
        "\"last_applied\":%llu,"
        "\"lag_entries\":%llu,"
        "\"lag_ms\":%llu,"
        "\"leader_node_id\":%llu,"
        "\"cluster_size\":%zu,"
        "\"healthy_peers\":%zu"
        "}",
        h.healthy ? "true" : "false",
        h.isLeader ? "true" : "false",
        h.term, h.commitIndex, h.lastApplied,
        h.lagEntries, h.lagMs, h.leaderNodeId,
        h.clusterSize, h.healthyPeers);
}
```

---

## Alerting

### Critical Alerts

```yaml
# Prometheus alerting rules
groups:
  - name: kraft_critical
    rules:
      # No leader for extended period
      - alert: KraftNoLeader
        expr: sum(kraft_role == 2) == 0
        for: 30s
        labels:
          severity: critical
        annotations:
          summary: "Kraft cluster has no leader"
          description: "No node is leader for 30+ seconds"

      # Quorum at risk
      - alert: KraftQuorumAtRisk
        expr: sum(up{job="kraft"}) < ceil(kraft_cluster_size / 2) + 1
        for: 1m
        labels:
          severity: critical
        annotations:
          summary: "Kraft quorum at risk"
          description: "Fewer than quorum nodes are healthy"

      # Replication stalled
      - alert: KraftReplicationStalled
        expr: rate(kraft_commit_index[5m]) == 0
          AND kraft_role == 2
        for: 2m
        labels:
          severity: critical
        annotations:
          summary: "Kraft replication stalled"
          description: "Leader not committing entries"
```

### Warning Alerts

```yaml
groups:
  - name: kraft_warning
    rules:
      # High commit latency
      - alert: KraftHighCommitLatency
        expr: histogram_quantile(0.99, kraft_commit_latency_seconds_bucket) > 0.5
        for: 5m
        labels:
          severity: warning
        annotations:
          summary: "High commit latency"
          description: "P99 commit latency > 500ms"

      # Follower lagging
      - alert: KraftFollowerLagging
        expr: kraft_replication_lag_entries > 10000
        for: 5m
        labels:
          severity: warning
        annotations:
          summary: "Follower lagging"
          description: "Follower {{ $labels.follower }} is 10k+ entries behind"

      # High election rate
      - alert: KraftFrequentElections
        expr: rate(kraft_elections_total[1h]) > 5
        labels:
          severity: warning
        annotations:
          summary: "Frequent elections"
          description: "More than 5 elections per hour"

      # Snapshot old
      - alert: KraftSnapshotOld
        expr: time() - kraft_last_snapshot_time > 86400
        labels:
          severity: warning
        annotations:
          summary: "Snapshot is old"
          description: "No snapshot in 24+ hours"
```

### Informational Alerts

```yaml
groups:
  - name: kraft_info
    rules:
      # Leadership change
      - alert: KraftLeadershipChange
        expr: changes(kraft_leader_id[5m]) > 0
        labels:
          severity: info
        annotations:
          summary: "Leadership changed"

      # Node joined/left
      - alert: KraftMembershipChange
        expr: changes(kraft_cluster_size[5m]) > 0
        labels:
          severity: info
        annotations:
          summary: "Cluster membership changed"
```

---

## Dashboards

### Overview Dashboard

```
┌─────────────────────────────────────────────────────────────────┐
│                    KRAFT CLUSTER OVERVIEW                        │
├─────────────────────────────────────────────────────────────────┤
│  Cluster Status: [HEALTHY]        Leader: Node 2                │
│  Nodes: 5/5 healthy               Term: 42                      │
│  Commit Index: 1,234,567          Applied: 1,234,560            │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  Writes/sec     Reads/sec      Commit Latency (P99)             │
│  ┌──────────┐   ┌──────────┐   ┌────────────────────┐           │
│  │   1,234  │   │   5,678  │   │ ████████░░ 45ms    │           │
│  └──────────┘   └──────────┘   └────────────────────┘           │
│                                                                  │
├─────────────────────────────────────────────────────────────────┤
│  Node Status                                                     │
│  ┌────────┬──────────┬─────────┬────────────┬──────────┐        │
│  │ Node   │ Role     │ Match   │ Lag        │ Status   │        │
│  ├────────┼──────────┼─────────┼────────────┼──────────┤        │
│  │ 1      │ Follower │ 1234560 │ 7 entries  │ Healthy  │        │
│  │ 2      │ Leader   │ 1234567 │ -          │ Healthy  │        │
│  │ 3      │ Follower │ 1234565 │ 2 entries  │ Healthy  │        │
│  │ 4      │ Follower │ 1234567 │ 0 entries  │ Healthy  │        │
│  │ 5      │ Follower │ 1234563 │ 4 entries  │ Healthy  │        │
│  └────────┴──────────┴─────────┴────────────┴──────────┘        │
│                                                                  │
├─────────────────────────────────────────────────────────────────┤
│  Throughput (24h)                   Latency Distribution        │
│  ┌────────────────────────────┐    ┌────────────────────────┐   │
│  │▁▂▃▄▅▆▇█▇▆▅▄▃▂▁▂▃▄▅▆▇█▇▆▅▄│    │  P50: 12ms            │   │
│  │                            │    │  P95: 35ms            │   │
│  │                            │    │  P99: 45ms            │   │
│  └────────────────────────────┘    └────────────────────────┘   │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Per-Node Dashboard

```
┌─────────────────────────────────────────────────────────────────┐
│                    NODE 2 DETAILS (LEADER)                       │
├─────────────────────────────────────────────────────────────────┤
│  CPU: 25%          Memory: 2.1GB/8GB      Disk: 45GB/100GB     │
│  Net In: 50MB/s    Net Out: 200MB/s       Connections: 1,234   │
├─────────────────────────────────────────────────────────────────┤
│                                                                  │
│  Replication to Followers                                        │
│  ┌────────────────────────────────────────────────────────────┐ │
│  │ Node 1: ████████████████████░░░░ 85% (lag: 7 entries)      │ │
│  │ Node 3: ██████████████████████░░ 92% (lag: 2 entries)      │ │
│  │ Node 4: ████████████████████████ 100% (lag: 0 entries)     │ │
│  │ Node 5: ███████████████████████░ 96% (lag: 4 entries)      │ │
│  └────────────────────────────────────────────────────────────┘ │
│                                                                  │
│  Log Growth                         Snapshot Status              │
│  ┌───────────────────────────┐     ┌─────────────────────────┐  │
│  │                     ╱     │     │ Last: 2h ago            │  │
│  │               ╱──╱──      │     │ Size: 512MB             │  │
│  │         ╱──╱──            │     │ Index: 1,200,000        │  │
│  │   ╱──╱──                  │     │ Next: ~30min            │  │
│  └───────────────────────────┘     └─────────────────────────┘  │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Logging

### Structured Logging

```c
typedef struct {
    uint64_t timestamp;
    const char *level;
    const char *component;
    const char *message;
    uint64_t nodeId;
    uint64_t term;
    // Additional context
    void *context;
} LogEntry;

void logStructured(const char *level, const char *component,
                   const char *format, ...) {
    // Format message
    va_list args;
    va_start(args, format);
    char message[1024];
    vsnprintf(message, sizeof(message), format, args);
    va_end(args);

    // Output as JSON
    printf("{\"ts\":%llu,\"level\":\"%s\",\"component\":\"%s\","
           "\"msg\":\"%s\",\"node\":%llu,\"term\":%llu}\n",
           now(), level, component, message,
           globalState->self.nodeId, globalState->self.currentTerm);
}

// Usage
logStructured("info", "election", "started election for term %llu", term);
logStructured("warn", "replication", "follower %llu lagging by %llu entries",
              followerId, lag);
```

### Log Levels

```c
typedef enum {
    LOG_DEBUG,   // Detailed debugging
    LOG_INFO,    // Normal operations
    LOG_WARN,    // Potential issues
    LOG_ERROR,   // Errors (recoverable)
    LOG_FATAL,   // Fatal errors (unrecoverable)
} LogLevel;

// Recommended production level: INFO
// Debugging: DEBUG
```

### Key Events to Log

```
┌─────────────────────────────────────────────────────────────────┐
│                   Important Log Events                           │
│                                                                  │
│  Election:                                                       │
│  • Election started (INFO)                                      │
│  • Vote granted/denied (DEBUG)                                  │
│  • Became leader/follower (INFO)                                │
│                                                                  │
│  Replication:                                                    │
│  • AppendEntries sent/received (DEBUG)                          │
│  • Entry committed (DEBUG)                                      │
│  • Follower caught up (INFO)                                    │
│  • Follower fell behind (WARN)                                  │
│                                                                  │
│  Membership:                                                     │
│  • Node added/removed (INFO)                                    │
│  • Joint consensus started/completed (INFO)                     │
│                                                                  │
│  Errors:                                                         │
│  • Connection failed (WARN)                                     │
│  • Write failed (ERROR)                                         │
│  • Snapshot failed (ERROR)                                      │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Tracing

### Distributed Tracing

```c
typedef struct {
    char traceId[32];
    char spanId[16];
    char parentSpanId[16];
} TraceContext;

// Propagate trace context through Raft messages
typedef struct {
    // ... normal AppendEntries fields ...
    TraceContext trace;
} TracedAppendEntries;

void handleAppendEntriesWithTrace(TracedAppendEntries *ae) {
    // Start child span
    Span *span = startSpan("raft.appendEntries",
                           ae->trace.traceId,
                           ae->trace.spanId);

    // Process
    processAppendEntries(ae);

    // End span with metadata
    spanSetTag(span, "entries", ae->entriesCount);
    spanSetTag(span, "leader", ae->leaderId);
    endSpan(span);
}
```

### Request Tracing

```c
// Trace client request through system
void traceClientRequest(ClientRequest *req) {
    Span *rootSpan = startRootSpan("kraft.clientRequest");

    // Trace to leader
    Span *routeSpan = startChildSpan("route.toLeader", rootSpan);
    routeToLeader(req);
    endSpan(routeSpan);

    // Trace replication
    Span *replicateSpan = startChildSpan("replicate", rootSpan);
    replicateToFollowers(req);
    endSpan(replicateSpan);

    // Trace commit
    Span *commitSpan = startChildSpan("commit", rootSpan);
    waitForCommit(req);
    endSpan(commitSpan);

    endSpan(rootSpan);
}
```

---

## Best Practices

### Metric Naming

```
kraft_<component>_<metric>_<unit>

Examples:
kraft_replication_lag_entries
kraft_commit_latency_seconds
kraft_elections_total
kraft_snapshot_size_bytes
```

### Label Cardinality

```c
// GOOD: Low cardinality labels
"node", "1"
"role", "leader"
"status", "healthy"

// BAD: High cardinality labels (avoid!)
"request_id", "abc123..."
"timestamp", "1234567890"
"client_ip", "192.168.1.1"
```

### Retention

```
Metrics retention recommendations:
• High-resolution (1s): 2 hours
• Medium (1m): 2 weeks
• Low (1h): 1 year
• Aggregated: forever
```

---

## Next Steps

- [Troubleshooting](troubleshooting.md) - Common issues
- [Deployment](deployment.md) - Production setup
- [Configuration Reference](../reference/configuration.md) - All options
