/* kraftLinearizabilityCluster.h - Live Cluster Management
 *
 * Manages lifecycle of kraft-loopy cluster processes for linearizability
 * testing.
 *
 * Features:
 * - Fork/exec kraft-loopy nodes with proper configuration
 * - PID tracking and process termination
 * - Health monitoring via TCP connectivity
 * - Leader discovery via STATUS command
 * - Clean resource cleanup
 */

#ifndef KRAFT_LINEARIZABILITY_CLUSTER_H
#define KRAFT_LINEARIZABILITY_CLUSTER_H

#include <stdbool.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Cluster Configuration
 * ============================================================================
 */

typedef struct kraftLinClusterConfig {
    uint32_t nodeCount;      /* Number of nodes in cluster */
    uint32_t basePort;       /* Base port for Raft protocol */
    uint32_t clientBasePort; /* Base port for Client API */
    const char *binaryPath;  /* Path to kraft-loopy binary */
    const char *dataDir;     /* Base directory for node data */
    bool verbose;            /* Verbose logging */
} kraftLinClusterConfig;

/* ============================================================================
 * Cluster State
 * ============================================================================
 */

typedef struct kraftLinCluster {
    kraftLinClusterConfig config;

    /* Node state */
    pid_t *nodePids;     /* PIDs of kraft-loopy processes */
    bool *nodeAlive;     /* Track which nodes are running */
    char **nodeDataDirs; /* Per-node data directories */
    char **nodeLogFiles; /* Per-node log files */

    /* Cluster state */
    uint32_t currentLeader; /* Current leader node ID (0 if unknown) */

} kraftLinCluster;

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

/**
 * kraftLinClusterStart - Start a kraft-loopy cluster
 *
 * Spawns nodeCount kraft-loopy processes, each configured to form a cluster.
 * Waits for all nodes to start and establish a leader.
 *
 * @param config  Cluster configuration
 * @return        Cluster handle, or NULL on failure
 */
kraftLinCluster *kraftLinClusterStart(const kraftLinClusterConfig *config);

/**
 * kraftLinClusterStop - Stop the cluster
 *
 * Sends SIGTERM to all running nodes, waits briefly, then SIGKILL if needed.
 * Cleans up temp directories and other resources.
 *
 * @param cluster  Cluster to stop
 */
void kraftLinClusterStop(kraftLinCluster *cluster);

/* ============================================================================
 * Node Control
 * ============================================================================
 */

/**
 * kraftLinClusterKillNode - Kill a specific node
 *
 * @param cluster  Cluster handle
 * @param nodeId   Node ID (1-based)
 * @param signal   Signal to send (SIGTERM or SIGKILL)
 * @return         true on success
 */
bool kraftLinClusterKillNode(kraftLinCluster *cluster, uint32_t nodeId,
                             int signal);

/**
 * kraftLinClusterRestartNode - Restart a crashed node
 *
 * @param cluster  Cluster handle
 * @param nodeId   Node ID (1-based)
 * @return         true on success
 */
bool kraftLinClusterRestartNode(kraftLinCluster *cluster, uint32_t nodeId);

/**
 * kraftLinClusterIsNodeAlive - Check if node is running
 *
 * @param cluster  Cluster handle
 * @param nodeId   Node ID (1-based)
 * @return         true if node is alive
 */
bool kraftLinClusterIsNodeAlive(const kraftLinCluster *cluster,
                                uint32_t nodeId);

/* ============================================================================
 * Cluster State Queries
 * ============================================================================
 */

/**
 * kraftLinClusterFindLeader - Discover current leader
 *
 * Queries all running nodes via STATUS command to find the current leader.
 *
 * @param cluster  Cluster handle
 * @return         Leader node ID (1-based), or 0 if no leader
 */
uint32_t kraftLinClusterFindLeader(kraftLinCluster *cluster);

/**
 * kraftLinClusterWaitForLeader - Wait for leader election
 *
 * Polls the cluster until a leader is elected or timeout expires.
 *
 * @param cluster     Cluster handle
 * @param timeoutSec  Timeout in seconds
 * @return            Leader node ID, or 0 on timeout
 */
uint32_t kraftLinClusterWaitForLeader(kraftLinCluster *cluster,
                                      uint32_t timeoutSec);

/**
 * kraftLinClusterGetQuorumSize - Calculate quorum size
 *
 * @param cluster  Cluster handle
 * @return         Quorum size (majority)
 */
static inline uint32_t
kraftLinClusterGetQuorumSize(const kraftLinCluster *cluster) {
    return (cluster->config.nodeCount / 2) + 1;
}

/**
 * kraftLinClusterGetClientPort - Get Client API port for a node
 *
 * @param cluster  Cluster handle
 * @param nodeId   Node ID (1-based)
 * @return         Client API port number
 */
static inline uint32_t
kraftLinClusterGetClientPort(const kraftLinCluster *cluster, uint32_t nodeId) {
    return cluster->config.clientBasePort + nodeId - 1;
}

#ifdef __cplusplus
}
#endif

#endif /* KRAFT_LINEARIZABILITY_CLUSTER_H */
