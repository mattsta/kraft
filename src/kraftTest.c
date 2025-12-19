#include "kraftTest.h"
#include "ctest.h"
#include "rpcLeader.h"
#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ====================================================================
 * Random Number Generation (for deterministic testing)
 * ==================================================================== */
static uint64_t test_seed = 0;

void testSeed(uint64_t seed) {
    test_seed = seed;
    srand(seed);
}

uint64_t testRandom(uint64_t min, uint64_t max) {
    if (min >= max) {
        return min;
    }
    return min + (rand() % (max - min + 1));
}

/* ====================================================================
 * Message Queue Management
 * ==================================================================== */
static void networkInitMessages(testNetwork *net) {
    net->messageCapacity = TEST_MAX_PENDING_MSGS;
    net->messages = calloc(net->messageCapacity, sizeof(testMessage));
    net->messageCount = 0;
}

static void networkFreeMessages(testNetwork *net) {
    for (uint32_t i = 0; i < net->messageCount; i++) {
        free(net->messages[i].data);
    }
    free(net->messages);
}

/* Insert message in time-sorted order */
static void networkQueueMessage(testNetwork *net, uint64_t deliveryTime,
                                uint8_t *data, size_t len, uint32_t from,
                                uint32_t to) {
    if (net->messageCount >= net->messageCapacity) {
        /* Grow queue */
        net->messageCapacity *= 2;
        net->messages =
            realloc(net->messages, net->messageCapacity * sizeof(testMessage));
    }

    testMessage msg = {.deliveryTime = deliveryTime,
                       .data = malloc(len),
                       .len = len,
                       .fromNode = from,
                       .toNode = to,
                       .dropped = false};
    memcpy(msg.data, data, len);

    /* Insert in sorted position */
    uint32_t insertPos = net->messageCount;
    for (uint32_t i = 0; i < net->messageCount; i++) {
        if (net->messages[i].deliveryTime > deliveryTime) {
            insertPos = i;
            break;
        }
    }

    /* Shift messages */
    if (insertPos < net->messageCount) {
        memmove(&net->messages[insertPos + 1], &net->messages[insertPos],
                (net->messageCount - insertPos) * sizeof(testMessage));
    }

    net->messages[insertPos] = msg;
    net->messageCount++;
    net->totalSent++;
}

static testMessage *networkDequeueMessage(testNetwork *net,
                                          uint64_t currentTime) {
    if (net->messageCount == 0) {
        return NULL;
    }

    /* Check if first message is ready */
    if (net->messages[0].deliveryTime > currentTime) {
        return NULL;
    }

    testMessage *msg = &net->messages[0];
    return msg;
}

static void networkRemoveFirstMessage(testNetwork *net) {
    if (net->messageCount == 0) {
        return;
    }

    free(net->messages[0].data);

    memmove(&net->messages[0], &net->messages[1],
            (net->messageCount - 1) * sizeof(testMessage));
    net->messageCount--;
}

/* ====================================================================
 * Test Network Callbacks (injected into Kraft nodes)
 * ==================================================================== */
static void testNetworkSendRpc(kraftState *ks, kraftRpc *rpc, uint8_t *buf,
                               uint64_t len, kraftNode *node) {
    testCluster *cluster = (testCluster *)ks->adapterState;

    /* Find source and destination node IDs */
    uint32_t fromNode = UINT32_MAX;
    uint32_t toNode = UINT32_MAX;

    for (uint32_t i = 0; i < cluster->nodeCount; i++) {
        if (cluster->nodes[i].state == ks) {
            fromNode = i;
        }
        /* Match by nodeId since each node has separate kraftNode instances */
        if (cluster->nodes[i].state->self->nodeId == node->nodeId) {
            toNode = i;
        }
    }

    assert(fromNode != UINT32_MAX);
    assert(toNode != UINT32_MAX);

    /* Check for partition */
    if (cluster->network.partition[fromNode][toNode]) {
        cluster->network.totalDropped++;
        return;
    }

    /* Apply network conditions */
    bool drop = false;
    uint64_t delay = 0;

    switch (cluster->network.mode) {
    case TEST_NETWORK_LOSSY:
        if (((double)rand() / RAND_MAX) < cluster->network.dropRate) {
            drop = true;
        }
        break;
    case TEST_NETWORK_DELAYED:
    case TEST_NETWORK_REORDERING:
        delay =
            testRandom(cluster->network.minDelay, cluster->network.maxDelay);
        break;
    case TEST_NETWORK_RELIABLE:
    default:
        break;
    }

    if (drop) {
        cluster->network.totalDropped++;
        return;
    }

    /* Queue message */
    uint64_t deliveryTime = cluster->currentTime + delay;
    networkQueueMessage(&cluster->network, deliveryTime, buf, len, fromNode,
                        toNode);

    cluster->nodes[fromNode].msgsSent++;
}

/* Stub implementations for other callbacks */
static bool testServerBegin(kraftState *state, char *listenIP, int listenPort) {
    (void)state;
    (void)listenIP;
    (void)listenPort;
    return true;
}

static void testCallbackTimeout(kraftState *state, void *data) {
    (void)state;
    (void)data;
}

static bool testProtocolEncode(const kraftState *state, const kraftRpc *rpc,
                               uint8_t **buf, uint64_t *len) {
    (void)state;

    /* Serialize RPC metadata + flex data */
    size_t flexSize = rpc->entry ? flexBytes(rpc->entry) : 0;
    *len = sizeof(kraftRpc) + flexSize;
    *buf = malloc(*len);

    /* Copy RPC struct */
    memcpy(*buf, rpc, sizeof(kraftRpc));

    /* Copy flex data if present */
    if (flexSize > 0) {
        memcpy(*buf + sizeof(kraftRpc), rpc->entry, flexSize);
        /* Update entry pointer in serialized struct to indicate offset */
        kraftRpc *serialized = (kraftRpc *)*buf;
        serialized->entry =
            (flex *)(uintptr_t)sizeof(kraftRpc); /* Store as offset */
    }

    return true;
}

static void testProtocolFree(void *buf) {
    free(buf);
}

static kraftProtocolStatus
testProtocolPopulate(kraftState *state, kraftNode *node, kraftRpc *rpc) {
    /* Will be called by test harness directly */
    return KRAFT_PROTOCOL_STATUS_RPC_POPULATED;
}

static void testDataApply(const void *data, const uint64_t len) {
    /* For testing, we just track that it was called */
    (void)data;
    (void)len;
}

/* Entry encoding/decoding for testing */
/* Simple format: [idx:8][term:8][cmd:8][len:8][data:len] repeated */

static bool testEntryEncode(kraftRpc *rpc, kraftRpcUniqueIndex idx,
                            kraftTerm term, kraftStorageCmd originalCmd,
                            const void *entry, size_t len) {
    /* Allocate if needed */
    if (!rpc->entry) {
        rpc->entry = flexNew();
        rpc->resume = NULL;
        rpc->remaining = 0;
    }

    /* Encode: index, term, cmd, length, data to TAIL */
    flexPushUnsigned(&rpc->entry, idx, FLEX_ENDPOINT_TAIL);
    flexPushUnsigned(&rpc->entry, term, FLEX_ENDPOINT_TAIL);
    flexPushUnsigned(&rpc->entry, (uint64_t)originalCmd, FLEX_ENDPOINT_TAIL);
    flexPushUnsigned(&rpc->entry, len, FLEX_ENDPOINT_TAIL);

    if (len > 0 && entry) {
        flexPushBytes(&rpc->entry, entry, len, FLEX_ENDPOINT_TAIL);
    }

    rpc->len = flexBytes(rpc->entry);
    /* Count complete entries, not flex elements */
    rpc->remaining++;
    if (!rpc->resume) {
        rpc->resume = flexHead(rpc->entry);
    }

    return true;
}

static bool testEntryEncodeBulk(kraftRpc *rpc, kraftTerm term,
                                kraftStorageCmd originalCmd, const size_t count,
                                const kraftRpcUniqueIndex idx[],
                                const void *data[], const size_t len[]) {
    for (size_t i = 0; i < count; i++) {
        if (!testEntryEncode(rpc, idx[i], term, originalCmd, data[i], len[i])) {
            return false;
        }
    }
    return true;
}

static bool testEntryDecode(kraftRpc *rpc, kraftRpcUniqueIndex *idx,
                            kraftTerm *term, kraftStorageCmd *originalCmd,
                            uint8_t **entry, size_t *len) {
    if (!rpc->entry || !rpc->resume) {
        return false;
    }

    /* CRITICAL FIX: Use temporary variables for ALL parsed values.
     * Only copy to output parameters at the very end after ALL validation
     * passes. This prevents modifying caller's variables when decode fails
     * partway through parsing, which was causing entryIndex/entryTerm to
     * retain stale values from partially-decoded entries. */
    uint64_t idxVal, termVal, cmdVal, lenVal;
    uint8_t *entryVal = NULL;
    size_t lenValFinal = 0;

    /* Save resume pointer in case we need to restore on failure */
    void *savedResume = rpc->resume;

    if (!flexGetUnsigned(rpc->resume, &idxVal)) {
        return false;
    }
    rpc->resume = flexNext(rpc->entry, rpc->resume);
    if (!rpc->resume) {
        rpc->resume = savedResume;
        return false;
    }

    if (!flexGetUnsigned(rpc->resume, &termVal)) {
        rpc->resume = savedResume;
        return false;
    }
    rpc->resume = flexNext(rpc->entry, rpc->resume);
    if (!rpc->resume) {
        rpc->resume = savedResume;
        return false;
    }

    if (!flexGetUnsigned(rpc->resume, &cmdVal)) {
        rpc->resume = savedResume;
        return false;
    }
    rpc->resume = flexNext(rpc->entry, rpc->resume);
    if (!rpc->resume) {
        rpc->resume = savedResume;
        return false;
    }

    if (!flexGetUnsigned(rpc->resume, &lenVal)) {
        rpc->resume = savedResume;
        return false;
    }
    rpc->resume = flexNext(rpc->entry, rpc->resume);

    lenValFinal = (size_t)lenVal;

    if (lenValFinal > 0 && rpc->resume) {
        /* For bytes, we need to use databox approach */
        databox box;
        flexGetByType(rpc->resume, &box);
        if (box.type == DATABOX_BYTES || box.type == DATABOX_BYTES_EMBED) {
            entryVal = (uint8_t *)box.data.bytes.start;
            lenValFinal = box.len;
        } else {
            rpc->resume = savedResume;
            return false;
        }
        rpc->resume = flexNext(rpc->entry, rpc->resume);
    }

    /* SUCCESS: All validation passed. Now copy to output parameters. */
    *idx = (kraftRpcUniqueIndex)idxVal;
    *term = (kraftTerm)termVal;
    *originalCmd = (kraftStorageCmd)cmdVal;
    *len = lenValFinal;
    *entry = entryVal;

    rpc->remaining--;
    return true;
}

static void testEntryRelease(kraftRpc *rpc) {
    if (rpc->entry) {
        flexFree(rpc->entry);
        rpc->entry = NULL;
        rpc->resume = NULL;
        rpc->remaining = 0;
        rpc->len = 0;
    }
}

/* Timer management - stores timeout values in node structure */
static bool testTimerLeaderContact(kraftState *state, kraftTimerAction action) {
    testCluster *cluster = (testCluster *)state->adapterState;
    uint32_t nodeId = UINT32_MAX;

    for (uint32_t i = 0; i < cluster->nodeCount; i++) {
        if (cluster->nodes[i].state == state) {
            nodeId = i;
            break;
        }
    }

    assert(nodeId != UINT32_MAX);
    testNode *node = &cluster->nodes[nodeId];

    switch (action) {
    case KRAFT_TIMER_START:
    case KRAFT_TIMER_AGAIN:
        node->nextElectionTimeout =
            cluster->currentTime + testRandom(cluster->electionTimeoutMin,
                                              cluster->electionTimeoutMax);
        break;
    case KRAFT_TIMER_STOP:
        node->nextElectionTimeout = UINT64_MAX;
        break;
    }

    return true;
}

static bool testTimerHeartbeat(kraftState *state, kraftTimerAction action) {
    testCluster *cluster = (testCluster *)state->adapterState;
    uint32_t nodeId = UINT32_MAX;

    for (uint32_t i = 0; i < cluster->nodeCount; i++) {
        if (cluster->nodes[i].state == state) {
            nodeId = i;
            break;
        }
    }

    assert(nodeId != UINT32_MAX);
    testNode *node = &cluster->nodes[nodeId];

    switch (action) {
    case KRAFT_TIMER_START:
    case KRAFT_TIMER_AGAIN:
        node->nextHeartbeatTimeout =
            cluster->currentTime + cluster->heartbeatInterval;
        break;
    case KRAFT_TIMER_STOP:
        node->nextHeartbeatTimeout = UINT64_MAX;
        break;
    }

    return true;
}

static bool testTimerElectionBackoff(kraftState *state,
                                     kraftTimerAction action) {
    /* Reuse leader contact timer logic */
    return testTimerLeaderContact(state, action);
}

/* ====================================================================
 * Cluster Management
 * ==================================================================== */
testCluster *testClusterNew(uint32_t nodeCount) {
    assert(nodeCount <= TEST_MAX_NODES);
    assert(nodeCount >= 1);

    testCluster *cluster = calloc(1, sizeof(testCluster));
    cluster->nodeCount = nodeCount;
    cluster->currentLeader = UINT32_MAX;
    cluster->currentTerm = 0;

    /* Initialize network */
    networkInitMessages(&cluster->network);
    cluster->network.mode = TEST_NETWORK_RELIABLE;
    cluster->network.dropRate = 0.0;
    cluster->network.minDelay = 0;
    cluster->network.maxDelay = 0;

    /* Default timing configuration */
    cluster->electionTimeoutMin = 150000; /* 150ms */
    cluster->electionTimeoutMax = 300000; /* 300ms */
    cluster->heartbeatInterval = 50000;   /* 50ms */

    cluster->enableInvariantChecking = true;
    cluster->invariantCheckInterval = 10000; /* Check every 10ms */

    /* Create nodes */
    for (uint32_t i = 0; i < nodeCount; i++) {
        cluster->nodes[i].nodeId = i;
        cluster->nodes[i].crashed = false;
        cluster->nodes[i].paused = false;
        cluster->nodes[i].nextElectionTimeout = UINT64_MAX;
        cluster->nodes[i].nextHeartbeatTimeout = UINT64_MAX;

        /* Create Kraft state */
        kraftState *state = kraftStateNew();
        cluster->nodes[i].state = state;

        /* Set up cluster configuration */
        kraftIp **ips = calloc(nodeCount, sizeof(kraftIp *));
        for (uint32_t j = 0; j < nodeCount; j++) {
            ips[j] = calloc(1, sizeof(kraftIp));
            snprintf(ips[j]->ip, sizeof(ips[j]->ip), "127.0.0.%u", j);
            ips[j]->port = 7000 + j;
            ips[j]->family = KRAFT_IPv4;
            ips[j]->outbound = true;
        }

        kraftStateInitRaft(state, nodeCount, ips, 1, i);

        /* Enable test mode to disable timing-based checks in state-machine
         * simulation */
        state->config.testMode = true;

        /* Add all other nodes to this node's cluster configuration */
        for (uint32_t j = 0; j < nodeCount; j++) {
            if (j != i) { /* Don't add ourselves again */
                kraftNode *node = kraftNodeNew(NULL);
                node->clusterId = 1;
                node->nodeId = j;
                node->runId = kraftustime() + j; /* Unique ID for each node */
                node->isOurself = false;

                /* Add to old configuration */
                vecPtrPush(NODES_OLD_NODES(state), node);
            }
        }

        /* Initialize in-memory storage for testing */
        state->log.kvidx.interface = kvidxInterfaceSqlite3;
        kvidxOpen(KRAFT_ENTRIES(state), ":memory:", NULL);

        /* Inject test callbacks */
        state->adapterState = cluster;
        state->internals.rpc.sendRpc = testNetworkSendRpc;
        state->internals.server.begin = testServerBegin;
        state->internals.callbacks.timeout = testCallbackTimeout;
        state->internals.protocol.encodeRpc = testProtocolEncode;
        state->internals.protocol.freeRpc = testProtocolFree;
        state->internals.protocol.populateRpcFromClient = testProtocolPopulate;
        state->internals.data.apply = testDataApply;
        state->internals.timer.leaderContactTimer = testTimerLeaderContact;
        state->internals.timer.heartbeatTimer = testTimerHeartbeat;
        state->internals.timer.electionBackoffTimer = testTimerElectionBackoff;
        state->internals.election.vote = kraftRpcRequestVote;
        state->internals.election.sendHeartbeat = kraftRpcSendHeartbeat;
        state->internals.entry.encode = testEntryEncode;
        state->internals.entry.encodeBulk = testEntryEncodeBulk;
        state->internals.entry.decode = testEntryDecode;
        state->internals.entry.release = testEntryRelease;
    }

    /* Initialize invariant tracking structures */
    cluster->stateMachineHistory.commandHashes = calloc(1000, sizeof(uint64_t));
    cluster->stateMachineHistory.maxIndex = 0;
    cluster->stateMachineHistory.capacity = 1000;

    cluster->appendOnlyTracking.highWaterMark =
        calloc(TEST_MAX_NODES, sizeof(kraftRpcUniqueIndex));
    /* Allocate 2D array for per-node term tracking */
    cluster->appendOnlyTracking.highWaterTerm =
        calloc(TEST_MAX_NODES, sizeof(kraftTerm *));
    for (uint32_t i = 0; i < TEST_MAX_NODES; i++) {
        cluster->appendOnlyTracking.highWaterTerm[i] =
            calloc(10000, sizeof(kraftTerm));
    }
    cluster->appendOnlyTracking.highWaterNodeTerm =
        calloc(TEST_MAX_NODES, sizeof(kraftTerm));

    return cluster;
}

void testClusterFree(testCluster *cluster) {
    if (!cluster) {
        return;
    }

    for (uint32_t i = 0; i < cluster->nodeCount; i++) {
        if (cluster->nodes[i].state) {
            /* Close storage before freeing state */
            kvidxClose(KRAFT_ENTRIES(cluster->nodes[i].state));
            kraftStateFree(cluster->nodes[i].state);
        }
    }

    /* Free invariant tracking structures */
    free(cluster->stateMachineHistory.commandHashes);
    free(cluster->appendOnlyTracking.highWaterMark);
    /* Free 2D highWaterTerm array */
    if (cluster->appendOnlyTracking.highWaterTerm) {
        for (uint32_t i = 0; i < TEST_MAX_NODES; i++) {
            free(cluster->appendOnlyTracking.highWaterTerm[i]);
        }
        free(cluster->appendOnlyTracking.highWaterTerm);
    }
    free(cluster->appendOnlyTracking.highWaterNodeTerm);

    /* Free performance profiling structures */
    free(cluster->perfMetrics.rpcLatency.samples);
    free(cluster->perfMetrics.commitLatency.samples);
    free(cluster->perfMetrics.electionLatency.samples);

    networkFreeMessages(&cluster->network);
    free(cluster);
}

void testClusterStart(testCluster *cluster) {
    cluster->currentTime = 0;

    /* Initialize election timeouts for all nodes */
    for (uint32_t i = 0; i < cluster->nodeCount; i++) {
        testNode *node = &cluster->nodes[i];
        node->nextElectionTimeout = testRandom(cluster->electionTimeoutMin,
                                               cluster->electionTimeoutMax);
    }
}

/* ====================================================================
 * Simulation Engine
 * ==================================================================== */
static void processTimeouts(testCluster *cluster) {
    for (uint32_t i = 0; i < cluster->nodeCount; i++) {
        testNode *node = &cluster->nodes[i];

        if (node->crashed || node->paused) {
            continue;
        }

        kraftState *state = node->state;

        /* Check election timeout */
        if (node->nextElectionTimeout <= cluster->currentTime &&
            node->nextElectionTimeout != UINT64_MAX) {
            node->electionsStarted++;
            cluster->totalElections++;

            /* Trigger election (pre-vote if enabled, otherwise direct vote) */
            if (state->config.enablePreVote) {
                kraftRpcRequestPreVote(state);
            } else {
                kraftRpcRequestVote(state);
            }

            /* Reset timeout */
            node->nextElectionTimeout =
                cluster->currentTime + testRandom(cluster->electionTimeoutMin,
                                                  cluster->electionTimeoutMax);
        }

        /* Check heartbeat timeout (leader only) */
        if (IS_LEADER(state->self) &&
            node->nextHeartbeatTimeout <= cluster->currentTime &&
            node->nextHeartbeatTimeout != UINT64_MAX) {
            kraftRpcSendHeartbeat(state);

            /* Reset timeout */
            node->nextHeartbeatTimeout =
                cluster->currentTime + cluster->heartbeatInterval;
        }
    }
}

static void processMessages(testCluster *cluster) {
    while (true) {
        testMessage *msg =
            networkDequeueMessage(&cluster->network, cluster->currentTime);
        if (!msg) {
            break;
        }

        testNode *toNode = &cluster->nodes[msg->toNode];

        if (!toNode->crashed && !toNode->paused) {
            /* Deserialize and process */
            kraftRpc rpc = {0};
            memcpy(&rpc, msg->data, sizeof(kraftRpc));

            /* Deserialize flex data if present */
            if (rpc.entry) {
                uintptr_t offset = (uintptr_t)rpc.entry;
                size_t flexSize = msg->len - offset;
                flex *flexData = malloc(flexSize);
                memcpy(flexData, msg->data + offset, flexSize);
                rpc.entry = flexData;
                rpc.resume = flexHead(rpc.entry);
            }

            /* Set source node - find the receiver's kraftNode instance for the
             * sender */
            rpc.src = NULL;
            uint32_t senderNodeId =
                cluster->nodes[msg->fromNode].state->self->nodeId;

            if (senderNodeId == toNode->state->self->nodeId) {
                rpc.src = toNode->state->self;
            } else {
                /* Find sender in receiver's node list */
                for (uint32_t i = 0;
                     i < vecPtrCount(NODES_OLD_NODES(toNode->state)); i++) {
                    kraftNode *node = NULL;
                    vecPtrGet(NODES_OLD_NODES(toNode->state), i,
                              (void **)&node);
                    if (node && node->nodeId == senderNodeId) {
                        rpc.src = node;
                        break;
                    }
                }
            }

            kraftRpcStatus status;
            kraftRpcProcess(toNode->state, &rpc, &status);

            /* Free deserialized flex data */
            if (rpc.entry) {
                free(rpc.entry);
            }

            toNode->msgsReceived++;
            cluster->network.totalDelivered++;
        }

        networkRemoveFirstMessage(&cluster->network);
    }
}

void testClusterTick(testCluster *cluster, uint64_t microseconds) {
    uint64_t targetTime = cluster->currentTime + microseconds;

    while (cluster->currentTime < targetTime) {
        /* Find next event time */
        uint64_t nextEvent = targetTime;

        /* Check for pending messages */
        if (cluster->network.messageCount > 0) {
            nextEvent = cluster->network.messages[0].deliveryTime;
            if (nextEvent > targetTime) {
                nextEvent = targetTime;
            }
        }

        /* Check for timeouts */
        for (uint32_t i = 0; i < cluster->nodeCount; i++) {
            if (!cluster->nodes[i].crashed && !cluster->nodes[i].paused) {
                if (cluster->nodes[i].nextElectionTimeout < nextEvent) {
                    nextEvent = cluster->nodes[i].nextElectionTimeout;
                }
                /* Only consider heartbeat timeout if node is actually a leader
                 */
                if (IS_LEADER(cluster->nodes[i].state->self) &&
                    cluster->nodes[i].nextHeartbeatTimeout < nextEvent) {
                    nextEvent = cluster->nodes[i].nextHeartbeatTimeout;
                }
            }
        }

        /* Advance time to next event */
        cluster->currentTime = nextEvent;

        /* Process events */
        processTimeouts(cluster);
        processMessages(cluster);

        /* Check invariants periodically */
        if (cluster->enableInvariantChecking &&
            cluster->currentTime - cluster->lastInvariantCheck >=
                cluster->invariantCheckInterval) {
            testClusterCheckInvariants(cluster);
            cluster->lastInvariantCheck = cluster->currentTime;
        }
    }
}

void testClusterRunUntil(testCluster *cluster, bool (*condition)(testCluster *),
                         uint64_t maxTime) {
    uint64_t startTime = cluster->currentTime;

    while (!condition(cluster)) {
        testClusterTick(cluster, 1000); /* Tick in 1ms increments */

        if (cluster->currentTime - startTime >= maxTime) {
            fprintf(stderr,
                    "NOTICE: Condition not met within %" PRIu64
                    " us - continuing anyway\n",
                    maxTime);
            fprintf(stderr, "  (This is not an error - consider increasing "
                            "timeout if this happens frequently)\n");
            fflush(stderr);
            break;
        }
    }
}

static bool hasLeader(testCluster *cluster) {
    return testClusterGetLeader(cluster) != UINT32_MAX;
}

void testClusterRunUntilLeaderElected(testCluster *cluster, uint64_t maxTime) {
    testClusterRunUntil(cluster, hasLeader, maxTime);
}

void testClusterRunUntilQuiet(testCluster *cluster, uint64_t quietPeriod) {
    uint64_t lastActivity = cluster->currentTime;
    uint64_t lastMessageCount = cluster->network.totalDelivered;

    while (cluster->currentTime - lastActivity < quietPeriod) {
        testClusterTick(cluster, 1000);

        if (cluster->network.totalDelivered != lastMessageCount) {
            lastActivity = cluster->currentTime;
            lastMessageCount = cluster->network.totalDelivered;
        }
    }
}

/* ====================================================================
 * State Inspection
 * ==================================================================== */
uint32_t testClusterGetLeader(testCluster *cluster) {
    uint32_t leader = UINT32_MAX;
    kraftTerm leaderTerm = 0;

    for (uint32_t i = 0; i < cluster->nodeCount; i++) {
        if (cluster->nodes[i].crashed) {
            continue;
        }

        kraftState *state = cluster->nodes[i].state;
        if (IS_LEADER(state->self)) {
            if (state->self->consensus.term > leaderTerm) {
                leader = i;
                leaderTerm = state->self->consensus.term;
            } else if (state->self->consensus.term == leaderTerm &&
                       leader != UINT32_MAX) {
                /* Multiple leaders in same term! */
                fprintf(stderr, "ERROR: Multiple leaders in term %" PRIu64 "\n",
                        leaderTerm);
                return UINT32_MAX;
            }
        }
    }

    return leader;
}

uint64_t testClusterGetMaxTerm(testCluster *cluster) {
    kraftTerm maxTerm = 0;

    for (uint32_t i = 0; i < cluster->nodeCount; i++) {
        if (cluster->nodes[i].crashed) {
            continue;
        }

        kraftTerm term = cluster->nodes[i].state->self->consensus.term;
        if (term > maxTerm) {
            maxTerm = term;
        }
    }

    return maxTerm;
}

bool testClusterHasConsistentLeader(testCluster *cluster) {
    uint32_t leader = testClusterGetLeader(cluster);
    if (leader == UINT32_MAX) {
        return false;
    }

    /* Check that all nodes agree */
    kraftTerm leaderTerm = cluster->nodes[leader].state->self->consensus.term;

    for (uint32_t i = 0; i < cluster->nodeCount; i++) {
        if (i == leader || cluster->nodes[i].crashed) {
            continue;
        }

        kraftState *state = cluster->nodes[i].state;
        if (IS_LEADER(state->self) &&
            state->self->consensus.term == leaderTerm) {
            return false; /* Another leader in same term */
        }
    }

    return true;
}

/* ====================================================================
 * Invariant Checking
 * ==================================================================== */
bool testInvariantElectionSafety(testCluster *cluster) {
    /* At most one leader per term */

    uint32_t leaderCount[1000] = {0}; /* Assume max 1000 terms */
    uint32_t leaderNodes[1000] = {0}; /* Track which node is leader */

    for (uint32_t i = 0; i < cluster->nodeCount; i++) {
        if (cluster->nodes[i].crashed) {
            continue;
        }

        kraftState *state = cluster->nodes[i].state;
        if (IS_LEADER(state->self)) {
            kraftTerm term = state->self->consensus.term;
            if (term < 1000) {
                if (leaderCount[term] == 0) {
                    leaderNodes[term] = i;
                }
                leaderCount[term]++;

                if (leaderCount[term] > 1) {
                    fprintf(
                        stderr,
                        "INVARIANT VIOLATION: Multiple leaders in term %" PRIu64
                        "\n",
                        term);
                    fprintf(
                        stderr,
                        "  Node %u and Node %u both think they are leader\n",
                        leaderNodes[term], i);
                    fprintf(stderr,
                            "  Node %u: votedFor=%" PRIu64 ", votes(old)=%u\n",
                            leaderNodes[term],
                            (uint64_t)KRAFT_VOTED_FOR(
                                cluster->nodes[leaderNodes[term]].state),
                            vecCount(KRAFT_SELF_VOTES_FROM_OLD(
                                cluster->nodes[leaderNodes[term]].state)));
                    fprintf(stderr,
                            "  Node %u: votedFor=%" PRIu64 ", votes(old)=%u\n",
                            i, (uint64_t)KRAFT_VOTED_FOR(state),
                            vecCount(KRAFT_SELF_VOTES_FROM_OLD(state)));
                    return false;
                }
            }
        }
    }

    return true;
}

/* ====================================================================
 * Leader Append-Only Invariant
 *
 * Raft Safety Property #2:
 * "A leader never overwrites or deletes entries in its log;
 *  it only appends new entries."
 *
 * Implementation:
 * Track the highest log index for each node when it's a leader.
 * If a node becomes leader again, verify all previously logged
 * entries are still present with same terms.
 * ==================================================================== */
bool testInvariantLeaderAppendOnly(testCluster *cluster) {
    for (uint32_t i = 0; i < cluster->nodeCount; i++) {
        if (cluster->nodes[i].crashed) {
            continue;
        }

        kraftState *state = cluster->nodes[i].state;

        /* Get current max index for this node */
        kraftRpcUniqueIndex currentMax = 0;
        kvidxMaxKey(KRAFT_ENTRIES(state), &currentMax);

        /* If this node is a leader, check against historical high water mark */
        if (IS_LEADER(state->self)) {
            /* Check if this node's term has changed since last check.
             * If so, reset high water mark - this is a new leadership period.
             * Also clear the global highWaterTerm entries for this node's
             * range. */
            kraftTerm prevNodeTerm =
                cluster->appendOnlyTracking.highWaterNodeTerm[i];
            if (prevNodeTerm != state->self->consensus.term) {
                kraftRpcUniqueIndex prevMax =
                    cluster->appendOnlyTracking.highWaterMark[i];
                D("Node %u term changed from %" PRIu64 " to %" PRIu64
                  ", resetting high water mark (was %" PRIu64 ")",
                  i, prevNodeTerm, state->self->consensus.term, prevMax);

                /* Clear highWaterTerm for this node's previous range */
                for (kraftRpcUniqueIndex idx = 1; idx <= prevMax && idx < 10000;
                     idx++) {
                    cluster->appendOnlyTracking.highWaterTerm[i][idx] = 0;
                }

                cluster->appendOnlyTracking.highWaterMark[i] = 0;
                cluster->appendOnlyTracking.highWaterNodeTerm[i] =
                    state->self->consensus.term;
            }

            kraftRpcUniqueIndex previousMax =
                cluster->appendOnlyTracking.highWaterMark[i];

            /* Verify all entries up to previous max still exist */
            for (kraftRpcUniqueIndex idx = 1; idx <= previousMax; idx++) {
                kraftTerm oldTerm =
                    cluster->appendOnlyTracking.highWaterTerm[i][idx];
                if (oldTerm == 0) {
                    continue; /* Not tracked yet */
                }

                kraftTerm currentTerm;
                const uint8_t *entry;
                size_t len;
                uint64_t cmd;

                bool exists = kvidxGet(KRAFT_ENTRIES(state), idx, &currentTerm,
                                       &cmd, &entry, &len);

                if (!exists) {
                    fprintf(stderr,
                            "INVARIANT VIOLATION: Leader Append-Only\n");
                    fprintf(stderr,
                            "  Node %u (leader) deleted entry at index %" PRIu64
                            "\n",
                            i, idx);
                    fprintf(stderr,
                            "  Entry existed in term %" PRIu64
                            " but is now missing\n",
                            oldTerm);
                    return false;
                }

                if (currentTerm != oldTerm) {
                    fprintf(stderr,
                            "INVARIANT VIOLATION: Leader Append-Only\n");
                    fprintf(
                        stderr,
                        "  Node %u (leader) modified entry at index %" PRIu64
                        "\n",
                        i, idx);
                    fprintf(stderr,
                            "  Term changed from %" PRIu64 " to %" PRIu64 "\n",
                            oldTerm, currentTerm);

                    /* DEBUG: Dump node state */
                    fprintf(stderr, "\nDEBUG: Node %u state:\n", i);
                    fprintf(stderr, "  Term: %" PRIu64 "\n",
                            state->self->consensus.term);
                    fprintf(stderr,
                            "  PreviousMax: %" PRIu64 ", CurrentMax: %" PRIu64
                            "\n",
                            previousMax, currentMax);
                    fprintf(stderr, "  Commit index: %" PRIu64 "\n",
                            state->commitIndex);
                    fprintf(stderr,
                            "  Current index: %" PRIu64
                            ", Current term: %" PRIu64 "\n",
                            state->self->consensus.currentIndex,
                            state->self->consensus.currentTerm);

                    /* Dump first 10 log entries */
                    fprintf(stderr, "  First 10 log entries:\n");
                    for (kraftRpcUniqueIndex logIdx = 1;
                         logIdx <= 10 && logIdx <= currentMax; logIdx++) {
                        kraftTerm logTerm;
                        const uint8_t *logEntry;
                        size_t logLen;
                        uint64_t logCmd;
                        bool logExists =
                            kvidxGet(KRAFT_ENTRIES(state), logIdx, &logTerm,
                                     &logCmd, &logEntry, &logLen);
                        fprintf(stderr,
                                "    [%" PRIu64 "]: exists=%d, term=%" PRIu64
                                ", highWaterTerm=%" PRIu64 "\n",
                                logIdx, logExists, logExists ? logTerm : 0,
                                logIdx < 10000 ? cluster->appendOnlyTracking
                                                     .highWaterTerm[i][logIdx]
                                               : 0);
                    }

                    uint32_t oldNodesCount =
                        vecPtrCount(NODES_OLD_NODES(state));
                    uint32_t oldPendingCount =
                        vecPtrCount(NODES_OLD_PENDING(state));
                    uint32_t newNodesCount =
                        vecPtrCount(NODES_NEW_NODES(state));
                    uint32_t newPendingCount =
                        vecPtrCount(NODES_NEW_PENDING(state));
                    uint32_t votesOldCount =
                        vecCount(KRAFT_SELF_VOTES_FROM_OLD(state));
                    uint32_t votesNewCount =
                        vecCount(KRAFT_SELF_VOTES_FROM_NEW(state));

                    fprintf(stderr, "  OLD nodes: %u, OLD pending: %u\n",
                            oldNodesCount, oldPendingCount);
                    fprintf(stderr, "  NEW nodes: %u, NEW pending: %u\n",
                            newNodesCount, newPendingCount);
                    fprintf(stderr, "  Votes OLD: %u, Votes NEW: %u\n",
                            votesOldCount, votesNewCount);

                    return false;
                }
            }

            /* Update tracking for this leader */
            if (currentMax > previousMax) {
                cluster->appendOnlyTracking.highWaterMark[i] = currentMax;

                /* Record terms for new entries */
                for (kraftRpcUniqueIndex idx = previousMax + 1;
                     idx <= currentMax; idx++) {
                    kraftTerm term;
                    const uint8_t *entry;
                    size_t len;
                    uint64_t cmd;

                    if (kvidxGet(KRAFT_ENTRIES(state), idx, &term, &cmd, &entry,
                                 &len)) {
                        if (idx < 10000) {   /* Bounded tracking */
                            if (idx <= 10) { /* Debug first 10 entries */
                                D("Node %u: Recording highWaterTerm[%" PRIu64
                                  "] = %" PRIu64 " (was %" PRIu64 ")",
                                  i, idx, term,
                                  cluster->appendOnlyTracking
                                      .highWaterTerm[i][idx]);
                            }
                            cluster->appendOnlyTracking.highWaterTerm[i][idx] =
                                term;
                        }
                    }
                }
            }
        }
    }

    return true;
}

bool testInvariantLogMatching(testCluster *cluster) {
    /* If two logs contain entry with same index and term,
     * all preceding entries must be identical */

    for (uint32_t i = 0; i < cluster->nodeCount; i++) {
        if (cluster->nodes[i].crashed) {
            continue;
        }

        for (uint32_t j = i + 1; j < cluster->nodeCount; j++) {
            if (cluster->nodes[j].crashed) {
                continue;
            }

            kraftState *state1 = cluster->nodes[i].state;
            kraftState *state2 = cluster->nodes[j].state;

            /* Compare logs */
            kraftRpcUniqueIndex maxIdx;
            if (!kvidxMaxKey(KRAFT_ENTRIES(state1), &maxIdx)) {
                continue;
            }

            for (kraftRpcUniqueIndex idx = 1; idx <= maxIdx; idx++) {
                kraftTerm term1, term2;
                const uint8_t *entry1, *entry2;
                size_t len1, len2;
                uint64_t cmd1, cmd2;

                bool exists1 = kvidxGet(KRAFT_ENTRIES(state1), idx, &term1,
                                        &cmd1, &entry1, &len1);
                bool exists2 = kvidxGet(KRAFT_ENTRIES(state2), idx, &term2,
                                        &cmd2, &entry2, &len2);

                if (exists1 && exists2) {
                    if (term1 == term2) {
                        /* Terms match - entries must be identical */
                        if (len1 != len2 || memcmp(entry1, entry2, len1) != 0) {
                            fprintf(stderr,
                                    "INVARIANT VIOLATION: Log Matching failed "
                                    "at index %" PRIu64 "\n",
                                    idx);
                            return false;
                        }
                    }
                }
            }
        }
    }

    return true;
}

/* ====================================================================
 * Leader Completeness Invariant
 *
 * Raft Safety Property #4:
 * "If a log entry is committed in a given term, then that entry will
 *  be present in the logs of the leaders for all higher-numbered terms."
 *
 * Implementation:
 * For each node, track entries that have been committed (replicated to
 * majority). Verify that all future leaders contain these entries.
 * ==================================================================== */
bool testInvariantLeaderCompleteness(testCluster *cluster) {
    /* Build set of committed entries across all nodes */
    struct {
        kraftRpcUniqueIndex index;
        kraftTerm term;
        uint64_t commandHash;
    } committedEntries[1000];
    uint32_t committedCount = 0;

    /* Collect committed entries from all nodes */
    for (uint32_t i = 0; i < cluster->nodeCount; i++) {
        if (cluster->nodes[i].crashed) {
            continue;
        }

        kraftState *state = cluster->nodes[i].state;
        kraftRpcUniqueIndex commitIdx = state->commitIndex;

        /* Get all committed entries */
        for (kraftRpcUniqueIndex idx = 1; idx <= commitIdx && idx <= 1000;
             idx++) {
            kraftTerm term;
            const uint8_t *entry;
            size_t len;
            uint64_t cmd;

            if (kvidxGet(KRAFT_ENTRIES(state), idx, &term, &cmd, &entry,
                         &len)) {
                /* Check if we've already recorded this */
                bool found = false;
                for (uint32_t c = 0; c < committedCount; c++) {
                    if (committedEntries[c].index == idx) {
                        /* Verify term matches */
                        if (committedEntries[c].term != term) {
                            fprintf(stderr, "INVARIANT VIOLATION: Committed "
                                            "entries have different terms\n");
                            fprintf(stderr,
                                    "  Index %" PRIu64 ": term %" PRIu64
                                    " vs %" PRIu64 "\n",
                                    idx, committedEntries[c].term, term);
                            return false;
                        }
                        found = true;
                        break;
                    }
                }

                if (!found && committedCount < 1000) {
                    committedEntries[committedCount].index = idx;
                    committedEntries[committedCount].term = term;
                    /* Simple hash of entry data */
                    committedEntries[committedCount].commandHash = cmd;
                    committedCount++;
                }
            }
        }
    }

    /* Verify all leaders contain all committed entries from earlier terms */
    for (uint32_t i = 0; i < cluster->nodeCount; i++) {
        if (cluster->nodes[i].crashed) {
            continue;
        }

        kraftState *state = cluster->nodes[i].state;
        if (!IS_LEADER(state->self)) {
            continue;
        }

        kraftTerm leaderTerm = state->self->consensus.term;

        /* Check that leader contains all committed entries from prior terms */
        for (uint32_t c = 0; c < committedCount; c++) {
            if (committedEntries[c].term < leaderTerm) {
                kraftTerm term;
                const uint8_t *entry;
                size_t len;
                uint64_t cmd;

                /* Entry is "present" if it's in the log OR was included in a
                 * snapshot. When log compaction occurs, entries at or below
                 * snapshot.lastIncludedIndex are removed from the log but
                 * are still considered part of the leader's state. */
                bool inSnapshot = committedEntries[c].index <=
                                  state->snapshot.lastIncludedIndex;
                bool exists = inSnapshot || kvidxGet(KRAFT_ENTRIES(state),
                                                     committedEntries[c].index,
                                                     &term, &cmd, &entry, &len);

                if (!exists) {
                    fprintf(stderr,
                            "INVARIANT VIOLATION: Leader Completeness\n");
                    fprintf(stderr,
                            "  Leader (node %u, term %" PRIu64
                            ") missing committed entry\n",
                            i, leaderTerm);
                    fprintf(stderr,
                            "  Missing: index %" PRIu64 ", term %" PRIu64 "\n",
                            committedEntries[c].index,
                            committedEntries[c].term);
                    fprintf(stderr,
                            "  Snapshot lastIncludedIndex: %" PRIu64 "\n",
                            state->snapshot.lastIncludedIndex);

                    /* Dump all nodes' state to help debug */
                    fprintf(stderr, "\nAll nodes state:\n");
                    for (uint32_t n = 0; n < cluster->nodeCount; n++) {
                        if (cluster->nodes[n].crashed) {
                            fprintf(stderr, "  Node %u: CRASHED\n", n);
                            continue;
                        }
                        kraftState *nstate = cluster->nodes[n].state;
                        kraftRpcUniqueIndex maxIdx = 0;
                        kvidxMaxKey(KRAFT_ENTRIES(nstate), &maxIdx);

                        kraftTerm nterm = 0;
                        const uint8_t *nentry;
                        size_t nlen;
                        uint64_t ncmd;
                        bool nexists = kvidxGet(KRAFT_ENTRIES(nstate),
                                                committedEntries[c].index,
                                                &nterm, &ncmd, &nentry, &nlen);
                        fprintf(
                            stderr,
                            "  Node %u: term=%" PRIu64 ", commitIdx=%" PRIu64
                            ", maxIdx=%" PRIu64 ", snapshotIdx=%" PRIu64
                            ", role=%s, hasEntry=%d, entryTerm=%" PRIu64 "\n",
                            n, nstate->self->consensus.term,
                            nstate->commitIndex, maxIdx,
                            nstate->snapshot.lastIncludedIndex,
                            kraftRoles[nstate->self->consensus.role], nexists,
                            nterm);
                        /* Dump full log for debugging */
                        if (maxIdx > 0 && maxIdx <= 20) {
                            D("    Log entries for node %u:", n);
                            for (uint64_t logIdx = 1; logIdx <= maxIdx;
                                 logIdx++) {
                                kraftTerm logTerm = 0;
                                uint64_t logCmd;
                                const uint8_t *logData;
                                size_t logLen;
                                if (kvidxGet(KRAFT_ENTRIES(nstate), logIdx,
                                             &logTerm, &logCmd, &logData,
                                             &logLen)) {
                                    D("      [%" PRIu64 ":t%" PRIu64 "]",
                                      logIdx, logTerm);
                                } else {
                                    D("      [%" PRIu64 ":MISSING]", logIdx);
                                }
                            }
                        }
                    }
                    return false;
                }

                /* Skip term verification for entries in snapshot - we can't
                 * retrieve the term from compacted log entries. The snapshot
                 * metadata only stores the lastIncludedTerm, not individual
                 * entry terms. Trust that snapshot was correctly created. */
                if (!inSnapshot && term != committedEntries[c].term) {
                    fprintf(stderr,
                            "INVARIANT VIOLATION: Leader Completeness\n");
                    fprintf(
                        stderr,
                        "  Leader has different term for committed entry\n");
                    fprintf(stderr,
                            "  Index %" PRIu64 ": expected term %" PRIu64
                            ", got %" PRIu64 "\n",
                            committedEntries[c].index, committedEntries[c].term,
                            term);
                    fprintf(stderr,
                            "\nDEBUG: Leader node %u, term %" PRIu64
                            ", commitIndex %" PRIu64 "\n",
                            i, leaderTerm, state->commitIndex);

                    /* Dump all nodes' state for this index */
                    fprintf(stderr, "All nodes state for index %" PRIu64 ":\n",
                            committedEntries[c].index);
                    for (uint32_t n = 0; n < cluster->nodeCount; n++) {
                        if (cluster->nodes[n].crashed) {
                            fprintf(stderr, "  Node %u: CRASHED\n", n);
                            continue;
                        }
                        kraftState *nstate = cluster->nodes[n].state;
                        kraftRpcUniqueIndex maxIdx = 0;
                        kvidxMaxKey(KRAFT_ENTRIES(nstate), &maxIdx);

                        kraftTerm nterm;
                        const uint8_t *nentry;
                        size_t nlen;
                        uint64_t ncmd;
                        bool nexists = kvidxGet(KRAFT_ENTRIES(nstate),
                                                committedEntries[c].index,
                                                &nterm, &ncmd, &nentry, &nlen);
                        fprintf(stderr,
                                "  Node %u: term=%" PRIu64
                                ", commitIdx=%" PRIu64 ", maxIdx=%" PRIu64
                                ", role=%s, exists=%d, entryTerm=%" PRIu64 "\n",
                                n, nstate->self->consensus.term,
                                nstate->commitIndex, maxIdx,
                                kraftRoles[nstate->self->consensus.role],
                                nexists, nexists ? nterm : 0);

                        /* Show first few entries for leader */
                        if (n == i) {
                            fprintf(stderr,
                                    "    Leader's log (first 10 entries):\n");
                            for (kraftRpcUniqueIndex idx = 1;
                                 idx <= 10 && idx <= maxIdx; idx++) {
                                kraftTerm eterm;
                                const uint8_t *eentry;
                                size_t elen;
                                uint64_t ecmd;
                                bool eexists =
                                    kvidxGet(KRAFT_ENTRIES(nstate), idx, &eterm,
                                             &ecmd, &eentry, &elen);
                                fprintf(stderr,
                                        "      [%" PRIu64
                                        "]: exists=%d, term=%" PRIu64 "\n",
                                        idx, eexists, eexists ? eterm : 0);
                            }
                        }
                    }
                    return false;
                }
            }
        }
    }

    return true;
}

/* ====================================================================
 * State Machine Safety Invariant
 *
 * Raft Safety Property #5:
 * "If a server has applied a log entry at a given index to its state
 *  machine, no other server will ever apply a different log entry for
 *  the same index."
 *
 * Implementation:
 * Track the command hash for each applied log index globally.
 * Verify that all nodes apply the same command at each index.
 * ==================================================================== */
bool testInvariantStateMachineSafety(testCluster *cluster) {
    /* Ensure tracking array is large enough */
    for (uint32_t i = 0; i < cluster->nodeCount; i++) {
        if (cluster->nodes[i].crashed) {
            continue;
        }

        kraftState *state = cluster->nodes[i].state;
        kraftRpcUniqueIndex lastApplied = state->lastApplied;

        /* Expand tracking array if needed */
        if (lastApplied > cluster->stateMachineHistory.capacity) {
            uint32_t newCapacity = lastApplied + 1000;
            cluster->stateMachineHistory.commandHashes =
                realloc(cluster->stateMachineHistory.commandHashes,
                        newCapacity * sizeof(uint64_t));
            /* Zero new entries */
            for (uint32_t j = cluster->stateMachineHistory.capacity;
                 j < newCapacity; j++) {
                cluster->stateMachineHistory.commandHashes[j] = 0;
            }
            cluster->stateMachineHistory.capacity = newCapacity;
        }

        /* Check all applied entries */
        for (kraftRpcUniqueIndex idx = 1; idx <= lastApplied; idx++) {
            /* Skip entries in snapshot (already applied) */
            if (idx <= state->snapshot.lastIncludedIndex) {
                continue;
            }

            kraftTerm term;
            const uint8_t *entry;
            size_t len;
            uint64_t cmd;

            if (kvidxGet(KRAFT_ENTRIES(state), idx, &term, &cmd, &entry,
                         &len)) {
                /* Compute simple hash of command */
                uint64_t hash = cmd;
                for (size_t b = 0; b < len && b < 64; b++) {
                    hash ^= ((uint64_t)entry[b]) << (b % 8);
                }

                if (cluster->stateMachineHistory.commandHashes[idx] == 0) {
                    /* First time seeing this index - record it */
                    cluster->stateMachineHistory.commandHashes[idx] = hash;
                    if (idx > cluster->stateMachineHistory.maxIndex) {
                        cluster->stateMachineHistory.maxIndex = idx;
                    }
                } else {
                    /* Verify it matches what we saw before */
                    if (cluster->stateMachineHistory.commandHashes[idx] !=
                        hash) {
                        fprintf(stderr,
                                "INVARIANT VIOLATION: State Machine Safety\n");
                        fprintf(stderr,
                                "  Node %u applied different command at index "
                                "%" PRIu64 "\n",
                                i, idx);
                        fprintf(
                            stderr, "  Expected hash: %016" PRIx64 "\n",
                            cluster->stateMachineHistory.commandHashes[idx]);
                        fprintf(stderr, "  Got hash:      %016" PRIx64 "\n",
                                hash);
                        fprintf(stderr,
                                "  Term: %" PRIu64 ", Cmd: %" PRIu64
                                ", Len: %zu\n",
                                term, cmd, len);
                        return false;
                    }
                }
            }
        }
    }

    return true;
}

/* ====================================================================
 * Performance Profiling & Benchmarking
 *
 * Comprehensive performance measurement system for analyzing Raft
 * protocol performance characteristics.
 * ==================================================================== */

uint64_t testGetWallTimeUs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000UL + (uint64_t)ts.tv_nsec / 1000UL;
}

void testPerfEnable(testCluster *cluster) {
    cluster->perfMetrics.enabled = true;
    cluster->perfMetrics.profilingStartTime = cluster->currentTime;

    /* Allocate sample arrays if not already allocated */
    if (!cluster->perfMetrics.rpcLatency.samples) {
        cluster->perfMetrics.rpcLatency.samples =
            calloc(10000, sizeof(uint64_t));
        cluster->perfMetrics.rpcLatency.capacity = 10000;
        cluster->perfMetrics.rpcLatency.min = UINT64_MAX;
    }

    if (!cluster->perfMetrics.commitLatency.samples) {
        cluster->perfMetrics.commitLatency.samples =
            calloc(10000, sizeof(uint64_t));
        cluster->perfMetrics.commitLatency.capacity = 10000;
        cluster->perfMetrics.commitLatency.min = UINT64_MAX;
    }

    if (!cluster->perfMetrics.electionLatency.samples) {
        cluster->perfMetrics.electionLatency.samples =
            calloc(1000, sizeof(uint64_t));
        cluster->perfMetrics.electionLatency.capacity = 1000;
        cluster->perfMetrics.electionLatency.min = UINT64_MAX;
    }
}

void testPerfDisable(testCluster *cluster) {
    cluster->perfMetrics.enabled = false;
    cluster->perfMetrics.profilingEndTime = cluster->currentTime;
}

static void recordLatencySample(
    struct {
        uint64_t *samples;
        uint32_t count;
        uint32_t capacity;
        uint64_t sum;
        uint64_t min;
        uint64_t max;
    } * latency,
    uint64_t sample) {
    if (latency->count < latency->capacity) {
        latency->samples[latency->count++] = sample;
        latency->sum += sample;
        if (sample < latency->min) {
            latency->min = sample;
        }
        if (sample > latency->max) {
            latency->max = sample;
        }
    }
}

void testPerfRecordRpcLatency(testCluster *cluster, uint64_t latencyUs) {
    if (!cluster->perfMetrics.enabled) {
        return;
    }
    recordLatencySample(&cluster->perfMetrics.rpcLatency, latencyUs);
    cluster->perfMetrics.rpcProcessed++;
}

void testPerfRecordCommitLatency(testCluster *cluster, uint64_t latencyUs) {
    if (!cluster->perfMetrics.enabled) {
        return;
    }
    recordLatencySample(&cluster->perfMetrics.commitLatency, latencyUs);
    cluster->perfMetrics.commandsCommitted++;
}

void testPerfRecordElectionLatency(testCluster *cluster, uint64_t latencyUs) {
    if (!cluster->perfMetrics.enabled) {
        return;
    }
    recordLatencySample(&cluster->perfMetrics.electionLatency, latencyUs);
}

/* Comparison function for qsort */
static int compareUint64(const void *a, const void *b) {
    uint64_t ia = *(const uint64_t *)a;
    uint64_t ib = *(const uint64_t *)b;
    return (ia > ib) - (ia < ib);
}

/* Calculate percentile from sorted samples */
static uint64_t calculatePercentile(uint64_t *samples, uint32_t count,
                                    double percentile) {
    if (count == 0) {
        return 0;
    }

    /* Sort samples if needed */
    uint64_t *sorted = malloc(count * sizeof(uint64_t));
    memcpy(sorted, samples, count * sizeof(uint64_t));
    qsort(sorted, count, sizeof(uint64_t), compareUint64);

    uint32_t index = (uint32_t)((percentile / 100.0) * (count - 1));
    uint64_t result = sorted[index];
    free(sorted);
    return result;
}

testPerfStats testPerfGetStats(testCluster *cluster) {
    testPerfStats stats = {0};

    uint64_t durationUs =
        cluster->perfMetrics.profilingEndTime > 0
            ? cluster->perfMetrics.profilingEndTime -
                  cluster->perfMetrics.profilingStartTime
            : cluster->currentTime - cluster->perfMetrics.profilingStartTime;

    stats.durationUs = durationUs;
    double durationSec = durationUs / 1000000.0;

    /* RPC statistics */
    if (cluster->perfMetrics.rpcLatency.count > 0) {
        stats.rpcAvgLatencyUs = cluster->perfMetrics.rpcLatency.sum /
                                cluster->perfMetrics.rpcLatency.count;
        stats.rpcMinLatencyUs = cluster->perfMetrics.rpcLatency.min;
        stats.rpcMaxLatencyUs = cluster->perfMetrics.rpcLatency.max;
        stats.rpcP50LatencyUs =
            calculatePercentile(cluster->perfMetrics.rpcLatency.samples,
                                cluster->perfMetrics.rpcLatency.count, 50.0);
        stats.rpcP95LatencyUs =
            calculatePercentile(cluster->perfMetrics.rpcLatency.samples,
                                cluster->perfMetrics.rpcLatency.count, 95.0);
        stats.rpcP99LatencyUs =
            calculatePercentile(cluster->perfMetrics.rpcLatency.samples,
                                cluster->perfMetrics.rpcLatency.count, 99.0);
    }

    stats.totalRpcs = cluster->perfMetrics.rpcProcessed;
    if (durationSec > 0) {
        stats.rpcThroughput =
            (uint64_t)(cluster->perfMetrics.rpcProcessed / durationSec);
    }

    /* Commit statistics */
    if (cluster->perfMetrics.commitLatency.count > 0) {
        stats.commitAvgLatencyUs = cluster->perfMetrics.commitLatency.sum /
                                   cluster->perfMetrics.commitLatency.count;
        stats.commitMinLatencyUs = cluster->perfMetrics.commitLatency.min;
        stats.commitMaxLatencyUs = cluster->perfMetrics.commitLatency.max;
        stats.commitP50LatencyUs =
            calculatePercentile(cluster->perfMetrics.commitLatency.samples,
                                cluster->perfMetrics.commitLatency.count, 50.0);
        stats.commitP95LatencyUs =
            calculatePercentile(cluster->perfMetrics.commitLatency.samples,
                                cluster->perfMetrics.commitLatency.count, 95.0);
        stats.commitP99LatencyUs =
            calculatePercentile(cluster->perfMetrics.commitLatency.samples,
                                cluster->perfMetrics.commitLatency.count, 99.0);
    }

    stats.totalCommits = cluster->perfMetrics.commandsCommitted;
    if (durationSec > 0) {
        stats.commitThroughput =
            (uint64_t)(cluster->perfMetrics.commandsCommitted / durationSec);
    }

    /* Election statistics */
    if (cluster->perfMetrics.electionLatency.count > 0) {
        stats.electionAvgLatencyUs = cluster->perfMetrics.electionLatency.sum /
                                     cluster->perfMetrics.electionLatency.count;
        stats.electionMinLatencyUs = cluster->perfMetrics.electionLatency.min;
        stats.electionMaxLatencyUs = cluster->perfMetrics.electionLatency.max;
    }

    stats.totalElections = cluster->totalElections;

    /* Network statistics */
    stats.networkThroughputBytesPerSec =
        durationSec > 0
            ? (uint64_t)(cluster->perfMetrics.bytesTransferred / durationSec)
            : 0;
    stats.messagesPerSecond =
        durationSec > 0
            ? (uint64_t)(cluster->network.totalDelivered / durationSec)
            : 0;

    return stats;
}

void testPerfPrintReport(testCluster *cluster) {
    testPerfStats stats = testPerfGetStats(cluster);

    printf("\n");
    printf("==================================================================="
           "=\n");
    printf("Performance Report\n");
    printf("==================================================================="
           "=\n");
    printf("Test Duration: %.3f seconds (%" PRIu64 " us)\n",
           stats.durationUs / 1000000.0, stats.durationUs);
    printf("\n");

    /* RPC Performance */
    printf("RPC Performance:\n");
    printf("  Total RPCs:        %" PRIu64 "\n", stats.totalRpcs);
    printf("  Throughput:        %" PRIu64 " RPCs/sec\n", stats.rpcThroughput);
    if (stats.rpcAvgLatencyUs > 0) {
        printf("  Latency (avg):     %" PRIu64 " us\n", stats.rpcAvgLatencyUs);
        printf("  Latency (min):     %" PRIu64 " us\n", stats.rpcMinLatencyUs);
        printf("  Latency (max):     %" PRIu64 " us\n", stats.rpcMaxLatencyUs);
        printf("  Latency (p50):     %" PRIu64 " us\n", stats.rpcP50LatencyUs);
        printf("  Latency (p95):     %" PRIu64 " us\n", stats.rpcP95LatencyUs);
        printf("  Latency (p99):     %" PRIu64 " us\n", stats.rpcP99LatencyUs);
    }
    printf("\n");

    /* Commit Performance */
    printf("Commit Performance:\n");
    printf("  Total Commits:     %" PRIu64 "\n", stats.totalCommits);
    printf("  Throughput:        %" PRIu64 " commits/sec\n",
           stats.commitThroughput);
    if (stats.commitAvgLatencyUs > 0) {
        printf("  Latency (avg):     %" PRIu64 " us\n",
               stats.commitAvgLatencyUs);
        printf("  Latency (min):     %" PRIu64 " us\n",
               stats.commitMinLatencyUs);
        printf("  Latency (max):     %" PRIu64 " us\n",
               stats.commitMaxLatencyUs);
        printf("  Latency (p50):     %" PRIu64 " us\n",
               stats.commitP50LatencyUs);
        printf("  Latency (p95):     %" PRIu64 " us\n",
               stats.commitP95LatencyUs);
        printf("  Latency (p99):     %" PRIu64 " us\n",
               stats.commitP99LatencyUs);
    }
    printf("\n");

    /* Election Performance */
    printf("Election Performance:\n");
    printf("  Total Elections:   %" PRIu64 "\n", stats.totalElections);
    if (stats.electionAvgLatencyUs > 0) {
        printf("  Latency (avg):     %" PRIu64 " us (%.3f ms)\n",
               stats.electionAvgLatencyUs, stats.electionAvgLatencyUs / 1000.0);
        printf("  Latency (min):     %" PRIu64 " us (%.3f ms)\n",
               stats.electionMinLatencyUs, stats.electionMinLatencyUs / 1000.0);
        printf("  Latency (max):     %" PRIu64 " us (%.3f ms)\n",
               stats.electionMaxLatencyUs, stats.electionMaxLatencyUs / 1000.0);
    }
    printf("\n");

    /* Network Performance */
    printf("Network Performance:\n");
    printf("  Messages Sent:     %" PRIu64 "\n", cluster->network.totalSent);
    printf("  Messages Dropped:  %" PRIu64 "\n", cluster->network.totalDropped);
    printf("  Messages Delivered:%" PRIu64 "\n",
           cluster->network.totalDelivered);
    printf("  Throughput:        %" PRIu64 " msgs/sec\n",
           stats.messagesPerSecond);
    printf("  Bandwidth:         %" PRIu64 " bytes/sec (%.2f KB/s)\n",
           stats.networkThroughputBytesPerSec,
           stats.networkThroughputBytesPerSec / 1024.0);
    printf("\n");

    printf("==================================================================="
           "=\n");
}

void testPerfReset(testCluster *cluster) {
    /* Reset all counters and samples */
    cluster->perfMetrics.rpcLatency.count = 0;
    cluster->perfMetrics.rpcLatency.sum = 0;
    cluster->perfMetrics.rpcLatency.min = UINT64_MAX;
    cluster->perfMetrics.rpcLatency.max = 0;

    cluster->perfMetrics.commitLatency.count = 0;
    cluster->perfMetrics.commitLatency.sum = 0;
    cluster->perfMetrics.commitLatency.min = UINT64_MAX;
    cluster->perfMetrics.commitLatency.max = 0;

    cluster->perfMetrics.electionLatency.count = 0;
    cluster->perfMetrics.electionLatency.sum = 0;
    cluster->perfMetrics.electionLatency.min = UINT64_MAX;
    cluster->perfMetrics.electionLatency.max = 0;

    cluster->perfMetrics.rpcProcessed = 0;
    cluster->perfMetrics.commandsCommitted = 0;
    cluster->perfMetrics.bytesTransferred = 0;

    cluster->perfMetrics.appendEntriesCount = 0;
    cluster->perfMetrics.requestVoteCount = 0;
    cluster->perfMetrics.snapshotCount = 0;

    cluster->perfMetrics.profilingStartTime = cluster->currentTime;
    cluster->perfMetrics.profilingEndTime = 0;
}

/* ====================================================================
 * Comprehensive Invariant Checking
 *
 * Verifies all 5 Raft safety properties:
 * 1. Election Safety - at most one leader per term
 * 2. Leader Append-Only - leaders never delete/modify entries
 * 3. Log Matching - matching entries imply identical histories
 * 4. Leader Completeness - committed entries in all future leaders
 * 5. State Machine Safety - same command at same index on all nodes
 * ==================================================================== */
void testClusterCheckInvariants(testCluster *cluster) {
    if (!cluster->enableInvariantChecking) {
        return;
    }

    /* Check all 5 Raft safety properties */
    if (!testInvariantElectionSafety(cluster)) {
        fprintf(stderr, "Fatal: Election Safety violated\n");
        assert(false);
    }

    if (!testInvariantLeaderAppendOnly(cluster)) {
        fprintf(stderr, "Fatal: Leader Append-Only violated\n");
        assert(false);
    }

    if (!testInvariantLogMatching(cluster)) {
        fprintf(stderr, "Fatal: Log Matching violated\n");
        assert(false);
    }

    if (!testInvariantLeaderCompleteness(cluster)) {
        fprintf(stderr, "Fatal: Leader Completeness violated\n");
        assert(false);
    }

    if (!testInvariantStateMachineSafety(cluster)) {
        fprintf(stderr, "Fatal: State Machine Safety violated\n");
        assert(false);
    }
}

/* ====================================================================
 * Node Control
 * ==================================================================== */
void testNodeCrash(testCluster *cluster, uint32_t nodeId) {
    assert(nodeId < cluster->nodeCount);
    cluster->nodes[nodeId].crashed = true;
}

void testNodeRecover(testCluster *cluster, uint32_t nodeId) {
    assert(nodeId < cluster->nodeCount);
    cluster->nodes[nodeId].crashed = false;

    /* When a node recovers from a crash, it should come back as a follower.
     * This simulates a node restart where it loses its volatile state. */
    kraftState *state = cluster->nodes[nodeId].state;
    if (!IS_FOLLOWER(state->self)) {
        BECOME(FOLLOWER, state->self);
        state->leader = NULL;

        /* Stop leader-specific timers */
        state->internals.timer.heartbeatTimer(state, KRAFT_TIMER_STOP);

        /* Start follower-specific timers */
        state->internals.timer.leaderContactTimer(state, KRAFT_TIMER_START);
        state->internals.timer.electionBackoffTimer(state, KRAFT_TIMER_STOP);
    }

    /* CRITICAL: Refresh previousEntry from storage after recovery.
     * On restart, the node's previousEntry is stale (volatile state lost).
     * Without this refresh, the node will compare votes against stale values,
     * potentially granting votes to candidates with older logs and violating
     * Leader Completeness (Raft Safety Property #4). */
    kraftStateRefreshPreviousEntry(state);

    /* Reset timeouts */
    cluster->nodes[nodeId].nextElectionTimeout =
        cluster->currentTime +
        testRandom(cluster->electionTimeoutMin, cluster->electionTimeoutMax);
}

void testNodePause(testCluster *cluster, uint32_t nodeId) {
    assert(nodeId < cluster->nodeCount);
    cluster->nodes[nodeId].paused = true;
}

void testNodeResume(testCluster *cluster, uint32_t nodeId) {
    assert(nodeId < cluster->nodeCount);
    cluster->nodes[nodeId].paused = false;
}

/* ====================================================================
 * Network Control
 * ==================================================================== */
void testNetworkSetMode(testCluster *cluster, testNetworkMode mode) {
    cluster->network.mode = mode;
}

void testNetworkSetDropRate(testCluster *cluster, double rate) {
    cluster->network.dropRate = rate;
}

void testNetworkSetDelay(testCluster *cluster, uint64_t minUs, uint64_t maxUs) {
    cluster->network.minDelay = minUs;
    cluster->network.maxDelay = maxUs;
}

void testNetworkPartition(testCluster *cluster, uint32_t *group1,
                          uint32_t group1Size, uint32_t *group2,
                          uint32_t group2Size) {
    /* Block communication between group1 and group2 */
    for (uint32_t i = 0; i < group1Size; i++) {
        for (uint32_t j = 0; j < group2Size; j++) {
            cluster->network.partition[group1[i]][group2[j]] = true;
            cluster->network.partition[group2[j]][group1[i]] = true;
        }
    }
}

void testNetworkHeal(testCluster *cluster) {
    memset(cluster->network.partition, 0, sizeof(cluster->network.partition));
}

/* ====================================================================
 * Client Operations
 * ==================================================================== */
bool testClusterSubmitCommand(testCluster *cluster, const void *data,
                              size_t len) {
    uint32_t leader = testClusterGetLeader(cluster);
    if (leader == UINT32_MAX) {
        return false; /* No leader */
    }

    kraftState *state = cluster->nodes[leader].state;

    /* Create RPC with entry */
    kraftRpc rpc = {0};

    /* Populate entry with the command data */
    bool populated = kraftRpcPopulateEntryAppend(
        state, &rpc, KRAFT_STORAGE_CMD_APPEND_ENTRIES, data, len);

    if (!populated) {
        return false;
    }

    /* Send RPC to all followers (leader will also persist locally) */
    bool sent = kraftRpcLeaderSendRpc(state, &rpc);

    /* Clean up RPC resources if needed */
    if (rpc.entry) {
        state->internals.entry.release(&rpc);
    }

    return sent;
}

bool testClusterAwaitCommit(testCluster *cluster, kraftRpcUniqueIndex index,
                            uint64_t timeout) {
    uint64_t startTime = cluster->currentTime;

    while (cluster->currentTime - startTime < timeout) {
        /* Check if majority of nodes have committed the index */
        uint32_t committedCount = 0;

        for (uint32_t i = 0; i < cluster->nodeCount; i++) {
            if (cluster->nodes[i].crashed) {
                continue;
            }

            if (cluster->nodes[i].state->commitIndex >= index) {
                committedCount++;
            }
        }

        if (committedCount >= (cluster->nodeCount / 2) + 1) {
            return true;
        }

        testClusterTick(cluster, 10000); /* Tick 10ms */
    }

    return false; /* Timeout */
}
