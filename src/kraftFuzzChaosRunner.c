/* Kraft Chaos Fuzzer Runner
 *
 * Command-line interface for running the chaos-enhanced fuzzer.
 */

#ifndef KRAFT_TEST
#define KRAFT_TEST
#endif

#include "kraftFuzzChaos.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void printUsage(const char *prog) {
    printf("Usage: %s [OPTIONS]\n", prog);
    printf("\nChaos-Enhanced Fuzzer - Tests Raft under adversarial "
           "network/Byzantine conditions\n");
    printf("\nOptions:\n");
    printf("  -n, --nodes N         Number of nodes (3-1000, default: 5)\n");
    printf("  -o, --ops N           Max operations (default: 100000)\n");
    printf("  -t, --time N          Max time in seconds (default: 300, or use "
           "Nm/Nh)\n");
    printf("  -s, --seed N          Random seed (default: current time)\n");
    printf("  -v, --verbose         Verbose output\n");
    printf("  -r, --report N        Report interval (default: 10000)\n");
    printf("  -h, --help            Show this help\n");
    printf("\nScenario Options:\n");
    printf("  --scenario NAME       Use preset scenario:\n");
    printf("                          none      - No chaos (baseline)\n");
    printf("                          flaky     - Flaky network (15%% loss)\n");
    printf("                          delay     - High latency network\n");
    printf("                          partition - Frequent partitions\n");
    printf("                          storm     - Partition storms\n");
    printf("                          byzantine - Single Byzantine node\n");
    printf("                          byzantine-multi - Multiple Byzantine "
           "nodes\n");
    printf("                          clock     - Clock drift/jumps\n");
    printf("                          gc        - Simulated GC pauses\n");
    printf(
        "                          mixed     - Mix of all faults (default)\n");
    printf("                          hellweek  - Maximum adversarial\n");
    printf("\nByzantine Options:\n");
    printf("  --byzantine N         Enable N Byzantine nodes\n");
    printf("  --rotate              Rotate which nodes are Byzantine\n");
    printf("\nChaos Dynamics:\n");
    printf("  --dynamic             Change chaos parameters over time\n");
    printf("  --escalate            Increase chaos intensity over time\n");
    printf("  --phase-interval N    Operations between chaos phase changes\n");
    printf("\nExamples:\n");
    printf(
        "  %s                                    # Default: 100k ops, 5 min\n",
        prog);
    printf("  %s -o 1000000 -t 30m                  # 1M ops, 30 minutes\n",
           prog);
    printf("  %s -n 100 -o 500000 -t 1h             # 100 nodes, 1 hour\n",
           prog);
    printf("  %s --scenario hellweek -o 10000000    # Extended hellweek\n",
           prog);
    printf("  %s --byzantine 2 -n 7           # 7 nodes with 2 Byzantine\n",
           prog);
    printf("  %s -s 12345 --scenario storm    # Reproducible storm test\n",
           prog);
}

static uint64_t parseTime(const char *str) {
    /* Parse time with optional suffix: s (seconds), m (minutes), h (hours) */
    char *end;
    uint64_t val = strtoull(str, &end, 10);
    if (*end == 'm' || *end == 'M') {
        val *= 60; /* minutes to seconds */
    } else if (*end == 'h' || *end == 'H') {
        val *= 3600; /* hours to seconds */
    }
    return val * 1000000UL; /* convert to microseconds */
}

static kraftFuzzChaosScenario parseScenario(const char *name) {
    if (strcmp(name, "none") == 0) {
        return FUZZ_CHAOS_NONE;
    }
    if (strcmp(name, "flaky") == 0) {
        return FUZZ_CHAOS_FLAKY;
    }
    if (strcmp(name, "delay") == 0) {
        return FUZZ_CHAOS_DELAY;
    }
    if (strcmp(name, "partition") == 0) {
        return FUZZ_CHAOS_PARTITION;
    }
    if (strcmp(name, "storm") == 0) {
        return FUZZ_CHAOS_STORM;
    }
    if (strcmp(name, "byzantine") == 0) {
        return FUZZ_CHAOS_BYZANTINE_SINGLE;
    }
    if (strcmp(name, "byzantine-multi") == 0) {
        return FUZZ_CHAOS_BYZANTINE_MULTI;
    }
    if (strcmp(name, "clock") == 0) {
        return FUZZ_CHAOS_CLOCK_DRIFT;
    }
    if (strcmp(name, "gc") == 0) {
        return FUZZ_CHAOS_GC_PAUSES;
    }
    if (strcmp(name, "mixed") == 0) {
        return FUZZ_CHAOS_MIXED;
    }
    if (strcmp(name, "hellweek") == 0) {
        return FUZZ_CHAOS_HELLWEEK;
    }
    return FUZZ_CHAOS_MIXED; /* default */
}

int main(int argc, char *argv[]) {
    kraftFuzzChaosConfig config = kraftFuzzChaosConfigDefault();
    config.seed = (uint32_t)time(NULL);

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
            if (config.nodeCount < 3 || config.nodeCount > 1000) {
                fprintf(stderr, "Error: nodes must be 3-1000\n");
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
            config.maxTime = parseTime(argv[i]);
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
        } else if (strcmp(argv[i], "--scenario") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --scenario requires an argument\n");
                return 1;
            }
            /* Save user-specified values before loading scenario */
            uint32_t savedNodeCount = config.nodeCount;
            uint64_t savedMaxOps = config.maxOperations;
            uint64_t savedMaxTime = config.maxTime;
            uint32_t savedSeed = config.seed;
            bool savedVerbose = config.verbose;
            uint64_t savedReportInterval = config.reportInterval;

            config = kraftFuzzChaosConfigForScenario(parseScenario(argv[i]));

            /* Restore user-specified values (if they differ from defaults) */
            if (savedNodeCount != 5) {
                config.nodeCount = savedNodeCount;
            }
            if (savedMaxOps != 100000) {
                config.maxOperations = savedMaxOps;
            }
            if (savedMaxTime != 5 * 60 * 1000000UL) {
                config.maxTime = savedMaxTime;
            }
            if (savedSeed != 0) {
                config.seed = savedSeed;
            }
            if (savedVerbose) {
                config.verbose = savedVerbose;
            }
            if (savedReportInterval != 10000) {
                config.reportInterval = savedReportInterval;
            }

            /* Reset seed if still default */
            if (config.seed == 0) {
                config.seed = (uint32_t)time(NULL);
            }
        } else if (strcmp(argv[i], "--byzantine") == 0) {
            if (++i >= argc) {
                fprintf(stderr, "Error: --byzantine requires an argument\n");
                return 1;
            }
            config.byzantineNodeCount = (uint32_t)atoi(argv[i]);
            config.byzantineBehavior = KRAFT_BYZANTINE_EQUIVOCATE;
        } else if (strcmp(argv[i], "--rotate") == 0) {
            config.rotateByzantine = true;
        } else if (strcmp(argv[i], "--dynamic") == 0) {
            config.dynamicChaos = true;
        } else if (strcmp(argv[i], "--escalate") == 0) {
            config.escalatingChaos = true;
        } else if (strcmp(argv[i], "--phase-interval") == 0) {
            if (++i >= argc) {
                fprintf(stderr,
                        "Error: --phase-interval requires an argument\n");
                return 1;
            }
            config.chaosPhaseInterval = (uint32_t)atoi(argv[i]);
        } else if (strcmp(argv[i], "--log-events") == 0) {
            config.logChaosEvents = true;
        } else {
            fprintf(stderr, "Error: unknown option %s\n", argv[i]);
            printUsage(argv[0]);
            return 1;
        }
    }

    printf("=======================================================\n");
    printf("   Kraft Chaos-Enhanced Fuzzer                        \n");
    printf("=======================================================\n\n");

    printf("Configuration:\n");
    printf("  Nodes:        %u\n", config.nodeCount);
    printf("  Scenario:     %s\n", kraftFuzzChaosScenarioName(config.scenario));
    printf("  Max ops:      %" PRIu64 "\n", config.maxOperations);
    printf("  Max time:     %" PRIu64 " s\n", config.maxTime / 1000000UL);
    printf("  Seed:         %u\n", config.seed);
    if (config.byzantineNodeCount > 0) {
        printf("  Byzantine:    %u node(s)%s\n", config.byzantineNodeCount,
               config.rotateByzantine ? " (rotating)" : "");
    }
    if (config.dynamicChaos) {
        printf("  Dynamic:      yes%s\n",
               config.escalatingChaos ? " (escalating)" : "");
    }
    printf("\n");

    kraftFuzzChaosStats stats = {0};
    kraftFuzzChaosResult result = kraftFuzzChaosRun(&config, &stats);

    /* Print statistics */
    kraftFuzzChaosPrintStats(&stats);

    printf("\n=======================================================\n");
    printf("   Result: ");

    switch (result) {
    case FUZZ_CHAOS_SUCCESS:
        printf("PASSED (completed all operations)\n");
        break;
    case FUZZ_CHAOS_INVARIANT_VIOLATION:
        printf("FAILED (Raft invariant violation)\n");
        printf("   Violation: %s\n", stats.lastViolation);
        break;
    case FUZZ_CHAOS_BYZANTINE_BREACH:
        printf("FAILED (Byzantine fault compromised safety)\n");
        break;
    case FUZZ_CHAOS_TIMEOUT:
        printf("PASSED (ran to time limit)\n");
        break;
    case FUZZ_CHAOS_ERROR:
        printf("FAILED (internal error)\n");
        break;
    }

    printf("   Seed: %u  (reproduce with: -s %u -n %u --scenario %s)\n",
           config.seed, config.seed, config.nodeCount,
           kraftFuzzChaosScenarioName(config.scenario));
    printf("=======================================================\n");

    return (result == FUZZ_CHAOS_SUCCESS || result == FUZZ_CHAOS_TIMEOUT) ? 0
                                                                          : 1;
}
