/* loopyEchoTest - Integration test for loopy-demo modules
 *
 * Tests:
 *   1. loopyConnection - TCP connection handling
 *   2. loopyTimers - Timer management
 *   3. Basic loopy event loop integration
 *
 * This verifies the foundation for Phase 1 of loopy-demo.
 */

#include "loopyConnection.h"
#include "loopyTimers.h"

/* loopy headers */
#include "../../deps/loopy/src/loopy.h"
#include "../../deps/loopy/src/loopyStream.h"
#include "../../deps/loopy/src/loopyTimer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* ============================================================================
 * Test State
 * ============================================================================
 */

typedef struct {
    loopyLoop *loop;
    loopyStream *server;
    loopyConn *serverConn; /* Accepted connection */
    loopyConn *clientConn; /* Client side connection */
    loopyTimerMgr *timers;

    /* Test state */
    int testPhase;
    int timerFires;
    int dataReceived;
    bool testPassed;
    char lastMessage[256];

    /* Shutdown timer */
    loopyTimer *shutdownTimer;
} TestState;

static TestState state;

/* ============================================================================
 * Connection Callbacks
 * ============================================================================
 */

static void serverConnCallback(loopyConn *conn, loopyConnEvent event,
                               const void *data, size_t len, void *userData) {
    TestState *s = (TestState *)userData;
    (void)conn;

    switch (event) {
    case LOOPY_CONN_EVENT_DATA:
        printf("  [Server] Received %zu bytes: \"%.*s\"\n", len, (int)len,
               (const char *)data);
        s->dataReceived++;

        /* Echo back */
        loopyConnWrite(conn, data, len);
        printf("  [Server] Echoed data back\n");
        break;

    case LOOPY_CONN_EVENT_CLOSE:
        printf("  [Server] Connection closed\n");
        s->serverConn = NULL;
        break;

    case LOOPY_CONN_EVENT_ERROR:
        printf("  [Server] Connection error\n");
        s->serverConn = NULL;
        break;

    default:
        break;
    }
}

static void clientConnCallback(loopyConn *conn, loopyConnEvent event,
                               const void *data, size_t len, void *userData) {
    TestState *s = (TestState *)userData;
    (void)conn;

    switch (event) {
    case LOOPY_CONN_EVENT_CONNECTED:
        printf("  [Client] Connected!\n");
        s->testPhase = 2;

        /* Send test message */
        const char *msg = "Hello, loopy!";
        loopyConnWrite(conn, msg, strlen(msg));
        printf("  [Client] Sent: \"%s\"\n", msg);
        break;

    case LOOPY_CONN_EVENT_DATA:
        printf("  [Client] Received echo: \"%.*s\"\n", (int)len,
               (const char *)data);

        if (len < sizeof(s->lastMessage)) {
            memcpy(s->lastMessage, data, len);
            s->lastMessage[len] = '\0';
        }

        /* Verify echo */
        if (strcmp(s->lastMessage, "Hello, loopy!") == 0) {
            printf("  [Client] Echo verified!\n");
            s->testPassed = true;

            /* Close client connection to trigger shutdown */
            loopyConnClose(conn);
        }
        break;

    case LOOPY_CONN_EVENT_CLOSE:
        printf("  [Client] Connection closed\n");
        s->clientConn = NULL;

        /* If server connection still exists, close it */
        if (s->serverConn) {
            loopyConnClose(s->serverConn);
        }
        break;

    case LOOPY_CONN_EVENT_ERROR:
        printf("  [Client] Connection error\n");
        s->clientConn = NULL;
        break;
    }
}

/* ============================================================================
 * Server Accept Callback
 * ============================================================================
 */

static void serverAcceptCallback(loopyStream *server, int status,
                                 void *userData) {
    TestState *s = (TestState *)userData;

    if (status < 0) {
        printf("  [Server] Accept error: %d\n", status);
        return;
    }

    loopyStream *client = loopyStreamAccept(server);
    if (!client) {
        printf("  [Server] Accept failed\n");
        return;
    }

    printf("  [Server] Accepted connection\n");

    /* Wrap in loopyConn */
    s->serverConn = loopyConnFromStream(client, serverConnCallback, s, NULL);
    if (!s->serverConn) {
        printf("  [Server] Failed to create connection wrapper\n");
        loopyStreamClose(client, NULL, NULL);
    }
}

/* ============================================================================
 * Timer Callback
 * ============================================================================
 */

static void timerCallback(loopyTimerMgr *mgr, loopyTimerType type,
                          void *userData) {
    TestState *s = (TestState *)userData;
    (void)mgr;

    s->timerFires++;

    switch (type) {
    case LOOPY_TIMER_ELECTION:
        printf("  [Timer] Election timeout fired\n");
        break;
    case LOOPY_TIMER_HEARTBEAT:
        printf("  [Timer] Heartbeat fired (%d)\n", s->timerFires);
        break;
    case LOOPY_TIMER_ELECTION_BACKOFF:
        printf("  [Timer] Election backoff fired\n");
        break;
    case LOOPY_TIMER_MAINTENANCE:
        printf("  [Timer] Maintenance fired\n");
        break;
    }
}

/* ============================================================================
 * Shutdown Timer
 * ============================================================================
 */

static void shutdownTimerCallback(loopyLoop *loop, loopyTimer *timer,
                                  void *userData) {
    TestState *s = (TestState *)userData;
    (void)timer;

    printf("\n=== Shutdown triggered ===\n");

    /* Stop timers */
    loopyTimerStopHeartbeat(s->timers);
    loopyTimerStopElection(s->timers);

    /* Stop the loop */
    loopyStop(loop);
}

/* ============================================================================
 * Test Runner
 * ============================================================================
 */

static int runEchoTest(void) {
    int result = 1;

    printf("\n=== Loopy Demo Echo Test ===\n\n");

    memset(&state, 0, sizeof(state));

    /* Create event loop */
    printf("[1] Creating event loop...\n");
    state.loop = loopyNew(1024);
    if (!state.loop) {
        printf("FAIL: Failed to create event loop\n");
        goto cleanup;
    }
    printf("  OK: Event loop created\n");

    /* Create timer manager */
    printf("\n[2] Creating timer manager...\n");
    loopyTimerConfig timerCfg;
    loopyTimerConfigInit(&timerCfg);
    timerCfg.heartbeatIntervalMs = 200; /* Faster for testing */
    timerCfg.electionTimeoutMinMs = 500;
    timerCfg.electionTimeoutMaxMs = 700;

    state.timers =
        loopyTimerMgrCreate(state.loop, timerCallback, &state, &timerCfg);
    if (!state.timers) {
        printf("FAIL: Failed to create timer manager\n");
        goto cleanup;
    }
    printf("  OK: Timer manager created\n");

    /* Start heartbeat timer */
    printf("  Starting heartbeat timer...\n");
    loopyTimerStartHeartbeat(state.timers);
    printf("  OK: Heartbeat timer started\n");

    /* Create TCP server */
    printf("\n[3] Creating TCP server on port 18080...\n");
    state.server = loopyStreamNewTcp(state.loop);
    if (!state.server) {
        printf("FAIL: Failed to create TCP stream\n");
        goto cleanup;
    }

    if (!loopyStreamBind(state.server, "127.0.0.1", 18080)) {
        printf("FAIL: Failed to bind to port 18080\n");
        goto cleanup;
    }

    if (!loopyStreamListen(state.server, 5, serverAcceptCallback, &state)) {
        printf("FAIL: Failed to listen\n");
        goto cleanup;
    }
    printf("  OK: Server listening on 127.0.0.1:18080\n");

    /* Create client connection */
    printf("\n[4] Creating client connection...\n");
    state.clientConn = loopyConnConnect(state.loop, "127.0.0.1", 18080,
                                        clientConnCallback, &state, NULL);
    if (!state.clientConn) {
        printf("FAIL: Failed to create client connection\n");
        goto cleanup;
    }
    printf("  OK: Client connection initiated\n");

    /* Set up shutdown timer (2 seconds max) */
    printf("\n[5] Setting up shutdown timer (2s)...\n");
    state.shutdownTimer =
        loopyTimerOneShotMs(state.loop, 2000, shutdownTimerCallback, &state);
    if (!state.shutdownTimer) {
        printf("FAIL: Failed to create shutdown timer\n");
        goto cleanup;
    }
    printf("  OK: Shutdown timer set\n");

    /* Run event loop */
    printf("\n[6] Running event loop...\n\n");
    loopyMain(state.loop);

    /* Check results */
    printf("\n=== Test Results ===\n");
    printf("  Data exchanges: %d\n", state.dataReceived);
    printf("  Timer fires: %d\n", state.timerFires);
    printf("  Echo verified: %s\n", state.testPassed ? "YES" : "NO");

    /* Get timer stats */
    loopyTimerStats timerStats;
    loopyTimerMgrGetStats(state.timers, &timerStats);
    printf("  Heartbeats: %llu\n",
           (unsigned long long)timerStats.heartbeatsSent);

    if (state.testPassed && state.timerFires > 0) {
        printf("\n*** ALL TESTS PASSED ***\n");
        result = 0;
    } else {
        printf("\n*** TESTS FAILED ***\n");
        if (!state.testPassed) {
            printf("  - Echo test did not complete\n");
        }
        if (state.timerFires == 0) {
            printf("  - No timer fires occurred\n");
        }
    }

cleanup:
    /* Cleanup */
    if (state.serverConn) {
        loopyConnCloseImmediate(state.serverConn);
    }
    if (state.clientConn) {
        loopyConnCloseImmediate(state.clientConn);
    }
    if (state.server) {
        loopyStreamClose(state.server, NULL, NULL);
    }
    if (state.timers) {
        loopyTimerMgrDestroy(state.timers);
    }
    if (state.loop) {
        loopyDelete(state.loop);
    }

    return result;
}

/* ============================================================================
 * Main
 * ============================================================================
 */

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    printf("Loopy Demo - Phase 1 Integration Test\n");
    printf("=====================================\n");
    printf("Testing: loopyConnection, loopyTimers, loopy core\n");

    int result = runEchoTest();

    return result;
}
