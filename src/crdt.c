/* crdt.c - Kraft CRDT-Aware Log Compaction Implementation
 *
 * Implements CRDT types and intelligent log compaction that understands
 * CRDT semantics for merging redundant entries.
 *
 * Built-in types:
 *   - LWW Register: Last-Writer-Wins based on timestamp
 *   - G-Counter: Grow-only counter with per-node tracking
 *   - PN-Counter: Positive-Negative counter
 *   - G-Set: Grow-only set
 *   - OR-Set: Observed-Remove set with unique tags
 */

#include "crdt.h"
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* =========================================================================
 * Memory Helpers
 * ========================================================================= */

static inline void *crdtAlloc(size_t size) {
    return calloc(1, size);
}

static inline void *crdtRealloc(void *ptr, size_t size) {
    return realloc(ptr, size);
}

static inline void crdtFree(void *ptr) {
    free(ptr);
}

static inline char *crdtStrdup(const char *s) {
    if (!s) {
        return NULL;
    }
    size_t len = strlen(s) + 1;
    char *copy = crdtAlloc(len);
    if (copy) {
        memcpy(copy, s, len);
    }
    return copy;
}

/* =========================================================================
 * Timestamp Helpers
 * ========================================================================= */

static int timestampCompare(const kraftCrdtTimestamp *a,
                            const kraftCrdtTimestamp *b) {
    /* Compare wall clock first */
    if (a->wallClock != b->wallClock) {
        return (a->wallClock > b->wallClock) ? 1 : -1;
    }
    /* Then logical clock */
    if (a->logicalClock != b->logicalClock) {
        return (a->logicalClock > b->logicalClock) ? 1 : -1;
    }
    /* Finally node ID as tiebreaker */
    if (a->nodeId != b->nodeId) {
        return (a->nodeId > b->nodeId) ? 1 : -1;
    }
    return 0;
}

/* =========================================================================
 * LWW Register Implementation
 * ========================================================================= */

kraftCrdtLwwValue *kraftCrdtLwwNew(const void *data, size_t dataLen,
                                   uint32_t nodeId, uint64_t timestamp) {
    kraftCrdtLwwValue *value = crdtAlloc(sizeof(*value));
    if (!value) {
        return NULL;
    }

    if (data && dataLen > 0) {
        value->data = crdtAlloc(dataLen);
        if (!value->data) {
            crdtFree(value);
            return NULL;
        }
        memcpy(value->data, data, dataLen);
        value->dataLen = dataLen;
    }

    value->timestamp.wallClock = timestamp;
    value->timestamp.logicalClock = 0;
    value->timestamp.nodeId = nodeId;

    return value;
}

void kraftCrdtLwwFree(kraftCrdtLwwValue *value) {
    if (!value) {
        return;
    }
    crdtFree(value->data);
    crdtFree(value);
}

kraftCrdtLwwValue *kraftCrdtLwwMerge(const kraftCrdtLwwValue *a,
                                     const kraftCrdtLwwValue *b) {
    if (!a && !b) {
        return NULL;
    }
    if (!a) {
        return kraftCrdtLwwNew(b->data, b->dataLen, b->timestamp.nodeId,
                               b->timestamp.wallClock);
    }
    if (!b) {
        return kraftCrdtLwwNew(a->data, a->dataLen, a->timestamp.nodeId,
                               a->timestamp.wallClock);
    }

    /* Return copy of winner */
    const kraftCrdtLwwValue *winner =
        (timestampCompare(&a->timestamp, &b->timestamp) >= 0) ? a : b;
    kraftCrdtLwwValue *result =
        kraftCrdtLwwNew(winner->data, winner->dataLen, winner->timestamp.nodeId,
                        winner->timestamp.wallClock);
    if (result) {
        result->timestamp.logicalClock = winner->timestamp.logicalClock;
    }
    return result;
}

/* LWW Type Operations */
static void *lwwMerge(const void *a, const void *b) {
    return kraftCrdtLwwMerge((const kraftCrdtLwwValue *)a,
                             (const kraftCrdtLwwValue *)b);
}

static void *lwwApply(const void *current, kraftCrdtOp op, const void *opData,
                      size_t opDataLen, uint32_t nodeId, uint64_t timestamp) {
    if (op != KRAFT_CRDT_OP_SET) {
        return NULL;
    }

    kraftCrdtLwwValue *newVal =
        kraftCrdtLwwNew(opData, opDataLen, nodeId, timestamp);
    if (!current) {
        return newVal;
    }

    kraftCrdtLwwValue *merged =
        kraftCrdtLwwMerge((const kraftCrdtLwwValue *)current, newVal);
    kraftCrdtLwwFree(newVal);
    return merged;
}

static bool lwwEquals(const void *a, const void *b) {
    if (!a || !b) {
        return a == b;
    }
    const kraftCrdtLwwValue *va = (const kraftCrdtLwwValue *)a;
    const kraftCrdtLwwValue *vb = (const kraftCrdtLwwValue *)b;
    if (va->dataLen != vb->dataLen) {
        return false;
    }
    return memcmp(va->data, vb->data, va->dataLen) == 0;
}

static void lwwFree(void *value) {
    kraftCrdtLwwFree((kraftCrdtLwwValue *)value);
}

static uint32_t lwwCanCompact(const kraftCrdtOp *ops, uint32_t opCount) {
    /* LWW: all SETs can be compacted to just the last one */
    if (opCount <= 1) {
        return 0;
    }
    uint32_t setCount = 0;
    for (uint32_t i = 0; i < opCount; i++) {
        if (ops[i] == KRAFT_CRDT_OP_SET) {
            setCount++;
        }
    }
    return (setCount > 1) ? setCount - 1 : 0;
}

/* =========================================================================
 * G-Counter Implementation
 * ========================================================================= */

kraftCrdtCounterValue *kraftCrdtGCounterNew(void) {
    kraftCrdtCounterValue *value = crdtAlloc(sizeof(*value));
    if (!value) {
        return NULL;
    }

    value->entryCapacity = 8;
    value->entries =
        crdtAlloc(value->entryCapacity * sizeof(kraftCrdtCounterEntry));
    if (!value->entries) {
        crdtFree(value);
        return NULL;
    }
    return value;
}

void kraftCrdtCounterFree(kraftCrdtCounterValue *value) {
    if (!value) {
        return;
    }
    crdtFree(value->entries);
    crdtFree(value);
}

static kraftCrdtCounterEntry *counterFindNode(kraftCrdtCounterValue *value,
                                              uint32_t nodeId) {
    for (uint32_t i = 0; i < value->entryCount; i++) {
        if (value->entries[i].nodeId == nodeId) {
            return &value->entries[i];
        }
    }
    return NULL;
}

static bool counterEnsureCapacity(kraftCrdtCounterValue *value) {
    if (value->entryCount < value->entryCapacity) {
        return true;
    }

    uint32_t newCap = value->entryCapacity * 2;
    kraftCrdtCounterEntry *newEntries =
        crdtRealloc(value->entries, newCap * sizeof(kraftCrdtCounterEntry));
    if (!newEntries) {
        return false;
    }

    value->entries = newEntries;
    value->entryCapacity = newCap;
    return true;
}

void kraftCrdtGCounterIncrement(kraftCrdtCounterValue *value, uint32_t nodeId,
                                int64_t amount) {
    if (!value || amount < 0) {
        return; /* G-Counter only grows */
    }

    kraftCrdtCounterEntry *entry = counterFindNode(value, nodeId);
    if (entry) {
        entry->value += amount;
    } else {
        if (!counterEnsureCapacity(value)) {
            return;
        }
        value->entries[value->entryCount].nodeId = nodeId;
        value->entries[value->entryCount].value = amount;
        value->entryCount++;
    }
}

int64_t kraftCrdtGCounterTotal(const kraftCrdtCounterValue *value) {
    if (!value) {
        return 0;
    }
    int64_t total = 0;
    for (uint32_t i = 0; i < value->entryCount; i++) {
        total += value->entries[i].value;
    }
    return total;
}

kraftCrdtCounterValue *kraftCrdtGCounterMerge(const kraftCrdtCounterValue *a,
                                              const kraftCrdtCounterValue *b) {
    kraftCrdtCounterValue *result = kraftCrdtGCounterNew();
    if (!result) {
        return NULL;
    }

    /* Add entries from a */
    if (a) {
        for (uint32_t i = 0; i < a->entryCount; i++) {
            kraftCrdtGCounterIncrement(result, a->entries[i].nodeId,
                                       a->entries[i].value);
        }
    }

    /* Merge entries from b (taking max per node) */
    if (b) {
        for (uint32_t i = 0; i < b->entryCount; i++) {
            kraftCrdtCounterEntry *existing =
                counterFindNode(result, b->entries[i].nodeId);
            if (existing) {
                if (b->entries[i].value > existing->value) {
                    existing->value = b->entries[i].value;
                }
            } else {
                kraftCrdtGCounterIncrement(result, b->entries[i].nodeId,
                                           b->entries[i].value);
            }
        }
    }

    return result;
}

/* G-Counter Type Operations */
static void *gcounterMerge(const void *a, const void *b) {
    return kraftCrdtGCounterMerge((const kraftCrdtCounterValue *)a,
                                  (const kraftCrdtCounterValue *)b);
}

static void *gcounterApply(const void *current, kraftCrdtOp op,
                           const void *opData, size_t opDataLen,
                           uint32_t nodeId, uint64_t timestamp) {
    (void)timestamp;
    if (op != KRAFT_CRDT_OP_INC) {
        return NULL;
    }

    int64_t amount = 1;
    if (opData && opDataLen >= sizeof(int64_t)) {
        amount = *(const int64_t *)opData;
    }

    kraftCrdtCounterValue *result;
    if (current) {
        result = kraftCrdtGCounterMerge((const kraftCrdtCounterValue *)current,
                                        NULL);
    } else {
        result = kraftCrdtGCounterNew();
    }
    if (result) {
        kraftCrdtGCounterIncrement(result, nodeId, amount);
    }
    return result;
}

static bool gcounterEquals(const void *a, const void *b) {
    return kraftCrdtGCounterTotal((const kraftCrdtCounterValue *)a) ==
           kraftCrdtGCounterTotal((const kraftCrdtCounterValue *)b);
}

static void gcounterFree(void *value) {
    kraftCrdtCounterFree((kraftCrdtCounterValue *)value);
}

static uint32_t gcounterCanCompact(const kraftCrdtOp *ops, uint32_t opCount) {
    /* G-Counter: all increments can be merged into one state */
    if (opCount <= 1) {
        return 0;
    }
    uint32_t incCount = 0;
    for (uint32_t i = 0; i < opCount; i++) {
        if (ops[i] == KRAFT_CRDT_OP_INC) {
            incCount++;
        }
    }
    return (incCount > 1) ? incCount - 1 : 0;
}

/* =========================================================================
 * PN-Counter Implementation
 * ========================================================================= */

kraftCrdtCounterValue *kraftCrdtPnCounterNew(void) {
    return kraftCrdtGCounterNew(); /* Same structure */
}

void kraftCrdtPnCounterIncrement(kraftCrdtCounterValue *value, uint32_t nodeId,
                                 int64_t amount) {
    if (!value) {
        return;
    }
    kraftCrdtCounterEntry *entry = counterFindNode(value, nodeId);
    if (entry) {
        entry->value += amount;
    } else {
        if (!counterEnsureCapacity(value)) {
            return;
        }
        value->entries[value->entryCount].nodeId = nodeId;
        value->entries[value->entryCount].value = amount;
        value->entryCount++;
    }
}

void kraftCrdtPnCounterDecrement(kraftCrdtCounterValue *value, uint32_t nodeId,
                                 int64_t amount) {
    kraftCrdtPnCounterIncrement(value, nodeId, -amount);
}

int64_t kraftCrdtPnCounterTotal(const kraftCrdtCounterValue *value) {
    return kraftCrdtGCounterTotal(value); /* Same calculation */
}

kraftCrdtCounterValue *kraftCrdtPnCounterMerge(const kraftCrdtCounterValue *a,
                                               const kraftCrdtCounterValue *b) {
    /* For PN-Counter, we need to handle negative values */
    kraftCrdtCounterValue *result = kraftCrdtPnCounterNew();
    if (!result) {
        return NULL;
    }

    /* Add entries from a */
    if (a) {
        for (uint32_t i = 0; i < a->entryCount; i++) {
            kraftCrdtPnCounterIncrement(result, a->entries[i].nodeId,
                                        a->entries[i].value);
        }
    }

    /* Merge entries from b - for PN we take max of absolute changes */
    if (b) {
        for (uint32_t i = 0; i < b->entryCount; i++) {
            kraftCrdtCounterEntry *existing =
                counterFindNode(result, b->entries[i].nodeId);
            if (existing) {
                /* Take the larger absolute value while preserving sign
                 * direction */
                int64_t av = existing->value;
                int64_t bv = b->entries[i].value;
                if ((av >= 0 && bv >= 0 && bv > av) ||
                    (av < 0 && bv < 0 && bv < av) || (av >= 0 && bv < 0) ||
                    (av < 0 && bv >= 0)) {
                    /* Complex merge: sum the contributions */
                    existing->value = av + bv;
                }
            } else {
                kraftCrdtPnCounterIncrement(result, b->entries[i].nodeId,
                                            b->entries[i].value);
            }
        }
    }

    return result;
}

/* PN-Counter Type Operations */
static void *pncounterMerge(const void *a, const void *b) {
    return kraftCrdtPnCounterMerge((const kraftCrdtCounterValue *)a,
                                   (const kraftCrdtCounterValue *)b);
}

static void *pncounterApply(const void *current, kraftCrdtOp op,
                            const void *opData, size_t opDataLen,
                            uint32_t nodeId, uint64_t timestamp) {
    (void)timestamp;
    if (op != KRAFT_CRDT_OP_INC && op != KRAFT_CRDT_OP_DEC) {
        return NULL;
    }

    int64_t amount = 1;
    if (opData && opDataLen >= sizeof(int64_t)) {
        amount = *(const int64_t *)opData;
    }
    if (op == KRAFT_CRDT_OP_DEC) {
        amount = -amount;
    }

    kraftCrdtCounterValue *result;
    if (current) {
        result = kraftCrdtPnCounterMerge((const kraftCrdtCounterValue *)current,
                                         NULL);
    } else {
        result = kraftCrdtPnCounterNew();
    }
    if (result) {
        kraftCrdtPnCounterIncrement(result, nodeId, amount);
    }
    return result;
}

static uint32_t pncounterCanCompact(const kraftCrdtOp *ops, uint32_t opCount) {
    if (opCount <= 1) {
        return 0;
    }
    uint32_t opCount2 = 0;
    for (uint32_t i = 0; i < opCount; i++) {
        if (ops[i] == KRAFT_CRDT_OP_INC || ops[i] == KRAFT_CRDT_OP_DEC) {
            opCount2++;
        }
    }
    return (opCount2 > 1) ? opCount2 - 1 : 0;
}

/* =========================================================================
 * G-Set Implementation
 * ========================================================================= */

kraftCrdtGSetValue *kraftCrdtGSetNew(void) {
    kraftCrdtGSetValue *value = crdtAlloc(sizeof(*value));
    if (!value) {
        return NULL;
    }

    value->elementCapacity = 16;
    value->elements =
        crdtAlloc(value->elementCapacity * sizeof(kraftCrdtSetElement));
    if (!value->elements) {
        crdtFree(value);
        return NULL;
    }
    return value;
}

void kraftCrdtGSetFree(kraftCrdtGSetValue *value) {
    if (!value) {
        return;
    }
    for (uint32_t i = 0; i < value->elementCount; i++) {
        crdtFree(value->elements[i].data);
    }
    crdtFree(value->elements);
    crdtFree(value);
}

static bool gsetEnsureCapacity(kraftCrdtGSetValue *value) {
    if (value->elementCount < value->elementCapacity) {
        return true;
    }

    uint32_t newCap = value->elementCapacity * 2;
    kraftCrdtSetElement *newElements =
        crdtRealloc(value->elements, newCap * sizeof(kraftCrdtSetElement));
    if (!newElements) {
        return false;
    }

    value->elements = newElements;
    value->elementCapacity = newCap;
    return true;
}

bool kraftCrdtGSetAdd(kraftCrdtGSetValue *value, const void *data,
                      size_t dataLen) {
    if (!value || !data || dataLen == 0) {
        return false;
    }

    /* Check if already exists */
    if (kraftCrdtGSetContains(value, data, dataLen)) {
        return true;
    }

    if (!gsetEnsureCapacity(value)) {
        return false;
    }

    kraftCrdtSetElement *elem = &value->elements[value->elementCount];
    elem->data = crdtAlloc(dataLen);
    if (!elem->data) {
        return false;
    }

    memcpy(elem->data, data, dataLen);
    elem->dataLen = dataLen;
    elem->addedAt = (uint64_t)time(NULL);
    value->elementCount++;
    return true;
}

bool kraftCrdtGSetContains(const kraftCrdtGSetValue *value, const void *data,
                           size_t dataLen) {
    if (!value || !data) {
        return false;
    }

    for (uint32_t i = 0; i < value->elementCount; i++) {
        if (value->elements[i].dataLen == dataLen &&
            memcmp(value->elements[i].data, data, dataLen) == 0) {
            return true;
        }
    }
    return false;
}

kraftCrdtGSetValue *kraftCrdtGSetMerge(const kraftCrdtGSetValue *a,
                                       const kraftCrdtGSetValue *b) {
    kraftCrdtGSetValue *result = kraftCrdtGSetNew();
    if (!result) {
        return NULL;
    }

    /* Add all elements from a */
    if (a) {
        for (uint32_t i = 0; i < a->elementCount; i++) {
            kraftCrdtGSetAdd(result, a->elements[i].data,
                             a->elements[i].dataLen);
        }
    }

    /* Add all elements from b (duplicates handled by GSetAdd) */
    if (b) {
        for (uint32_t i = 0; i < b->elementCount; i++) {
            kraftCrdtGSetAdd(result, b->elements[i].data,
                             b->elements[i].dataLen);
        }
    }

    return result;
}

/* G-Set Type Operations */
static void *gsetMerge(const void *a, const void *b) {
    return kraftCrdtGSetMerge((const kraftCrdtGSetValue *)a,
                              (const kraftCrdtGSetValue *)b);
}

static void *gsetApply(const void *current, kraftCrdtOp op, const void *opData,
                       size_t opDataLen, uint32_t nodeId, uint64_t timestamp) {
    (void)nodeId;
    (void)timestamp;
    if (op != KRAFT_CRDT_OP_ADD) {
        return NULL;
    }

    kraftCrdtGSetValue *result;
    if (current) {
        result = kraftCrdtGSetMerge((const kraftCrdtGSetValue *)current, NULL);
    } else {
        result = kraftCrdtGSetNew();
    }
    if (result && opData) {
        kraftCrdtGSetAdd(result, opData, opDataLen);
    }
    return result;
}

static bool gsetEquals(const void *a, const void *b) {
    const kraftCrdtGSetValue *sa = (const kraftCrdtGSetValue *)a;
    const kraftCrdtGSetValue *sb = (const kraftCrdtGSetValue *)b;
    if (!sa || !sb) {
        return sa == sb;
    }
    if (sa->elementCount != sb->elementCount) {
        return false;
    }

    /* Check each element in a is in b */
    for (uint32_t i = 0; i < sa->elementCount; i++) {
        if (!kraftCrdtGSetContains(sb, sa->elements[i].data,
                                   sa->elements[i].dataLen)) {
            return false;
        }
    }
    return true;
}

static void gsetFree(void *value) {
    kraftCrdtGSetFree((kraftCrdtGSetValue *)value);
}

static uint32_t gsetCanCompact(const kraftCrdtOp *ops, uint32_t opCount) {
    /* G-Set: duplicate adds can be removed */
    if (opCount <= 1) {
        return 0;
    }
    /* For simplicity, assume all ADDs can potentially be merged */
    uint32_t addCount = 0;
    for (uint32_t i = 0; i < opCount; i++) {
        if (ops[i] == KRAFT_CRDT_OP_ADD) {
            addCount++;
        }
    }
    return (addCount > 1) ? addCount - 1 : 0;
}

/* =========================================================================
 * OR-Set Implementation
 * ========================================================================= */

kraftCrdtOrSetValue *kraftCrdtOrSetNew(void) {
    kraftCrdtOrSetValue *value = crdtAlloc(sizeof(*value));
    if (!value) {
        return NULL;
    }

    value->elementCapacity = 16;
    value->elements =
        crdtAlloc(value->elementCapacity * sizeof(kraftCrdtOrSetElement));
    if (!value->elements) {
        crdtFree(value);
        return NULL;
    }
    value->nextTag = 1;
    return value;
}

void kraftCrdtOrSetFree(kraftCrdtOrSetValue *value) {
    if (!value) {
        return;
    }
    for (uint32_t i = 0; i < value->elementCount; i++) {
        crdtFree(value->elements[i].data);
    }
    crdtFree(value->elements);
    crdtFree(value);
}

static bool orsetEnsureCapacity(kraftCrdtOrSetValue *value) {
    if (value->elementCount < value->elementCapacity) {
        return true;
    }

    uint32_t newCap = value->elementCapacity * 2;
    kraftCrdtOrSetElement *newElements =
        crdtRealloc(value->elements, newCap * sizeof(kraftCrdtOrSetElement));
    if (!newElements) {
        return false;
    }

    value->elements = newElements;
    value->elementCapacity = newCap;
    return true;
}

uint64_t kraftCrdtOrSetAdd(kraftCrdtOrSetValue *value, const void *data,
                           size_t dataLen, uint32_t nodeId) {
    if (!value || !data || dataLen == 0) {
        return 0;
    }

    if (!orsetEnsureCapacity(value)) {
        return 0;
    }

    kraftCrdtOrSetElement *elem = &value->elements[value->elementCount];
    elem->data = crdtAlloc(dataLen);
    if (!elem->data) {
        return 0;
    }

    memcpy(elem->data, data, dataLen);
    elem->dataLen = dataLen;
    elem->tag = value->nextTag++;
    elem->nodeId = nodeId;
    elem->removed = false;
    value->elementCount++;

    return elem->tag;
}

bool kraftCrdtOrSetRemove(kraftCrdtOrSetValue *value, const void *data,
                          size_t dataLen) {
    if (!value || !data) {
        return false;
    }

    bool found = false;
    for (uint32_t i = 0; i < value->elementCount; i++) {
        if (!value->elements[i].removed &&
            value->elements[i].dataLen == dataLen &&
            memcmp(value->elements[i].data, data, dataLen) == 0) {
            value->elements[i].removed = true;
            found = true;
        }
    }
    return found;
}

bool kraftCrdtOrSetContains(const kraftCrdtOrSetValue *value, const void *data,
                            size_t dataLen) {
    if (!value || !data) {
        return false;
    }

    for (uint32_t i = 0; i < value->elementCount; i++) {
        if (!value->elements[i].removed &&
            value->elements[i].dataLen == dataLen &&
            memcmp(value->elements[i].data, data, dataLen) == 0) {
            return true;
        }
    }
    return false;
}

uint32_t kraftCrdtOrSetActiveCount(const kraftCrdtOrSetValue *value) {
    if (!value) {
        return 0;
    }
    uint32_t count = 0;
    for (uint32_t i = 0; i < value->elementCount; i++) {
        if (!value->elements[i].removed) {
            count++;
        }
    }
    return count;
}

kraftCrdtOrSetValue *kraftCrdtOrSetMerge(const kraftCrdtOrSetValue *a,
                                         const kraftCrdtOrSetValue *b) {
    kraftCrdtOrSetValue *result = kraftCrdtOrSetNew();
    if (!result) {
        return NULL;
    }

    /* Add all elements from a */
    if (a) {
        for (uint32_t i = 0; i < a->elementCount; i++) {
            if (!orsetEnsureCapacity(result)) {
                kraftCrdtOrSetFree(result);
                return NULL;
            }
            kraftCrdtOrSetElement *elem =
                &result->elements[result->elementCount];
            elem->data = crdtAlloc(a->elements[i].dataLen);
            if (!elem->data) {
                continue;
            }
            memcpy(elem->data, a->elements[i].data, a->elements[i].dataLen);
            elem->dataLen = a->elements[i].dataLen;
            elem->tag = a->elements[i].tag;
            elem->nodeId = a->elements[i].nodeId;
            elem->removed = a->elements[i].removed;
            result->elementCount++;
            if (elem->tag >= result->nextTag) {
                result->nextTag = elem->tag + 1;
            }
        }
    }

    /* Merge elements from b */
    if (b) {
        for (uint32_t i = 0; i < b->elementCount; i++) {
            /* Check if tag already exists */
            bool found = false;
            for (uint32_t j = 0; j < result->elementCount; j++) {
                if (result->elements[j].tag == b->elements[i].tag &&
                    result->elements[j].nodeId == b->elements[i].nodeId) {
                    /* Merge: if either is removed, mark as removed */
                    if (b->elements[i].removed) {
                        result->elements[j].removed = true;
                    }
                    found = true;
                    break;
                }
            }
            if (!found) {
                if (!orsetEnsureCapacity(result)) {
                    continue;
                }
                kraftCrdtOrSetElement *elem =
                    &result->elements[result->elementCount];
                elem->data = crdtAlloc(b->elements[i].dataLen);
                if (!elem->data) {
                    continue;
                }
                memcpy(elem->data, b->elements[i].data, b->elements[i].dataLen);
                elem->dataLen = b->elements[i].dataLen;
                elem->tag = b->elements[i].tag;
                elem->nodeId = b->elements[i].nodeId;
                elem->removed = b->elements[i].removed;
                result->elementCount++;
                if (elem->tag >= result->nextTag) {
                    result->nextTag = elem->tag + 1;
                }
            }
        }
    }

    return result;
}

uint32_t kraftCrdtOrSetPurgeTombstones(kraftCrdtOrSetValue *value,
                                       uint64_t olderThanMs) {
    (void)olderThanMs; /* TODO: implement age-based purging */
    if (!value) {
        return 0;
    }

    uint32_t purged = 0;
    uint32_t writeIdx = 0;

    for (uint32_t i = 0; i < value->elementCount; i++) {
        if (value->elements[i].removed) {
            crdtFree(value->elements[i].data);
            purged++;
        } else {
            if (writeIdx != i) {
                value->elements[writeIdx] = value->elements[i];
            }
            writeIdx++;
        }
    }
    value->elementCount = writeIdx;
    return purged;
}

/* OR-Set Type Operations */
static void *orsetMerge(const void *a, const void *b) {
    return kraftCrdtOrSetMerge((const kraftCrdtOrSetValue *)a,
                               (const kraftCrdtOrSetValue *)b);
}

static void *orsetApply(const void *current, kraftCrdtOp op, const void *opData,
                        size_t opDataLen, uint32_t nodeId, uint64_t timestamp) {
    (void)timestamp;

    kraftCrdtOrSetValue *result;
    if (current) {
        result =
            kraftCrdtOrSetMerge((const kraftCrdtOrSetValue *)current, NULL);
    } else {
        result = kraftCrdtOrSetNew();
    }
    if (!result) {
        return NULL;
    }

    if (op == KRAFT_CRDT_OP_ADD && opData) {
        kraftCrdtOrSetAdd(result, opData, opDataLen, nodeId);
    } else if (op == KRAFT_CRDT_OP_REMOVE && opData) {
        kraftCrdtOrSetRemove(result, opData, opDataLen);
    }

    return result;
}

static bool orsetEquals(const void *a, const void *b) {
    const kraftCrdtOrSetValue *sa = (const kraftCrdtOrSetValue *)a;
    const kraftCrdtOrSetValue *sb = (const kraftCrdtOrSetValue *)b;
    return kraftCrdtOrSetActiveCount(sa) == kraftCrdtOrSetActiveCount(sb);
}

static void orsetFree(void *value) {
    kraftCrdtOrSetFree((kraftCrdtOrSetValue *)value);
}

static uint32_t orsetCanCompact(const kraftCrdtOp *ops, uint32_t opCount) {
    /* OR-Set: ADD followed by REMOVE of same element can be compacted */
    if (opCount <= 1) {
        return 0;
    }
    /* Conservative: assume pairs of ADD/REMOVE can be compacted */
    uint32_t pairs = 0;
    for (uint32_t i = 0; i + 1 < opCount; i++) {
        if (ops[i] == KRAFT_CRDT_OP_ADD && ops[i + 1] == KRAFT_CRDT_OP_REMOVE) {
            pairs++;
            i++; /* Skip the remove */
        }
    }
    return pairs * 2;
}

/* =========================================================================
 * Manager Implementation
 * ========================================================================= */

kraftCrdtManager *kraftCrdtManagerNew(void) {
    kraftCrdtManager *mgr = crdtAlloc(sizeof(*mgr));
    if (!mgr) {
        return NULL;
    }

    mgr->typeCapacity = 16;
    mgr->types = crdtAlloc(mgr->typeCapacity * sizeof(kraftCrdtTypeEntry));
    if (!mgr->types) {
        crdtFree(mgr);
        return NULL;
    }

    mgr->keyCapacity = 64;
    mgr->keys = crdtAlloc(mgr->keyCapacity * sizeof(kraftCrdtKey));
    if (!mgr->keys) {
        crdtFree(mgr->types);
        crdtFree(mgr);
        return NULL;
    }

    /* Default configuration */
    mgr->minEntriesForCompaction = 10;
    mgr->maxEntriesPerKey = 1000;
    mgr->preserveTombstones = true;
    mgr->tombstoneRetentionMs = 24 * 60 * 60 * 1000ULL; /* 24 hours */

    return mgr;
}

void kraftCrdtManagerFree(kraftCrdtManager *mgr) {
    if (!mgr) {
        return;
    }

    /* Free key patterns and cached values */
    for (uint32_t i = 0; i < mgr->keyCount; i++) {
        crdtFree(mgr->keys[i].keyPattern);
        /* TODO: Free cached values using type's free function */
    }

    crdtFree(mgr->keys);
    crdtFree(mgr->types);
    crdtFree(mgr);
}

bool kraftCrdtAttach(kraftCrdtManager *mgr, struct kraftState *state) {
    if (!mgr) {
        return false;
    }
    mgr->attachedState = state;
    return true;
}

void kraftCrdtDetach(kraftCrdtManager *mgr) {
    if (mgr) {
        mgr->attachedState = NULL;
    }
}

/* =========================================================================
 * Type Registration
 * ========================================================================= */

static bool registerType(kraftCrdtManager *mgr, kraftCrdtType type,
                         const kraftCrdtTypeOps *ops) {
    if (!mgr || !ops) {
        return false;
    }

    /* Check if type already registered */
    for (uint32_t i = 0; i < mgr->typeCount; i++) {
        if (mgr->types[i].type == type) {
            mgr->types[i].ops = *ops;
            return true;
        }
    }

    /* Ensure capacity */
    if (mgr->typeCount >= mgr->typeCapacity) {
        uint32_t newCap = mgr->typeCapacity * 2;
        kraftCrdtTypeEntry *newTypes =
            crdtRealloc(mgr->types, newCap * sizeof(kraftCrdtTypeEntry));
        if (!newTypes) {
            return false;
        }
        mgr->types = newTypes;
        mgr->typeCapacity = newCap;
    }

    mgr->types[mgr->typeCount].type = type;
    mgr->types[mgr->typeCount].ops = *ops;
    mgr->typeCount++;
    return true;
}

bool kraftCrdtRegisterLwwRegister(kraftCrdtManager *mgr) {
    kraftCrdtTypeOps ops = {.merge = lwwMerge,
                            .apply = lwwApply,
                            .equals = lwwEquals,
                            .serialize = NULL,
                            .deserialize = NULL,
                            .free = lwwFree,
                            .canCompact = lwwCanCompact,
                            .name = "LWW_REGISTER"};
    return registerType(mgr, KRAFT_CRDT_LWW_REGISTER, &ops);
}

bool kraftCrdtRegisterGCounter(kraftCrdtManager *mgr) {
    kraftCrdtTypeOps ops = {.merge = gcounterMerge,
                            .apply = gcounterApply,
                            .equals = gcounterEquals,
                            .serialize = NULL,
                            .deserialize = NULL,
                            .free = gcounterFree,
                            .canCompact = gcounterCanCompact,
                            .name = "G_COUNTER"};
    return registerType(mgr, KRAFT_CRDT_G_COUNTER, &ops);
}

bool kraftCrdtRegisterPnCounter(kraftCrdtManager *mgr) {
    kraftCrdtTypeOps ops = {.merge = pncounterMerge,
                            .apply = pncounterApply,
                            .equals = gcounterEquals, /* Same equals logic */
                            .serialize = NULL,
                            .deserialize = NULL,
                            .free = gcounterFree, /* Same free logic */
                            .canCompact = pncounterCanCompact,
                            .name = "PN_COUNTER"};
    return registerType(mgr, KRAFT_CRDT_PN_COUNTER, &ops);
}

bool kraftCrdtRegisterGSet(kraftCrdtManager *mgr) {
    kraftCrdtTypeOps ops = {.merge = gsetMerge,
                            .apply = gsetApply,
                            .equals = gsetEquals,
                            .serialize = NULL,
                            .deserialize = NULL,
                            .free = gsetFree,
                            .canCompact = gsetCanCompact,
                            .name = "G_SET"};
    return registerType(mgr, KRAFT_CRDT_G_SET, &ops);
}

bool kraftCrdtRegisterOrSet(kraftCrdtManager *mgr) {
    kraftCrdtTypeOps ops = {.merge = orsetMerge,
                            .apply = orsetApply,
                            .equals = orsetEquals,
                            .serialize = NULL,
                            .deserialize = NULL,
                            .free = orsetFree,
                            .canCompact = orsetCanCompact,
                            .name = "OR_SET"};
    return registerType(mgr, KRAFT_CRDT_OR_SET, &ops);
}

bool kraftCrdtRegisterBuiltins(kraftCrdtManager *mgr) {
    if (!mgr) {
        return false;
    }
    return kraftCrdtRegisterLwwRegister(mgr) &&
           kraftCrdtRegisterGCounter(mgr) && kraftCrdtRegisterPnCounter(mgr) &&
           kraftCrdtRegisterGSet(mgr) && kraftCrdtRegisterOrSet(mgr);
}

bool kraftCrdtRegisterType(kraftCrdtManager *mgr, const char *name,
                           kraftCrdtType typeId, const kraftCrdtTypeOps *ops) {
    if (!mgr || !ops || typeId < KRAFT_CRDT_USER_TYPE_BASE) {
        return false;
    }
    kraftCrdtTypeOps opsCopy = *ops;
    opsCopy.name = name;
    return registerType(mgr, typeId, &opsCopy);
}

const kraftCrdtTypeOps *kraftCrdtGetTypeOps(const kraftCrdtManager *mgr,
                                            kraftCrdtType type) {
    if (!mgr) {
        return NULL;
    }
    for (uint32_t i = 0; i < mgr->typeCount; i++) {
        if (mgr->types[i].type == type) {
            return &mgr->types[i].ops;
        }
    }
    return NULL;
}

/* =========================================================================
 * Key Declaration
 * ========================================================================= */

bool kraftCrdtDeclareKey(kraftCrdtManager *mgr, const char *key,
                         kraftCrdtType type) {
    if (!mgr || !key) {
        return false;
    }

    /* Check if already declared */
    for (uint32_t i = 0; i < mgr->keyCount; i++) {
        if (strcmp(mgr->keys[i].keyPattern, key) == 0) {
            mgr->keys[i].type = type;
            return true;
        }
    }

    /* Ensure capacity */
    if (mgr->keyCount >= mgr->keyCapacity) {
        uint32_t newCap = mgr->keyCapacity * 2;
        kraftCrdtKey *newKeys =
            crdtRealloc(mgr->keys, newCap * sizeof(kraftCrdtKey));
        if (!newKeys) {
            return false;
        }
        mgr->keys = newKeys;
        mgr->keyCapacity = newCap;
    }

    mgr->keys[mgr->keyCount].keyPattern = crdtStrdup(key);
    mgr->keys[mgr->keyCount].type = type;
    mgr->keys[mgr->keyCount].isPattern = false;
    mgr->keys[mgr->keyCount].currentValue = NULL;
    mgr->keys[mgr->keyCount].lastCompacted = 0;
    mgr->keyCount++;
    return true;
}

bool kraftCrdtDeclarePattern(kraftCrdtManager *mgr, const char *pattern,
                             kraftCrdtType type) {
    if (!kraftCrdtDeclareKey(mgr, pattern, type)) {
        return false;
    }
    mgr->keys[mgr->keyCount - 1].isPattern = true;
    return true;
}

/* Simple glob pattern matching */
static bool patternMatches(const char *pattern, const char *str) {
    while (*pattern && *str) {
        if (*pattern == '*') {
            pattern++;
            if (!*pattern) {
                return true; /* Trailing * matches everything */
            }
            while (*str) {
                if (patternMatches(pattern, str)) {
                    return true;
                }
                str++;
            }
            return false;
        }
        if (*pattern != *str) {
            return false;
        }
        pattern++;
        str++;
    }
    return *pattern == '\0' && *str == '\0';
}

kraftCrdtType kraftCrdtGetKeyType(const kraftCrdtManager *mgr,
                                  const char *key) {
    if (!mgr || !key) {
        return KRAFT_CRDT_NONE;
    }

    /* First check exact matches */
    for (uint32_t i = 0; i < mgr->keyCount; i++) {
        if (!mgr->keys[i].isPattern &&
            strcmp(mgr->keys[i].keyPattern, key) == 0) {
            return mgr->keys[i].type;
        }
    }

    /* Then check pattern matches */
    for (uint32_t i = 0; i < mgr->keyCount; i++) {
        if (mgr->keys[i].isPattern &&
            patternMatches(mgr->keys[i].keyPattern, key)) {
            return mgr->keys[i].type;
        }
    }

    return KRAFT_CRDT_NONE;
}

bool kraftCrdtRemoveKey(kraftCrdtManager *mgr, const char *key) {
    if (!mgr || !key) {
        return false;
    }

    for (uint32_t i = 0; i < mgr->keyCount; i++) {
        if (strcmp(mgr->keys[i].keyPattern, key) == 0) {
            crdtFree(mgr->keys[i].keyPattern);
            /* Shift remaining keys */
            memmove(&mgr->keys[i], &mgr->keys[i + 1],
                    (mgr->keyCount - i - 1) * sizeof(kraftCrdtKey));
            mgr->keyCount--;
            return true;
        }
    }
    return false;
}

/* =========================================================================
 * Configuration
 * ========================================================================= */

void kraftCrdtConfigureCompaction(kraftCrdtManager *mgr, uint32_t minEntries,
                                  uint32_t maxEntries) {
    if (!mgr) {
        return;
    }
    mgr->minEntriesForCompaction = minEntries;
    mgr->maxEntriesPerKey = maxEntries;
}

void kraftCrdtConfigureTombstones(kraftCrdtManager *mgr, bool preserve,
                                  uint64_t retentionMs) {
    if (!mgr) {
        return;
    }
    mgr->preserveTombstones = preserve;
    mgr->tombstoneRetentionMs = retentionMs;
}

/* =========================================================================
 * Statistics
 * ========================================================================= */

void kraftCrdtGetStats(const kraftCrdtManager *mgr, kraftCrdtStats *stats) {
    if (!mgr || !stats) {
        return;
    }

    stats->entriesCompacted = mgr->entriesCompacted;
    stats->bytesReclaimed = mgr->bytesReclaimed;
    stats->compactionRuns = mgr->compactionRuns;
    stats->mergeOperations = mgr->mergeOperations;
    stats->registeredTypes = mgr->typeCount;
    stats->registeredKeys = mgr->keyCount;
}

void kraftCrdtResetStats(kraftCrdtManager *mgr) {
    if (!mgr) {
        return;
    }
    mgr->entriesCompacted = 0;
    mgr->bytesReclaimed = 0;
    mgr->compactionRuns = 0;
    mgr->mergeOperations = 0;
}

/* =========================================================================
 * String Helpers
 * ========================================================================= */

const char *kraftCrdtTypeName(kraftCrdtType type) {
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
    case KRAFT_CRDT_MV_REGISTER:
        return "MV_REGISTER";
    default:
        if (type >= KRAFT_CRDT_USER_TYPE_BASE) {
            return "USER_TYPE";
        }
        return "UNKNOWN";
    }
}

const char *kraftCrdtOpName(kraftCrdtOp op) {
    switch (op) {
    case KRAFT_CRDT_OP_SET:
        return "SET";
    case KRAFT_CRDT_OP_INC:
        return "INC";
    case KRAFT_CRDT_OP_DEC:
        return "DEC";
    case KRAFT_CRDT_OP_ADD:
        return "ADD";
    case KRAFT_CRDT_OP_REMOVE:
        return "REMOVE";
    case KRAFT_CRDT_OP_MERGE:
        return "MERGE";
    case KRAFT_CRDT_OP_DELETE:
        return "DELETE";
    default:
        return "UNKNOWN";
    }
}

/* =========================================================================
 * Compaction (Stub - requires log integration)
 * ========================================================================= */

uint32_t kraftCrdtNeedsCompaction(const kraftCrdtManager *mgr,
                                  const char *key) {
    (void)mgr;
    (void)key;
    /* TODO: Scan log for entries matching key and count compactable ops */
    return 0;
}

bool kraftCrdtCompactKey(kraftCrdtManager *mgr, const char *key,
                         kraftCrdtCompactionResult *result) {
    if (!mgr || !key || !result) {
        return false;
    }

    memset(result, 0, sizeof(*result));
    result->success = true;
    result->keysProcessed = 1;

    /* TODO: Implement actual log scanning and compaction */
    mgr->compactionRuns++;

    return true;
}

bool kraftCrdtCompactAll(kraftCrdtManager *mgr,
                         kraftCrdtCompactionResult *result) {
    if (!mgr || !result) {
        return false;
    }

    memset(result, 0, sizeof(*result));
    result->success = true;

    for (uint32_t i = 0; i < mgr->keyCount; i++) {
        kraftCrdtCompactionResult keyResult;
        kraftCrdtCompactKey(mgr, mgr->keys[i].keyPattern, &keyResult);
        result->originalEntries += keyResult.originalEntries;
        result->compactedEntries += keyResult.compactedEntries;
        result->bytesReclaimed += keyResult.bytesReclaimed;
        result->mergeOperations += keyResult.mergeOperations;
        result->keysProcessed++;
    }

    return true;
}

/* Stub implementations for value operations */
bool kraftCrdtGetValue(const kraftCrdtManager *mgr, const char *key,
                       void **outValue, size_t *outLen) {
    (void)mgr;
    (void)key;
    if (outValue) {
        *outValue = NULL;
    }
    if (outLen) {
        *outLen = 0;
    }
    return false; /* TODO: Implement */
}

bool kraftCrdtApplyOp(kraftCrdtManager *mgr, const char *key, kraftCrdtOp op,
                      const void *data, size_t dataLen, uint32_t nodeId) {
    (void)mgr;
    (void)key;
    (void)op;
    (void)data;
    (void)dataLen;
    (void)nodeId;
    return false; /* TODO: Implement */
}
