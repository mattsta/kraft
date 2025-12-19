#define KRAFT_TEST
#include "kraftFuzz.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void printUsage(const char *prog) {
    printf("Usage: %s [OPTIONS]\n", prog);
    printf("\nOptions:\n");
    printf("  -n, --nodes N        Number of nodes (default: 5)\n");
    printf("  -o, --ops N          Max operations (default: 10000)\n");
    printf("  -t, --time N         Max time in seconds (default: 60)\n");
    printf("  -s, --seed N         Random seed (default: current time)\n");
    printf("  -v, --verbose        Verbose output\n");
    printf(
        "  --replay             Replay mode: step-by-step operation output\n");
    printf("  -r, --report N       Report interval (default: 1000)\n");
    printf("  -h, --help           Show this help\n");
    printf("\nExamples:\n");
    printf("  %s                         # Quick fuzz test\n", prog);
    printf("  %s -n 7 -o 100000          # 7 nodes, 100k operations\n", prog);
    printf("  %s -t 3600 -v              # 1 hour, verbose\n", prog);
    printf(
        "  %s -s 12345 --replay       # Replay seed with step-by-step output\n",
        prog);
}

int main(int argc, char *argv[]) {
    kraftFuzzConfig config = kraftFuzzConfigDefault();

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
            if (config.nodeCount < 1 || config.nodeCount > 100) {
                fprintf(stderr, "Error: node count must be 1-100\n");
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
        } else if (strcmp(argv[i], "--replay") == 0) {
            config.replayMode = true;
            config.verbose = true; /* Replay mode implies verbose */
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
    printf("   Kraft Raft Consensus - Fuzzer                     \n");
    printf("=======================================================\n\n");

    kraftFuzzStats stats = {0};
    kraftFuzzResult result = kraftFuzzRun(&config, &stats);

    printf("\n=======================================================\n");
    printf("   Fuzzing Result: ");

    switch (result) {
    case FUZZ_SUCCESS:
        printf("PASSED (completed all operations)\n");
        break;
    case FUZZ_INVARIANT_VIOLATION:
        printf("FAILED (invariant violation)\n");
        break;
    case FUZZ_TIMEOUT:
        printf("PASSED (ran to time limit without issues)\n");
        break;
    case FUZZ_ERROR:
        printf("FAILED (error)\n");
        break;
    }

    /* Always print seed at the end for reproducibility - this is visible
     * when tailing output from failed runs */
    printf("   Seed: %u  (reproduce with: -s %u -n %u)\n", config.seed,
           config.seed, config.nodeCount);
    printf("=======================================================\n");

    /* Both SUCCESS and TIMEOUT are passing results */
    return (result == FUZZ_SUCCESS || result == FUZZ_TIMEOUT) ? 0 : 1;
}
