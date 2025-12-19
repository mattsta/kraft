/* loopyClientAPI - Client API implementation
 *
 * TCP listener for external client connections with text protocol.
 */

#include "loopyClientAPI.h"
#include "loopyKV.h"
#include "loopyPlatform.h"
#include "loopyServer.h"

/* loopy includes */
#include "../../deps/loopy/src/loopy.h"
#include "../../deps/loopy/src/loopyStream.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Constants
 * ============================================================================
 */

/* Initial buffer size for client reads */
#define CLIENT_BUFFER_INITIAL_SIZE 4096
#define CLIENT_BUFFER_MAX_SIZE 65536 /* Maximum buffer size (64KB) */
#define CLIENT_MAX_LINE_LENGTH 65536 /* Maximum single command line length */

/* ============================================================================
 * Internal Structures
 * ============================================================================
 */

/**
 * Pending command request - tracks async command execution.
 */
typedef struct ClientRequest {
    loopyClientConn *conn;      /* Connection to respond to */
    uint32_t connId;            /* Connection ID at time of request */
    struct ClientRequest *next; /* For free list */
} ClientRequest;

/**
 * Client connection.
 *
 * Each client connection maintains a dynamically-sized read buffer that grows
 * as needed to accommodate large commands. The buffer starts at
 * CLIENT_BUFFER_INITIAL_SIZE and can grow up to CLIENT_BUFFER_MAX_SIZE.
 */
struct loopyClientConn {
    loopyClientAPI *api; /* Parent API */
    loopyStream *stream; /* TCP stream */
    uint32_t clientId;   /* Unique client ID */

    /* Read buffer for line accumulation (dynamic) */
    char *buffer;          /* Dynamically allocated buffer */
    size_t bufferLen;      /* Current data length in buffer */
    size_t bufferCapacity; /* Current buffer capacity */

    /* Connection state */
    bool closing; /* Close in progress */

    /* Linked list */
    struct loopyClientConn *next;
    struct loopyClientConn *prev;
};

/**
 * Client API.
 */
struct loopyClientAPI {
    loopyLoop *loop;       /* Event loop */
    loopyServer *server;   /* For command submission */
    loopyKV *kv;           /* For local reads */
    loopyStream *listener; /* TCP server */
    int port;              /* Listening port */

    /* Active connections (doubly-linked list) */
    loopyClientConn *clients;
    uint32_t clientCount;
    uint32_t nextClientId;

    /* State */
    bool listening;

    /* Statistics */
    loopyClientAPIStats stats;
};

/* ============================================================================
 * Forward Declarations
 * ============================================================================
 */

static void connectionCallback(loopyStream *server, int status, void *userData);
static void allocCallback(loopyStream *stream, size_t suggested, void **buf,
                          size_t *bufLen, void *userData);
static void readCallback(loopyStream *stream, ssize_t nread, const void *buf,
                         void *userData);
static void closeCallback(loopyStream *stream, void *userData);

static void processLines(loopyClientConn *conn);
static void processCommand(loopyClientConn *conn, const char *line, size_t len);
static void commandCallback(loopyServer *server, uint64_t index, bool success,
                            const void *result, size_t resultLen,
                            void *userData);

static void sendLine(loopyClientConn *conn, const char *line);
static void sendOK(loopyClientConn *conn);
static void sendError(loopyClientConn *conn, const char *msg);
static void sendBulk(loopyClientConn *conn, const char *data, size_t len);
static void sendInteger(loopyClientConn *conn, int64_t val);
static void sendRedirect(loopyClientConn *conn);
static void sendNull(loopyClientConn *conn);

static void removeClient(loopyClientConn *conn);
static bool isWriteCommand(const char *line, size_t len);
static bool isAdminCommand(const char *line, size_t len);
static bool handleAdminCommand(loopyClientConn *conn, const char *line,
                               size_t len);

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

loopyClientAPI *loopyClientAPICreate(loopyLoop *loop, int port,
                                     loopyServer *server, loopyKV *kv) {
    if (!loop || port <= 0 || !server) {
        return NULL;
    }

    loopyClientAPI *api = calloc(1, sizeof(*api));
    if (!api) {
        return NULL;
    }

    api->loop = loop;
    api->server = server;
    api->kv = kv;
    api->port = port;
    api->nextClientId = 1;

    return api;
}

/**
 * Destroy a client API instance.
 *
 * Closes the listener, disconnects all clients, and frees all resources.
 */
void loopyClientAPIDestroy(loopyClientAPI *api) {
    if (!api) {
        return;
    }

    /* Close listener */
    if (api->listener) {
        loopyStreamClose(api->listener, NULL, NULL);
        api->listener = NULL;
    }

    /* Close all client connections and free resources */
    loopyClientConn *conn = api->clients;
    while (conn) {
        loopyClientConn *next = conn->next;
        if (conn->stream) {
            loopyStreamClose(conn->stream, NULL, NULL);
        }
        free(conn->buffer); /* Free dynamic read buffer */
        free(conn);
        conn = next;
    }

    free(api);
}

int loopyClientAPIStart(loopyClientAPI *api) {
    if (!api || api->listening) {
        return -1;
    }

    /* Create TCP stream */
    api->listener = loopyStreamNewTcp(api->loop);
    if (!api->listener) {
        return -1;
    }

    /* Bind to port */
    if (!loopyStreamBind(api->listener, "0.0.0.0", api->port)) {
        loopyStreamClose(api->listener, NULL, NULL);
        api->listener = NULL;
        return -1;
    }

    /* Start listening */
    if (!loopyStreamListen(api->listener, 128, connectionCallback, api)) {
        loopyStreamClose(api->listener, NULL, NULL);
        api->listener = NULL;
        return -1;
    }

    api->listening = true;
    return 0;
}

void loopyClientAPIStop(loopyClientAPI *api) {
    if (!api || !api->listening) {
        return;
    }

    if (api->listener) {
        loopyStreamClose(api->listener, NULL, NULL);
        api->listener = NULL;
    }

    api->listening = false;
}

/* ============================================================================
 * Properties
 * ============================================================================
 */

void loopyClientAPIGetStats(const loopyClientAPI *api,
                            loopyClientAPIStats *stats) {
    if (!stats) {
        return;
    }

    if (!api) {
        memset(stats, 0, sizeof(*stats));
        return;
    }

    *stats = api->stats;
    stats->connectionsActive = api->clientCount;
}

int loopyClientAPIGetPort(const loopyClientAPI *api) {
    return api ? api->port : 0;
}

size_t loopyClientAPIGetConnectionCount(const loopyClientAPI *api) {
    return api ? api->clientCount : 0;
}

bool loopyClientAPIIsListening(const loopyClientAPI *api) {
    return api ? api->listening : false;
}

/* ============================================================================
 * Connection Management
 * ============================================================================
 */

static void connectionCallback(loopyStream *server, int status,
                               void *userData) {
    loopyClientAPI *api = userData;
    (void)server;

    if (status < 0) {
        return;
    }

    /* Accept the connection */
    loopyStream *clientStream = loopyStreamAccept(api->listener);
    if (!clientStream) {
        return;
    }

    /* Create client connection */
    loopyClientConn *conn = calloc(1, sizeof(*conn));
    if (!conn) {
        loopyStreamClose(clientStream, NULL, NULL);
        return;
    }

    /* Allocate initial read buffer */
    conn->buffer = malloc(CLIENT_BUFFER_INITIAL_SIZE);
    if (!conn->buffer) {
        free(conn);
        loopyStreamClose(clientStream, NULL, NULL);
        return;
    }
    conn->bufferCapacity = CLIENT_BUFFER_INITIAL_SIZE;
    conn->bufferLen = 0;

    conn->api = api;
    conn->stream = clientStream;
    conn->clientId = api->nextClientId++;

    /* Add to list */
    conn->next = api->clients;
    if (api->clients) {
        api->clients->prev = conn;
    }
    api->clients = conn;
    api->clientCount++;

    /* Start reading */
    if (!loopyStreamReadStart(clientStream, allocCallback, readCallback,
                              conn)) {
        removeClient(conn);
        return;
    }

    api->stats.connectionsAccepted++;
}

/**
 * Alloc callback for stream reads.
 *
 * Provides buffer space for incoming data. If the current buffer is full,
 * attempts to grow it up to CLIENT_BUFFER_MAX_SIZE. Returns NULL if the
 * buffer cannot be grown further (client will be disconnected).
 */
static void allocCallback(loopyStream *stream, size_t suggested, void **buf,
                          size_t *bufLen, void *userData) {
    loopyClientConn *conn = userData;
    (void)stream;
    (void)suggested;

    /* Calculate remaining space in buffer */
    size_t remaining = conn->bufferCapacity - conn->bufferLen;

    /* If buffer is nearly full, try to grow it */
    if (remaining < 256 && conn->bufferCapacity < CLIENT_BUFFER_MAX_SIZE) {
        size_t newCapacity = conn->bufferCapacity * 2;
        if (newCapacity > CLIENT_BUFFER_MAX_SIZE) {
            newCapacity = CLIENT_BUFFER_MAX_SIZE;
        }

        char *newBuffer = realloc(conn->buffer, newCapacity);
        if (newBuffer) {
            conn->buffer = newBuffer;
            conn->bufferCapacity = newCapacity;
            remaining = newCapacity - conn->bufferLen;
        }
    }

    if (remaining == 0) {
        /* Buffer at maximum size and full - cannot accept more data */
        *buf = NULL;
        *bufLen = 0;
        return;
    }

    *buf = conn->buffer + conn->bufferLen;
    *bufLen = remaining;
}

static void readCallback(loopyStream *stream, ssize_t nread, const void *buf,
                         void *userData) {
    loopyClientConn *conn = userData;
    (void)stream;
    (void)buf;

    if (nread <= 0) {
        /* Client disconnected or error */
        removeClient(conn);
        return;
    }

    conn->bufferLen += (size_t)nread;
    conn->api->stats.bytesReceived += (size_t)nread;

    /* Process complete lines */
    processLines(conn);
}

/**
 * Close callback for client connections.
 *
 * Cleans up connection resources including the dynamic read buffer.
 */
static void closeCallback(loopyStream *stream, void *userData) {
    loopyClientConn *conn = userData;
    (void)stream;

    /* Remove from list */
    if (conn->prev) {
        conn->prev->next = conn->next;
    } else {
        conn->api->clients = conn->next;
    }
    if (conn->next) {
        conn->next->prev = conn->prev;
    }
    conn->api->clientCount--;

    /* Free dynamic buffer */
    free(conn->buffer);
    free(conn);
}

static void removeClient(loopyClientConn *conn) {
    if (!conn || conn->closing) {
        return;
    }

    conn->closing = true;
    loopyStreamClose(conn->stream, closeCallback, conn);
}

/* ============================================================================
 * Command Processing
 * ============================================================================
 */

/**
 * Process complete lines from the client buffer.
 *
 * Scans the buffer for newline-terminated commands and dispatches them
 * for processing. Handles both \n and \r\n line endings.
 */
static void processLines(loopyClientConn *conn) {
    while (conn->bufferLen > 0 && !conn->closing) {
        /* Find line ending (\r\n or \n) */
        char *lineEnd = NULL;
        size_t lineLen = 0;
        size_t skipLen = 0;

        for (size_t i = 0; i < conn->bufferLen; i++) {
            if (conn->buffer[i] == '\n') {
                lineEnd = conn->buffer + i;
                lineLen = i;
                skipLen = 1;
                /* Check for \r\n */
                if (i > 0 && conn->buffer[i - 1] == '\r') {
                    lineLen--;
                }
                break;
            }
        }

        if (!lineEnd) {
            /* No complete line yet */
            if (conn->bufferLen >= CLIENT_BUFFER_MAX_SIZE) {
                /* Buffer at maximum size with no newline - line too long */
                sendError(conn, "Line too long (max 64KB)");
                removeClient(conn);
            }
            return;
        }

        /* Process this line */
        if (lineLen > 0) {
            processCommand(conn, conn->buffer, lineLen);
        }

        /* Remove processed data from buffer */
        size_t consumed = (size_t)(lineEnd - conn->buffer) + skipLen;
        if (consumed < conn->bufferLen) {
            memmove(conn->buffer, lineEnd + skipLen,
                    conn->bufferLen - consumed);
        }
        conn->bufferLen -= consumed;
    }
}

static void processCommand(loopyClientConn *conn, const char *line,
                           size_t len) {
    loopyClientAPI *api = conn->api;
    loopyServer *server = api->server;

    api->stats.commandsReceived++;

    fprintf(stderr, "[CLIENT-API] Received command: len=%zu, first_char='%c'\n",
            len, len > 0 ? line[0] : '?');

    /* Skip empty lines */
    if (len == 0) {
        return;
    }

    /* Handle admin/cluster management commands first */
    if (isAdminCommand(line, len)) {
        fprintf(stderr, "[CLIENT-API] Detected as admin command\n");
        if (handleAdminCommand(conn, line, len)) {
            return;
        }
        /* Fall through if admin command handler returned false */
    }

    /* Check if we're leader for write commands */
    if (isWriteCommand(line, len) && !loopyServerIsLeader(server)) {
        sendRedirect(conn);
        api->stats.redirectsSent++;
        return;
    }

    /* Parse command using loopyKV */
    void *cmdData = NULL;
    size_t cmdLen = 0;

    if (!loopyKVParseCommand(line, len, &cmdData, &cmdLen)) {
        sendError(conn, "Invalid command");
        api->stats.commandsFailed++;
        return;
    }

    /* Determine command type from first byte */
    uint8_t cmdType = ((uint8_t *)cmdData)[0];

    /* Handle read commands locally */
    if (cmdType == LOOPY_KV_CMD_GET) {
        /* Extract key from command */
        if (cmdLen < 4) {
            sendError(conn, "Invalid GET command");
            free(cmdData);
            api->stats.commandsFailed++;
            return;
        }

        uint16_t keyLen =
            ((uint8_t *)cmdData)[1] | (((uint8_t *)cmdData)[2] << 8);
        const char *key = (const char *)cmdData + 3;

        if (keyLen + 3 > cmdLen) {
            sendError(conn, "Invalid GET command");
            free(cmdData);
            api->stats.commandsFailed++;
            return;
        }

        /* Execute local read */
        char *value = NULL;
        size_t valueLen = 0;

        if (loopyKVGet(api->kv, key, keyLen, &value, &valueLen)) {
            sendBulk(conn, value, valueLen);
            free(value);
        } else {
            sendNull(conn);
        }

        free(cmdData);
        api->stats.commandsSucceeded++;
        return;
    }

    /* Handle KEYS locally */
    if (cmdType == LOOPY_KV_CMD_KEYS) {
        /* For now, just send a simple response */
        /* TODO: Implement proper KEYS iteration */
        sendError(conn, "KEYS not yet implemented over client API");
        free(cmdData);
        api->stats.commandsFailed++;
        return;
    }

    /* Handle STAT locally */
    if (cmdType == LOOPY_KV_CMD_STAT) {
        loopyKVStats kvStats;
        loopyKVGetStats(api->kv, &kvStats);

        char statBuf[256];
        int statLen = snprintf(
            statBuf, sizeof(statBuf), "keys:%lu,sets:%lu,gets:%lu,hits:%lu",
            (unsigned long)kvStats.keyCount, (unsigned long)kvStats.setsTotal,
            (unsigned long)kvStats.getsTotal, (unsigned long)kvStats.hitsTotal);

        sendBulk(conn, statBuf, (size_t)statLen);
        free(cmdData);
        api->stats.commandsSucceeded++;
        return;
    }

    /* Write commands go through Raft */
    ClientRequest *req = malloc(sizeof(*req));
    if (!req) {
        sendError(conn, "Out of memory");
        free(cmdData);
        api->stats.commandsFailed++;
        return;
    }

    req->conn = conn;
    req->connId = conn->clientId;
    req->next = NULL;

    uint64_t idx =
        loopyServerSubmit(server, cmdData, cmdLen, commandCallback, req);
    free(cmdData);

    if (idx == 0) {
        /* Not leader (shouldn't happen, we checked above) */
        sendRedirect(conn);
        free(req);
        api->stats.redirectsSent++;
        return;
    }

    /* Command submitted, response will come via callback */
}

static void commandCallback(loopyServer *server, uint64_t index, bool success,
                            const void *result, size_t resultLen,
                            void *userData) {
    ClientRequest *req = userData;
    (void)server;
    (void)index;

    if (!req) {
        return;
    }

    loopyClientConn *conn = req->conn;

    /* Check if connection is still valid */
    if (!conn || conn->closing || conn->clientId != req->connId) {
        /* Client disconnected during request */
        free(req);
        return;
    }

    loopyClientAPI *api = conn->api;

    if (!success) {
        sendError(conn, "Command failed");
        api->stats.commandsFailed++;
    } else if (result && resultLen > 0) {
        /* Parse response to determine type */
        loopyKVStatus status;
        char *value = NULL;
        size_t valueLen = 0;

        if (loopyKVDecodeResponse(result, resultLen, &status, &value,
                                  &valueLen)) {
            if (status == LOOPY_KV_OK) {
                if (value && valueLen > 0) {
                    /* Check if it's a numeric response (for INCR) */
                    bool isNumeric = true;
                    for (size_t i = 0; i < valueLen && isNumeric; i++) {
                        if (i == 0 && value[i] == '-') {
                            continue;
                        }
                        if (!isdigit((unsigned char)value[i])) {
                            isNumeric = false;
                        }
                    }

                    if (isNumeric && valueLen < 20) {
                        char numBuf[24];
                        memcpy(numBuf, value, valueLen);
                        numBuf[valueLen] = '\0';
                        sendInteger(conn, strtoll(numBuf, NULL, 10));
                    } else {
                        sendBulk(conn, value, valueLen);
                    }
                } else {
                    sendOK(conn);
                }
            } else if (status == LOOPY_KV_ERR_NOTFOUND) {
                sendNull(conn);
            } else {
                sendError(conn, loopyKVStatusName(status));
            }
            free(value);
        } else {
            sendOK(conn);
        }
        api->stats.commandsSucceeded++;
    } else {
        sendOK(conn);
        api->stats.commandsSucceeded++;
    }

    free(req);
}

/* ============================================================================
 * Response Helpers
 * ============================================================================
 */

static void sendLine(loopyClientConn *conn, const char *line) {
    if (!conn || conn->closing || !conn->stream) {
        return;
    }

    size_t len = strlen(line);
    loopyStreamWrite(conn->stream, line, len, NULL, NULL);
    loopyStreamWrite(conn->stream, "\r\n", 2, NULL, NULL);

    conn->api->stats.bytesSent += len + 2;
}

static void sendOK(loopyClientConn *conn) {
    sendLine(conn, "+OK");
}

static void sendError(loopyClientConn *conn, const char *msg) {
    if (!conn || conn->closing || !conn->stream) {
        return;
    }

    char buf[256];
    snprintf(buf, sizeof(buf), "-ERR %s", msg);
    sendLine(conn, buf);
}

static void sendBulk(loopyClientConn *conn, const char *data, size_t len) {
    if (!conn || conn->closing || !conn->stream) {
        fprintf(
            stderr,
            "[CLIENT-API] sendBulk: NULL check failed (conn=%p, closing=%d)\n",
            (void *)conn, conn ? conn->closing : -1);
        return;
    }

    fprintf(stderr, "[CLIENT-API] sendBulk: sending %zu bytes\n", len);

    char header[32];
    int headerLen = snprintf(header, sizeof(header), "$%zu\r\n", len);
    loopyStreamWrite(conn->stream, header, (size_t)headerLen, NULL, NULL);
    loopyStreamWrite(conn->stream, data, len, NULL, NULL);
    loopyStreamWrite(conn->stream, "\r\n", 2, NULL, NULL);

    conn->api->stats.bytesSent += (size_t)headerLen + len + 2;
    fprintf(stderr, "[CLIENT-API] sendBulk: sent total %d bytes\n",
            headerLen + (int)len + 2);
}

static void sendInteger(loopyClientConn *conn, int64_t val) {
    if (!conn || conn->closing || !conn->stream) {
        return;
    }

    char buf[32];
    snprintf(buf, sizeof(buf), ":%lld", (long long)val);
    sendLine(conn, buf);
}

static void sendRedirect(loopyClientConn *conn) {
    if (!conn || conn->closing || !conn->stream) {
        return;
    }

    uint64_t leaderId = loopyServerGetLeaderId(conn->api->server);
    char buf[64];
    snprintf(buf, sizeof(buf), "-REDIRECT %lu", (unsigned long)leaderId);
    sendLine(conn, buf);
}

static void sendNull(loopyClientConn *conn) {
    sendLine(conn, "$-1");
}

/* ============================================================================
 * Helpers
 * ============================================================================
 */

static bool isWriteCommand(const char *line, size_t len) {
    /* Skip leading whitespace */
    while (len > 0 && isspace((unsigned char)*line)) {
        line++;
        len--;
    }

    /* Write commands: SET, DEL, INCR, APPEND */
    if (len >= 3 && strncasecmp(line, "SET", 3) == 0 &&
        (len == 3 || isspace((unsigned char)line[3]))) {
        return true;
    }
    if (len >= 3 && strncasecmp(line, "DEL", 3) == 0 &&
        (len == 3 || isspace((unsigned char)line[3]))) {
        return true;
    }
    if (len >= 4 && strncasecmp(line, "INCR", 4) == 0 &&
        (len == 4 || isspace((unsigned char)line[4]))) {
        return true;
    }
    if (len >= 6 && strncasecmp(line, "APPEND", 6) == 0 &&
        (len == 6 || isspace((unsigned char)line[6]))) {
        return true;
    }

    return false;
}

/* ============================================================================
 * Admin/Cluster Management Commands
 * ============================================================================
 */

/**
 * Check if a command is an admin/cluster command.
 */
static bool isAdminCommand(const char *line, size_t len) {
    /* Skip leading whitespace */
    while (len > 0 && isspace((unsigned char)*line)) {
        line++;
        len--;
    }

    /* Admin commands: STATUS, SNAPSHOT, TRANSFER, ADD, REMOVE */
    if (len >= 6 && strncasecmp(line, "STATUS", 6) == 0 &&
        (len == 6 || isspace((unsigned char)line[6]))) {
        return true;
    }
    if (len >= 8 && strncasecmp(line, "SNAPSHOT", 8) == 0 &&
        (len == 8 || isspace((unsigned char)line[8]))) {
        return true;
    }
    if (len >= 8 && strncasecmp(line, "TRANSFER", 8) == 0 &&
        (len == 8 || isspace((unsigned char)line[8]))) {
        return true;
    }
    if (len >= 3 && strncasecmp(line, "ADD", 3) == 0 &&
        (len == 3 || isspace((unsigned char)line[3]))) {
        return true;
    }
    if (len >= 6 && strncasecmp(line, "REMOVE", 6) == 0 &&
        (len == 6 || isspace((unsigned char)line[6]))) {
        return true;
    }
    if (len >= 4 && strncasecmp(line, "INFO", 4) == 0 &&
        (len == 4 || isspace((unsigned char)line[4]))) {
        return true;
    }

    return false;
}

/**
 * Handle admin/cluster commands.
 * Returns true if command was handled, false to fall through.
 */
static bool handleAdminCommand(loopyClientConn *conn, const char *line,
                               size_t len) {
    loopyClientAPI *api = conn->api;
    loopyServer *server = api->server;

    fprintf(stderr, "[CLIENT-API] handleAdminCommand: len=%zu, cmd='%.*s'\n",
            len, (int)len, line);

    /* Skip leading whitespace */
    while (len > 0 && isspace((unsigned char)*line)) {
        line++;
        len--;
    }

    /* STATUS - Return cluster status as bulk string */
    if (len >= 6 && strncasecmp(line, "STATUS", 6) == 0) {
        const char *roleNames[] = {"follower", "candidate", "leader"};
        loopyServerRole role = loopyServerGetRole(server);
        fprintf(stderr, "[CLIENT-API] STATUS: role=%u (%s)\n", role,
                roleNames[role]);

        loopyKVStats kvStats;
        if (api->kv) {
            loopyKVGetStats(api->kv, &kvStats);
        } else {
            memset(&kvStats, 0, sizeof(kvStats));
        }

        /* Use generous buffer size and check for truncation */
        char buf[1024];
        int bufLen = snprintf(
            buf, sizeof(buf),
            "node_id:%lu\r\n"
            "cluster_id:%lu\r\n"
            "role:%s\r\n"
            "term:%lu\r\n"
            "leader_id:%lu\r\n"
            "commit_index:%lu\r\n"
            "last_applied:%lu\r\n"
            "is_leader:%d\r\n"
            "kv_keys:%lu\r\n"
            "kv_memory:%lu",
            (unsigned long)loopyServerGetNodeId(server),
            (unsigned long)loopyServerGetClusterId(server), roleNames[role],
            (unsigned long)loopyServerGetTerm(server),
            (unsigned long)loopyServerGetLeaderId(server),
            (unsigned long)loopyServerGetCommitIndex(server),
            (unsigned long)loopyServerGetLastApplied(server),
            loopyServerIsLeader(server) ? 1 : 0,
            (unsigned long)kvStats.keyCount, (unsigned long)kvStats.memoryUsed);

        /* Handle truncation (shouldn't happen with 1KB buffer) */
        if (bufLen < 0 || (size_t)bufLen >= sizeof(buf)) {
            bufLen = (int)(sizeof(buf) - 1);
        }

        sendBulk(conn, buf, (size_t)bufLen);
        api->stats.commandsSucceeded++;
        return true;
    }

    /* INFO - Alias for STATUS */
    if (len >= 4 && strncasecmp(line, "INFO", 4) == 0) {
        /* Recursive call to STATUS handler */
        return handleAdminCommand(conn, "STATUS", 6);
    }

    /* SNAPSHOT - Trigger manual snapshot */
    if (len >= 8 && strncasecmp(line, "SNAPSHOT", 8) == 0) {
        if (!api->kv) {
            sendError(conn, "No state machine");
            api->stats.commandsFailed++;
            return true;
        }

        loopyStateMachine *sm = loopyKVGetStateMachine(api->kv);
        if (!sm || !sm->snapshot) {
            sendError(conn, "Snapshots not supported");
            api->stats.commandsFailed++;
            return true;
        }

        void *snapData = NULL;
        size_t snapLen = 0;
        uint64_t lastIndex = 0;
        uint64_t lastTerm = 0;

        if (sm->snapshot(sm, &snapData, &snapLen, &lastIndex, &lastTerm)) {
            char buf[128];
            int bufLen =
                snprintf(buf, sizeof(buf),
                         "size:%lu\r\n"
                         "last_index:%lu\r\n"
                         "last_term:%lu",
                         (unsigned long)snapLen, (unsigned long)lastIndex,
                         (unsigned long)lastTerm);

            /* Free the snapshot data (we just demonstrate it works) */
            free(snapData);

            sendBulk(conn, buf, (size_t)bufLen);
            api->stats.commandsSucceeded++;
        } else {
            sendError(conn, "Snapshot creation failed");
            api->stats.commandsFailed++;
        }
        return true;
    }

    /* TRANSFER <node-id> - Transfer leadership */
    if (len >= 8 && strncasecmp(line, "TRANSFER", 8) == 0) {
        /* Must be leader to transfer */
        if (!loopyServerIsLeader(server)) {
            sendRedirect(conn);
            api->stats.redirectsSent++;
            return true;
        }

        /* Parse node ID */
        const char *arg = line + 8;
        size_t argLen = len - 8;
        while (argLen > 0 && isspace((unsigned char)*arg)) {
            arg++;
            argLen--;
        }

        if (argLen == 0) {
            sendError(conn, "Usage: TRANSFER <node-id>");
            api->stats.commandsFailed++;
            return true;
        }

        char nodeIdStr[32];
        size_t i = 0;
        while (i < argLen && i < sizeof(nodeIdStr) - 1 &&
               isdigit((unsigned char)arg[i])) {
            nodeIdStr[i] = arg[i];
            i++;
        }
        nodeIdStr[i] = '\0';

        uint64_t targetNodeId = strtoull(nodeIdStr, NULL, 10);
        if (targetNodeId == 0) {
            sendError(conn, "Invalid node ID");
            api->stats.commandsFailed++;
            return true;
        }

        if (loopyServerTransferLeadership(server, targetNodeId)) {
            sendOK(conn);
            api->stats.commandsSucceeded++;
        } else {
            sendError(conn, "Leadership transfer failed");
            api->stats.commandsFailed++;
        }
        return true;
    }

    /* ADD <node-id> <addr:port> - Add node to cluster */
    if (len >= 3 && strncasecmp(line, "ADD", 3) == 0) {
        /* Must be leader to add nodes */
        if (!loopyServerIsLeader(server)) {
            sendRedirect(conn);
            api->stats.redirectsSent++;
            return true;
        }

        /* Parse: ADD <node-id> <addr:port> */
        const char *arg = line + 3;
        size_t argLen = len - 3;
        while (argLen > 0 && isspace((unsigned char)*arg)) {
            arg++;
            argLen--;
        }

        if (argLen == 0) {
            sendError(conn, "Usage: ADD <node-id> <addr:port>");
            api->stats.commandsFailed++;
            return true;
        }

        /* Parse node ID */
        char nodeIdStr[32];
        size_t i = 0;
        while (i < argLen && i < sizeof(nodeIdStr) - 1 &&
               isdigit((unsigned char)arg[i])) {
            nodeIdStr[i] = arg[i];
            i++;
        }
        nodeIdStr[i] = '\0';

        uint64_t nodeId = strtoull(nodeIdStr, NULL, 10);
        if (nodeId == 0) {
            sendError(conn, "Invalid node ID");
            api->stats.commandsFailed++;
            return true;
        }

        /* Skip to address */
        while (i < argLen && isspace((unsigned char)arg[i])) {
            i++;
        }

        if (i >= argLen) {
            sendError(conn, "Usage: ADD <node-id> <addr:port>");
            api->stats.commandsFailed++;
            return true;
        }

        /* Parse addr:port */
        char addr[64] = {0};
        int port = 0;
        size_t addrStart = i;

        while (i < argLen && arg[i] != ':' && !isspace((unsigned char)arg[i])) {
            if (i - addrStart < sizeof(addr) - 1) {
                addr[i - addrStart] = arg[i];
            }
            i++;
        }

        if (i < argLen && arg[i] == ':') {
            i++;
            char portStr[16] = {0};
            size_t j = 0;
            while (i < argLen && isdigit((unsigned char)arg[i]) &&
                   j < sizeof(portStr) - 1) {
                portStr[j++] = arg[i++];
            }
            port = atoi(portStr);
        }

        if (addr[0] == '\0' || port <= 0) {
            sendError(conn, "Invalid address format (use addr:port)");
            api->stats.commandsFailed++;
            return true;
        }

        if (loopyServerAddNode(server, nodeId, addr, port)) {
            sendOK(conn);
            api->stats.commandsSucceeded++;
        } else {
            sendError(conn, "Failed to add node");
            api->stats.commandsFailed++;
        }
        return true;
    }

    /* REMOVE <node-id> - Remove node from cluster */
    if (len >= 6 && strncasecmp(line, "REMOVE", 6) == 0) {
        /* Must be leader to remove nodes */
        if (!loopyServerIsLeader(server)) {
            sendRedirect(conn);
            api->stats.redirectsSent++;
            return true;
        }

        /* Parse node ID */
        const char *arg = line + 6;
        size_t argLen = len - 6;
        while (argLen > 0 && isspace((unsigned char)*arg)) {
            arg++;
            argLen--;
        }

        if (argLen == 0) {
            sendError(conn, "Usage: REMOVE <node-id>");
            api->stats.commandsFailed++;
            return true;
        }

        char nodeIdStr[32];
        size_t i = 0;
        while (i < argLen && i < sizeof(nodeIdStr) - 1 &&
               isdigit((unsigned char)arg[i])) {
            nodeIdStr[i] = arg[i];
            i++;
        }
        nodeIdStr[i] = '\0';

        uint64_t nodeId = strtoull(nodeIdStr, NULL, 10);
        if (nodeId == 0) {
            sendError(conn, "Invalid node ID");
            api->stats.commandsFailed++;
            return true;
        }

        if (loopyServerRemoveNode(server, nodeId)) {
            sendOK(conn);
            api->stats.commandsSucceeded++;
        } else {
            sendError(conn, "Failed to remove node");
            api->stats.commandsFailed++;
        }
        return true;
    }

    return false;
}
