/* kraftLinearizabilityWorkload.h - Concurrent Client Workload Generator
 *
 * Multi-threaded client workload generator for linearizability testing.
 *
 * Features:
 * - Parameterized operation mix (read/write/increment/delete percentages)
 * - Configurable key space and value sizes
 * - Thread-safe operation recording
 * - Automatic redirect handling
 * - Reconnection on errors
 */

#ifndef KRAFT_LINEARIZABILITY_WORKLOAD_H
#define KRAFT_LINEARIZABILITY_WORKLOAD_H

#include "kraftLinearizability.h"
#include "kraftLinearizabilityCluster.h"

#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Workload Configuration
 * ============================================================================
 */

typedef struct kraftLinWorkloadConfig {
    /* Operation mix (percentages, must sum to ≤ 100) */
    uint32_t readPct;  /* GET operations */
    uint32_t writePct; /* SET operations */
    uint32_t incrPct;  /* INCR operations */
    uint32_t delPct;   /* DEL operations */

    /* Data characteristics */
    uint32_t keyCount;  /* Number of distinct keys */
    uint32_t valueSize; /* Size of values in bytes (for writes) */

    /* Operation limits */
    uint64_t totalOps;    /* Total operations across all clients */
    uint32_t opTimeoutMs; /* Per-operation timeout */

    /* Value generation */
    enum {
        KRAFT_LIN_VALUE_SEQUENTIAL, /* 1, 2, 3, ... */
        KRAFT_LIN_VALUE_RANDOM,     /* Random values */
        KRAFT_LIN_VALUE_FIXED,      /* Fixed value */
    } valuePattern;

} kraftLinWorkloadConfig;

/**
 * kraftLinWorkloadConfigDefault - Get default workload configuration
 *
 * @return  Default configuration
 */
static inline kraftLinWorkloadConfig kraftLinWorkloadConfigDefault(void) {
    kraftLinWorkloadConfig config = {
        .readPct = 50,
        .writePct = 40,
        .incrPct = 10,
        .delPct = 0,
        .keyCount = 10,
        .valueSize = 64,
        .totalOps = 10000,
        .opTimeoutMs = 5000,
        .valuePattern = KRAFT_LIN_VALUE_SEQUENTIAL,
    };
    return config;
}

/* ============================================================================
 * Client Worker Pool
 * ============================================================================
 */

typedef struct kraftLinClientPool {
    pthread_t *threads;
    uint32_t clientCount;

    kraftLinCluster *cluster;
    kraftLinHistory *history;
    kraftLinWorkloadConfig config;

    volatile bool stopFlag;
    uint64_t opsCompleted;
    pthread_mutex_t opsLock;

} kraftLinClientPool;

/**
 * kraftLinClientPoolCreate - Create pool of client workers
 *
 * @param clientCount  Number of concurrent clients
 * @param cluster      Cluster to connect to
 * @param history      Shared operation history
 * @param config       Workload configuration
 * @return             Client pool, or NULL on failure
 */
kraftLinClientPool *
kraftLinClientPoolCreate(uint32_t clientCount, kraftLinCluster *cluster,
                         kraftLinHistory *history,
                         const kraftLinWorkloadConfig *config);

/**
 * kraftLinClientPoolStart - Start all client threads
 *
 * Spawns clientCount threads, each running the configured workload.
 * Returns immediately; threads run in background.
 *
 * @param pool  Client pool
 * @return      true on success
 */
bool kraftLinClientPoolStart(kraftLinClientPool *pool);

/**
 * kraftLinClientPoolStop - Stop all client threads
 *
 * Signals all threads to stop and waits for them to complete.
 *
 * @param pool  Client pool
 */
void kraftLinClientPoolStop(kraftLinClientPool *pool);

/**
 * kraftLinClientPoolDestroy - Destroy client pool
 *
 * Stops all threads if running and frees resources.
 *
 * @param pool  Client pool to destroy
 */
void kraftLinClientPoolDestroy(kraftLinClientPool *pool);

/**
 * kraftLinClientPoolGetOpsCompleted - Get total operations completed
 *
 * Thread-safe.
 *
 * @param pool  Client pool
 * @return      Number of operations completed across all clients
 */
uint64_t kraftLinClientPoolGetOpsCompleted(kraftLinClientPool *pool);

#ifdef __cplusplus
}
#endif

#endif /* KRAFT_LINEARIZABILITY_WORKLOAD_H */
