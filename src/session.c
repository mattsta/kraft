#include "session.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

/* =========================================================================
 * Internal Helpers
 * ========================================================================= */

static uint64_t sessionNowUs(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

/* Simple hash for session lookup */
static uint32_t sessionHash(kraftSessionId id, uint32_t capacity) {
    /* FNV-1a inspired hash */
    uint64_t hash = 14695981039346656037ULL;
    hash ^= id;
    hash *= 1099511628211ULL;
    return (uint32_t)(hash % capacity);
}

/* Find session slot (returns NULL if not found) */
static kraftSession *findSession(kraftSessionManager *mgr,
                                 kraftSessionId sessionId) {
    if (!mgr->sessions || mgr->capacity == 0) {
        return NULL;
    }

    uint32_t idx = sessionHash(sessionId, mgr->capacity);
    uint32_t start = idx;

    do {
        kraftSession *s = &mgr->sessions[idx];
        if (s->sessionId == sessionId && s->active) {
            return s;
        }
        if (s->sessionId == 0 && !s->active) {
            /* Empty slot, session not found */
            return NULL;
        }
        idx = (idx + 1) % mgr->capacity;
    } while (idx != start);

    return NULL;
}

/* Find empty slot for new session */
static kraftSession *findEmptySlot(kraftSessionManager *mgr,
                                   uint32_t startIdx) {
    uint32_t idx = startIdx;
    uint32_t attempts = 0;

    while (attempts < mgr->capacity) {
        kraftSession *s = &mgr->sessions[idx];
        if (!s->active) {
            return s;
        }
        idx = (idx + 1) % mgr->capacity;
        attempts++;
    }

    return NULL;
}

/* Free cached responses for a session */
static void freeCachedResponses(kraftSession *session) {
    if (!session->cache) {
        return;
    }

    for (uint32_t i = 0; i < session->cacheSize; i++) {
        free(session->cache[i].data);
    }
    free(session->cache);
    session->cache = NULL;
    session->cacheSize = 0;
    session->cacheCapacity = 0;
}

/* =========================================================================
 * Configuration
 * ========================================================================= */

kraftSessionConfig kraftSessionConfigDefault(void) {
    return (kraftSessionConfig){
        .maxSessions = KRAFT_SESSION_DEFAULT_MAX,
        .maxCachedPerSession = KRAFT_SESSION_DEFAULT_CACHE,
        .sessionTimeoutUs = KRAFT_SESSION_DEFAULT_TIMEOUT_US,
        .cacheRetentionUs = KRAFT_SESSION_DEFAULT_RETENTION_US};
}

/* =========================================================================
 * Initialization & Lifecycle
 * ========================================================================= */

bool kraftSessionManagerInit(kraftSessionManager *mgr,
                             const kraftSessionConfig *config) {
    memset(mgr, 0, sizeof(*mgr));

    if (config) {
        mgr->config = *config;
    } else {
        mgr->config = kraftSessionConfigDefault();
    }

    /* Allocate hash table with 1.5x capacity for load factor */
    mgr->capacity = (mgr->config.maxSessions * 3) / 2;
    if (mgr->capacity < 16) {
        mgr->capacity = 16;
    }

    mgr->sessions = calloc(mgr->capacity, sizeof(kraftSession));
    if (!mgr->sessions) {
        return false;
    }

    /* Start session IDs from 1 (0 is invalid) */
    mgr->nextSessionId = 1;

    return true;
}

void kraftSessionManagerFree(kraftSessionManager *mgr) {
    if (!mgr->sessions) {
        return;
    }

    /* Free all cached responses */
    for (uint32_t i = 0; i < mgr->capacity; i++) {
        if (mgr->sessions[i].active) {
            freeCachedResponses(&mgr->sessions[i]);
        }
    }

    free(mgr->sessions);
    memset(mgr, 0, sizeof(*mgr));
}

/* =========================================================================
 * Session Management
 * ========================================================================= */

kraftSessionId kraftSessionRegister(kraftSessionManager *mgr) {
    if (mgr->sessionCount >= mgr->config.maxSessions) {
        return 0; /* At capacity */
    }

    kraftSessionId id = mgr->nextSessionId++;
    uint32_t idx = sessionHash(id, mgr->capacity);
    kraftSession *slot = findEmptySlot(mgr, idx);

    if (!slot) {
        return 0; /* No empty slot (shouldn't happen with proper load factor) */
    }

    /* Initialize session */
    memset(slot, 0, sizeof(*slot));
    slot->sessionId = id;
    slot->nextSeqNum = 1; /* Start sequence at 1 */
    slot->lastActivity = sessionNowUs();
    slot->active = true;

    /* Pre-allocate small cache */
    slot->cacheCapacity = 4;
    slot->cache = calloc(slot->cacheCapacity, sizeof(kraftCachedResponse));

    mgr->sessionCount++;
    mgr->sessionsCreated++;

    return id;
}

bool kraftSessionUnregister(kraftSessionManager *mgr,
                            kraftSessionId sessionId) {
    kraftSession *session = findSession(mgr, sessionId);
    if (!session) {
        return false;
    }

    freeCachedResponses(session);
    session->active = false;
    session->sessionId = 0;
    mgr->sessionCount--;

    return true;
}

kraftSessionStatus kraftSessionGetStatus(kraftSessionManager *mgr,
                                         kraftSessionId sessionId) {
    kraftSession *session = findSession(mgr, sessionId);
    if (!session) {
        return KRAFT_SESSION_UNKNOWN;
    }

    uint64_t now = sessionNowUs();
    if (now - session->lastActivity > mgr->config.sessionTimeoutUs) {
        return KRAFT_SESSION_EXPIRED;
    }

    return KRAFT_SESSION_ACTIVE;
}

void kraftSessionTouch(kraftSessionManager *mgr, kraftSessionId sessionId) {
    kraftSession *session = findSession(mgr, sessionId);
    if (session) {
        session->lastActivity = sessionNowUs();
    }
}

uint32_t kraftSessionExpireStale(kraftSessionManager *mgr, uint64_t nowUs) {
    uint32_t expired = 0;

    for (uint32_t i = 0; i < mgr->capacity; i++) {
        kraftSession *s = &mgr->sessions[i];
        if (!s->active) {
            continue;
        }

        if (nowUs - s->lastActivity > mgr->config.sessionTimeoutUs) {
            freeCachedResponses(s);
            s->active = false;
            s->sessionId = 0;
            mgr->sessionCount--;
            mgr->sessionsExpired++;
            expired++;
        }
    }

    return expired;
}

/* =========================================================================
 * Request Processing
 * ========================================================================= */

bool kraftSessionIsDuplicate(kraftSessionManager *mgr, kraftSessionId sessionId,
                             kraftSeqNum seqNum, void **cachedData,
                             size_t *cachedLen) {
    kraftSession *session = findSession(mgr, sessionId);
    if (!session) {
        return false; /* Unknown session - not a duplicate */
    }

    /* Update activity */
    session->lastActivity = sessionNowUs();

    /* Check if this seqNum has already been processed */
    if (seqNum < session->nextSeqNum) {
        /* This is a duplicate - look for cached response */
        for (uint32_t i = 0; i < session->cacheSize; i++) {
            if (session->cache[i].seqNum == seqNum) {
                *cachedData = session->cache[i].data;
                *cachedLen = session->cache[i].len;
                mgr->duplicatesDetected++;
                return true;
            }
        }
        /* Duplicate but cache expired - treat as duplicate anyway */
        *cachedData = NULL;
        *cachedLen = 0;
        mgr->duplicatesDetected++;
        return true;
    }

    if (seqNum > session->nextSeqNum) {
        /* Out of order - gap in sequence
         * For strict ordering, this is an error.
         * For now, treat as new request. */
    }

    return false; /* New request */
}

bool kraftSessionRecordResponse(kraftSessionManager *mgr,
                                kraftSessionId sessionId, kraftSeqNum seqNum,
                                const void *data, size_t len) {
    kraftSession *session = findSession(mgr, sessionId);
    if (!session) {
        return false;
    }

    /* Update next expected sequence number */
    if (seqNum >= session->nextSeqNum) {
        session->nextSeqNum = seqNum + 1;
    }

    /* Grow cache if needed */
    if (session->cacheSize >= session->cacheCapacity) {
        if (session->cacheCapacity >= mgr->config.maxCachedPerSession) {
            /* At max - evict oldest */
            free(session->cache[0].data);
            memmove(&session->cache[0], &session->cache[1],
                    (session->cacheSize - 1) * sizeof(kraftCachedResponse));
            session->cacheSize--;
        } else {
            /* Grow */
            uint32_t newCapacity = session->cacheCapacity * 2;
            if (newCapacity > mgr->config.maxCachedPerSession) {
                newCapacity = mgr->config.maxCachedPerSession;
            }
            kraftCachedResponse *newCache = realloc(
                session->cache, newCapacity * sizeof(kraftCachedResponse));
            if (!newCache) {
                return false;
            }
            session->cache = newCache;
            session->cacheCapacity = newCapacity;
        }
    }

    /* Add cached response */
    kraftCachedResponse *resp = &session->cache[session->cacheSize];
    resp->seqNum = seqNum;
    resp->timestamp = sessionNowUs();

    if (data && len > 0) {
        resp->data = malloc(len);
        if (!resp->data) {
            return false;
        }
        memcpy(resp->data, data, len);
        resp->len = len;
    } else {
        resp->data = NULL;
        resp->len = 0;
    }

    session->cacheSize++;
    mgr->requestsProcessed++;

    return true;
}

kraftSeqNum kraftSessionGetNextSeqNum(kraftSessionManager *mgr,
                                      kraftSessionId sessionId) {
    kraftSession *session = findSession(mgr, sessionId);
    if (!session) {
        return 0;
    }
    return session->nextSeqNum;
}

/* =========================================================================
 * Statistics
 * ========================================================================= */

void kraftSessionGetStats(const kraftSessionManager *mgr,
                          kraftSessionStats *stats) {
    memset(stats, 0, sizeof(*stats));

    stats->activeSessions = mgr->sessionCount;
    stats->totalCreated = mgr->sessionsCreated;
    stats->totalExpired = mgr->sessionsExpired;
    stats->duplicatesDetected = mgr->duplicatesDetected;
    stats->requestsProcessed = mgr->requestsProcessed;

    uint64_t total = stats->requestsProcessed + stats->duplicatesDetected;
    if (total > 0) {
        stats->duplicateRate =
            (double)stats->duplicatesDetected / (double)total;
    }
}

int kraftSessionFormatStats(const kraftSessionStats *stats, char *buf,
                            size_t bufLen) {
    return snprintf(buf, bufLen,
                    "Session Stats:\n"
                    "  Active Sessions: %u\n"
                    "  Total Created: %llu\n"
                    "  Total Expired: %llu\n"
                    "  Requests Processed: %llu\n"
                    "  Duplicates Detected: %llu (%.2f%%)\n",
                    stats->activeSessions,
                    (unsigned long long)stats->totalCreated,
                    (unsigned long long)stats->totalExpired,
                    (unsigned long long)stats->requestsProcessed,
                    (unsigned long long)stats->duplicatesDetected,
                    stats->duplicateRate * 100.0);
}
