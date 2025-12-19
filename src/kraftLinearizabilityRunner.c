/* kraftLinearizabilityRunner.c - CLI Runner for Linearizability Testing
 *
 * Main entry point for kraft-linearizability tool.
 */

#ifndef KRAFT_TEST
#define KRAFT_TEST
#endif

#include "kraftLinearizability.h"
#include "kraftLinearizabilityCluster.h"
#include "kraftLinearizabilityFailure.h"
#include "kraftLinearizabilityWorkload.h"

#include <getopt.h>
#include <inttypes.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

/* ============================================================================
 * Test Configuration
 * ============================================================================
 */

typedef struct kraftLinTestConfig {
    /* Cluster */
    uint32_t nodeCount;
    uint32_t basePort;

    /* Clients */
    uint32_t clientCount;

    /* Workload */
    kraftLinWorkloadConfig workload;

    /* Failure */
    kraftLinFailureSpec failure;

    /* Test control */
    uint64_t maxTimeUs;
    uint32_t seed;
    bool verbose;

    /* Output */
    const char *jsonFile;
    const char *historyFile;

} kraftLinTestConfig;

/* ============================================================================
 * Presets
 * ============================================================================
 */

static kraftLinTestConfig presetQuick(void) {
    kraftLinTestConfig config = {
        .nodeCount = 3,
        .clientCount = 5,
        .basePort = 45000,
        .workload = kraftLinWorkloadConfigDefault(),
        .failure = {.type = KRAFT_LIN_FAILURE_NONE},
        .maxTimeUs = 30 * 1000000UL, /* 30 seconds */
        .seed = (uint32_t)time(NULL),
        .verbose = false,
        .jsonFile = NULL,
        .historyFile = NULL,
    };
    config.workload.totalOps = 1000;
    return config;
}

static kraftLinTestConfig presetStandard(void) {
    kraftLinTestConfig config = {
        .nodeCount = 5,
        .clientCount = 10,
        .basePort = 45000,
        .workload = kraftLinWorkloadConfigDefault(),
        .failure =
            {
                .type = KRAFT_LIN_FAILURE_CRASH_LEADER,
                .triggerAtUs = 0, /* Will be set to 50% of test time */
                .durationUs = 10 * 1000000UL,
            },
        .maxTimeUs = 5 * 60 * 1000000UL, /* 5 minutes */
        .seed = (uint32_t)time(NULL),
        .verbose = false,
    };
    config.workload.totalOps = 10000;
    return config;
}

static kraftLinTestConfig presetCI(void) {
    kraftLinTestConfig config = {
        .nodeCount = 5,
        .clientCount = 10,
        .basePort = 45000,
        .workload = kraftLinWorkloadConfigDefault(),
        .failure =
            {
                .type = KRAFT_LIN_FAILURE_PARTITION_LEADER,
                .triggerAtUs = 0, /* 30% of test time */
                .durationUs = 15 * 1000000UL,
            },
        .maxTimeUs = 2 * 60 * 1000000UL, /* 2 minutes */
        .seed = (uint32_t)time(NULL),
        .verbose = false,
    };
    config.workload.totalOps = 5000;
    return config;
}

/* ============================================================================
 * Usage
 * ============================================================================
 */

static void printUsage(const char *prog) {
    printf("Usage: %s [OPTIONS]\n", prog);
    printf("\nKraft Linearizability Testing - Verify Raft correctness under "
           "failures\n");
    printf("\nCluster Options:\n");
    printf("  -n, --nodes N         Number of nodes (3-100, default: 5)\n");
    printf(
        "  -c, --clients M       Concurrent clients (1-1000, default: 10)\n");
    printf("  -o, --ops N           Total operations (default: 10000)\n");
    printf("  -t, --time TIME       Max test duration (e.g., 30s, 5m, 1h)\n");
    printf("  --data-dir DIR        Data directory (default: "
           "./kraft-lin-test-PID)\n");
    printf("\nWorkload Options:\n");
    printf("  --read-pct N          Read percentage (0-100, default: 50)\n");
    printf("  --write-pct N         Write percentage (0-100, default: 40)\n");
    printf(
        "  --incr-pct N          Increment percentage (0-100, default: 10)\n");
    printf("  --keys N              Key space size (default: 10)\n");
    printf("\nFailure Options:\n");
    printf("  --failure TYPE        Failure scenario:\n");
    printf("                          none, crash-leader, crash-follower,\n");
    printf("                          partition-leader, partition-minority\n");
    printf("  --failure-at TIME     When to inject (e.g., 30s, 50%%)\n");
    printf("  --failure-for TIME    Failure duration (default: 10s)\n");
    printf("\nOutput Options:\n");
    printf("  -v, --verbose         Verbose output\n");
    printf("  -s, --seed N          Random seed\n");
    printf("  --json FILE           Write JSON results to file\n");
    printf("  --history FILE        Save operation history\n");
    printf("\nPresets:\n");
    printf("  --preset quick        3 nodes, 5 clients, 1000 ops, no "
           "failures (30s)\n");
    printf("  --preset standard     5 nodes, 10 clients, 10000 ops, "
           "crash-leader (5m)\n");
    printf("  --preset ci           5 nodes, 10 clients, 5000 ops, partition "
           "(2m)\n");
    printf("\nExamples:\n");
    printf("  %s --preset quick\n", prog);
    printf("  %s -n 7 -c 20 -o 50000 --failure crash-leader\n", prog);
    printf("  %s -n 5 -c 10 --failure partition-leader --failure-at 60s\n",
           prog);
}

/* ============================================================================
 * Argument Parsing
 * ============================================================================
 */

static kraftLinFailureType parseFailureType(const char *str) {
    if (strcmp(str, "none") == 0) {
        return KRAFT_LIN_FAILURE_NONE;
    }
    if (strcmp(str, "crash-leader") == 0) {
        return KRAFT_LIN_FAILURE_CRASH_LEADER;
    }
    if (strcmp(str, "crash-follower") == 0) {
        return KRAFT_LIN_FAILURE_CRASH_FOLLOWER;
    }
    if (strcmp(str, "partition-leader") == 0) {
        return KRAFT_LIN_FAILURE_PARTITION_LEADER;
    }
    if (strcmp(str, "partition-minority") == 0) {
        return KRAFT_LIN_FAILURE_PARTITION_MINORITY;
    }
    return KRAFT_LIN_FAILURE_NONE;
}

static uint64_t parseTimeSpec(const char *str) {
    char *end;
    uint64_t val = strtoull(str, &end, 10);

    if (*end == 's' || *end == 'S') {
        return val * 1000000UL;
    } else if (*end == 'm' || *end == 'M') {
        return val * 60 * 1000000UL;
    } else if (*end == 'h' || *end == 'H') {
        return val * 3600 * 1000000UL;
    } else if (*end == '%') {
        /* Percentage - will be calculated later */
        return val;
    }

    return val * 1000000UL; /* Default: seconds */
}

/* ============================================================================
 * Main Test Execution
 * ============================================================================
 */

static volatile bool g_interrupted = false;
static kraftLinCluster *g_cluster = NULL;
static kraftLinClientPool *g_clients = NULL;

static void signalHandler(int sig) {
    (void)sig;
    printf("\nInterrupted (Ctrl-C), cleaning up...\n");
    g_interrupted = true;

    /* Immediately stop clients and cluster */
    if (g_clients) {
        kraftLinClientPoolStop(g_clients);
    }
    if (g_cluster) {
        kraftLinClusterStop(g_cluster);
    }

    exit(1);
}

int main(int argc, char *argv[]) {
    kraftLinTestConfig config;
    memset(&config, 0, sizeof(config));
    const char *preset = NULL;

    /* Track CLI overrides to apply after preset */
    bool hasNodeCount = false, hasClientCount = false, hasOps = false;
    bool hasTime = false, hasVerbose = false;
    uint32_t nodeCountOverride = 0, clientCountOverride = 0;
    uint64_t opsOverride = 0, timeOverride = 0;
    const char *dataDirOverride = NULL;

    /* Option parsing */
    static struct option longOpts[] = {
        {"nodes", required_argument, NULL, 'n'},
        {"clients", required_argument, NULL, 'c'},
        {"ops", required_argument, NULL, 'o'},
        {"time", required_argument, NULL, 't'},
        {"read-pct", required_argument, NULL, 'R'},
        {"write-pct", required_argument, NULL, 'W'},
        {"incr-pct", required_argument, NULL, 'I'},
        {"keys", required_argument, NULL, 'K'},
        {"failure", required_argument, NULL, 'F'},
        {"failure-at", required_argument, NULL, 'A'},
        {"failure-for", required_argument, NULL, 'D'},
        {"verbose", no_argument, NULL, 'v'},
        {"seed", required_argument, NULL, 's'},
        {"json", required_argument, NULL, 'J'},
        {"history", required_argument, NULL, 'H'},
        {"preset", required_argument, NULL, 'P'},
        {"data-dir", required_argument, NULL, 'd'},
        {"help", no_argument, NULL, 'h'},
        {NULL, 0, NULL, 0},
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "n:c:o:t:vs:h", longOpts, NULL)) !=
           -1) {
        switch (opt) {
        case 'n':
            nodeCountOverride = atoi(optarg);
            hasNodeCount = true;
            break;
        case 'c':
            clientCountOverride = atoi(optarg);
            hasClientCount = true;
            break;
        case 'o':
            opsOverride = atoll(optarg);
            hasOps = true;
            break;
        case 't':
            timeOverride = parseTimeSpec(optarg);
            hasTime = true;
            break;
        case 'R':
            config.workload.readPct = atoi(optarg);
            break;
        case 'W':
            config.workload.writePct = atoi(optarg);
            break;
        case 'I':
            config.workload.incrPct = atoi(optarg);
            break;
        case 'K':
            config.workload.keyCount = atoi(optarg);
            break;
        case 'F':
            config.failure.type = parseFailureType(optarg);
            break;
        case 'A':
            config.failure.triggerAtUs = parseTimeSpec(optarg);
            break;
        case 'D':
            config.failure.durationUs = parseTimeSpec(optarg);
            break;
        case 'v':
            hasVerbose = true;
            break;
        case 's':
            config.seed = atoi(optarg);
            break;
        case 'J':
            config.jsonFile = optarg;
            break;
        case 'H':
            config.historyFile = optarg;
            break;
        case 'P':
            preset = optarg;
            break;
        case 'd':
            dataDirOverride = optarg;
            break;
        case 'h':
            printUsage(argv[0]);
            return 0;
        default:
            printUsage(argv[0]);
            return 1;
        }
    }

    /* Apply preset or use minimal defaults */
    if (preset) {
        if (strcmp(preset, "quick") == 0) {
            config = presetQuick();
        } else if (strcmp(preset, "standard") == 0) {
            config = presetStandard();
        } else if (strcmp(preset, "ci") == 0) {
            config = presetCI();
        } else {
            fprintf(stderr, "Unknown preset: %s\n", preset);
            return 1;
        }
    } else {
        /* No preset - use minimal defaults, let CLI override */
        config = presetQuick(); /* Start with quick as base */
    }

    /* Apply CLI overrides after preset */
    if (hasNodeCount) {
        config.nodeCount = nodeCountOverride;
    }
    if (hasClientCount) {
        config.clientCount = clientCountOverride;
    }
    if (hasOps) {
        config.workload.totalOps = opsOverride;
    }
    if (hasTime) {
        config.maxTimeUs = timeOverride;
    }
    if (hasVerbose) {
        config.verbose = true;
    }

    /* Initialize RNG */
    srand(config.seed);

    /* Setup signal handler */
    signal(SIGINT, signalHandler);

    /* Print configuration */
    printf("===============================================================\n");
    printf("  Kraft Linearizability Test\n");
    printf(
        "===============================================================\n\n");
    printf("Configuration:\n");
    printf("  Nodes:      %u\n", config.nodeCount);
    printf("  Clients:    %u\n", config.clientCount);
    printf("  Operations: %" PRIu64 "\n", config.workload.totalOps);
    printf("  Max time:   %.1f seconds\n", config.maxTimeUs / 1000000.0);
    printf("  Failure:    %s\n",
           kraftLinFailureTypeString(config.failure.type));
    printf("  Seed:       %u\n", config.seed);
    printf("\n");

    /* Create data directory with CONSISTENT name */
    char dataDir[512];
    if (dataDirOverride) {
        snprintf(dataDir, sizeof(dataDir), "%s", dataDirOverride);
    } else {
        snprintf(dataDir, sizeof(dataDir), "test-latest");
    }

    /* Create directories using proper APIs */
    /* Create parent directory */
    mkdir(dataDir, 0755);

    /* Create logs subdirectory */
    char logsDir[600];
    snprintf(logsDir, sizeof(logsDir), "%s/logs", dataDir);
    mkdir(logsDir, 0755);

    /* Create node data directories */
    for (uint32_t i = 1; i <= config.nodeCount; i++) {
        char nodeDir[600];
        snprintf(nodeDir, sizeof(nodeDir), "%s/node%u", dataDir, i);
        mkdir(nodeDir, 0755);
    }

    printf("Test data: %s\n", dataDir);

    /* Start cluster */
    printf("Starting cluster...\n");
    kraftLinClusterConfig clusterCfg = {
        .nodeCount = config.nodeCount,
        .basePort = config.basePort,
        .clientBasePort = config.basePort + 100,
        .binaryPath = NULL, /* Auto-detect */
        .dataDir = dataDir,
        .verbose = config.verbose, /* Pass through verbose flag */
    };

    if (config.verbose) {
        printf("[DEBUG] Cluster config: nodes=%u, basePort=%u, "
               "clientBasePort=%u, verbose=%d\n",
               clusterCfg.nodeCount, clusterCfg.basePort,
               clusterCfg.clientBasePort, clusterCfg.verbose);
    }

    g_cluster = kraftLinClusterStart(&clusterCfg);
    if (!g_cluster) {
        fprintf(stderr, "Failed to start cluster\n");
        return 2;
    }

    printf("[✓] Cluster ready (leader: node %u)\n\n", g_cluster->currentLeader);

    /* Create operation history */
    kraftLinHistory *history = kraftLinHistoryCreate(config.workload.totalOps);
    if (!history) {
        fprintf(stderr, "Failed to create history\n");
        kraftLinClusterStop(g_cluster);
        return 2;
    }

    /* Create client pool */
    printf("Starting %u clients...\n", config.clientCount);
    g_clients = kraftLinClientPoolCreate(config.clientCount, g_cluster, history,
                                         &config.workload);
    if (!g_clients) {
        fprintf(stderr, "Failed to create client pool\n");
        kraftLinHistoryDestroy(history);
        kraftLinClusterStop(g_cluster);
        g_cluster = NULL;
        return 2;
    }

    if (!kraftLinClientPoolStart(g_clients)) {
        fprintf(stderr, "Failed to start clients\n");
        kraftLinClientPoolDestroy(g_clients);
        g_clients = NULL;
        kraftLinHistoryDestroy(history);
        kraftLinClusterStop(g_cluster);
        g_cluster = NULL;
        return 2;
    }

    printf("[✓] Clients started\n\n");

    /* Run test */
    printf("Running workload...\n");
    uint64_t startTime = kraftLinGetMonotonicUs();
    bool failureInjected = false;
    bool failureRecovered = false;

    /* Calculate failure trigger time if percentage */
    if (config.failure.triggerAtUs < 100 &&
        config.failure.type != KRAFT_LIN_FAILURE_NONE) {
        /* Treat as percentage of test duration */
        uint64_t pct = config.failure.triggerAtUs;
        config.failure.triggerAtUs = (config.maxTimeUs * pct) / 100;
    }

    while (!g_interrupted) {
        uint64_t elapsed = kraftLinGetMonotonicUs() - startTime;
        uint64_t opsCompleted = kraftLinClientPoolGetOpsCompleted(g_clients);

        /* Check for completion */
        if (opsCompleted >= config.workload.totalOps) {
            printf("\n[✓] All operations completed\n");
            break;
        }

        if (elapsed >= config.maxTimeUs) {
            printf("\n[!] Max time reached\n");
            break;
        }

        /* Inject failure if scheduled */
        if (!failureInjected && config.failure.type != KRAFT_LIN_FAILURE_NONE &&
            elapsed >= config.failure.triggerAtUs) {
            config.failure.verbose = config.verbose;
            kraftLinInjectFailure(g_cluster, &config.failure);
            failureInjected = true;
        }

        /* Recover from failure if duration expired */
        if (failureInjected && !failureRecovered &&
            elapsed >= config.failure.triggerAtUs + config.failure.durationUs) {
            kraftLinRecoverFailure(g_cluster, &config.failure);
            failureRecovered = true;

            /* Wait for new leader */
            if (config.verbose) {
                printf("[RECOVERY] Waiting for leader election...\n");
            }
            kraftLinClusterWaitForLeader(g_cluster, 15);
        }

        /* Progress update */
        if (opsCompleted % 1000 == 0 || elapsed % 5000000 == 0) {
            double progress =
                (double)opsCompleted / config.workload.totalOps * 100.0;
            printf("\rProgress: %.1f%% (%" PRIu64 "/%" PRIu64
                   " ops, %.1fs elapsed)   ",
                   progress, opsCompleted, config.workload.totalOps,
                   elapsed / 1000000.0);
            fflush(stdout);
        }

        usleep(100000); /* 100ms */
    }

    printf("\n\n");

    /* Stop clients */
    printf("Stopping clients...\n");
    kraftLinClientPoolStop(g_clients);
    uint64_t finalOps = kraftLinClientPoolGetOpsCompleted(g_clients);
    printf("[✓] Clients stopped (%" PRIu64 " operations completed)\n\n",
           finalOps);

    /* Stop cluster */
    printf("Stopping cluster...\n");
    kraftLinClusterStop(g_cluster);
    printf("[✓] Cluster stopped\n\n");

    /* Verify linearizability */
    printf("Verifying linearizability...\n");
    kraftLinResult result = kraftLinVerify(history);

    /* Print results */
    printf("\n");
    printf("===============================================================\n");
    if (result.isLinearizable) {
        printf("  RESULT: PASS\n");
    } else {
        printf("  RESULT: VIOLATION DETECTED\n");
    }
    printf("===============================================================\n");
    printf("\n");

    kraftLinResultPrint(&result, history);

    /* Get statistics */
    uint64_t invocations, completions, timeouts;
    kraftLinHistoryGetStats(history, &invocations, &completions, &timeouts);

    printf("Statistics:\n");
    printf("  Invocations:  %" PRIu64 "\n", invocations);
    printf("  Completions:  %" PRIu64 "\n", completions);
    printf("  Timeouts:     %" PRIu64 "\n", timeouts);
    printf("  Success rate: %.2f%%\n",
           (double)completions / invocations * 100.0);
    printf("\n");

    /* Save JSON if requested */
    if (config.jsonFile) {
        FILE *jsonOut = fopen(config.jsonFile, "w");
        if (jsonOut) {
            kraftLinResultPrintJSON(&result, history, jsonOut);
            fclose(jsonOut);
            printf("JSON results saved to: %s\n", config.jsonFile);
        }
    }

    /* Save result before cleanup (kraftLinResultDestroy zeros the struct) */
    bool passed = result.isLinearizable;

    /* Cleanup */
    kraftLinResultDestroy(&result);
    kraftLinClientPoolDestroy(g_clients);
    kraftLinHistoryDestroy(history);

    /* Cleanup data directory - left in place for inspection */
    /* Users can manually delete test-latest if needed */

    /* Exit code */
    return passed ? 0 : 1;
}
