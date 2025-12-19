/* Kraft Client API Implementation
 * =================================
 *
 * Implementation of the client-facing API for command submission.
 */

#include "kraftClientAPI.h"
#include "kraftAdapter.h"
#include "kraftProtocol.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

/* =========================================================================
 * Internal Helpers
 * ========================================================================= */

static uint64_t nowUs(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000 + (uint64_t)tv.tv_usec;
}

static uint64_t generateClientId(void) {
    /* Simple client ID generation using time + random component */
    uint64_t id = nowUs();
    /* Mix in some entropy if available */
#ifdef __APPLE__
    id ^= (uint64_t)arc4random() << 32;
    id ^= (uint64_t)arc4random();
#else
    id ^= (uint64_t)rand() << 32;
    id ^= (uint64_t)rand();
#endif
    return id;
}

/* =========================================================================
 * Status String Conversion
 * ========================================================================= */

const char *kraftClientStatusString(kraftClientStatus status) {
    switch (status) {
    case KRAFT_CLIENT_OK:
        return "OK";
    case KRAFT_CLIENT_ERROR:
        return "ERROR";
    case KRAFT_CLIENT_TIMEOUT:
        return "TIMEOUT";
    case KRAFT_CLIENT_NOT_LEADER:
        return "NOT_LEADER";
    case KRAFT_CLIENT_NO_LEADER:
        return "NO_LEADER";
    case KRAFT_CLIENT_DISCONNECTED:
        return "DISCONNECTED";
    case KRAFT_CLIENT_REJECTED:
        return "REJECTED";
    case KRAFT_CLIENT_INVALID:
        return "INVALID";
    case KRAFT_CLIENT_SHUTDOWN:
        return "SHUTDOWN";
    default:
        return "UNKNOWN";
    }
}

/* =========================================================================
 * Configuration
 * ========================================================================= */

void kraftClientConfigDefault(kraftClientConfig *config) {
    if (!config) {
        return;
    }

    memset(config, 0, sizeof(*config));

    config->connectTimeoutMs = KRAFT_CLIENT_DEFAULT_CONNECT_TIMEOUT_MS;
    config->requestTimeoutMs = KRAFT_CLIENT_DEFAULT_REQUEST_TIMEOUT_MS;
    config->sessionTimeoutMs = KRAFT_CLIENT_DEFAULT_SESSION_TIMEOUT_MS;

    config->maxRetries = KRAFT_CLIENT_DEFAULT_MAX_RETRIES;
    config->retryBackoffMs = KRAFT_CLIENT_DEFAULT_RETRY_BACKOFF_MS;
    config->retryBackoffMaxMs = KRAFT_CLIENT_DEFAULT_RETRY_BACKOFF_MAX_MS;
    config->retryBackoffMultiplier = KRAFT_CLIENT_DEFAULT_RETRY_MULTIPLIER;

    config->maxPendingRequests = KRAFT_CLIENT_DEFAULT_MAX_PENDING;
    config->connectionPoolSize = KRAFT_CLIENT_DEFAULT_POOL_SIZE;
    config->autoReconnect = true;
    config->followRedirects = true;

    config->enableBatching = false;
    config->batchMaxSize = KRAFT_CLIENT_DEFAULT_BATCH_SIZE;
    config->batchDelayMs = KRAFT_CLIENT_DEFAULT_BATCH_DELAY_MS;

    config->enableSession = true;
    config->clientId = 0; /* Auto-generate */
}

/* =========================================================================
 * Client Lifecycle
 * ========================================================================= */

kraftClientSession *kraftClientCreate(struct kraftAdapter *adapter,
                                      struct kraftProtocol *protocol,
                                      const kraftClientConfig *config) {
    if (!adapter || !protocol) {
        return NULL;
    }

    kraftClientSession *session = calloc(1, sizeof(*session));
    if (!session) {
        return NULL;
    }

    session->adapter = adapter;
    session->protocol = protocol;

    /* Apply configuration */
    if (config) {
        session->config = *config;
    } else {
        kraftClientConfigDefault(&session->config);
    }

    /* Generate client ID if not specified */
    if (session->config.clientId == 0) {
        session->clientId = generateClientId();
    } else {
        session->clientId = session->config.clientId;
    }

    session->nextSeq = 1;
    session->connected = false;

    return session;
}

bool kraftClientSetCluster(kraftClientSession *session,
                           const kraftClusterConfig *cluster) {
    if (!session || !cluster) {
        return false;
    }

    /* Free existing endpoints if any */
    if (session->cluster.endpoints) {
        free(session->cluster.endpoints);
    }

    /* Copy cluster configuration */
    session->cluster.clusterId = cluster->clusterId;
    session->cluster.endpointCount = cluster->endpointCount;

    if (cluster->endpointCount > 0 && cluster->endpoints) {
        size_t size = cluster->endpointCount * sizeof(kraftClusterEndpoint);
        session->cluster.endpoints = malloc(size);
        if (!session->cluster.endpoints) {
            session->cluster.endpointCount = 0;
            return false;
        }
        memcpy(session->cluster.endpoints, cluster->endpoints, size);
    } else {
        session->cluster.endpoints = NULL;
    }

    return true;
}

bool kraftClientAddEndpoint(kraftClientSession *session, const char *address,
                            uint16_t port) {
    if (!session || !address) {
        return false;
    }

    uint32_t newCount = session->cluster.endpointCount + 1;
    kraftClusterEndpoint *newEndpoints = realloc(
        session->cluster.endpoints, newCount * sizeof(kraftClusterEndpoint));

    if (!newEndpoints) {
        return false;
    }

    session->cluster.endpoints = newEndpoints;
    kraftClusterEndpoint *ep =
        &session->cluster.endpoints[session->cluster.endpointCount];

    memset(ep, 0, sizeof(*ep));
    strncpy(ep->address, address, sizeof(ep->address) - 1);
    ep->port = port;
    ep->nodeId = 0;
    ep->isLeader = false;

    session->cluster.endpointCount = newCount;
    return true;
}

void kraftClientSetCallbacks(kraftClientSession *session,
                             kraftClientConnectFn onConnect,
                             kraftClientLeaderChangeFn onLeaderChange,
                             void *userData) {
    if (!session) {
        return;
    }

    session->onConnect = onConnect;
    session->onLeaderChange = onLeaderChange;
    session->callbackUserData = userData;
}

bool kraftClientConnect(kraftClientSession *session) {
    if (!session || !session->adapter) {
        return false;
    }

    if (session->cluster.endpointCount == 0) {
        return false;
    }

    /* Try to connect to first endpoint (leader discovery will follow) */
    kraftClusterEndpoint *ep = &session->cluster.endpoints[0];

    /* Use adapter to initiate connection */
    if (!session->adapter->ops || !session->adapter->ops->connect) {
        return false;
    }

    kraftConnection *conn = session->adapter->ops->connect(
        session->adapter, ep->address, ep->port, KRAFT_CONN_CLIENT);

    if (!conn) {
        return false;
    }

    /* Store connection in impl for now */
    session->impl = conn;

    return true;
}

void kraftClientDisconnect(kraftClientSession *session) {
    if (!session) {
        return;
    }

    /* Cancel all pending requests */
    kraftClientCancelAll(session);

    /* Close connection */
    if (session->impl && session->adapter && session->adapter->ops) {
        if (session->adapter->ops->close) {
            session->adapter->ops->close(session->adapter, session->impl);
        }
    }

    session->impl = NULL;
    session->connected = false;
}

void kraftClientDestroy(kraftClientSession *session) {
    if (!session) {
        return;
    }

    kraftClientDisconnect(session);

    /* Free cluster configuration */
    free(session->cluster.endpoints);

    /* Free any pending requests */
    kraftClientRequest *req = session->pendingHead;
    while (req) {
        kraftClientRequest *next = req->next;
        free(req);
        req = next;
    }

    free(session);
}

/* =========================================================================
 * Request Management
 * ========================================================================= */

static kraftClientRequest *allocateRequest(kraftClientSession *session) {
    if (!session) {
        return NULL;
    }

    if (session->pendingCount >= session->config.maxPendingRequests) {
        return NULL; /* Too many pending requests */
    }

    kraftClientRequest *req = calloc(1, sizeof(*req));
    if (!req) {
        return NULL;
    }

    req->requestId = session->nextSeq++;
    req->clientSeq = req->requestId;
    req->submitTimeUs = nowUs();
    req->timeoutUs =
        req->submitTimeUs + (session->config.requestTimeoutMs * 1000);
    req->retryCount = 0;
    req->status = KRAFT_CLIENT_OK;

    /* Add to pending list */
    req->prev = session->pendingTail;
    req->next = NULL;

    if (session->pendingTail) {
        session->pendingTail->next = req;
    } else {
        session->pendingHead = req;
    }
    session->pendingTail = req;
    session->pendingCount++;

    return req;
}

static void freeRequest(kraftClientSession *session, kraftClientRequest *req) {
    if (!session || !req) {
        return;
    }

    /* Remove from pending list */
    if (req->prev) {
        req->prev->next = req->next;
    } else {
        session->pendingHead = req->next;
    }

    if (req->next) {
        req->next->prev = req->prev;
    } else {
        session->pendingTail = req->prev;
    }

    session->pendingCount--;
    free(req);
}

/* =========================================================================
 * Command Submission
 * ========================================================================= */

kraftClientRequest *kraftClientSubmit(kraftClientSession *session,
                                      const uint8_t *command, size_t commandLen,
                                      kraftClientResponseFn callback,
                                      void *userData) {
    return kraftClientSubmitEx(session, 0, command, commandLen, 0, callback,
                               userData);
}

kraftClientRequest *kraftClientSubmitEx(kraftClientSession *session,
                                        uint32_t commandType,
                                        const uint8_t *command,
                                        size_t commandLen, uint64_t timeoutMs,
                                        kraftClientResponseFn callback,
                                        void *userData) {
    if (!session || !command || commandLen == 0) {
        return NULL;
    }

    if (!session->connected && !session->impl) {
        return NULL;
    }

    kraftClientRequest *req = allocateRequest(session);
    if (!req) {
        return NULL;
    }

    req->userData = userData;

    /* Override timeout if specified */
    if (timeoutMs > 0) {
        req->timeoutUs = req->submitTimeUs + (timeoutMs * 1000);
    }

    /* Encode and send the request */
    if (session->protocol && session->protocol->ops) {
        kraftEncodeBuffer buf;
        if (kraftEncodeBufferInit(&buf, 256 + commandLen)) {
            kraftMsgClientRequest msg = {.clientId = session->clientId,
                                         .requestSeq = req->clientSeq,
                                         .commandType = commandType};

            if (session->protocol->ops->encodeClientRequest) {
                if (session->protocol->ops->encodeClientRequest(
                        session->protocol, &buf, &msg, command, commandLen)) {
                    /* Send via adapter */
                    if (session->adapter && session->adapter->ops &&
                        session->adapter->ops->write && session->impl) {
                        session->adapter->ops->write(
                            session->adapter, session->impl, buf.data, buf.len);
                        session->stats.requestsSent++;
                    }
                }
            }
            kraftEncodeBufferFree(&buf);
        }
    }

    /* TODO: Store callback for later invocation */
    (void)callback;

    return req;
}

kraftClientStatus kraftClientSubmitSync(kraftClientSession *session,
                                        const uint8_t *command,
                                        size_t commandLen,
                                        kraftClientResponse *response,
                                        uint64_t timeoutMs) {
    if (!session || !command || !response) {
        return KRAFT_CLIENT_INVALID;
    }

    /* For sync operation, we submit and poll until complete */
    kraftClientRequest *req = kraftClientSubmitEx(
        session, 0, command, commandLen, timeoutMs, NULL, NULL);

    if (!req) {
        return KRAFT_CLIENT_ERROR;
    }

    /* Poll until response or timeout */
    uint64_t deadline =
        nowUs() + (timeoutMs ? timeoutMs * 1000
                             : session->config.requestTimeoutMs * 1000);

    while (nowUs() < deadline) {
        int events = kraftClientPoll(session, 10);
        if (events < 0) {
            break;
        }

        /* Check if our request completed */
        if (req->status != KRAFT_CLIENT_OK || req->retryCount > 0) {
            /* Response received or error occurred */
            break;
        }
    }

    kraftClientStatus status = req->status;

    /* Fill in response if we got one */
    memset(response, 0, sizeof(*response));
    response->status = status;
    response->requestId = req->requestId;
    response->latencyUs = nowUs() - req->submitTimeUs;

    freeRequest(session, req);

    return status;
}

bool kraftClientCancel(kraftClientSession *session,
                       kraftClientRequest *request) {
    if (!session || !request) {
        return false;
    }

    request->status = KRAFT_CLIENT_SHUTDOWN;
    freeRequest(session, request);
    return true;
}

void kraftClientCancelAll(kraftClientSession *session) {
    if (!session) {
        return;
    }

    kraftClientRequest *req = session->pendingHead;
    while (req) {
        kraftClientRequest *next = req->next;
        req->status = KRAFT_CLIENT_SHUTDOWN;
        free(req);
        req = next;
    }

    session->pendingHead = NULL;
    session->pendingTail = NULL;
    session->pendingCount = 0;
}

/* =========================================================================
 * Linearizable Reads
 * ========================================================================= */

kraftClientRequest *kraftClientRead(kraftClientSession *session,
                                    const uint8_t *key, size_t keyLen,
                                    kraftClientReadFn callback,
                                    void *userData) {
    if (!session || !key) {
        return NULL;
    }

    kraftClientRequest *req = allocateRequest(session);
    if (!req) {
        return NULL;
    }

    req->userData = userData;

    /* Encode ReadIndex request */
    if (session->protocol && session->protocol->ops &&
        session->protocol->ops->encodeReadIndex) {
        kraftEncodeBuffer buf;
        if (kraftEncodeBufferInit(&buf, 128 + keyLen)) {
            kraftMsgReadIndex msg = {.clientId = session->clientId,
                                     .requestId = req->requestId};

            if (session->protocol->ops->encodeReadIndex(session->protocol, &buf,
                                                        &msg, key, keyLen)) {
                if (session->adapter && session->adapter->ops &&
                    session->adapter->ops->write && session->impl) {
                    session->adapter->ops->write(
                        session->adapter, session->impl, buf.data, buf.len);
                }
            }
            kraftEncodeBufferFree(&buf);
        }
    }

    (void)callback;
    return req;
}

kraftClientStatus kraftClientReadSync(kraftClientSession *session,
                                      const uint8_t *key, size_t keyLen,
                                      uint8_t **value, size_t *valueLen,
                                      uint64_t timeoutMs) {
    if (!session || !key || !value || !valueLen) {
        return KRAFT_CLIENT_INVALID;
    }

    *value = NULL;
    *valueLen = 0;

    kraftClientRequest *req = kraftClientRead(session, key, keyLen, NULL, NULL);
    if (!req) {
        return KRAFT_CLIENT_ERROR;
    }

    /* Poll until response */
    uint64_t deadline =
        nowUs() + (timeoutMs ? timeoutMs * 1000
                             : session->config.requestTimeoutMs * 1000);

    while (nowUs() < deadline) {
        int events = kraftClientPoll(session, 10);
        if (events < 0) {
            break;
        }

        if (req->status != KRAFT_CLIENT_OK) {
            break;
        }
    }

    kraftClientStatus status = req->status;
    freeRequest(session, req);
    return status;
}

/* =========================================================================
 * Batching Support
 * ========================================================================= */

bool kraftClientBatchBegin(kraftClientSession *session) {
    if (!session) {
        return false;
    }

    /* TODO: Implement batching - queue requests instead of sending */
    return true;
}

bool kraftClientBatchEnd(kraftClientSession *session) {
    if (!session) {
        return false;
    }

    /* TODO: Send all queued requests as batch */
    return true;
}

void kraftClientBatchDiscard(kraftClientSession *session) {
    if (!session) {
        return;
    }

    /* TODO: Discard queued batch */
}

/* =========================================================================
 * Leader Discovery
 * ========================================================================= */

bool kraftClientGetLeader(kraftClientSession *session, uint64_t *nodeId,
                          char *address, size_t addressLen, uint16_t *port) {
    if (!session) {
        return false;
    }

    if (session->knownLeaderId == 0) {
        return false;
    }

    if (nodeId) {
        *nodeId = session->knownLeaderId;
    }

    if (address && addressLen > 0) {
        strncpy(address, session->knownLeaderAddress, addressLen - 1);
        address[addressLen - 1] = '\0';
    }

    if (port) {
        *port = session->knownLeaderPort;
    }

    return true;
}

bool kraftClientRefreshLeader(kraftClientSession *session) {
    if (!session) {
        return false;
    }

    /* TODO: Send leader discovery request */
    return true;
}

bool kraftClientIsConnectedToLeader(kraftClientSession *session) {
    if (!session || !session->connected) {
        return false;
    }

    /* TODO: Check if current connection is to leader */
    return session->knownLeaderId != 0;
}

/* =========================================================================
 * Connection Status
 * ========================================================================= */

bool kraftClientIsConnected(kraftClientSession *session) {
    return session && session->connected;
}

uint32_t kraftClientPendingCount(kraftClientSession *session) {
    return session ? session->pendingCount : 0;
}

void kraftClientGetLatencyStats(kraftClientSession *session, uint64_t *minUs,
                                uint64_t *maxUs, uint64_t *avgUs,
                                uint64_t *p99Us) {
    if (!session) {
        return;
    }

    /* TODO: Track detailed latency histogram */
    if (avgUs && session->stats.responsesReceived > 0) {
        *avgUs =
            session->stats.totalLatencyUs / session->stats.responsesReceived;
    }

    (void)minUs;
    (void)maxUs;
    (void)p99Us;
}

/* =========================================================================
 * Statistics
 * ========================================================================= */

void kraftClientGetStats(const kraftClientSession *session,
                         kraftClientStats *stats) {
    if (!session || !stats) {
        return;
    }

    memset(stats, 0, sizeof(*stats));

    stats->requestsSent = session->stats.requestsSent;
    stats->responsesReceived = session->stats.responsesReceived;
    stats->timeouts = session->stats.timeouts;
    stats->retries = session->stats.retries;
    stats->redirects = session->stats.redirects;
    stats->errors = session->stats.errors;
    stats->pendingRequests = session->pendingCount;
    stats->connected = session->connected;
    stats->knownLeaderId = session->knownLeaderId;

    if (session->stats.responsesReceived > 0) {
        stats->avgLatencyUs =
            session->stats.totalLatencyUs / session->stats.responsesReceived;
    }
}

int kraftClientFormatStats(const kraftClientStats *stats, char *buf,
                           size_t bufLen) {
    if (!stats || !buf || bufLen == 0) {
        return 0;
    }

    return snprintf(buf, bufLen,
                    "KraftClient Stats:\n"
                    "  Requests sent:     %" PRIu64 "\n"
                    "  Responses recv:    %" PRIu64 "\n"
                    "  Timeouts:          %" PRIu64 "\n"
                    "  Retries:           %" PRIu64 "\n"
                    "  Redirects:         %" PRIu64 "\n"
                    "  Errors:            %" PRIu64 "\n"
                    "  Pending:           %" PRIu64 "\n"
                    "  Avg latency:       %" PRIu64 " us\n"
                    "  Connected:         %s\n"
                    "  Leader:            %" PRIu64 "\n",
                    stats->requestsSent, stats->responsesReceived,
                    stats->timeouts, stats->retries, stats->redirects,
                    stats->errors, stats->pendingRequests, stats->avgLatencyUs,
                    stats->connected ? "yes" : "no", stats->knownLeaderId);
}

void kraftClientResetStats(kraftClientSession *session) {
    if (!session) {
        return;
    }

    memset(&session->stats, 0, sizeof(session->stats));
}

/* =========================================================================
 * Event Loop Integration
 * ========================================================================= */

int kraftClientPoll(kraftClientSession *session, uint64_t timeoutMs) {
    if (!session || !session->adapter || !session->adapter->ops) {
        return -1;
    }

    if (!session->adapter->ops->poll) {
        return -1;
    }

    /* Check for timed out requests */
    uint64_t now = nowUs();
    kraftClientRequest *req = session->pendingHead;
    while (req) {
        kraftClientRequest *next = req->next;
        if (now >= req->timeoutUs) {
            req->status = KRAFT_CLIENT_TIMEOUT;
            session->stats.timeouts++;
            /* TODO: Invoke callback */
        }
        req = next;
    }

    return session->adapter->ops->poll(session->adapter, timeoutMs);
}

void kraftClientRun(kraftClientSession *session) {
    if (!session || !session->adapter || !session->adapter->ops) {
        return;
    }

    if (session->adapter->ops->run) {
        session->adapter->ops->run(session->adapter);
    }
}

void kraftClientStop(kraftClientSession *session) {
    if (!session || !session->adapter || !session->adapter->ops) {
        return;
    }

    if (session->adapter->ops->stop) {
        session->adapter->ops->stop(session->adapter);
    }
}

/* =========================================================================
 * Multi-Raft Support
 * ========================================================================= */

kraftClientRequest *
kraftClientSubmitToGroup(kraftClientSession *session, uint32_t groupId,
                         const uint8_t *command, size_t commandLen,
                         kraftClientResponseFn callback, void *userData) {
    /* For multi-raft, we need to tag the request with groupId */
    /* TODO: Implement group-aware routing */
    (void)groupId;

    return kraftClientSubmit(session, command, commandLen, callback, userData);
}

bool kraftClientGetGroupLeader(kraftClientSession *session, uint32_t groupId,
                               uint64_t *nodeId, char *address,
                               size_t addressLen, uint16_t *port) {
    /* TODO: Track per-group leader information */
    (void)groupId;

    return kraftClientGetLeader(session, nodeId, address, addressLen, port);
}

/* =========================================================================
 * Request Utilities
 * ========================================================================= */

kraftClientRequest *kraftClientGetRequest(kraftClientSession *session,
                                          uint64_t requestId) {
    if (!session) {
        return NULL;
    }

    kraftClientRequest *req = session->pendingHead;
    while (req) {
        if (req->requestId == requestId) {
            return req;
        }
        req = req->next;
    }

    return NULL;
}

uint64_t kraftClientRequestElapsed(const kraftClientRequest *request) {
    if (!request) {
        return 0;
    }

    return nowUs() - request->submitTimeUs;
}

bool kraftClientRequestTimedOut(const kraftClientRequest *request) {
    if (!request) {
        return false;
    }

    return nowUs() >= request->timeoutUs;
}

/* =========================================================================
 * Encode Buffer Helpers
 * ========================================================================= */

bool kraftEncodeBufferInit(kraftEncodeBuffer *buf, size_t capacity) {
    if (!buf) {
        return false;
    }

    buf->data = malloc(capacity);
    if (!buf->data) {
        return false;
    }

    buf->capacity = capacity;
    buf->len = 0;
    buf->owned = true;

    return true;
}

void kraftEncodeBufferReset(kraftEncodeBuffer *buf) {
    if (buf) {
        buf->len = 0;
    }
}

void kraftEncodeBufferFree(kraftEncodeBuffer *buf) {
    if (buf && buf->owned) {
        free(buf->data);
    }

    if (buf) {
        buf->data = NULL;
        buf->len = 0;
        buf->capacity = 0;
    }
}
