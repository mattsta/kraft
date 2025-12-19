/* kraftLinearizabilityWorkload.c - Client Workload Implementation
 *
 * Multi-threaded client workers that issue concurrent operations
 * to the cluster and record them for linearizability verification.
 */

#include "kraftLinearizabilityWorkload.h"

#include <arpa/inet.h>
#include <errno.h>
#include <inttypes.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

/* ============================================================================
 * Client Worker Thread Context
 * ============================================================================
 */

typedef struct kraftLinClientWorker {
    uint32_t clientId;
    kraftLinClientPool *pool;
    uint64_t clientSeq; /* Per-client operation sequence */
    int sockfd;
    uint32_t connectedNodeId;
    char readBuffer[8192];
    size_t readBufferLen;
} kraftLinClientWorker;

/* ============================================================================
 * TCP Connection Management
 * ============================================================================
 */

static int connectToNode(kraftLinCluster *cluster, uint32_t nodeId) {
    if (!cluster || nodeId < 1 || nodeId > cluster->config.nodeCount) {
        return -1;
    }

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(kraftLinClusterGetClientPort(cluster, nodeId));
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");

    if (connect(sock, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        close(sock);
        return -1;
    }

    return sock;
}

static int connectToCluster(kraftLinCluster *cluster,
                            uint32_t *connectedNodeId) {
    /* Try nodes in random order */
    for (uint32_t i = 1; i <= cluster->config.nodeCount; i++) {
        if (!cluster->nodeAlive[i - 1]) {
            continue;
        }

        int sock = connectToNode(cluster, i);
        if (sock >= 0) {
            *connectedNodeId = i;
            return sock;
        }
    }

    return -1; /* All nodes unreachable */
}

/* ============================================================================
 * Protocol Handling
 * ============================================================================
 */

/**
 * sendCommand - Send command over socket
 */
static bool sendCommand(int sock, const char *cmd) {
    size_t len = strlen(cmd);
    ssize_t sent = send(sock, cmd, len, 0);
    return (sent == (ssize_t)len);
}

/**
 * recvResponse - Receive response from server
 *
 * Reads until we get a complete response (terminated by \r\n or \n).
 * Returns malloc'd response string, or NULL on error.
 */
static char *recvResponse(int sock, uint32_t timeoutMs) {
    char buffer[4096];
    size_t totalRead = 0;

    /* Set socket timeout */
    struct timeval tv;
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    while (totalRead < sizeof(buffer) - 1) {
        ssize_t n =
            recv(sock, buffer + totalRead, sizeof(buffer) - totalRead - 1, 0);
        if (n <= 0) {
            if (n == 0 || errno == EAGAIN || errno == EWOULDBLOCK) {
                break; /* Timeout or EOF */
            }
            return NULL; /* Error */
        }

        totalRead += n;

        /* Check for end of response (\n) */
        if (buffer[totalRead - 1] == '\n') {
            break;
        }
    }

    if (totalRead == 0) {
        return NULL;
    }

    buffer[totalRead] = '\0';
    return strdup(buffer);
}

/**
 * parseResponse - Parse server response
 *
 * Returns:
 *   1 = success with value (value written to output)
 *   0 = success without value (OK)
 *  -1 = redirect (leaderNodeId written to redirect output)
 *  -2 = error
 */
static int parseResponse(const char *resp, char *valueOut, size_t valueOutSize,
                         uint32_t *redirectNodeId) {
    if (!resp) {
        return -2;
    }

    /* +OK\r\n */
    if (strncmp(resp, "+OK", 3) == 0) {
        return 0;
    }

    /* $N\r\ndata\r\n (bulk string) */
    if (resp[0] == '$') {
        int len = atoi(resp + 1);
        if (len < 0) {
            /* $-1 = null/not found */
            return 0;
        }

        const char *data = strchr(resp, '\n');
        if (data) {
            data++; /* Skip newline */
            /* Check if data is within bounds and has enough bytes */
            size_t remaining = strlen(data);
            if (remaining < (size_t)len) {
                /* Response truncated or malformed */
                return -2;
            }
            size_t copyLen =
                (len < (int)valueOutSize - 1) ? len : valueOutSize - 1;
            memcpy(valueOut, data, copyLen);
            valueOut[copyLen] = '\0';
            return 1;
        }
        return -2;
    }

    /* :N\r\n (integer) */
    if (resp[0] == ':') {
        snprintf(valueOut, valueOutSize, "%d", atoi(resp + 1));
        return 1;
    }

    /* -REDIRECT N\r\n */
    if (strncmp(resp, "-REDIRECT", 9) == 0) {
        if (redirectNodeId) {
            *redirectNodeId = atoi(resp + 10);
        }
        return -1;
    }

    /* -ERR message\r\n */
    if (resp[0] == '-') {
        return -2;
    }

    return -2; /* Unknown format */
}

/* ============================================================================
 * Operation Execution
 * ============================================================================
 */

typedef struct kraftLinOpSpec {
    kraftLinOpType type;
    char key[256];
    char value[512];
} kraftLinOpSpec;

/**
 * generateOperation - Generate random operation based on workload config
 */
static kraftLinOpSpec generateOperation(kraftLinClientWorker *worker) {
    kraftLinOpSpec op;
    memset(&op, 0, sizeof(op));

    kraftLinWorkloadConfig *config = &worker->pool->config;

    /* Choose operation type based on percentages */
    uint32_t r = rand() % 100;

    if (r < config->readPct) {
        op.type = KRAFT_LIN_OP_GET;
    } else if (r < config->readPct + config->writePct) {
        op.type = KRAFT_LIN_OP_SET;
    } else if (r < config->readPct + config->writePct + config->incrPct) {
        op.type = KRAFT_LIN_OP_INCR;
    } else {
        op.type = KRAFT_LIN_OP_DEL;
    }

    /* Choose key */
    snprintf(op.key, sizeof(op.key), "key%u", rand() % config->keyCount);

    /* Generate value for writes */
    if (op.type == KRAFT_LIN_OP_SET) {
        if (config->valuePattern == KRAFT_LIN_VALUE_SEQUENTIAL) {
            snprintf(op.value, sizeof(op.value), "%" PRIu64 "",
                     worker->clientSeq);
        } else {
            snprintf(op.value, sizeof(op.value), "value%d", rand());
        }
    }

    return op;
}

/**
 * executeOperation - Execute operation via Client API
 *
 * Returns true on success, false on error/timeout
 */
static bool executeOperation(kraftLinClientWorker *worker, kraftLinOpSpec *op,
                             char *resultValue, size_t resultSize) {
    char cmd[1024];

    /* Format command */
    switch (op->type) {
    case KRAFT_LIN_OP_GET:
        snprintf(cmd, sizeof(cmd), "GET %s\r\n", op->key);
        break;
    case KRAFT_LIN_OP_SET:
        snprintf(cmd, sizeof(cmd), "SET %s %s\r\n", op->key, op->value);
        break;
    case KRAFT_LIN_OP_INCR:
        snprintf(cmd, sizeof(cmd), "INCR %s\r\n", op->key);
        break;
    case KRAFT_LIN_OP_DEL:
        snprintf(cmd, sizeof(cmd), "DEL %s\r\n", op->key);
        break;
    default:
        return false;
    }

    /* Send command */
    if (!sendCommand(worker->sockfd, cmd)) {
        return false;
    }

    /* Receive response */
    char *resp = recvResponse(worker->sockfd, worker->pool->config.opTimeoutMs);
    if (!resp) {
        return false; /* Timeout or error */
    }

    /* Parse response */
    uint32_t redirectNode = 0;
    int parseResult =
        parseResponse(resp, resultValue, resultSize, &redirectNode);
    free(resp);

    if (parseResult == -1) {
        /* Redirect - need to reconnect to leader */
        close(worker->sockfd);
        worker->sockfd = -1;

        if (redirectNode > 0 &&
            redirectNode <= worker->pool->cluster->config.nodeCount) {
            worker->sockfd = connectToNode(worker->pool->cluster, redirectNode);
            if (worker->sockfd >= 0) {
                worker->connectedNodeId = redirectNode;
                /* Retry operation on new connection */
                return executeOperation(worker, op, resultValue, resultSize);
            }
        }

        return false;
    }

    return (parseResult >= 0); /* Success if 0 or 1 */
}

/* ============================================================================
 * Client Worker Thread
 * ============================================================================
 */

static void *clientWorkerThread(void *arg) {
    kraftLinClientWorker *worker = (kraftLinClientWorker *)arg;
    kraftLinClientPool *pool = worker->pool;

    /* Connect to cluster */
    worker->sockfd = connectToCluster(pool->cluster, &worker->connectedNodeId);
    if (worker->sockfd < 0) {
        fprintf(stderr, "Client %u: Failed to connect to cluster\n",
                worker->clientId);
        return NULL;
    }

    /* Run workload */
    while (!pool->stopFlag) {
        /* Check if we've reached target ops */
        pthread_mutex_lock(&pool->opsLock);
        if (pool->opsCompleted >= pool->config.totalOps) {
            pthread_mutex_unlock(&pool->opsLock);
            break;
        }
        pool->opsCompleted++;
        pthread_mutex_unlock(&pool->opsLock);

        /* Generate operation */
        kraftLinOpSpec op = generateOperation(worker);

        /* Record invocation */
        kraftLinOpId opId = kraftLinHistoryRecordInvoke(
            pool->history, worker->clientId, worker->clientSeq++, op.type,
            op.key, op.value);

        /* Execute operation */
        char resultValue[512] = {0};
        bool success =
            executeOperation(worker, &op, resultValue, sizeof(resultValue));

        if (success) {
            /* Record return */
            kraftLinHistoryRecordReturn(pool->history, opId, true, resultValue);
        } else {
            /* Record timeout/error */
            kraftLinHistoryRecordTimeout(pool->history, opId);

            /* Try to reconnect */
            if (worker->sockfd < 0) {
                worker->sockfd =
                    connectToCluster(pool->cluster, &worker->connectedNodeId);
            }
        }
    }

    /* Cleanup */
    if (worker->sockfd >= 0) {
        close(worker->sockfd);
    }

    return NULL;
}

/* ============================================================================
 * Client Pool Management
 * ============================================================================
 */

kraftLinClientPool *
kraftLinClientPoolCreate(uint32_t clientCount, kraftLinCluster *cluster,
                         kraftLinHistory *history,
                         const kraftLinWorkloadConfig *config) {
    if (!cluster || !history || !config || clientCount == 0) {
        return NULL;
    }

    kraftLinClientPool *pool = calloc(1, sizeof(*pool));
    if (!pool) {
        return NULL;
    }

    pool->threads = calloc(clientCount, sizeof(pthread_t));
    if (!pool->threads) {
        free(pool);
        return NULL;
    }

    pool->clientCount = clientCount;
    pool->cluster = cluster;
    pool->history = history;
    pool->config = *config;
    pool->stopFlag = false;
    pool->opsCompleted = 0;

    pthread_mutex_init(&pool->opsLock, NULL);

    return pool;
}

bool kraftLinClientPoolStart(kraftLinClientPool *pool) {
    if (!pool) {
        return false;
    }

    /* Create worker context for each client */
    for (uint32_t i = 0; i < pool->clientCount; i++) {
        kraftLinClientWorker *worker = calloc(1, sizeof(*worker));
        if (!worker) {
            return false;
        }

        worker->clientId = i + 1; /* 1-based client IDs */
        worker->pool = pool;
        worker->clientSeq = 1;
        worker->sockfd = -1;

        if (pthread_create(&pool->threads[i], NULL, clientWorkerThread,
                           worker) != 0) {
            free(worker);
            return false;
        }
    }

    return true;
}

void kraftLinClientPoolStop(kraftLinClientPool *pool) {
    if (!pool) {
        return;
    }

    /* Signal threads to stop */
    pool->stopFlag = true;

    /* Wait for all threads */
    for (uint32_t i = 0; i < pool->clientCount; i++) {
        if (pool->threads[i]) {
            pthread_join(pool->threads[i], NULL);
        }
    }
}

void kraftLinClientPoolDestroy(kraftLinClientPool *pool) {
    if (!pool) {
        return;
    }

    /* Ensure threads are stopped */
    kraftLinClientPoolStop(pool);

    pthread_mutex_destroy(&pool->opsLock);
    free(pool->threads);
    free(pool);
}

uint64_t kraftLinClientPoolGetOpsCompleted(kraftLinClientPool *pool) {
    if (!pool) {
        return 0;
    }

    pthread_mutex_lock(&pool->opsLock);
    uint64_t completed = pool->opsCompleted;
    pthread_mutex_unlock(&pool->opsLock);

    return completed;
}
