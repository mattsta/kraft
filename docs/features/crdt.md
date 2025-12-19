# CRDT Support

Kraft provides first-class support for Conflict-free Replicated Data Types (CRDTs), enabling intelligent log compaction and conflict resolution.

## Overview

CRDTs are data structures that can be replicated across nodes and merged without coordination:

```
┌─────────────────────────────────────────────────────────────────┐
│                      CRDT Properties                             │
│                                                                  │
│  • Commutative: order of operations doesn't matter              │
│  • Associative: grouping doesn't matter                         │
│  • Idempotent: applying same operation twice = once             │
│                                                                  │
│  Result: Any two replicas that have seen the same operations    │
│          will converge to the same state                        │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Supported CRDT Types

### G-Counter (Grow-only Counter)

```c
// Only increments, never decrements
// Each node maintains its own count
// Total = sum of all node counts

typedef struct {
    uint64_t counts[MAX_NODES];
} GCounter;

// Operations
void gcounterIncrement(GCounter *c, uint64_t nodeId, uint64_t amount);
uint64_t gcounterValue(GCounter *c);

// Merge: element-wise max
void gcounterMerge(GCounter *dst, GCounter *src) {
    for (int i = 0; i < MAX_NODES; i++) {
        if (src->counts[i] > dst->counts[i]) {
            dst->counts[i] = src->counts[i];
        }
    }
}
```

**Use cases**: Page views, API call counts, event counters

### PN-Counter (Positive-Negative Counter)

```c
// Supports increment and decrement
// Two G-Counters: one for increments, one for decrements

typedef struct {
    GCounter positive;
    GCounter negative;
} PNCounter;

void pncounterIncrement(PNCounter *c, uint64_t nodeId, uint64_t amount);
void pncounterDecrement(PNCounter *c, uint64_t nodeId, uint64_t amount);
int64_t pncounterValue(PNCounter *c);

// Value = sum(positive) - sum(negative)
```

**Use cases**: Stock levels, account balances, gauges

### LWW-Register (Last-Writer-Wins Register)

```c
// Single value with timestamp
// Latest timestamp wins on merge

typedef struct {
    uint8_t *value;
    size_t valueLen;
    uint64_t timestamp;
    uint64_t nodeId;      // Tie-breaker
} LWWRegister;

void lwwSet(LWWRegister *r, const void *value, size_t len, uint64_t ts, uint64_t nodeId);

// Merge: keep value with highest (timestamp, nodeId)
void lwwMerge(LWWRegister *dst, LWWRegister *src) {
    if (src->timestamp > dst->timestamp ||
        (src->timestamp == dst->timestamp && src->nodeId > dst->nodeId)) {
        // src wins
        memcpy(dst->value, src->value, src->valueLen);
        dst->valueLen = src->valueLen;
        dst->timestamp = src->timestamp;
        dst->nodeId = src->nodeId;
    }
}
```

**Use cases**: User profiles, configuration values, single-value state

### G-Set (Grow-only Set)

```c
// Elements can only be added, never removed

typedef struct {
    void **elements;
    size_t count;
    size_t capacity;
} GSet;

void gsetAdd(GSet *s, const void *element, size_t len);
bool gsetContains(GSet *s, const void *element, size_t len);

// Merge: union of all elements
void gsetMerge(GSet *dst, GSet *src) {
    for (size_t i = 0; i < src->count; i++) {
        if (!gsetContains(dst, src->elements[i])) {
            gsetAdd(dst, src->elements[i]);
        }
    }
}
```

**Use cases**: Tag collections, seen message IDs, immutable logs

### 2P-Set (Two-Phase Set)

```c
// Elements can be added and removed (once)
// Removed elements cannot be re-added

typedef struct {
    GSet added;
    GSet removed;
} TwoPhaseSet;

void twoPSetAdd(TwoPhaseSet *s, const void *element, size_t len);
void twoPSetRemove(TwoPhaseSet *s, const void *element, size_t len);

bool twoPSetContains(TwoPhaseSet *s, const void *element, size_t len) {
    return gsetContains(&s->added, element, len) &&
           !gsetContains(&s->removed, element, len);
}
```

**Use cases**: One-time tokens, revocation lists

### OR-Set (Observed-Remove Set)

```c
// Elements can be added and removed multiple times
// Each addition gets unique tag

typedef struct {
    char *element;
    uint64_t tag;        // Unique identifier for this add
} ORSetEntry;

typedef struct {
    ORSetEntry *entries;
    size_t count;
    uint64_t *removed;   // Tags that have been removed
    size_t removedCount;
} ORSet;

void orsetAdd(ORSet *s, const char *element, uint64_t tag);
void orsetRemove(ORSet *s, const char *element);  // Removes all current tags

bool orsetContains(ORSet *s, const char *element) {
    for (size_t i = 0; i < s->count; i++) {
        if (strcmp(s->entries[i].element, element) == 0) {
            // Check if this tag was removed
            bool removed = false;
            for (size_t j = 0; j < s->removedCount; j++) {
                if (s->removed[j] == s->entries[i].tag) {
                    removed = true;
                    break;
                }
            }
            if (!removed) return true;
        }
    }
    return false;
}
```

**Use cases**: Shopping carts, friend lists, mutable collections

### LWW-Map (Last-Writer-Wins Map)

```c
// Map where each key is an LWW-Register

typedef struct {
    char *key;
    LWWRegister value;
} LWWMapEntry;

typedef struct {
    LWWMapEntry *entries;
    size_t count;
} LWWMap;

void lwwMapSet(LWWMap *m, const char *key, const void *value, size_t len, uint64_t ts);
void *lwwMapGet(LWWMap *m, const char *key);
void lwwMapDelete(LWWMap *m, const char *key, uint64_t ts);  // Tombstone
```

**Use cases**: User preferences, document fields, key-value stores

---

## Configuration

### Key Pattern Mapping

```c
typedef enum {
    KRAFT_CRDT_GCOUNTER,
    KRAFT_CRDT_PNCOUNTER,
    KRAFT_CRDT_LWW_REGISTER,
    KRAFT_CRDT_GSET,
    KRAFT_CRDT_TWOPHASE_SET,
    KRAFT_CRDT_ORSET,
    KRAFT_CRDT_LWW_MAP,
} kraftCRDTType;

typedef struct {
    const char *pattern;      // Glob pattern
    kraftCRDTType type;
    kraftCRDTOptions options;
} kraftCRDTMapping;

kraftCRDTConfig config = {
    .mappings = (kraftCRDTMapping[]){
        // Counters
        {"counter:*", KRAFT_CRDT_GCOUNTER, {}},
        {"gauge:*", KRAFT_CRDT_PNCOUNTER, {}},

        // Registers
        {"user:*:profile", KRAFT_CRDT_LWW_REGISTER, {}},
        {"config:*", KRAFT_CRDT_LWW_REGISTER, {}},

        // Sets
        {"tags:*", KRAFT_CRDT_GSET, {}},
        {"cart:*", KRAFT_CRDT_ORSET, {}},
        {"blocklist:*", KRAFT_CRDT_TWOPHASE_SET, {}},

        // Maps
        {"doc:*", KRAFT_CRDT_LWW_MAP, {}},
    },
    .mappingCount = 8,

    // Global options
    .defaultType = KRAFT_CRDT_LWW_REGISTER,
    .enableCompaction = true,
};

kraftStateCRDTConfigure(state, &config);
```

### CRDT Options

```c
typedef struct {
    // For counters
    uint64_t maxValue;           // Cap counter value
    bool allowNegative;          // For PN-Counter

    // For registers
    uint64_t tombstoneTTL;       // How long to keep deleted values

    // For sets
    size_t maxElements;          // Maximum set size
    bool deduplicateOnMerge;     // Remove duplicates

    // For maps
    size_t maxKeys;              // Maximum number of keys
    kraftCRDTType valueType;     // Type for map values
} kraftCRDTOptions;
```

---

## CRDT-Aware Compaction

### How It Works

```
┌─────────────────────────────────────────────────────────────────┐
│                   CRDT Compaction Process                        │
│                                                                  │
│  Log entries before compaction:                                 │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │ [1] INC counter:views 1                                  │   │
│  │ [2] SET user:alice {name:"Alice"}                       │   │
│  │ [3] INC counter:views 1                                  │   │
│  │ [4] SET user:alice {name:"Alice Smith"}                 │   │
│  │ [5] INC counter:views 3                                  │   │
│  │ [6] ADD tags:post:1 "golang"                            │   │
│  │ [7] ADD tags:post:1 "raft"                              │   │
│  │ [8] INC counter:views 2                                  │   │
│  └─────────────────────────────────────────────────────────┘   │
│                                                                  │
│  After compaction:                                              │
│  ┌─────────────────────────────────────────────────────────┐   │
│  │ [1'] SET counter:views 7     (1+1+3+2 = 7)              │   │
│  │ [2'] SET user:alice {name:"Alice Smith"}  (latest)      │   │
│  │ [3'] SET tags:post:1 {"golang","raft"}    (merged)      │   │
│  └─────────────────────────────────────────────────────────┘   │
│                                                                  │
│  Entries reduced: 8 → 3                                         │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Compaction During Snapshot

```c
void crdtAwareSnapshot(kraftState *state, kraftSnapshotWriter *writer) {
    // Iterate all keys
    Iterator *it = stateMachineIterator(state);

    while (iteratorValid(it)) {
        const char *key = iteratorKey(it);
        kraftCRDTType type = getCRDTType(state, key);

        switch (type) {
            case KRAFT_CRDT_GCOUNTER: {
                // Merge all increments into final value
                uint64_t total = gcounterValue(iteratorValue(it));
                writeCompactedCounter(writer, key, total);
                break;
            }

            case KRAFT_CRDT_LWW_REGISTER: {
                // Keep only latest value
                LWWRegister *reg = iteratorValue(it);
                writeCompactedRegister(writer, key, reg);
                break;
            }

            case KRAFT_CRDT_ORSET: {
                // Merge and compact set
                ORSet *set = iteratorValue(it);
                ORSet *compacted = orsetCompact(set);
                writeCompactedSet(writer, key, compacted);
                orsetFree(compacted);
                break;
            }

            // ... other types
        }

        iteratorNext(it);
    }
}
```

---

## Command Format

### Encoding CRDT Operations

```c
typedef enum {
    CRDT_OP_INCREMENT = 1,
    CRDT_OP_DECREMENT = 2,
    CRDT_OP_SET = 3,
    CRDT_OP_ADD = 4,
    CRDT_OP_REMOVE = 5,
    CRDT_OP_MERGE = 6,
} CRDTOperation;

typedef struct __attribute__((packed)) {
    uint8_t type;           // CRDT type
    uint8_t operation;      // Operation type
    uint64_t timestamp;     // For LWW types
    uint64_t nodeId;        // Originating node
    uint16_t keyLen;
    uint32_t valueLen;
    // Followed by: key, value
} CRDTCommand;

// Create increment command
void encodeCRDTIncrement(kraftEncodeBuffer *buf, const char *key,
                         uint64_t amount, uint64_t nodeId) {
    CRDTCommand cmd = {
        .type = KRAFT_CRDT_GCOUNTER,
        .operation = CRDT_OP_INCREMENT,
        .timestamp = now(),
        .nodeId = nodeId,
        .keyLen = strlen(key),
        .valueLen = sizeof(amount),
    };

    kraftEncodeAppend(buf, &cmd, sizeof(cmd));
    kraftEncodeAppend(buf, key, cmd.keyLen);
    kraftEncodeAppend(buf, &amount, sizeof(amount));
}
```

### Applying CRDT Operations

```c
void applyCRDTCommand(MyStateMachine *sm, const CRDTCommand *cmd,
                      const char *key, const void *value) {
    void *existing = hashTableGet(sm->data, key);

    switch (cmd->type) {
        case KRAFT_CRDT_GCOUNTER: {
            GCounter *counter = existing;
            if (!counter) {
                counter = gcounterCreate();
                hashTableSet(sm->data, key, counter);
            }

            if (cmd->operation == CRDT_OP_INCREMENT) {
                uint64_t amount = *(uint64_t *)value;
                gcounterIncrement(counter, cmd->nodeId, amount);
            }
            break;
        }

        case KRAFT_CRDT_LWW_REGISTER: {
            LWWRegister *reg = existing;
            if (!reg) {
                reg = lwwCreate();
                hashTableSet(sm->data, key, reg);
            }

            if (cmd->operation == CRDT_OP_SET) {
                lwwSet(reg, value, cmd->valueLen, cmd->timestamp, cmd->nodeId);
            }
            break;
        }

        case KRAFT_CRDT_ORSET: {
            ORSet *set = existing;
            if (!set) {
                set = orsetCreate();
                hashTableSet(sm->data, key, set);
            }

            if (cmd->operation == CRDT_OP_ADD) {
                uint64_t tag = generateUniqueTag(cmd->timestamp, cmd->nodeId);
                orsetAdd(set, value, tag);
            } else if (cmd->operation == CRDT_OP_REMOVE) {
                orsetRemove(set, value);
            }
            break;
        }
    }
}
```

---

## Conflict Resolution

### Automatic Resolution

```
┌─────────────────────────────────────────────────────────────────┐
│                  Concurrent Operations                           │
│                                                                  │
│  Node A                         Node B                          │
│     │                              │                             │
│     │  SET user:1 {name:"Alice"}   │  SET user:1 {name:"Ally"}  │
│     │  timestamp: 100              │  timestamp: 101             │
│     │                              │                             │
│     │◄─────────────────────────────│                             │
│     │──────────────────────────────►│                             │
│     │                              │                             │
│     ▼                              ▼                             │
│  Result: {name:"Ally"}          Result: {name:"Ally"}           │
│  (timestamp 101 > 100)          (timestamp 101 > 100)           │
│                                                                  │
│  Both nodes converge to same value!                             │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Counter Convergence

```
┌─────────────────────────────────────────────────────────────────┐
│                  Counter Convergence                             │
│                                                                  │
│  Node A: counter[A]=5, counter[B]=0                             │
│  Node B: counter[A]=0, counter[B]=3                             │
│                                                                  │
│  After sync:                                                     │
│  Node A: counter[A]=5, counter[B]=3  → Total: 8                 │
│  Node B: counter[A]=5, counter[B]=3  → Total: 8                 │
│                                                                  │
│  No conflicts, just merge!                                       │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Client API

### Counter Operations

```c
// Increment a counter
kraftClientIncrementCounter(client, "counter:pageviews", 1);

// Get counter value
uint64_t value;
kraftClientGetCounter(client, "counter:pageviews", &value);

// Decrement (PN-Counter only)
kraftClientDecrementCounter(client, "gauge:stock", 5);
```

### Register Operations

```c
// Set a value with automatic timestamp
kraftClientSet(client, "user:123:profile", profileData, profileLen);

// Get current value
uint8_t *data;
size_t len;
kraftClientGet(client, "user:123:profile", &data, &len);
```

### Set Operations

```c
// Add to set
kraftClientSetAdd(client, "tags:post:1", "golang");
kraftClientSetAdd(client, "tags:post:1", "raft");

// Check membership
bool contains;
kraftClientSetContains(client, "tags:post:1", "golang", &contains);

// Get all members
char **members;
size_t count;
kraftClientSetMembers(client, "tags:post:1", &members, &count);

// Remove from set (OR-Set only)
kraftClientSetRemove(client, "cart:user:1", "item:123");
```

---

## Performance Considerations

### Memory Usage

```c
// CRDT memory overhead per type
typedef struct {
    kraftCRDTType type;
    size_t baseOverhead;
    size_t perElementOverhead;
} CRDTMemoryProfile;

CRDTMemoryProfile profiles[] = {
    {KRAFT_CRDT_GCOUNTER, 8 * MAX_NODES, 0},           // Fixed size
    {KRAFT_CRDT_PNCOUNTER, 16 * MAX_NODES, 0},         // Fixed size
    {KRAFT_CRDT_LWW_REGISTER, 24, 0},                  // Header only
    {KRAFT_CRDT_GSET, 24, 16},                         // Per element
    {KRAFT_CRDT_ORSET, 48, 24},                        // Per element + tag
    {KRAFT_CRDT_LWW_MAP, 32, 40},                      // Per key-value
};
```

### Compaction Effectiveness

```
┌─────────────────────────────────────────────────────────────────┐
│              Compaction Ratio by Type                            │
│                                                                  │
│  Type          │ Typical Compaction │ Best Case │ Worst Case   │
│  ──────────────┼────────────────────┼───────────┼─────────────  │
│  G-Counter     │ 100:1              │ 10000:1   │ 1:1          │
│  PN-Counter    │ 50:1               │ 5000:1    │ 1:1          │
│  LWW-Register  │ N:1 (N updates)    │ ∞:1       │ 1:1          │
│  G-Set         │ N:1 (N duplicates) │ 100:1     │ 1:1          │
│  OR-Set        │ 2-5:1              │ 10:1      │ 1:2          │
│  LWW-Map       │ N:1 per key        │ 100:1     │ 1:1          │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

---

## Best Practices

### Choosing the Right CRDT

```
┌─────────────────────────────────────────────────────────────────┐
│                     Decision Tree                                │
│                                                                  │
│  Do you need a counter?                                         │
│  ├─ Yes, only increment? → G-Counter                            │
│  └─ Yes, increment and decrement? → PN-Counter                  │
│                                                                  │
│  Do you need a single value?                                    │
│  └─ Yes → LWW-Register                                          │
│                                                                  │
│  Do you need a set/collection?                                  │
│  ├─ Elements never removed? → G-Set                             │
│  ├─ Elements removed once? → 2P-Set                             │
│  └─ Elements can be re-added? → OR-Set                          │
│                                                                  │
│  Do you need a key-value map?                                   │
│  └─ Yes → LWW-Map                                               │
│                                                                  │
└─────────────────────────────────────────────────────────────────┘
```

### Clock Synchronization

For LWW types, clock accuracy matters:

```c
// Use hybrid logical clocks for better ordering
typedef struct {
    uint64_t physical;    // Wall clock
    uint32_t logical;     // Logical counter
    uint32_t nodeId;      // Tie-breaker
} HybridTimestamp;

HybridTimestamp hlcNow(HybridTimestamp *last) {
    HybridTimestamp now;
    now.physical = wallClockNow();
    now.nodeId = getNodeId();

    if (now.physical > last->physical) {
        now.logical = 0;
    } else {
        now.physical = last->physical;
        now.logical = last->logical + 1;
    }

    *last = now;
    return now;
}
```

---

## Next Steps

- [Snapshots](snapshots.md) - CRDT-aware compaction
- [Multi-Raft](../concepts/multi-raft.md) - CRDTs across shards
- [API Reference](../reference/api.md) - CRDT API details
