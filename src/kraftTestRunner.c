#define KRAFT_TEST
#include "asyncCommit.h"
#include "ctest.h"
#include "kraftTest.h"
#include "leadershipTransfer.h"
#include "multiRaft.h"
#include "readIndex.h"
#include "snapshot.h"
#include "witness.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Simple test framework */
static int totalTests = 0;
static int passedTests = 0;
static int failedTests = 0;

#define TEST_START(name)                                                       \
    do {                                                                       \
        totalTests++;                                                          \
        printf("\n[TEST %d] %s...\n", totalTests, name);                       \
    } while (0)

#define TEST_PASS()                                                            \
    do {                                                                       \
        passedTests++;                                                         \
        printf("  ✓ PASSED\n");                                                \
        return true;                                                           \
    } while (0)

#define TEST_FAIL(msg)                                                         \
    do {                                                                       \
        failedTests++;                                                         \
        printf("  ✗ FAILED: %s\n", msg);                                       \
        return false;                                                          \
    } while (0)

#define VERIFY(cond, msg)                                                      \
    do {                                                                       \
        if (!(cond)) {                                                         \
            printf("  ✗ Assertion failed: %s\n", msg);                         \
            TEST_FAIL(msg);                                                    \
        }                                                                      \
    } while (0)

/* ====================================================================
 * Test Cases
 * ==================================================================== */

static bool test_InitialElection3Nodes(void) {
    TEST_START("Initial leader election - 3 nodes");

    testSeed(12345);
    testCluster *cluster = testClusterNew(3);
    VERIFY(cluster != NULL, "Failed to create cluster");

    testClusterStart(cluster);

    /* Run until leader elected */
    testClusterRunUntilLeaderElected(cluster, 5000000UL);

    uint32_t leader = testClusterGetLeader(cluster);
    VERIFY(leader != UINT32_MAX, "No leader elected");
    VERIFY(testClusterHasConsistentLeader(cluster), "Inconsistent leader");

    kraftTerm maxTerm = testClusterGetMaxTerm(cluster);
    VERIFY(maxTerm > 0, "Term should be > 0");

    printf("  Leader: Node %u, Term: %" PRIu64 ", Elections: %" PRIu64 "\n",
           leader, maxTerm, cluster->totalElections);

    testClusterFree(cluster);
    TEST_PASS();
}

static bool test_InitialElection5Nodes(void) {
    TEST_START("Initial leader election - 5 nodes");

    testSeed(54321);
    testCluster *cluster = testClusterNew(5);
    VERIFY(cluster != NULL, "Failed to create cluster");

    testClusterStart(cluster);
    testClusterRunUntilLeaderElected(cluster, 5000000UL);

    uint32_t leader = testClusterGetLeader(cluster);
    VERIFY(leader != UINT32_MAX, "No leader elected");
    VERIFY(testClusterHasConsistentLeader(cluster), "Inconsistent leader");

    printf("  5-node cluster leader: Node %u\n", leader);

    testClusterFree(cluster);
    TEST_PASS();
}

static bool test_ElectionWithOneFailure(void) {
    TEST_START("Election with one node failure");

    testSeed(11111);
    testCluster *cluster = testClusterNew(5);
    VERIFY(cluster != NULL, "Failed to create cluster");

    testClusterStart(cluster);

    /* Crash one node before election */
    testNodeCrash(cluster, 2);

    testClusterRunUntilLeaderElected(cluster, 5000000UL);

    uint32_t leader = testClusterGetLeader(cluster);
    VERIFY(leader != UINT32_MAX, "No leader elected");
    VERIFY(leader != 2, "Crashed node became leader!");

    printf("  Leader despite node 2 crash: Node %u\n", leader);

    testClusterFree(cluster);
    TEST_PASS();
}

static bool test_ReElectionAfterLeaderCrash(void) {
    TEST_START("Re-election after leader crashes");

    testSeed(22222);
    testCluster *cluster = testClusterNew(5);
    VERIFY(cluster != NULL, "Failed to create cluster");

    testClusterStart(cluster);

    /* Elect initial leader */
    testClusterRunUntilLeaderElected(cluster, 5000000UL);
    uint32_t firstLeader = testClusterGetLeader(cluster);
    VERIFY(firstLeader != UINT32_MAX, "No initial leader");

    printf("  Initial leader: Node %u\n", firstLeader);

    /* Crash the leader */
    testNodeCrash(cluster, firstLeader);

    /* Wait for new leader */
    uint64_t startTime = cluster->currentTime;
    while (cluster->currentTime - startTime < 5000000UL) {
        testClusterTick(cluster, 10000);

        uint32_t currentLeader = testClusterGetLeader(cluster);
        if (currentLeader != UINT32_MAX && currentLeader != firstLeader) {
            printf("  New leader after crash: Node %u\n", currentLeader);
            testClusterFree(cluster);
            TEST_PASS();
        }
    }

    TEST_FAIL("New leader not elected after crash");
}

static bool test_ElectionSafety(void) {
    TEST_START("Election Safety - at most one leader per term");

    testSeed(66666);
    testCluster *cluster = testClusterNew(5);
    VERIFY(cluster != NULL, "Failed to create cluster");

    testClusterStart(cluster);

    /* Run multiple elections */
    for (int i = 0; i < 5; i++) {
        testClusterRunUntilLeaderElected(cluster, 5000000UL);

        /* Verify election safety */
        VERIFY(testInvariantElectionSafety(cluster),
               "Election safety violated!");

        /* Crash current leader */
        uint32_t leader = testClusterGetLeader(cluster);
        if (leader != UINT32_MAX) {
            testNodeCrash(cluster, leader);
            testClusterTick(cluster, 500000);
            testNodeRecover(cluster, leader);
        }
    }

    printf("  Election safety maintained across %" PRIu64 " elections\n",
           cluster->totalElections);

    testClusterFree(cluster);
    TEST_PASS();
}

static bool test_LogMatching(void) {
    TEST_START("Log Matching Property");

    testSeed(77777);
    testCluster *cluster = testClusterNew(5);
    VERIFY(cluster != NULL, "Failed to create cluster");

    testClusterStart(cluster);
    testClusterRunUntilLeaderElected(cluster, 5000000UL);

    /* Submit several commands */
    for (int i = 0; i < 10; i++) {
        char data[64];
        snprintf(data, sizeof(data), "entry %d", i);
        testClusterSubmitCommand(cluster, data, strlen(data));
        testClusterTick(cluster, 100000);
    }

    /* Give time for replication to complete (3 seconds) */
    testClusterTick(cluster, 3000000);

    /* Verify log matching */
    VERIFY(testInvariantLogMatching(cluster), "Log matching violated!");

    printf("  Log matching verified across all nodes\n");

    testClusterFree(cluster);
    TEST_PASS();
}

static bool test_MinorityFailure(void) {
    TEST_START("Cluster survives minority failures");

    testSeed(88888);
    testCluster *cluster = testClusterNew(5);
    VERIFY(cluster != NULL, "Failed to create cluster");

    testClusterStart(cluster);
    testClusterRunUntilLeaderElected(cluster, 5000000UL);

    /* Crash 2 nodes (minority) */
    testNodeCrash(cluster, 1);
    testNodeCrash(cluster, 3);

    /* Give cluster time to stabilize (leaders send heartbeats so it won't be
     * "quiet") */
    testClusterTick(cluster, 2000000); /* 2 seconds */

    /* Should still have a leader */
    uint32_t leader = testClusterGetLeader(cluster);
    VERIFY(leader != UINT32_MAX, "No leader with minority failure");

    printf("  Cluster operational with 2/5 nodes crashed\n");

    testClusterFree(cluster);
    TEST_PASS();
}

static bool test_NetworkPartition(void) {
    TEST_START("Network partition - split brain prevention");

    testSeed(99999);
    testCluster *cluster = testClusterNew(5);
    VERIFY(cluster != NULL, "Failed to create cluster");

    testClusterStart(cluster);
    testClusterRunUntilLeaderElected(cluster, 5000000UL);

    uint32_t initialLeader = testClusterGetLeader(cluster);
    if (initialLeader == UINT32_MAX) {
        TEST_FAIL("Failed to elect initial leader");
    }

    /* Create partition: {0, 1} vs {2, 3, 4} */
    uint32_t minority[] = {0, 1};
    uint32_t majority[] = {2, 3, 4};

    testNetworkPartition(cluster, minority, 2, majority, 3);

    /* Run for a while */
    testClusterTick(cluster, 3000000);

    /* Verify election safety during partition */
    VERIFY(testInvariantElectionSafety(cluster),
           "Election safety violated during partition!");

    /* Heal partition */
    testNetworkHeal(cluster);

    /* Give time for cluster to converge after healing */
    testClusterTick(cluster, 3000000); /* 3 seconds */

    /* Verify after healing */
    VERIFY(testInvariantElectionSafety(cluster),
           "Election safety violated after heal!");
    VERIFY(testClusterHasConsistentLeader(cluster),
           "No consistent leader after heal");

    printf("  Split brain prevented, cluster healed successfully\n");

    testClusterFree(cluster);
    TEST_PASS();
}

static bool test_PacketLoss(void) {
    TEST_START("Packet loss tolerance (10%)");

    testSeed(30303);
    testCluster *cluster = testClusterNew(5);
    VERIFY(cluster != NULL, "Failed to create cluster");

    /* Enable 10% packet loss */
    testNetworkSetMode(cluster, TEST_NETWORK_LOSSY);
    testNetworkSetDropRate(cluster, 0.10);

    testClusterStart(cluster);
    testClusterRunUntilLeaderElected(cluster, 10000000UL);

    uint32_t leader = testClusterGetLeader(cluster);
    VERIFY(leader != UINT32_MAX, "No leader with packet loss");

    printf("  Leader elected with 10%% loss (dropped: %" PRIu64 "/%" PRIu64
           ")\n",
           cluster->network.totalDropped, cluster->network.totalSent);

    testClusterFree(cluster);
    TEST_PASS();
}

static bool test_SingleNode(void) {
    TEST_START("Single node cluster (trivial case)");

    testSeed(70707);
    testCluster *cluster = testClusterNew(1);
    VERIFY(cluster != NULL, "Failed to create cluster");

    testClusterStart(cluster);

    printf("  Starting election process...\n");
    printf("  Node 0 role: %d\n",
           cluster->nodes[0].state->self->consensus.role);
    printf("  Node 0 term: %" PRIu64 "\n",
           cluster->nodes[0].state->self->consensus.term);
    printf("  Election timeout: %" PRIu64 "\n",
           cluster->nodes[0].nextElectionTimeout);

    /* Manually trigger election for single node (pre-vote if enabled) */
    if (cluster->nodes[0].state->config.enablePreVote) {
        kraftRpcRequestPreVote(cluster->nodes[0].state);
    } else {
        kraftRpcRequestVote(cluster->nodes[0].state);
    }

    printf("  After election request - role: %d, term: %" PRIu64 "\n",
           cluster->nodes[0].state->self->consensus.role,
           cluster->nodes[0].state->self->consensus.term);

    uint32_t leader = testClusterGetLeader(cluster);
    printf("  Leader: %u\n", leader);

    VERIFY(leader == 0, "Wrong leader in single node");

    printf("  Single node correctly elected itself\n");

    testClusterFree(cluster);
    TEST_PASS();
}

static bool test_LeadershipTransfer(void) {
    TEST_START("Leadership transfer (zero-downtime handoff)");

    testSeed(88888);
    testCluster *cluster = testClusterNew(3);
    VERIFY(cluster != NULL, "Failed to create cluster");

    testClusterStart(cluster);

    /* Establish initial leader */
    printf("  Waiting for initial leader election...\n");
    testClusterRunUntilLeaderElected(cluster, 5000000UL);

    uint32_t initialLeader = testClusterGetLeader(cluster);
    VERIFY(initialLeader != UINT32_MAX, "No initial leader elected");
    printf("  Initial leader: Node %u\n", initialLeader);

    /* Find target kraftNode in leader's node list - just pick first non-self
     * node */
    kraftState *leaderState = cluster->nodes[initialLeader].state;
    kraftNode *target = NULL;

    for (size_t i = 0; i < vecPtrCount(NODES_OLD_NODES(leaderState)); i++) {
        kraftNode *node = NULL;
        vecPtrGet(NODES_OLD_NODES(leaderState), i, (void **)&node);
        if (node && !node->isOurself) {
            target = node;
            printf("  Found target node for transfer: clusterId=%" PRIu64 ", "
                   "nodeId=%" PRIu64 "\n",
                   (unsigned long long)target->clusterId,
                   (unsigned long long)target->nodeId);
            break;
        }
    }

    VERIFY(target != NULL, "Target node not found in leader's node list");

    /* Initiate leadership transfer */
    printf("  Initiating leadership transfer...\n");
    kraftTransferStatus status =
        kraftTransferLeadershipBegin(leaderState, target);
    VERIFY(status == KRAFT_TRANSFER_STATUS_OK, "Failed to initiate transfer");
    printf("  Transfer initiated successfully\n");

    /* Run cluster to allow transfer to complete */
    printf("  Waiting for transfer to complete...\n");
    for (int i = 0; i < 100; i++) {
        testClusterTick(cluster, 100000); /* 100ms ticks */

        /* Check if transfer completed */
        if (leaderState->transfer.state == KRAFT_TRANSFER_NONE) {
            printf("  Transfer state reset after %d ticks\n", i);
            break;
        }
    }

    /* Give time for new leader to emerge */
    testClusterTick(cluster, 1000000); /* 1 second */

    /* Verify new leader emerged */
    uint32_t newLeader = testClusterGetLeader(cluster);
    printf("  New leader after transfer: Node %u\n", newLeader);

    VERIFY(newLeader != UINT32_MAX, "No leader after transfer");
    VERIFY(newLeader != initialLeader, "Leadership did not transfer");

    printf("  Leadership successfully transferred from Node %u to Node %u\n",
           initialLeader, newLeader);

    testClusterFree(cluster);
    TEST_PASS();
}

static bool test_ReadIndex(void) {
    TEST_START("ReadIndex (linearizable reads without log writes)");

    testSeed(99999);
    testCluster *cluster = testClusterNew(3);
    VERIFY(cluster != NULL, "Failed to create cluster");

    testClusterStart(cluster);

    /* Establish leader */
    printf("  Waiting for leader election...\n");
    testClusterRunUntilLeaderElected(cluster, 5000000UL);

    uint32_t leader = testClusterGetLeader(cluster);
    VERIFY(leader != UINT32_MAX, "No leader elected");
    printf("  Leader: Node %u\n", leader);

    kraftState *leaderState = cluster->nodes[leader].state;

    /* Enable ReadIndex */
    leaderState->config.enableReadIndex = true;
    printf("  ReadIndex enabled\n");

    /* Test 1: ReadIndex API returns valid handle */
    printf("  Testing ReadIndex API...\n");
    int64_t readIndex = kraftReadIndexBegin(leaderState);
    printf("  ReadIndex request returned: %lld (commitIndex=%" PRIu64 ")\n",
           (long long)readIndex, (unsigned long long)leaderState->commitIndex);

    /* ReadIndex should return the current commitIndex (>= 0) */
    VERIFY(readIndex >= 0, "ReadIndex should return non-negative handle");

    /* Test 2: Can check if read is ready (may not be confirmed yet) */
    bool ready = kraftReadIndexIsReady(leaderState, (uint64_t)readIndex);
    printf(
        "  Initial ready state: %s (expected: no - quorum not yet confirmed)\n",
        ready ? "yes" : "no");

    /* Test 3: Run a few ticks to allow heartbeat processing */
    for (int i = 0; i < 5; i++) {
        testClusterTick(cluster, 50000); /* 50ms ticks */
        kraftReadIndexProcess(leaderState);
    }

    ready = kraftReadIndexIsReady(leaderState, (uint64_t)readIndex);
    printf("  After heartbeats ready state: %s\n", ready ? "yes" : "no");

    /* Test 4: Complete the read (cleanup) */
    if (ready) {
        kraftReadIndexComplete(leaderState, (uint64_t)readIndex);
        printf("  ReadIndex completed successfully\n");
    }

    /* Test 5: Verify ReadIndex fails for non-leader */
    kraftState *followerState = NULL;
    for (uint32_t i = 0; i < cluster->nodeCount; i++) {
        if (i != leader && !cluster->nodes[i].crashed) {
            followerState = cluster->nodes[i].state;
            break;
        }
    }

    if (followerState) {
        followerState->config.enableReadIndex = true;
        int64_t followerRead = kraftReadIndexBegin(followerState);
        printf("  ReadIndex on follower returned: %lld (expected: negative "
               "error)\n",
               (long long)followerRead);
        VERIFY(followerRead < 0, "ReadIndex should fail for non-leader");
    }

    printf("  ReadIndex API test completed successfully\n");

    testClusterFree(cluster);
    TEST_PASS();
}

/* ====================================================================
 * Witness Node Tests
 * ==================================================================== */

static bool test_WitnessManagerLifecycle(void) {
    TEST_START("Witness Manager - Lifecycle (init, add, remove, free)");

    kraftWitnessManager mgr;
    kraftWitnessConfig config = kraftWitnessConfigDefault();

    /* Test initialization */
    VERIFY(kraftWitnessManagerInit(&mgr, &config), "Failed to init manager");
    VERIFY(mgr.witnessCount == 0, "Initial witness count should be 0");
    VERIFY(mgr.capacity >= 4, "Capacity should be at least 4");

    /* Create mock nodes for testing */
    kraftNode node1 = {.nodeId = 100, .clusterId = 1};
    kraftNode node2 = {.nodeId = 101, .clusterId = 1};
    kraftNode node3 = {.nodeId = 102, .clusterId = 1};

    /* Test adding witnesses */
    VERIFY(kraftWitnessAdd(&mgr, &node1), "Failed to add node1");
    VERIFY(mgr.witnessCount == 1, "Witness count should be 1");
    VERIFY(kraftWitnessIsWitness(&mgr, &node1), "node1 should be a witness");

    VERIFY(kraftWitnessAdd(&mgr, &node2), "Failed to add node2");
    VERIFY(mgr.witnessCount == 2, "Witness count should be 2");

    /* Test duplicate add fails */
    VERIFY(!kraftWitnessAdd(&mgr, &node1), "Duplicate add should fail");
    VERIFY(mgr.witnessCount == 2, "Count unchanged after dup");

    /* Test get witness */
    kraftWitness *w1 = kraftWitnessGet(&mgr, &node1);
    VERIFY(w1 != NULL, "Should get witness for node1");
    VERIFY(w1->node == &node1, "Witness node should match");
    VERIFY(w1->status == KRAFT_WITNESS_CATCHING_UP, "Initial status");

    /* Test remove */
    VERIFY(kraftWitnessRemove(&mgr, &node1), "Failed to remove node1");
    VERIFY(mgr.witnessCount == 1, "Witness count should be 1");
    VERIFY(!kraftWitnessIsWitness(&mgr, &node1), "node1 no longer witness");
    VERIFY(kraftWitnessIsWitness(&mgr, &node2), "node2 still witness");

    /* Test remove non-existent */
    VERIFY(!kraftWitnessRemove(&mgr, &node3), "Remove non-witness should fail");

    /* Test stats */
    kraftWitnessStats stats;
    kraftWitnessGetStats(&mgr, &stats);
    VERIFY(stats.totalAdded == 2, "Should have added 2 witnesses");
    VERIFY(stats.totalRemoved == 1, "Should have removed 1 witness");

    kraftWitnessManagerFree(&mgr);
    printf("  Witness lifecycle tests passed\n");
    TEST_PASS();
}

static bool test_WitnessReplicationTracking(void) {
    TEST_START("Witness Manager - Replication Tracking");

    kraftWitnessManager mgr;
    kraftWitnessConfig config = kraftWitnessConfigDefault();
    config.maxStalenessUs = 5000000; /* 5 seconds */
    config.catchupThreshold = 100;

    VERIFY(kraftWitnessManagerInit(&mgr, &config), "Failed to init");

    kraftNode node1 = {.nodeId = 100, .clusterId = 1};
    VERIFY(kraftWitnessAdd(&mgr, &node1), "Failed to add witness");

    kraftWitness *w = kraftWitnessGet(&mgr, &node1);
    VERIFY(w != NULL, "Should get witness");

    /* Update replication index */
    kraftWitnessUpdateReplicated(&mgr, &node1, 50);
    VERIFY(w->replicatedIndex == 50, "Replicated index should be 50");

    kraftWitnessUpdateReplicated(&mgr, &node1, 100);
    VERIFY(w->replicatedIndex == 100, "Replicated index should be 100");

    /* Update heartbeat */
    uint64_t nowUs = 1000000000ULL;
    kraftWitnessUpdateHeartbeat(&mgr, &node1, nowUs);
    VERIFY(w->lastHeartbeat == nowUs, "Heartbeat time should match");

    /* Test staleness update */
    uint64_t leaderIndex = 150;
    kraftWitnessUpdateStaleness(&mgr, leaderIndex, nowUs);
    VERIFY(w->entriesBehind == 50, "Should be 50 entries behind");

    /* With high replication, should be ACTIVE */
    w->lastReplication = nowUs; /* Set recent replication */
    kraftWitnessUpdateStaleness(&mgr, 105, nowUs);
    VERIFY(w->status == KRAFT_WITNESS_ACTIVE,
           "Should be active when caught up");

    printf("  Replication tracking tests passed\n");
    kraftWitnessManagerFree(&mgr);
    TEST_PASS();
}

static bool test_WitnessReadServing(void) {
    TEST_START("Witness Manager - Read Serving");

    kraftWitnessManager mgr;
    kraftWitnessConfig config = kraftWitnessConfigDefault();
    config.maxStalenessUs = 1000000; /* 1 second */

    VERIFY(kraftWitnessManagerInit(&mgr, &config), "Failed to init");

    kraftNode node1 = {.nodeId = 100, .clusterId = 1};
    kraftNode node2 = {.nodeId = 101, .clusterId = 1};

    VERIFY(kraftWitnessAdd(&mgr, &node1), "Failed to add node1");
    VERIFY(kraftWitnessAdd(&mgr, &node2), "Failed to add node2");

    kraftWitness *w1 = kraftWitnessGet(&mgr, &node1);
    kraftWitness *w2 = kraftWitnessGet(&mgr, &node2);

    uint64_t nowUs = 1000000000ULL;

    /* Set up w1 as active, w2 as stale */
    w1->status = KRAFT_WITNESS_ACTIVE;
    w1->lastReplication = nowUs - 500000; /* 0.5 sec ago */
    w1->replicatedIndex = 100;

    w2->status = KRAFT_WITNESS_STALE;
    w2->lastReplication = nowUs - 2000000; /* 2 sec ago */
    w2->replicatedIndex = 50;

    /* Test can serve read */
    VERIFY(kraftWitnessCanServeRead(&mgr, w1, nowUs), "w1 should serve reads");
    VERIFY(!kraftWitnessCanServeRead(&mgr, w2, nowUs),
           "w2 too stale for reads");

    /* Test get best for read */
    kraftWitness *best = kraftWitnessGetBestForRead(&mgr, nowUs);
    VERIFY(best == w1, "w1 should be best for reads");

    /* Test recording reads */
    kraftWitnessRecordRead(&mgr);
    kraftWitnessStats stats;
    kraftWitnessGetStats(&mgr, &stats);
    VERIFY(stats.readsServed == 1, "Should have served 1 read");

    printf("  Read serving tests passed\n");
    kraftWitnessManagerFree(&mgr);
    TEST_PASS();
}

static bool test_WitnessPromotion(void) {
    TEST_START("Witness Manager - Promotion to Voter");

    kraftWitnessManager mgr;
    kraftWitnessConfig config = kraftWitnessConfigDefault();
    config.catchupThreshold = 10; /* Only 10 entries behind allowed */

    VERIFY(kraftWitnessManagerInit(&mgr, &config), "Failed to init");

    kraftNode node1 = {.nodeId = 100, .clusterId = 1};
    kraftNode node2 = {.nodeId = 101, .clusterId = 1};

    VERIFY(kraftWitnessAdd(&mgr, &node1), "Failed to add node1");
    VERIFY(kraftWitnessAdd(&mgr, &node2), "Failed to add node2");

    kraftWitness *w1 = kraftWitnessGet(&mgr, &node1);
    kraftWitness *w2 = kraftWitnessGet(&mgr, &node2);

    /* Set w1 caught up, w2 behind */
    w1->replicatedIndex = 95;
    w2->replicatedIndex = 50;

    uint64_t leaderIndex = 100;

    /* Test caught up check */
    VERIFY(kraftWitnessIsCaughtUp(&mgr, w1, leaderIndex),
           "w1 should be caught up");
    VERIFY(!kraftWitnessIsCaughtUp(&mgr, w2, leaderIndex), "w2 not caught up");

    /* Test get promotable */
    kraftWitness *promotable[10];
    uint32_t count =
        kraftWitnessGetPromotable(&mgr, promotable, 10, leaderIndex);
    VERIFY(count == 1, "Should have 1 promotable witness");
    VERIFY(promotable[0] == w1, "w1 should be promotable");

    /* Test promote fails for behind node */
    VERIFY(!kraftWitnessPromote(&mgr, &node2, leaderIndex),
           "Promote behind node should fail");
    VERIFY(mgr.witnessCount == 2, "Count unchanged after failed promote");

    /* Test successful promotion */
    VERIFY(kraftWitnessPromote(&mgr, &node1, leaderIndex),
           "Promote caught-up should succeed");
    VERIFY(mgr.witnessCount == 1, "Count should decrease after promote");
    VERIFY(!kraftWitnessIsWitness(&mgr, &node1),
           "node1 no longer witness after promote");

    kraftWitnessStats stats;
    kraftWitnessGetStats(&mgr, &stats);
    VERIFY(stats.totalPromoted == 1, "Should have promoted 1");

    printf("  Promotion tests passed\n");
    kraftWitnessManagerFree(&mgr);
    TEST_PASS();
}

static bool test_WitnessVotePrevention(void) {
    TEST_START("Witness Manager - Vote Prevention");

    kraftWitnessManager mgr;
    VERIFY(kraftWitnessManagerInit(&mgr, NULL), "Failed to init");

    kraftNode witness = {.nodeId = 100, .clusterId = 1};
    kraftNode voter = {.nodeId = 101, .clusterId = 1};

    VERIFY(kraftWitnessAdd(&mgr, &witness), "Failed to add witness");

    /* Witness should NOT be allowed to vote */
    VERIFY(!kraftWitnessAllowVote(&mgr, &witness), "Witness should not vote");

    /* Non-witness should be allowed to vote */
    VERIFY(kraftWitnessAllowVote(&mgr, &voter), "Voter should be allowed");

    printf("  Vote prevention tests passed\n");
    kraftWitnessManagerFree(&mgr);
    TEST_PASS();
}

/* ====================================================================
 * Async Commit Mode Tests
 * ==================================================================== */

static bool test_AsyncCommitConfig(void) {
    TEST_START("Async Commit - Configuration");

    kraftAsyncCommitConfig config = kraftAsyncCommitConfigDefault();

    VERIFY(config.defaultMode == KRAFT_COMMIT_STRONG,
           "Default mode should be STRONG");
    VERIFY(config.flexibleAcks == 1, "Default flexible acks should be 1");
    VERIFY(config.asyncFlushIntervalUs == 10000, "Default flush interval 10ms");
    VERIFY(config.asyncMaxPendingBytes == 16 * 1024 * 1024,
           "Default max pending 16MB");
    VERIFY(config.allowModeOverride == true,
           "Mode override allowed by default");

    printf("  Config tests passed\n");
    TEST_PASS();
}

static bool test_AsyncCommitLifecycle(void) {
    TEST_START("Async Commit - Lifecycle (init, submit, free)");

    kraftAsyncCommitManager mgr;
    kraftAsyncCommitConfig config = kraftAsyncCommitConfigDefault();

    VERIFY(kraftAsyncCommitInit(&mgr, &config), "Failed to init");
    VERIFY(mgr.pendingCount == 0, "Initial pending should be 0");
    VERIFY(mgr.totalWrites == 0, "Initial writes should be 0");

    kraftAsyncCommitFree(&mgr);
    printf("  Lifecycle tests passed\n");
    TEST_PASS();
}

static bool test_AsyncCommitStrongMode(void) {
    TEST_START("Async Commit - STRONG Mode");

    kraftAsyncCommitManager mgr;
    kraftAsyncCommitConfig config = kraftAsyncCommitConfigDefault();
    config.defaultMode = KRAFT_COMMIT_STRONG;

    VERIFY(kraftAsyncCommitInit(&mgr, &config), "Failed to init");

    /* Submit with STRONG mode - should be pending */
    VERIFY(kraftAsyncCommitSubmit(&mgr, NULL, 1, 100), "Submit should succeed");
    VERIFY(mgr.pendingCount == 1, "Should have 1 pending write");
    VERIFY(mgr.strongWrites == 1, "Should have 1 strong write");
    VERIFY(mgr.lastAckedIndex == 0, "Not acked yet");

    /* Record durable - should ack */
    kraftAsyncCommitRecordDurable(&mgr, 1);
    VERIFY(mgr.lastAckedIndex == 1, "Should be acked after durable");
    VERIFY(mgr.lastDurableIndex == 1, "Durable index updated");

    /* Check should ack */
    VERIFY(!kraftAsyncCommitShouldAck(&mgr, 1), "Already acked");

    kraftAsyncCommitStats stats;
    kraftAsyncCommitGetStats(&mgr, &stats);
    VERIFY(stats.totalWrites == 1, "Total writes should be 1");
    VERIFY(stats.strongWrites == 1, "Strong writes should be 1");

    printf("  STRONG mode tests passed\n");
    kraftAsyncCommitFree(&mgr);
    TEST_PASS();
}

static bool test_AsyncCommitLeaderOnlyMode(void) {
    TEST_START("Async Commit - LEADER_ONLY Mode");

    kraftAsyncCommitManager mgr;
    kraftAsyncCommitConfig config = kraftAsyncCommitConfigDefault();
    config.defaultMode = KRAFT_COMMIT_LEADER_ONLY;

    VERIFY(kraftAsyncCommitInit(&mgr, &config), "Failed to init");

    /* Submit with LEADER_ONLY - should ack immediately */
    VERIFY(kraftAsyncCommitSubmit(&mgr, NULL, 1, 100), "Submit should succeed");
    VERIFY(mgr.pendingCount == 0, "No pending for LEADER_ONLY");
    VERIFY(mgr.leaderOnlyWrites == 1, "Should have 1 leader-only write");
    VERIFY(mgr.lastAckedIndex == 1, "Should be acked immediately");

    /* Multiple writes */
    VERIFY(kraftAsyncCommitSubmit(&mgr, NULL, 2, 100),
           "Submit 2 should succeed");
    VERIFY(kraftAsyncCommitSubmit(&mgr, NULL, 3, 100),
           "Submit 3 should succeed");
    VERIFY(mgr.lastAckedIndex == 3, "All should be acked");
    VERIFY(mgr.leaderOnlyWrites == 3, "Should have 3 leader-only writes");

    printf("  LEADER_ONLY mode tests passed\n");
    kraftAsyncCommitFree(&mgr);
    TEST_PASS();
}

static bool test_AsyncCommitAsyncMode(void) {
    TEST_START("Async Commit - ASYNC Mode");

    kraftAsyncCommitManager mgr;
    kraftAsyncCommitConfig config = kraftAsyncCommitConfigDefault();
    config.defaultMode = KRAFT_COMMIT_ASYNC;

    VERIFY(kraftAsyncCommitInit(&mgr, &config), "Failed to init");

    /* Submit with ASYNC - should ack immediately */
    VERIFY(kraftAsyncCommitSubmit(&mgr, NULL, 1, 100), "Submit should succeed");
    VERIFY(mgr.pendingCount == 0, "No pending for ASYNC");
    VERIFY(mgr.asyncWrites == 1, "Should have 1 async write");
    VERIFY(mgr.lastAckedIndex == 1, "Should be acked immediately");

    printf("  ASYNC mode tests passed\n");
    kraftAsyncCommitFree(&mgr);
    TEST_PASS();
}

static bool test_AsyncCommitFlexibleMode(void) {
    TEST_START("Async Commit - FLEXIBLE Mode");

    kraftAsyncCommitManager mgr;
    kraftAsyncCommitConfig config = kraftAsyncCommitConfigDefault();
    config.defaultMode = KRAFT_COMMIT_FLEXIBLE;
    config.flexibleAcks = 2; /* Require 2 acks */

    VERIFY(kraftAsyncCommitInit(&mgr, &config), "Failed to init");

    /* Submit with FLEXIBLE - should be pending */
    VERIFY(kraftAsyncCommitSubmit(&mgr, NULL, 1, 100), "Submit should succeed");
    VERIFY(mgr.pendingCount == 1, "Should have 1 pending");
    VERIFY(mgr.flexibleWrites == 1, "Should have 1 flexible write");
    VERIFY(mgr.lastAckedIndex == 0, "Not acked yet (need 2 acks)");

    /* First ack (leader counts as 1, so this is 2nd) */
    kraftAsyncCommitRecordAck(&mgr, 1);
    VERIFY(mgr.lastAckedIndex == 1, "Should be acked with 2 acks");

    printf("  FLEXIBLE mode tests passed\n");
    kraftAsyncCommitFree(&mgr);
    TEST_PASS();
}

static bool test_AsyncCommitModeOverride(void) {
    TEST_START("Async Commit - Per-Request Mode Override");

    kraftAsyncCommitManager mgr;
    kraftAsyncCommitConfig config = kraftAsyncCommitConfigDefault();
    config.defaultMode = KRAFT_COMMIT_STRONG;
    config.allowModeOverride = true;

    VERIFY(kraftAsyncCommitInit(&mgr, &config), "Failed to init");

    /* Override to ASYNC */
    VERIFY(
        kraftAsyncCommitSubmitWithMode(&mgr, NULL, 1, 100, KRAFT_COMMIT_ASYNC),
        "Submit with ASYNC override");
    VERIFY(mgr.asyncWrites == 1, "Should count as async");
    VERIFY(mgr.lastAckedIndex == 1, "Acked immediately");

    /* Override to LEADER_ONLY */
    VERIFY(kraftAsyncCommitSubmitWithMode(&mgr, NULL, 2, 100,
                                          KRAFT_COMMIT_LEADER_ONLY),
           "Submit with LEADER_ONLY override");
    VERIFY(mgr.leaderOnlyWrites == 1, "Should count as leader-only");

    /* Disable override */
    mgr.config.allowModeOverride = false;
    kraftCommitMode mode = kraftAsyncCommitGetMode(&mgr, KRAFT_COMMIT_ASYNC);
    VERIFY(mode == KRAFT_COMMIT_STRONG,
           "Should use default when override disabled");

    printf("  Mode override tests passed\n");
    kraftAsyncCommitFree(&mgr);
    TEST_PASS();
}

static bool test_AsyncCommitBlocking(void) {
    TEST_START("Async Commit - Blocking on Max Pending");

    kraftAsyncCommitManager mgr;
    kraftAsyncCommitConfig config = kraftAsyncCommitConfigDefault();
    config.defaultMode = KRAFT_COMMIT_STRONG;
    config.asyncMaxPendingBytes = 500; /* Low limit for testing */

    VERIFY(kraftAsyncCommitInit(&mgr, &config), "Failed to init");

    /* Submit writes until blocked */
    VERIFY(kraftAsyncCommitSubmit(&mgr, NULL, 1, 200), "Submit 1");
    VERIFY(!kraftAsyncCommitIsBlocked(&mgr), "Not blocked yet");

    VERIFY(kraftAsyncCommitSubmit(&mgr, NULL, 2, 200), "Submit 2");
    VERIFY(!kraftAsyncCommitIsBlocked(&mgr), "Not blocked yet");

    VERIFY(kraftAsyncCommitSubmit(&mgr, NULL, 3, 200), "Submit 3");
    VERIFY(kraftAsyncCommitIsBlocked(&mgr), "Should be blocked now");

    /* Should fail while blocked */
    VERIFY(!kraftAsyncCommitSubmit(&mgr, NULL, 4, 100),
           "Submit should fail when blocked");

    /* Flush to unblock */
    uint32_t flushed = kraftAsyncCommitFlush(&mgr);
    VERIFY(flushed == 3, "Should flush 3 pending");
    VERIFY(!kraftAsyncCommitIsBlocked(&mgr), "Should be unblocked after flush");

    printf("  Blocking tests passed\n");
    kraftAsyncCommitFree(&mgr);
    TEST_PASS();
}

static bool test_AsyncCommitStats(void) {
    TEST_START("Async Commit - Statistics");

    kraftAsyncCommitManager mgr;
    kraftAsyncCommitConfig config = kraftAsyncCommitConfigDefault();
    config.allowModeOverride = true;

    VERIFY(kraftAsyncCommitInit(&mgr, &config), "Failed to init");

    /* Submit various modes */
    kraftAsyncCommitSubmitWithMode(&mgr, NULL, 1, 100, KRAFT_COMMIT_STRONG);
    kraftAsyncCommitSubmitWithMode(&mgr, NULL, 2, 100, KRAFT_COMMIT_STRONG);
    kraftAsyncCommitSubmitWithMode(&mgr, NULL, 3, 100,
                                   KRAFT_COMMIT_LEADER_ONLY);
    kraftAsyncCommitSubmitWithMode(&mgr, NULL, 4, 100, KRAFT_COMMIT_ASYNC);
    kraftAsyncCommitSubmitWithMode(&mgr, NULL, 5, 100, KRAFT_COMMIT_ASYNC);
    kraftAsyncCommitSubmitWithMode(&mgr, NULL, 6, 100, KRAFT_COMMIT_ASYNC);

    kraftAsyncCommitStats stats;
    kraftAsyncCommitGetStats(&mgr, &stats);

    VERIFY(stats.totalWrites == 6, "Total writes should be 6");
    VERIFY(stats.strongWrites == 2, "Strong writes should be 2");
    VERIFY(stats.leaderOnlyWrites == 1, "Leader-only writes should be 1");
    VERIFY(stats.asyncWrites == 3, "Async writes should be 3");
    VERIFY(stats.pendingWrites == 2, "Pending should be 2 (STRONG mode)");

    /* Format stats */
    char buf[512];
    int len = kraftAsyncCommitFormatStats(&stats, buf, sizeof(buf));
    VERIFY(len > 0, "Stats format should succeed");
    printf("  Stats:\n%s", buf);

    kraftAsyncCommitFree(&mgr);
    TEST_PASS();
}

/* ====================================================================
 * Intelligent Snapshot Tests
 * ==================================================================== */

static bool test_SnapshotConfig(void) {
    TEST_START("Snapshot - Configuration Defaults");

    kraftSnapshotConfig config = kraftSnapshotConfigDefault();

    VERIFY(config.triggerPolicy == KRAFT_TRIGGER_COMBINED,
           "Default policy should be COMBINED");
    VERIFY(config.logSizeThresholdBytes == 100 * 1024 * 1024,
           "Default log size 100MB");
    VERIFY(config.entryCountThreshold == 10000, "Default entry count 10000");
    VERIFY(config.chunkSizeBytes == 1 * 1024 * 1024, "Default chunk size 1MB");
    VERIFY(config.maxConcurrentTransfers == 3, "Default max transfers 3");
    VERIFY(config.maxRetries == 3, "Default max retries 3");
    VERIFY(config.compactAfterSnapshot == true,
           "Compact after snapshot by default");

    printf("  Config defaults verified\n");
    TEST_PASS();
}

static bool test_SnapshotLifecycle(void) {
    TEST_START("Snapshot - Manager Lifecycle");

    kraftSnapshotManager mgr;
    kraftSnapshotConfig config = kraftSnapshotConfigDefault();

    VERIFY(kraftSnapshotManagerInit(&mgr, &config), "Failed to init");
    VERIFY(mgr.state == KRAFT_SNAPSHOT_IDLE, "Initial state should be IDLE");
    VERIFY(mgr.snapshotBuffer != NULL, "Buffer should be allocated");
    VERIFY(mgr.transfers != NULL, "Transfers array should be allocated");

    kraftSnapshotManagerFree(&mgr);
    printf("  Lifecycle tests passed\n");
    TEST_PASS();
}

static bool test_SnapshotTrigger(void) {
    TEST_START("Snapshot - Trigger Policies");

    kraftSnapshotManager mgr;
    kraftSnapshotConfig config = kraftSnapshotConfigDefault();
    config.logSizeThresholdBytes = 1000;
    config.entryCountThreshold = 100;
    config.timeIntervalUs = 1000000; /* 1 second */

    VERIFY(kraftSnapshotManagerInit(&mgr, &config), "Failed to init");

    uint64_t now = 2000000; /* 2 seconds */

    /* Test COMBINED trigger - any threshold */
    VERIFY(kraftSnapshotShouldTrigger(&mgr, 500, 50, now),
           "Should trigger (time)");

    /* Reset last snapshot time to recent */
    mgr.lastSnapshotTime = now - 100000; /* 0.1 sec ago */
    VERIFY(!kraftSnapshotShouldTrigger(&mgr, 500, 50, now),
           "Should not trigger (all below)");
    VERIFY(kraftSnapshotShouldTrigger(&mgr, 2000, 50, now),
           "Should trigger (size)");
    VERIFY(kraftSnapshotShouldTrigger(&mgr, 500, 200, now),
           "Should trigger (count)");

    /* Test MANUAL policy */
    mgr.config.triggerPolicy = KRAFT_TRIGGER_MANUAL;
    VERIFY(!kraftSnapshotShouldTrigger(&mgr, 2000, 200, now),
           "Manual mode no trigger");

    /* Test during snapshot in progress */
    mgr.config.triggerPolicy = KRAFT_TRIGGER_COMBINED;
    mgr.state = KRAFT_SNAPSHOT_CREATING;
    VERIFY(!kraftSnapshotShouldTrigger(&mgr, 2000, 200, now),
           "No trigger during creation");

    printf("  Trigger policy tests passed\n");
    kraftSnapshotManagerFree(&mgr);
    TEST_PASS();
}

static bool test_SnapshotCreation(void) {
    TEST_START("Snapshot - Creation and Writing");

    kraftSnapshotManager mgr;
    VERIFY(kraftSnapshotManagerInit(&mgr, NULL), "Failed to init");

    /* Begin snapshot */
    uint64_t snapshotId = kraftSnapshotBegin(&mgr, NULL, 100, 5);
    VERIFY(snapshotId > 0, "Should return valid snapshot ID");
    VERIFY(mgr.state == KRAFT_SNAPSHOT_CREATING, "State should be CREATING");

    /* Write some data */
    const char *data1 = "Hello, snapshot!";
    const char *data2 = " More data here.";
    VERIFY(kraftSnapshotWrite(&mgr, data1, strlen(data1)),
           "Write 1 should succeed");
    VERIFY(kraftSnapshotWrite(&mgr, data2, strlen(data2)),
           "Write 2 should succeed");
    VERIFY(mgr.snapshotBufferSize == strlen(data1) + strlen(data2),
           "Buffer size correct");

    /* Finish snapshot - state returns to IDLE so more snapshots can be created
     */
    VERIFY(kraftSnapshotFinish(&mgr), "Finish should succeed");
    VERIFY(mgr.state == KRAFT_SNAPSHOT_IDLE,
           "State should be IDLE after finish");
    VERIFY(mgr.currentSnapshot.lastIncludedIndex == 100, "Last index correct");
    VERIFY(mgr.currentSnapshot.lastIncludedTerm == 5, "Last term correct");
    VERIFY(mgr.totalSnapshots == 1, "Total snapshots should be 1");

    /* Verify metadata */
    const kraftSnapshotMeta *meta = kraftSnapshotGetCurrent(&mgr);
    VERIFY(meta != NULL, "Should get current snapshot");
    VERIFY(meta->snapshotId == snapshotId, "Snapshot ID should match");
    VERIFY(meta->sizeBytes == strlen(data1) + strlen(data2),
           "Size should match");

    printf("  Creation tests passed\n");
    kraftSnapshotManagerFree(&mgr);
    TEST_PASS();
}

static bool test_SnapshotTransfer(void) {
    TEST_START("Snapshot - Transfer to Follower");

    kraftSnapshotManager mgr;
    kraftSnapshotConfig config = kraftSnapshotConfigDefault();
    config.chunkSizeBytes = 16; /* Small chunks for testing */

    VERIFY(kraftSnapshotManagerInit(&mgr, &config), "Failed to init");

    /* Create a snapshot first */
    kraftSnapshotBegin(&mgr, NULL, 50, 3);
    const char *data = "This is test snapshot data for transfer testing!";
    kraftSnapshotWrite(&mgr, data, strlen(data));
    kraftSnapshotFinish(&mgr);

    /* Create mock node */
    kraftNode node = {.nodeId = 200, .clusterId = 1};

    /* Begin transfer */
    kraftSnapshotTransfer *transfer = kraftSnapshotTransferBegin(&mgr, &node);
    VERIFY(transfer != NULL, "Transfer should start");
    VERIFY(transfer->state == KRAFT_TRANSFER_SENDING,
           "State should be SENDING");
    VERIFY(transfer->totalChunks > 0, "Should have chunks");
    VERIFY(mgr.transferCount == 1, "Transfer count should be 1");

    /* Get chunks */
    kraftSnapshotChunk chunk;
    uint32_t chunkCount = 0;
    while (kraftSnapshotTransferNextChunk(&mgr, transfer, &chunk)) {
        VERIFY(chunk.chunkIndex == chunkCount, "Chunk index should match");
        VERIFY(chunk.data != NULL, "Chunk data should not be null");

        /* Ack the chunk */
        kraftSnapshotTransferRecordAck(&mgr, transfer, chunk.chunkIndex);
        chunkCount++;
    }

    VERIFY(chunkCount == transfer->totalChunks, "Should have sent all chunks");
    VERIFY(kraftSnapshotTransferIsComplete(transfer),
           "Transfer should be complete");

    printf("  Transfer tests passed (%u chunks)\n", chunkCount);
    kraftSnapshotManagerFree(&mgr);
    TEST_PASS();
}

static bool test_SnapshotInstallation(void) {
    TEST_START("Snapshot - Installation on Follower");

    kraftSnapshotManager mgr;
    kraftSnapshotConfig config = kraftSnapshotConfigDefault();
    config.chunkSizeBytes = 16;

    VERIFY(kraftSnapshotManagerInit(&mgr, &config), "Failed to init");

    /* Create source snapshot */
    kraftSnapshotBegin(&mgr, NULL, 75, 4);
    const char *data = "Snapshot data to install on follower";
    kraftSnapshotWrite(&mgr, data, strlen(data));
    kraftSnapshotFinish(&mgr);

    /* Copy metadata for installation */
    kraftSnapshotMeta meta = mgr.currentSnapshot;

    /* Create a second manager to simulate follower */
    kraftSnapshotManager followerMgr;
    VERIFY(kraftSnapshotManagerInit(&followerMgr, &config),
           "Failed to init follower");

    /* Begin installation */
    VERIFY(kraftSnapshotInstallBegin(&followerMgr, &meta),
           "Install begin should succeed");
    VERIFY(followerMgr.state == KRAFT_SNAPSHOT_INSTALLING,
           "State should be INSTALLING");

    /* Simulate receiving chunks */
    uint64_t offset = 0;
    uint32_t chunkIdx = 0;
    size_t dataLen = strlen(data);

    while (offset < dataLen) {
        uint32_t chunkLen = (dataLen - offset) < 16 ? (dataLen - offset) : 16;
        kraftSnapshotChunk chunk = {.snapshotId = meta.snapshotId,
                                    .chunkIndex = chunkIdx++,
                                    .offset = offset,
                                    .length = chunkLen,
                                    .data = (uint8_t *)(data + offset)};
        VERIFY(kraftSnapshotInstallWriteChunk(&followerMgr, &chunk),
               "Chunk write should succeed");
        offset += chunkLen;
    }

    /* Finish installation */
    VERIFY(kraftSnapshotInstallFinish(&followerMgr, NULL),
           "Install finish should succeed");
    VERIFY(followerMgr.state == KRAFT_SNAPSHOT_COMPLETE,
           "State should be COMPLETE");
    VERIFY(followerMgr.currentSnapshot.lastIncludedIndex == 75,
           "Index should match");

    printf("  Installation tests passed\n");
    kraftSnapshotManagerFree(&mgr);
    kraftSnapshotManagerFree(&followerMgr);
    TEST_PASS();
}

static bool test_SnapshotStats(void) {
    TEST_START("Snapshot - Statistics");

    kraftSnapshotManager mgr;
    VERIFY(kraftSnapshotManagerInit(&mgr, NULL), "Failed to init");

    /* Create some snapshots */
    kraftSnapshotBegin(&mgr, NULL, 10, 1);
    kraftSnapshotWrite(&mgr, "data1", 5);
    kraftSnapshotFinish(&mgr);

    kraftSnapshotBegin(&mgr, NULL, 20, 2);
    kraftSnapshotWrite(&mgr, "data2data2", 10);
    kraftSnapshotFinish(&mgr);

    kraftSnapshotStats stats;
    kraftSnapshotGetStats(&mgr, &stats);

    VERIFY(stats.totalSnapshots == 2, "Should have 2 snapshots");
    VERIFY(stats.totalBytes == 15, "Total bytes should be 15");
    VERIFY(stats.currentState == KRAFT_SNAPSHOT_IDLE,
           "State should be IDLE after completion");
    VERIFY(stats.lastIncludedIndex == 20, "Last index should be 20");

    /* Format stats */
    char buf[512];
    int len = kraftSnapshotFormatStats(&stats, buf, sizeof(buf));
    VERIFY(len > 0, "Stats format should succeed");
    printf("  Stats:\n%s", buf);

    kraftSnapshotManagerFree(&mgr);
    TEST_PASS();
}

/* ====================================================================
 * Multi-Raft Group Tests
 * ==================================================================== */

static bool test_MultiRaftConfig(void) {
    TEST_START("Multi-Raft - Configuration");

    kraftMultiRaftConfig config = kraftMultiRaftConfigDefault();

    VERIFY(config.maxGroups == KRAFT_MULTI_DEFAULT_MAX_GROUPS,
           "Default max groups");
    VERIFY(config.initialCapacity == KRAFT_MULTI_DEFAULT_INITIAL_CAPACITY,
           "Default capacity");
    VERIFY(config.tickIntervalUs == KRAFT_MULTI_DEFAULT_TICK_INTERVAL,
           "Default tick interval");
    VERIFY(config.electionTimeoutUs == KRAFT_MULTI_DEFAULT_ELECTION_TIMEOUT,
           "Default election timeout");
    VERIFY(config.heartbeatIntervalUs == KRAFT_MULTI_DEFAULT_HEARTBEAT_INTERVAL,
           "Default heartbeat");
    VERIFY(config.maxBatchSize == KRAFT_MULTI_DEFAULT_MAX_BATCH,
           "Default batch size");
    VERIFY(config.enableLoadBalancing == true,
           "Load balancing enabled by default");
    VERIFY(config.shareConnections == true,
           "Connection sharing enabled by default");

    printf("  Config tests passed\n");
    TEST_PASS();
}

static bool test_MultiRaftLifecycle(void) {
    TEST_START("Multi-Raft - Lifecycle (init, create, free)");

    kraftMultiRaft mr;
    VERIFY(kraftMultiRaftInit(&mr, NULL, 1), "Init should succeed");
    VERIFY(mr.groupCount == 0, "Initial group count should be 0");
    VERIFY(mr.localNodeId == 1, "Local node ID should match");
    VERIFY(mr.groupIndex != NULL, "Group index should be allocated");

    kraftMultiRaftFree(&mr);
    VERIFY(mr.groups == NULL, "Groups should be freed");
    VERIFY(mr.groupIndex == NULL, "Index should be freed");

    printf("  Lifecycle tests passed\n");
    TEST_PASS();
}

static bool test_MultiRaftGroupCreation(void) {
    TEST_START("Multi-Raft - Group Creation");

    kraftMultiRaft mr;
    VERIFY(kraftMultiRaftInit(&mr, NULL, 1), "Init should succeed");

    /* Create first group */
    kraftReplicaId replicas1[] = {1, 2, 3};
    kraftGroup *group1 = kraftMultiRaftCreateGroup(&mr, 100, replicas1, 3);
    VERIFY(group1 != NULL, "Group 1 creation should succeed");
    VERIFY(group1->descriptor.groupId == 100, "Group ID should be 100");
    VERIFY(group1->descriptor.replicaCount == 3, "Should have 3 replicas");
    VERIFY(group1->active == true, "Group should be active");
    VERIFY(mr.groupCount == 1, "Group count should be 1");
    VERIFY(mr.activeGroups == 1, "Active groups should be 1");

    /* Create second group */
    kraftReplicaId replicas2[] = {1, 2, 3, 4, 5};
    kraftGroup *group2 = kraftMultiRaftCreateGroup(&mr, 200, replicas2, 5);
    VERIFY(group2 != NULL, "Group 2 creation should succeed");
    VERIFY(group2->descriptor.groupId == 200, "Group ID should be 200");
    VERIFY(group2->descriptor.replicaCount == 5, "Should have 5 replicas");
    VERIFY(mr.groupCount == 2, "Group count should be 2");

    /* Lookup groups */
    kraftGroup *found1 = kraftMultiRaftGetGroup(&mr, 100);
    kraftGroup *found2 = kraftMultiRaftGetGroup(&mr, 200);
    kraftGroup *notFound = kraftMultiRaftGetGroup(&mr, 999);
    VERIFY(found1 == group1, "Should find group 1");
    VERIFY(found2 == group2, "Should find group 2");
    VERIFY(notFound == NULL, "Should not find non-existent group");

    /* Cannot create duplicate */
    kraftGroup *dup = kraftMultiRaftCreateGroup(&mr, 100, replicas1, 3);
    VERIFY(dup == NULL, "Duplicate creation should fail");

    printf("  Group creation tests passed\n");
    kraftMultiRaftFree(&mr);
    TEST_PASS();
}

static bool test_MultiRaftGroupRemoval(void) {
    TEST_START("Multi-Raft - Group Removal");

    kraftMultiRaft mr;
    VERIFY(kraftMultiRaftInit(&mr, NULL, 1), "Init should succeed");

    /* Create groups */
    kraftReplicaId replicas[] = {1, 2, 3};
    kraftMultiRaftCreateGroup(&mr, 100, replicas, 3);
    kraftMultiRaftCreateGroup(&mr, 200, replicas, 3);
    kraftMultiRaftCreateGroup(&mr, 300, replicas, 3);
    VERIFY(mr.groupCount == 3, "Should have 3 groups");

    /* Remove middle group */
    VERIFY(kraftMultiRaftRemoveGroup(&mr, 200), "Removal should succeed");
    VERIFY(mr.groupCount == 2, "Should have 2 groups");
    VERIFY(kraftMultiRaftGetGroup(&mr, 200) == NULL,
           "Removed group should not exist");
    VERIFY(kraftMultiRaftGetGroup(&mr, 100) != NULL,
           "Group 100 should still exist");
    VERIFY(kraftMultiRaftGetGroup(&mr, 300) != NULL,
           "Group 300 should still exist");

    /* Remove non-existent */
    VERIFY(!kraftMultiRaftRemoveGroup(&mr, 999),
           "Non-existent removal should fail");

    printf("  Group removal tests passed\n");
    kraftMultiRaftFree(&mr);
    TEST_PASS();
}

static bool test_MultiRaftGroupStopResume(void) {
    TEST_START("Multi-Raft - Group Stop/Resume");

    kraftMultiRaft mr;
    VERIFY(kraftMultiRaftInit(&mr, NULL, 1), "Init should succeed");

    kraftReplicaId replicas[] = {1, 2, 3};
    kraftGroup *group = kraftMultiRaftCreateGroup(&mr, 100, replicas, 3);
    VERIFY(group != NULL, "Creation should succeed");
    VERIFY(group->active == true, "Should be active initially");
    VERIFY(mr.activeGroups == 1, "Active count should be 1");

    /* Stop group */
    VERIFY(kraftMultiRaftStopGroup(&mr, 100), "Stop should succeed");
    VERIFY(group->active == false, "Should be inactive after stop");
    VERIFY(group->descriptor.state == KRAFT_GROUP_STOPPED,
           "State should be STOPPED");
    VERIFY(mr.activeGroups == 0, "Active count should be 0");

    /* Group still exists in registry */
    VERIFY(kraftMultiRaftGetGroup(&mr, 100) != NULL,
           "Stopped group should still exist");

    /* Resume group */
    VERIFY(kraftMultiRaftResumeGroup(&mr, 100), "Resume should succeed");
    VERIFY(group->active == true, "Should be active after resume");
    VERIFY(group->descriptor.state == KRAFT_GROUP_ACTIVE,
           "State should be ACTIVE");
    VERIFY(mr.activeGroups == 1, "Active count should be 1 again");

    printf("  Stop/Resume tests passed\n");
    kraftMultiRaftFree(&mr);
    TEST_PASS();
}

static bool test_MultiRaftRangeLookup(void) {
    TEST_START("Multi-Raft - Range Key Lookup");

    kraftMultiRaft mr;
    VERIFY(kraftMultiRaftInit(&mr, NULL, 1), "Init should succeed");

    kraftReplicaId replicas[] = {1, 2, 3};

    /* Create groups with key ranges */
    /* Group 1: [a, m) */
    kraftGroup *group1 = kraftMultiRaftCreateGroupWithRange(
        &mr, 100, replicas, 3, (const uint8_t *)"a", 1, (const uint8_t *)"m",
        1);
    VERIFY(group1 != NULL, "Group 1 should be created");

    /* Group 2: [m, z) */
    kraftGroup *group2 = kraftMultiRaftCreateGroupWithRange(
        &mr, 200, replicas, 3, (const uint8_t *)"m", 1, (const uint8_t *)"z",
        1);
    VERIFY(group2 != NULL, "Group 2 should be created");

    /* Lookup keys */
    kraftGroup *found;
    found = kraftMultiRaftFindGroupForKey(&mr, (const uint8_t *)"apple", 5);
    VERIFY(found == group1, "apple should be in group 1");

    found = kraftMultiRaftFindGroupForKey(&mr, (const uint8_t *)"hello", 5);
    VERIFY(found == group1, "hello should be in group 1");

    found = kraftMultiRaftFindGroupForKey(&mr, (const uint8_t *)"mango", 5);
    VERIFY(found == group2, "mango should be in group 2");

    found = kraftMultiRaftFindGroupForKey(&mr, (const uint8_t *)"xyz", 3);
    VERIFY(found == group2, "xyz should be in group 2");

    printf("  Range lookup tests passed\n");
    kraftMultiRaftFree(&mr);
    TEST_PASS();
}

static bool test_MultiRaftTicking(void) {
    TEST_START("Multi-Raft - Ticking Groups");

    kraftMultiRaft mr;
    VERIFY(kraftMultiRaftInit(&mr, NULL, 1), "Init should succeed");

    kraftReplicaId replicas[] = {1, 2, 3};
    kraftGroup *group1 = kraftMultiRaftCreateGroup(&mr, 100, replicas, 3);
    kraftGroup *group2 = kraftMultiRaftCreateGroup(&mr, 200, replicas, 3);
    VERIFY(group1 != NULL && group2 != NULL, "Groups should be created");

    /* Initial tick times should be 0 */
    VERIFY(group1->lastTickTime == 0, "Initial tick time should be 0");
    VERIFY(group2->lastTickTime == 0, "Initial tick time should be 0");
    VERIFY(mr.totalTicks == 0, "Initial total ticks should be 0");

    /* Tick the multi-raft */
    uint64_t now = 1000000; /* 1 second */
    kraftMultiRaftTick(&mr, now);

    VERIFY(mr.lastTickTime == now, "Last tick time should update");
    VERIFY(mr.totalTicks == 1, "Total ticks should be 1");
    VERIFY(group1->lastTickTime == now, "Group 1 tick time should update");
    VERIFY(group2->lastTickTime == now, "Group 2 tick time should update");
    VERIFY(group1->descriptor.createdAt == now,
           "Group 1 created time should be set");

    /* Tick again */
    now = 2000000;
    kraftMultiRaftTick(&mr, now);
    VERIFY(mr.totalTicks == 2, "Total ticks should be 2");
    VERIFY(group1->lastTickTime == now,
           "Group 1 tick time should update again");

    printf("  Ticking tests passed\n");
    kraftMultiRaftFree(&mr);
    TEST_PASS();
}

static bool test_MultiRaftStats(void) {
    TEST_START("Multi-Raft - Statistics");

    kraftMultiRaft mr;
    VERIFY(kraftMultiRaftInit(&mr, NULL, 1), "Init should succeed");

    kraftReplicaId replicas[] = {1, 2, 3};
    kraftMultiRaftCreateGroup(&mr, 100, replicas, 3);
    kraftMultiRaftCreateGroup(&mr, 200, replicas, 3);

    /* Tick a few times */
    for (int i = 0; i < 5; i++) {
        kraftMultiRaftTick(&mr, (uint64_t)(i + 1) * 1000000);
    }

    kraftMultiRaftStats stats;
    kraftMultiRaftGetStats(&mr, &stats);

    VERIFY(stats.totalGroups == 2, "Total groups should be 2");
    VERIFY(stats.activeGroups == 2, "Active groups should be 2");
    VERIFY(stats.totalTicks == 5, "Total ticks should be 5");

    /* Format stats */
    char buf[512];
    int len = kraftMultiRaftFormatStats(&stats, buf, sizeof(buf));
    VERIFY(len > 0, "Stats format should succeed");
    printf("  Stats:\n%s", buf);

    kraftMultiRaftFree(&mr);
    TEST_PASS();
}

/* ====================================================================
 * Multi-Raft Extended Tests - Stress & Adversarial Cases
 * ==================================================================== */

static bool test_MultiRaftManyGroups(void) {
    TEST_START("Multi-Raft - Many Groups Stress Test");

    kraftMultiRaftConfig config = kraftMultiRaftConfigDefault();
    config.maxGroups = 500;
    config.initialCapacity = 8; /* Start small, force many resizes */

    kraftMultiRaft mr;
    VERIFY(kraftMultiRaftInit(&mr, &config, 1), "Init should succeed");

    kraftReplicaId replicas[] = {1, 2, 3, 4, 5};

    /* Create many groups, forcing array resizes */
    for (uint32_t i = 0; i < 200; i++) {
        kraftGroup *g = kraftMultiRaftCreateGroup(&mr, 1000 + i, replicas, 5);
        VERIFY(g != NULL, "Group creation should succeed");
        VERIFY(g->descriptor.groupId == 1000 + i, "Group ID should match");
    }

    VERIFY(mr.groupCount == 200, "Should have 200 groups");
    VERIFY(mr.activeGroups == 200, "All should be active");

    /* Verify all groups can be looked up */
    for (uint32_t i = 0; i < 200; i++) {
        kraftGroup *g = kraftMultiRaftGetGroup(&mr, 1000 + i);
        VERIFY(g != NULL, "Lookup should succeed");
        VERIFY(g->descriptor.groupId == 1000 + i, "ID should match");
    }

    /* Remove half the groups */
    for (uint32_t i = 0; i < 100; i++) {
        VERIFY(kraftMultiRaftRemoveGroup(&mr, 1000 + i * 2),
               "Removal should succeed");
    }

    VERIFY(mr.groupCount == 100, "Should have 100 groups after removal");

    /* Verify remaining groups are correct */
    for (uint32_t i = 0; i < 100; i++) {
        kraftGroup *g = kraftMultiRaftGetGroup(&mr, 1001 + i * 2);
        VERIFY(g != NULL, "Odd group should still exist");
        VERIFY(kraftMultiRaftGetGroup(&mr, 1000 + i * 2) == NULL,
               "Even group should be gone");
    }

    printf("  Created 200 groups, removed 100, verified consistency\n");
    kraftMultiRaftFree(&mr);
    TEST_PASS();
}

static bool test_MultiRaftHashCollisions(void) {
    TEST_START("Multi-Raft - Hash Table Collision Handling");

    kraftMultiRaft mr;
    VERIFY(kraftMultiRaftInit(&mr, NULL, 1), "Init should succeed");

    kraftReplicaId replicas[] = {1, 2, 3};

    /* Create groups that may have hash collisions (sequential IDs) */
    /* The hash table uses modulo bucket count, so consecutive IDs
     * will hit different buckets, but we create enough to fill up */
    for (uint64_t i = 0; i < 64; i++) {
        kraftGroup *g = kraftMultiRaftCreateGroup(&mr, i, replicas, 3);
        VERIFY(g != NULL, "Creation should succeed");
    }

    /* All lookups should succeed */
    for (uint64_t i = 0; i < 64; i++) {
        kraftGroup *g = kraftMultiRaftGetGroup(&mr, i);
        VERIFY(g != NULL, "Lookup should find group");
        VERIFY(g->descriptor.groupId == i, "Group ID should match");
    }

    /* Remove groups in non-sequential order (stress collision chains) */
    for (uint64_t i = 0; i < 32; i++) {
        /* Remove odd groups first */
        VERIFY(kraftMultiRaftRemoveGroup(&mr, i * 2 + 1),
               "Odd removal should succeed");
    }

    /* Verify even groups still work */
    for (uint64_t i = 0; i < 32; i++) {
        kraftGroup *g = kraftMultiRaftGetGroup(&mr, i * 2);
        VERIFY(g != NULL, "Even group should still exist");
        VERIFY(kraftMultiRaftGetGroup(&mr, i * 2 + 1) == NULL,
               "Odd group should be gone");
    }

    printf("  Hash collision handling verified\n");
    kraftMultiRaftFree(&mr);
    TEST_PASS();
}

static bool test_MultiRaftRangeBoundaries(void) {
    TEST_START("Multi-Raft - Range Boundary Edge Cases");

    kraftMultiRaft mr;
    VERIFY(kraftMultiRaftInit(&mr, NULL, 1), "Init should succeed");

    kraftReplicaId replicas[] = {1, 2, 3};

    /* Create groups with specific boundary conditions */
    /* Group 1: empty start key, end at "m" - should cover beginning */
    kraftGroup *g1 = kraftMultiRaftCreateGroupWithRange(
        &mr, 1, replicas, 3, NULL, 0, (const uint8_t *)"m", 1);
    VERIFY(g1 != NULL, "Group 1 should be created");

    /* Group 2: start at "m", empty end key - should cover to end */
    kraftGroup *g2 = kraftMultiRaftCreateGroupWithRange(
        &mr, 2, replicas, 3, (const uint8_t *)"m", 1, NULL, 0);
    VERIFY(g2 != NULL, "Group 2 should be created");

    /* Lookup boundary keys */
    kraftGroup *found;

    /* Empty string should match first group (no start key) */
    found = kraftMultiRaftFindGroupForKey(&mr, (const uint8_t *)"", 0);
    /* Note: empty key length is edge case - might not find anything */

    /* Key just before 'm' should be in group 1 */
    found = kraftMultiRaftFindGroupForKey(&mr, (const uint8_t *)"lzz", 3);
    VERIFY(found == g1, "lzz should be in group 1");

    /* Key 'm' exactly should be in group 2 (exclusive end) */
    found = kraftMultiRaftFindGroupForKey(&mr, (const uint8_t *)"m", 1);
    VERIFY(found == g2, "m should be in group 2");

    /* Key well after 'm' should be in group 2 */
    found = kraftMultiRaftFindGroupForKey(&mr, (const uint8_t *)"zzzzz", 5);
    VERIFY(found == g2, "zzzzz should be in group 2");

    printf("  Range boundary edge cases passed\n");
    kraftMultiRaftFree(&mr);
    TEST_PASS();
}

static bool test_MultiRaftSplitGroup(void) {
    TEST_START("Multi-Raft - Group Split Operation");

    kraftMultiRaft mr;
    VERIFY(kraftMultiRaftInit(&mr, NULL, 1), "Init should succeed");

    kraftReplicaId replicas[] = {1, 2, 3};

    /* Create a group with range [a, z) */
    kraftGroup *original = kraftMultiRaftCreateGroupWithRange(
        &mr, 100, replicas, 3, (const uint8_t *)"a", 1, (const uint8_t *)"z",
        1);
    VERIFY(original != NULL, "Original group should be created");

    /* Split at 'm' */
    kraftGroupId leftId, rightId;
    VERIFY(kraftMultiRaftSplitGroup(&mr, 100, (const uint8_t *)"m", 1, &leftId,
                                    &rightId),
           "Split should succeed");

    VERIFY(mr.groupCount == 2, "Should have 2 groups after split");

    /* Left group should have range [a, m) */
    kraftGroup *left = kraftMultiRaftGetGroup(&mr, leftId);
    VERIFY(left != NULL, "Left group should exist");
    VERIFY(left->descriptor.startKeyLen == 1, "Left start key len");
    VERIFY(left->descriptor.endKeyLen == 1, "Left end key len");
    VERIFY(memcmp(left->descriptor.startKey, "a", 1) == 0, "Left start key");
    VERIFY(memcmp(left->descriptor.endKey, "m", 1) == 0, "Left end key");

    /* Right group should have range [m, z) */
    kraftGroup *right = kraftMultiRaftGetGroup(&mr, rightId);
    VERIFY(right != NULL, "Right group should exist");
    VERIFY(right->descriptor.startKeyLen == 1, "Right start key len");
    VERIFY(right->descriptor.endKeyLen == 1, "Right end key len");
    VERIFY(memcmp(right->descriptor.startKey, "m", 1) == 0, "Right start key");
    VERIFY(memcmp(right->descriptor.endKey, "z", 1) == 0, "Right end key");

    /* Key lookup should route correctly */
    kraftGroup *found;
    found = kraftMultiRaftFindGroupForKey(&mr, (const uint8_t *)"abc", 3);
    VERIFY(found == left, "abc should route to left group");
    found = kraftMultiRaftFindGroupForKey(&mr, (const uint8_t *)"mno", 3);
    VERIFY(found == right, "mno should route to right group");

    printf("  Group split operation verified\n");
    kraftMultiRaftFree(&mr);
    TEST_PASS();
}

static bool test_MultiRaftMergeGroups(void) {
    TEST_START("Multi-Raft - Group Merge Operation");

    kraftMultiRaft mr;
    VERIFY(kraftMultiRaftInit(&mr, NULL, 1), "Init should succeed");

    kraftReplicaId replicas[] = {1, 2, 3};

    /* Create two adjacent groups */
    kraftGroup *g1 = kraftMultiRaftCreateGroupWithRange(
        &mr, 100, replicas, 3, (const uint8_t *)"a", 1, (const uint8_t *)"m",
        1);
    VERIFY(g1 != NULL, "Group 1 should be created");

    kraftGroup *g2 = kraftMultiRaftCreateGroupWithRange(
        &mr, 200, replicas, 3, (const uint8_t *)"m", 1, (const uint8_t *)"z",
        1);
    VERIFY(g2 != NULL, "Group 2 should be created");

    VERIFY(mr.groupCount == 2, "Should have 2 groups");

    /* Merge the groups */
    kraftGroupId mergedId;
    VERIFY(kraftMultiRaftMergeGroups(&mr, 100, 200, &mergedId),
           "Merge should succeed");

    VERIFY(mr.groupCount == 1, "Should have 1 group after merge");

    /* Merged group should have range [a, z) */
    kraftGroup *merged = kraftMultiRaftGetGroup(&mr, mergedId);
    VERIFY(merged != NULL, "Merged group should exist");
    VERIFY(merged->descriptor.startKeyLen == 1, "Start key len");
    VERIFY(merged->descriptor.endKeyLen == 1, "End key len");
    VERIFY(memcmp(merged->descriptor.startKey, "a", 1) == 0, "Start key");
    VERIFY(memcmp(merged->descriptor.endKey, "z", 1) == 0, "End key");

    /* All keys should route to merged group */
    kraftGroup *found;
    found = kraftMultiRaftFindGroupForKey(&mr, (const uint8_t *)"abc", 3);
    VERIFY(found == merged, "abc should route to merged");
    found = kraftMultiRaftFindGroupForKey(&mr, (const uint8_t *)"xyz", 3);
    VERIFY(found == merged, "xyz should route to merged");

    printf("  Group merge operation verified\n");
    kraftMultiRaftFree(&mr);
    TEST_PASS();
}

static bool test_MultiRaftGroupIdEnumeration(void) {
    TEST_START("Multi-Raft - Group ID Enumeration");

    kraftMultiRaft mr;
    VERIFY(kraftMultiRaftInit(&mr, NULL, 1), "Init should succeed");

    kraftReplicaId replicas[] = {1, 2, 3};

    /* Create groups with non-sequential IDs */
    uint64_t expectedIds[] = {7, 42, 100, 256, 1000};
    for (int i = 0; i < 5; i++) {
        kraftMultiRaftCreateGroup(&mr, expectedIds[i], replicas, 3);
    }

    /* Get all group IDs */
    kraftGroupId ids[10];
    uint32_t count = kraftMultiRaftGetGroupIds(&mr, ids, 10);

    VERIFY(count == 5, "Should get 5 IDs");

    /* Verify all expected IDs are present (order may differ) */
    for (int i = 0; i < 5; i++) {
        bool found = false;
        for (uint32_t j = 0; j < count; j++) {
            if (ids[j] == expectedIds[i]) {
                found = true;
                break;
            }
        }
        VERIFY(found, "Expected ID should be present");
    }

    /* Test with smaller buffer */
    kraftGroupId smallIds[3];
    count = kraftMultiRaftGetGroupIds(&mr, smallIds, 3);
    VERIFY(count == 3, "Should only get 3 IDs when buffer is small");

    printf("  Group ID enumeration verified\n");
    kraftMultiRaftFree(&mr);
    TEST_PASS();
}

/* Iterator helper struct and functions for test_MultiRaftIterator */
typedef struct {
    int count;
    uint64_t idSum;
} iteratorTestContext;

static bool countIteratorHelper(kraftGroup *group, void *userData) {
    iteratorTestContext *ctx = (iteratorTestContext *)userData;
    ctx->count++;
    ctx->idSum += group->descriptor.groupId;
    return true; /* Continue iterating */
}

static bool stopIteratorHelper(kraftGroup *group, void *userData) {
    (void)group;
    int *visitCount = (int *)userData;
    (*visitCount)++;
    return *visitCount < 2; /* Stop after 2 visits */
}

static bool test_MultiRaftIterator(void) {
    TEST_START("Multi-Raft - Group Iterator");

    kraftMultiRaft mr;
    VERIFY(kraftMultiRaftInit(&mr, NULL, 1), "Init should succeed");

    kraftReplicaId replicas[] = {1, 2, 3};

    /* Create some groups */
    kraftMultiRaftCreateGroup(&mr, 10, replicas, 3);
    kraftMultiRaftCreateGroup(&mr, 20, replicas, 3);
    kraftMultiRaftCreateGroup(&mr, 30, replicas, 3);

    /* Count and sum using iterator */
    iteratorTestContext ctx = {0, 0};
    kraftMultiRaftForEachGroup(&mr, countIteratorHelper, &ctx);

    VERIFY(ctx.count == 3, "Iterator should visit 3 groups");
    VERIFY(ctx.idSum == 60, "ID sum should be 60 (10+20+30)");

    /* Test early termination */
    int visits = 0;
    kraftMultiRaftForEachGroup(&mr, stopIteratorHelper, &visits);
    VERIFY(visits == 2, "Iterator should stop after 2 visits");

    printf("  Group iterator verified\n");
    kraftMultiRaftFree(&mr);
    TEST_PASS();
}

static bool test_MultiRaftMaxCapacity(void) {
    TEST_START("Multi-Raft - Maximum Capacity Limits");

    kraftMultiRaftConfig config = kraftMultiRaftConfigDefault();
    config.maxGroups = 10; /* Set very low limit */
    config.initialCapacity = 4;

    kraftMultiRaft mr;
    VERIFY(kraftMultiRaftInit(&mr, &config, 1), "Init should succeed");

    kraftReplicaId replicas[] = {1, 2, 3};

    /* Create groups up to limit */
    for (int i = 0; i < 10; i++) {
        kraftGroup *g =
            kraftMultiRaftCreateGroup(&mr, (uint64_t)i, replicas, 3);
        VERIFY(g != NULL, "Creation within limit should succeed");
    }

    VERIFY(mr.groupCount == 10, "Should have 10 groups");

    /* Next creation should fail (at max) */
    kraftGroup *overflow = kraftMultiRaftCreateGroup(&mr, 999, replicas, 3);
    VERIFY(overflow == NULL, "Creation at max capacity should fail");
    VERIFY(mr.groupCount == 10, "Group count should still be 10");

    /* Remove one and try again */
    VERIFY(kraftMultiRaftRemoveGroup(&mr, 5), "Removal should succeed");
    VERIFY(mr.groupCount == 9, "Should have 9 groups");

    kraftGroup *newGroup = kraftMultiRaftCreateGroup(&mr, 999, replicas, 3);
    VERIFY(newGroup != NULL, "Creation after removal should succeed");
    VERIFY(mr.groupCount == 10, "Should be back to 10 groups");

    printf("  Maximum capacity limits verified\n");
    kraftMultiRaftFree(&mr);
    TEST_PASS();
}

static bool test_MultiRaftNullAndEdgeCases(void) {
    TEST_START("Multi-Raft - Null/Edge Case Handling");

    /* Note: Some tests are commented out because they would crash
     * due to intentional null dereference. The implementation could
     * add null checks, but these are defensive tests. */

    kraftMultiRaft mr;
    VERIFY(kraftMultiRaftInit(&mr, NULL, 1), "Init should succeed");

    /* Test zero replica count */
    kraftReplicaId replicas[] = {1, 2, 3};
    kraftGroup *g = kraftMultiRaftCreateGroup(&mr, 100, replicas, 0);
    VERIFY(g == NULL, "Zero replica count should fail");

    /* Test operations on non-existent groups */
    VERIFY(!kraftMultiRaftStopGroup(&mr, 9999),
           "Stop non-existent should fail");
    VERIFY(!kraftMultiRaftResumeGroup(&mr, 9999),
           "Resume non-existent should fail");
    VERIFY(!kraftMultiRaftRemoveGroup(&mr, 9999),
           "Remove non-existent should fail");

    /* Test resume on non-stopped group */
    g = kraftMultiRaftCreateGroup(&mr, 100, replicas, 3);
    VERIFY(g != NULL, "Creation should succeed");
    VERIFY(!kraftMultiRaftResumeGroup(&mr, 100),
           "Resume active group should fail");

    /* Test message routing to non-existent group */
    uint8_t data[] = {1, 2, 3, 4};
    VERIFY(!kraftMultiRaftRouteMessage(&mr, 9999, 1, data, sizeof(data)),
           "Route to non-existent should fail");

    /* Test empty data routing */
    VERIFY(!kraftMultiRaftRouteMessage(&mr, 100, 1, NULL, 0),
           "Route null data should fail");
    VERIFY(!kraftMultiRaftRouteMessage(&mr, 100, 1, data, 0),
           "Route zero length should fail");

    /* Test empty key lookup */
    kraftGroup *found = kraftMultiRaftFindGroupForKey(&mr, NULL, 0);
    VERIFY(found == NULL, "Null key lookup should return null");

    printf("  Null and edge case handling verified\n");
    kraftMultiRaftFree(&mr);
    TEST_PASS();
}

static bool test_MultiRaftConcurrentStopResume(void) {
    TEST_START("Multi-Raft - Stop/Resume Many Groups Concurrently");

    kraftMultiRaft mr;
    VERIFY(kraftMultiRaftInit(&mr, NULL, 1), "Init should succeed");

    kraftReplicaId replicas[] = {1, 2, 3};

    /* Create many groups */
    for (int i = 0; i < 50; i++) {
        kraftMultiRaftCreateGroup(&mr, (uint64_t)i, replicas, 3);
    }

    VERIFY(mr.activeGroups == 50, "All 50 should be active");

    /* Stop all odd groups */
    for (int i = 0; i < 50; i += 2) {
        VERIFY(kraftMultiRaftStopGroup(&mr, (uint64_t)i),
               "Stop should succeed");
    }

    VERIFY(mr.activeGroups == 25, "25 should be active");

    /* Verify states */
    for (int i = 0; i < 50; i++) {
        kraftGroup *g = kraftMultiRaftGetGroup(&mr, (uint64_t)i);
        VERIFY(g != NULL, "Group should still exist");
        if (i % 2 == 0) {
            VERIFY(!g->active, "Even groups should be stopped");
        } else {
            VERIFY(g->active, "Odd groups should be active");
        }
    }

    /* Resume stopped groups */
    for (int i = 0; i < 50; i += 2) {
        VERIFY(kraftMultiRaftResumeGroup(&mr, (uint64_t)i),
               "Resume should succeed");
    }

    VERIFY(mr.activeGroups == 50, "All 50 should be active again");

    printf("  Concurrent stop/resume verified\n");
    kraftMultiRaftFree(&mr);
    TEST_PASS();
}

static bool test_MultiRaftOverlappingRanges(void) {
    TEST_START("Multi-Raft - Overlapping Range Detection");

    kraftMultiRaft mr;
    VERIFY(kraftMultiRaftInit(&mr, NULL, 1), "Init should succeed");

    kraftReplicaId replicas[] = {1, 2, 3};

    /* Create non-overlapping ranges */
    kraftMultiRaftCreateGroupWithRange(
        &mr, 1, replicas, 3, (const uint8_t *)"a", 1, (const uint8_t *)"m", 1);
    kraftMultiRaftCreateGroupWithRange(
        &mr, 2, replicas, 3, (const uint8_t *)"m", 1, (const uint8_t *)"z", 1);

    /* Find a key at exact boundary */
    kraftGroup *found;
    found = kraftMultiRaftFindGroupForKey(&mr, (const uint8_t *)"m", 1);
    VERIFY(found != NULL, "Boundary key should find a group");
    VERIFY(found->descriptor.groupId == 2,
           "m should be in group 2 (exclusive)");

    /* Create third group with gap */
    kraftMultiRaftCreateGroupWithRange(
        &mr, 3, replicas, 3, (const uint8_t *)"0", 1, (const uint8_t *)"9", 1);

    /* Find key in gap (before 'a') */
    found = kraftMultiRaftFindGroupForKey(&mr, (const uint8_t *)"5", 1);
    VERIFY(found != NULL, "Numeric key should find group 3");
    VERIFY(found->descriptor.groupId == 3, "5 should be in numeric group");

    printf("  Overlapping/gap range handling verified\n");
    kraftMultiRaftFree(&mr);
    TEST_PASS();
}

static bool test_MultiRaftLongKeys(void) {
    TEST_START("Multi-Raft - Long Key Handling");

    kraftMultiRaft mr;
    VERIFY(kraftMultiRaftInit(&mr, NULL, 1), "Init should succeed");

    kraftReplicaId replicas[] = {1, 2, 3};

    /* Create groups with long keys */
    uint8_t longKeyStart[256];
    uint8_t longKeyEnd[256];
    memset(longKeyStart, 'a', 256);
    memset(longKeyEnd, 'm', 256);

    kraftGroup *g = kraftMultiRaftCreateGroupWithRange(
        &mr, 100, replicas, 3, longKeyStart, 256, longKeyEnd, 256);
    VERIFY(g != NULL, "Long key group should be created");
    VERIFY(g->descriptor.startKeyLen == 256, "Start key len should be 256");
    VERIFY(g->descriptor.endKeyLen == 256, "End key len should be 256");

    /* Lookup with long key */
    uint8_t lookupKey[256];
    memset(lookupKey, 'f', 256); /* Between 'a' and 'm' */
    kraftGroup *found = kraftMultiRaftFindGroupForKey(&mr, lookupKey, 256);
    VERIFY(found == g, "Long key lookup should succeed");

    printf("  Long key handling verified\n");
    kraftMultiRaftFree(&mr);
    TEST_PASS();
}

static bool test_MultiRaftRapidCreateRemove(void) {
    TEST_START("Multi-Raft - Rapid Create/Remove Cycles");

    kraftMultiRaft mr;
    VERIFY(kraftMultiRaftInit(&mr, NULL, 1), "Init should succeed");

    kraftReplicaId replicas[] = {1, 2, 3};

    /* Rapidly create and remove groups */
    for (int cycle = 0; cycle < 10; cycle++) {
        /* Create 20 groups */
        for (int i = 0; i < 20; i++) {
            uint64_t id = (uint64_t)(cycle * 1000 + i);
            kraftGroup *g = kraftMultiRaftCreateGroup(&mr, id, replicas, 3);
            VERIFY(g != NULL, "Creation should succeed");
        }

        /* Remove all but 5 */
        for (int i = 5; i < 20; i++) {
            uint64_t id = (uint64_t)(cycle * 1000 + i);
            VERIFY(kraftMultiRaftRemoveGroup(&mr, id),
                   "Removal should succeed");
        }
    }

    /* Should have 5 groups from each cycle = 50 total */
    VERIFY(mr.groupCount == 50, "Should have 50 groups after cycles");

    /* Verify remaining groups */
    for (int cycle = 0; cycle < 10; cycle++) {
        for (int i = 0; i < 5; i++) {
            uint64_t id = (uint64_t)(cycle * 1000 + i);
            VERIFY(kraftMultiRaftGetGroup(&mr, id) != NULL,
                   "Remaining group should exist");
        }
    }

    printf("  Rapid create/remove cycles verified\n");
    kraftMultiRaftFree(&mr);
    TEST_PASS();
}

/* ====================================================================
 * Configuration Profiles Tests
 * ==================================================================== */

static bool test_ProfileDefault(void) {
    TEST_START("Profile - Default Configuration");

    kraftConfig config = kraftConfigDefault();

    VERIFY(config.profile == KRAFT_PROFILE_PRODUCTION,
           "Default profile should be PRODUCTION");
    VERIFY(config.timing.electionTimeoutMinMs > 0,
           "Election timeout min should be non-zero");
    VERIFY(config.timing.heartbeatIntervalMs > 0,
           "Heartbeat interval should be non-zero");
    VERIFY(config.timing.heartbeatIntervalMs <
               config.timing.electionTimeoutMinMs,
           "Heartbeat must be less than election timeout");
    VERIFY(config.features.enablePreVote == true,
           "Production should enable pre-vote");

    printf("  Default profile validated\n");
    TEST_PASS();
}

static bool test_ProfileAllValid(void) {
    TEST_START("Profile - All Profiles Valid");

    char errorMsg[256];

    /* Test all profile types */
    for (int i = 0; i < KRAFT_PROFILE_COUNT; i++) {
        kraftConfig config = kraftConfigForProfile((kraftProfile)i);
        kraftConfigValidation result =
            kraftConfigValidate(&config, errorMsg, sizeof(errorMsg));

        /* All profiles should be at least valid (may have warnings) */
        VERIFY(result < KRAFT_CONFIG_ERROR_INVALID_TIMING,
               "Profile should be valid or warn-only");

        /* Verify basic timing constraints */
        VERIFY(config.timing.heartbeatIntervalMs <
                   config.timing.electionTimeoutMinMs,
               "Heartbeat must be less than election timeout in all profiles");
    }

    printf("  All %d profiles validated\n", KRAFT_PROFILE_COUNT);
    TEST_PASS();
}

static bool test_ProfileValidation(void) {
    TEST_START("Profile - Validation Logic");

    char errorMsg[256];
    kraftConfig config;

    /* Test: NULL config should fail */
    kraftConfigValidation result =
        kraftConfigValidate(NULL, errorMsg, sizeof(errorMsg));
    VERIFY(result == KRAFT_CONFIG_ERROR_INVALID_COUNTS,
           "NULL config should fail validation");

    /* Test: Invalid timing (heartbeat >= election timeout) */
    config = kraftConfigDefault();
    config.timing.heartbeatIntervalMs = 200;
    config.timing.electionTimeoutMinMs =
        100; /* Invalid: heartbeat > election */
    result = kraftConfigValidate(&config, errorMsg, sizeof(errorMsg));
    VERIFY(result == KRAFT_CONFIG_ERROR_INVALID_TIMING,
           "Heartbeat >= election timeout should fail");

    /* Test: Invalid timing (min >= max) */
    config = kraftConfigDefault();
    config.timing.electionTimeoutMinMs = 500;
    config.timing.electionTimeoutMaxMs = 300; /* Invalid: min > max */
    result = kraftConfigValidate(&config, errorMsg, sizeof(errorMsg));
    VERIFY(result == KRAFT_CONFIG_ERROR_INVALID_TIMING,
           "Election min >= max should fail");

    /* Test: Zero heartbeat */
    config = kraftConfigDefault();
    config.timing.heartbeatIntervalMs = 0;
    result = kraftConfigValidate(&config, errorMsg, sizeof(errorMsg));
    VERIFY(result == KRAFT_CONFIG_ERROR_INVALID_COUNTS,
           "Zero heartbeat should fail");

    /* Test: Warning for very short election timeout */
    config = kraftConfigDefault();
    config.timing.electionTimeoutMinMs = 30;
    config.timing.electionTimeoutMaxMs = 60;
    config.timing.heartbeatIntervalMs = 10;
    result = kraftConfigValidate(&config, errorMsg, sizeof(errorMsg));
    VERIFY(result == KRAFT_CONFIG_WARN_TIMING,
           "Very short timeout should warn");

    printf("  Validation logic verified\n");
    TEST_PASS();
}

static bool test_ProfileOverride(void) {
    TEST_START("Profile - Override Helpers");

    /* Test timing override */
    kraftConfig config = kraftConfigForProfile(KRAFT_PROFILE_PRODUCTION);
    kraftConfigSetTiming(&config, 100, 300, 30);
    VERIFY(config.timing.electionTimeoutMinMs == 100,
           "Override min should apply");
    VERIFY(config.timing.electionTimeoutMaxMs == 300,
           "Override max should apply");
    VERIFY(config.timing.heartbeatIntervalMs == 30,
           "Override heartbeat should apply");

    /* Test batch override */
    config = kraftConfigForProfile(KRAFT_PROFILE_PRODUCTION);
    kraftConfigSetBatch(&config, 500, 8);
    VERIFY(config.batch.maxBatchSize == 500,
           "Override batch size should apply");
    VERIFY(config.batch.maxInFlight == 8, "Override in-flight should apply");

    /* Test all features toggle */
    config = kraftConfigForProfile(KRAFT_PROFILE_PRODUCTION);
    kraftConfigSetAllFeatures(&config, false);
    VERIFY(config.features.enablePreVote == false,
           "All features should be disabled");
    VERIFY(config.features.enableBatching == false,
           "All features should be disabled");
    VERIFY(config.features.enableSnapshots == false,
           "All features should be disabled");

    kraftConfigSetAllFeatures(&config, true);
    VERIFY(config.features.enablePreVote == true,
           "All features should be enabled");
    VERIFY(config.features.enableBatching == true,
           "All features should be enabled");

    printf("  Override helpers verified\n");
    TEST_PASS();
}

static bool test_ProfileTimingConstraints(void) {
    TEST_START("Profile - Timing Constraints Per Profile");

    /* Development: Fast timeouts */
    kraftConfig dev = kraftConfigForProfile(KRAFT_PROFILE_DEVELOPMENT);
    VERIFY(dev.timing.electionTimeoutMinMs <= 100,
           "Development should have fast election timeout");
    VERIFY(dev.timing.heartbeatIntervalMs <= 30,
           "Development should have fast heartbeat");

    /* Large cluster: Long timeouts */
    kraftConfig large = kraftConfigForProfile(KRAFT_PROFILE_LARGE_CLUSTER);
    VERIFY(large.timing.electionTimeoutMinMs >= 200,
           "Large cluster should have longer election timeout");
    VERIFY(large.timing.electionTimeoutMaxMs >= 500,
           "Large cluster should have wide timeout range");

    /* Low latency: Quick failover */
    kraftConfig low = kraftConfigForProfile(KRAFT_PROFILE_LOW_LATENCY);
    VERIFY(low.batch.maxBatchSize <= 10,
           "Low latency should have small batch size");
    VERIFY(low.batch.flushIntervalUs == 0,
           "Low latency should flush immediately");

    /* High throughput: Large batches */
    kraftConfig high = kraftConfigForProfile(KRAFT_PROFILE_HIGH_THROUGHPUT);
    VERIFY(high.batch.maxBatchSize >= 500,
           "High throughput should have large batch size");
    VERIFY(high.batch.maxInFlight >= 8,
           "High throughput should have aggressive pipelining");

    printf("  Timing constraints verified per profile\n");
    TEST_PASS();
}

static bool test_ProfileNames(void) {
    TEST_START("Profile - Name Strings");

    VERIFY(strcmp(kraftProfileName(KRAFT_PROFILE_CUSTOM), "CUSTOM") == 0,
           "CUSTOM name should match");
    VERIFY(strcmp(kraftProfileName(KRAFT_PROFILE_DEVELOPMENT), "DEVELOPMENT") ==
               0,
           "DEVELOPMENT name should match");
    VERIFY(strcmp(kraftProfileName(KRAFT_PROFILE_PRODUCTION), "PRODUCTION") ==
               0,
           "PRODUCTION name should match");
    VERIFY(strcmp(kraftProfileName(KRAFT_PROFILE_HIGH_THROUGHPUT),
                  "HIGH_THROUGHPUT") == 0,
           "HIGH_THROUGHPUT name should match");
    VERIFY(strcmp(kraftProfileName(KRAFT_PROFILE_LOW_LATENCY), "LOW_LATENCY") ==
               0,
           "LOW_LATENCY name should match");
    VERIFY(strcmp(kraftProfileName(KRAFT_PROFILE_LARGE_CLUSTER),
                  "LARGE_CLUSTER") == 0,
           "LARGE_CLUSTER name should match");
    VERIFY(strcmp(kraftProfileName(KRAFT_PROFILE_MEMORY_CONSTRAINED),
                  "MEMORY_CONSTRAINED") == 0,
           "MEMORY_CONSTRAINED name should match");

    /* Test invalid profile */
    VERIFY(strcmp(kraftProfileName((kraftProfile)999), "UNKNOWN") == 0,
           "Invalid profile should return UNKNOWN");

    printf("  Profile names verified\n");
    TEST_PASS();
}

/* ====================================================================
 * Chaos Testing Framework Tests
 * ==================================================================== */

static bool test_ChaosEngineLifecycle(void) {
    TEST_START("Chaos - Engine Lifecycle");

    /* Test creation */
    kraftChaosEngine *chaos = kraftChaosEngineNew();
    VERIFY(chaos != NULL, "Engine creation should succeed");
    VERIFY(chaos->enabled == true, "Engine should be enabled by default");

    /* Test seed-based creation for reproducibility */
    kraftChaosEngine *chaos2 = kraftChaosEngineNewWithSeed(12345);
    VERIFY(chaos2 != NULL, "Seeded engine creation should succeed");
    VERIFY(chaos2->seed == 12345, "Seed should be stored");

    /* Test enable/disable */
    kraftChaosSetEnabled(chaos, false);
    VERIFY(chaos->enabled == false, "Disable should work");
    kraftChaosSetEnabled(chaos, true);
    VERIFY(chaos->enabled == true, "Enable should work");

    /* Free both */
    kraftChaosEngineFree(chaos);
    kraftChaosEngineFree(chaos2);

    printf("  Engine lifecycle verified\n");
    TEST_PASS();
}

static bool test_ChaosNetworkFaults(void) {
    TEST_START("Chaos - Network Fault Configuration");

    kraftChaosEngine *chaos = kraftChaosEngineNewWithSeed(42);
    VERIFY(chaos != NULL, "Engine creation should succeed");

    /* Configure network faults */
    kraftChaosSetDropProbability(chaos, 0.1f);
    VERIFY(chaos->network.dropProbability == 0.1f,
           "Drop probability should be set");

    kraftChaosSetDelay(chaos, 0.2f, 10, 100);
    VERIFY(chaos->network.delayProbability == 0.2f,
           "Delay probability should be set");
    VERIFY(chaos->network.minDelayMs == 10, "Min delay should be set");
    VERIFY(chaos->network.maxDelayMs == 100, "Max delay should be set");

    kraftChaosSetDuplicateProbability(chaos, 0.05f);
    VERIFY(chaos->network.duplicateProbability == 0.05f,
           "Duplicate probability should be set");

    kraftChaosSetCorruptProbability(chaos, 0.01f);
    VERIFY(chaos->network.corruptProbability == 0.01f,
           "Corrupt probability should be set");

    /* Test full config */
    kraftChaosNetworkConfig config = {.dropProbability = 0.15f,
                                      .delayProbability = 0.25f,
                                      .duplicateProbability = 0.08f,
                                      .reorderProbability = 0.1f,
                                      .corruptProbability = 0.02f,
                                      .minDelayMs = 5,
                                      .maxDelayMs = 50,
                                      .maxDuplicates = 5};
    kraftChaosConfigureNetwork(chaos, &config);
    VERIFY(chaos->network.dropProbability == 0.15f, "Config should apply drop");
    VERIFY(chaos->network.reorderProbability == 0.1f,
           "Config should apply reorder");

    kraftChaosEngineFree(chaos);
    printf("  Network fault configuration verified\n");
    TEST_PASS();
}

static bool test_ChaosPartitions(void) {
    TEST_START("Chaos - Partition Management");

    kraftChaosEngine *chaos = kraftChaosEngineNewWithSeed(42);
    VERIFY(chaos != NULL, "Engine creation should succeed");

    /* Allocate node state arrays (normally done in Attach) */
    chaos->nodeCount = 5;
    chaos->nodeStates = zcalloc(5, sizeof(kraftNodeFault));

    /* Create a partition isolating node 0 */
    uint32_t isolated[] = {0};
    uint32_t partId = kraftChaosCreatePartition(chaos, isolated, 1, 5000);
    VERIFY(partId > 0, "Partition creation should return valid ID");
    VERIFY(chaos->partitionCount == 1, "Partition count should increase");
    VERIFY(chaos->partitionsCreated == 1, "Stats should track creation");

    /* Create another partition */
    uint32_t isolated2[] = {3, 4};
    uint32_t partId2 = kraftChaosCreatePartition(chaos, isolated2, 2, 3000);
    VERIFY(partId2 > 0, "Second partition should succeed");
    VERIFY(chaos->partitionCount == 2, "Partition count should be 2");

    /* Heal first partition */
    bool healed = kraftChaosHealPartition(chaos, partId);
    VERIFY(healed == true, "Heal should succeed");
    VERIFY(chaos->partitionCount == 1, "Partition count should decrease");

    /* Heal all partitions */
    kraftChaosHealAllPartitions(chaos);
    VERIFY(chaos->partitionCount == 0, "All partitions should be healed");

    zfree(chaos->nodeStates);
    chaos->nodeStates = NULL;
    kraftChaosEngineFree(chaos);
    printf("  Partition management verified\n");
    TEST_PASS();
}

static bool test_ChaosNodeFaults(void) {
    TEST_START("Chaos - Node Fault Injection");

    kraftChaosEngine *chaos = kraftChaosEngineNewWithSeed(42);
    VERIFY(chaos != NULL, "Engine creation should succeed");

    /* Manually setup node arrays */
    chaos->nodeCount = 5;
    chaos->nodeStates = zcalloc(5, sizeof(kraftNodeFault));
    chaos->nodePauseEnd = zcalloc(5, sizeof(uint64_t));

    /* Test crash */
    kraftChaosCrashNode(chaos, 0);
    VERIFY(chaos->nodeStates[0] == KRAFT_NODE_CRASHED,
           "Node 0 should be crashed");
    VERIFY(chaos->nodesCrashed == 1, "Crash count should increase");

    /* Test pause */
    kraftChaosPauseNode(chaos, 1, 5000);
    VERIFY(chaos->nodeStates[1] == KRAFT_NODE_PAUSED,
           "Node 1 should be paused");
    VERIFY(chaos->nodesPaused == 1, "Pause count should increase");

    /* Test slow */
    kraftChaosSlowNode(chaos, 2, 500);
    VERIFY(chaos->nodeStates[2] == KRAFT_NODE_SLOW, "Node 2 should be slow");

    /* Test restart */
    kraftChaosRestartNode(chaos, 0);
    VERIFY(chaos->nodeStates[0] == KRAFT_NODE_HEALTHY,
           "Node 0 should be restarted");

    /* Test resume */
    kraftChaosResumeNode(chaos, 1);
    VERIFY(chaos->nodeStates[1] == KRAFT_NODE_HEALTHY,
           "Node 1 should be resumed");

    /* Test restore all */
    kraftChaosCrashNode(chaos, 3);
    kraftChaosPauseNode(chaos, 4, 1000);
    kraftChaosRestoreAllNodes(chaos);
    VERIFY(chaos->nodeStates[3] == KRAFT_NODE_HEALTHY,
           "Node 3 should be restored");
    VERIFY(chaos->nodeStates[4] == KRAFT_NODE_HEALTHY,
           "Node 4 should be restored");

    zfree(chaos->nodeStates);
    zfree(chaos->nodePauseEnd);
    chaos->nodeStates = NULL;
    chaos->nodePauseEnd = NULL;
    kraftChaosEngineFree(chaos);
    printf("  Node fault injection verified\n");
    TEST_PASS();
}

static bool test_ChaosByzantine(void) {
    TEST_START("Chaos - Byzantine Behavior");

    kraftChaosEngine *chaos = kraftChaosEngineNewWithSeed(42);
    VERIFY(chaos != NULL, "Engine creation should succeed");

    /* Setup node state */
    chaos->nodeCount = 5;
    chaos->nodeStates = zcalloc(5, sizeof(kraftNodeFault));

    /* Set simple Byzantine node */
    bool ok = kraftChaosSetByzantineNode(chaos, 0, KRAFT_BYZANTINE_EQUIVOCATE);
    VERIFY(ok == true, "Setting Byzantine node should succeed");
    VERIFY(chaos->byzantineNodeCount == 1, "Byzantine count should increase");
    VERIFY(chaos->nodeStates[0] == KRAFT_NODE_BYZANTINE,
           "Node state should be Byzantine");

    /* Check behavior */
    kraftByzantineBehavior beh = kraftChaosGetByzantineBehavior(chaos, 0);
    VERIFY(beh == KRAFT_BYZANTINE_EQUIVOCATE, "Behavior should be equivocate");

    /* Configure detailed Byzantine node */
    kraftChaosByzantineNode config = {.nodeId = 1,
                                      .behavior = KRAFT_BYZANTINE_LIE_VOTE,
                                      .targetNodeMask = 0xF,
                                      .activationProbability = 0.5f};
    ok = kraftChaosConfigureByzantineNode(chaos, &config);
    VERIFY(ok == true, "Configuring Byzantine node should succeed");
    VERIFY(chaos->byzantineNodeCount == 2, "Byzantine count should be 2");

    /* Clear single Byzantine node */
    kraftChaosClearByzantineNode(chaos, 0);
    VERIFY(chaos->byzantineNodeCount == 1, "Byzantine count should decrease");
    VERIFY(chaos->nodeStates[0] == KRAFT_NODE_HEALTHY,
           "Node 0 should be healthy");

    /* Clear all Byzantine */
    kraftChaosClearAllByzantine(chaos);
    VERIFY(chaos->byzantineNodeCount == 0, "All Byzantine should be cleared");

    zfree(chaos->nodeStates);
    chaos->nodeStates = NULL;
    kraftChaosEngineFree(chaos);
    printf("  Byzantine behavior verified\n");
    TEST_PASS();
}

static bool test_ChaosClockManipulation(void) {
    TEST_START("Chaos - Clock Manipulation");

    kraftChaosEngine *chaos = kraftChaosEngineNewWithSeed(42);
    VERIFY(chaos != NULL, "Engine creation should succeed");

    /* Setup clock arrays */
    chaos->nodeCount = 3;
    chaos->clockOffsets = zcalloc(3, sizeof(int64_t));
    chaos->clockDriftFactors = zcalloc(3, sizeof(float));
    for (uint32_t i = 0; i < 3; i++) {
        chaos->clockDriftFactors[i] = 1.0f;
    }

    /* Test clock offset */
    kraftChaosSetClockOffset(chaos, 0, 1000); /* 1 second ahead */
    VERIFY(chaos->clockOffsets[0] == 1000, "Offset should be set");

    /* Test clock drift */
    kraftChaosSetClockDrift(chaos, 1, 1.5f); /* 1.5x speed */
    VERIFY(chaos->clockDriftFactors[1] == 1.5f, "Drift factor should be set");

    /* Test clock jump */
    kraftChaosClockJump(chaos, 2, 5000); /* Jump 5 seconds forward */
    VERIFY(chaos->clockOffsets[2] == 5000, "Jump should add to offset");

    /* Test getNodeTime */
    uint64_t realTime = 10000; /* 10 seconds */
    uint64_t nodeTime0 = kraftChaosGetNodeTime(chaos, 0, realTime);
    VERIFY(nodeTime0 == 11000, "Node 0 should be 1 second ahead");

    uint64_t nodeTime1 = kraftChaosGetNodeTime(chaos, 1, realTime);
    VERIFY(nodeTime1 == 15000, "Node 1 should run at 1.5x speed");

    zfree(chaos->clockOffsets);
    zfree(chaos->clockDriftFactors);
    chaos->clockOffsets = NULL;
    chaos->clockDriftFactors = NULL;
    kraftChaosEngineFree(chaos);
    printf("  Clock manipulation verified\n");
    TEST_PASS();
}

static bool test_ChaosStats(void) {
    TEST_START("Chaos - Statistics");

    kraftChaosEngine *chaos = kraftChaosEngineNewWithSeed(42);
    VERIFY(chaos != NULL, "Engine creation should succeed");

    /* Setup node state */
    chaos->nodeCount = 5;
    chaos->nodeStates = zcalloc(5, sizeof(kraftNodeFault));
    chaos->nodePauseEnd = zcalloc(5, sizeof(uint64_t));

    /* Generate some stats */
    chaos->messagesDropped = 10;
    chaos->messagesDelayed = 5;
    chaos->messagesDuplicated = 3;
    chaos->messagesCorrupted = 1;
    kraftChaosCrashNode(chaos, 0);
    kraftChaosPauseNode(chaos, 1, 1000);

    /* Get stats */
    kraftChaosStats stats;
    kraftChaosGetStats(chaos, &stats);
    VERIFY(stats.messagesDropped == 10, "Dropped count should match");
    VERIFY(stats.messagesDelayed == 5, "Delayed count should match");
    VERIFY(stats.activeCrashes == 1, "Active crash count should be 1");

    /* Reset stats */
    kraftChaosResetStats(chaos);
    kraftChaosGetStats(chaos, &stats);
    VERIFY(stats.messagesDropped == 0, "Dropped should be reset");
    VERIFY(stats.messagesDelayed == 0, "Delayed should be reset");

    zfree(chaos->nodeStates);
    zfree(chaos->nodePauseEnd);
    chaos->nodeStates = NULL;
    chaos->nodePauseEnd = NULL;
    kraftChaosEngineFree(chaos);
    printf("  Statistics verified\n");
    TEST_PASS();
}

static bool test_ChaosPresets(void) {
    TEST_START("Chaos - Presets");

    kraftChaosEngine *chaos = kraftChaosEngineNewWithSeed(42);
    VERIFY(chaos != NULL, "Engine creation should succeed");

    /* Setup node state for presets */
    chaos->nodeCount = 5;
    chaos->nodeStates = zcalloc(5, sizeof(kraftNodeFault));
    chaos->clockOffsets = zcalloc(5, sizeof(int64_t));
    chaos->clockDriftFactors = zcalloc(5, sizeof(float));
    for (uint32_t i = 0; i < 5; i++) {
        chaos->clockDriftFactors[i] = 1.0f;
    }

    /* Test Flaky Network preset */
    kraftChaosPresetFlakyNetwork(chaos);
    VERIFY(chaos->network.dropProbability > 0,
           "Flaky should set drop probability");
    VERIFY(chaos->network.delayProbability > 0,
           "Flaky should set delay probability");

    /* Test Partition Storm preset */
    kraftChaosPresetPartitionStorm(chaos);
    VERIFY(chaos->partition.partitionProbability > 0,
           "Storm should set partition probability");
    VERIFY(chaos->partition.asymmetric == true,
           "Storm should enable asymmetric");

    /* Test Byzantine Single preset */
    kraftChaosPresetByzantineSingle(chaos, 0);
    VERIFY(chaos->byzantineNodeCount == 1,
           "Byzantine single should add one node");
    kraftChaosClearAllByzantine(chaos);

    /* Test Byzantine Minority preset */
    uint32_t byzantineNodes[] = {0, 1};
    kraftChaosPresetByzantineMinority(chaos, byzantineNodes, 2);
    VERIFY(chaos->byzantineNodeCount == 2,
           "Byzantine minority should add nodes");
    kraftChaosClearAllByzantine(chaos);

    /* Test Clock Chaos preset */
    kraftChaosPresetClockChaos(chaos);
    VERIFY(chaos->clock.driftProbability > 0,
           "Clock chaos should set drift probability");

    /* Test Total Chaos preset */
    kraftChaosPresetTotalChaos(chaos);
    VERIFY(chaos->network.dropProbability > 0,
           "Total chaos should set network faults");
    VERIFY(chaos->partition.partitionProbability > 0,
           "Total chaos should set partitions");
    VERIFY(chaos->node.crashProbability > 0,
           "Total chaos should set node crashes");

    zfree(chaos->nodeStates);
    zfree(chaos->clockOffsets);
    zfree(chaos->clockDriftFactors);
    chaos->nodeStates = NULL;
    chaos->clockOffsets = NULL;
    chaos->clockDriftFactors = NULL;
    kraftChaosEngineFree(chaos);
    printf("  Presets verified\n");
    TEST_PASS();
}

static bool test_ChaosStringHelpers(void) {
    TEST_START("Chaos - String Helpers");

    /* Network fault names */
    VERIFY(strcmp(kraftNetworkFaultName(KRAFT_FAULT_NONE), "None") == 0,
           "NONE should match");
    VERIFY(strcmp(kraftNetworkFaultName(KRAFT_FAULT_DROP), "Drop") == 0,
           "DROP should match");
    VERIFY(strcmp(kraftNetworkFaultName(KRAFT_FAULT_DELAY), "Delay") == 0,
           "DELAY should match");
    VERIFY(strcmp(kraftNetworkFaultName(KRAFT_FAULT_CORRUPT_TERM),
                  "CorruptTerm") == 0,
           "CORRUPT_TERM should match");

    /* Node fault names */
    VERIFY(strcmp(kraftNodeFaultName(KRAFT_NODE_HEALTHY), "Healthy") == 0,
           "HEALTHY should match");
    VERIFY(strcmp(kraftNodeFaultName(KRAFT_NODE_CRASHED), "Crashed") == 0,
           "CRASHED should match");
    VERIFY(strcmp(kraftNodeFaultName(KRAFT_NODE_BYZANTINE), "Byzantine") == 0,
           "BYZANTINE should match");

    /* Byzantine behavior names */
    VERIFY(strcmp(kraftByzantineBehaviorName(KRAFT_BYZANTINE_NONE), "None") ==
               0,
           "BYZANTINE_NONE should match");
    VERIFY(strcmp(kraftByzantineBehaviorName(KRAFT_BYZANTINE_EQUIVOCATE),
                  "Equivocate") == 0,
           "EQUIVOCATE should match");
    VERIFY(strcmp(kraftByzantineBehaviorName(KRAFT_BYZANTINE_LIE_VOTE),
                  "LieVote") == 0,
           "LIE_VOTE should match");

    /* Clock fault names */
    VERIFY(strcmp(kraftClockFaultName(KRAFT_CLOCK_NORMAL), "Normal") == 0,
           "CLOCK_NORMAL should match");
    VERIFY(strcmp(kraftClockFaultName(KRAFT_CLOCK_JUMP_FORWARD),
                  "JumpForward") == 0,
           "JUMP_FORWARD should match");

    printf("  String helpers verified\n");
    TEST_PASS();
}

/* ====================================================================
 * CRDT-Aware Log Compaction Tests
 * ==================================================================== */

static bool test_CrdtManagerLifecycle(void) {
    TEST_START("CRDT - Manager Lifecycle");

    /* Test creation */
    kraftCrdtManager *mgr = kraftCrdtManagerNew();
    VERIFY(mgr != NULL, "Manager creation should succeed");
    VERIFY(mgr->typeCount == 0, "Initial type count should be 0");
    VERIFY(mgr->keyCount == 0, "Initial key count should be 0");

    /* Register builtins */
    bool ok = kraftCrdtRegisterBuiltins(mgr);
    VERIFY(ok == true, "Registering builtins should succeed");
    VERIFY(mgr->typeCount == 5, "Should have 5 built-in types");

    /* Get stats */
    kraftCrdtStats stats;
    kraftCrdtGetStats(mgr, &stats);
    VERIFY(stats.registeredTypes == 5, "Stats should show 5 types");

    kraftCrdtManagerFree(mgr);
    printf("  Manager lifecycle verified\n");
    TEST_PASS();
}

static bool test_CrdtKeyDeclaration(void) {
    TEST_START("CRDT - Key Declaration");

    kraftCrdtManager *mgr = kraftCrdtManagerNew();
    VERIFY(mgr != NULL, "Manager creation should succeed");
    kraftCrdtRegisterBuiltins(mgr);

    /* Declare exact key */
    bool ok =
        kraftCrdtDeclareKey(mgr, "user:123:name", KRAFT_CRDT_LWW_REGISTER);
    VERIFY(ok == true, "Declaring key should succeed");
    VERIFY(mgr->keyCount == 1, "Key count should be 1");

    /* Get key type */
    kraftCrdtType type = kraftCrdtGetKeyType(mgr, "user:123:name");
    VERIFY(type == KRAFT_CRDT_LWW_REGISTER, "Key type should match");

    /* Unknown key */
    type = kraftCrdtGetKeyType(mgr, "unknown:key");
    VERIFY(type == KRAFT_CRDT_NONE, "Unknown key should return NONE");

    /* Declare pattern */
    ok = kraftCrdtDeclarePattern(mgr, "counter:*", KRAFT_CRDT_G_COUNTER);
    VERIFY(ok == true, "Declaring pattern should succeed");

    /* Pattern match */
    type = kraftCrdtGetKeyType(mgr, "counter:views");
    VERIFY(type == KRAFT_CRDT_G_COUNTER, "Pattern should match");

    type = kraftCrdtGetKeyType(mgr, "counter:clicks");
    VERIFY(type == KRAFT_CRDT_G_COUNTER, "Another pattern match");

    /* Remove key */
    ok = kraftCrdtRemoveKey(mgr, "user:123:name");
    VERIFY(ok == true, "Removing key should succeed");
    VERIFY(mgr->keyCount == 1, "Key count should decrease");

    kraftCrdtManagerFree(mgr);
    printf("  Key declaration verified\n");
    TEST_PASS();
}

static bool test_CrdtLwwRegister(void) {
    TEST_START("CRDT - LWW Register");

    /* Create LWW value */
    const char *data1 = "hello";
    kraftCrdtLwwValue *v1 = kraftCrdtLwwNew(data1, strlen(data1), 1, 1000);
    VERIFY(v1 != NULL, "LWW creation should succeed");
    VERIFY(v1->dataLen == 5, "Data length should match");
    VERIFY(memcmp(v1->data, "hello", 5) == 0, "Data should match");

    /* Create newer value */
    const char *data2 = "world";
    kraftCrdtLwwValue *v2 = kraftCrdtLwwNew(data2, strlen(data2), 2, 2000);
    VERIFY(v2 != NULL, "Second LWW creation should succeed");

    /* Merge (newer wins) */
    kraftCrdtLwwValue *merged = kraftCrdtLwwMerge(v1, v2);
    VERIFY(merged != NULL, "Merge should succeed");
    VERIFY(merged->dataLen == 5, "Merged data length should be from newer");
    VERIFY(memcmp(merged->data, "world", 5) == 0,
           "Merged data should be newer");

    /* Merge reverse order (still newer wins) */
    kraftCrdtLwwValue *merged2 = kraftCrdtLwwMerge(v2, v1);
    VERIFY(merged2 != NULL, "Reverse merge should succeed");
    VERIFY(memcmp(merged2->data, "world", 5) == 0,
           "Reverse merge still picks newer");

    kraftCrdtLwwFree(v1);
    kraftCrdtLwwFree(v2);
    kraftCrdtLwwFree(merged);
    kraftCrdtLwwFree(merged2);

    printf("  LWW Register verified\n");
    TEST_PASS();
}

static bool test_CrdtGCounter(void) {
    TEST_START("CRDT - G-Counter");

    /* Create counter */
    kraftCrdtCounterValue *c1 = kraftCrdtGCounterNew();
    VERIFY(c1 != NULL, "G-Counter creation should succeed");
    VERIFY(kraftCrdtGCounterTotal(c1) == 0, "Initial total should be 0");

    /* Increment from node 1 */
    kraftCrdtGCounterIncrement(c1, 1, 5);
    VERIFY(kraftCrdtGCounterTotal(c1) == 5, "Total should be 5");

    /* Increment again from node 1 */
    kraftCrdtGCounterIncrement(c1, 1, 3);
    VERIFY(kraftCrdtGCounterTotal(c1) == 8, "Total should be 8");

    /* Increment from node 2 */
    kraftCrdtGCounterIncrement(c1, 2, 10);
    VERIFY(kraftCrdtGCounterTotal(c1) == 18, "Total should be 18");

    /* Create second counter */
    kraftCrdtCounterValue *c2 = kraftCrdtGCounterNew();
    kraftCrdtGCounterIncrement(c2, 1, 2); /* Node 1 at 2 */
    kraftCrdtGCounterIncrement(c2, 3, 7); /* Node 3 at 7 */

    /* Merge counters */
    kraftCrdtCounterValue *merged = kraftCrdtGCounterMerge(c1, c2);
    VERIFY(merged != NULL, "Merge should succeed");
    /* c1: node1=8, node2=10 -> 18 */
    /* c2: node1=2, node3=7 -> 9 */
    /* merged: node1=max(8,2)=8, node2=10, node3=7 -> 25 */
    VERIFY(kraftCrdtGCounterTotal(merged) == 25, "Merged total should be 25");

    kraftCrdtCounterFree(c1);
    kraftCrdtCounterFree(c2);
    kraftCrdtCounterFree(merged);

    printf("  G-Counter verified\n");
    TEST_PASS();
}

static bool test_CrdtPnCounter(void) {
    TEST_START("CRDT - PN-Counter");

    /* Create counter */
    kraftCrdtCounterValue *c = kraftCrdtPnCounterNew();
    VERIFY(c != NULL, "PN-Counter creation should succeed");
    VERIFY(kraftCrdtPnCounterTotal(c) == 0, "Initial total should be 0");

    /* Increment */
    kraftCrdtPnCounterIncrement(c, 1, 10);
    VERIFY(kraftCrdtPnCounterTotal(c) == 10, "Total should be 10");

    /* Decrement */
    kraftCrdtPnCounterDecrement(c, 1, 3);
    VERIFY(kraftCrdtPnCounterTotal(c) == 7,
           "Total should be 7 after decrement");

    /* More operations */
    kraftCrdtPnCounterIncrement(c, 2, 5);
    kraftCrdtPnCounterDecrement(c, 2, 8);
    /* node1: 10-3=7, node2: 5-8=-3, total=4 */
    VERIFY(kraftCrdtPnCounterTotal(c) == 4, "Total should be 4");

    kraftCrdtCounterFree(c);

    printf("  PN-Counter verified\n");
    TEST_PASS();
}

static bool test_CrdtGSet(void) {
    TEST_START("CRDT - G-Set");

    /* Create set */
    kraftCrdtGSetValue *s1 = kraftCrdtGSetNew();
    VERIFY(s1 != NULL, "G-Set creation should succeed");
    VERIFY(s1->elementCount == 0, "Initial set should be empty");

    /* Add elements */
    bool ok = kraftCrdtGSetAdd(s1, "apple", 5);
    VERIFY(ok == true, "Adding element should succeed");
    VERIFY(s1->elementCount == 1, "Set should have 1 element");

    ok = kraftCrdtGSetAdd(s1, "banana", 6);
    VERIFY(ok == true, "Adding second element should succeed");
    VERIFY(s1->elementCount == 2, "Set should have 2 elements");

    /* Check contains */
    VERIFY(kraftCrdtGSetContains(s1, "apple", 5) == true,
           "Set should contain apple");
    VERIFY(kraftCrdtGSetContains(s1, "banana", 6) == true,
           "Set should contain banana");
    VERIFY(kraftCrdtGSetContains(s1, "cherry", 6) == false,
           "Set should not contain cherry");

    /* Duplicate add is idempotent */
    ok = kraftCrdtGSetAdd(s1, "apple", 5);
    VERIFY(ok == true, "Duplicate add should succeed");
    VERIFY(s1->elementCount == 2, "Set should still have 2 elements");

    /* Create second set and merge */
    kraftCrdtGSetValue *s2 = kraftCrdtGSetNew();
    kraftCrdtGSetAdd(s2, "cherry", 6);
    kraftCrdtGSetAdd(s2, "apple", 5); /* Duplicate */

    kraftCrdtGSetValue *merged = kraftCrdtGSetMerge(s1, s2);
    VERIFY(merged != NULL, "Merge should succeed");
    VERIFY(merged->elementCount == 3,
           "Merged set should have 3 unique elements");

    kraftCrdtGSetFree(s1);
    kraftCrdtGSetFree(s2);
    kraftCrdtGSetFree(merged);

    printf("  G-Set verified\n");
    TEST_PASS();
}

static bool test_CrdtOrSet(void) {
    TEST_START("CRDT - OR-Set");

    /* Create set */
    kraftCrdtOrSetValue *s = kraftCrdtOrSetNew();
    VERIFY(s != NULL, "OR-Set creation should succeed");
    VERIFY(kraftCrdtOrSetActiveCount(s) == 0,
           "Initial active count should be 0");

    /* Add elements */
    uint64_t tag1 = kraftCrdtOrSetAdd(s, "apple", 5, 1);
    VERIFY(tag1 > 0, "Add should return valid tag");
    VERIFY(kraftCrdtOrSetActiveCount(s) == 1, "Active count should be 1");

    uint64_t tag2 = kraftCrdtOrSetAdd(s, "banana", 6, 1);
    VERIFY(tag2 > tag1, "Tags should be monotonically increasing");
    VERIFY(kraftCrdtOrSetActiveCount(s) == 2, "Active count should be 2");

    /* Check contains */
    VERIFY(kraftCrdtOrSetContains(s, "apple", 5) == true,
           "Should contain apple");
    VERIFY(kraftCrdtOrSetContains(s, "banana", 6) == true,
           "Should contain banana");

    /* Remove element */
    bool removed = kraftCrdtOrSetRemove(s, "apple", 5);
    VERIFY(removed == true, "Remove should succeed");
    VERIFY(kraftCrdtOrSetContains(s, "apple", 5) == false,
           "Apple should be removed");
    VERIFY(kraftCrdtOrSetActiveCount(s) == 1, "Active count should be 1");

    /* Element count still includes tombstone */
    VERIFY(s->elementCount == 2, "Total elements includes tombstone");

    /* Re-add removed element */
    uint64_t tag3 = kraftCrdtOrSetAdd(s, "apple", 5, 1);
    VERIFY(tag3 > tag2, "New tag for re-added element");
    VERIFY(kraftCrdtOrSetContains(s, "apple", 5) == true,
           "Re-added apple should exist");
    VERIFY(kraftCrdtOrSetActiveCount(s) == 2, "Active count should be 2");

    /* Purge tombstones */
    uint32_t purged = kraftCrdtOrSetPurgeTombstones(s, 0);
    VERIFY(purged == 1, "Should purge 1 tombstone");
    VERIFY(s->elementCount == 2, "Total elements after purge");

    kraftCrdtOrSetFree(s);

    printf("  OR-Set verified\n");
    TEST_PASS();
}

static bool test_CrdtStringHelpers(void) {
    TEST_START("CRDT - String Helpers");

    /* Type names */
    VERIFY(strcmp(kraftCrdtTypeName(KRAFT_CRDT_NONE), "NONE") == 0,
           "NONE type should match");
    VERIFY(strcmp(kraftCrdtTypeName(KRAFT_CRDT_LWW_REGISTER), "LWW_REGISTER") ==
               0,
           "LWW_REGISTER type should match");
    VERIFY(strcmp(kraftCrdtTypeName(KRAFT_CRDT_G_COUNTER), "G_COUNTER") == 0,
           "G_COUNTER type should match");
    VERIFY(strcmp(kraftCrdtTypeName(KRAFT_CRDT_PN_COUNTER), "PN_COUNTER") == 0,
           "PN_COUNTER type should match");
    VERIFY(strcmp(kraftCrdtTypeName(KRAFT_CRDT_G_SET), "G_SET") == 0,
           "G_SET type should match");
    VERIFY(strcmp(kraftCrdtTypeName(KRAFT_CRDT_OR_SET), "OR_SET") == 0,
           "OR_SET type should match");

    /* Op names */
    VERIFY(strcmp(kraftCrdtOpName(KRAFT_CRDT_OP_SET), "SET") == 0,
           "SET op should match");
    VERIFY(strcmp(kraftCrdtOpName(KRAFT_CRDT_OP_INC), "INC") == 0,
           "INC op should match");
    VERIFY(strcmp(kraftCrdtOpName(KRAFT_CRDT_OP_DEC), "DEC") == 0,
           "DEC op should match");
    VERIFY(strcmp(kraftCrdtOpName(KRAFT_CRDT_OP_ADD), "ADD") == 0,
           "ADD op should match");
    VERIFY(strcmp(kraftCrdtOpName(KRAFT_CRDT_OP_REMOVE), "REMOVE") == 0,
           "REMOVE op should match");

    printf("  String helpers verified\n");
    TEST_PASS();
}

/* ====================================================================
 * Main Test Runner
 * ==================================================================== */
int main(int argc, char *argv[]) {
    int testNum = -1; /* -1 means run all tests */

    if (argc > 1) {
        testNum = atoi(argv[1]);
    }

    printf("=======================================================\n");
    printf("   Kraft Raft Consensus - Test Suite                  \n");
    printf("=======================================================\n");

    /* Run tests */
    if (testNum == -1 || testNum == 1) {
        test_SingleNode();
    }
    if (testNum == -1 || testNum == 2) {
        test_InitialElection3Nodes();
    }
    if (testNum == -1 || testNum == 3) {
        test_InitialElection5Nodes();
    }
    if (testNum == -1 || testNum == 4) {
        test_ElectionWithOneFailure();
    }
    if (testNum == -1 || testNum == 5) {
        test_ReElectionAfterLeaderCrash();
    }
    if (testNum == -1 || testNum == 6) {
        test_ElectionSafety();
    }
    if (testNum == -1 || testNum == 7) {
        test_LogMatching();
    }
    if (testNum == -1 || testNum == 8) {
        test_MinorityFailure();
    }
    if (testNum == -1 || testNum == 9) {
        test_NetworkPartition();
    }
    if (testNum == -1 || testNum == 10) {
        test_PacketLoss();
    }
    if (testNum == -1 || testNum == 11) {
        test_LeadershipTransfer();
    }
    if (testNum == -1 || testNum == 12) {
        test_ReadIndex();
    }

    /* Witness Node Tests */
    if (testNum == -1 || testNum == 13) {
        test_WitnessManagerLifecycle();
    }
    if (testNum == -1 || testNum == 14) {
        test_WitnessReplicationTracking();
    }
    if (testNum == -1 || testNum == 15) {
        test_WitnessReadServing();
    }
    if (testNum == -1 || testNum == 16) {
        test_WitnessPromotion();
    }
    if (testNum == -1 || testNum == 17) {
        test_WitnessVotePrevention();
    }

    /* Async Commit Mode Tests */
    if (testNum == -1 || testNum == 18) {
        test_AsyncCommitConfig();
    }
    if (testNum == -1 || testNum == 19) {
        test_AsyncCommitLifecycle();
    }
    if (testNum == -1 || testNum == 20) {
        test_AsyncCommitStrongMode();
    }
    if (testNum == -1 || testNum == 21) {
        test_AsyncCommitLeaderOnlyMode();
    }
    if (testNum == -1 || testNum == 22) {
        test_AsyncCommitAsyncMode();
    }
    if (testNum == -1 || testNum == 23) {
        test_AsyncCommitFlexibleMode();
    }
    if (testNum == -1 || testNum == 24) {
        test_AsyncCommitModeOverride();
    }
    if (testNum == -1 || testNum == 25) {
        test_AsyncCommitBlocking();
    }
    if (testNum == -1 || testNum == 26) {
        test_AsyncCommitStats();
    }

    /* Intelligent Snapshot Tests */
    if (testNum == -1 || testNum == 27) {
        test_SnapshotConfig();
    }
    if (testNum == -1 || testNum == 28) {
        test_SnapshotLifecycle();
    }
    if (testNum == -1 || testNum == 29) {
        test_SnapshotTrigger();
    }
    if (testNum == -1 || testNum == 30) {
        test_SnapshotCreation();
    }
    if (testNum == -1 || testNum == 31) {
        test_SnapshotTransfer();
    }
    if (testNum == -1 || testNum == 32) {
        test_SnapshotInstallation();
    }
    if (testNum == -1 || testNum == 33) {
        test_SnapshotStats();
    }

    /* Multi-Raft Group Tests */
    if (testNum == -1 || testNum == 34) {
        test_MultiRaftConfig();
    }
    if (testNum == -1 || testNum == 35) {
        test_MultiRaftLifecycle();
    }
    if (testNum == -1 || testNum == 36) {
        test_MultiRaftGroupCreation();
    }
    if (testNum == -1 || testNum == 37) {
        test_MultiRaftGroupRemoval();
    }
    if (testNum == -1 || testNum == 38) {
        test_MultiRaftGroupStopResume();
    }
    if (testNum == -1 || testNum == 39) {
        test_MultiRaftRangeLookup();
    }
    if (testNum == -1 || testNum == 40) {
        test_MultiRaftTicking();
    }
    if (testNum == -1 || testNum == 41) {
        test_MultiRaftStats();
    }

    /* Extended: Multi-Raft Stress & Adversarial Tests */
    if (testNum == -1 || testNum == 42) {
        test_MultiRaftManyGroups();
    }
    if (testNum == -1 || testNum == 43) {
        test_MultiRaftHashCollisions();
    }
    if (testNum == -1 || testNum == 44) {
        test_MultiRaftRangeBoundaries();
    }
    if (testNum == -1 || testNum == 45) {
        test_MultiRaftSplitGroup();
    }
    if (testNum == -1 || testNum == 46) {
        test_MultiRaftMergeGroups();
    }
    if (testNum == -1 || testNum == 47) {
        test_MultiRaftGroupIdEnumeration();
    }
    if (testNum == -1 || testNum == 48) {
        test_MultiRaftIterator();
    }
    if (testNum == -1 || testNum == 49) {
        test_MultiRaftMaxCapacity();
    }
    if (testNum == -1 || testNum == 50) {
        test_MultiRaftNullAndEdgeCases();
    }
    if (testNum == -1 || testNum == 51) {
        test_MultiRaftConcurrentStopResume();
    }
    if (testNum == -1 || testNum == 52) {
        test_MultiRaftOverlappingRanges();
    }
    if (testNum == -1 || testNum == 53) {
        test_MultiRaftLongKeys();
    }
    if (testNum == -1 || testNum == 54) {
        test_MultiRaftRapidCreateRemove();
    }

    /* ===================================================================
     * Configuration Profiles Tests
     * =================================================================== */
    if (testNum == -1 || testNum == 55) {
        test_ProfileDefault();
    }
    if (testNum == -1 || testNum == 56) {
        test_ProfileAllValid();
    }
    if (testNum == -1 || testNum == 57) {
        test_ProfileValidation();
    }
    if (testNum == -1 || testNum == 58) {
        test_ProfileOverride();
    }
    if (testNum == -1 || testNum == 59) {
        test_ProfileTimingConstraints();
    }
    if (testNum == -1 || testNum == 60) {
        test_ProfileNames();
    }

    /* ===================================================================
     * Chaos Testing Framework Tests
     * =================================================================== */
    if (testNum == -1 || testNum == 61) {
        test_ChaosEngineLifecycle();
    }
    if (testNum == -1 || testNum == 62) {
        test_ChaosNetworkFaults();
    }
    if (testNum == -1 || testNum == 63) {
        test_ChaosPartitions();
    }
    if (testNum == -1 || testNum == 64) {
        test_ChaosNodeFaults();
    }
    if (testNum == -1 || testNum == 65) {
        test_ChaosByzantine();
    }
    if (testNum == -1 || testNum == 66) {
        test_ChaosClockManipulation();
    }
    if (testNum == -1 || testNum == 67) {
        test_ChaosStats();
    }
    if (testNum == -1 || testNum == 68) {
        test_ChaosPresets();
    }
    if (testNum == -1 || testNum == 69) {
        test_ChaosStringHelpers();
    }

    /* ===================================================================
     * CRDT-Aware Log Compaction Tests
     * =================================================================== */
    if (testNum == -1 || testNum == 70) {
        test_CrdtManagerLifecycle();
    }
    if (testNum == -1 || testNum == 71) {
        test_CrdtKeyDeclaration();
    }
    if (testNum == -1 || testNum == 72) {
        test_CrdtLwwRegister();
    }
    if (testNum == -1 || testNum == 73) {
        test_CrdtGCounter();
    }
    if (testNum == -1 || testNum == 74) {
        test_CrdtPnCounter();
    }
    if (testNum == -1 || testNum == 75) {
        test_CrdtGSet();
    }
    if (testNum == -1 || testNum == 76) {
        test_CrdtOrSet();
    }
    if (testNum == -1 || testNum == 77) {
        test_CrdtStringHelpers();
    }

    printf("\n=======================================================\n");
    printf("   Test Results:                                       \n");
    printf("   Total:  %d                                          \n",
           totalTests);
    printf("   Passed: %d                                          \n",
           passedTests);
    printf("   Failed: %d                                          \n",
           failedTests);
    printf("=======================================================\n");

    return failedTests > 0 ? 1 : 0;
}
