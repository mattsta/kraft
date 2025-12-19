# Configuration Reference

Complete reference for all Kraft configuration options.

## Client Configuration

### kraftClientConfig

```c
typedef struct {
    // Connection settings
    uint64_t connectTimeout;        // Connection timeout (ms)
                                    // Default: 5000

    uint64_t requestTimeout;        // Request timeout (ms)
                                    // Default: 30000

    uint32_t maxRetries;            // Max retry attempts
                                    // Default: 3

    uint64_t retryInterval;         // Time between retries (ms)
                                    // Default: 100

    // Buffer settings
    size_t sendBufferSize;          // Send buffer size (bytes)
                                    // Default: 65536

    size_t recvBufferSize;          // Receive buffer size (bytes)
                                    // Default: 65536

    // Batching
    bool batchingEnabled;           // Enable request batching
                                    // Default: true

    uint32_t maxBatchSize;          // Max requests per batch
                                    // Default: 100

    uint64_t batchTimeout;          // Max batch delay (ms)
                                    // Default: 5

    // Leader discovery
    uint64_t leaderRefreshInterval; // Auto-refresh leader (ms)
                                    // Default: 5000, 0 = disabled

    bool autoReconnect;             // Auto-reconnect on disconnect
                                    // Default: true

    // Callbacks
    kraftClientStateCallback onStateChange;
    void *userData;

} kraftClientConfig;
```

### Default Configuration

```c
kraftClientConfig kraftClientConfigDefault(void) {
    return (kraftClientConfig){
        .connectTimeout = 5000,
        .requestTimeout = 30000,
        .maxRetries = 3,
        .retryInterval = 100,
        .sendBufferSize = 65536,
        .recvBufferSize = 65536,
        .batchingEnabled = true,
        .maxBatchSize = 100,
        .batchTimeout = 5,
        .leaderRefreshInterval = 5000,
        .autoReconnect = true,
    };
}
```

---

## Server Configuration

### kraftServerConfig

```c
typedef struct {
    // Identity
    uint64_t nodeId;                // This node's ID (required)
    uint64_t clusterId;             // Cluster ID (required)

    // Network
    const char *bindAddress;        // Bind address
                                    // Default: "0.0.0.0"

    uint16_t raftPort;              // Raft protocol port
                                    // Default: 7001

    uint16_t clientPort;            // Client API port
                                    // Default: 7002

    uint16_t adminPort;             // Admin/metrics port
                                    // Default: 7003

    uint32_t maxConnections;        // Max concurrent connections
                                    // Default: 10000

    uint64_t connectionTimeout;     // Idle connection timeout (ms)
                                    // Default: 30000

    // Raft timing
    uint64_t electionTimeoutMin;    // Min election timeout (ms)
                                    // Default: 150

    uint64_t electionTimeoutMax;    // Max election timeout (ms)
                                    // Default: 300

    uint64_t heartbeatInterval;     // Heartbeat interval (ms)
                                    // Default: 50

    // Features
    bool enablePreVote;             // Enable pre-vote protocol
                                    // Default: true

    bool enableReadIndex;           // Enable ReadIndex
                                    // Default: true

    bool enableLeadershipTransfer;  // Enable leadership transfer
                                    // Default: true

    // Callbacks
    kraftApplyCallback onApply;
    kraftStateChangeCallback onStateChange;
    kraftLeaderChangeCallback onLeaderChange;
    void *userData;

} kraftServerConfig;
```

---

## Storage Configuration

### kraftStorageConfig

```c
typedef struct {
    // Directories
    const char *dataDir;            // Main data directory (required)
    const char *walDir;             // WAL directory (optional, defaults to dataDir)
    const char *snapshotDir;        // Snapshot directory (optional)

    // Durability
    kraftFsyncMode fsyncMode;       // Fsync behavior
                                    // Default: KRAFT_FSYNC_COMMIT

    uint32_t batchFsyncCount;       // Entries per batch fsync
                                    // Default: 100

    uint64_t batchFsyncInterval;    // Batch fsync interval (ms)
                                    // Default: 100

    // Log management
    uint64_t maxLogSize;            // Max log size before compaction (bytes)
                                    // Default: 1GB

    uint64_t logSegmentSize;        // Size of each log segment (bytes)
                                    // Default: 64MB

    size_t logCacheSize;            // In-memory log cache (bytes)
                                    // Default: 256MB

    // Recovery
    bool verifyOnRecovery;          // Verify checksums on recovery
                                    // Default: true

    bool repairOnRecovery;          // Attempt repair of corruption
                                    // Default: false

} kraftStorageConfig;
```

---

## Commit Configuration

### kraftCommitConfig

```c
typedef struct {
    // Mode
    kraftCommitMode mode;           // Commit mode
                                    // Default: KRAFT_COMMIT_SYNC

    // For LEADER_ONLY mode
    bool leaderFsync;               // Fsync on leader before ack
                                    // Default: true

    bool asyncReplicate;            // Continue replicating after ack
                                    // Default: true

    // For ASYNC mode
    uint32_t batchSize;             // Batch size before replicating
                                    // Default: 100

    uint64_t batchTimeout;          // Max batch delay (ms)
                                    // Default: 10

    // For ALL mode
    uint64_t timeout;               // Timeout for all acks (ms)
                                    // Default: 5000

    bool fallbackToMajority;        // Fall back if timeout
                                    // Default: true

    // Limits
    uint32_t maxPendingEntries;     // Max uncommitted entries
                                    // Default: 100000

    size_t maxPendingBytes;         // Max uncommitted bytes
                                    // Default: 256MB

} kraftCommitConfig;
```

---

## Snapshot Configuration

### kraftSnapshotConfig

```c
typedef struct {
    // Trigger conditions
    uint64_t triggerThreshold;      // Entries since last snapshot
                                    // Default: 100000

    uint64_t minInterval;           // Min time between snapshots (ms)
                                    // Default: 60000 (1 minute)

    // Size limits
    size_t maxSize;                 // Max snapshot size (bytes)
                                    // Default: 1GB

    size_t chunkSize;               // Transfer chunk size (bytes)
                                    // Default: 1MB

    // Compression
    bool compressionEnabled;        // Enable compression
                                    // Default: true

    int compressionLevel;           // Compression level (1-9)
                                    // Default: 6

    // Incremental
    bool incrementalEnabled;        // Enable incremental snapshots
                                    // Default: false

    // Retention
    uint32_t retainCount;           // Snapshots to retain
                                    // Default: 3

    uint32_t retainDays;            // Days to retain (if > retainCount)
                                    // Default: 7

    // Async
    bool asyncWrite;                // Write snapshots asynchronously
                                    // Default: true

    size_t writeBufferSize;         // Async write buffer (bytes)
                                    // Default: 4MB

    // Verification
    kraftChecksumAlgorithm checksumAlgorithm;
                                    // Default: KRAFT_CHECKSUM_CRC32

    bool verifyOnLoad;              // Verify checksum on load
                                    // Default: true

    // Callbacks
    kraftSnapshotCreateCallback onCreate;
    kraftSnapshotRestoreCallback onRestore;
    void *userData;

} kraftSnapshotConfig;
```

---

## ReadIndex Configuration

### kraftReadIndexConfig

```c
typedef struct {
    bool enabled;                   // Enable ReadIndex
                                    // Default: true

    uint64_t timeout;               // Request timeout (ms)
                                    // Default: 5000

    // Batching
    uint32_t batchSize;             // Max reads per batch
                                    // Default: 100

    uint64_t batchTimeout;          // Max batch delay (ms)
                                    // Default: 5

    // Lease mode
    bool useLease;                  // Use lease-based reads
                                    // Default: false

    uint64_t leaseTimeout;          // Lease duration (ms)
                                    // Default: 150

    uint64_t clockDrift;            // Max clock drift (ms)
                                    // Default: 10

} kraftReadIndexConfig;
```

---

## Witness Configuration

### kraftWitnessConfig

```c
typedef struct {
    uint64_t nodeId;                // Witness node ID (required)
    const char *address;            // Witness address (required)
    uint16_t port;                  // Witness port (required)

    // Sync settings
    kraftWitnessSyncMode syncMode;  // Sync mode
                                    // Default: KRAFT_WITNESS_ASYNC

    uint64_t maxLag;                // Max acceptable lag (ms)
                                    // Default: 1000

    // Read settings
    bool serveReads;                // Can serve reads
                                    // Default: true

    uint64_t maxStaleness;          // Max read staleness (ms)
                                    // Default: 5000

} kraftWitnessConfig;

typedef enum {
    KRAFT_WITNESS_ASYNC,            // Don't wait for witness ack
    KRAFT_WITNESS_SYNC,             // Wait for witness ack
    KRAFT_WITNESS_LAZY,             // Batch updates to witness
} kraftWitnessSyncMode;
```

---

## Transfer Configuration

### kraftTransferConfig

```c
typedef struct {
    uint64_t timeout;               // Transfer timeout (ms)
                                    // Default: 10000

    uint64_t retryInterval;         // Retry interval (ms)
                                    // Default: 500

    uint32_t maxRetries;            // Max retries per target
                                    // Default: 3

    bool autoFallback;              // Try other targets on failure
                                    // Default: true

    bool rejectDuringTransfer;      // Reject writes during transfer
                                    // Default: true

} kraftTransferConfig;
```

---

## Replication Configuration

### kraftReplicationConfig

```c
typedef struct {
    // Pipelining
    bool pipeliningEnabled;         // Enable pipelining
                                    // Default: true

    uint32_t pipelineWindow;        // Max outstanding batches
                                    // Default: 10

    // Batching
    uint32_t maxBatchSize;          // Max entries per batch
                                    // Default: 1000

    size_t maxBatchBytes;           // Max bytes per batch
                                    // Default: 1MB

    // Compression
    bool compressionEnabled;        // Compress replication
                                    // Default: false

    // Flow control
    uint64_t probeInterval;         // Probe slow followers (ms)
                                    // Default: 1000

    uint64_t catchupThreshold;      // Entries before probe mode
                                    // Default: 10000

} kraftReplicationConfig;
```

---

## TLS Configuration

### kraftTLSConfig

```c
typedef struct {
    bool enabled;                   // Enable TLS
                                    // Default: false

    const char *certFile;           // Server certificate path
    const char *keyFile;            // Server key path
    const char *caFile;             // CA certificate path

    bool clientAuth;                // Require client certificates
                                    // Default: false

    const char *clientCertFile;     // Client certificate (for outbound)
    const char *clientKeyFile;      // Client key (for outbound)

    kraftTLSVersion minVersion;     // Minimum TLS version
                                    // Default: TLS_1_2

    const char *cipherSuites;       // Allowed cipher suites
                                    // Default: NULL (use defaults)

} kraftTLSConfig;

typedef enum {
    TLS_1_0 = 0,
    TLS_1_1 = 1,
    TLS_1_2 = 2,
    TLS_1_3 = 3,
} kraftTLSVersion;
```

---

## Metrics Configuration

### kraftMetricsConfig

```c
typedef struct {
    bool enabled;                   // Enable metrics
                                    // Default: true

    const char *endpoint;           // Metrics endpoint path
                                    // Default: "/metrics"

    kraftMetricsFormat format;      // Output format
                                    // Default: KRAFT_METRICS_PROMETHEUS

    // Histograms
    double *buckets;                // Histogram buckets
    size_t bucketCount;             // Number of buckets

    // Labels
    const char **labelNames;        // Custom label names
    const char **labelValues;       // Custom label values
    size_t labelCount;

} kraftMetricsConfig;

typedef enum {
    KRAFT_METRICS_PROMETHEUS,
    KRAFT_METRICS_STATSD,
    KRAFT_METRICS_JSON,
} kraftMetricsFormat;
```

---

## CRDT Configuration

### kraftCRDTConfig

```c
typedef struct {
    kraftCRDTMapping *mappings;     // Key pattern to CRDT type
    size_t mappingCount;

    kraftCRDTType defaultType;      // Default for unmatched keys
                                    // Default: KRAFT_CRDT_LWW_REGISTER

    bool enableCompaction;          // Enable CRDT-aware compaction
                                    // Default: true

} kraftCRDTConfig;

typedef struct {
    const char *pattern;            // Glob pattern (e.g., "counter:*")
    kraftCRDTType type;             // CRDT type
    kraftCRDTOptions options;       // Type-specific options
} kraftCRDTMapping;

typedef enum {
    KRAFT_CRDT_GCOUNTER,
    KRAFT_CRDT_PNCOUNTER,
    KRAFT_CRDT_LWW_REGISTER,
    KRAFT_CRDT_GSET,
    KRAFT_CRDT_TWOPHASE_SET,
    KRAFT_CRDT_ORSET,
    KRAFT_CRDT_LWW_MAP,
} kraftCRDTType;
```

---

## Environment Variables

All configuration options can be set via environment variables:

```bash
# Pattern: KRAFT_<CATEGORY>_<OPTION>

# Server
KRAFT_NODE_ID=1
KRAFT_CLUSTER_ID=0xCAFEBABE
KRAFT_BIND_ADDRESS=0.0.0.0
KRAFT_RAFT_PORT=7001
KRAFT_CLIENT_PORT=7002
KRAFT_ADMIN_PORT=7003

# Timing
KRAFT_ELECTION_TIMEOUT_MIN=150
KRAFT_ELECTION_TIMEOUT_MAX=300
KRAFT_HEARTBEAT_INTERVAL=50

# Storage
KRAFT_DATA_DIR=/var/lib/kraft/data
KRAFT_WAL_DIR=/var/lib/kraft/wal
KRAFT_SNAPSHOT_DIR=/var/lib/kraft/snapshots
KRAFT_FSYNC_MODE=commit  # none, batch, commit, always

# Features
KRAFT_ENABLE_PRE_VOTE=true
KRAFT_ENABLE_READ_INDEX=true
KRAFT_ENABLE_LEADERSHIP_TRANSFER=true

# Snapshots
KRAFT_SNAPSHOT_THRESHOLD=100000
KRAFT_SNAPSHOT_COMPRESSION=true

# TLS
KRAFT_TLS_ENABLED=true
KRAFT_TLS_CERT_FILE=/etc/kraft/certs/server.crt
KRAFT_TLS_KEY_FILE=/etc/kraft/certs/server.key
KRAFT_TLS_CA_FILE=/etc/kraft/certs/ca.crt
```

---

## Next Steps

- [API Reference](api.md) - Complete API documentation
- [Message Reference](messages.md) - Message format details
- [Deployment](../operations/deployment.md) - Production configuration
