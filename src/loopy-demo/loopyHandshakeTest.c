/* loopyHandshakeTest - Session handshake protocol verification
 *
 * Tests the HELLO/VERIFIED/GOAWAY handshake between two nodes.
 *
 * Test scenarios:
 *   1. Normal handshake: two nodes connect and verify
 *   2. Self-connection rejection: node connects to itself
 *   3. Cluster mismatch rejection: different cluster IDs
 */

#include "loopyConnection.h"
#include "loopySession.h"
#include "loopyTimers.h"
#include "loopyTransport.h"

/* loopy headers */
#include "../../deps/loopy/src/loopy.h"
#include "../../deps/loopy/src/loopyTimer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ============================================================================
 * Test Infrastructure
 * ============================================================================
 */

#define TEST_PORT_BASE 19000
#define TEST_TIMEOUT_MS 5000

/* Forward declaration for shared test state */
typedef struct TestState TestState;

typedef struct TestContext {
    loopyLoop *loop;
    loopyTransport *transport;

    /* Sessions */
    loopySession *serverSession; /* Inbound session */
    loopySession *clientSession; /* Outbound session */

    /* Identity */
    loopyNodeId localId;

    /* Test state */
    int serverVerified;
    int clientVerified;
    int serverRejected;
    int clientRejected;
    int messagesReceived;
    char lastMessage[256];

    /* Shutdown */
    loopyTimer *timeoutTimer;
    bool testComplete;
    bool testPassed;

    /* Shared state reference */
    TestState *shared;
} TestContext;

/* Shared state between nodes in a test */
struct TestState {
    loopyLoop *loop;
    TestContext *node1;
    TestContext *node2;
    bool complete;
    int verifiedCount;
    int rejectedCount;
    bool expectRejection; /* For rejection tests */
};

/* Forward declarations */
static void transportCallback(loopyTransport *transport,
                              loopyTransportEvent event, loopyConn *conn,
                              loopyTransportConnType connType, const void *data,
                              size_t len, void *userData);

static void sessionCallback(loopySession *session, loopySessionEvent event,
                            const void *data, size_t len, void *userData);

static bool duplicateCheck(const loopyNodeId *peerId, void *userData);

static void timeoutCallback(loopyLoop *loop, loopyTimer *timer, void *userData);

/* ============================================================================
 * Test Helpers
 * ============================================================================
 */

static uint64_t generateRunId(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return ((uint64_t)ts.tv_sec << 20) ^ (uint64_t)ts.tv_nsec ^
           (uint64_t)rand();
}

static void initContext(TestContext *ctx, loopyLoop *loop, uint64_t nodeId,
                        uint64_t clusterId, int port, TestState *shared) {
    memset(ctx, 0, sizeof(*ctx));
    ctx->loop = loop;
    ctx->localId.nodeId = nodeId;
    ctx->localId.clusterId = clusterId;
    ctx->localId.runId = generateRunId();
    ctx->localId.port = port;
    ctx->shared = shared;
    snprintf(ctx->localId.addr, sizeof(ctx->localId.addr), "127.0.0.1");
}

static bool createTransport(TestContext *ctx, int port) {
    loopyTransportConfig cfg;
    loopyTransportConfigInit(&cfg);
    cfg.port = port;
    cfg.bindAddr = "127.0.0.1";

    ctx->transport =
        loopyTransportCreate(ctx->loop, transportCallback, ctx, &cfg);
    if (!ctx->transport) {
        return false;
    }

    return loopyTransportStart(ctx->transport);
}

/* Check if test completion criteria are met */
static void checkTestComplete(TestState *state) {
    if (state->complete) {
        return;
    }

    if (state->expectRejection) {
        /* For rejection tests, stop when either side rejects */
        if (state->rejectedCount > 0) {
            state->complete = true;
            loopyStop(state->loop);
        }
    } else {
        /* For normal handshake, stop when both sides verified */
        if (state->verifiedCount >= 2) {
            state->complete = true;
            loopyStop(state->loop);
        }
    }
}

/* ============================================================================
 * Callbacks
 * ============================================================================
 */

static void transportCallback(loopyTransport *transport,
                              loopyTransportEvent event, loopyConn *conn,
                              loopyTransportConnType connType, const void *data,
                              size_t len, void *userData) {
    TestContext *ctx = (TestContext *)userData;
    (void)transport;
    (void)data;
    (void)len;

    if (event == LOOPY_TRANSPORT_NEW_CONNECTION ||
        event == LOOPY_TRANSPORT_CONNECTED) {
        bool inbound = (connType == LOOPY_CONN_INBOUND);

        /* Create session */
        loopySessionConfig sessionCfg;
        loopySessionConfigInit(&sessionCfg);
        sessionCfg.localId = ctx->localId;
        sessionCfg.handshakeTimeoutMs = 2000;

        loopySession *session = loopySessionCreate(
            conn, inbound, sessionCallback, ctx, &sessionCfg);
        if (!session) {
            printf("  [ERROR] Failed to create session\n");
            return;
        }

        /* Set duplicate checker */
        loopySessionSetDuplicateCheck(session, duplicateCheck, ctx);

        /* Store session */
        if (inbound) {
            ctx->serverSession = session;
            printf("  [Server] Accepted connection, starting handshake\n");
        } else {
            ctx->clientSession = session;
            printf("  [Client] Connected, starting handshake\n");
        }

        /* Start handshake */
        loopySessionStartHandshake(session);
    }
}

static void sessionCallback(loopySession *session, loopySessionEvent event,
                            const void *data, size_t len, void *userData) {
    TestContext *ctx = (TestContext *)userData;
    bool isServer = (session == ctx->serverSession);
    const char *side = isServer ? "Server" : "Client";

    switch (event) {
    case LOOPY_SESSION_EVENT_CONNECTED:
        printf("  [%s] Session connected\n", side);
        break;

    case LOOPY_SESSION_EVENT_VERIFIED:
        printf("  [%s] Session VERIFIED!\n", side);
        if (isServer) {
            ctx->serverVerified = 1;
        } else {
            ctx->clientVerified = 1;
        }

        /* Print peer info */
        const loopyNodeId *peerId = loopySessionGetPeerId(session);
        if (peerId) {
            printf("  [%s] Peer: nodeId=%llu, runId=%llx\n", side,
                   (unsigned long long)peerId->nodeId,
                   (unsigned long long)peerId->runId);
        }

        /* Update shared state and check if complete */
        if (ctx->shared) {
            ctx->shared->verifiedCount++;
            checkTestComplete(ctx->shared);
        }
        break;

    case LOOPY_SESSION_EVENT_REJECTED:
        printf(
            "  [%s] Session REJECTED: %s\n", side,
            loopySessionRejectReasonName(loopySessionGetRejectReason(session)));
        if (isServer) {
            ctx->serverRejected = 1;
        } else {
            ctx->clientRejected = 1;
        }

        /* Update shared state and check if complete */
        if (ctx->shared) {
            ctx->shared->rejectedCount++;
            checkTestComplete(ctx->shared);
        }
        break;

    case LOOPY_SESSION_EVENT_MESSAGE:
        ctx->messagesReceived++;
        if (len < sizeof(ctx->lastMessage)) {
            memcpy(ctx->lastMessage, data, len);
            ctx->lastMessage[len] = '\0';
        }
        printf("  [%s] Received message (%zu bytes): \"%s\"\n", side, len,
               ctx->lastMessage);

        /* For message test, stop when server (inbound) receives a non-empty
         * message */
        if (ctx->shared && isServer && len > 0 && ctx->messagesReceived > 0) {
            ctx->shared->complete = true;
            loopyStop(ctx->shared->loop);
        }
        break;

    case LOOPY_SESSION_EVENT_DISCONNECTED:
        printf("  [%s] Session disconnected\n", side);
        /* Session's connection is already closed but session memory still needs
         * to be freed. Don't close here - we're still in the callback chain.
         * Leave the reference intact so cleanup code can free the session. */
        break;

    case LOOPY_SESSION_EVENT_ERROR:
        printf("  [%s] Session error\n", side);
        break;
    }
}

static bool duplicateCheck(const loopyNodeId *peerId, void *userData) {
    TestContext *ctx = (TestContext *)userData;

    /* Check if we already have a verified session with this runId */
    if (ctx->serverSession && ctx->serverVerified) {
        const loopyNodeId *existingPeerId =
            loopySessionGetPeerId(ctx->serverSession);
        if (existingPeerId && existingPeerId->runId == peerId->runId) {
            return true; /* Duplicate */
        }
    }
    if (ctx->clientSession && ctx->clientVerified) {
        const loopyNodeId *existingPeerId =
            loopySessionGetPeerId(ctx->clientSession);
        if (existingPeerId && existingPeerId->runId == peerId->runId) {
            return true; /* Duplicate */
        }
    }

    return false;
}

static void timeoutCallback(loopyLoop *loop, loopyTimer *timer,
                            void *userData) {
    TestState *state = (TestState *)userData;
    (void)timer;

    printf("  [TIMEOUT] Test timed out!\n");
    state->complete = true;
    loopyStop(loop);
}

/* ============================================================================
 * Test 1: Normal Handshake
 * ============================================================================
 */

static bool test_normal_handshake(void) {
    printf("\n=== Test 1: Normal Handshake ===\n");

    loopyLoop *loop = loopyNew(256);
    if (!loop) {
        printf("  [FAIL] Could not create event loop\n");
        return false;
    }

    /* Shared state */
    TestState state = {0};
    state.loop = loop;
    state.expectRejection = false;

    /* Create two test contexts (simulating two nodes) */
    TestContext node1, node2;
    int port1 = TEST_PORT_BASE;
    int port2 = TEST_PORT_BASE + 1;

    initContext(&node1, loop, 1, 100, port1, &state); /* Node 1, cluster 100 */
    initContext(&node2, loop, 2, 100, port2, &state); /* Node 2, cluster 100 */
    state.node1 = &node1;
    state.node2 = &node2;

    /* Create transports */
    if (!createTransport(&node1, port1)) {
        printf("  [FAIL] Could not create transport for node1\n");
        loopyDelete(loop);
        return false;
    }

    if (!createTransport(&node2, port2)) {
        printf("  [FAIL] Could not create transport for node2\n");
        loopyTransportDestroy(node1.transport);
        loopyDelete(loop);
        return false;
    }

    printf("  Node1 listening on port %d (nodeId=1, clusterId=100)\n", port1);
    printf("  Node2 listening on port %d (nodeId=2, clusterId=100)\n", port2);

    /* Node2 connects to Node1 */
    printf("  Node2 connecting to Node1...\n");
    loopyTransportConnect(node2.transport, 1, "127.0.0.1", port1);

    /* Set timeout */
    loopyTimer *timeoutTimer =
        loopyTimerOneShotMs(loop, TEST_TIMEOUT_MS, timeoutCallback, &state);

    /* Run event loop - blocks until loopyStop() is called */
    loopyMain(loop);

    printf("  Both sides verified!\n");

    /* Phase 2: Test message exchange after handshake */
    if (node1.serverVerified && node2.clientVerified) {
        printf("  Sending test message from client to server...\n");
        const char *testMsg = "Hello from Node2!";
        state.complete = false; /* Reset for message phase */
        loopySessionSend(node2.clientSession, testMsg, strlen(testMsg));

        /* Run again to receive message */
        loopyMain(loop);
    }

    /* Cleanup - destroy transports first as they own the connections.
     * TODO: Fix ownership model so sessions properly release connection
     * references when transport is destroyed. */
    if (timeoutTimer) {
        loopyTimerCancel(timeoutTimer);
    }
    loopyTransportDestroy(node1.transport);
    loopyTransportDestroy(node2.transport);
    /* Note: Sessions leak here - need to fix ownership model */
    loopyDelete(loop);

    /* Check results */
    bool passed = (node1.serverVerified && node2.clientVerified &&
                   node1.messagesReceived > 0);

    printf("  Result: server_verified=%d, client_verified=%d, messages=%d\n",
           node1.serverVerified, node2.clientVerified, node1.messagesReceived);
    printf("  %s\n", passed ? "PASSED" : "FAILED");

    return passed;
}

/* ============================================================================
 * Test 2: Self-Connection Rejection
 * ============================================================================
 */

static bool test_self_connection(void) {
    printf("\n=== Test 2: Self-Connection Rejection ===\n");

    loopyLoop *loop = loopyNew(256);
    if (!loop) {
        printf("  [FAIL] Could not create event loop\n");
        return false;
    }

    /* Shared state - expecting rejection */
    TestState state = {0};
    state.loop = loop;
    state.expectRejection = true;

    TestContext node;
    int port = TEST_PORT_BASE + 10;

    initContext(&node, loop, 1, 100, port, &state);
    state.node1 = &node;

    if (!createTransport(&node, port)) {
        printf("  [FAIL] Could not create transport\n");
        loopyDelete(loop);
        return false;
    }

    printf("  Node listening on port %d\n", port);
    printf("  Connecting to self...\n");

    /* Connect to ourselves */
    loopyTransportConnect(node.transport, 1, "127.0.0.1", port);

    /* Set timeout */
    loopyTimer *timeoutTimer =
        loopyTimerOneShotMs(loop, TEST_TIMEOUT_MS, timeoutCallback, &state);

    /* Run event loop */
    loopyMain(loop);

    /* Cleanup */
    if (timeoutTimer) {
        loopyTimerCancel(timeoutTimer);
    }
    loopyTransportDestroy(node.transport);
    loopyDelete(loop);

    /* Self-connection should be rejected */
    bool passed = (node.serverRejected || node.clientRejected);

    printf("  Result: server_rejected=%d, client_rejected=%d\n",
           node.serverRejected, node.clientRejected);
    printf("  %s\n", passed ? "PASSED" : "FAILED");

    return passed;
}

/* ============================================================================
 * Test 3: Cluster Mismatch Rejection
 * ============================================================================
 */

static bool test_cluster_mismatch(void) {
    printf("\n=== Test 3: Cluster Mismatch Rejection ===\n");

    loopyLoop *loop = loopyNew(256);
    if (!loop) {
        printf("  [FAIL] Could not create event loop\n");
        return false;
    }

    /* Shared state - expecting rejection */
    TestState state = {0};
    state.loop = loop;
    state.expectRejection = true;

    TestContext node1, node2;
    int port1 = TEST_PORT_BASE + 20;
    int port2 = TEST_PORT_BASE + 21;

    initContext(&node1, loop, 1, 100, port1, &state); /* Cluster 100 */
    initContext(&node2, loop, 2, 200, port2,
                &state); /* Cluster 200 (different!) */
    state.node1 = &node1;
    state.node2 = &node2;

    if (!createTransport(&node1, port1) || !createTransport(&node2, port2)) {
        printf("  [FAIL] Could not create transports\n");
        loopyDelete(loop);
        return false;
    }

    printf("  Node1: clusterId=100\n");
    printf("  Node2: clusterId=200 (different!)\n");
    printf("  Node2 connecting to Node1...\n");

    loopyTransportConnect(node2.transport, 1, "127.0.0.1", port1);

    /* Set timeout */
    loopyTimer *timeoutTimer =
        loopyTimerOneShotMs(loop, TEST_TIMEOUT_MS, timeoutCallback, &state);

    /* Run event loop */
    loopyMain(loop);

    /* Cleanup */
    if (timeoutTimer) {
        loopyTimerCancel(timeoutTimer);
    }
    loopyTransportDestroy(node1.transport);
    loopyTransportDestroy(node2.transport);
    loopyDelete(loop);

    /* Cluster mismatch should be rejected */
    bool passed = (node1.serverRejected || node2.clientRejected);

    printf("  Result: server_rejected=%d, client_rejected=%d\n",
           node1.serverRejected, node2.clientRejected);
    printf("  %s\n", passed ? "PASSED" : "FAILED");

    return passed;
}

/* ============================================================================
 * Main
 * ============================================================================
 */

int main(void) {
    printf("Loopy Demo - Handshake Protocol Tests\n");
    printf("=====================================\n");

    /* Seed random */
    srand((unsigned int)time(NULL));

    int passed = 0;
    int total = 0;

    /* Run tests */
    total++;
    if (test_normal_handshake()) {
        passed++;
    }

    total++;
    if (test_self_connection()) {
        passed++;
    }

    total++;
    if (test_cluster_mismatch()) {
        passed++;
    }

    /* Summary */
    printf("\n=== Test Summary ===\n");
    printf("  Passed: %d/%d\n", passed, total);

    if (passed == total) {
        printf("\n*** ALL TESTS PASSED ***\n");
        return 0;
    } else {
        printf("\n*** SOME TESTS FAILED ***\n");
        return 1;
    }
}
