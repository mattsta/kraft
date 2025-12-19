/* loopyKV - Demo Key-Value State Machine Implementation
 *
 * Simple hash table implementation for the demo KV store.
 */

#include "loopyKV.h"

#include <ctype.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ============================================================================
 * Hash Table Implementation
 * ============================================================================
 */

#define KV_INITIAL_BUCKETS 64
#define KV_LOAD_FACTOR 0.75

typedef struct kvEntry {
    char *key;
    size_t keyLen;
    char *value;
    size_t valueLen;
    struct kvEntry *next;
} kvEntry;

struct loopyKV {
    /* Hash table */
    kvEntry **buckets;
    size_t numBuckets;
    size_t count;

    /* State machine interface */
    loopyStateMachine sm;

    /* Statistics */
    loopyKVStats stats;

    /* Change callback */
    loopyKVChangeCallback *changeCallback;
    void *changeUserData;
};

/* FNV-1a hash */
static uint64_t kvHash(const char *key, size_t len) {
    uint64_t hash = 0xcbf29ce484222325ULL;
    for (size_t i = 0; i < len; i++) {
        hash ^= (uint8_t)key[i];
        hash *= 0x100000001b3ULL;
    }
    return hash;
}

static kvEntry *kvFindEntry(loopyKV *kv, const char *key, size_t keyLen) {
    uint64_t hash = kvHash(key, keyLen);
    size_t idx = hash % kv->numBuckets;

    kvEntry *entry = kv->buckets[idx];
    while (entry) {
        if (entry->keyLen == keyLen && memcmp(entry->key, key, keyLen) == 0) {
            return entry;
        }
        entry = entry->next;
    }
    return NULL;
}

static bool kvResize(loopyKV *kv) {
    size_t newNumBuckets = kv->numBuckets * 2;
    kvEntry **newBuckets = calloc(newNumBuckets, sizeof(kvEntry *));
    if (!newBuckets) {
        return false;
    }

    /* Rehash all entries */
    for (size_t i = 0; i < kv->numBuckets; i++) {
        kvEntry *entry = kv->buckets[i];
        while (entry) {
            kvEntry *next = entry->next;
            uint64_t hash = kvHash(entry->key, entry->keyLen);
            size_t idx = hash % newNumBuckets;
            entry->next = newBuckets[idx];
            newBuckets[idx] = entry;
            entry = next;
        }
    }

    free(kv->buckets);
    kv->buckets = newBuckets;
    kv->numBuckets = newNumBuckets;
    return true;
}

static bool kvSet(loopyKV *kv, const char *key, size_t keyLen,
                  const char *value, size_t valueLen) {
    /* Check load factor */
    if ((double)kv->count / kv->numBuckets > KV_LOAD_FACTOR) {
        if (!kvResize(kv)) {
            return false;
        }
    }

    /* Find or create entry */
    kvEntry *entry = kvFindEntry(kv, key, keyLen);

    if (entry) {
        /* Update existing */
        kv->stats.memoryUsed -= entry->valueLen;
        char *newValue = malloc(valueLen + 1);
        if (!newValue) {
            return false;
        }
        memcpy(newValue, value, valueLen);
        newValue[valueLen] = '\0';
        free(entry->value);
        entry->value = newValue;
        entry->valueLen = valueLen;
        kv->stats.memoryUsed += valueLen;
    } else {
        /* Create new entry */
        entry = malloc(sizeof(kvEntry));
        if (!entry) {
            return false;
        }

        entry->key = malloc(keyLen + 1);
        entry->value = malloc(valueLen + 1);
        if (!entry->key || !entry->value) {
            free(entry->key);
            free(entry->value);
            free(entry);
            return false;
        }

        memcpy(entry->key, key, keyLen);
        entry->key[keyLen] = '\0';
        entry->keyLen = keyLen;
        memcpy(entry->value, value, valueLen);
        entry->value[valueLen] = '\0';
        entry->valueLen = valueLen;

        uint64_t hash = kvHash(key, keyLen);
        size_t idx = hash % kv->numBuckets;
        entry->next = kv->buckets[idx];
        kv->buckets[idx] = entry;

        kv->count++;
        kv->stats.keyCount++;
        kv->stats.memoryUsed += keyLen + valueLen + sizeof(kvEntry);
    }

    kv->stats.setsTotal++;
    return true;
}

static bool kvDel(loopyKV *kv, const char *key, size_t keyLen) {
    uint64_t hash = kvHash(key, keyLen);
    size_t idx = hash % kv->numBuckets;

    kvEntry *prev = NULL;
    kvEntry *entry = kv->buckets[idx];

    while (entry) {
        if (entry->keyLen == keyLen && memcmp(entry->key, key, keyLen) == 0) {
            if (prev) {
                prev->next = entry->next;
            } else {
                kv->buckets[idx] = entry->next;
            }

            kv->stats.memoryUsed -=
                entry->keyLen + entry->valueLen + sizeof(kvEntry);
            kv->stats.keyCount--;
            kv->count--;

            free(entry->key);
            free(entry->value);
            free(entry);

            kv->stats.delsTotal++;
            return true;
        }
        prev = entry;
        entry = entry->next;
    }

    return false;
}

/* ============================================================================
 * State Machine Callbacks
 * ============================================================================
 */

static bool smApply(loopyStateMachine *sm, uint64_t index, const void *data,
                    size_t len, void **result, size_t *resultLen) {
    loopyKV *kv = (loopyKV *)sm->userData;

    if (!data || len < 1) {
        return false;
    }

    const uint8_t *p = (const uint8_t *)data;
    loopyKVCmd cmd = p[0];
    p++;
    len--;

    *result = NULL;
    *resultLen = 0;

    loopyKVStatus status = LOOPY_KV_OK;
    char *respValue = NULL;
    size_t respValueLen = 0;

    switch (cmd) {
    case LOOPY_KV_CMD_SET: {
        if (len < 4) {
            status = LOOPY_KV_ERR_INVALID;
            break;
        }

        uint16_t keyLen = p[0] | (p[1] << 8);
        p += 2;
        len -= 2;

        if (len < keyLen + 2) {
            status = LOOPY_KV_ERR_INVALID;
            break;
        }

        const char *key = (const char *)p;
        p += keyLen;
        len -= keyLen;

        uint16_t valueLen = p[0] | (p[1] << 8);
        p += 2;
        len -= 2;

        if (len < valueLen) {
            status = LOOPY_KV_ERR_INVALID;
            break;
        }

        const char *value = (const char *)p;

        fprintf(stderr, "[KV-SET] index=%lu key=%.*s value=%.*s\n", index,
                (int)keyLen, key, (int)valueLen, value);

        if (!kvSet(kv, key, keyLen, value, valueLen)) {
            status = LOOPY_KV_ERR_NOMEM;
            fprintf(stderr, "[KV-SET] FAILED - out of memory\n");
        } else {
            fprintf(stderr, "[KV-SET] SUCCESS\n");
            if (kv->changeCallback) {
                kv->changeCallback(kv, cmd, key, keyLen, value, valueLen,
                                   kv->changeUserData);
            }
        }
        break;
    }

    case LOOPY_KV_CMD_GET: {
        if (len < 2) {
            status = LOOPY_KV_ERR_INVALID;
            break;
        }

        uint16_t keyLen = p[0] | (p[1] << 8);
        p += 2;
        len -= 2;

        if (len < keyLen) {
            status = LOOPY_KV_ERR_INVALID;
            break;
        }

        const char *key = (const char *)p;
        kv->stats.getsTotal++;

        kvEntry *entry = kvFindEntry(kv, key, keyLen);
        if (entry) {
            fprintf(stderr, "[KV-GET] index=%lu key=%.*s FOUND value=%.*s\n",
                    index, (int)keyLen, key, (int)entry->valueLen,
                    entry->value);
            kv->stats.hitsTotal++;
            respValue = malloc(entry->valueLen);
            if (respValue) {
                memcpy(respValue, entry->value, entry->valueLen);
                respValueLen = entry->valueLen;
            } else {
                status = LOOPY_KV_ERR_NOMEM;
            }
        } else {
            kv->stats.missesTotal++;
            status = LOOPY_KV_ERR_NOTFOUND;
        }
        break;
    }

    case LOOPY_KV_CMD_DEL: {
        if (len < 2) {
            status = LOOPY_KV_ERR_INVALID;
            break;
        }

        uint16_t keyLen = p[0] | (p[1] << 8);
        p += 2;
        len -= 2;

        if (len < keyLen) {
            status = LOOPY_KV_ERR_INVALID;
            break;
        }

        const char *key = (const char *)p;

        if (kvDel(kv, key, keyLen)) {
            if (kv->changeCallback) {
                kv->changeCallback(kv, cmd, key, keyLen, NULL, 0,
                                   kv->changeUserData);
            }
        } else {
            status = LOOPY_KV_ERR_NOTFOUND;
        }
        break;
    }

    case LOOPY_KV_CMD_INCR: {
        if (len < 2) {
            status = LOOPY_KV_ERR_INVALID;
            break;
        }

        uint16_t keyLen = p[0] | (p[1] << 8);
        p += 2;
        len -= 2;

        if (len < keyLen) {
            status = LOOPY_KV_ERR_INVALID;
            break;
        }

        const char *key = (const char *)p;
        p += keyLen;
        len -= keyLen;

        int64_t amount = 1;
        if (len >= 8) {
            memcpy(&amount, p, sizeof(amount));
        }

        kvEntry *entry = kvFindEntry(kv, key, keyLen);
        int64_t value = 0;

        if (entry) {
            /* Parse existing value as number */
            char *endptr;
            value = strtoll(entry->value, &endptr, 10);
            if (*endptr != '\0') {
                status = LOOPY_KV_ERR_TYPE;
                break;
            }
        }

        value += amount;

        /* Store new value */
        char buf[32];
        int bufLen = snprintf(buf, sizeof(buf), "%" PRId64, value);
        if (!kvSet(kv, key, keyLen, buf, (size_t)bufLen)) {
            status = LOOPY_KV_ERR_NOMEM;
        } else {
            /* Return new value */
            respValue = malloc((size_t)bufLen);
            if (respValue) {
                memcpy(respValue, buf, (size_t)bufLen);
                respValueLen = (size_t)bufLen;
            }
            if (kv->changeCallback) {
                kv->changeCallback(kv, cmd, key, keyLen, buf, (size_t)bufLen,
                                   kv->changeUserData);
            }
        }
        break;
    }

    case LOOPY_KV_CMD_APPEND: {
        if (len < 4) {
            status = LOOPY_KV_ERR_INVALID;
            break;
        }

        uint16_t keyLen = p[0] | (p[1] << 8);
        p += 2;
        len -= 2;

        if (len < keyLen + 2) {
            status = LOOPY_KV_ERR_INVALID;
            break;
        }

        const char *key = (const char *)p;
        p += keyLen;
        len -= keyLen;

        uint16_t appendLen = p[0] | (p[1] << 8);
        p += 2;
        len -= 2;

        if (len < appendLen) {
            status = LOOPY_KV_ERR_INVALID;
            break;
        }

        const char *appendValue = (const char *)p;

        kvEntry *entry = kvFindEntry(kv, key, keyLen);
        if (entry) {
            /* Check for integer overflow before append */
            if (appendLen > SIZE_MAX - entry->valueLen) {
                status = LOOPY_KV_ERR_NOMEM;
                break;
            }

            /* Append to existing */
            size_t newLen = entry->valueLen + appendLen;
            char *newValue = malloc(newLen + 1);
            if (!newValue) {
                status = LOOPY_KV_ERR_NOMEM;
                break;
            }
            memcpy(newValue, entry->value, entry->valueLen);
            memcpy(newValue + entry->valueLen, appendValue, appendLen);
            newValue[newLen] = '\0';

            kv->stats.memoryUsed -= entry->valueLen;
            free(entry->value);
            entry->value = newValue;
            entry->valueLen = newLen;
            kv->stats.memoryUsed += newLen;
            kv->stats.setsTotal++;

            if (kv->changeCallback) {
                kv->changeCallback(kv, cmd, key, keyLen, newValue, newLen,
                                   kv->changeUserData);
            }
        } else {
            /* Create new with appended value */
            if (!kvSet(kv, key, keyLen, appendValue, appendLen)) {
                status = LOOPY_KV_ERR_NOMEM;
            } else if (kv->changeCallback) {
                kv->changeCallback(kv, cmd, key, keyLen, appendValue, appendLen,
                                   kv->changeUserData);
            }
        }
        break;
    }

    case LOOPY_KV_CMD_STAT: {
        char buf[512];
        int bufLen = snprintf(
            buf, sizeof(buf),
            "keys:%zu\n"
            "memory:%zu\n"
            "sets:%zu\n"
            "gets:%zu\n"
            "dels:%zu\n"
            "hits:%zu\n"
            "misses:%zu\n"
            "last_applied:%zu\n",
            (size_t)kv->stats.keyCount, (size_t)kv->stats.memoryUsed,
            (size_t)kv->stats.setsTotal, (size_t)kv->stats.getsTotal,
            (size_t)kv->stats.delsTotal, (size_t)kv->stats.hitsTotal,
            (size_t)kv->stats.missesTotal, (size_t)kv->stats.lastAppliedIndex);
        respValue = malloc((size_t)bufLen);
        if (respValue) {
            memcpy(respValue, buf, (size_t)bufLen);
            respValueLen = (size_t)bufLen;
        }
        break;
    }

    case LOOPY_KV_CMD_KEYS:
        /* TODO: Implement pattern matching */
        status = LOOPY_KV_ERR_INVALID;
        break;

    default:
        status = LOOPY_KV_ERR_INVALID;
        break;
    }

    /* Build response */
    size_t respLen = 1 + 2 + respValueLen;
    uint8_t *resp = malloc(respLen);
    if (!resp) {
        free(respValue);
        return false;
    }

    resp[0] = (uint8_t)status;
    resp[1] = (uint8_t)(respValueLen & 0xFF);
    resp[2] = (uint8_t)((respValueLen >> 8) & 0xFF);
    if (respValue && respValueLen > 0) {
        memcpy(resp + 3, respValue, respValueLen);
    }
    free(respValue);

    *result = resp;
    *resultLen = respLen;

    kv->stats.lastAppliedIndex = index;
    return true;
}

static bool smSnapshot(loopyStateMachine *sm, void **data, size_t *len,
                       uint64_t *lastIndex, uint64_t *lastTerm) {
    loopyKV *kv = (loopyKV *)sm->userData;

    /* Calculate snapshot size */
    size_t totalSize = sizeof(uint64_t); /* key count */
    for (size_t i = 0; i < kv->numBuckets; i++) {
        kvEntry *entry = kv->buckets[i];
        while (entry) {
            totalSize += 2 + entry->keyLen + 2 + entry->valueLen;
            entry = entry->next;
        }
    }

    /* Allocate snapshot */
    uint8_t *snapshot = malloc(totalSize);
    if (!snapshot) {
        return false;
    }

    uint8_t *p = snapshot;

    /* Write key count */
    uint64_t count = kv->count;
    memcpy(p, &count, sizeof(count));
    p += sizeof(count);

    /* Write all entries */
    for (size_t i = 0; i < kv->numBuckets; i++) {
        kvEntry *entry = kv->buckets[i];
        while (entry) {
            uint16_t keyLen = (uint16_t)entry->keyLen;
            uint16_t valueLen = (uint16_t)entry->valueLen;

            p[0] = keyLen & 0xFF;
            p[1] = (keyLen >> 8) & 0xFF;
            p += 2;

            memcpy(p, entry->key, entry->keyLen);
            p += entry->keyLen;

            p[0] = valueLen & 0xFF;
            p[1] = (valueLen >> 8) & 0xFF;
            p += 2;

            memcpy(p, entry->value, entry->valueLen);
            p += entry->valueLen;

            entry = entry->next;
        }
    }

    *data = snapshot;
    *len = totalSize;
    *lastIndex = kv->stats.lastAppliedIndex;
    *lastTerm = 0; /* TODO: Track term */

    kv->stats.snapshotsCreated++;
    return true;
}

static bool smRestore(loopyStateMachine *sm, const void *data, size_t len,
                      uint64_t lastIndex, uint64_t lastTerm) {
    loopyKV *kv = (loopyKV *)sm->userData;
    (void)lastTerm;

    if (!data || len < sizeof(uint64_t)) {
        return false;
    }

    /* Clear existing data */
    for (size_t i = 0; i < kv->numBuckets; i++) {
        kvEntry *entry = kv->buckets[i];
        while (entry) {
            kvEntry *next = entry->next;
            free(entry->key);
            free(entry->value);
            free(entry);
            entry = next;
        }
        kv->buckets[i] = NULL;
    }
    kv->count = 0;
    kv->stats.keyCount = 0;
    kv->stats.memoryUsed = 0;

    const uint8_t *p = (const uint8_t *)data;

    /* Read key count */
    uint64_t count;
    memcpy(&count, p, sizeof(count));
    p += sizeof(count);
    len -= sizeof(count);

    /* Read all entries */
    for (uint64_t i = 0; i < count && len >= 4; i++) {
        uint16_t keyLen = p[0] | (p[1] << 8);
        p += 2;
        len -= 2;

        if (len < keyLen + 2) {
            break;
        }

        const char *key = (const char *)p;
        p += keyLen;
        len -= keyLen;

        uint16_t valueLen = p[0] | (p[1] << 8);
        p += 2;
        len -= 2;

        if (len < valueLen) {
            break;
        }

        const char *value = (const char *)p;
        p += valueLen;
        len -= valueLen;

        kvSet(kv, key, keyLen, value, valueLen);
    }

    kv->stats.lastAppliedIndex = lastIndex;
    kv->stats.snapshotsRestored++;
    return true;
}

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

loopyKV *loopyKVCreate(void) {
    loopyKV *kv = calloc(1, sizeof(*kv));
    if (!kv) {
        return NULL;
    }

    kv->buckets = calloc(KV_INITIAL_BUCKETS, sizeof(kvEntry *));
    if (!kv->buckets) {
        free(kv);
        return NULL;
    }
    kv->numBuckets = KV_INITIAL_BUCKETS;

    /* Setup state machine interface */
    kv->sm.apply = smApply;
    kv->sm.snapshot = smSnapshot;
    kv->sm.restore = smRestore;
    kv->sm.userData = kv;

    return kv;
}

void loopyKVDestroy(loopyKV *kv) {
    if (!kv) {
        return;
    }

    /* Free all entries */
    for (size_t i = 0; i < kv->numBuckets; i++) {
        kvEntry *entry = kv->buckets[i];
        while (entry) {
            kvEntry *next = entry->next;
            free(entry->key);
            free(entry->value);
            free(entry);
            entry = next;
        }
    }

    free(kv->buckets);
    free(kv);
}

loopyStateMachine *loopyKVGetStateMachine(loopyKV *kv) {
    if (!kv) {
        return NULL;
    }
    return &kv->sm;
}

void loopyKVSetChangeCallback(loopyKV *kv, loopyKVChangeCallback *cb,
                              void *userData) {
    if (!kv) {
        return;
    }
    kv->changeCallback = cb;
    kv->changeUserData = userData;
}

/* ============================================================================
 * Direct API
 * ============================================================================
 */

bool loopyKVGet(loopyKV *kv, const char *key, size_t keyLen, char **value,
                size_t *valueLen) {
    if (!kv || !key || !value || !valueLen) {
        return false;
    }

    kvEntry *entry = kvFindEntry(kv, key, keyLen);
    if (!entry) {
        return false;
    }

    *value = malloc(entry->valueLen + 1);
    if (!*value) {
        return false;
    }

    memcpy(*value, entry->value, entry->valueLen);
    (*value)[entry->valueLen] = '\0';
    *valueLen = entry->valueLen;
    return true;
}

size_t loopyKVCount(const loopyKV *kv) {
    if (!kv) {
        return 0;
    }
    return kv->count;
}

void loopyKVGetStats(const loopyKV *kv, loopyKVStats *stats) {
    if (!stats) {
        return;
    }
    if (!kv) {
        memset(stats, 0, sizeof(*stats));
        return;
    }
    *stats = kv->stats;
}

/* ============================================================================
 * Command Encoding
 * ============================================================================
 */

bool loopyKVEncodeSet(const char *key, size_t keyLen, const char *value,
                      size_t valueLen, void **data, size_t *dataLen) {
    if (!key || !value || !data || !dataLen) {
        return false;
    }
    if (keyLen > 65535 || valueLen > 65535) {
        return false;
    }

    size_t len = 1 + 2 + keyLen + 2 + valueLen;
    uint8_t *buf = malloc(len);
    if (!buf) {
        return false;
    }

    uint8_t *p = buf;
    *p++ = LOOPY_KV_CMD_SET;
    *p++ = keyLen & 0xFF;
    *p++ = (keyLen >> 8) & 0xFF;
    memcpy(p, key, keyLen);
    p += keyLen;
    *p++ = valueLen & 0xFF;
    *p++ = (valueLen >> 8) & 0xFF;
    memcpy(p, value, valueLen);

    *data = buf;
    *dataLen = len;
    return true;
}

bool loopyKVEncodeGet(const char *key, size_t keyLen, void **data,
                      size_t *dataLen) {
    if (!key || !data || !dataLen) {
        return false;
    }
    if (keyLen > 65535) {
        return false;
    }

    size_t len = 1 + 2 + keyLen;
    uint8_t *buf = malloc(len);
    if (!buf) {
        return false;
    }

    uint8_t *p = buf;
    *p++ = LOOPY_KV_CMD_GET;
    *p++ = keyLen & 0xFF;
    *p++ = (keyLen >> 8) & 0xFF;
    memcpy(p, key, keyLen);

    *data = buf;
    *dataLen = len;
    return true;
}

bool loopyKVEncodeDel(const char *key, size_t keyLen, void **data,
                      size_t *dataLen) {
    if (!key || !data || !dataLen) {
        return false;
    }
    if (keyLen > 65535) {
        return false;
    }

    size_t len = 1 + 2 + keyLen;
    uint8_t *buf = malloc(len);
    if (!buf) {
        return false;
    }

    uint8_t *p = buf;
    *p++ = LOOPY_KV_CMD_DEL;
    *p++ = keyLen & 0xFF;
    *p++ = (keyLen >> 8) & 0xFF;
    memcpy(p, key, keyLen);

    *data = buf;
    *dataLen = len;
    return true;
}

bool loopyKVEncodeIncr(const char *key, size_t keyLen, int64_t amount,
                       void **data, size_t *dataLen) {
    if (!key || !data || !dataLen) {
        return false;
    }
    if (keyLen > 65535) {
        return false;
    }

    size_t len = 1 + 2 + keyLen + 8;
    uint8_t *buf = malloc(len);
    if (!buf) {
        return false;
    }

    uint8_t *p = buf;
    *p++ = LOOPY_KV_CMD_INCR;
    *p++ = keyLen & 0xFF;
    *p++ = (keyLen >> 8) & 0xFF;
    memcpy(p, key, keyLen);
    p += keyLen;
    memcpy(p, &amount, sizeof(amount));

    *data = buf;
    *dataLen = len;
    return true;
}

bool loopyKVDecodeResponse(const void *data, size_t dataLen,
                           loopyKVStatus *status, char **value,
                           size_t *valueLen) {
    if (!data || dataLen < 3 || !status) {
        return false;
    }

    const uint8_t *p = (const uint8_t *)data;
    *status = (loopyKVStatus)p[0];

    uint16_t len = p[1] | (p[2] << 8);
    if (dataLen < 3 + len) {
        return false;
    }

    if (value && valueLen) {
        if (len > 0) {
            *value = malloc(len + 1);
            if (!*value) {
                return false;
            }
            memcpy(*value, p + 3, len);
            (*value)[len] = '\0';
        } else {
            *value = NULL;
        }
        *valueLen = len;
    }

    return true;
}

/* ============================================================================
 * Text Command Parsing
 * ============================================================================
 */

static bool skipWhitespace(const char **p, const char *end) {
    while (*p < end && isspace((unsigned char)**p)) {
        (*p)++;
    }
    return *p < end;
}

static bool parseToken(const char **p, const char *end, char *buf,
                       size_t bufSize) {
    if (!skipWhitespace(p, end)) {
        return false;
    }

    size_t i = 0;
    while (*p < end && !isspace((unsigned char)**p) && i < bufSize - 1) {
        buf[i++] = *(*p)++;
    }
    buf[i] = '\0';
    return i > 0;
}

/* Protocol maximum key/value sizes (as uint16_t) */
#define MAX_KEY_LEN 65535
#define MAX_VALUE_LEN 65535

/**
 * parseQuotedOrTokenDynamic - Parse a quoted or unquoted token with dynamic
 * allocation
 *
 * This function parses the next token from the input, handling both quoted
 * strings (with escape sequences) and unquoted whitespace-delimited tokens.
 * Memory is dynamically allocated to accommodate tokens of any size up to the
 * protocol maximum.
 *
 * @param p       Pointer to current parse position (updated on success)
 * @param end     Pointer to end of input
 * @param outBuf  Output: dynamically allocated buffer (caller must free)
 * @param outLen  Output: length of parsed token
 * @param maxLen  Maximum allowed length (fails if exceeded)
 * @return        true on success, false on failure (no token, too long, or OOM)
 */
static bool parseQuotedOrTokenDynamic(const char **p, const char *end,
                                      char **outBuf, size_t *outLen,
                                      size_t maxLen) {
    if (!skipWhitespace(p, end)) {
        return false;
    }

    /* Initial allocation - will grow as needed */
    size_t capacity = 256;
    char *buf = malloc(capacity);
    if (!buf) {
        return false;
    }

    size_t len = 0;
    bool quoted = (**p == '"');

    if (quoted) {
        /* Quoted string - skip opening quote */
        (*p)++;
        while (*p < end && **p != '"') {
            /* Handle escape sequences */
            if (**p == '\\' && *p + 1 < end) {
                (*p)++;
            }

            /* Grow buffer if needed */
            if (len >= capacity - 1) {
                size_t newCapacity = capacity * 2;
                if (newCapacity > maxLen + 1) {
                    newCapacity = maxLen + 1;
                }
                if (len >= newCapacity - 1) {
                    /* Token too long */
                    free(buf);
                    return false;
                }
                char *newBuf = realloc(buf, newCapacity);
                if (!newBuf) {
                    free(buf);
                    return false;
                }
                buf = newBuf;
                capacity = newCapacity;
            }

            buf[len++] = *(*p)++;
        }
        /* Skip closing quote if present */
        if (*p < end && **p == '"') {
            (*p)++;
        }
    } else {
        /* Unquoted token - read until whitespace */
        while (*p < end && !isspace((unsigned char)**p)) {
            /* Grow buffer if needed */
            if (len >= capacity - 1) {
                size_t newCapacity = capacity * 2;
                if (newCapacity > maxLen + 1) {
                    newCapacity = maxLen + 1;
                }
                if (len >= newCapacity - 1) {
                    /* Token too long */
                    free(buf);
                    return false;
                }
                char *newBuf = realloc(buf, newCapacity);
                if (!newBuf) {
                    free(buf);
                    return false;
                }
                buf = newBuf;
                capacity = newCapacity;
            }

            buf[len++] = *(*p)++;
        }
    }

    if (len == 0) {
        free(buf);
        return false;
    }

    buf[len] = '\0';
    *outBuf = buf;
    *outLen = len;
    return true;
}

/**
 * loopyKVParseCommand - Parse a text command into binary format
 *
 * Parses human-readable KV commands (SET, GET, DEL, INCR, DECR, STAT) into
 * binary wire format. Uses dynamic allocation internally to support keys
 * and values up to the protocol maximum (65535 bytes each).
 *
 * @param text     Command text to parse
 * @param textLen  Length of command text
 * @param data     Output: dynamically allocated binary command (caller must
 * free)
 * @param dataLen  Output: length of binary command
 * @return         true on success, false on parse error or OOM
 */
bool loopyKVParseCommand(const char *text, size_t textLen, void **data,
                         size_t *dataLen) {
    if (!text || textLen == 0 || !data || !dataLen) {
        return false;
    }

    const char *p = text;
    const char *end = text + textLen;

    char cmd[16];
    if (!parseToken(&p, end, cmd, sizeof(cmd))) {
        return false;
    }

    /* Convert to uppercase */
    for (char *c = cmd; *c; c++) {
        *c = (char)toupper((unsigned char)*c);
    }

    if (strcmp(cmd, "SET") == 0) {
        char *key = NULL;
        size_t keyLen = 0;
        char *value = NULL;
        size_t valueLen = 0;
        bool result = false;

        if (!parseQuotedOrTokenDynamic(&p, end, &key, &keyLen, MAX_KEY_LEN)) {
            goto set_cleanup;
        }
        if (!parseQuotedOrTokenDynamic(&p, end, &value, &valueLen,
                                       MAX_VALUE_LEN)) {
            goto set_cleanup;
        }

        result = loopyKVEncodeSet(key, keyLen, value, valueLen, data, dataLen);

    set_cleanup:
        free(key);
        free(value);
        return result;
    }

    if (strcmp(cmd, "GET") == 0) {
        char *key = NULL;
        size_t keyLen = 0;

        if (!parseQuotedOrTokenDynamic(&p, end, &key, &keyLen, MAX_KEY_LEN)) {
            return false;
        }

        bool result = loopyKVEncodeGet(key, keyLen, data, dataLen);
        free(key);
        return result;
    }

    if (strcmp(cmd, "DEL") == 0) {
        char *key = NULL;
        size_t keyLen = 0;

        if (!parseQuotedOrTokenDynamic(&p, end, &key, &keyLen, MAX_KEY_LEN)) {
            return false;
        }

        bool result = loopyKVEncodeDel(key, keyLen, data, dataLen);
        free(key);
        return result;
    }

    if (strcmp(cmd, "INCR") == 0 || strcmp(cmd, "DECR") == 0) {
        char *key = NULL;
        size_t keyLen = 0;

        if (!parseQuotedOrTokenDynamic(&p, end, &key, &keyLen, MAX_KEY_LEN)) {
            return false;
        }

        int64_t amount = (strcmp(cmd, "DECR") == 0) ? -1 : 1;

        /* Try to parse optional amount argument */
        char *amountStr = NULL;
        size_t amountLen = 0;
        if (parseQuotedOrTokenDynamic(&p, end, &amountStr, &amountLen, 32)) {
            amount = strtoll(amountStr, NULL, 10);
            if (strcmp(cmd, "DECR") == 0) {
                amount = -amount;
            }
            free(amountStr);
        }

        bool result = loopyKVEncodeIncr(key, keyLen, amount, data, dataLen);
        free(key);
        return result;
    }

    if (strcmp(cmd, "STAT") == 0 || strcmp(cmd, "STATS") == 0 ||
        strcmp(cmd, "INFO") == 0) {
        uint8_t *buf = malloc(1);
        if (!buf) {
            return false;
        }
        buf[0] = LOOPY_KV_CMD_STAT;
        *data = buf;
        *dataLen = 1;
        return true;
    }

    return false;
}

bool loopyKVFormatResponse(const void *data, size_t dataLen, char **text) {
    if (!data || dataLen < 3 || !text) {
        return false;
    }

    loopyKVStatus status;
    char *value = NULL;
    size_t valueLen = 0;

    if (!loopyKVDecodeResponse(data, dataLen, &status, &value, &valueLen)) {
        return false;
    }

    char *result;
    if (status == LOOPY_KV_OK) {
        if (value && valueLen > 0) {
            result = malloc(valueLen + 1);
            if (!result) {
                free(value);
                return false;
            }
            memcpy(result, value, valueLen);
            result[valueLen] = '\0';
        } else {
            result = strdup("OK");
        }
    } else {
        const char *errName = loopyKVStatusName(status);
        result = malloc(strlen(errName) + 8);
        if (result) {
            sprintf(result, "ERROR: %s", errName);
        }
    }

    free(value);
    *text = result;
    return result != NULL;
}

/* ============================================================================
 * Debugging
 * ============================================================================
 */

void loopyKVDump(const loopyKV *kv) {
    if (!kv) {
        printf("(null kv)\n");
        return;
    }

    printf("=== KV Store Dump ===\n");
    printf("Keys: %zu, Buckets: %zu, Memory: %zu bytes\n", kv->count,
           kv->numBuckets, (size_t)kv->stats.memoryUsed);
    printf("\n");

    for (size_t i = 0; i < kv->numBuckets; i++) {
        kvEntry *entry = kv->buckets[i];
        while (entry) {
            printf("  %.*s = %.*s\n", (int)entry->keyLen, entry->key,
                   (int)entry->valueLen, entry->value);
            entry = entry->next;
        }
    }
}

const char *loopyKVCmdName(loopyKVCmd cmd) {
    switch (cmd) {
    case LOOPY_KV_CMD_SET:
        return "SET";
    case LOOPY_KV_CMD_GET:
        return "GET";
    case LOOPY_KV_CMD_DEL:
        return "DEL";
    case LOOPY_KV_CMD_INCR:
        return "INCR";
    case LOOPY_KV_CMD_APPEND:
        return "APPEND";
    case LOOPY_KV_CMD_KEYS:
        return "KEYS";
    case LOOPY_KV_CMD_STAT:
        return "STAT";
    default:
        return "UNKNOWN";
    }
}

const char *loopyKVStatusName(loopyKVStatus status) {
    switch (status) {
    case LOOPY_KV_OK:
        return "OK";
    case LOOPY_KV_ERR_NOTFOUND:
        return "NOTFOUND";
    case LOOPY_KV_ERR_INVALID:
        return "INVALID";
    case LOOPY_KV_ERR_TYPE:
        return "TYPE";
    case LOOPY_KV_ERR_NOMEM:
        return "NOMEM";
    default:
        return "UNKNOWN";
    }
}
