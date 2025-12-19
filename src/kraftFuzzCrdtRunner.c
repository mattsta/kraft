/* Kraft CRDT Property Fuzzer Runner
 *
 * Command-line interface for CRDT property-based testing.
 */

#ifndef KRAFT_TEST
#define KRAFT_TEST
#endif

#include "kraftFuzzCrdt.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void printUsage(const char *prog) {
    printf("Usage: %s [OPTIONS]\n", prog);
    printf("\nCRDT Property Fuzzer - Verifies CRDT mathematical properties\n");
    printf("\nOptions:\n");
    printf(
        "  -o, --ops N           Operations per property (default: 10000)\n");
    printf("  -r, --replicas N      Number of simulated replicas (2-1000, "
           "default: 10)\n");
    printf("  -s, --seed N          Random seed (default: current time)\n");
    printf("  -v, --verbose         Verbose output\n");
    printf("  -h, --help            Show this help\n");
    printf("\nType Selection:\n");
    printf("  --type TYPE           Test specific type only:\n");
    printf("                          lww       - LWW Register\n");
    printf("                          gcounter  - G-Counter\n");
    printf("                          pncounter - PN-Counter\n");
    printf("                          gset      - G-Set\n");
    printf("                          orset     - OR-Set\n");
    printf("                          all       - All types (default)\n");
    printf("\nProperty Selection:\n");
    printf("  --property PROP       Test specific property only:\n");
    printf("                          comm      - Commutativity\n");
    printf("                          assoc     - Associativity\n");
    printf("                          idemp     - Idempotence\n");
    printf("                          mono      - Monotonicity\n");
    printf("                          conv      - Convergence\n");
    printf("                          all       - All properties (default)\n");
    printf("\nExamples:\n");
    printf(
        "  %s                               # Default: 10k ops, 10 replicas\n",
        prog);
    printf("  %s -o 100000 -r 100              # 100k ops, 100 replicas\n",
           prog);
    printf("  %s --type gcounter -o 1000000    # Intensive G-Counter testing\n",
           prog);
    printf("  %s --property comm -r 500        # Commutativity with 500 "
           "replicas\n",
           prog);
    printf("  %s -s 12345 --verbose            # Reproducible verbose test\n",
           prog);
}

static kraftCrdtType parseType(const char *name) {
    if (strcmp(name, "lww") == 0) {
        return KRAFT_CRDT_LWW_REGISTER;
    }
    if (strcmp(name, "gcounter") == 0) {
        return KRAFT_CRDT_G_COUNTER;
    }
    if (strcmp(name, "pncounter") == 0) {
        return KRAFT_CRDT_PN_COUNTER;
    }
    if (strcmp(name, "gset") == 0) {
        return KRAFT_CRDT_G_SET;
    }
    if (strcmp(name, "orset") == 0) {
        return KRAFT_CRDT_OR_SET;
    }
    if (strcmp(name, "all") == 0) {
        return KRAFT_CRDT_NONE;
    }
    return KRAFT_CRDT_NONE;
}

static uint32_t parseProperty(const char *name) {
    if (strcmp(name, "comm") == 0) {
        return CRDT_PROP_COMMUTATIVITY;
    }
    if (strcmp(name, "assoc") == 0) {
        return CRDT_PROP_ASSOCIATIVITY;
    }
    if (strcmp(name, "idemp") == 0) {
        return CRDT_PROP_IDEMPOTENCE;
    }
    if (strcmp(name, "mono") == 0) {
        return CRDT_PROP_MONOTONICITY;
    }
    if (strcmp(name, "conv") == 0) {
        return CRDT_PROP_CONVERGENCE;
    }
    if (strcmp(name, "all") == 0) {
        return CRDT_PROP_ALL;
    }
    return CRDT_PROP_ALL;
}

int main(int argc, char *argv[]) {
    kraftFuzzCrdtConfig config = kraftFuzzCrdtConfigDefault();
    config.seed = (uint32_t)time(NULL);

    /* Parse arguments */
    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-h") == 0 || strcmp(argv[i], "--help") == 0) {
            printUsage(argv[0]);
            return 0;
        } else if (strcmp(argv[i], "-o") == 0 ||
                   strcmp(argv[i], "--ops") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n",
                        argv[i - 1]);
                return 1;
            }
            config.opsPerProperty = (uint32_t)atoi(argv[i]);
        } else if (strcmp(argv[i], "-r") == 0 ||
                   strcmp(argv[i], "--replicas") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: %s requires an argument\n",
                        argv[i - 1]);
                return 1;
            }
            config.replicaCount = (uint32_t)atoi(argv[i]);
            if (config.replicaCount < 2 || config.replicaCount > 1000) {
                fprintf(stderr, "Error: replicas must be 2-1000\n");
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
        } else if (strcmp(argv[i], "--type") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --type requires an argument\n");
                return 1;
            }
            config.typeFilter = parseType(argv[i]);
        } else if (strcmp(argv[i], "--property") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --property requires an argument\n");
                return 1;
            }
            config.propertyMask = parseProperty(argv[i]);
        } else if (strcmp(argv[i], "--continue") == 0) {
            config.stopOnFailure = false;
        } else {
            fprintf(stderr, "Error: unknown option %s\n", argv[i]);
            printUsage(argv[0]);
            return 1;
        }
    }

    printf("=======================================================\n");
    printf("   Kraft CRDT Property Fuzzer                         \n");
    printf("=======================================================\n\n");

    printf("Configuration:\n");
    printf("  Seed:               %u\n", config.seed);
    printf("  Replicas:           %u\n", config.replicaCount);
    printf("  Ops per property:   %u\n", config.opsPerProperty);
    printf("  Type filter:        %s\n",
           config.typeFilter == KRAFT_CRDT_NONE
               ? "all"
               : kraftFuzzCrdtTypeName(config.typeFilter));
    printf("  Properties:         ");
    if (config.propertyMask == CRDT_PROP_ALL) {
        printf("all");
    } else {
        if (config.propertyMask & CRDT_PROP_COMMUTATIVITY) {
            printf("comm ");
        }
        if (config.propertyMask & CRDT_PROP_ASSOCIATIVITY) {
            printf("assoc ");
        }
        if (config.propertyMask & CRDT_PROP_IDEMPOTENCE) {
            printf("idemp ");
        }
        if (config.propertyMask & CRDT_PROP_MONOTONICITY) {
            printf("mono ");
        }
        if (config.propertyMask & CRDT_PROP_CONVERGENCE) {
            printf("conv ");
        }
    }
    printf("\n\n");

    kraftFuzzCrdtStats stats = {0};
    kraftFuzzCrdtResult result = kraftFuzzCrdtRun(&config, &stats);

    /* Print detailed statistics */
    kraftFuzzCrdtPrintStats(&stats);

    printf("\n=======================================================\n");
    printf("   Result: ");

    switch (result) {
    case FUZZ_CRDT_SUCCESS:
        printf("PASSED (all properties verified)\n");
        break;
    case FUZZ_CRDT_PROPERTY_VIOLATION:
        printf("FAILED (property violation)\n");
        printf("   %s\n", stats.lastFailure);
        break;
    case FUZZ_CRDT_TIMEOUT:
        printf("TIMEOUT\n");
        break;
    case FUZZ_CRDT_ERROR:
        printf("ERROR\n");
        break;
    }

    printf("   Seed: %u  (reproduce with: -s %u)\n", config.seed, config.seed);
    printf("=======================================================\n");

    return (result == FUZZ_CRDT_SUCCESS) ? 0 : 1;
}
