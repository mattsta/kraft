#ifndef KRAFT_FUZZ_CRDT_H
#define KRAFT_FUZZ_CRDT_H

/* Kraft CRDT Property-Based Fuzzer
 * ==================================
 * Tests CRDT implementations for mathematical correctness by verifying:
 *
 * 1. Commutativity:  merge(a, b) == merge(b, a)
 * 2. Associativity:  merge(merge(a, b), c) == merge(a, merge(b, c))
 * 3. Idempotence:    merge(a, a) == a
 * 4. Monotonicity:   value only increases (for counters/sets)
 * 5. Convergence:    all replicas eventually agree
 *
 * Each CRDT type is tested with:
 *   - Random operation sequences
 *   - Concurrent operations across simulated replicas
 *   - Merge ordering variations
 *   - Edge cases (empty values, max values, etc.)
 *
 * Usage:
 *   ./kraft-fuzz-crdt                    # Test all CRDT types
 *   ./kraft-fuzz-crdt --type lww         # Test LWW Register only
 *   ./kraft-fuzz-crdt --property comm    # Test commutativity only
 *   ./kraft-fuzz-crdt -o 100000 -r 10    # 100k ops, 10 replicas
 */

#include "crdt.h"
#include <stdbool.h>
#include <stdint.h>

/* =========================================================================
 * Test Configuration
 * ========================================================================= */

/* CRDT properties to test */
typedef enum kraftCrdtProperty {
    CRDT_PROP_NONE = 0,
    CRDT_PROP_COMMUTATIVITY = (1 << 0), /* merge(a,b) == merge(b,a) */
    CRDT_PROP_ASSOCIATIVITY =
        (1 << 1), /* merge(merge(a,b),c) == merge(a,merge(b,c)) */
    CRDT_PROP_IDEMPOTENCE = (1 << 2),  /* merge(a,a) == a */
    CRDT_PROP_MONOTONICITY = (1 << 3), /* value only grows */
    CRDT_PROP_CONVERGENCE = (1 << 4),  /* replicas agree after sync */
    CRDT_PROP_PERSISTENCE = (1 << 5),  /* serialize/deserialize roundtrip */
    CRDT_PROP_ALL = 0x3F
} kraftCrdtProperty;

/* Test modes */
typedef enum kraftCrdtTestMode {
    CRDT_MODE_SEQUENTIAL,  /* Operations in sequence */
    CRDT_MODE_CONCURRENT,  /* Simulated concurrent ops */
    CRDT_MODE_ADVERSARIAL, /* Designed to break properties */
    CRDT_MODE_RANDOM       /* Fully random operations */
} kraftCrdtTestMode;

/* Configuration */
typedef struct kraftFuzzCrdtConfig {
    /* Basic parameters */
    uint64_t maxOperations; /* Max operations to perform */
    uint32_t replicaCount;  /* Number of simulated replicas */
    uint32_t seed;          /* Random seed */

    /* What to test */
    kraftCrdtType typeFilter; /* Specific type to test (NONE = all) */
    uint32_t propertyMask;    /* Which properties to test */
    kraftCrdtTestMode mode;   /* Test mode */

    /* Test intensity */
    uint32_t opsPerProperty; /* Operations per property test */
    uint32_t mergeDepth;     /* Max merge chain depth */
    uint32_t valueSize;      /* Max value size for registers/sets */

    /* Convergence testing */
    uint32_t convergenceRounds; /* Rounds of sync for convergence */
    float partitionProbability; /* Simulate partitions during convergence */

    /* Reporting */
    bool verbose;
    uint64_t reportInterval;
    bool stopOnFailure; /* Stop on first failure */
} kraftFuzzCrdtConfig;

/* =========================================================================
 * Statistics
 * ========================================================================= */

typedef struct kraftFuzzCrdtTypeStats {
    const char *typeName;
    uint64_t operationsRun;
    uint64_t propertiesChecked;
    uint64_t propertiesPassed;
    uint64_t propertiesFailed;

    /* Per-property results */
    uint64_t commutativityChecks;
    uint64_t commutativityPassed;
    uint64_t associativityChecks;
    uint64_t associativityPassed;
    uint64_t idempotenceChecks;
    uint64_t idempotencePassed;
    uint64_t monotonicityChecks;
    uint64_t monotonicityPassed;
    uint64_t convergenceChecks;
    uint64_t convergencePassed;
    uint64_t persistenceChecks;
    uint64_t persistencePassed;
} kraftFuzzCrdtTypeStats;

typedef struct kraftFuzzCrdtStats {
    uint64_t totalOperations;
    uint64_t totalChecks;
    uint64_t totalPassed;
    uint64_t totalFailed;

    /* Per-type stats */
    kraftFuzzCrdtTypeStats lwwRegister;
    kraftFuzzCrdtTypeStats gCounter;
    kraftFuzzCrdtTypeStats pnCounter;
    kraftFuzzCrdtTypeStats gSet;
    kraftFuzzCrdtTypeStats orSet;

    /* Timing */
    uint64_t elapsedMs;

    /* Failure info */
    char lastFailure[256];
    kraftCrdtType failedType;
    kraftCrdtProperty failedProperty;
} kraftFuzzCrdtStats;

/* =========================================================================
 * Results
 * ========================================================================= */

typedef enum kraftFuzzCrdtResult {
    FUZZ_CRDT_SUCCESS = 0,        /* All properties verified */
    FUZZ_CRDT_PROPERTY_VIOLATION, /* Property failed */
    FUZZ_CRDT_TIMEOUT,            /* Time limit hit */
    FUZZ_CRDT_ERROR               /* Internal error */
} kraftFuzzCrdtResult;

/* =========================================================================
 * API Functions
 * ========================================================================= */

/**
 * Get default configuration
 */
kraftFuzzCrdtConfig kraftFuzzCrdtConfigDefault(void);

/**
 * Run CRDT property tests
 */
kraftFuzzCrdtResult kraftFuzzCrdtRun(const kraftFuzzCrdtConfig *config,
                                     kraftFuzzCrdtStats *stats);

/**
 * Test a specific CRDT type
 */
kraftFuzzCrdtResult kraftFuzzCrdtTestType(kraftCrdtType type,
                                          const kraftFuzzCrdtConfig *config,
                                          kraftFuzzCrdtTypeStats *stats);

/**
 * Test a specific property for a type
 */
bool kraftFuzzCrdtTestProperty(kraftCrdtType type, kraftCrdtProperty property,
                               const kraftFuzzCrdtConfig *config);

/**
 * Print statistics summary
 */
void kraftFuzzCrdtPrintStats(const kraftFuzzCrdtStats *stats);

/**
 * Get property name
 */
const char *kraftFuzzCrdtPropertyName(kraftCrdtProperty property);

/**
 * Get type name
 */
const char *kraftFuzzCrdtTypeName(kraftCrdtType type);

#endif /* KRAFT_FUZZ_CRDT_H */
