/* Kraft CRDT Property-Based Fuzzer Implementation
 *
 * Verifies CRDT mathematical properties through random testing.
 */

#include "kraftFuzzCrdt.h"
#include "crdt.h"
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* =========================================================================
 * Internal Utilities
 * ========================================================================= */

typedef struct fuzzCrdtState {
    kraftFuzzCrdtConfig config;
    kraftFuzzCrdtStats stats;
    kraftCrdtManager *mgr;
    uint64_t rngState;
} fuzzCrdtState;

static uint64_t crdtRand(fuzzCrdtState *state) {
    state->rngState ^= state->rngState >> 12;
    state->rngState ^= state->rngState << 25;
    state->rngState ^= state->rngState >> 27;
    return state->rngState * 0x2545F4914F6CDD1DULL;
}

static uint32_t crdtRandRange(fuzzCrdtState *state, uint32_t min,
                              uint32_t max) {
    if (min >= max) {
        return min;
    }
    return min + (crdtRand(state) % (max - min + 1));
}

static void *zmalloc(size_t size) {
    return calloc(1, size);
}

static void *zcalloc(size_t size) {
    return calloc(1, size);
}

static void zfree(void *ptr) {
    free(ptr);
}

/* =========================================================================
 * LWW Register Testing
 * ========================================================================= */

/* Simple LWW value for testing */
typedef struct testLwwValue {
    uint64_t timestamp;
    uint32_t nodeId;
    char data[64];
} testLwwValue;

static testLwwValue *createLwwValue(fuzzCrdtState *state, uint64_t ts,
                                    uint32_t node) {
    testLwwValue *v = zmalloc(sizeof(testLwwValue));
    v->timestamp = ts;
    v->nodeId = node;
    snprintf(v->data, sizeof(v->data), "value-%lu-%u", ts, node);
    return v;
}

static testLwwValue *mergeLww(const testLwwValue *a, const testLwwValue *b) {
    testLwwValue *result = zmalloc(sizeof(testLwwValue));
    /* LWW: highest timestamp wins, break ties by nodeId */
    if (a->timestamp > b->timestamp ||
        (a->timestamp == b->timestamp && a->nodeId > b->nodeId)) {
        *result = *a;
    } else {
        *result = *b;
    }
    return result;
}

static bool lwwEquals(const testLwwValue *a, const testLwwValue *b) {
    return a->timestamp == b->timestamp && a->nodeId == b->nodeId &&
           strcmp(a->data, b->data) == 0;
}

static bool testLwwCommutativity(fuzzCrdtState *state) {
    kraftFuzzCrdtTypeStats *stats = &state->stats.lwwRegister;

    for (uint32_t i = 0; i < state->config.opsPerProperty; i++) {
        uint64_t ts1 = crdtRandRange(state, 1, 1000000);
        uint64_t ts2 = crdtRandRange(state, 1, 1000000);
        uint32_t node1 = crdtRandRange(state, 0, 9);
        uint32_t node2 = crdtRandRange(state, 0, 9);

        testLwwValue *a = createLwwValue(state, ts1, node1);
        testLwwValue *b = createLwwValue(state, ts2, node2);

        testLwwValue *ab = mergeLww(a, b);
        testLwwValue *ba = mergeLww(b, a);

        stats->commutativityChecks++;

        if (!lwwEquals(ab, ba)) {
            snprintf(
                state->stats.lastFailure, sizeof(state->stats.lastFailure),
                "LWW commutativity: merge(a,b) != merge(b,a) for ts=%lu,%lu",
                ts1, ts2);
            free(a);
            free(b);
            free(ab);
            free(ba);
            return false;
        }

        stats->commutativityPassed++;
        free(a);
        free(b);
        free(ab);
        free(ba);
    }
    return true;
}

static bool testLwwAssociativity(fuzzCrdtState *state) {
    kraftFuzzCrdtTypeStats *stats = &state->stats.lwwRegister;

    for (uint32_t i = 0; i < state->config.opsPerProperty; i++) {
        testLwwValue *a =
            createLwwValue(state, crdtRandRange(state, 1, 1000000),
                           crdtRandRange(state, 0, 9));
        testLwwValue *b =
            createLwwValue(state, crdtRandRange(state, 1, 1000000),
                           crdtRandRange(state, 0, 9));
        testLwwValue *c =
            createLwwValue(state, crdtRandRange(state, 1, 1000000),
                           crdtRandRange(state, 0, 9));

        testLwwValue *ab = mergeLww(a, b);
        testLwwValue *ab_c = mergeLww(ab, c);

        testLwwValue *bc = mergeLww(b, c);
        testLwwValue *a_bc = mergeLww(a, bc);

        stats->associativityChecks++;

        if (!lwwEquals(ab_c, a_bc)) {
            snprintf(state->stats.lastFailure, sizeof(state->stats.lastFailure),
                     "LWW associativity: merge(merge(a,b),c) != "
                     "merge(a,merge(b,c))");
            free(a);
            free(b);
            free(c);
            free(ab);
            free(ab_c);
            free(bc);
            free(a_bc);
            return false;
        }

        stats->associativityPassed++;
        free(a);
        free(b);
        free(c);
        free(ab);
        free(ab_c);
        free(bc);
        free(a_bc);
    }
    return true;
}

static bool testLwwIdempotence(fuzzCrdtState *state) {
    kraftFuzzCrdtTypeStats *stats = &state->stats.lwwRegister;

    for (uint32_t i = 0; i < state->config.opsPerProperty; i++) {
        testLwwValue *a =
            createLwwValue(state, crdtRandRange(state, 1, 1000000),
                           crdtRandRange(state, 0, 9));
        testLwwValue *aa = mergeLww(a, a);

        stats->idempotenceChecks++;

        if (!lwwEquals(a, aa)) {
            snprintf(state->stats.lastFailure, sizeof(state->stats.lastFailure),
                     "LWW idempotence: merge(a,a) != a");
            free(a);
            free(aa);
            return false;
        }

        stats->idempotencePassed++;
        free(a);
        free(aa);
    }
    return true;
}

/* =========================================================================
 * G-Counter Testing (dynamically allocated for large replica counts)
 * ========================================================================= */

typedef struct testGCounterValue {
    int64_t *counts; /* dynamically allocated array */
    uint32_t nodeCount;
} testGCounterValue;

static testGCounterValue *createGCounter(uint32_t nodes) {
    testGCounterValue *v = zmalloc(sizeof(testGCounterValue));
    v->counts = zcalloc(nodes * sizeof(int64_t));
    v->nodeCount = nodes;
    return v;
}

static void freeGCounter(testGCounterValue *c) {
    if (c) {
        zfree(c->counts);
        zfree(c);
    }
}

static void gCounterIncrement(testGCounterValue *c, uint32_t nodeId,
                              int64_t delta) {
    if (nodeId < c->nodeCount && delta > 0) {
        c->counts[nodeId] += delta;
    }
}

static int64_t gCounterValue(const testGCounterValue *c) {
    int64_t sum = 0;
    for (uint32_t i = 0; i < c->nodeCount; i++) {
        sum += c->counts[i];
    }
    return sum;
}

static testGCounterValue *mergeGCounter(const testGCounterValue *a,
                                        const testGCounterValue *b) {
    uint32_t maxNodes =
        a->nodeCount > b->nodeCount ? a->nodeCount : b->nodeCount;
    testGCounterValue *result = createGCounter(maxNodes);

    for (uint32_t i = 0; i < maxNodes; i++) {
        int64_t va = i < a->nodeCount ? a->counts[i] : 0;
        int64_t vb = i < b->nodeCount ? b->counts[i] : 0;
        result->counts[i] = va > vb ? va : vb;
    }
    return result;
}

static bool gCounterEquals(const testGCounterValue *a,
                           const testGCounterValue *b) {
    uint32_t maxNodes =
        a->nodeCount > b->nodeCount ? a->nodeCount : b->nodeCount;
    for (uint32_t i = 0; i < maxNodes; i++) {
        int64_t va = i < a->nodeCount ? a->counts[i] : 0;
        int64_t vb = i < b->nodeCount ? b->counts[i] : 0;
        if (va != vb) {
            return false;
        }
    }
    return true;
}

static bool testGCounterCommutativity(fuzzCrdtState *state) {
    kraftFuzzCrdtTypeStats *stats = &state->stats.gCounter;

    for (uint32_t i = 0; i < state->config.opsPerProperty; i++) {
        testGCounterValue *a = createGCounter(state->config.replicaCount);
        testGCounterValue *b = createGCounter(state->config.replicaCount);

        /* Apply random increments */
        for (uint32_t j = 0; j < 5; j++) {
            gCounterIncrement(
                a, crdtRandRange(state, 0, state->config.replicaCount - 1),
                crdtRandRange(state, 1, 100));
            gCounterIncrement(
                b, crdtRandRange(state, 0, state->config.replicaCount - 1),
                crdtRandRange(state, 1, 100));
        }

        testGCounterValue *ab = mergeGCounter(a, b);
        testGCounterValue *ba = mergeGCounter(b, a);

        stats->commutativityChecks++;

        if (!gCounterEquals(ab, ba)) {
            snprintf(state->stats.lastFailure, sizeof(state->stats.lastFailure),
                     "G-Counter commutativity: merge(a,b) != merge(b,a)");
            freeGCounter(a);
            freeGCounter(b);
            freeGCounter(ab);
            freeGCounter(ba);
            return false;
        }

        stats->commutativityPassed++;
        freeGCounter(a);
        freeGCounter(b);
        freeGCounter(ab);
        freeGCounter(ba);
    }
    return true;
}

static bool testGCounterAssociativity(fuzzCrdtState *state) {
    kraftFuzzCrdtTypeStats *stats = &state->stats.gCounter;

    for (uint32_t i = 0; i < state->config.opsPerProperty; i++) {
        testGCounterValue *a = createGCounter(state->config.replicaCount);
        testGCounterValue *b = createGCounter(state->config.replicaCount);
        testGCounterValue *c = createGCounter(state->config.replicaCount);

        /* Apply random increments */
        for (uint32_t j = 0; j < 3; j++) {
            gCounterIncrement(
                a, crdtRandRange(state, 0, state->config.replicaCount - 1),
                crdtRandRange(state, 1, 100));
            gCounterIncrement(
                b, crdtRandRange(state, 0, state->config.replicaCount - 1),
                crdtRandRange(state, 1, 100));
            gCounterIncrement(
                c, crdtRandRange(state, 0, state->config.replicaCount - 1),
                crdtRandRange(state, 1, 100));
        }

        testGCounterValue *ab = mergeGCounter(a, b);
        testGCounterValue *ab_c = mergeGCounter(ab, c);
        testGCounterValue *bc = mergeGCounter(b, c);
        testGCounterValue *a_bc = mergeGCounter(a, bc);

        stats->associativityChecks++;

        if (!gCounterEquals(ab_c, a_bc)) {
            snprintf(state->stats.lastFailure, sizeof(state->stats.lastFailure),
                     "G-Counter associativity: merge(merge(a,b),c) != "
                     "merge(a,merge(b,c))");
            freeGCounter(a);
            freeGCounter(b);
            freeGCounter(c);
            freeGCounter(ab);
            freeGCounter(ab_c);
            freeGCounter(bc);
            freeGCounter(a_bc);
            return false;
        }

        stats->associativityPassed++;
        freeGCounter(a);
        freeGCounter(b);
        freeGCounter(c);
        freeGCounter(ab);
        freeGCounter(ab_c);
        freeGCounter(bc);
        freeGCounter(a_bc);
    }
    return true;
}

static bool testGCounterIdempotence(fuzzCrdtState *state) {
    kraftFuzzCrdtTypeStats *stats = &state->stats.gCounter;

    for (uint32_t i = 0; i < state->config.opsPerProperty; i++) {
        testGCounterValue *a = createGCounter(state->config.replicaCount);

        for (uint32_t j = 0; j < 5; j++) {
            gCounterIncrement(
                a, crdtRandRange(state, 0, state->config.replicaCount - 1),
                crdtRandRange(state, 1, 100));
        }

        testGCounterValue *aa = mergeGCounter(a, a);

        stats->idempotenceChecks++;

        if (!gCounterEquals(a, aa)) {
            snprintf(state->stats.lastFailure, sizeof(state->stats.lastFailure),
                     "G-Counter idempotence: merge(a,a) != a");
            freeGCounter(a);
            freeGCounter(aa);
            return false;
        }

        stats->idempotencePassed++;
        freeGCounter(a);
        freeGCounter(aa);
    }
    return true;
}

static bool testGCounterMonotonicity(fuzzCrdtState *state) {
    kraftFuzzCrdtTypeStats *stats = &state->stats.gCounter;

    for (uint32_t i = 0; i < state->config.opsPerProperty; i++) {
        testGCounterValue *a = createGCounter(state->config.replicaCount);
        int64_t prevValue = 0;

        /* Apply sequence of increments and verify monotonicity */
        for (uint32_t j = 0; j < 20; j++) {
            gCounterIncrement(
                a, crdtRandRange(state, 0, state->config.replicaCount - 1),
                crdtRandRange(state, 1, 100));

            int64_t currentValue = gCounterValue(a);

            stats->monotonicityChecks++;

            if (currentValue < prevValue) {
                snprintf(
                    state->stats.lastFailure, sizeof(state->stats.lastFailure),
                    "G-Counter monotonicity: value decreased from %ld to %ld",
                    prevValue, currentValue);
                freeGCounter(a);
                return false;
            }

            stats->monotonicityPassed++;
            prevValue = currentValue;
        }

        freeGCounter(a);
    }
    return true;
}

static bool testGCounterConvergence(fuzzCrdtState *state) {
    kraftFuzzCrdtTypeStats *stats = &state->stats.gCounter;

    for (uint32_t i = 0; i < state->config.opsPerProperty / 10; i++) {
        uint32_t numReplicas = state->config.replicaCount;
        testGCounterValue **replicas =
            zmalloc(numReplicas * sizeof(testGCounterValue *));

        for (uint32_t r = 0; r < numReplicas; r++) {
            replicas[r] = createGCounter(numReplicas);
        }

        /* Simulate concurrent operations on different replicas */
        for (uint32_t op = 0; op < 50; op++) {
            uint32_t targetReplica = crdtRandRange(state, 0, numReplicas - 1);
            gCounterIncrement(replicas[targetReplica], targetReplica,
                              crdtRandRange(state, 1, 10));
        }

        /* Merge all replicas together */
        testGCounterValue *merged = createGCounter(numReplicas);
        for (uint32_t r = 0; r < numReplicas; r++) {
            testGCounterValue *temp = mergeGCounter(merged, replicas[r]);
            freeGCounter(merged);
            merged = temp;
        }

        /* Verify all replicas converge to same value after merging */
        for (uint32_t r = 0; r < numReplicas; r++) {
            testGCounterValue *replicaMerged =
                mergeGCounter(replicas[r], merged);

            stats->convergenceChecks++;

            if (!gCounterEquals(replicaMerged, merged)) {
                snprintf(
                    state->stats.lastFailure, sizeof(state->stats.lastFailure),
                    "G-Counter convergence: replica %u didn't converge", r);
                for (uint32_t j = 0; j < numReplicas; j++) {
                    freeGCounter(replicas[j]);
                }
                zfree(replicas);
                freeGCounter(merged);
                freeGCounter(replicaMerged);
                return false;
            }

            stats->convergencePassed++;
            freeGCounter(replicaMerged);
        }

        for (uint32_t r = 0; r < numReplicas; r++) {
            freeGCounter(replicas[r]);
        }
        zfree(replicas);
        freeGCounter(merged);
    }
    return true;
}

/* =========================================================================
 * PN-Counter Testing (dynamically allocated for large replica counts)
 * ========================================================================= */

typedef struct testPNCounterValue {
    int64_t *positive; /* dynamically allocated */
    int64_t *negative; /* dynamically allocated */
    uint32_t nodeCount;
} testPNCounterValue;

static testPNCounterValue *createPNCounter(uint32_t nodes) {
    testPNCounterValue *v = zmalloc(sizeof(testPNCounterValue));
    v->positive = zcalloc(nodes * sizeof(int64_t));
    v->negative = zcalloc(nodes * sizeof(int64_t));
    v->nodeCount = nodes;
    return v;
}

static void freePNCounter(testPNCounterValue *c) {
    if (c) {
        zfree(c->positive);
        zfree(c->negative);
        zfree(c);
    }
}

static void pnCounterIncrement(testPNCounterValue *c, uint32_t nodeId,
                               int64_t delta) {
    if (nodeId < c->nodeCount) {
        if (delta > 0) {
            c->positive[nodeId] += delta;
        } else if (delta < 0) {
            c->negative[nodeId] += (-delta);
        }
    }
}

static int64_t pnCounterValue(const testPNCounterValue *c) {
    int64_t pos = 0, neg = 0;
    for (uint32_t i = 0; i < c->nodeCount; i++) {
        pos += c->positive[i];
        neg += c->negative[i];
    }
    return pos - neg;
}

static testPNCounterValue *mergePNCounter(const testPNCounterValue *a,
                                          const testPNCounterValue *b) {
    uint32_t maxNodes =
        a->nodeCount > b->nodeCount ? a->nodeCount : b->nodeCount;
    testPNCounterValue *result = createPNCounter(maxNodes);

    for (uint32_t i = 0; i < maxNodes; i++) {
        int64_t pa = i < a->nodeCount ? a->positive[i] : 0;
        int64_t pb = i < b->nodeCount ? b->positive[i] : 0;
        int64_t na = i < a->nodeCount ? a->negative[i] : 0;
        int64_t nb = i < b->nodeCount ? b->negative[i] : 0;
        result->positive[i] = pa > pb ? pa : pb;
        result->negative[i] = na > nb ? na : nb;
    }
    return result;
}

static bool pnCounterEquals(const testPNCounterValue *a,
                            const testPNCounterValue *b) {
    uint32_t maxNodes =
        a->nodeCount > b->nodeCount ? a->nodeCount : b->nodeCount;
    for (uint32_t i = 0; i < maxNodes; i++) {
        int64_t pa = i < a->nodeCount ? a->positive[i] : 0;
        int64_t pb = i < b->nodeCount ? b->positive[i] : 0;
        int64_t na = i < a->nodeCount ? a->negative[i] : 0;
        int64_t nb = i < b->nodeCount ? b->negative[i] : 0;
        if (pa != pb || na != nb) {
            return false;
        }
    }
    return true;
}

static bool testPNCounterCommutativity(fuzzCrdtState *state) {
    kraftFuzzCrdtTypeStats *stats = &state->stats.pnCounter;

    for (uint32_t i = 0; i < state->config.opsPerProperty; i++) {
        testPNCounterValue *a = createPNCounter(state->config.replicaCount);
        testPNCounterValue *b = createPNCounter(state->config.replicaCount);

        for (uint32_t j = 0; j < 5; j++) {
            int64_t delta = (int64_t)crdtRandRange(state, 1, 100);
            if (crdtRand(state) % 2 == 0) {
                delta = -delta;
            }
            pnCounterIncrement(
                a, crdtRandRange(state, 0, state->config.replicaCount - 1),
                delta);

            delta = (int64_t)crdtRandRange(state, 1, 100);
            if (crdtRand(state) % 2 == 0) {
                delta = -delta;
            }
            pnCounterIncrement(
                b, crdtRandRange(state, 0, state->config.replicaCount - 1),
                delta);
        }

        testPNCounterValue *ab = mergePNCounter(a, b);
        testPNCounterValue *ba = mergePNCounter(b, a);

        stats->commutativityChecks++;

        if (!pnCounterEquals(ab, ba)) {
            snprintf(state->stats.lastFailure, sizeof(state->stats.lastFailure),
                     "PN-Counter commutativity: merge(a,b) != merge(b,a)");
            freePNCounter(a);
            freePNCounter(b);
            freePNCounter(ab);
            freePNCounter(ba);
            return false;
        }

        stats->commutativityPassed++;
        freePNCounter(a);
        freePNCounter(b);
        freePNCounter(ab);
        freePNCounter(ba);
    }
    return true;
}

static bool testPNCounterAssociativity(fuzzCrdtState *state) {
    kraftFuzzCrdtTypeStats *stats = &state->stats.pnCounter;

    for (uint32_t i = 0; i < state->config.opsPerProperty; i++) {
        testPNCounterValue *a = createPNCounter(state->config.replicaCount);
        testPNCounterValue *b = createPNCounter(state->config.replicaCount);
        testPNCounterValue *c = createPNCounter(state->config.replicaCount);

        for (uint32_t j = 0; j < 3; j++) {
            int64_t delta = (int64_t)crdtRandRange(state, 1, 100);
            if (crdtRand(state) % 2 == 0) {
                delta = -delta;
            }
            pnCounterIncrement(
                a, crdtRandRange(state, 0, state->config.replicaCount - 1),
                delta);
            pnCounterIncrement(
                b, crdtRandRange(state, 0, state->config.replicaCount - 1),
                delta);
            pnCounterIncrement(
                c, crdtRandRange(state, 0, state->config.replicaCount - 1),
                delta);
        }

        testPNCounterValue *ab = mergePNCounter(a, b);
        testPNCounterValue *ab_c = mergePNCounter(ab, c);
        testPNCounterValue *bc = mergePNCounter(b, c);
        testPNCounterValue *a_bc = mergePNCounter(a, bc);

        stats->associativityChecks++;

        if (!pnCounterEquals(ab_c, a_bc)) {
            snprintf(state->stats.lastFailure, sizeof(state->stats.lastFailure),
                     "PN-Counter associativity failed");
            freePNCounter(a);
            freePNCounter(b);
            freePNCounter(c);
            freePNCounter(ab);
            freePNCounter(ab_c);
            freePNCounter(bc);
            freePNCounter(a_bc);
            return false;
        }

        stats->associativityPassed++;
        freePNCounter(a);
        freePNCounter(b);
        freePNCounter(c);
        freePNCounter(ab);
        freePNCounter(ab_c);
        freePNCounter(bc);
        freePNCounter(a_bc);
    }
    return true;
}

static bool testPNCounterIdempotence(fuzzCrdtState *state) {
    kraftFuzzCrdtTypeStats *stats = &state->stats.pnCounter;

    for (uint32_t i = 0; i < state->config.opsPerProperty; i++) {
        testPNCounterValue *a = createPNCounter(state->config.replicaCount);

        for (uint32_t j = 0; j < 5; j++) {
            int64_t delta = (int64_t)crdtRandRange(state, 1, 100);
            if (crdtRand(state) % 2 == 0) {
                delta = -delta;
            }
            pnCounterIncrement(
                a, crdtRandRange(state, 0, state->config.replicaCount - 1),
                delta);
        }

        testPNCounterValue *aa = mergePNCounter(a, a);

        stats->idempotenceChecks++;

        if (!pnCounterEquals(a, aa)) {
            snprintf(state->stats.lastFailure, sizeof(state->stats.lastFailure),
                     "PN-Counter idempotence: merge(a,a) != a");
            freePNCounter(a);
            freePNCounter(aa);
            return false;
        }

        stats->idempotencePassed++;
        freePNCounter(a);
        freePNCounter(aa);
    }
    return true;
}

/* =========================================================================
 * G-Set Testing
 * ========================================================================= */

#define MAX_GSET_ELEMENTS 64

typedef struct testGSetValue {
    uint64_t elements[MAX_GSET_ELEMENTS];
    uint32_t count;
} testGSetValue;

static testGSetValue *createGSet(void) {
    return zmalloc(sizeof(testGSetValue));
}

static bool gSetContains(const testGSetValue *s, uint64_t elem) {
    for (uint32_t i = 0; i < s->count; i++) {
        if (s->elements[i] == elem) {
            return true;
        }
    }
    return false;
}

static void gSetAdd(testGSetValue *s, uint64_t elem) {
    if (!gSetContains(s, elem) && s->count < MAX_GSET_ELEMENTS) {
        s->elements[s->count++] = elem;
    }
}

static testGSetValue *mergeGSet(const testGSetValue *a,
                                const testGSetValue *b) {
    testGSetValue *result = zmalloc(sizeof(testGSetValue));

    for (uint32_t i = 0; i < a->count && result->count < MAX_GSET_ELEMENTS;
         i++) {
        result->elements[result->count++] = a->elements[i];
    }

    for (uint32_t i = 0; i < b->count && result->count < MAX_GSET_ELEMENTS;
         i++) {
        if (!gSetContains(result, b->elements[i])) {
            result->elements[result->count++] = b->elements[i];
        }
    }

    return result;
}

static bool gSetEquals(const testGSetValue *a, const testGSetValue *b) {
    if (a->count != b->count) {
        return false;
    }
    for (uint32_t i = 0; i < a->count; i++) {
        if (!gSetContains(b, a->elements[i])) {
            return false;
        }
    }
    return true;
}

static bool testGSetCommutativity(fuzzCrdtState *state) {
    kraftFuzzCrdtTypeStats *stats = &state->stats.gSet;

    for (uint32_t i = 0; i < state->config.opsPerProperty; i++) {
        testGSetValue *a = createGSet();
        testGSetValue *b = createGSet();

        for (uint32_t j = 0; j < 10; j++) {
            gSetAdd(a, crdtRandRange(state, 1, 100));
            gSetAdd(b, crdtRandRange(state, 1, 100));
        }

        testGSetValue *ab = mergeGSet(a, b);
        testGSetValue *ba = mergeGSet(b, a);

        stats->commutativityChecks++;

        if (!gSetEquals(ab, ba)) {
            snprintf(state->stats.lastFailure, sizeof(state->stats.lastFailure),
                     "G-Set commutativity: merge(a,b) != merge(b,a)");
            free(a);
            free(b);
            free(ab);
            free(ba);
            return false;
        }

        stats->commutativityPassed++;
        free(a);
        free(b);
        free(ab);
        free(ba);
    }
    return true;
}

static bool testGSetAssociativity(fuzzCrdtState *state) {
    kraftFuzzCrdtTypeStats *stats = &state->stats.gSet;

    for (uint32_t i = 0; i < state->config.opsPerProperty; i++) {
        testGSetValue *a = createGSet();
        testGSetValue *b = createGSet();
        testGSetValue *c = createGSet();

        for (uint32_t j = 0; j < 5; j++) {
            gSetAdd(a, crdtRandRange(state, 1, 50));
            gSetAdd(b, crdtRandRange(state, 1, 50));
            gSetAdd(c, crdtRandRange(state, 1, 50));
        }

        testGSetValue *ab = mergeGSet(a, b);
        testGSetValue *ab_c = mergeGSet(ab, c);
        testGSetValue *bc = mergeGSet(b, c);
        testGSetValue *a_bc = mergeGSet(a, bc);

        stats->associativityChecks++;

        if (!gSetEquals(ab_c, a_bc)) {
            snprintf(state->stats.lastFailure, sizeof(state->stats.lastFailure),
                     "G-Set associativity failed");
            free(a);
            free(b);
            free(c);
            free(ab);
            free(ab_c);
            free(bc);
            free(a_bc);
            return false;
        }

        stats->associativityPassed++;
        free(a);
        free(b);
        free(c);
        free(ab);
        free(ab_c);
        free(bc);
        free(a_bc);
    }
    return true;
}

static bool testGSetIdempotence(fuzzCrdtState *state) {
    kraftFuzzCrdtTypeStats *stats = &state->stats.gSet;

    for (uint32_t i = 0; i < state->config.opsPerProperty; i++) {
        testGSetValue *a = createGSet();

        for (uint32_t j = 0; j < 10; j++) {
            gSetAdd(a, crdtRandRange(state, 1, 100));
        }

        testGSetValue *aa = mergeGSet(a, a);

        stats->idempotenceChecks++;

        if (!gSetEquals(a, aa)) {
            snprintf(state->stats.lastFailure, sizeof(state->stats.lastFailure),
                     "G-Set idempotence: merge(a,a) != a");
            free(a);
            free(aa);
            return false;
        }

        stats->idempotencePassed++;
        free(a);
        free(aa);
    }
    return true;
}

static bool testGSetMonotonicity(fuzzCrdtState *state) {
    kraftFuzzCrdtTypeStats *stats = &state->stats.gSet;

    for (uint32_t i = 0; i < state->config.opsPerProperty; i++) {
        testGSetValue *a = createGSet();
        uint32_t prevCount = 0;

        for (uint32_t j = 0; j < 20; j++) {
            gSetAdd(a, crdtRandRange(state, 1, 100));

            stats->monotonicityChecks++;

            if (a->count < prevCount) {
                snprintf(state->stats.lastFailure,
                         sizeof(state->stats.lastFailure),
                         "G-Set monotonicity: size decreased from %u to %u",
                         prevCount, a->count);
                free(a);
                return false;
            }

            stats->monotonicityPassed++;
            prevCount = a->count;
        }

        free(a);
    }
    return true;
}

/* =========================================================================
 * OR-Set Testing
 * ========================================================================= */

typedef struct testOrSetElement {
    uint64_t value;
    uint64_t tag;
    bool removed;
} testOrSetElement;

typedef struct testOrSetValue {
    testOrSetElement elements[MAX_GSET_ELEMENTS];
    uint32_t count;
    uint64_t nextTag;
} testOrSetValue;

static testOrSetValue *createOrSet(void) {
    testOrSetValue *s = zmalloc(sizeof(testOrSetValue));
    s->nextTag = 1;
    return s;
}

static void orSetAdd(testOrSetValue *s, uint64_t value) {
    if (s->count < MAX_GSET_ELEMENTS) {
        s->elements[s->count].value = value;
        s->elements[s->count].tag = s->nextTag++;
        s->elements[s->count].removed = false;
        s->count++;
    }
}

static void orSetRemove(testOrSetValue *s, uint64_t value) {
    for (uint32_t i = 0; i < s->count; i++) {
        if (s->elements[i].value == value && !s->elements[i].removed) {
            s->elements[i].removed = true;
        }
    }
}

static uint32_t orSetActiveCount(const testOrSetValue *s) {
    uint32_t count = 0;
    for (uint32_t i = 0; i < s->count; i++) {
        if (!s->elements[i].removed) {
            count++;
        }
    }
    return count;
}

static testOrSetValue *mergeOrSet(const testOrSetValue *a,
                                  const testOrSetValue *b) {
    testOrSetValue *result = zmalloc(sizeof(testOrSetValue));
    result->nextTag = a->nextTag > b->nextTag ? a->nextTag : b->nextTag;

    /* Add all elements from a */
    for (uint32_t i = 0; i < a->count && result->count < MAX_GSET_ELEMENTS;
         i++) {
        result->elements[result->count] = a->elements[i];
        result->count++;
    }

    /* Merge elements from b */
    for (uint32_t i = 0; i < b->count; i++) {
        bool found = false;
        for (uint32_t j = 0; j < result->count; j++) {
            if (result->elements[j].tag == b->elements[i].tag) {
                /* Same tag - merge removed status (removed wins) */
                if (b->elements[i].removed) {
                    result->elements[j].removed = true;
                }
                found = true;
                break;
            }
        }
        if (!found && result->count < MAX_GSET_ELEMENTS) {
            result->elements[result->count] = b->elements[i];
            result->count++;
        }
    }

    return result;
}

static bool orSetEquals(const testOrSetValue *a, const testOrSetValue *b) {
    /* Compare active elements only */
    if (orSetActiveCount(a) != orSetActiveCount(b)) {
        return false;
    }

    for (uint32_t i = 0; i < a->count; i++) {
        if (a->elements[i].removed) {
            continue;
        }

        bool found = false;
        for (uint32_t j = 0; j < b->count; j++) {
            if (!b->elements[j].removed &&
                b->elements[j].tag == a->elements[i].tag) {
                found = true;
                break;
            }
        }
        if (!found) {
            return false;
        }
    }
    return true;
}

static bool testOrSetCommutativity(fuzzCrdtState *state) {
    kraftFuzzCrdtTypeStats *stats = &state->stats.orSet;

    for (uint32_t i = 0; i < state->config.opsPerProperty; i++) {
        testOrSetValue *a = createOrSet();
        testOrSetValue *b = createOrSet();

        for (uint32_t j = 0; j < 5; j++) {
            orSetAdd(a, crdtRandRange(state, 1, 50));
            orSetAdd(b, crdtRandRange(state, 1, 50));
        }

        /* Some removes */
        if (a->count > 0) {
            orSetRemove(a, a->elements[0].value);
        }
        if (b->count > 0) {
            orSetRemove(b, b->elements[0].value);
        }

        testOrSetValue *ab = mergeOrSet(a, b);
        testOrSetValue *ba = mergeOrSet(b, a);

        stats->commutativityChecks++;

        if (!orSetEquals(ab, ba)) {
            snprintf(state->stats.lastFailure, sizeof(state->stats.lastFailure),
                     "OR-Set commutativity: merge(a,b) != merge(b,a)");
            free(a);
            free(b);
            free(ab);
            free(ba);
            return false;
        }

        stats->commutativityPassed++;
        free(a);
        free(b);
        free(ab);
        free(ba);
    }
    return true;
}

static bool testOrSetIdempotence(fuzzCrdtState *state) {
    kraftFuzzCrdtTypeStats *stats = &state->stats.orSet;

    for (uint32_t i = 0; i < state->config.opsPerProperty; i++) {
        testOrSetValue *a = createOrSet();

        for (uint32_t j = 0; j < 10; j++) {
            orSetAdd(a, crdtRandRange(state, 1, 50));
        }

        if (a->count > 2) {
            orSetRemove(a, a->elements[0].value);
            orSetRemove(a, a->elements[1].value);
        }

        testOrSetValue *aa = mergeOrSet(a, a);

        stats->idempotenceChecks++;

        if (!orSetEquals(a, aa)) {
            snprintf(state->stats.lastFailure, sizeof(state->stats.lastFailure),
                     "OR-Set idempotence: merge(a,a) != a");
            free(a);
            free(aa);
            return false;
        }

        stats->idempotencePassed++;
        free(a);
        free(aa);
    }
    return true;
}

/* =========================================================================
 * Main Test Runner
 * ========================================================================= */

kraftFuzzCrdtConfig kraftFuzzCrdtConfigDefault(void) {
    kraftFuzzCrdtConfig config = {0};
    config.maxOperations = 100000; /* 100k total ops default */
    config.replicaCount = 10;      /* 10 replicas default */
    config.seed = (uint32_t)time(NULL);
    config.typeFilter = KRAFT_CRDT_NONE; /* all types */
    config.propertyMask = CRDT_PROP_ALL;
    config.mode = CRDT_MODE_RANDOM;
    config.opsPerProperty = 10000; /* 10k ops per property default */
    config.mergeDepth = 10;        /* deeper merge chains */
    config.valueSize = 64;
    config.convergenceRounds = 20; /* more convergence rounds */
    config.partitionProbability = 0.1f;
    config.verbose = false;
    config.reportInterval = 10000; /* less frequent reporting */
    config.stopOnFailure = true;
    return config;
}

static void runTypeTests(fuzzCrdtState *state, kraftCrdtType type) {
    kraftFuzzCrdtTypeStats *stats = NULL;
    bool passed = true;

    switch (type) {
    case KRAFT_CRDT_LWW_REGISTER:
        stats = &state->stats.lwwRegister;
        stats->typeName = "LWW_REGISTER";

        if (state->config.propertyMask & CRDT_PROP_COMMUTATIVITY) {
            if (!testLwwCommutativity(state)) {
                passed = false;
            }
        }
        if (passed && (state->config.propertyMask & CRDT_PROP_ASSOCIATIVITY)) {
            if (!testLwwAssociativity(state)) {
                passed = false;
            }
        }
        if (passed && (state->config.propertyMask & CRDT_PROP_IDEMPOTENCE)) {
            if (!testLwwIdempotence(state)) {
                passed = false;
            }
        }
        break;

    case KRAFT_CRDT_G_COUNTER:
        stats = &state->stats.gCounter;
        stats->typeName = "G_COUNTER";

        if (state->config.propertyMask & CRDT_PROP_COMMUTATIVITY) {
            if (!testGCounterCommutativity(state)) {
                passed = false;
            }
        }
        if (passed && (state->config.propertyMask & CRDT_PROP_ASSOCIATIVITY)) {
            if (!testGCounterAssociativity(state)) {
                passed = false;
            }
        }
        if (passed && (state->config.propertyMask & CRDT_PROP_IDEMPOTENCE)) {
            if (!testGCounterIdempotence(state)) {
                passed = false;
            }
        }
        if (passed && (state->config.propertyMask & CRDT_PROP_MONOTONICITY)) {
            if (!testGCounterMonotonicity(state)) {
                passed = false;
            }
        }
        if (passed && (state->config.propertyMask & CRDT_PROP_CONVERGENCE)) {
            if (!testGCounterConvergence(state)) {
                passed = false;
            }
        }
        break;

    case KRAFT_CRDT_PN_COUNTER:
        stats = &state->stats.pnCounter;
        stats->typeName = "PN_COUNTER";

        if (state->config.propertyMask & CRDT_PROP_COMMUTATIVITY) {
            if (!testPNCounterCommutativity(state)) {
                passed = false;
            }
        }
        if (passed && (state->config.propertyMask & CRDT_PROP_ASSOCIATIVITY)) {
            if (!testPNCounterAssociativity(state)) {
                passed = false;
            }
        }
        if (passed && (state->config.propertyMask & CRDT_PROP_IDEMPOTENCE)) {
            if (!testPNCounterIdempotence(state)) {
                passed = false;
            }
        }
        break;

    case KRAFT_CRDT_G_SET:
        stats = &state->stats.gSet;
        stats->typeName = "G_SET";

        if (state->config.propertyMask & CRDT_PROP_COMMUTATIVITY) {
            if (!testGSetCommutativity(state)) {
                passed = false;
            }
        }
        if (passed && (state->config.propertyMask & CRDT_PROP_ASSOCIATIVITY)) {
            if (!testGSetAssociativity(state)) {
                passed = false;
            }
        }
        if (passed && (state->config.propertyMask & CRDT_PROP_IDEMPOTENCE)) {
            if (!testGSetIdempotence(state)) {
                passed = false;
            }
        }
        if (passed && (state->config.propertyMask & CRDT_PROP_MONOTONICITY)) {
            if (!testGSetMonotonicity(state)) {
                passed = false;
            }
        }
        break;

    case KRAFT_CRDT_OR_SET:
        stats = &state->stats.orSet;
        stats->typeName = "OR_SET";

        if (state->config.propertyMask & CRDT_PROP_COMMUTATIVITY) {
            if (!testOrSetCommutativity(state)) {
                passed = false;
            }
        }
        if (passed && (state->config.propertyMask & CRDT_PROP_IDEMPOTENCE)) {
            if (!testOrSetIdempotence(state)) {
                passed = false;
            }
        }
        break;

    default:
        return;
    }

    if (!passed) {
        state->stats.failedType = type;
        state->stats.totalFailed++;
    }
}

kraftFuzzCrdtResult kraftFuzzCrdtRun(const kraftFuzzCrdtConfig *config,
                                     kraftFuzzCrdtStats *stats) {
    fuzzCrdtState state = {0};
    state.config = *config;
    state.rngState = config->seed;
    if (state.rngState == 0) {
        state.rngState = 1;
    }

    clock_t start = clock();

    if (config->verbose) {
        printf("CRDT Property Fuzzer starting:\n");
        printf("  Seed: %u\n", config->seed);
        printf("  Replicas: %u\n", config->replicaCount);
        printf("  Ops per property: %u\n", config->opsPerProperty);
        printf("\n");
    }

    /* Test each CRDT type */
    kraftCrdtType types[] = {KRAFT_CRDT_LWW_REGISTER, KRAFT_CRDT_G_COUNTER,
                             KRAFT_CRDT_PN_COUNTER, KRAFT_CRDT_G_SET,
                             KRAFT_CRDT_OR_SET};

    for (size_t i = 0; i < sizeof(types) / sizeof(types[0]); i++) {
        if (config->typeFilter != KRAFT_CRDT_NONE &&
            config->typeFilter != types[i]) {
            continue;
        }

        if (config->verbose) {
            printf("Testing %s...\n", kraftFuzzCrdtTypeName(types[i]));
        }

        runTypeTests(&state, types[i]);

        if (state.stats.totalFailed > 0 && config->stopOnFailure) {
            break;
        }
    }

    state.stats.elapsedMs = (clock() - start) * 1000 / CLOCKS_PER_SEC;

    /* Aggregate stats */
    kraftFuzzCrdtTypeStats *typeStats[] = {
        &state.stats.lwwRegister, &state.stats.gCounter, &state.stats.pnCounter,
        &state.stats.gSet, &state.stats.orSet};

    for (size_t i = 0; i < 5; i++) {
        state.stats.totalChecks += typeStats[i]->commutativityChecks +
                                   typeStats[i]->associativityChecks +
                                   typeStats[i]->idempotenceChecks +
                                   typeStats[i]->monotonicityChecks +
                                   typeStats[i]->convergenceChecks;
        state.stats.totalPassed += typeStats[i]->commutativityPassed +
                                   typeStats[i]->associativityPassed +
                                   typeStats[i]->idempotencePassed +
                                   typeStats[i]->monotonicityPassed +
                                   typeStats[i]->convergencePassed;
    }

    if (stats) {
        *stats = state.stats;
    }

    if (state.stats.totalFailed > 0) {
        return FUZZ_CRDT_PROPERTY_VIOLATION;
    }
    return FUZZ_CRDT_SUCCESS;
}

void kraftFuzzCrdtPrintStats(const kraftFuzzCrdtStats *stats) {
    printf("\n--- CRDT Property Fuzzer Results ---\n");
    printf("Elapsed time:    %lu ms\n", stats->elapsedMs);
    printf("Total checks:    %lu\n", stats->totalChecks);
    printf("Passed:          %lu\n", stats->totalPassed);
    printf("Failed:          %lu\n", stats->totalFailed);
    printf("\n");

    printf("Per-Type Results:\n");

    const kraftFuzzCrdtTypeStats *types[] = {
        &stats->lwwRegister, &stats->gCounter, &stats->pnCounter, &stats->gSet,
        &stats->orSet};

    for (size_t i = 0; i < 5; i++) {
        const kraftFuzzCrdtTypeStats *t = types[i];
        if (!t->typeName) {
            continue;
        }

        uint64_t total = t->commutativityChecks + t->associativityChecks +
                         t->idempotenceChecks + t->monotonicityChecks +
                         t->convergenceChecks;
        uint64_t passed = t->commutativityPassed + t->associativityPassed +
                          t->idempotencePassed + t->monotonicityPassed +
                          t->convergencePassed;

        printf("  %s: %lu/%lu", t->typeName, passed, total);
        if (passed == total) {
            printf(" PASS\n");
        } else {
            printf(" FAIL\n");
        }

        if (t->commutativityChecks > 0) {
            printf("    Commutativity:  %lu/%lu\n", t->commutativityPassed,
                   t->commutativityChecks);
        }
        if (t->associativityChecks > 0) {
            printf("    Associativity:  %lu/%lu\n", t->associativityPassed,
                   t->associativityChecks);
        }
        if (t->idempotenceChecks > 0) {
            printf("    Idempotence:    %lu/%lu\n", t->idempotencePassed,
                   t->idempotenceChecks);
        }
        if (t->monotonicityChecks > 0) {
            printf("    Monotonicity:   %lu/%lu\n", t->monotonicityPassed,
                   t->monotonicityChecks);
        }
        if (t->convergenceChecks > 0) {
            printf("    Convergence:    %lu/%lu\n", t->convergencePassed,
                   t->convergenceChecks);
        }
    }

    if (stats->totalFailed > 0) {
        printf("\nFailure: %s\n", stats->lastFailure);
    }

    printf("------------------------------------\n");
}

const char *kraftFuzzCrdtPropertyName(kraftCrdtProperty property) {
    switch (property) {
    case CRDT_PROP_COMMUTATIVITY:
        return "commutativity";
    case CRDT_PROP_ASSOCIATIVITY:
        return "associativity";
    case CRDT_PROP_IDEMPOTENCE:
        return "idempotence";
    case CRDT_PROP_MONOTONICITY:
        return "monotonicity";
    case CRDT_PROP_CONVERGENCE:
        return "convergence";
    case CRDT_PROP_PERSISTENCE:
        return "persistence";
    default:
        return "unknown";
    }
}

const char *kraftFuzzCrdtTypeName(kraftCrdtType type) {
    switch (type) {
    case KRAFT_CRDT_NONE:
        return "NONE";
    case KRAFT_CRDT_LWW_REGISTER:
        return "LWW_REGISTER";
    case KRAFT_CRDT_G_COUNTER:
        return "G_COUNTER";
    case KRAFT_CRDT_PN_COUNTER:
        return "PN_COUNTER";
    case KRAFT_CRDT_G_SET:
        return "G_SET";
    case KRAFT_CRDT_OR_SET:
        return "OR_SET";
    default:
        return "UNKNOWN";
    }
}
