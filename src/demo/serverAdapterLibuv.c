// #undef KRAFT_TEST_VERBOSE
#include "serverAdapterLibuv.h"
#include "../rpc.h"
#include "../serverAdapter.h"

#include "../rpcCandidate.h"
#include "../rpcLeader.h"

#include "../serverKit.h"
#include "kraftStateMachine.h"

#include <uv.h>

#define RECONNECT(node)                                                        \
    (!(node)->disconnect && (node)->client->reconnectIp.port)
#define fmtipport(node) (node)->client->ip.ip, (node)->client->ip.port

/* Dirty convenience macros for pulling data from libuv privdata.
 * Mark all final vars as void so we don't get unused warnings if unused. */
#define STATE(stream)                                                          \
    kraftServerAdapterLibuvState *adapterState =                               \
        (kraftServerAdapterLibuvState *)(stream->data);                        \
    kraftState *state = adapterState->state;                                   \
    (void)state;

typedef struct kraftServerAdapterLibuvState {
    kraftState *state;
    uv_loop_t loop;
    uv_poll_t handle;
    const struct sockaddr *serverAddr;
    int events; /* 'int' dictated by libuv API of uv_poll_start() */
} kraftServerAdapterLibuvState;

#define STATE_CLIENT(stream)                                                   \
    stateClient *clientState = (stateClient *)(stream->data);                  \
    kraftClient *client = clientState->client;                                 \
    kraftNode *node = clientState->node;                                       \
    kraftServerAdapterLibuvState *adapterState = clientState->state;           \
    kraftState *state = adapterState->state;                                   \
    (void)state;                                                               \
    (void)node;                                                                \
    (void)client;

typedef struct stateClient {
    kraftClient *client;
    kraftNode *node;
    kraftServerAdapterLibuvState *state;
} stateClient;

#define STATE_TIMER(timer)                                                     \
    stateTimer *timerState = (stateTimer *)(timer->data);                      \
    kraftState *state = timerState->state;                                     \
    (void)state;

typedef struct stateTimer {
    void *watch;
    kraftState *state;
} stateTimer;

#define STATE_OUTBOUND(outbound)                                               \
    stateOutbound *outboundState = (stateOutbound *)(outbound->data);          \
    kraftIp *ip = &outboundState->ip;                                          \
    kraftServerAdapterLibuvState *adapterState = outboundState->adapterState;  \
    kraftState *state = adapterState->state;                                   \
    (void)adapterState;                                                        \
    (void)state;                                                               \
    (void)ip;
typedef struct stateOutbound {
    kraftServerAdapterLibuvState *adapterState;
    kraftIp ip;
} stateOutbound;

/* Joint Consensus Pool Assignment */
typedef enum jcPool { ADAPTER_NODES_OLD = 1, ADAPTER_NODES_NEW } jcPool;

/* ====================================================================
 * New
 * ==================================================================== */
kraftServerAdapterLibuvState *kraftServerAdapterLibuvNew(kraftState *state) {
    kraftServerAdapterLibuvState *adapterState =
        calloc(1, sizeof(*adapterState));
    adapterState->state = state;
    state->adapterState = adapterState;
    return adapterState;
}

/* ====================================================================
 * ServerAdapter functions
 * ==================================================================== */
static void destroyState(uv_handle_t *handle) {
    STATE(handle);
    free(state);
}

void kraftServerAdapterLibuvClose(void *data) {
    STATE(((uv_handle_t *)data));
    (void)state;
    uv_close((uv_handle_t *)&adapterState->handle, destroyState);
}

static bool timer(char *name, kraftState *state, kraftTimerAction timerAction,
                  void **setTimer, void *data, uint64_t timeout,
                  uv_timer_cb fun) {
    if (!setTimer) {
        assert(NULL);
    }

    (void)name;

    //    D("TIMER %s: ACTION %d", name, timerAction);

    /* If storage location for timer isn't populated, create timer. */
    if (!*setTimer) {
        stateTimer *timerState = calloc(1, sizeof(*timerState));
        timerState->state = state;

        uv_timer_t *timer = calloc(1, sizeof(*timer));
        timer->data = timerState;
        *setTimer = timer;

        uv_timer_init(state->loop, *setTimer);
    }

    /* Allow updating data even if we aren't creating timer. */
    if (data) {
        ((stateTimer *)((uv_timer_t *)*setTimer)->data)->watch = data;
    }

    switch (timerAction) {
    case KRAFT_TIMER_START:
        uv_timer_start(*setTimer, (uv_timer_cb)fun, timeout, timeout);
        break;
    case KRAFT_TIMER_STOP:
        uv_timer_stop(*setTimer);
        break;
    case KRAFT_TIMER_AGAIN:
        uv_timer_again(*setTimer);
        break;
    }
    return true;
}

/* ====================================================================
 * TESTING: every 100 ms generate new AppendEntries only for Leader!
 * ==================================================================== */
static void testingAppendEntriesCallback(uv_timer_t *timer) {
    STATE_TIMER(timer);

    static uint64_t counter = 1;
    if (!IS_LEADER(state->self)) {
        return;
    }

    D("On Leader!  Sending Append RPC...");

    /* Format of entry:
     *   [CMD    ][COUNTER ]
     *   [1 byte][127 bytes] */
    uint8_t rpcEntryData[128] = {0};
    rpcEntryData[0] = KRAFT_STATE_MACHINE_SET;

    /* This increment pattern should force every index to have
     * the same entry value as the RPC itself. */

    snprintf((char *)rpcEntryData + 1, sizeof(rpcEntryData) - 1, "%" PRIu64,
             counter++);

    kraftRpc entries = {0};
    kraftRpcPopulateEntryAppend(state, &entries,
                                KRAFT_STORAGE_CMD_APPEND_ENTRIES, rpcEntryData,
                                sizeof(rpcEntryData));
    kraftRpcLeaderSendRpc(state, &entries);
}

static void *testingTimer = NULL;
bool kraftServerAdapterLibuvTimerAppendEntriesClient(
    kraftState *state, kraftTimerAction timerAction) {
    D("Setting append entires timer...");
    return timer("Leader Appending Entries For Testing", state, timerAction,
                 &testingTimer, NULL, 100, testingAppendEntriesCallback);
}

/* ====================================================================
 * Internal Connection Maintenance
 * ==================================================================== */
static void maintTimerCallback(uv_timer_t *timer) {
    STATE_TIMER(timer);

    /* For each of our pending nodes, reach out and try to
     * get a response out of them.  Our pending nodes list *should*
     * be empty most of the time unless there are network
     * problems. */

#if 0
    for each node in our NODES_RECONNECT array, ...
        if (RECONNECT(node)) {
            /* reconnect */
            /* ... perhaps after a delay ... */
            outbound(adapterState, &node->client->reconnectIp);
        }
#endif
}

bool kraftServerAdapterLibuvTimerMaint(kraftState *state,
                                       kraftTimerAction timerAction) {
    return timer("Periodic Maint Timer", state, timerAction, &state->maintTimer,
                 NULL, 1000, maintTimerCallback);
}

/* ====================================================================
 * Heartbeat / Election / Contact Timers
 * ==================================================================== */
static void electionTimerCallback(uv_timer_t *timer) {
    STATE_TIMER(timer);

    //    D("ELECTION TIMER TRIGGERED!");
    /* No heartbeats in TRIGGERED milliseconds; begin voting. */
    uv_timer_set_repeat(timer, 3000 + kraftRandomDelay());
    state->internals.election.vote(state);
}

bool kraftServerAdapterLibuvTimerLeaderContact(kraftState *state,
                                               kraftTimerAction timerAction) {
    return timer("No Contact From Leader", state, timerAction,
                 &state->leaderContactTimer, NULL, 3000 + kraftRandomDelay(),
                 electionTimerCallback);
}

static void heartbeatSendingCallback(uv_timer_t *timer) {
    STATE_TIMER(timer);

    D("HEARTBEAT TIMER TRIGGERED!");
    state->internals.election.sendHeartbeat(state);
}

bool kraftServerAdapterLibuvTimerHeartbeatSending(
    kraftState *state, kraftTimerAction timerAction) {
    return timer("Leader Heartbeat Sending", state, timerAction,
                 &state->leaderHeartbeatSending, NULL, 1000,
                 heartbeatSendingCallback);
}

bool kraftServerAdapterLibuvTimerElectionBackoff(kraftState *state,
                                                 kraftTimerAction timerAction);
static void electionBackoffCallback(uv_timer_t *timer) {
    STATE_TIMER(timer);

    //    D("ELECTION BACKOFF TIMER TRIGGERED!");
    //    D("Current repeat on timer: %d", uv_timer_get_repeat(timer));
    uint32_t newTimer = uv_timer_get_repeat(timer) + 300;

    /* limit election delay to a maximum of 5 seconds, otherwise it would
     * grow forever during a failure. */
    newTimer = MIN(5000, newTimer) + kraftRandomDelay();

    //    D("Setting timer repeat to: %d", newTimer);
    uv_timer_set_repeat(timer, newTimer);
    /* new value doesn't take effect unless forcing it */

    state->internals.election.vote(state);
}

bool kraftServerAdapterLibuvTimerElectionBackoff(kraftState *state,
                                                 kraftTimerAction timerAction) {
    return timer("Election Backoff", state, timerAction,
                 &state->candidateElectionBackoff, NULL, 1000,
                 electionBackoffCallback);
}

/* ====================================================================
 * Reading Data from Connection
 * ==================================================================== */
static void allocBuffer(uv_handle_t *handle, size_t suggestedSize,
                        uv_buf_t *buf) {
    (void)handle;
    *buf = uv_buf_init(calloc(1, suggestedSize), suggestedSize);
}

static void releaseClientNode(uv_handle_t *stream) {
    STATE_CLIENT(stream);

/* TODO: Fix node reconnect looping. */
#if 0
    if (RECONNECT(node)) {
        D("RECONNECT Reconnecting to %s:%d", fmtipport(node));
        /* reconnect */
        /* ... perhaps after a delay ... */
        outbound(adapterState, &node->client->reconnectIp);
    }
#endif

    D("Removing host %s:%d from list of nodes (%p)...", fmtipport(node),
      (void *)node);
    kraftNodeFree(state, node);
    free(clientState);
}

static bool _processCompleteRpc(kraftState *state, kraftRpc *rpc) {
    /* If not VERIFIED, we need to do connection acceptance testing! */
    if (serverKitNodeNeedsHelloProcessing(rpc->src)) {
        return serverKitProcessHello(state, rpc);
    }

    /* Give RPC to Kraft for processing. */
    kraftRpcStatus rpcStatus = 0;
    /* IF rpc->cmd IS KRAFT_RPC_START_JOINT_CONSENSUS then
     * we need to extract [IP][PORT] pairs from rpc->entry
     * and connect outbound to each new IP/PORT pair and
     * add them to the node list and start joint consensus
     * mode. */

    if (!kraftRpcProcess(state, rpc, &rpcStatus)) {
        /* ERROR!  Try to send error or just disconnect. */

        /* Attempt to generate a proper error reply.
         * If we can reply, we don't disconnect. */
        if (rpc->disconnectSource) {
            /* else, we don't have an error reply and we need
             * to terminate this client. */
            /* ERROR — Disconnect Client! */
            D("Closing connection after invalid RPC reason: %s",
              kraftRpcStatuses[rpcStatus]);
            return false;
        }
    }

    return true;
}

#define CLOSE_CLIENT_SAFE(stream)                                              \
    do {                                                                       \
        D("Requested closing client stream %p", (stream));                     \
        if (!uv_is_closing((uv_handle_t *)(stream))) {                         \
            D("Closing client stream %p", (stream));                           \
            uv_close((uv_handle_t *)(stream), releaseClientNode);              \
        }                                                                      \
    } while (0)

static void readClientData(uv_stream_t *stream, ssize_t nread,
                           const uv_buf_t *buf) {
    STATE_CLIENT(stream);

    if (nread == UV_EOF) {
        /* done */
        D("Closing entire node due to EOF: %s:%d", fmtipport(node));
        CLOSE_CLIENT_SAFE(stream);
    } else if (nread == 0) {
        /* Nothing to read, but not an error. */
    } else if (nread < 0) {
        /* ERROR */
        CLOSE_CLIENT_SAFE(stream);
        /* reconnect? */
    } else {
        /* We have 'nread' bytes available in 'buf' */

        protoKitStatus packetStatus = protoKitProcessPacketData(
            &state->protoKit, &node->client->buf, (uint8_t *)buf->base, nread);
        /* D("Packet process status: %s", kraftProtocolStatuses[packetStatus]);
         */

        if (packetStatus == PROTOKIT_STATUS_PDU_AVAILABLE) {
            /* Process client buffer until we run out of buffered RPCs */
            /* Note: we are processing *every* available entry in this RPC
             *       right now.  We _could_ potentially only processes N
             *       entries per RPC before yielding back to the event loop
             *       to provide more fairness, but we don't expect entry
             *       processing to be a bottleneck here.  For now, it's
             *       best to process all the data we have as soon as we
             *       can instead of yielding back to others. */
            bool maybeHasMore = true;
            while (maybeHasMore) {
                kraftRpc rpc = {0};
                rpc.src = node;
                kraftProtocolStatus populateStatus =
                    state->internals.protocol.populateRpcFromClient(state, node,
                                                                    &rpc);
                /* D("RPC Populate status: %s",
                 *   kraftProtocolStatuses[populateStatus]); */

                bool disconnect = false;
                switch (populateStatus) {
                case KRAFT_PROTOCOL_STATUS_RPC_POPULATED:
                    if (!_processCompleteRpc(state, &rpc)) {
                        disconnect = true;
                    }
                    break;
                case KRAFT_PROTOCOL_STATUS_NO_RPC:
                    /* No RPC available, but also no error.  Just stop. */
                    /* means basically "no more RPCs remaining,"
                     * so we are done for now. */
                    maybeHasMore = false;
                    break;
                case KRAFT_PROTOCOL_STATUS_PARSE_ERROR:
                    D("Closing connection after PARSE ERROR...");
                    disconnect = true;
                    break;
                default:
                    /* SYSTEM ERROR or non-applicable return value */
                    assert(
                        NULL &&
                        "Bad return value from protocol.populateRpcFromClient");
                }

                if (disconnect) {
                    node->disconnect = true;
                    CLOSE_CLIENT_SAFE(stream);
                    /* Don't process another RPC here since we want the
                     * client to go away. */
                    maybeHasMore = false;
                }
            }
        }
    }
    /* 'buf->base' is guaranteed to be either NULL or a malloc'd pointer, so we
     * can always free it. */
    free(buf->base);
}

/* Create Kraft Client and populate IP, port, and family from network client */
static kraftClient *createKraftClient(kraftState *state, uv_stream_t *client) {
    if (!state || !client) {
        return NULL;
    }

    kraftClient *kc = kraftClientNew(state, client);

    /* Retrieve remote IP, port information from client */
    struct sockaddr_in *s = NULL;
    struct sockaddr_in6 *s6 = NULL;
    struct sockaddr_storage addr = {0};
    int namelen = sizeof(addr);
    uv_tcp_getpeername((uv_tcp_t *)client, (struct sockaddr *)&addr, &namelen);

    /* Populate kraftClient fields */
    switch (addr.ss_family) {
    case AF_INET:
        kc->ip.family = KRAFT_IPv4;
        s = (struct sockaddr_in *)&addr;
        kc->ip.port = ntohs(s->sin_port);
        uv_inet_ntop(AF_INET, &s->sin_addr, kc->ip.ip, sizeof(kc->ip.ip));
        break;
    case AF_INET6:
        kc->ip.family = KRAFT_IPv6;
        s6 = (struct sockaddr_in6 *)&addr;
        kc->ip.port = ntohs(s6->sin6_port);
        uv_inet_ntop(AF_INET6, &s6->sin6_addr, kc->ip.ip, sizeof(kc->ip.ip));
        break;
    }
    return kc;
}

static stateClient *genClientState(kraftServerAdapterLibuvState *adapterState,
                                   jcPool pool, uv_stream_t *client) {
    stateClient *clientState = calloc(1, sizeof(*clientState));

    /* Create Kraft client */
    kraftClient *kc = createKraftClient(adapterState->state, client);
    D("Created client %s:%d and attaching holder %p\n", kc->ip.ip, kc->ip.port,
      kc);

    kraftNode *node = NULL;

    switch (pool) {
    case ADAPTER_NODES_OLD:
        node = kraftNodeCreateOld(adapterState->state, kc);
        break;
    case ADAPTER_NODES_NEW:
        node = kraftNodeCreateNew(adapterState->state, kc);
        break;
    default:
        assert(NULL && "Invalid pool specified!");
    }
    D("Created node (and added it to global %s pending node array).",
      pool == ADAPTER_NODES_OLD ? "OLD" : "NEW");

    /* Attach client to callback state */
    clientState->client = kc;
    clientState->node = node;
    clientState->state = adapterState;

    /* If we're an outbound connection, we have a valid reconnect IP. */
    if (node->client->ip.outbound) {
        node->client->reconnectIp = node->client->ip;
    }

    return clientState;
}

static stateClient *
assignClientState(kraftState *state, kraftServerAdapterLibuvState *adapterState,
                  uv_handle_t *client) {
    jcPool pool = ADAPTER_NODES_OLD;
    if (kraftOperatingUnderJointConsensusOldNew(state) ||
        kraftOperatingUnderJointConsensusNew(state)) {
        pool = ADAPTER_NODES_NEW;
    }
    stateClient *clientState =
        genClientState(adapterState, pool, (uv_stream_t *)client);
    client->data = clientState;
    return clientState;
}

static void
kraftServerAdapterLibuvCallbackConnectionOutbound(uv_connect_t *cxnClient,
                                                  int status) {
    STATE_OUTBOUND(cxnClient);

    if (status < 0) {
        /* ERROR */
        D("Error on outbound: %s", uv_strerror(status));
        /* Free state?  Free client?  Free peer? */

        /* reconnect */
        /* ... perhaps after a delay ... */
        /* COMMENT OUT FOR DEV.  YOU ONLY GET ONE CHANCE TO RECONNECT! */
        uv_close((uv_handle_t *)cxnClient->handle, NULL); /* memory leak? */
        free(cxnClient->data);
        /* need to free cxnClient */

        // outbound(adapterState, ip);
        return;
    }

    uv_stream_t *client = cxnClient->handle;
    D("Accepted outbound callback!");
    /* This is '->data' for readClientData() */
    /* Add client to list of known clients */
    /* Create client for OUTBOUND connecton. */
    stateClient *clientState =
        assignClientState(state, adapterState, (uv_handle_t *)client);

    /* Mark this connection as outbound */
    clientState->node->client->ip.outbound = true;

    /* We are a verified outbound connection (this is the outbound callback),
     * so set up our reconnect ip. */
    clientState->client->reconnectIp = clientState->client->ip;

    /* introduce ourselves to our new neighbor! */
    serverKitSendHello(adapterState->state, clientState->node);

    uv_read_start(client, allocBuffer, readClientData);
    free(cxnClient->data);
    free(cxnClient);
}

static void
kraftServerAdapterLibuvCallbackConnectionInbound(uv_stream_t *server,
                                                 int status) {
    STATE(server);

    if (status < 0) {
        /* ERROR */
        return;
    }

    D("Connection inbound: %p\n", server);
    uv_tcp_t *client = calloc(1, sizeof(*client));

    /* Attach client to event loop */
    uv_tcp_init(&adapterState->loop, client);

    /* TODO:
     *   - If inbound connection isn't known in config
     *   - If current connections are all populated
     *   - Add inbound client to NEW instead of OLD? */
    if (uv_accept(server, (uv_stream_t *)client) == 0) {
        /* Create client for INBOUND connection */
        assignClientState(state, adapterState, (uv_handle_t *)client);
        uv_read_start((uv_stream_t *)client, allocBuffer, readClientData);
    } else {
        D("Accept error!");
        uv_close((uv_handle_t *)client, NULL); /* memory leak? */
    }
}

/* ====================================================================
 * Send RPC API
 * ==================================================================== */
static void cleanupRpcSend(uv_write_t *req, int status) {
    if (status < 0) {
        /* write error */
        /* Potentially enqueue a re-send of the RPC. */
    }

    free(req);
}

void serverAdapterLibuvSendRpc(kraftState *state, kraftRpc *rpc, uint8_t *buf,
                               uint64_t len, kraftNode *node) {
    (void)state;
    uv_buf_t buf_uv = {.base = (char *)buf, .len = len};

    kraftClient *kc = node->client;
    D("Broadcasting %s to: %s:%d", kraftRpcCmds[rpc->cmd], kc->ip.ip,
      kc->ip.port);

    uv_stream_t *client = kc->stream;
    if (client) {
        /* This looks memory-heavy just to create a new write request
         * context for every RPC.  Maybe keep a reusable pool of these? */
        uv_write_t *req = calloc(1, sizeof(*req));
        uv_write(req, client, &buf_uv, 1, cleanupRpcSend);
    }
}

void kraftServerAdapterLibuvConnectOutbound(kraftState *state, kraftIp *ip) {
    kraftServerAdapterLibuvState *adapterState = state->adapterState;

    uv_tcp_t *peer = calloc(1, sizeof(*peer));
    uv_connect_t *connect = calloc(1, sizeof(*connect));

    struct sockaddr_in dest = {0};

    /* Attach peer to event loop */
    uv_tcp_init(&adapterState->loop, peer);

    /* Bind outbound connection to server source address */
    int outboundBindError = uv_tcp_bind(peer, adapterState->serverAddr, 0);
    if (outboundBindError) {
        D("Outbound Bind Error: %s", uv_strerror(outboundBindError));
    }

    /* Get address of peer */
    D("Connecting to %s:%d", ip->ip, ip->port);

    /* This is outbound connect.  Establish outbound IP authority. */
    ip->outbound = true;

    /* TODO: detect 4-vs-6 */
    int ipError = uv_ip4_addr(ip->ip, ip->port, &dest);
    if (ipError) {
        D("IP Error: %s", uv_strerror(ipError));
    }

    /* Create state for tracking this client in connection callback. */
    stateOutbound *outboundState = calloc(1, sizeof(*outboundState));
    outboundState->adapterState = adapterState;
    outboundState->ip = *ip;

    /* Attach state to connect */
    connect->data = outboundState;

    /* Attempt connect */
    int connectError =
        uv_tcp_connect(connect, peer, (const struct sockaddr *)&dest,
                       kraftServerAdapterLibuvCallbackConnectionOutbound);
    if (connectError) {
        D("Connect error: %s", uv_strerror(connectError));
    }
}

/* ====================================================================
 * Listen for new connections
 * ==================================================================== */
/* Begin requires:
 *   - Bind + Listen on configured IP:Port
 *   - Connect to other peers defined by configuration
 *   - Start event loop. */
bool kraftServerAdapterLibuvBegin(kraftState *state, char *listenIP,
                                  int listenPort) {
    kraftServerAdapterLibuvState *adapterState =
        kraftServerAdapterLibuvNew(state);

    /* rand() is only used for setting random backoff/retry delays. */
    srand(kraftustime());

    int backlog = 511;

    /* For more stable memory usage, create pool of UV objects to use/free */

    uv_loop_init(&adapterState->loop);
    state->loop = &adapterState->loop;

    uv_tcp_t server = {0};
    uv_tcp_init(&adapterState->loop, &server);
    server.data = adapterState;

    /* can ask for uv_getaddrinfo to determine IP type (or resolve hostname) */
    struct sockaddr_in serverAddr = {0};
    uv_ip4_addr(listenIP, listenPort, &serverAddr);
    adapterState->serverAddr = (const struct sockaddr *)&serverAddr;

    D("Listening on %s:%d", listenIP, listenPort);

    /* Bind to listen address */
    int bindError =
        uv_tcp_bind(&server, (const struct sockaddr *)&serverAddr, 0);
    if (bindError) {
        D("Bind Error: %s", uv_strerror(bindError));
    }

    /* TODO: Replace with actual config management */
    uint32_t peerCount = state->initialIpsCount;

    /* Connect to each peer */
    for (uint32_t i = 0; i < peerCount; i++) {
        kraftServerAdapterLibuvConnectOutbound(state, state->initialIps[i]);
    }

    int listenError =
        uv_listen((uv_stream_t *)&server, backlog,
                  kraftServerAdapterLibuvCallbackConnectionInbound);

    if (listenError) {
        D("Listen error: %s", uv_strerror(listenError));
        return false;
    }

    kraftServerAdapterLibuvTimerMaint(state, KRAFT_TIMER_START);

    /* On start, we are FOLLOWER and we need a heatbeat election timeout.
     * If we don't receive a heatbeat in the next N seconds, we start an
     * election */
    kraftServerAdapterLibuvTimerLeaderContact(state, KRAFT_TIMER_START);

    /* FOR TESTING */
    kraftServerAdapterLibuvTimerAppendEntriesClient(state, KRAFT_TIMER_START);

    uv_run(&adapterState->loop, UV_RUN_DEFAULT);
    D("Event loop exited!");
    uv_loop_close(&adapterState->loop);
    return true;
}
