#include "profile.h"
#include <stdio.h>
#include <string.h>

/* Profile names for logging/debugging */
static const char *profileNames[] = {
    [KRAFT_PROFILE_CUSTOM] = "CUSTOM",
    [KRAFT_PROFILE_DEVELOPMENT] = "DEVELOPMENT",
    [KRAFT_PROFILE_PRODUCTION] = "PRODUCTION",
    [KRAFT_PROFILE_HIGH_THROUGHPUT] = "HIGH_THROUGHPUT",
    [KRAFT_PROFILE_LOW_LATENCY] = "LOW_LATENCY",
    [KRAFT_PROFILE_LARGE_CLUSTER] = "LARGE_CLUSTER",
    [KRAFT_PROFILE_MEMORY_CONSTRAINED] = "MEMORY_CONSTRAINED"};

/* =========================================================================
 * Profile Definitions
 * ========================================================================= */

/* DEVELOPMENT: Fast iteration with verbose output */
static kraftConfig profileDevelopment(void) {
    return (kraftConfig){
        .profile = KRAFT_PROFILE_DEVELOPMENT,
        .timing = {.electionTimeoutMinMs =
                       50, /* Fast elections for quick testing */
                   .electionTimeoutMaxMs =
                       100, /* Narrow range for predictability */
                   .heartbeatIntervalMs = 20, /* Frequent heartbeats */
                   .rpcTimeoutMs = 50},
        .features = {.enablePreVote = false, /* Simpler debugging */
                     .enableLeadershipTransfer = true,
                     .enableReadIndex = true,
                     .enableMetrics = true,
                     .enableBatching =
                         false, /* See each command individually */
                     .enableSessions = false,
                     .enableWitness = false,
                     .enableAsyncCommit = false,
                     .enableSnapshots = true},
        .batch = {.maxBatchSize = 10,         /* Small batches for visibility */
                  .maxBatchBytes = 64 * 1024, /* 64KB */
                  .flushIntervalUs = 100,     /* Flush quickly */
                  .maxInFlight = 1},
        .snapshot = {.logSizeThresholdBytes =
                         10 * 1024 * 1024,       /* 10MB - frequent snapshots */
                     .entryCountThreshold = 100, /* Frequent for testing */
                     .chunkSizeBytes = 256 * 1024, /* 256KB */
                     .maxConcurrentTransfers = 1,
                     .maxSnapshots = 2,
                     .compactAfterSnapshot = true},
        .session =
            {
                .maxSessions = 100,
                .maxCachedPerSession = 5,
                .sessionTimeoutUs =
                    10 * 1000000ULL /* 10s - short for testing */
            },
        .asyncCommit =
            {
                .defaultMode = 0, /* STRONG */
                .flexibleAcks = 1,
                .asyncMaxPendingBytes = 1 * 1024 * 1024 /* 1MB */
            },
        .bufferPool = {.smallPoolCount = 16,
                       .mediumPoolCount = 8,
                       .largePoolCount = 4},
        .logLevel = KRAFT_LOG_DEBUG};
}

/* PRODUCTION: Conservative, safety-first defaults */
static kraftConfig profileProduction(void) {
    return (kraftConfig){
        .profile = KRAFT_PROFILE_PRODUCTION,
        .timing = {.electionTimeoutMinMs = 150, /* Raft paper recommendation */
                   .electionTimeoutMaxMs = 500, /* Wide range for jitter */
                   .heartbeatIntervalMs = 50,   /* Standard heartbeat */
                   .rpcTimeoutMs = 100},
        .features = {.enablePreVote = true, /* Prevent disruption */
                     .enableLeadershipTransfer = true,
                     .enableReadIndex = true,
                     .enableMetrics = true,
                     .enableBatching = true,
                     .enableSessions = true,
                     .enableWitness = false,     /* Opt-in */
                     .enableAsyncCommit = false, /* Opt-in */
                     .enableSnapshots = true},
        .batch = {.maxBatchSize = KRAFT_PROFILE_DEFAULT_BATCH_SIZE,
                  .maxBatchBytes = KRAFT_PROFILE_DEFAULT_BATCH_BYTES,
                  .flushIntervalUs = KRAFT_PROFILE_DEFAULT_BATCH_FLUSH_US,
                  .maxInFlight = KRAFT_PROFILE_DEFAULT_BATCH_IN_FLIGHT},
        .snapshot =
            {.logSizeThresholdBytes = KRAFT_PROFILE_DEFAULT_SNAPSHOT_SIZE,
             .entryCountThreshold = KRAFT_PROFILE_DEFAULT_SNAPSHOT_ENTRIES,
             .chunkSizeBytes = KRAFT_PROFILE_DEFAULT_SNAPSHOT_CHUNK,
             .maxConcurrentTransfers = KRAFT_PROFILE_DEFAULT_SNAPSHOT_TRANSFERS,
             .maxSnapshots = KRAFT_PROFILE_DEFAULT_SNAPSHOT_RETENTION,
             .compactAfterSnapshot = true},
        .session = {.maxSessions = KRAFT_PROFILE_DEFAULT_MAX_SESSIONS,
                    .maxCachedPerSession =
                        KRAFT_PROFILE_DEFAULT_CACHED_PER_SESSION,
                    .sessionTimeoutUs =
                        KRAFT_PROFILE_DEFAULT_SESSION_TIMEOUT_US},
        .asyncCommit =
            {
                .defaultMode = 0, /* STRONG - full durability */
                .flexibleAcks = 2,
                .asyncMaxPendingBytes = 16 * 1024 * 1024 /* 16MB */
            },
        .bufferPool = {.smallPoolCount = KRAFT_PROFILE_DEFAULT_POOL_SMALL,
                       .mediumPoolCount = KRAFT_PROFILE_DEFAULT_POOL_MEDIUM,
                       .largePoolCount = KRAFT_PROFILE_DEFAULT_POOL_LARGE},
        .logLevel = KRAFT_LOG_WARN};
}

/* HIGH_THROUGHPUT: Maximize write throughput */
static kraftConfig profileHighThroughput(void) {
    return (kraftConfig){
        .profile = KRAFT_PROFILE_HIGH_THROUGHPUT,
        .timing = {.electionTimeoutMinMs =
                       200, /* Slightly longer for stability */
                   .electionTimeoutMaxMs = 600,
                   .heartbeatIntervalMs = 75,
                   .rpcTimeoutMs = 150},
        .features = {.enablePreVote = true,
                     .enableLeadershipTransfer = true,
                     .enableReadIndex = true,
                     .enableMetrics = true,
                     .enableBatching = true, /* Essential for throughput */
                     .enableSessions = true,
                     .enableWitness = false,
                     .enableAsyncCommit = true, /* Trade durability for speed */
                     .enableSnapshots = true},
        .batch =
            {
                .maxBatchSize = 1000,             /* Large batches */
                .maxBatchBytes = 4 * 1024 * 1024, /* 4MB */
                .flushIntervalUs = 5000,          /* 5ms - batch accumulation */
                .maxInFlight = 16                 /* Aggressive pipelining */
            },
        .snapshot = {.logSizeThresholdBytes =
                         500 * 1024 * 1024, /* 500MB - less frequent */
                     .entryCountThreshold = 100000,
                     .chunkSizeBytes = 4 * 1024 * 1024, /* 4MB chunks */
                     .maxConcurrentTransfers = 5,
                     .maxSnapshots = 2,
                     .compactAfterSnapshot = true},
        .session =
            {
                .maxSessions = 5000, /* More concurrent clients */
                .maxCachedPerSession = 20,
                .sessionTimeoutUs = 120 * 1000000ULL /* 2 minutes */
            },
        .asyncCommit =
            {
                .defaultMode = 1,  /* FLEXIBLE */
                .flexibleAcks = 1, /* Single ack for speed */
                .asyncMaxPendingBytes = 64 * 1024 * 1024 /* 64MB */
            },
        .bufferPool =
            {
                .smallPoolCount = 128,
                .mediumPoolCount = 64,
                .largePoolCount = 32 /* Many large buffers */
            },
        .logLevel = KRAFT_LOG_WARN};
}

/* LOW_LATENCY: Minimize commit latency */
static kraftConfig profileLowLatency(void) {
    return (kraftConfig){
        .profile = KRAFT_PROFILE_LOW_LATENCY,
        .timing = {.electionTimeoutMinMs = 100, /* Quick failover */
                   .electionTimeoutMaxMs = 300,
                   .heartbeatIntervalMs = 30, /* Fast leadership confirmation */
                   .rpcTimeoutMs = 50},
        .features = {.enablePreVote = true,
                     .enableLeadershipTransfer = true,
                     .enableReadIndex = true, /* Fast linearizable reads */
                     .enableMetrics = true,
                     .enableBatching = false, /* No batching delay */
                     .enableSessions = true,
                     .enableWitness = false,
                     .enableAsyncCommit = false, /* Full durability */
                     .enableSnapshots = true},
        .batch =
            {
                .maxBatchSize = 1, /* No batching */
                .maxBatchBytes = 64 * 1024,
                .flushIntervalUs = 0, /* Immediate flush */
                .maxInFlight = 1      /* No pipelining */
            },
        .snapshot = {.logSizeThresholdBytes =
                         KRAFT_PROFILE_DEFAULT_SNAPSHOT_SIZE,
                     .entryCountThreshold =
                         KRAFT_PROFILE_DEFAULT_SNAPSHOT_ENTRIES,
                     .chunkSizeBytes = 256 * 1024, /* 256KB - smaller chunks */
                     .maxConcurrentTransfers = 2,
                     .maxSnapshots = 3,
                     .compactAfterSnapshot = true},
        .session =
            {
                .maxSessions = 500,
                .maxCachedPerSession = 5,
                .sessionTimeoutUs = 30 * 1000000ULL /* 30s */
            },
        .asyncCommit = {.defaultMode = 0, /* STRONG */
                        .flexibleAcks = 2,
                        .asyncMaxPendingBytes = 4 * 1024 * 1024},
        .bufferPool = {.smallPoolCount = 128, /* Many small buffers */
                       .mediumPoolCount = 32,
                       .largePoolCount = 8},
        .logLevel = KRAFT_LOG_WARN};
}

/* LARGE_CLUSTER: Tuned for 10+ nodes */
static kraftConfig profileLargeCluster(void) {
    return (kraftConfig){
        .profile = KRAFT_PROFILE_LARGE_CLUSTER,
        .timing = {.electionTimeoutMinMs = 300,  /* Longer for network delays */
                   .electionTimeoutMaxMs = 1000, /* Wide jitter range */
                   .heartbeatIntervalMs = 100,   /* Less frequent heartbeats */
                   .rpcTimeoutMs = 200},
        .features = {.enablePreVote = true, /* Critical for large clusters */
                     .enableLeadershipTransfer = true,
                     .enableReadIndex = true,
                     .enableMetrics = true,
                     .enableBatching = true,
                     .enableSessions = true,
                     .enableWitness = true, /* Useful for geo-distribution */
                     .enableAsyncCommit = true,
                     .enableSnapshots = true},
        .batch =
            {
                .maxBatchSize = 500,
                .maxBatchBytes = 2 * 1024 * 1024,
                .flushIntervalUs = 2000, /* 2ms */
                .maxInFlight = 8         /* More pipelining for parallelism */
            },
        .snapshot = {.logSizeThresholdBytes = 200 * 1024 * 1024,
                     .entryCountThreshold = 50000,
                     .chunkSizeBytes = 2 * 1024 * 1024,
                     .maxConcurrentTransfers = 5, /* Parallel transfers */
                     .maxSnapshots = 3,
                     .compactAfterSnapshot = true},
        .session =
            {
                .maxSessions = 2000,
                .maxCachedPerSession = 10,
                .sessionTimeoutUs = 90 * 1000000ULL /* 90s */
            },
        .asyncCommit = {.defaultMode = 0,  /* STRONG for safety */
                        .flexibleAcks = 3, /* More acks for larger quorum */
                        .asyncMaxPendingBytes = 32 * 1024 * 1024},
        .bufferPool = {.smallPoolCount = 128,
                       .mediumPoolCount = 64,
                       .largePoolCount = 16},
        .logLevel = KRAFT_LOG_WARN};
}

/* MEMORY_CONSTRAINED: Minimal footprint */
static kraftConfig profileMemoryConstrained(void) {
    return (kraftConfig){
        .profile = KRAFT_PROFILE_MEMORY_CONSTRAINED,
        .timing = {.electionTimeoutMinMs = 150,
                   .electionTimeoutMaxMs = 500,
                   .heartbeatIntervalMs = 75,
                   .rpcTimeoutMs = 100},
        .features =
            {
                .enablePreVote = true,
                .enableLeadershipTransfer = false, /* Save memory */
                .enableReadIndex = false,          /* Save memory */
                .enableMetrics = false,            /* Save memory */
                .enableBatching = true,  /* Small batches still useful */
                .enableSessions = false, /* Save memory */
                .enableWitness = false,
                .enableAsyncCommit = false,
                .enableSnapshots = true /* Essential for log compaction */
            },
        .batch = {.maxBatchSize = 10,         /* Small batches */
                  .maxBatchBytes = 64 * 1024, /* 64KB */
                  .flushIntervalUs = 500,
                  .maxInFlight = 2},
        .snapshot = {.logSizeThresholdBytes =
                         10 * 1024 * 1024, /* 10MB - compact early */
                     .entryCountThreshold = 1000,
                     .chunkSizeBytes = 64 * 1024, /* 64KB - small chunks */
                     .maxConcurrentTransfers = 1,
                     .maxSnapshots = 1, /* Minimal retention */
                     .compactAfterSnapshot = true},
        .session = {.maxSessions = 100,
                    .maxCachedPerSession = 2,
                    .sessionTimeoutUs = 30 * 1000000ULL},
        .asyncCommit =
            {
                .defaultMode = 0,
                .flexibleAcks = 1,
                .asyncMaxPendingBytes = 1 * 1024 * 1024 /* 1MB */
            },
        .bufferPool = {.smallPoolCount = 16, /* Minimal pools */
                       .mediumPoolCount = 8,
                       .largePoolCount = 2},
        .logLevel = KRAFT_LOG_ERROR /* Minimal logging */
    };
}

/* =========================================================================
 * API Implementation
 * ========================================================================= */

kraftConfig kraftConfigForProfile(kraftProfile profile) {
    switch (profile) {
    case KRAFT_PROFILE_DEVELOPMENT:
        return profileDevelopment();
    case KRAFT_PROFILE_HIGH_THROUGHPUT:
        return profileHighThroughput();
    case KRAFT_PROFILE_LOW_LATENCY:
        return profileLowLatency();
    case KRAFT_PROFILE_LARGE_CLUSTER:
        return profileLargeCluster();
    case KRAFT_PROFILE_MEMORY_CONSTRAINED:
        return profileMemoryConstrained();
    case KRAFT_PROFILE_PRODUCTION:
    case KRAFT_PROFILE_CUSTOM:
    default:
        return profileProduction();
    }
}

kraftConfig kraftConfigDefault(void) {
    return kraftConfigForProfile(KRAFT_PROFILE_PRODUCTION);
}

const char *kraftProfileName(kraftProfile profile) {
    if (profile >= KRAFT_PROFILE_COUNT) {
        return "UNKNOWN";
    }
    return profileNames[profile];
}

kraftConfigValidation kraftConfigValidate(const kraftConfig *config,
                                          char *errorMsg, size_t errorMsgLen) {
    if (!config) {
        if (errorMsg && errorMsgLen > 0) {
            snprintf(errorMsg, errorMsgLen, "Config pointer is NULL");
        }
        return KRAFT_CONFIG_ERROR_INVALID_COUNTS;
    }

    /* Check timing constraints */
    /* Heartbeat must be much less than election timeout (Raft requirement) */
    if (config->timing.heartbeatIntervalMs >=
        config->timing.electionTimeoutMinMs) {
        if (errorMsg && errorMsgLen > 0) {
            snprintf(errorMsg, errorMsgLen,
                     "Heartbeat interval (%u ms) must be less than "
                     "min election timeout (%u ms)",
                     config->timing.heartbeatIntervalMs,
                     config->timing.electionTimeoutMinMs);
        }
        return KRAFT_CONFIG_ERROR_INVALID_TIMING;
    }

    /* Election timeout range must be valid */
    if (config->timing.electionTimeoutMinMs >=
        config->timing.electionTimeoutMaxMs) {
        if (errorMsg && errorMsgLen > 0) {
            snprintf(
                errorMsg, errorMsgLen,
                "Election timeout min (%u ms) must be less than max (%u ms)",
                config->timing.electionTimeoutMinMs,
                config->timing.electionTimeoutMaxMs);
        }
        return KRAFT_CONFIG_ERROR_INVALID_TIMING;
    }

    /* Check for zero values that would cause issues */
    if (config->timing.heartbeatIntervalMs == 0 ||
        config->timing.electionTimeoutMinMs == 0) {
        if (errorMsg && errorMsgLen > 0) {
            snprintf(errorMsg, errorMsgLen, "Timing values must be non-zero");
        }
        return KRAFT_CONFIG_ERROR_INVALID_COUNTS;
    }

    /* Check batch configuration */
    if (config->features.enableBatching && config->batch.maxBatchSize == 0) {
        if (errorMsg && errorMsgLen > 0) {
            snprintf(errorMsg, errorMsgLen,
                     "Batch size must be non-zero when batching is enabled");
        }
        return KRAFT_CONFIG_ERROR_INVALID_COUNTS;
    }

    /* Warning: Very short election timeouts */
    if (config->timing.electionTimeoutMinMs < 50) {
        if (errorMsg && errorMsgLen > 0) {
            snprintf(errorMsg, errorMsgLen,
                     "Warning: Very short election timeout (%u ms) may cause "
                     "instability",
                     config->timing.electionTimeoutMinMs);
        }
        return KRAFT_CONFIG_WARN_TIMING;
    }

    /* Warning: Very large buffers */
    uint64_t totalPoolBytes = (config->bufferPool.smallPoolCount * 256) +
                              (config->bufferPool.mediumPoolCount * 4096) +
                              (config->bufferPool.largePoolCount * 65536);
    if (totalPoolBytes > 100 * 1024 * 1024) { /* 100MB */
        if (errorMsg && errorMsgLen > 0) {
            snprintf(errorMsg, errorMsgLen,
                     "Warning: Buffer pool size (~%lluMB) is very large",
                     (unsigned long long)(totalPoolBytes / (1024 * 1024)));
        }
        return KRAFT_CONFIG_WARN_BUFFERS;
    }

    return KRAFT_CONFIG_VALID;
}

/* =========================================================================
 * Override Helpers
 * ========================================================================= */

void kraftConfigSetTiming(kraftConfig *config, uint32_t electionTimeoutMinMs,
                          uint32_t electionTimeoutMaxMs,
                          uint32_t heartbeatIntervalMs) {
    if (!config) {
        return;
    }
    config->timing.electionTimeoutMinMs = electionTimeoutMinMs;
    config->timing.electionTimeoutMaxMs = electionTimeoutMaxMs;
    config->timing.heartbeatIntervalMs = heartbeatIntervalMs;
}

void kraftConfigSetBatch(kraftConfig *config, uint32_t maxBatchSize,
                         uint32_t maxInFlight) {
    if (!config) {
        return;
    }
    config->batch.maxBatchSize = maxBatchSize;
    config->batch.maxInFlight = maxInFlight;
}

void kraftConfigSetAllFeatures(kraftConfig *config, bool enabled) {
    if (!config) {
        return;
    }
    config->features.enablePreVote = enabled;
    config->features.enableLeadershipTransfer = enabled;
    config->features.enableReadIndex = enabled;
    config->features.enableMetrics = enabled;
    config->features.enableBatching = enabled;
    config->features.enableSessions = enabled;
    config->features.enableWitness = enabled;
    config->features.enableAsyncCommit = enabled;
    config->features.enableSnapshots = enabled;
}
