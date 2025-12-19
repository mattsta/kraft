/* kraftLinearizabilityCluster.c - Live Cluster Management Implementation
 *
 * Manages kraft-loopy cluster processes for linearizability testing.
 */

#include "kraftLinearizabilityCluster.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* ============================================================================
 * Utility Functions
 * ============================================================================
 */

/**
 * findKraftLoopyBinary - Locate kraft-loopy binary
 *
 * Search order:
 * 1. KRAFT_LOOPY_BINARY environment variable
 * 2. kraft-loopy in current working directory
 * 3. kraft-loopy in PATH
 */
static const char *findKraftLoopyBinary(void) {
    /* Check environment variable first */
    const char *envBinary = getenv("KRAFT_LOOPY_BINARY");
    if (envBinary && access(envBinary, X_OK) == 0) {
        return envBinary;
    }

    /* Check current directory and subdirectories */
    static const char *relativePaths[] = {
        "kraft-loopy",
        "./kraft-loopy",
        "loopy-demo/kraft-loopy",
        "../loopy-demo/kraft-loopy",
        NULL,
    };

    for (int i = 0; relativePaths[i]; i++) {
        if (access(relativePaths[i], X_OK) == 0) {
            return relativePaths[i];
        }
    }

    /* Try PATH lookup using 'which' command */
    FILE *which = popen("which kraft-loopy 2>/dev/null", "r");
    if (which) {
        static char pathBuf[1024];
        if (fgets(pathBuf, sizeof(pathBuf), which) != NULL) {
            /* Remove trailing newline */
            pathBuf[strcspn(pathBuf, "\n")] = '\0';
            pclose(which);
            if (access(pathBuf, X_OK) == 0) {
                return pathBuf;
            }
        }
        pclose(which);
    }

    return NULL;
}

/**
 * waitForPort - Wait for port to be listening
 */
static bool waitForPort(uint32_t port, uint32_t timeoutSec) {
    time_t start = time(NULL);

    while ((time(NULL) - start) < timeoutSec) {
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            continue;
        }

        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = inet_addr("127.0.0.1");

        if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) == 0) {
            close(sock);
            return true;
        }

        close(sock);
        usleep(100000); /* 100ms */
    }

    return false;
}

/**
 * queryStatus - Query node STATUS via Client API
 *
 * Returns malloc'd response string, or NULL on failure.
 */
static char *queryStatus(uint32_t clientPort) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return NULL;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(clientPort);
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(sock);
        return NULL;
    }

    /* Send STATUS command */
    const char *cmd = "STATUS\r\n";
    if (send(sock, cmd, strlen(cmd), 0) < 0) {
        close(sock);
        return NULL;
    }

    /* Read response */
    char *buffer = malloc(4096);
    if (!buffer) {
        close(sock);
        return NULL;
    }

    ssize_t n = recv(sock, buffer, 4095, 0);
    close(sock);

    if (n <= 0) {
        free(buffer);
        return NULL;
    }

    buffer[n] = '\0';
    return buffer;
}

/**
 * parseLeaderFromStatus - Extract leader ID from STATUS response
 */
static uint32_t parseLeaderFromStatus(const char *statusResp) {
    if (!statusResp) {
        return 0;
    }

    /* Look for "leader:N" in response */
    const char *leader = strstr(statusResp, "leader:");
    if (!leader) {
        return 0;
    }

    return (uint32_t)atoi(leader + 7);
}

/* ============================================================================
 * Node Lifecycle
 * ============================================================================
 */

/**
 * startNode - Fork and exec a kraft-loopy node
 *
 * Takes absolute binary path to avoid any pointer/memory issues across fork.
 */
static pid_t startNode(const char *binaryPath, uint32_t basePort,
                       uint32_t clientBasePort, uint32_t nodeId,
                       const char *clusterAddrs, const char *dataDir,
                       const char *logFile, bool verbose) {
    if (!binaryPath) {
        fprintf(stderr, "ERROR: binaryPath is NULL\n");
        return -1;
    }

    pid_t pid = fork();

    if (pid < 0) {
        /* Fork failed */
        return -1;
    }

    if (pid == 0) {
        /* Child process */

        /* Redirect stdout/stderr to log file */
        FILE *log = fopen(logFile, "w");
        if (log) {
            dup2(fileno(log), STDOUT_FILENO);
            dup2(fileno(log), STDERR_FILENO);
            fclose(log);
        }

        /* Build command-line arguments */
        uint32_t raftPort = basePort + nodeId - 1;
        uint32_t clientPort = clientBasePort + nodeId - 1;

        char nodeIdStr[32];
        char listenAddr[64];
        char clientPortStr[32];

        // CRITICAL: Use port as node ID to match parseClusterPeers behavior
        snprintf(nodeIdStr, sizeof(nodeIdStr), "%u", raftPort);
        snprintf(listenAddr, sizeof(listenAddr), "127.0.0.1:%u", raftPort);
        snprintf(clientPortStr, sizeof(clientPortStr), "%u", clientPort);

        /* Debug: Print command line */
        if (verbose) {
            fprintf(stderr,
                    "[DEBUG] Node %u exec: %s --node %s --listen %s --cluster "
                    "%s --data %s --client %s\n",
                    nodeId, binaryPath, nodeIdStr, listenAddr, clusterAddrs,
                    dataDir, clientPortStr);
        }

        char *argv[] = {(char *)binaryPath,
                        "--node",
                        nodeIdStr,
                        "--listen",
                        listenAddr,
                        "--cluster",
                        (char *)clusterAddrs,
                        "--data",
                        (char *)dataDir,
                        "--client",
                        clientPortStr,
                        "--daemon",
                        NULL};

        execv(binaryPath, argv);

        /* If exec fails */
        fprintf(stderr,
                "ERROR: Failed to exec kraft-loopy at %s: %s (errno=%d)\n",
                binaryPath, strerror(errno), errno);
        exit(1);
    }

    /* Parent process */
    return pid;
}

/* ============================================================================
 * Cluster Lifecycle
 * ============================================================================
 */

kraftLinCluster *kraftLinClusterStart(const kraftLinClusterConfig *config) {
    if (!config || config->nodeCount < 3 || config->nodeCount > 100) {
        return NULL;
    }

    /* Find binary if not specified */
    const char *binaryPath = config->binaryPath;
    if (!binaryPath) {
        binaryPath = findKraftLoopyBinary();
        if (!binaryPath) {
            fprintf(stderr, "ERROR: kraft-loopy binary not found.\n");
            fprintf(stderr, "Set KRAFT_LOOPY_BINARY environment variable or "
                            "ensure kraft-loopy is in PATH.\n");
            fprintf(
                stderr,
                "Example: export KRAFT_LOOPY_BINARY=loopy-demo/kraft-loopy\n");
            return NULL;
        }
    }

    /* Verify binary exists */
    if (access(binaryPath, X_OK) != 0) {
        fprintf(stderr, "ERROR: kraft-loopy binary not executable: %s\n",
                binaryPath);
        fprintf(stderr, "Check permissions and path.\n");
        return NULL;
    }

    /* Always show which binary we're using */
    printf("Using kraft-loopy binary: %s\n", binaryPath);

    kraftLinCluster *cluster = calloc(1, sizeof(*cluster));
    if (!cluster) {
        return NULL;
    }

    /* Copy config and ensure binary path is set */
    cluster->config = *config;

    /* Store resolved absolute path to ensure it persists and works from any CWD
     */
    char absPath[1024];
    if (realpath(binaryPath, absPath) != NULL) {
        cluster->config.binaryPath = strdup(absPath);
        printf("Resolved to: %s\n", absPath);
    } else {
        cluster->config.binaryPath = strdup(binaryPath);
    }

    if (!cluster->config.binaryPath) {
        free(cluster);
        return NULL;
    }

    /* Allocate arrays */
    cluster->nodePids = calloc(config->nodeCount, sizeof(pid_t));
    cluster->nodeAlive = calloc(config->nodeCount, sizeof(bool));
    cluster->nodeDataDirs = calloc(config->nodeCount, sizeof(char *));
    cluster->nodeLogFiles = calloc(config->nodeCount, sizeof(char *));

    if (!cluster->nodePids || !cluster->nodeAlive || !cluster->nodeDataDirs ||
        !cluster->nodeLogFiles) {
        free(cluster->nodePids);
        free(cluster->nodeAlive);
        free(cluster->nodeDataDirs);
        free(cluster->nodeLogFiles);
        free(cluster);
        return NULL;
    }

    /* Build cluster address list */
    char clusterAddrs[2048] = {0};
    for (uint32_t i = 1; i <= config->nodeCount; i++) {
        uint32_t port = config->basePort + i - 1;
        char addr[64];
        snprintf(addr, sizeof(addr), "127.0.0.1:%u", port);

        if (i > 1) {
            strncat(clusterAddrs, ",",
                    sizeof(clusterAddrs) - strlen(clusterAddrs) - 1);
        }
        strncat(clusterAddrs, addr,
                sizeof(clusterAddrs) - strlen(clusterAddrs) - 1);
    }

    if (config->verbose) {
        printf("Starting %u-node cluster: %s\n", config->nodeCount,
               clusterAddrs);
        printf("  Base ports: Raft=%u, Client=%u\n", config->basePort,
               config->clientBasePort);
    }

    /* Start each node */
    for (uint32_t i = 1; i <= config->nodeCount; i++) {
        /* Create node-specific directories */
        char dataDir[512];
        char logFile[512];
        snprintf(dataDir, sizeof(dataDir), "%s/node%u", config->dataDir, i);
        snprintf(logFile, sizeof(logFile), "%s/logs/node%u.log",
                 config->dataDir, i);

        cluster->nodeDataDirs[i - 1] = strdup(dataDir);
        cluster->nodeLogFiles[i - 1] = strdup(logFile);

        /* Node data directory should already be created by runner */

        /* Start node */
        pid_t pid =
            startNode(cluster->config.binaryPath, cluster->config.basePort,
                      cluster->config.clientBasePort, i, clusterAddrs, dataDir,
                      logFile, cluster->config.verbose);
        if (pid < 0) {
            fprintf(stderr, "Failed to start node %u\n", i);
            kraftLinClusterStop(cluster);
            return NULL;
        }

        cluster->nodePids[i - 1] = pid;
        cluster->nodeAlive[i - 1] = true;

        if (config->verbose) {
            printf("  Node %u started (PID %d, Raft port %u, Client port %u)\n",
                   i, pid, config->basePort + i - 1,
                   config->clientBasePort + i - 1);
        }

        /* Brief delay between starts */
        usleep(300000); /* 300ms */
    }

    printf("All nodes forked. Waiting for client API ports...\n");

    /* Wait for all client API ports to be ready */
    for (uint32_t i = 1; i <= config->nodeCount; i++) {
        uint32_t clientPort = config->clientBasePort + i - 1;
        if (config->verbose) {
            printf("  Waiting for node %u client port %u...\n", i, clientPort);
        }

        if (!waitForPort(clientPort, 10)) {
            fprintf(stderr,
                    "ERROR: Node %u client API port %u not ready after 10s\n",
                    i, clientPort);

            /* Check if process still alive */
            if (kill(cluster->nodePids[i - 1], 0) != 0) {
                fprintf(stderr, "ERROR: Node %u process (PID %d) is DEAD!\n", i,
                        cluster->nodePids[i - 1]);
            }

            kraftLinClusterStop(cluster);
            return NULL;
        }

        if (config->verbose) {
            printf("  Node %u client port ready\n", i);
        }
    }

    /* Let cluster stabilize (nodes need time to establish connections) */
    if (config->verbose) {
        printf("Cluster ports ready, letting nodes stabilize...\n");
    }
    sleep(2);

    /* Wait for leader election */
    if (config->verbose) {
        printf("Waiting for leader election...\n");
    }

    uint32_t leader =
        kraftLinClusterWaitForLeader(cluster, 30); /* Increased to 30s */
    if (leader == 0) {
        fprintf(stderr, "Cluster failed to elect leader\n");
        kraftLinClusterStop(cluster);
        return NULL;
    }

    cluster->currentLeader = leader;

    if (config->verbose) {
        printf("Cluster ready (leader: node %u)\n", leader);
    }

    return cluster;
}

void kraftLinClusterStop(kraftLinCluster *cluster) {
    if (!cluster) {
        return;
    }

    if (cluster->config.verbose) {
        printf("Stopping cluster...\n");
    }

    /* Send SIGTERM to all nodes */
    for (uint32_t i = 0; i < cluster->config.nodeCount; i++) {
        if (cluster->nodeAlive[i] && cluster->nodePids[i] > 0) {
            kill(cluster->nodePids[i], SIGTERM);
        }
    }

    /* Wait briefly for graceful shutdown */
    sleep(1);

    /* SIGKILL any remaining */
    for (uint32_t i = 0; i < cluster->config.nodeCount; i++) {
        if (cluster->nodeAlive[i] && cluster->nodePids[i] > 0) {
            kill(cluster->nodePids[i], SIGKILL);
            waitpid(cluster->nodePids[i], NULL, 0);
            cluster->nodeAlive[i] = false;
        }
    }

    /* Free resources */
    for (uint32_t i = 0; i < cluster->config.nodeCount; i++) {
        free(cluster->nodeDataDirs[i]);
        free(cluster->nodeLogFiles[i]);
    }

    free(cluster->nodePids);
    free(cluster->nodeAlive);
    free(cluster->nodeDataDirs);
    free(cluster->nodeLogFiles);
    free(cluster);
}

/* ============================================================================
 * Node Control
 * ============================================================================
 */

bool kraftLinClusterKillNode(kraftLinCluster *cluster, uint32_t nodeId,
                             int signal) {
    if (!cluster || nodeId < 1 || nodeId > cluster->config.nodeCount) {
        return false;
    }

    uint32_t idx = nodeId - 1;
    if (!cluster->nodeAlive[idx]) {
        return false; /* Already dead */
    }

    if (cluster->config.verbose) {
        printf("Killing node %u (PID %d) with signal %d\n", nodeId,
               cluster->nodePids[idx], signal);
    }

    kill(cluster->nodePids[idx], signal);

    if (signal == SIGKILL) {
        waitpid(cluster->nodePids[idx], NULL, 0);
    }

    cluster->nodeAlive[idx] = false;

    /* Update leader if we killed it */
    if (cluster->currentLeader == nodeId) {
        cluster->currentLeader = 0;
    }

    return true;
}

bool kraftLinClusterRestartNode(kraftLinCluster *cluster, uint32_t nodeId) {
    if (!cluster || nodeId < 1 || nodeId > cluster->config.nodeCount) {
        return false;
    }

    uint32_t idx = nodeId - 1;
    if (cluster->nodeAlive[idx]) {
        return false; /* Already running */
    }

    /* Build cluster address list */
    char clusterAddrs[2048] = {0};
    for (uint32_t i = 1; i <= cluster->config.nodeCount; i++) {
        uint32_t port = cluster->config.basePort + i - 1;
        char addr[64];
        snprintf(addr, sizeof(addr), "127.0.0.1:%u", port);

        if (i > 1) {
            strncat(clusterAddrs, ",",
                    sizeof(clusterAddrs) - strlen(clusterAddrs) - 1);
        }
        strncat(clusterAddrs, addr,
                sizeof(clusterAddrs) - strlen(clusterAddrs) - 1);
    }

    /* Restart node */
    pid_t pid = startNode(cluster->config.binaryPath, cluster->config.basePort,
                          cluster->config.clientBasePort, nodeId, clusterAddrs,
                          cluster->nodeDataDirs[idx],
                          cluster->nodeLogFiles[idx], cluster->config.verbose);

    if (pid < 0) {
        return false;
    }

    cluster->nodePids[idx] = pid;
    cluster->nodeAlive[idx] = true;

    if (cluster->config.verbose) {
        printf("Node %u restarted (PID %d)\n", nodeId, pid);
    }

    /* Wait for client port */
    uint32_t clientPort = cluster->config.clientBasePort + nodeId - 1;
    return waitForPort(clientPort, 10);
}

bool kraftLinClusterIsNodeAlive(const kraftLinCluster *cluster,
                                uint32_t nodeId) {
    if (!cluster || nodeId < 1 || nodeId > cluster->config.nodeCount) {
        return false;
    }

    return cluster->nodeAlive[nodeId - 1];
}

/* ============================================================================
 * Leader Discovery
 * ============================================================================
 */

uint32_t kraftLinClusterFindLeader(kraftLinCluster *cluster) {
    if (!cluster) {
        return 0;
    }

    if (cluster->config.verbose) {
        printf("[DEBUG] Finding leader among %u nodes...\n",
               cluster->config.nodeCount);
    }

    /* Query each alive node */
    for (uint32_t i = 1; i <= cluster->config.nodeCount; i++) {
        if (!cluster->nodeAlive[i - 1]) {
            if (cluster->config.verbose) {
                printf("[DEBUG] Node %u is dead, skipping\n", i);
            }
            continue;
        }

        uint32_t clientPort = cluster->config.clientBasePort + i - 1;
        if (cluster->config.verbose) {
            printf("[DEBUG] Querying node %u on port %u...\n", i, clientPort);
        }

        char *status = queryStatus(clientPort);
        if (!status) {
            if (cluster->config.verbose) {
                printf("[DEBUG] Node %u: No STATUS response\n", i);
            }
            continue;
        }

        if (cluster->config.verbose) {
            printf("[DEBUG] Node %u STATUS: %.200s\n", i, status);
        }

        /* Check if this node is the leader */
        if (strstr(status, "role:LEADER") != NULL) {
            if (cluster->config.verbose) {
                printf("[DEBUG] Node %u is LEADER!\n", i);
            }
            free(status);
            return i;
        }

        /* Or extract leader ID from response */
        uint32_t leaderId = parseLeaderFromStatus(status);
        free(status);

        if (leaderId > 0) {
            if (cluster->config.verbose) {
                printf("[DEBUG] Node %u reports leader is %u\n", i, leaderId);
            }
            return leaderId;
        }
    }

    if (cluster->config.verbose) {
        printf("[DEBUG] No leader found\n");
    }
    return 0; /* No leader found */
}

uint32_t kraftLinClusterWaitForLeader(kraftLinCluster *cluster,
                                      uint32_t timeoutSec) {
    if (!cluster) {
        return 0;
    }

    time_t start = time(NULL);

    while ((time(NULL) - start) < timeoutSec) {
        uint32_t leader = kraftLinClusterFindLeader(cluster);
        if (leader > 0) {
            cluster->currentLeader = leader;
            return leader;
        }

        sleep(1);
    }

    return 0; /* Timeout */
}
