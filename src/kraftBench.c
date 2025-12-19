/* ====================================================================
 * Kraft Raft Implementation - Performance Benchmarks
 *
 * Comprehensive performance testing suite for measuring Raft protocol
 * performance under various conditions.
 * ==================================================================== */

#include "ctest.h"
#include "kraftTest.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ====================================================================
 * Benchmark: Throughput - How many operations can we commit per second?
 * ==================================================================== */
static void benchThroughput(uint32_t nodeCount, uint32_t numCommands,
                            const char *scenario) {
    printf("\n");
    printf("==================================================================="
           "=\n");
    printf("Throughput Benchmark: %s\n", scenario);
    printf("Cluster Size: %u nodes, Commands: %u\n", nodeCount, numCommands);
    printf("==================================================================="
           "=\n");

    testSeed(12345);
    testCluster *cluster = testClusterNew(nodeCount);
    testPerfEnable(cluster);

    testClusterStart(cluster);
    testClusterRunUntilLeaderElected(cluster, 5000000UL);

    uint32_t leader = testClusterGetLeader(cluster);
    if (leader == UINT32_MAX) {
        printf("ERROR: No leader elected\n");
        testClusterFree(cluster);
        return;
    }

    /* Start profiling */
    testPerfReset(cluster);
    uint64_t startTime = cluster->currentTime;

    /* Submit commands as fast as possible */
    for (uint32_t i = 0; i < numCommands; i++) {
        uint8_t data[64];
        snprintf((char *)data, sizeof(data), "command-%u", i);

        uint64_t submitStart = cluster->currentTime;

        if (!testClusterSubmitCommand(cluster, data, strlen((char *)data))) {
            /* Leader might have changed, find new leader */
            testClusterRunUntilLeaderElected(cluster, 1000000UL);
            leader = testClusterGetLeader(cluster);
            if (leader != UINT32_MAX) {
                testClusterSubmitCommand(cluster, data, strlen((char *)data));
            }
        }

        /* Run cluster to process messages */
        testClusterTick(cluster, 1000); /* 1ms tick */

        /* Track commit latency when command is committed */
        kraftState *leaderState = cluster->nodes[leader].state;
        if (leaderState->commitIndex >= i + 1) {
            uint64_t commitLatency = cluster->currentTime - submitStart;
            testPerfRecordCommitLatency(cluster, commitLatency);
        }
    }

    /* Wait for all commands to commit */
    uint64_t maxWait = 30000000UL; /* 30 seconds */
    uint64_t waitStart = cluster->currentTime;

    while (cluster->currentTime - waitStart < maxWait) {
        leader = testClusterGetLeader(cluster);
        if (leader != UINT32_MAX) {
            kraftState *leaderState = cluster->nodes[leader].state;
            if (leaderState->commitIndex >= numCommands) {
                break;
            }
        }
        testClusterTick(cluster, 10000); /* 10ms ticks */
    }

    uint64_t endTime = cluster->currentTime;
    testPerfDisable(cluster);

    /* Calculate actual throughput */
    double durationSec = (endTime - startTime) / 1000000.0;
    double throughput = numCommands / durationSec;

    printf("\nResults:\n");
    printf("  Duration:          %.3f seconds\n", durationSec);
    printf("  Commands Committed:%u\n", numCommands);
    printf("  Throughput:        %.2f commands/sec\n", throughput);

    testPerfPrintReport(cluster);
    testClusterFree(cluster);
}

/* ====================================================================
 * Benchmark: Latency - How quickly can we commit a single operation?
 * ==================================================================== */
static void benchLatency(uint32_t nodeCount, uint32_t numSamples) {
    printf("\n");
    printf("==================================================================="
           "=\n");
    printf("Latency Benchmark: Single Command Commit Latency\n");
    printf("Cluster Size: %u nodes, Samples: %u\n", nodeCount, numSamples);
    printf("==================================================================="
           "=\n");

    testSeed(23456);
    testCluster *cluster = testClusterNew(nodeCount);
    testPerfEnable(cluster);

    testClusterStart(cluster);
    testClusterRunUntilLeaderElected(cluster, 5000000UL);

    uint32_t leader = testClusterGetLeader(cluster);
    if (leader == UINT32_MAX) {
        printf("ERROR: No leader elected\n");
        testClusterFree(cluster);
        return;
    }

    testPerfReset(cluster);

    /* Measure commit latency for individual commands */
    for (uint32_t i = 0; i < numSamples; i++) {
        uint8_t data[64];
        snprintf((char *)data, sizeof(data), "latency-test-%u", i);

        kraftState *leaderState = cluster->nodes[leader].state;
        kraftRpcUniqueIndex beforeIndex = leaderState->commitIndex;
        uint64_t submitTime = cluster->currentTime;

        testClusterSubmitCommand(cluster, data, strlen((char *)data));

        /* Run until committed */
        uint64_t timeout = 5000000UL; /* 5 seconds */
        uint64_t startWait = cluster->currentTime;

        while (cluster->currentTime - startWait < timeout) {
            leader = testClusterGetLeader(cluster);
            if (leader != UINT32_MAX) {
                leaderState = cluster->nodes[leader].state;
                if (leaderState->commitIndex > beforeIndex) {
                    break;
                }
            }
            testClusterTick(cluster, 1000); /* 1ms tick */
        }

        uint64_t commitLatency = cluster->currentTime - submitTime;
        testPerfRecordCommitLatency(cluster, commitLatency);

        /* Small delay between samples to avoid overwhelming the system */
        testClusterTick(cluster, 10000); /* 10ms */
    }

    testPerfDisable(cluster);
    testPerfPrintReport(cluster);
    testClusterFree(cluster);
}

/* ====================================================================
 * Benchmark: Election Speed - How quickly can we elect a new leader?
 * ==================================================================== */
static void benchElection(uint32_t nodeCount, uint32_t numElections) {
    printf("\n");
    printf("==================================================================="
           "=\n");
    printf("Election Benchmark: Leader Election Speed\n");
    printf("Cluster Size: %u nodes, Elections: %u\n", nodeCount, numElections);
    printf("==================================================================="
           "=\n");

    testSeed(34567);
    testCluster *cluster = testClusterNew(nodeCount);
    testPerfEnable(cluster);

    testClusterStart(cluster);

    for (uint32_t i = 0; i < numElections; i++) {
        uint64_t electionStart = cluster->currentTime;

        /* If there's a leader, crash it to trigger election */
        uint32_t oldLeader = testClusterGetLeader(cluster);
        if (oldLeader != UINT32_MAX) {
            testNodeCrash(cluster, oldLeader);
        }

        /* Wait for new leader election */
        testClusterRunUntilLeaderElected(cluster, 5000000UL);

        uint32_t newLeader = testClusterGetLeader(cluster);
        if (newLeader == UINT32_MAX) {
            printf("  Election %u: FAILED\n", i + 1);
            continue;
        }

        uint64_t electionLatency = cluster->currentTime - electionStart;
        testPerfRecordElectionLatency(cluster, electionLatency);

        printf("  Election %u: %.3f ms (Node %u -> Node %u)\n", i + 1,
               electionLatency / 1000.0, oldLeader, newLeader);

        /* Recover crashed node for next iteration (if not last iteration) */
        if (i < numElections - 1 && oldLeader != UINT32_MAX) {
            testNodeRecover(cluster, oldLeader);
            testClusterTick(cluster, 100000); /* 100ms for recovery */
        }
    }

    testPerfDisable(cluster);
    testPerfPrintReport(cluster);
    testClusterFree(cluster);
}

/* ====================================================================
 * Benchmark: Scalability - Performance vs Cluster Size
 * ==================================================================== */
static void benchScalability(void) {
    printf("\n");
    printf("==================================================================="
           "=\n");
    printf("Scalability Benchmark: Performance vs Cluster Size\n");
    printf("==================================================================="
           "=\n");

    uint32_t clusterSizes[] = {3, 5, 7, 9, 11};
    uint32_t numCommands = 100;

    printf("\n%-12s %-20s %-20s %-20s\n", "Cluster Size", "Throughput (cmd/s)",
           "Avg Commit (us)", "Avg Election (us)");
    printf("------------ -------------------- -------------------- "
           "--------------------\n");

    for (size_t i = 0; i < sizeof(clusterSizes) / sizeof(clusterSizes[0]);
         i++) {
        uint32_t nodeCount = clusterSizes[i];

        testSeed(45678 + i);
        testCluster *cluster = testClusterNew(nodeCount);
        testPerfEnable(cluster);

        testClusterStart(cluster);
        testClusterRunUntilLeaderElected(cluster, 5000000UL);

        /* Measure election time */
        uint64_t electionTime = cluster->currentTime;
        testPerfRecordElectionLatency(cluster, electionTime);

        /* Submit commands and measure throughput */
        testPerfReset(cluster);
        uint64_t startTime = cluster->currentTime;

        for (uint32_t j = 0; j < numCommands; j++) {
            uint8_t data[64];
            snprintf((char *)data, sizeof(data), "cmd-%u", j);

            uint64_t submitStart = cluster->currentTime;
            testClusterSubmitCommand(cluster, data, strlen((char *)data));
            testClusterTick(cluster, 1000);

            uint32_t leader = testClusterGetLeader(cluster);
            if (leader != UINT32_MAX) {
                kraftState *leaderState = cluster->nodes[leader].state;
                if (leaderState->commitIndex >= j + 1) {
                    testPerfRecordCommitLatency(cluster, cluster->currentTime -
                                                             submitStart);
                }
            }
        }

        /* Wait for commits */
        testClusterRunUntil(
            cluster, (bool (*)(testCluster *))testClusterHasConsistentLeader,
            10000000UL);

        uint64_t endTime = cluster->currentTime;
        testPerfDisable(cluster);

        testPerfStats stats = testPerfGetStats(cluster);
        double throughput = numCommands / ((endTime - startTime) / 1000000.0);

        printf("%-12u %-20.2f %-20" PRIu64 " %-20.2f\n", nodeCount, throughput,
               stats.commitAvgLatencyUs, electionTime / 1000.0);

        testClusterFree(cluster);
    }
}

/* ====================================================================
 * Benchmark: Fault Tolerance - Performance under adverse conditions
 * ==================================================================== */
static void benchFaultTolerance(void) {
    printf("\n");
    printf("==================================================================="
           "=\n");
    printf("Fault Tolerance Benchmark: Performance Under Network Issues\n");
    printf("==================================================================="
           "=\n");

    const char *scenarios[] = {"Reliable Network", "10% Packet Loss",
                               "Network Delay (5-50ms)",
                               "Combined (5% loss + delay)"};

    for (size_t i = 0; i < sizeof(scenarios) / sizeof(scenarios[0]); i++) {
        printf("\n[%s]\n", scenarios[i]);

        testSeed(56789 + i);
        testCluster *cluster = testClusterNew(5);
        testPerfEnable(cluster);

        /* Configure network based on scenario */
        switch (i) {
        case 1: /* Packet loss */
            testNetworkSetMode(cluster, TEST_NETWORK_LOSSY);
            testNetworkSetDropRate(cluster, 0.10);
            break;
        case 2: /* Delay */
            testNetworkSetMode(cluster, TEST_NETWORK_DELAYED);
            testNetworkSetDelay(cluster, 5000, 50000);
            break;
        case 3: /* Combined */
            testNetworkSetMode(cluster, TEST_NETWORK_LOSSY);
            testNetworkSetDropRate(cluster, 0.05);
            testNetworkSetDelay(cluster, 5000, 50000);
            break;
        default: /* Reliable */
            testNetworkSetMode(cluster, TEST_NETWORK_RELIABLE);
            break;
        }

        testClusterStart(cluster);
        testClusterRunUntilLeaderElected(cluster, 10000000UL);

        testPerfReset(cluster);
        uint32_t numCommands = 50;

        for (uint32_t j = 0; j < numCommands; j++) {
            uint8_t data[64];
            snprintf((char *)data, sizeof(data), "cmd-%u", j);
            testClusterSubmitCommand(cluster, data, strlen((char *)data));
            testClusterTick(cluster, 1000);
        }

        /* Wait for all commits */
        testClusterRunUntil(
            cluster, (bool (*)(testCluster *))testClusterHasConsistentLeader,
            30000000UL);

        testPerfDisable(cluster);

        testPerfStats stats = testPerfGetStats(cluster);
        printf("  Throughput:    %" PRIu64 " commands/sec\n",
               stats.commitThroughput);
        printf("  Commit Latency:%" PRIu64 " us (avg), %" PRIu64 " us (p95)\n",
               stats.commitAvgLatencyUs, stats.commitP95LatencyUs);
        printf("  Messages Lost: %" PRIu64 " / %" PRIu64 " (%.1f%%)\n",
               cluster->network.totalDropped, cluster->network.totalSent,
               (cluster->network.totalSent > 0
                    ? (100.0 * cluster->network.totalDropped /
                       cluster->network.totalSent)
                    : 0.0));

        testClusterFree(cluster);
    }
}

/* ====================================================================
 * Main Benchmark Runner
 * ==================================================================== */
int main(int argc, char *argv[]) {
    printf("\n");
    printf("==================================================================="
           "=\n");
    printf("Kraft Raft Implementation - Performance Benchmarks\n");
    printf("==================================================================="
           "=\n");

    if (argc > 1) {
        /* Run specific benchmark */
        const char *bench = argv[1];

        if (strcmp(bench, "throughput") == 0) {
            benchThroughput(5, 1000, "5-node cluster, 1000 commands");
        } else if (strcmp(bench, "latency") == 0) {
            benchLatency(5, 100);
        } else if (strcmp(bench, "election") == 0) {
            benchElection(5, 10);
        } else if (strcmp(bench, "scalability") == 0) {
            benchScalability();
        } else if (strcmp(bench, "fault-tolerance") == 0) {
            benchFaultTolerance();
        } else {
            printf("Unknown benchmark: %s\n", bench);
            printf("Available benchmarks:\n");
            printf("  throughput      - Measure command throughput\n");
            printf("  latency         - Measure commit latency\n");
            printf("  election        - Measure election speed\n");
            printf("  scalability     - Performance vs cluster size\n");
            printf(
                "  fault-tolerance - Performance under adverse conditions\n");
            return 1;
        }
    } else {
        /* Run all benchmarks */
        benchThroughput(3, 500, "3-node cluster, 500 commands");
        benchThroughput(5, 500, "5-node cluster, 500 commands");
        benchLatency(5, 100);
        benchElection(5, 10);
        benchScalability();
        benchFaultTolerance();
    }

    printf("\n");
    printf("==================================================================="
           "=\n");
    printf("Benchmarks Complete\n");
    printf("==================================================================="
           "=\n");
    printf("\n");

    return 0;
}

/* vi:ai et sw=4 ts=4: */
