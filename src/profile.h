#ifndef KRAFT_PROFILE_H
#define KRAFT_PROFILE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Kraft Configuration Profiles
 * ==========================================
 * Pre-built configurations for common deployment scenarios, providing
 * a unified interface to configure all kraft subsystems.
 *
 * Quick Start:
 *   kraftConfig config = kraftConfigForProfile(KRAFT_PROFILE_PRODUCTION);
 *   kraftStateApplyConfig(state, &config);
 *
 * Customization:
 *   kraftConfig config = kraftConfigForProfile(KRAFT_PROFILE_HIGH_THROUGHPUT);
 *   config.batch.maxBatchSize = 2000;  // Override specific value
 *   kraftStateApplyConfig(state, &config);
 *
 * Profile Selection Guide:
 *   - DEVELOPMENT: Fast timeouts, verbose logging, small buffers
 *   - PRODUCTION: Conservative, safety-first (DEFAULT)
 *   - HIGH_THROUGHPUT: Large batches, aggressive pipelining
 *   - LOW_LATENCY: Small batches, minimal buffering
 *   - LARGE_CLUSTER: Tuned for 10+ nodes
 *   - MEMORY_CONSTRAINED: Minimal buffer sizes
 */

/* Forward declarations */
struct kraftState;

/* =========================================================================
 * Profile Types
 * ========================================================================= */

/* Configuration profile types for common deployment scenarios */
typedef enum kraftProfile {
    KRAFT_PROFILE_CUSTOM = 0,      /* User-defined configuration */
    KRAFT_PROFILE_DEVELOPMENT,     /* Fast timeouts, verbose, small buffers */
    KRAFT_PROFILE_PRODUCTION,      /* Conservative, safety-first (DEFAULT) */
    KRAFT_PROFILE_HIGH_THROUGHPUT, /* Large batches, aggressive pipelining */
    KRAFT_PROFILE_LOW_LATENCY,     /* Small batches, minimal buffering */
    KRAFT_PROFILE_LARGE_CLUSTER,   /* Tuned for 10+ nodes */
    KRAFT_PROFILE_MEMORY_CONSTRAINED, /* Minimal buffer sizes */
    KRAFT_PROFILE_COUNT               /* Sentinel for iteration */
} kraftProfile;

/* Configuration validation result */
typedef enum kraftConfigValidation {
    KRAFT_CONFIG_VALID = 0,    /* Configuration is valid */
    KRAFT_CONFIG_WARN_TIMING,  /* Timing may cause issues (warning) */
    KRAFT_CONFIG_WARN_BUFFERS, /* Buffer sizes may cause OOM (warning) */
    KRAFT_CONFIG_ERROR_INVALID_TIMING, /* heartbeat >= election timeout (error)
                                        */
    KRAFT_CONFIG_ERROR_INVALID_COUNTS  /* Invalid counts - zero or overflow */
} kraftConfigValidation;

/* Log verbosity level */
typedef enum kraftLogLevel {
    KRAFT_LOG_ERROR = 0,
    KRAFT_LOG_WARN,
    KRAFT_LOG_INFO,
    KRAFT_LOG_DEBUG,
    KRAFT_LOG_TRACE
} kraftLogLevel;

/* =========================================================================
 * Configuration Structures
 * ========================================================================= */

/* Core Raft timing configuration */
typedef struct kraftTimingConfig {
    uint32_t electionTimeoutMinMs; /* Min election timeout (ms) */
    uint32_t electionTimeoutMaxMs; /* Max election timeout (ms) */
    uint32_t heartbeatIntervalMs;  /* Heartbeat interval (ms) */
    uint32_t rpcTimeoutMs;         /* RPC timeout (ms) */
} kraftTimingConfig;

/* Feature enable/disable flags */
typedef struct kraftFeatureConfig {
    bool enablePreVote;            /* Pre-vote protocol (Section 9.6) */
    bool enableLeadershipTransfer; /* Leadership transfer (Section 3.10) */
    bool enableReadIndex;          /* Linearizable reads (Section 6.4) */
    bool enableMetrics;            /* Metrics collection */
    bool enableBatching;           /* Command batching */
    bool enableSessions;           /* Client session deduplication */
    bool enableWitness;            /* Witness nodes */
    bool enableAsyncCommit;        /* Async commit modes */
    bool enableSnapshots;          /* Automatic snapshots */
} kraftFeatureConfig;

/* Batch configuration (mirrors kraftBatchConfig) */
typedef struct kraftProfileBatchConfig {
    uint32_t maxBatchSize;    /* Max commands per batch */
    uint32_t maxBatchBytes;   /* Max bytes per batch */
    uint64_t flushIntervalUs; /* Auto-flush interval (microseconds) */
    uint32_t maxInFlight;     /* Max concurrent batches */
} kraftProfileBatchConfig;

/* Snapshot configuration (subset of kraftSnapshotConfig) */
typedef struct kraftProfileSnapshotConfig {
    uint64_t logSizeThresholdBytes;  /* Trigger on log size */
    uint64_t entryCountThreshold;    /* Trigger on entry count */
    uint32_t chunkSizeBytes;         /* Streaming chunk size */
    uint32_t maxConcurrentTransfers; /* Parallel transfer limit */
    uint32_t maxSnapshots;           /* Retention limit */
    bool compactAfterSnapshot;       /* Log compaction policy */
} kraftProfileSnapshotConfig;

/* Session configuration (subset of kraftSessionConfig) */
typedef struct kraftProfileSessionConfig {
    uint32_t maxSessions;         /* Max concurrent sessions */
    uint32_t maxCachedPerSession; /* Cached responses per session */
    uint64_t sessionTimeoutUs;    /* Session timeout (microseconds) */
} kraftProfileSessionConfig;

/* Async commit configuration (subset of kraftAsyncCommitConfig) */
typedef struct kraftProfileAsyncCommitConfig {
    uint32_t defaultMode;          /* Default commit mode (STRONG=0) */
    uint32_t flexibleAcks;         /* Acks required for FLEXIBLE mode */
    uint64_t asyncMaxPendingBytes; /* Max pending bytes for async */
} kraftProfileAsyncCommitConfig;

/* Buffer pool configuration */
typedef struct kraftProfileBufferPoolConfig {
    uint32_t smallPoolCount;  /* Small buffer count (256B each) */
    uint32_t mediumPoolCount; /* Medium buffer count (4KB each) */
    uint32_t largePoolCount;  /* Large buffer count (64KB each) */
} kraftProfileBufferPoolConfig;

/* Unified configuration structure */
typedef struct kraftConfig {
    kraftProfile profile; /* Active profile (for documentation) */

    /* Core timing */
    kraftTimingConfig timing;

    /* Feature flags */
    kraftFeatureConfig features;

    /* Subsystem configurations */
    kraftProfileBatchConfig batch;
    kraftProfileSnapshotConfig snapshot;
    kraftProfileSessionConfig session;
    kraftProfileAsyncCommitConfig asyncCommit;
    kraftProfileBufferPoolConfig bufferPool;

    /* Logging */
    kraftLogLevel logLevel;
} kraftConfig;

/* =========================================================================
 * Default Values
 * ========================================================================= */

/* Timing defaults (Production profile) */
#define KRAFT_PROFILE_DEFAULT_ELECTION_MIN_MS 150
#define KRAFT_PROFILE_DEFAULT_ELECTION_MAX_MS 500
#define KRAFT_PROFILE_DEFAULT_HEARTBEAT_MS 50
#define KRAFT_PROFILE_DEFAULT_RPC_TIMEOUT_MS 100

/* Batch defaults */
#define KRAFT_PROFILE_DEFAULT_BATCH_SIZE 100
#define KRAFT_PROFILE_DEFAULT_BATCH_BYTES (1024 * 1024) /* 1 MB */
#define KRAFT_PROFILE_DEFAULT_BATCH_FLUSH_US 1000       /* 1 ms */
#define KRAFT_PROFILE_DEFAULT_BATCH_IN_FLIGHT 4

/* Snapshot defaults */
#define KRAFT_PROFILE_DEFAULT_SNAPSHOT_SIZE (100 * 1024 * 1024) /* 100 MB */
#define KRAFT_PROFILE_DEFAULT_SNAPSHOT_ENTRIES 10000
#define KRAFT_PROFILE_DEFAULT_SNAPSHOT_CHUNK (1024 * 1024) /* 1 MB */
#define KRAFT_PROFILE_DEFAULT_SNAPSHOT_TRANSFERS 3
#define KRAFT_PROFILE_DEFAULT_SNAPSHOT_RETENTION 3

/* Session defaults */
#define KRAFT_PROFILE_DEFAULT_MAX_SESSIONS 1000
#define KRAFT_PROFILE_DEFAULT_CACHED_PER_SESSION 10
#define KRAFT_PROFILE_DEFAULT_SESSION_TIMEOUT_US (60 * 1000000ULL) /* 60s */

/* Buffer pool defaults */
#define KRAFT_PROFILE_DEFAULT_POOL_SMALL 64
#define KRAFT_PROFILE_DEFAULT_POOL_MEDIUM 32
#define KRAFT_PROFILE_DEFAULT_POOL_LARGE 8

/* =========================================================================
 * API Functions
 * ========================================================================= */

/**
 * Get configuration for a specific profile
 *
 * @param profile The profile type
 * @return Configuration structure initialized for the specified profile
 */
kraftConfig kraftConfigForProfile(kraftProfile profile);

/**
 * Get default configuration (alias for KRAFT_PROFILE_PRODUCTION)
 *
 * @return Configuration structure with production defaults
 */
kraftConfig kraftConfigDefault(void);

/**
 * Get profile name as string (for logging/debugging)
 *
 * @param profile The profile type
 * @return Human-readable profile name
 */
const char *kraftProfileName(kraftProfile profile);

/**
 * Validate configuration for consistency and safety
 *
 * @param config Configuration to validate
 * @param errorMsg Buffer for error message (can be NULL)
 * @param errorMsgLen Length of error message buffer
 * @return Validation result (KRAFT_CONFIG_VALID if OK)
 */
kraftConfigValidation kraftConfigValidate(const kraftConfig *config,
                                          char *errorMsg, size_t errorMsgLen);

/**
 * Apply unified config to kraftState during initialization.
 * Should be called after kraftStateNew() but before starting cluster.
 *
 * @param state Kraft state to configure
 * @param config Configuration to apply
 * @return true on success, false on validation error
 */
bool kraftStateApplyConfig(struct kraftState *state, const kraftConfig *config);

/* =========================================================================
 * Override Helpers
 * ========================================================================= */

/**
 * Override timing parameters within a configuration
 */
void kraftConfigSetTiming(kraftConfig *config, uint32_t electionTimeoutMinMs,
                          uint32_t electionTimeoutMaxMs,
                          uint32_t heartbeatIntervalMs);

/**
 * Override batch configuration
 */
void kraftConfigSetBatch(kraftConfig *config, uint32_t maxBatchSize,
                         uint32_t maxInFlight);

/**
 * Enable or disable all advanced features
 */
void kraftConfigSetAllFeatures(kraftConfig *config, bool enabled);

/* =========================================================================
 * Statistics
 * ========================================================================= */

typedef struct kraftProfileStats {
    kraftProfile activeProfile;
    uint64_t configAppliedAt; /* Timestamp when config was applied */
    uint32_t overrideCount;   /* Number of values overridden from profile */
} kraftProfileStats;

/**
 * Get profile statistics from state
 */
void kraftProfileGetStats(const struct kraftState *state,
                          kraftProfileStats *stats);

#endif /* KRAFT_PROFILE_H */
