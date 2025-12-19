#include "vec.h"
#include <stdlib.h>
#include <string.h>

/* ====================================================================
 * Internal Structures
 *
 * The vector struct contains metadata and a pointer to the data array.
 * The struct itself is stable; only the data pointer is reallocated
 * when growing, allowing single-pointer API and stack allocation.
 * ==================================================================== */

struct vec {
    uint64_t *data;         /* Pointer to heap-allocated data array */
    uint32_t count;         /* Current number of elements */
    uint32_t capacity;      /* Total allocated slots */
    bool growable;          /* Auto-grow on push when full */
    vecGrowthPolicy growth; /* Growth policy (only used if growable) */
};

struct vecPtr {
    void **data; /* Pointer to heap-allocated pointer array */
    uint32_t count;
    uint32_t capacity;
    bool growable;
    vecGrowthPolicy growth;
};

/* ====================================================================
 * Error Handling
 * ==================================================================== */

const char *vecErrorString(vecError err) {
    switch (err) {
    case VEC_OK:
        return "Success";
    case VEC_ERROR_NULL:
        return "NULL pointer argument";
    case VEC_ERROR_NOMEM:
        return "Memory allocation failed";
    case VEC_ERROR_FULL:
        return "Vector at capacity";
    case VEC_ERROR_EMPTY:
        return "Vector is empty";
    case VEC_ERROR_BOUNDS:
        return "Index out of bounds";
    case VEC_ERROR_EXISTS:
        return "Element already exists";
    case VEC_ERROR_NOTFOUND:
        return "Element not found";
    case VEC_ERROR_INVALID:
        return "Invalid parameter";
    default:
        return "Unknown error";
    }
}

/* ====================================================================
 * Growth Policy
 * ==================================================================== */

vecGrowthPolicy vecDefaultGrowthPolicy(void) {
    return (vecGrowthPolicy){
        .numerator = VEC_DEFAULT_GROWTH_NUMERATOR,
        .denominator = VEC_DEFAULT_GROWTH_DENOMINATOR,
        .minGrowth = 8,
        .maxCapacity = VEC_MAX_CAPACITY,
    };
}

/* Calculate new capacity based on growth policy */
static uint32_t calculateGrowth(uint32_t current, vecGrowthPolicy policy) {
    /* Apply growth factor */
    uint64_t grown =
        ((uint64_t)current * policy.numerator) / policy.denominator;

    /* Ensure minimum growth */
    uint64_t withMin = current + policy.minGrowth;
    if (withMin > grown) {
        grown = withMin;
    }

    /* Cap at maximum */
    if (grown > policy.maxCapacity) {
        grown = policy.maxCapacity;
    }

    return (uint32_t)grown;
}

/* ====================================================================
 * Vector Creation & Destruction (uint64_t)
 * ==================================================================== */

vec *vecNew(uint32_t capacity, vecError *err) {
    if (capacity > VEC_MAX_CAPACITY) {
        if (err) {
            *err = VEC_ERROR_INVALID;
        }
        return NULL;
    }

    vec *v = malloc(sizeof(vec));
    if (!v) {
        if (err) {
            *err = VEC_ERROR_NOMEM;
        }
        return NULL;
    }

    v->data = calloc(capacity, sizeof(uint64_t));
    if (!v->data) {
        free(v);
        if (err) {
            *err = VEC_ERROR_NOMEM;
        }
        return NULL;
    }

    v->count = 0;
    v->capacity = capacity;
    v->growable = false;

    if (err) {
        *err = VEC_OK;
    }
    return v;
}

vec *vecNewGrowable(uint32_t initialCapacity, vecError *err) {
    return vecNewGrowableCustom(initialCapacity, vecDefaultGrowthPolicy(), err);
}

vec *vecNewGrowableCustom(uint32_t initialCapacity, vecGrowthPolicy policy,
                          vecError *err) {
    vec *v = vecNew(initialCapacity, err);
    if (!v) {
        return NULL;
    }

    v->growable = true;
    v->growth = policy;

    if (err) {
        *err = VEC_OK;
    }
    return v;
}

void vecFree(vec *v) {
    if (!v) {
        return;
    }

    free(v->data);
    free(v);
}

/* ====================================================================
 * Vector Information
 * ==================================================================== */

uint32_t vecCount(const vec *v) {
    return v ? v->count : 0;
}

uint32_t vecCapacity(const vec *v) {
    return v ? v->capacity : 0;
}

bool vecIsEmpty(const vec *v) {
    return v ? (v->count == 0) : true;
}

bool vecIsFull(const vec *v) {
    return v ? (v->count >= v->capacity) : true;
}

bool vecIsGrowable(const vec *v) {
    return v ? v->growable : false;
}

/* ====================================================================
 * Vector Modification
 * ==================================================================== */

vecError vecReserve(vec *v, uint32_t newCapacity) {
    if (!v) {
        return VEC_ERROR_NULL;
    }

    if (newCapacity > VEC_MAX_CAPACITY) {
        return VEC_ERROR_INVALID;
    }

    /* Don't shrink */
    if (newCapacity <= v->capacity) {
        return VEC_OK;
    }

    /* Reallocate data array only */
    uint64_t *newData = realloc(v->data, newCapacity * sizeof(uint64_t));
    if (!newData) {
        return VEC_ERROR_NOMEM;
    }

    /* Zero new memory */
    memset(newData + v->capacity, 0,
           (newCapacity - v->capacity) * sizeof(uint64_t));

    v->data = newData;
    v->capacity = newCapacity;

    return VEC_OK;
}

vecError vecPush(vec *v, uint64_t value) {
    if (!v) {
        return VEC_ERROR_NULL;
    }

    /* Check if we need to grow */
    if (v->count >= v->capacity) {
        if (!v->growable) {
            return VEC_ERROR_FULL;
        }

        /* Calculate new capacity */
        uint32_t newCapacity = calculateGrowth(v->capacity, v->growth);
        if (newCapacity <= v->capacity) {
            /* Already at max capacity */
            return VEC_ERROR_FULL;
        }

        /* Grow the data array only */
        uint64_t *newData = realloc(v->data, newCapacity * sizeof(uint64_t));
        if (!newData) {
            return VEC_ERROR_NOMEM;
        }

        /* Zero new memory */
        memset(newData + v->capacity, 0,
               (newCapacity - v->capacity) * sizeof(uint64_t));

        v->data = newData;
        v->capacity = newCapacity;
    }

    /* Add element */
    v->data[v->count++] = value;
    return VEC_OK;
}

vecError vecPushUnique(vec *v, uint64_t value) {
    if (!v) {
        return VEC_ERROR_NULL;
    }

    if (vecContains(v, value)) {
        return VEC_ERROR_EXISTS;
    }

    return vecPush(v, value);
}

vecError vecPop(vec *v, uint64_t *value) {
    if (!v) {
        return VEC_ERROR_NULL;
    }

    if (v->count == 0) {
        return VEC_ERROR_EMPTY;
    }

    if (value) {
        *value = v->data[v->count - 1];
    }

    v->count--;
    return VEC_OK;
}

vecError vecInsert(vec *v, uint32_t index, uint64_t value) {
    if (!v) {
        return VEC_ERROR_NULL;
    }

    if (index > v->count) {
        return VEC_ERROR_BOUNDS;
    }

    /* For now, insert doesn't auto-grow. Use push instead. */
    if (v->count >= v->capacity) {
        return VEC_ERROR_FULL;
    }

    /* Shift elements right */
    if (index < v->count) {
        memmove(&v->data[index + 1], &v->data[index],
                (v->count - index) * sizeof(uint64_t));
    }

    v->data[index] = value;
    v->count++;

    return VEC_OK;
}

vecError vecRemove(vec *v, uint32_t index, uint64_t *value) {
    if (!v) {
        return VEC_ERROR_NULL;
    }

    if (index >= v->count) {
        return VEC_ERROR_BOUNDS;
    }

    if (value) {
        *value = v->data[index];
    }

    /* Shift elements left */
    if (index < v->count - 1) {
        memmove(&v->data[index], &v->data[index + 1],
                (v->count - index - 1) * sizeof(uint64_t));
    }

    v->count--;
    return VEC_OK;
}

uint32_t vecRemoveValue(vec *v, uint64_t value) {
    if (!v) {
        return 0;
    }

    uint32_t removed = 0;

    /* Search backwards to avoid iterator invalidation */
    for (uint32_t i = v->count; i > 0; i--) {
        if (v->data[i - 1] == value) {
            vecRemove(v, i - 1, NULL);
            removed++;
        }
    }

    return removed;
}

vecError vecRemoveFirst(vec *v, uint64_t value) {
    if (!v) {
        return VEC_ERROR_NULL;
    }

    for (uint32_t i = 0; i < v->count; i++) {
        if (v->data[i] == value) {
            return vecRemove(v, i, NULL);
        }
    }

    return VEC_ERROR_NOTFOUND;
}

void vecClear(vec *v) {
    if (v) {
        v->count = 0;
        /* Optionally zero memory for security */
        memset(v->data, 0, v->capacity * sizeof(uint64_t));
    }
}

vecError vecShrinkToFit(vec *v) {
    if (!v) {
        return VEC_ERROR_NULL;
    }

    if (v->count == v->capacity) {
        return VEC_OK; /* Already fits */
    }

    /* Reallocate data array to exact size */
    uint32_t newCapacity = v->count > 0 ? v->count : 1;
    uint64_t *newData = realloc(v->data, newCapacity * sizeof(uint64_t));
    if (!newData) {
        /* Shrink failure is not critical */
        return VEC_OK;
    }

    v->data = newData;
    v->capacity = newCapacity;

    return VEC_OK;
}

/* ====================================================================
 * Vector Access
 * ==================================================================== */

vecError vecGet(const vec *v, uint32_t index, uint64_t *value) {
    if (!v) {
        return VEC_ERROR_NULL;
    }

    if (index >= v->count) {
        return VEC_ERROR_BOUNDS;
    }

    if (value) {
        *value = v->data[index];
    }

    return VEC_OK;
}

vecError vecSet(vec *v, uint32_t index, uint64_t value) {
    if (!v) {
        return VEC_ERROR_NULL;
    }

    if (index >= v->count) {
        return VEC_ERROR_BOUNDS;
    }

    v->data[index] = value;
    return VEC_OK;
}

uint64_t *vecData(const vec *v) {
    if (!v || v->count == 0) {
        return NULL;
    }

    return (uint64_t *)v->data;
}

vecError vecFirst(const vec *v, uint64_t *value) {
    return vecGet(v, 0, value);
}

vecError vecLast(const vec *v, uint64_t *value) {
    if (!v || v->count == 0) {
        return VEC_ERROR_EMPTY;
    }

    if (value) {
        *value = v->data[v->count - 1];
    }

    return VEC_OK;
}

/* ====================================================================
 * Vector Search & Query
 * ==================================================================== */

vecError vecFind(const vec *v, uint64_t value, uint32_t *index) {
    if (!v) {
        return VEC_ERROR_NULL;
    }

    for (uint32_t i = 0; i < v->count; i++) {
        if (v->data[i] == value) {
            if (index) {
                *index = i;
            }
            return VEC_OK;
        }
    }

    return VEC_ERROR_NOTFOUND;
}

bool vecContains(const vec *v, uint64_t value) {
    return vecFind(v, value, NULL) == VEC_OK;
}

uint32_t vecCountValue(const vec *v, uint64_t value) {
    if (!v) {
        return 0;
    }

    uint32_t count = 0;
    for (uint32_t i = 0; i < v->count; i++) {
        if (v->data[i] == value) {
            count++;
        }
    }

    return count;
}

/* ====================================================================
 * Vector Iteration
 * ==================================================================== */

vecIter vecIterator(const vec *v) {
    return (vecIter){
        .v = v,
        .index = 0,
        .valid = (v != NULL && v->count > 0),
    };
}

bool vecIterHasNext(const vecIter *it) {
    return it && it->valid && it->v && it->index < it->v->count;
}

bool vecIterNext(vecIter *it, uint64_t *value) {
    if (!vecIterHasNext(it)) {
        return false;
    }

    if (value) {
        *value = it->v->data[it->index];
    }

    it->index++;

    /* Check if still valid after increment */
    if (it->index >= it->v->count) {
        it->valid = false;
    }

    return true;
}

void vecForEach(const vec *v, vecForEachFn fn, void *userData) {
    if (!v || !fn) {
        return;
    }

    for (uint32_t i = 0; i < v->count; i++) {
        fn(v->data[i], userData);
    }
}

/* ====================================================================
 * Pointer Vector Implementation
 *
 * These are simple wrappers that cast between void* and uint64_t.
 * On 64-bit systems this is direct. On 32-bit, we waste space but
 * maintain compatibility and simplicity.
 * ==================================================================== */

vecPtr *vecPtrNew(uint32_t capacity, vecError *err) {
    return (vecPtr *)vecNew(capacity, err);
}

vecPtr *vecPtrNewGrowable(uint32_t initialCapacity, vecError *err) {
    return (vecPtr *)vecNewGrowable(initialCapacity, err);
}

vecPtr *vecPtrNewGrowableCustom(uint32_t initialCapacity,
                                vecGrowthPolicy policy, vecError *err) {
    return (vecPtr *)vecNewGrowableCustom(initialCapacity, policy, err);
}

void vecPtrFree(vecPtr *v) {
    vecFree((vec *)v);
}

uint32_t vecPtrCount(const vecPtr *v) {
    return vecCount((const vec *)v);
}

uint32_t vecPtrCapacity(const vecPtr *v) {
    return vecCapacity((const vec *)v);
}

bool vecPtrIsEmpty(const vecPtr *v) {
    return vecIsEmpty((const vec *)v);
}

bool vecPtrIsFull(const vecPtr *v) {
    return vecIsFull((const vec *)v);
}

bool vecPtrIsGrowable(const vecPtr *v) {
    return vecIsGrowable((const vec *)v);
}

vecError vecPtrPush(vecPtr *v, void *value) {
    return vecPush((vec *)v, (uintptr_t)value);
}

vecError vecPtrPushUnique(vecPtr *v, void *value) {
    if (!v) {
        return VEC_ERROR_NULL;
    }

    if (vecPtrContains(v, value)) {
        return VEC_ERROR_EXISTS;
    }

    return vecPtrPush(v, value);
}

vecError vecPtrPop(vecPtr *v, void **value) {
    uint64_t val;
    vecError err = vecPop((vec *)v, &val);
    if (err == VEC_OK && value) {
        *value = (void *)(uintptr_t)val;
    }
    return err;
}

vecError vecPtrInsert(vecPtr *v, uint32_t index, void *value) {
    return vecInsert((vec *)v, index, (uintptr_t)value);
}

vecError vecPtrRemove(vecPtr *v, uint32_t index, void **value) {
    uint64_t val;
    vecError err = vecRemove((vec *)v, index, &val);
    if (err == VEC_OK && value) {
        *value = (void *)(uintptr_t)val;
    }
    return err;
}

uint32_t vecPtrRemoveValue(vecPtr *v, void *value) {
    return vecRemoveValue((vec *)v, (uintptr_t)value);
}

vecError vecPtrRemoveFirst(vecPtr *v, void *value) {
    return vecRemoveFirst((vec *)v, (uintptr_t)value);
}

void vecPtrClear(vecPtr *v) {
    vecClear((vec *)v);
}

vecError vecPtrReserve(vecPtr *v, uint32_t newCapacity) {
    return vecReserve((vec *)v, newCapacity);
}

vecError vecPtrShrinkToFit(vecPtr *v) {
    return vecShrinkToFit((vec *)v);
}

vecError vecPtrGet(const vecPtr *v, uint32_t index, void **value) {
    uint64_t val;
    vecError err = vecGet((const vec *)v, index, &val);
    if (err == VEC_OK && value) {
        *value = (void *)(uintptr_t)val;
    }
    return err;
}

vecError vecPtrSet(vecPtr *v, uint32_t index, void *value) {
    return vecSet((vec *)v, index, (uintptr_t)value);
}

void **vecPtrData(const vecPtr *v) {
    return (void **)vecData((const vec *)v);
}

vecError vecPtrFirst(const vecPtr *v, void **value) {
    uint64_t val;
    vecError err = vecFirst((const vec *)v, &val);
    if (err == VEC_OK && value) {
        *value = (void *)(uintptr_t)val;
    }
    return err;
}

vecError vecPtrLast(const vecPtr *v, void **value) {
    uint64_t val;
    vecError err = vecLast((const vec *)v, &val);
    if (err == VEC_OK && value) {
        *value = (void *)(uintptr_t)val;
    }
    return err;
}

vecError vecPtrFind(const vecPtr *v, void *value, uint32_t *index) {
    return vecFind((const vec *)v, (uintptr_t)value, index);
}

bool vecPtrContains(const vecPtr *v, void *value) {
    return vecContains((const vec *)v, (uintptr_t)value);
}

uint32_t vecPtrCountValue(const vecPtr *v, void *value) {
    return vecCountValue((const vec *)v, (uintptr_t)value);
}

vecPtrIter vecPtrIterator(const vecPtr *v) {
    vecIter it = vecIterator((const vec *)v);
    return (vecPtrIter){
        .v = v,
        .index = it.index,
        .valid = it.valid,
    };
}

bool vecPtrIterHasNext(const vecPtrIter *it) {
    return vecIterHasNext((const vecIter *)it);
}

bool vecPtrIterNext(vecPtrIter *it, void **value) {
    uint64_t val;
    bool result = vecIterNext((vecIter *)it, &val);
    if (result && value) {
        *value = (void *)(uintptr_t)val;
    }
    return result;
}

void vecPtrForEach(const vecPtr *v, vecPtrForEachFn fn, void *userData) {
    if (!v || !fn) {
        return;
    }

    vecForEach((const vec *)v, (vecForEachFn)fn, userData);
}

/* vi:ai et sw=4 ts=4: */
