#ifndef KRAFT_CRDT_H
#define KRAFT_CRDT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Kraft CRDT-Aware Log Compaction
 * ============================================
 * Enables intelligent log compaction that understands CRDT semantics,
 * eliminating redundant entries while preserving convergence guarantees.
 *
 * Built-in CRDT Types:
 *   - LWW Register: Last-Writer-Wins register (timestamp-based)
 *   - G-Counter: Grow-only counter
 *   - PN-Counter: Positive-negative counter (increment/decrement)
 *   - G-Set: Grow-only set (add-only)
 *   - OR-Set: Observed-Remove set (add/remove with tombstones)
 *
 * Quick Start:
 *   kraftCrdtManager *mgr = kraftCrdtManagerNew();
 *   kraftCrdtRegisterBuiltins(mgr);  // Register LWW, counters, sets
 *
 *   // Declare a key as a specific CRDT type
 *   kraftCrdtDeclareKey(mgr, "user:123:name", KRAFT_CRDT_LWW_REGISTER);
 *   kraftCrdtDeclareKey(mgr, "page:views", KRAFT_CRDT_G_COUNTER);
 *
 *   // During compaction, redundant entries are merged
 *   kraftCrdtCompactLog(mgr, log, startIndex, endIndex);
 *
 * Custom Types:
 *   kraftCrdtTypeOps myTypeOps = { .merge = myMerge, ... };
 *   kraftCrdtRegisterType(mgr, "MyType", 100, &myTypeOps);
 */

/* Forward declarations */
struct kraftState;
struct kraftLog;

/* =========================================================================
 * CRDT Type Identifiers
 * ========================================================================= */

typedef enum kraftCrdtType {
    KRAFT_CRDT_NONE = 0, /* Not a CRDT (standard key-value) */

    /* Built-in types (1-99 reserved) */
    KRAFT_CRDT_LWW_REGISTER = 1, /* Last-Writer-Wins Register */
    KRAFT_CRDT_G_COUNTER = 2,    /* Grow-only Counter */
    KRAFT_CRDT_PN_COUNTER = 3,   /* Positive-Negative Counter */
    KRAFT_CRDT_G_SET = 4,        /* Grow-only Set */
    KRAFT_CRDT_OR_SET = 5,       /* Observed-Remove Set */
    KRAFT_CRDT_MV_REGISTER = 6,  /* Multi-Value Register (vector clock) */

    /* User-defined types start at 100 */
    KRAFT_CRDT_USER_TYPE_BASE = 100,
} kraftCrdtType;

/* =========================================================================
 * CRDT Operations
 * ========================================================================= */

/* Operation types for CRDT entries */
typedef enum kraftCrdtOp {
    KRAFT_CRDT_OP_SET = 0,    /* Set/replace value */
    KRAFT_CRDT_OP_INC = 1,    /* Increment (counters) */
    KRAFT_CRDT_OP_DEC = 2,    /* Decrement (PN-counter) */
    KRAFT_CRDT_OP_ADD = 3,    /* Add element (sets) */
    KRAFT_CRDT_OP_REMOVE = 4, /* Remove element (OR-set) */
    KRAFT_CRDT_OP_MERGE = 5,  /* Explicit merge operation */
    KRAFT_CRDT_OP_DELETE = 6, /* Delete entire key */
} kraftCrdtOp;

/* =========================================================================
 * CRDT Value Structures
 * ========================================================================= */

/* Timestamp for LWW operations */
typedef struct kraftCrdtTimestamp {
    uint64_t wallClock;    /* Wall-clock time (microseconds) */
    uint64_t logicalClock; /* Logical clock (Lamport) */
    uint32_t nodeId;       /* Node that created this timestamp */
} kraftCrdtTimestamp;

/* LWW Register value */
typedef struct kraftCrdtLwwValue {
    kraftCrdtTimestamp timestamp;
    void *data;
    size_t dataLen;
} kraftCrdtLwwValue;

/* G-Counter / PN-Counter value (per-node counts) */
typedef struct kraftCrdtCounterEntry {
    uint32_t nodeId;
    int64_t value; /* Positive for G/PN, can be negative for PN */
} kraftCrdtCounterEntry;

typedef struct kraftCrdtCounterValue {
    kraftCrdtCounterEntry *entries;
    uint32_t entryCount;
    uint32_t entryCapacity;
} kraftCrdtCounterValue;

/* G-Set element */
typedef struct kraftCrdtSetElement {
    void *data;
    size_t dataLen;
    uint64_t addedAt; /* Log index when added */
} kraftCrdtSetElement;

/* G-Set value */
typedef struct kraftCrdtGSetValue {
    kraftCrdtSetElement *elements;
    uint32_t elementCount;
    uint32_t elementCapacity;
} kraftCrdtGSetValue;

/* OR-Set element (with unique tag for removal) */
typedef struct kraftCrdtOrSetElement {
    void *data;
    size_t dataLen;
    uint64_t tag;    /* Unique tag for this add operation */
    uint32_t nodeId; /* Node that added this element */
    bool removed;    /* Tombstone flag */
} kraftCrdtOrSetElement;

/* OR-Set value */
typedef struct kraftCrdtOrSetValue {
    kraftCrdtOrSetElement *elements;
    uint32_t elementCount;
    uint32_t elementCapacity;
    uint64_t nextTag; /* Next unique tag to use */
} kraftCrdtOrSetValue;

/* =========================================================================
 * CRDT Type Operations (for extensibility)
 * ========================================================================= */

/* Function pointer types for custom CRDT implementations */

/**
 * Merge two CRDT values into a new value
 * @param a First value
 * @param b Second value
 * @return Merged value (caller owns)
 */
typedef void *(*kraftCrdtMergeFn)(const void *a, const void *b);

/**
 * Apply an operation to a CRDT value
 * @param current Current value (NULL if not exists)
 * @param op Operation type
 * @param opData Operation data
 * @param opDataLen Operation data length
 * @param nodeId Node performing the operation
 * @param timestamp Timestamp of operation
 * @return New value (caller owns)
 */
typedef void *(*kraftCrdtApplyFn)(const void *current, kraftCrdtOp op,
                                  const void *opData, size_t opDataLen,
                                  uint32_t nodeId, uint64_t timestamp);

/**
 * Check if two values are equivalent (for detecting redundant ops)
 * @param a First value
 * @param b Second value
 * @return true if equivalent
 */
typedef bool (*kraftCrdtEqualsFn)(const void *a, const void *b);

/**
 * Serialize a CRDT value for storage/transmission
 * @param value Value to serialize
 * @param outData Output buffer (allocated by function)
 * @param outLen Output length
 * @return true on success
 */
typedef bool (*kraftCrdtSerializeFn)(const void *value, void **outData,
                                     size_t *outLen);

/**
 * Deserialize a CRDT value from storage/transmission
 * @param data Serialized data
 * @param dataLen Data length
 * @return Deserialized value (caller owns)
 */
typedef void *(*kraftCrdtDeserializeFn)(const void *data, size_t dataLen);

/**
 * Free a CRDT value
 * @param value Value to free
 */
typedef void (*kraftCrdtFreeFn)(void *value);

/**
 * Check if a sequence of operations can be compacted
 * @param ops Array of operation types
 * @param opCount Number of operations
 * @return Number of ops that can be merged (0 = no compaction possible)
 */
typedef uint32_t (*kraftCrdtCanCompactFn)(const kraftCrdtOp *ops,
                                          uint32_t opCount);

/* Type operations structure */
typedef struct kraftCrdtTypeOps {
    kraftCrdtMergeFn merge;
    kraftCrdtApplyFn apply;
    kraftCrdtEqualsFn equals;
    kraftCrdtSerializeFn serialize;
    kraftCrdtDeserializeFn deserialize;
    kraftCrdtFreeFn free;
    kraftCrdtCanCompactFn canCompact;
    const char *name; /* Human-readable type name */
} kraftCrdtTypeOps;

/* =========================================================================
 * Key Registration
 * ========================================================================= */

/* Registered CRDT key */
typedef struct kraftCrdtKey {
    char *keyPattern;       /* Key or pattern (e.g., "user:*:name") */
    kraftCrdtType type;     /* CRDT type for this key */
    bool isPattern;         /* Is this a wildcard pattern? */
    void *currentValue;     /* Cached current value (optional) */
    uint64_t lastCompacted; /* Log index of last compaction */
} kraftCrdtKey;

/* =========================================================================
 * CRDT Manager
 * ========================================================================= */

/* Type registration entry */
typedef struct kraftCrdtTypeEntry {
    kraftCrdtType type;
    kraftCrdtTypeOps ops;
} kraftCrdtTypeEntry;

/* CRDT Manager structure */
typedef struct kraftCrdtManager {
    /* Registered types */
    kraftCrdtTypeEntry *types;
    uint32_t typeCount;
    uint32_t typeCapacity;

    /* Registered keys */
    kraftCrdtKey *keys;
    uint32_t keyCount;
    uint32_t keyCapacity;

    /* Configuration */
    uint32_t minEntriesForCompaction; /* Min entries before compacting */
    uint32_t maxEntriesPerKey;        /* Max entries to keep per key */
    bool preserveTombstones;          /* Keep tombstones for OR-Set? */
    uint64_t tombstoneRetentionMs;    /* How long to keep tombstones */

    /* Statistics */
    uint64_t entriesCompacted;
    uint64_t bytesReclaimed;
    uint64_t compactionRuns;
    uint64_t mergeOperations;

    /* Attached state */
    struct kraftState *attachedState;
} kraftCrdtManager;

/* Compaction result */
typedef struct kraftCrdtCompactionResult {
    uint64_t originalEntries;
    uint64_t compactedEntries;
    uint64_t bytesReclaimed;
    uint64_t keysProcessed;
    uint64_t mergeOperations;
    bool success;
    char errorMsg[128];
} kraftCrdtCompactionResult;

/* Statistics snapshot */
typedef struct kraftCrdtStats {
    uint64_t entriesCompacted;
    uint64_t bytesReclaimed;
    uint64_t compactionRuns;
    uint64_t mergeOperations;
    uint32_t registeredTypes;
    uint32_t registeredKeys;
} kraftCrdtStats;

/* =========================================================================
 * API Functions
 * ========================================================================= */

/* --- Manager Lifecycle --- */

/**
 * Create a new CRDT manager
 * @return New manager or NULL on failure
 */
kraftCrdtManager *kraftCrdtManagerNew(void);

/**
 * Free a CRDT manager and all resources
 * @param mgr Manager to free
 */
void kraftCrdtManagerFree(kraftCrdtManager *mgr);

/**
 * Attach CRDT manager to a kraft state
 * @param mgr CRDT manager
 * @param state Kraft state
 * @return true on success
 */
bool kraftCrdtAttach(kraftCrdtManager *mgr, struct kraftState *state);

/**
 * Detach CRDT manager from kraft state
 * @param mgr CRDT manager
 */
void kraftCrdtDetach(kraftCrdtManager *mgr);

/* --- Built-in Type Registration --- */

/**
 * Register all built-in CRDT types
 * @param mgr CRDT manager
 * @return true on success
 */
bool kraftCrdtRegisterBuiltins(kraftCrdtManager *mgr);

/**
 * Register LWW Register type
 */
bool kraftCrdtRegisterLwwRegister(kraftCrdtManager *mgr);

/**
 * Register G-Counter type
 */
bool kraftCrdtRegisterGCounter(kraftCrdtManager *mgr);

/**
 * Register PN-Counter type
 */
bool kraftCrdtRegisterPnCounter(kraftCrdtManager *mgr);

/**
 * Register G-Set type
 */
bool kraftCrdtRegisterGSet(kraftCrdtManager *mgr);

/**
 * Register OR-Set type
 */
bool kraftCrdtRegisterOrSet(kraftCrdtManager *mgr);

/* --- Custom Type Registration --- */

/**
 * Register a custom CRDT type
 * @param mgr CRDT manager
 * @param name Type name
 * @param typeId Type ID (must be >= KRAFT_CRDT_USER_TYPE_BASE)
 * @param ops Type operations
 * @return true on success
 */
bool kraftCrdtRegisterType(kraftCrdtManager *mgr, const char *name,
                           kraftCrdtType typeId, const kraftCrdtTypeOps *ops);

/**
 * Get operations for a type
 * @param mgr CRDT manager
 * @param type Type ID
 * @return Type operations or NULL if not registered
 */
const kraftCrdtTypeOps *kraftCrdtGetTypeOps(const kraftCrdtManager *mgr,
                                            kraftCrdtType type);

/* --- Key Declaration --- */

/**
 * Declare a key as a specific CRDT type
 * @param mgr CRDT manager
 * @param key Key name
 * @param type CRDT type
 * @return true on success
 */
bool kraftCrdtDeclareKey(kraftCrdtManager *mgr, const char *key,
                         kraftCrdtType type);

/**
 * Declare a key pattern as a specific CRDT type
 * @param mgr CRDT manager
 * @param pattern Key pattern (e.g., "user:*:counter")
 * @param type CRDT type
 * @return true on success
 */
bool kraftCrdtDeclarePattern(kraftCrdtManager *mgr, const char *pattern,
                             kraftCrdtType type);

/**
 * Get the CRDT type for a key
 * @param mgr CRDT manager
 * @param key Key name
 * @return CRDT type (KRAFT_CRDT_NONE if not registered)
 */
kraftCrdtType kraftCrdtGetKeyType(const kraftCrdtManager *mgr, const char *key);

/**
 * Remove a key declaration
 * @param mgr CRDT manager
 * @param key Key name
 * @return true if key was removed
 */
bool kraftCrdtRemoveKey(kraftCrdtManager *mgr, const char *key);

/* --- Configuration --- */

/**
 * Configure compaction thresholds
 * @param mgr CRDT manager
 * @param minEntries Min entries before compacting a key
 * @param maxEntries Max entries to keep per key
 */
void kraftCrdtConfigureCompaction(kraftCrdtManager *mgr, uint32_t minEntries,
                                  uint32_t maxEntries);

/**
 * Configure tombstone handling for OR-Set
 * @param mgr CRDT manager
 * @param preserve Keep tombstones?
 * @param retentionMs How long to keep (0 = forever)
 */
void kraftCrdtConfigureTombstones(kraftCrdtManager *mgr, bool preserve,
                                  uint64_t retentionMs);

/* --- Compaction --- */

/**
 * Compact the log for all registered CRDT keys
 * @param mgr CRDT manager
 * @param result Output result structure
 * @return true if compaction was performed
 */
bool kraftCrdtCompactAll(kraftCrdtManager *mgr,
                         kraftCrdtCompactionResult *result);

/**
 * Compact the log for a specific key
 * @param mgr CRDT manager
 * @param key Key to compact
 * @param result Output result structure
 * @return true if compaction was performed
 */
bool kraftCrdtCompactKey(kraftCrdtManager *mgr, const char *key,
                         kraftCrdtCompactionResult *result);

/**
 * Check if a key needs compaction
 * @param mgr CRDT manager
 * @param key Key to check
 * @return Number of compactable entries (0 = no compaction needed)
 */
uint32_t kraftCrdtNeedsCompaction(const kraftCrdtManager *mgr, const char *key);

/* --- Value Operations --- */

/**
 * Get the current merged value for a CRDT key
 * @param mgr CRDT manager
 * @param key Key name
 * @param outValue Output value (caller owns)
 * @param outLen Output length
 * @return true if value exists
 */
bool kraftCrdtGetValue(const kraftCrdtManager *mgr, const char *key,
                       void **outValue, size_t *outLen);

/**
 * Merge a new operation into a key's value
 * @param mgr CRDT manager
 * @param key Key name
 * @param op Operation type
 * @param data Operation data
 * @param dataLen Data length
 * @param nodeId Node performing operation
 * @return true on success
 */
bool kraftCrdtApplyOp(kraftCrdtManager *mgr, const char *key, kraftCrdtOp op,
                      const void *data, size_t dataLen, uint32_t nodeId);

/* --- Statistics --- */

/**
 * Get CRDT statistics
 * @param mgr CRDT manager
 * @param stats Output statistics
 */
void kraftCrdtGetStats(const kraftCrdtManager *mgr, kraftCrdtStats *stats);

/**
 * Reset statistics
 * @param mgr CRDT manager
 */
void kraftCrdtResetStats(kraftCrdtManager *mgr);

/* --- String Helpers --- */

/**
 * Get human-readable name for CRDT type
 */
const char *kraftCrdtTypeName(kraftCrdtType type);

/**
 * Get human-readable name for CRDT operation
 */
const char *kraftCrdtOpName(kraftCrdtOp op);

/* =========================================================================
 * Built-in Type Helpers (for direct use)
 * ========================================================================= */

/* --- LWW Register --- */

/**
 * Create a new LWW value
 */
kraftCrdtLwwValue *kraftCrdtLwwNew(const void *data, size_t dataLen,
                                   uint32_t nodeId, uint64_t timestamp);

/**
 * Free an LWW value
 */
void kraftCrdtLwwFree(kraftCrdtLwwValue *value);

/**
 * Merge two LWW values (returns newer one)
 */
kraftCrdtLwwValue *kraftCrdtLwwMerge(const kraftCrdtLwwValue *a,
                                     const kraftCrdtLwwValue *b);

/* --- G-Counter --- */

/**
 * Create a new G-Counter value
 */
kraftCrdtCounterValue *kraftCrdtGCounterNew(void);

/**
 * Free a counter value
 */
void kraftCrdtCounterFree(kraftCrdtCounterValue *value);

/**
 * Increment a G-Counter
 */
void kraftCrdtGCounterIncrement(kraftCrdtCounterValue *value, uint32_t nodeId,
                                int64_t amount);

/**
 * Get total G-Counter value
 */
int64_t kraftCrdtGCounterTotal(const kraftCrdtCounterValue *value);

/**
 * Merge two G-Counters
 */
kraftCrdtCounterValue *kraftCrdtGCounterMerge(const kraftCrdtCounterValue *a,
                                              const kraftCrdtCounterValue *b);

/* --- PN-Counter --- */

/**
 * Create a new PN-Counter value
 */
kraftCrdtCounterValue *kraftCrdtPnCounterNew(void);

/**
 * Increment a PN-Counter
 */
void kraftCrdtPnCounterIncrement(kraftCrdtCounterValue *value, uint32_t nodeId,
                                 int64_t amount);

/**
 * Decrement a PN-Counter
 */
void kraftCrdtPnCounterDecrement(kraftCrdtCounterValue *value, uint32_t nodeId,
                                 int64_t amount);

/**
 * Get total PN-Counter value
 */
int64_t kraftCrdtPnCounterTotal(const kraftCrdtCounterValue *value);

/**
 * Merge two PN-Counters
 */
kraftCrdtCounterValue *kraftCrdtPnCounterMerge(const kraftCrdtCounterValue *a,
                                               const kraftCrdtCounterValue *b);

/* --- G-Set --- */

/**
 * Create a new G-Set value
 */
kraftCrdtGSetValue *kraftCrdtGSetNew(void);

/**
 * Free a G-Set value
 */
void kraftCrdtGSetFree(kraftCrdtGSetValue *value);

/**
 * Add element to G-Set
 */
bool kraftCrdtGSetAdd(kraftCrdtGSetValue *value, const void *data,
                      size_t dataLen);

/**
 * Check if element is in G-Set
 */
bool kraftCrdtGSetContains(const kraftCrdtGSetValue *value, const void *data,
                           size_t dataLen);

/**
 * Merge two G-Sets
 */
kraftCrdtGSetValue *kraftCrdtGSetMerge(const kraftCrdtGSetValue *a,
                                       const kraftCrdtGSetValue *b);

/* --- OR-Set --- */

/**
 * Create a new OR-Set value
 */
kraftCrdtOrSetValue *kraftCrdtOrSetNew(void);

/**
 * Free an OR-Set value
 */
void kraftCrdtOrSetFree(kraftCrdtOrSetValue *value);

/**
 * Add element to OR-Set
 */
uint64_t kraftCrdtOrSetAdd(kraftCrdtOrSetValue *value, const void *data,
                           size_t dataLen, uint32_t nodeId);

/**
 * Remove element from OR-Set (by data match)
 */
bool kraftCrdtOrSetRemove(kraftCrdtOrSetValue *value, const void *data,
                          size_t dataLen);

/**
 * Check if element is in OR-Set (not removed)
 */
bool kraftCrdtOrSetContains(const kraftCrdtOrSetValue *value, const void *data,
                            size_t dataLen);

/**
 * Get active element count (non-removed)
 */
uint32_t kraftCrdtOrSetActiveCount(const kraftCrdtOrSetValue *value);

/**
 * Merge two OR-Sets
 */
kraftCrdtOrSetValue *kraftCrdtOrSetMerge(const kraftCrdtOrSetValue *a,
                                         const kraftCrdtOrSetValue *b);

/**
 * Compact OR-Set by removing old tombstones
 */
uint32_t kraftCrdtOrSetPurgeTombstones(kraftCrdtOrSetValue *value,
                                       uint64_t olderThanMs);

#endif /* KRAFT_CRDT_H */
