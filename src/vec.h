#ifndef VEC_H
#define VEC_H

/* ====================================================================
 * vec.h - Modern Dynamic Array Library
 *
 * A clean, type-safe, well-tested replacement for dumbarray with:
 * - Proper error handling
 * - Comprehensive NULL safety
 * - Minimal, intuitive API
 * - Excellent performance
 * - Full test coverage
 *
 * Design Philosophy:
 * - Simple and predictable
 * - Self-documenting code
 * - Zero hidden behavior
 * - Fail-safe defaults
 * ==================================================================== */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ====================================================================
 * Error Codes
 * ==================================================================== */
typedef enum vecError {
    VEC_OK = 0,         /* Success */
    VEC_ERROR_NULL,     /* NULL pointer argument */
    VEC_ERROR_NOMEM,    /* Memory allocation failed */
    VEC_ERROR_FULL,     /* Array at capacity, no auto-grow */
    VEC_ERROR_EMPTY,    /* Operation on empty array */
    VEC_ERROR_BOUNDS,   /* Index out of bounds */
    VEC_ERROR_EXISTS,   /* Element already exists (for unique operations) */
    VEC_ERROR_NOTFOUND, /* Element not found */
    VEC_ERROR_INVALID,  /* Invalid parameter (e.g., capacity too large) */
} vecError;

/* Convert error code to human-readable string */
const char *vecErrorString(vecError err);

/* ====================================================================
 * Vector Types
 *
 * Opaque handles for type safety. Never access internals directly.
 * ==================================================================== */

/* Vector of uint64_t values */
typedef struct vec vec;

/* Vector of void* pointers */
typedef struct vecPtr vecPtr;

/* ====================================================================
 * Configuration
 * ==================================================================== */

/* Maximum vector capacity (limited by metadata encoding) */
#define VEC_MAX_CAPACITY ((1U << 21) - 1) /* ~2 million elements */

/* Default growth factor for auto-growing vectors (1.5x) */
#define VEC_DEFAULT_GROWTH_NUMERATOR 3
#define VEC_DEFAULT_GROWTH_DENOMINATOR 2

/* Growth policy for auto-growing vectors */
typedef struct vecGrowthPolicy {
    uint32_t numerator;   /* Growth factor numerator (e.g., 3 for 1.5x) */
    uint32_t denominator; /* Growth factor denominator (e.g., 2 for 1.5x) */
    uint32_t minGrowth;   /* Minimum elements to add (default: 8) */
    uint32_t
        maxCapacity; /* Maximum allowed capacity (default: VEC_MAX_CAPACITY) */
} vecGrowthPolicy;

/* Get default growth policy (1.5x growth, min 8 elements) */
vecGrowthPolicy vecDefaultGrowthPolicy(void);

/* ====================================================================
 * Vector Creation & Destruction (uint64_t)
 * ==================================================================== */

/* Create new vector with specified initial capacity */
vec *vecNew(uint32_t capacity, vecError *err);

/* Create new auto-growing vector with default growth policy */
vec *vecNewGrowable(uint32_t initialCapacity, vecError *err);

/* Create new auto-growing vector with custom growth policy */
vec *vecNewGrowableCustom(uint32_t initialCapacity, vecGrowthPolicy policy,
                          vecError *err);

/* Free vector */
void vecFree(vec *v);

/* ====================================================================
 * Vector Information (uint64_t)
 * ==================================================================== */

/* Get current number of elements */
uint32_t vecCount(const vec *v);

/* Get current capacity */
uint32_t vecCapacity(const vec *v);

/* Check if vector is empty */
bool vecIsEmpty(const vec *v);

/* Check if vector is at capacity */
bool vecIsFull(const vec *v);

/* Check if vector auto-grows */
bool vecIsGrowable(const vec *v);

/* ====================================================================
 * Vector Modification (uint64_t)
 * ==================================================================== */

/* Append element to end. Returns VEC_ERROR_FULL if at capacity and not
 * growable. */
vecError vecPush(vec *v, uint64_t value);

/* Push element only if not already present. Returns VEC_ERROR_EXISTS if already
 * present. */
vecError vecPushUnique(vec *v, uint64_t value);

/* Remove and return last element. Returns VEC_ERROR_EMPTY if empty. */
vecError vecPop(vec *v, uint64_t *value);

/* Insert element at specific index. Shifts elements right. */
vecError vecInsert(vec *v, uint32_t index, uint64_t value);

/* Remove element at specific index. Shifts elements left. */
vecError vecRemove(vec *v, uint32_t index, uint64_t *value);

/* Remove all occurrences of value. Returns number removed. */
uint32_t vecRemoveValue(vec *v, uint64_t value);

/* Remove first occurrence of value */
vecError vecRemoveFirst(vec *v, uint64_t value);

/* Clear all elements (keeps capacity) */
void vecClear(vec *v);

/* Reserve capacity (grow if needed, never shrinks) */
vecError vecReserve(vec *v, uint32_t newCapacity);

/* Shrink capacity to exactly fit current elements */
vecError vecShrinkToFit(vec *v);

/* ====================================================================
 * Vector Access (uint64_t)
 * ==================================================================== */

/* Get element at index */
vecError vecGet(const vec *v, uint32_t index, uint64_t *value);

/* Set element at index */
vecError vecSet(vec *v, uint32_t index, uint64_t value);

/* Get pointer to first element (NULL if empty) */
uint64_t *vecData(const vec *v);

/* Get first element */
vecError vecFirst(const vec *v, uint64_t *value);

/* Get last element */
vecError vecLast(const vec *v, uint64_t *value);

/* ====================================================================
 * Vector Search & Query (uint64_t)
 * ==================================================================== */

/* Find first occurrence of value. Returns index or VEC_ERROR_NOTFOUND. */
vecError vecFind(const vec *v, uint64_t value, uint32_t *index);

/* Check if value exists in vector */
bool vecContains(const vec *v, uint64_t value);

/* Count occurrences of value */
uint32_t vecCountValue(const vec *v, uint64_t value);

/* ====================================================================
 * Vector Iteration (uint64_t)
 * ==================================================================== */

/* Iterator for safe traversal */
typedef struct vecIter {
    const vec *v;
    uint32_t index;
    bool valid;
} vecIter;

/* Create iterator for vector */
vecIter vecIterator(const vec *v);

/* Check if iterator has next element */
bool vecIterHasNext(const vecIter *it);

/* Get next element and advance iterator */
bool vecIterNext(vecIter *it, uint64_t *value);

/* Callback for forEach */
typedef void (*vecForEachFn)(uint64_t value, void *userData);

/* Call function for each element */
void vecForEach(const vec *v, vecForEachFn fn, void *userData);

/* ====================================================================
 * Pointer Vector Creation & Destruction
 * ==================================================================== */

vecPtr *vecPtrNew(uint32_t capacity, vecError *err);
vecPtr *vecPtrNewGrowable(uint32_t initialCapacity, vecError *err);
vecPtr *vecPtrNewGrowableCustom(uint32_t initialCapacity,
                                vecGrowthPolicy policy, vecError *err);
void vecPtrFree(vecPtr *v);

/* ====================================================================
 * Pointer Vector Information
 * ==================================================================== */

uint32_t vecPtrCount(const vecPtr *v);
uint32_t vecPtrCapacity(const vecPtr *v);
bool vecPtrIsEmpty(const vecPtr *v);
bool vecPtrIsFull(const vecPtr *v);
bool vecPtrIsGrowable(const vecPtr *v);

/* ====================================================================
 * Pointer Vector Modification
 * ==================================================================== */

vecError vecPtrPush(vecPtr *v, void *value);
vecError vecPtrPushUnique(vecPtr *v,
                          void *value); /* Push only if not present */
vecError vecPtrPop(vecPtr *v, void **value);
vecError vecPtrInsert(vecPtr *v, uint32_t index, void *value);
vecError vecPtrRemove(vecPtr *v, uint32_t index, void **value);
uint32_t vecPtrRemoveValue(vecPtr *v, void *value);
vecError vecPtrRemoveFirst(vecPtr *v, void *value);
void vecPtrClear(vecPtr *v);
vecError vecPtrReserve(vecPtr *v, uint32_t newCapacity);
vecError vecPtrShrinkToFit(vecPtr *v);

/* ====================================================================
 * Pointer Vector Access
 * ==================================================================== */

vecError vecPtrGet(const vecPtr *v, uint32_t index, void **value);
vecError vecPtrSet(vecPtr *v, uint32_t index, void *value);
void **vecPtrData(const vecPtr *v);
vecError vecPtrFirst(const vecPtr *v, void **value);
vecError vecPtrLast(const vecPtr *v, void **value);

/* ====================================================================
 * Pointer Vector Search & Query
 * ==================================================================== */

vecError vecPtrFind(const vecPtr *v, void *value, uint32_t *index);
bool vecPtrContains(const vecPtr *v, void *value);
uint32_t vecPtrCountValue(const vecPtr *v, void *value);

/* ====================================================================
 * Pointer Vector Iteration
 * ==================================================================== */

typedef struct vecPtrIter {
    const vecPtr *v;
    uint32_t index;
    bool valid;
} vecPtrIter;

vecPtrIter vecPtrIterator(const vecPtr *v);
bool vecPtrIterHasNext(const vecPtrIter *it);
bool vecPtrIterNext(vecPtrIter *it, void **value);

typedef void (*vecPtrForEachFn)(void *value, void *userData);
void vecPtrForEach(const vecPtr *v, vecPtrForEachFn fn, void *userData);

/* ====================================================================
 * Utility Macros
 * ==================================================================== */

/* Iterate over vector with index and value */
#define VEC_FOREACH(v, idx, val)                                               \
    for (uint32_t idx = 0;                                                     \
         idx < vecCount(v) && (vecGet(v, idx, &val) == VEC_OK); idx++)

#define VEC_PTR_FOREACH(v, idx, val)                                           \
    for (uint32_t idx = 0;                                                     \
         idx < vecPtrCount(v) && (vecPtrGet(v, idx, &val) == VEC_OK); idx++)

/* ====================================================================
 * Testing
 * ==================================================================== */

int vecTest(void);

#endif /* VEC_H */

/* vi:ai et sw=4 ts=4: */
