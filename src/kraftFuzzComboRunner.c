/* Kraft Feature Combination Fuzzer Runner
 *
 * Command-line interface for running the feature combination fuzzer.
 */

#ifndef KRAFT_TEST
#define KRAFT_TEST
#endif
#include "kraftFuzzCombo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void printUsage(const char *prog) {
    printf("Usage: %s [OPTIONS]\n", prog);
    printf("\nFeature Combination Fuzzer - Tests various feature "
           "enable/disable combinations\n");
    printf("\nOptions:\n");
    printf("  -n, --nodes N        Number of nodes (default: 5)\n");
    printf("  -o, --ops N          Max ops per combination (default: 1000)\n");
    printf("  -t, --time N         Max time per combination in seconds "
           "(default: 10)\n");
    printf("  -c, --combos N       Number of combinations to test (default: "
           "16)\n");
    printf("  -s, --seed N         Random seed (default: current time)\n");
    printf("  -v, --verbose        Verbose output\n");
    printf("  -r, --report N       Report interval (default: 500)\n");
    printf("  -h, --help           Show this help\n");
    printf("\nFeatures tested in random combinations:\n");
    printf("  - Leadership Transfer (LT)\n");
    printf("  - ReadIndex / Linearizable Reads (RI)\n");
    printf("  - Command Batching (BAT)\n");
    printf("  - Buffer Pool (BP)\n");
    printf("  - Client Sessions (SES)\n");
    printf("  - Witness Nodes (WIT)\n");
    printf("  - Async Commit (AC)\n");
    printf("  - Snapshots (SNP)\n");
    printf("\nExamples:\n");
    printf("  %s                         # Quick test (16 combos)\n", prog);
    printf("  %s -c 64 -o 5000           # 64 combos, 5k ops each\n", prog);
    printf("  %s -s 12345 -c 1           # Single combo with specific seed\n",
           prog);
    printf("  %s -n 7 -c 32 -t 30        # 7 nodes, 32 combos, 30s each\n",
           prog);
}

int main(int argc, char *argv[]) {
    kraftFuzzComboConfig config = kraftFuzzComboConfigDefault();

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printUsage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-n") == 0 ||
                   strcmp(argv[i], "--nodes") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n",
                        argv[i - 1]);
                return 1;
            }
            config.nodeCount = (uint32_t)atoi(argv[i]);
            if (config.nodeCount < 3 || config.nodeCount > 15) {
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
        } else if (strcmp(argv[i], "-c") == 0 ||
                   strcmp(argv[i], "--combos") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n",
                        argv[i - 1]);
                return 1;
            }
            config.combinationsToTest = (uint32_t)atoi(argv[i]);
            if (config.combinationsToTest < 1 ||
                config.combinationsToTest > 256) {
                fprintf(stderr, "Error: combos must be 1-256\n");
                return 1;
            }
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
        } else {
            fprintf(stderr, "Error: unknown option %s\n", argv[i]);
            printUsage(argv[0]);
            return 1;
        }
    }

    printf("=======================================================\n");
    printf("   Kraft Feature Combination Fuzzer                   \n");
    printf("=======================================================\n\n");

    kraftFuzzComboStats stats = {0};
    kraftFuzzComboResult result = kraftFuzzComboRun(&config, &stats);

    printf("\n=======================================================\n");
    printf("   Overall Result: ");

    switch (result) {
    case FUZZ_COMBO_SUCCESS:
        printf("PASSED (all combinations passed)\n");
        break;
    case FUZZ_COMBO_INVARIANT_VIOLATION:
        printf("FAILED (invariant violation in %u combinations)\n",
               stats.combinationsFailed);
        break;
    case FUZZ_COMBO_TIMEOUT:
        printf("PASSED (some combinations hit time limit)\n");
        break;
    case FUZZ_COMBO_ERROR:
        printf("FAILED (error)\n");
        break;
    }

    printf("   Seed: %u  (reproduce with: -s %u -n %u -c %u)\n", config.seed,
           config.seed, config.nodeCount, config.combinationsToTest);
    printf("=======================================================\n");

    /* All passed or timeout-only is success */
    return (result == FUZZ_COMBO_SUCCESS || result == FUZZ_COMBO_TIMEOUT) ? 0
                                                                          : 1;
}
