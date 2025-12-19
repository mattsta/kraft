/* Kraft Multi-Raft Fuzzer Runner
 *
 * Command-line interface for running the Multi-Raft fuzzer.
 * Tests multiple independent Raft groups with actual consensus.
 */

#ifndef KRAFT_TEST
#define KRAFT_TEST
#endif
#include "kraftFuzzMulti.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void printUsage(const char *prog) {
    printf("Usage: %s [OPTIONS]\n", prog);
    printf("\nMulti-Raft Fuzzer - Tests multiple independent Raft consensus "
           "groups\n");
    printf("\nCluster Options:\n");
    printf("  -g, --groups N       Number of Raft groups (default: 3)\n");
    printf("  -n, --nodes N        Nodes per group (default: 5)\n");
    printf("\nLimit Options:\n");
    printf("  -o, --ops N          Max operations (default: 50000)\n");
    printf("  -t, --time N         Max time in seconds (default: 120)\n");
    printf("  -s, --seed N         Random seed (default: current time)\n");
    printf("\nOutput Options:\n");
    printf("  -v, --verbose        Verbose output\n");
    printf("  -r, --report N       Report interval (default: 1000)\n");
    printf("\nInvariant Options:\n");
    printf("  --no-election        Disable election safety checks\n");
    printf("  --no-log             Disable log matching checks\n");
    printf("  --no-leader          Disable leader completeness checks\n");
    printf("  --no-state           Disable state machine safety checks\n");
    printf("\nOther:\n");
    printf("  -h, --help           Show this help\n");
    printf("\nExamples:\n");
    printf("  %s                               # Default: 3 groups x 5 nodes\n",
           prog);
    printf("  %s -g 10 -n 7 -o 100000          # 10 groups, 7 nodes each, 100k "
           "ops\n",
           prog);
    printf("  %s -g 5 -n 5 -t 3600             # 5x5 cluster, 1 hour\n", prog);
    printf("  %s -s 12345 -g 3 -n 5 -v         # Reproduce seed, verbose\n",
           prog);
    printf("\nThis fuzzer runs REAL Raft consensus in multiple independent "
           "groups,\n");
    printf("testing: command submission, node crashes/recovery, network "
           "partitions,\n");
    printf("cross-group operations, and Raft safety invariants.\n");
}

int main(int argc, char *argv[]) {
    kraftFuzzMultiConfig config = kraftFuzzMultiConfigDefault();

    /* Parse arguments */
    for (int32_t i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printUsage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-g") == 0 ||
                   strcmp(argv[i], "--groups") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n",
                        argv[i - 1]);
                return 1;
            }
            config.groupCount = (uint32_t)atoi(argv[i]);
            if (config.groupCount < 1 || config.groupCount > 64) {
                fprintf(stderr, "Error: groups must be 1-64\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-n") == 0 ||
                   strcmp(argv[i], "--nodes") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n",
                        argv[i - 1]);
                return 1;
            }
            config.nodesPerGroup = (uint32_t)atoi(argv[i]);
            if (config.nodesPerGroup < 3 || config.nodesPerGroup > 15) {
                fprintf(stderr, "Error: nodes must be 3-15\n");
                return 1;
            }
        } else if (strcmp(argv[i], "-o") == 0 ||
                   strcmp(argv[i], "--ops") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n",
                        argv[i - 1]);
                return 1;
            }
            config.maxOperations = (uint64_t)atoll(argv[i]);
        } else if (strcmp(argv[i], "-t") == 0 ||
                   strcmp(argv[i], "--time") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n",
                        argv[i - 1]);
                return 1;
            }
            config.maxTime =
                (uint64_t)atoll(argv[i]) * 1000000UL; /* seconds to us */
        } else if (strcmp(argv[i], "-s") == 0 ||
                   strcmp(argv[i], "--seed") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n",
                        argv[i - 1]);
                return 1;
            }
            config.seed = (uint32_t)atoi(argv[i]);
        } else if (strcmp(argv[i], "-v") == 0 ||
                   strcmp(argv[i], "--verbose") == 0) {
            config.verbose = true;
        } else if (strcmp(argv[i], "-r") == 0 ||
                   strcmp(argv[i], "--report") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n",
                        argv[i - 1]);
                return 1;
            }
            config.reportInterval = (uint64_t)atoll(argv[i]);
        } else if (strcmp(argv[i], "--no-election") == 0) {
            config.checkElectionSafety = false;
        } else if (strcmp(argv[i], "--no-log") == 0) {
            config.checkLogMatching = false;
        } else if (strcmp(argv[i], "--no-leader") == 0) {
            config.checkLeaderCompleteness = false;
        } else if (strcmp(argv[i], "--no-state") == 0) {
            config.checkStateMachineSafety = false;
        } else {
            fprintf(stderr, "Error: unknown option %s\n", argv[i]);
            printUsage(argv[0]);
            return 1;
        }
    }

    /* Run fuzzer - it handles its own banner output */
    kraftFuzzMultiStats stats = {0};
    kraftFuzzMultiResult result = kraftFuzzMultiRun(&config, &stats);

    printf("\nSeed: %" PRIu32 "  (reproduce with: -s %" PRIu32 " -g %" PRIu32
           " -n %" PRIu32 ")\n",
           config.seed, config.seed, config.groupCount, config.nodesPerGroup);

    /* Both SUCCESS and TIMEOUT are passing results */
    return (result == FUZZ_MULTI_SUCCESS || result == FUZZ_MULTI_TIMEOUT) ? 0
                                                                          : 1;
}
