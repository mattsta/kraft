#include "witness.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>

/* Forward declaration - we need kraftNode for node operations */
struct kraftNode;

/* =========================================================================
 * Internal Helpers
 * ========================================================================= */

static uint64_t witnessNowUs(void) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000000ULL + (uint64_t)tv.tv_usec;
}

/* Find witness slot for a node */
static kraftWitness *findWitness(kraftWitnessManager *mgr,
                                 const struct kraftNode *node) {
    if (!mgr->witnesses || !node) {
        return NULL;
    }

    for (uint32_t i = 0; i < mgr->capacity; i++) {
        if (mgr->witnesses[i].active && mgr->witnesses[i].node == node) {
            return &mgr->witnesses[i];
        }
    }

    return NULL;
}

/* Find empty witness slot */
static kraftWitness *findEmptySlot(kraftWitnessManager *mgr) {
    if (!mgr->witnesses) {
        return NULL;
    }

    for (uint32_t i = 0; i < mgr->capacity; i++) {
        if (!mgr->witnesses[i].active) {
            return &mgr->witnesses[i];
        }
    }

    return NULL;
}

/* Update witness status based on staleness */
static void updateWitnessStatus(kraftWitness *witness, uint64_t leaderIndex,
                                uint64_t nowUs,
                                const kraftWitnessConfig *config) {
    if (!witness->active) {
        return;
    }

    /* Calculate entries behind */
    if (leaderIndex > witness->replicatedIndex) {
        witness->entriesBehind = leaderIndex - witness->replicatedIndex;
    } else {
        witness->entriesBehind = 0;
    }

    /* Check for staleness based on time since last replication */
    uint64_t timeSinceReplication = nowUs - witness->lastReplication;

    if (witness->lastHeartbeat == 0 ||
        (nowUs - witness->lastHeartbeat) > config->maxStalenessUs * 2) {
        /* No heartbeat for 2x staleness window = disconnected */
        witness->status = KRAFT_WITNESS_DISCONNECTED;
    } else if (timeSinceReplication > config->maxStalenessUs) {
        /* No replication for staleness window = stale */
        witness->status = KRAFT_WITNESS_STALE;
    } else if (witness->entriesBehind > config->catchupThreshold) {
        /* Far behind = catching up */
        witness->status = KRAFT_WITNESS_CATCHING_UP;
    } else {
        /* Healthy */
        witness->status = KRAFT_WITNESS_ACTIVE;
    }
}

/* =========================================================================
 * Configuration
 * ========================================================================= */

kraftWitnessConfig kraftWitnessConfigDefault(void) {
    return (kraftWitnessConfig){
        .maxWitnesses = KRAFT_WITNESS_DEFAULT_MAX,
        .maxStalenessUs = KRAFT_WITNESS_DEFAULT_STALENESS_US,
        .catchupThreshold = KRAFT_WITNESS_DEFAULT_CATCHUP,
        .autoPromote = false};
}

/* =========================================================================
 * Initialization & Lifecycle
 * ========================================================================= */

bool kraftWitnessManagerInit(kraftWitnessManager *mgr,
                             const kraftWitnessConfig *config) {
    memset(mgr, 0, sizeof(*mgr));

    if (config) {
        mgr->config = *config;
    } else {
        mgr->config = kraftWitnessConfigDefault();
    }

    /* Allocate witness array */
    mgr->capacity = mgr->config.maxWitnesses;
    if (mgr->capacity < 4) {
        mgr->capacity = 4;
    }

    mgr->witnesses = calloc(mgr->capacity, sizeof(kraftWitness));
    if (!mgr->witnesses) {
        return false;
    }

    return true;
}

void kraftWitnessManagerFree(kraftWitnessManager *mgr) {
    if (!mgr->witnesses) {
        return;
    }

    free(mgr->witnesses);
    memset(mgr, 0, sizeof(*mgr));
}

/* =========================================================================
 * Witness Management
 * ========================================================================= */

bool kraftWitnessAdd(kraftWitnessManager *mgr, struct kraftNode *node) {
    if (!node) {
        return false;
    }

    /* Check if already a witness */
    if (findWitness(mgr, node)) {
        return false;
    }

    /* Check capacity */
    if (mgr->witnessCount >= mgr->config.maxWitnesses) {
        return false;
    }

    /* Find empty slot */
    kraftWitness *slot = findEmptySlot(mgr);
    if (!slot) {
        return false;
    }

    /* Initialize witness */
    uint64_t now = witnessNowUs();
    memset(slot, 0, sizeof(*slot));
    slot->node = node;
    slot->status = KRAFT_WITNESS_CATCHING_UP;
    slot->lastReplication = now;
    slot->lastHeartbeat = now;
    slot->replicatedIndex = 0;
    slot->entriesBehind = 0;
    slot->active = true;

    mgr->witnessCount++;
    mgr->totalAdded++;

    return true;
}

bool kraftWitnessRemove(kraftWitnessManager *mgr, struct kraftNode *node) {
    kraftWitness *witness = findWitness(mgr, node);
    if (!witness) {
        return false;
    }

    witness->active = false;
    witness->node = NULL;
    mgr->witnessCount--;
    mgr->totalRemoved++;

    return true;
}

bool kraftWitnessIsWitness(const kraftWitnessManager *mgr,
                           const struct kraftNode *node) {
    /* Cast away const for internal use - findWitness doesn't modify */
    return findWitness((kraftWitnessManager *)mgr, node) != NULL;
}

kraftWitness *kraftWitnessGet(kraftWitnessManager *mgr,
                              const struct kraftNode *node) {
    return findWitness(mgr, node);
}

/* =========================================================================
 * Replication Tracking
 * ========================================================================= */

void kraftWitnessUpdateReplicated(kraftWitnessManager *mgr,
                                  struct kraftNode *node,
                                  uint64_t replicatedIndex) {
    kraftWitness *witness = findWitness(mgr, node);
    if (!witness) {
        return;
    }

    witness->replicatedIndex = replicatedIndex;
    witness->lastReplication = witnessNowUs();
}

void kraftWitnessUpdateHeartbeat(kraftWitnessManager *mgr,
                                 struct kraftNode *node, uint64_t nowUs) {
    kraftWitness *witness = findWitness(mgr, node);
    if (!witness) {
        return;
    }

    witness->lastHeartbeat = nowUs;
}

void kraftWitnessUpdateStaleness(kraftWitnessManager *mgr, uint64_t leaderIndex,
                                 uint64_t nowUs) {
    for (uint32_t i = 0; i < mgr->capacity; i++) {
        updateWitnessStatus(&mgr->witnesses[i], leaderIndex, nowUs,
                            &mgr->config);
    }
}

/* =========================================================================
 * Read Serving
 * ========================================================================= */

bool kraftWitnessCanServeRead(const kraftWitnessManager *mgr,
                              const kraftWitness *witness, uint64_t nowUs) {
    if (!witness || !witness->active) {
        return false;
    }

    /* Check if within staleness bound */
    uint64_t timeSinceReplication = nowUs - witness->lastReplication;
    if (timeSinceReplication > mgr->config.maxStalenessUs) {
        return false;
    }

    /* Only ACTIVE witnesses can serve reads */
    return witness->status == KRAFT_WITNESS_ACTIVE;
}

kraftWitness *kraftWitnessGetBestForRead(kraftWitnessManager *mgr,
                                         uint64_t nowUs) {
    kraftWitness *best = NULL;
    uint64_t bestIndex = 0;

    for (uint32_t i = 0; i < mgr->capacity; i++) {
        kraftWitness *w = &mgr->witnesses[i];
        if (!kraftWitnessCanServeRead(mgr, w, nowUs)) {
            continue;
        }

        /* Prefer witness with highest replicated index */
        if (!best || w->replicatedIndex > bestIndex) {
            best = w;
            bestIndex = w->replicatedIndex;
        }
    }

    return best;
}

void kraftWitnessRecordRead(kraftWitnessManager *mgr) {
    mgr->readsServed++;
}

/* =========================================================================
 * Promotion (Witness -> Voter)
 * ========================================================================= */

bool kraftWitnessIsCaughtUp(const kraftWitnessManager *mgr,
                            const kraftWitness *witness, uint64_t leaderIndex) {
    if (!witness || !witness->active) {
        return false;
    }

    /* Calculate entries behind */
    uint64_t behind = 0;
    if (leaderIndex > witness->replicatedIndex) {
        behind = leaderIndex - witness->replicatedIndex;
    }

    return behind <= mgr->config.catchupThreshold;
}

bool kraftWitnessPromote(kraftWitnessManager *mgr, struct kraftNode *node,
                         uint64_t leaderIndex) {
    kraftWitness *witness = findWitness(mgr, node);
    if (!witness) {
        return false;
    }

    /* Must be caught up to promote */
    if (!kraftWitnessIsCaughtUp(mgr, witness, leaderIndex)) {
        return false;
    }

    /* Remove from witness list */
    witness->active = false;
    witness->node = NULL;
    mgr->witnessCount--;
    mgr->totalPromoted++;

    return true;
}

uint32_t kraftWitnessGetPromotable(kraftWitnessManager *mgr,
                                   kraftWitness **outWitnesses, uint32_t maxOut,
                                   uint64_t leaderIndex) {
    uint32_t count = 0;

    for (uint32_t i = 0; i < mgr->capacity && count < maxOut; i++) {
        kraftWitness *w = &mgr->witnesses[i];
        if (kraftWitnessIsCaughtUp(mgr, w, leaderIndex)) {
            outWitnesses[count++] = w;
        }
    }

    return count;
}

/* =========================================================================
 * Statistics
 * ========================================================================= */

void kraftWitnessGetStats(const kraftWitnessManager *mgr,
                          kraftWitnessStats *stats) {
    memset(stats, 0, sizeof(*stats));

    stats->totalAdded = mgr->totalAdded;
    stats->totalRemoved = mgr->totalRemoved;
    stats->totalPromoted = mgr->totalPromoted;
    stats->readsServed = mgr->readsServed;

    uint64_t totalBehind = 0;
    uint32_t activeCount = 0;

    for (uint32_t i = 0; i < mgr->capacity; i++) {
        const kraftWitness *w = &mgr->witnesses[i];
        if (!w->active) {
            continue;
        }

        activeCount++;
        totalBehind += w->entriesBehind;

        switch (w->status) {
        case KRAFT_WITNESS_ACTIVE:
            stats->activeWitnesses++;
            break;
        case KRAFT_WITNESS_CATCHING_UP:
            stats->catchingUp++;
            break;
        case KRAFT_WITNESS_STALE:
            stats->stale++;
            break;
        case KRAFT_WITNESS_DISCONNECTED:
            stats->disconnected++;
            break;
        }
    }

    if (activeCount > 0) {
        stats->avgEntriesBehind = totalBehind / activeCount;
    }
}

int kraftWitnessFormatStats(const kraftWitnessStats *stats, char *buf,
                            size_t bufLen) {
    return snprintf(buf, bufLen,
                    "Witness Stats:\n"
                    "  Active: %u (healthy)\n"
                    "  Catching Up: %u\n"
                    "  Stale: %u\n"
                    "  Disconnected: %u\n"
                    "  Total Added: %llu\n"
                    "  Total Removed: %llu\n"
                    "  Total Promoted: %llu\n"
                    "  Reads Served: %llu\n"
                    "  Avg Entries Behind: %llu\n",
                    stats->activeWitnesses, stats->catchingUp, stats->stale,
                    stats->disconnected, (unsigned long long)stats->totalAdded,
                    (unsigned long long)stats->totalRemoved,
                    (unsigned long long)stats->totalPromoted,
                    (unsigned long long)stats->readsServed,
                    (unsigned long long)stats->avgEntriesBehind);
}

/* =========================================================================
 * Integration Helpers
 * ========================================================================= */

bool kraftWitnessShouldReplicate(const kraftWitnessManager *mgr,
                                 const kraftWitness *witness,
                                 uint64_t leaderIndex) {
    if (!witness || !witness->active) {
        return false;
    }

    /* Always replicate to active/catching-up witnesses */
    if (witness->status == KRAFT_WITNESS_ACTIVE ||
        witness->status == KRAFT_WITNESS_CATCHING_UP) {
        return true;
    }

    /* Don't replicate to stale/disconnected - they need to reconnect first */
    return false;
}

bool kraftWitnessAllowVote(const kraftWitnessManager *mgr,
                           const struct kraftNode *node) {
    /* If node is a witness, it cannot vote */
    if (kraftWitnessIsWitness(mgr, node)) {
        return false;
    }

    /* Non-witness nodes can vote */
    return true;
}
