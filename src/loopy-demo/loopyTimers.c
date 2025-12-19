/* loopyTimers - Centralized timer management for Raft
 *
 * Implementation of Raft-specific timer management.
 */

#include "loopyTimers.h"

/* loopy headers */
#include "../../deps/loopy/src/loopy.h"
#include "../../deps/loopy/src/loopyTimer.h"

#include <stdlib.h>
#include <string.h>
#include <time.h>

/* ============================================================================
 * Internal Structures
 * ============================================================================
 */

struct loopyTimerMgr {
    loopyLoop *loop;
    loopyTimerMgrCallback *callback;
    void *userData;
    loopyTimerConfig config;
    loopyTimerStats stats;

    /* Active timers */
    loopyTimer *electionTimer;
    loopyTimer *heartbeatTimer;
    loopyTimer *backoffTimer;
    loopyTimer *maintenanceTimer;

    /* For tracking election timer remaining time */
    uint64_t electionStartTimeMs;
    uint64_t electionTimeoutMs;
};

/* ============================================================================
 * Internal Helpers
 * ============================================================================
 */

static uint64_t getCurrentTimeMs(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static uint32_t randomInRange(uint32_t min, uint32_t max) {
    if (min >= max) {
        return min;
    }
    /* Simple random - for production, use a better PRNG */
    return min + (uint32_t)(rand() % (max - min + 1));
}

/* ============================================================================
 * Timer Callbacks
 * ============================================================================
 */

static void electionTimerCallback(loopyLoop *loop, loopyTimer *timer,
                                  void *userData) {
    loopyTimerMgr *mgr = (loopyTimerMgr *)userData;
    (void)loop;
    (void)timer;

    mgr->electionTimer = NULL; /* One-shot, auto-cleaned */
    mgr->stats.electionTimeouts++;

    if (mgr->callback) {
        mgr->callback(mgr, LOOPY_TIMER_ELECTION, mgr->userData);
    }
}

static void heartbeatTimerCallback(loopyLoop *loop, loopyTimer *timer,
                                   void *userData) {
    loopyTimerMgr *mgr = (loopyTimerMgr *)userData;
    (void)loop;
    (void)timer;

    mgr->stats.heartbeatsSent++;

    if (mgr->callback) {
        mgr->callback(mgr, LOOPY_TIMER_HEARTBEAT, mgr->userData);
    }
}

static void backoffTimerCallback(loopyLoop *loop, loopyTimer *timer,
                                 void *userData) {
    loopyTimerMgr *mgr = (loopyTimerMgr *)userData;
    (void)loop;
    (void)timer;

    mgr->backoffTimer = NULL; /* One-shot, auto-cleaned */
    mgr->stats.electionBackoffs++;

    if (mgr->callback) {
        mgr->callback(mgr, LOOPY_TIMER_ELECTION_BACKOFF, mgr->userData);
    }
}

static void maintenanceTimerCallback(loopyLoop *loop, loopyTimer *timer,
                                     void *userData) {
    loopyTimerMgr *mgr = (loopyTimerMgr *)userData;
    (void)loop;
    (void)timer;

    mgr->stats.maintenanceCycles++;

    if (mgr->callback) {
        mgr->callback(mgr, LOOPY_TIMER_MAINTENANCE, mgr->userData);
    }
}

/* ============================================================================
 * Configuration
 * ============================================================================
 */

void loopyTimerConfigInit(loopyTimerConfig *cfg) {
    if (!cfg) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));

    /* Raft paper recommendations */
    cfg->electionTimeoutMinMs = 150;
    cfg->electionTimeoutMaxMs = 300;
    cfg->heartbeatIntervalMs = 50; /* 1/3 of min election timeout */
    cfg->electionBackoffMs = 100;
    cfg->maintenanceIntervalMs = 1000;
}

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

loopyTimerMgr *loopyTimerMgrCreate(loopyLoop *loop, loopyTimerMgrCallback *cb,
                                   void *userData,
                                   const loopyTimerConfig *cfg) {
    if (!loop) {
        return NULL;
    }

    loopyTimerMgr *mgr = calloc(1, sizeof(*mgr));
    if (!mgr) {
        return NULL;
    }

    mgr->loop = loop;
    mgr->callback = cb;
    mgr->userData = userData;

    if (cfg) {
        mgr->config = *cfg;
    } else {
        loopyTimerConfigInit(&mgr->config);
    }

    /* Seed random number generator (simple approach) */
    static bool seeded = false;
    if (!seeded) {
        srand((unsigned int)time(NULL) ^ (unsigned int)getCurrentTimeMs());
        seeded = true;
    }

    return mgr;
}

void loopyTimerMgrDestroy(loopyTimerMgr *mgr) {
    if (!mgr) {
        return;
    }

    /* Cancel all active timers */
    if (mgr->electionTimer) {
        loopyTimerCancel(mgr->electionTimer);
        mgr->electionTimer = NULL;
    }
    if (mgr->heartbeatTimer) {
        loopyTimerCancel(mgr->heartbeatTimer);
        mgr->heartbeatTimer = NULL;
    }
    if (mgr->backoffTimer) {
        loopyTimerCancel(mgr->backoffTimer);
        mgr->backoffTimer = NULL;
    }
    if (mgr->maintenanceTimer) {
        loopyTimerCancel(mgr->maintenanceTimer);
        mgr->maintenanceTimer = NULL;
    }

    free(mgr);
}

/* ============================================================================
 * Election Timer
 * ============================================================================
 */

void loopyTimerStartElection(loopyTimerMgr *mgr) {
    if (!mgr) {
        return;
    }

    /* Cancel existing timer if any */
    if (mgr->electionTimer) {
        loopyTimerCancel(mgr->electionTimer);
        mgr->electionTimer = NULL;
    }

    /* Randomize timeout */
    mgr->electionTimeoutMs = randomInRange(mgr->config.electionTimeoutMinMs,
                                           mgr->config.electionTimeoutMaxMs);
    mgr->electionStartTimeMs = getCurrentTimeMs();

    /* Create one-shot timer */
    mgr->electionTimer = loopyTimerOneShotMs(mgr->loop, mgr->electionTimeoutMs,
                                             electionTimerCallback, mgr);
}

void loopyTimerStopElection(loopyTimerMgr *mgr) {
    if (!mgr || !mgr->electionTimer) {
        return;
    }

    loopyTimerCancel(mgr->electionTimer);
    mgr->electionTimer = NULL;
}

bool loopyTimerElectionActive(const loopyTimerMgr *mgr) {
    if (!mgr) {
        return false;
    }
    return mgr->electionTimer != NULL;
}

uint64_t loopyTimerElectionRemaining(const loopyTimerMgr *mgr) {
    if (!mgr || !mgr->electionTimer) {
        return 0;
    }

    uint64_t now = getCurrentTimeMs();
    uint64_t elapsed = now - mgr->electionStartTimeMs;

    if (elapsed >= mgr->electionTimeoutMs) {
        return 0;
    }

    return mgr->electionTimeoutMs - elapsed;
}

/* ============================================================================
 * Heartbeat Timer
 * ============================================================================
 */

void loopyTimerStartHeartbeat(loopyTimerMgr *mgr) {
    if (!mgr) {
        return;
    }

    /* Cancel existing timer if any */
    if (mgr->heartbeatTimer) {
        loopyTimerCancel(mgr->heartbeatTimer);
        mgr->heartbeatTimer = NULL;
    }

    /* Create periodic timer */
    mgr->heartbeatTimer =
        loopyTimerPeriodicMs(mgr->loop, mgr->config.heartbeatIntervalMs,
                             heartbeatTimerCallback, mgr);
}

void loopyTimerStopHeartbeat(loopyTimerMgr *mgr) {
    if (!mgr || !mgr->heartbeatTimer) {
        return;
    }

    loopyTimerCancel(mgr->heartbeatTimer);
    mgr->heartbeatTimer = NULL;
}

bool loopyTimerHeartbeatActive(const loopyTimerMgr *mgr) {
    if (!mgr) {
        return false;
    }
    return mgr->heartbeatTimer != NULL;
}

/* ============================================================================
 * Election Backoff Timer
 * ============================================================================
 */

void loopyTimerStartElectionBackoff(loopyTimerMgr *mgr) {
    if (!mgr) {
        return;
    }

    /* Cancel existing timer if any */
    if (mgr->backoffTimer) {
        loopyTimerCancel(mgr->backoffTimer);
        mgr->backoffTimer = NULL;
    }

    /* Create one-shot timer */
    mgr->backoffTimer = loopyTimerOneShotMs(
        mgr->loop, mgr->config.electionBackoffMs, backoffTimerCallback, mgr);
}

void loopyTimerStopElectionBackoff(loopyTimerMgr *mgr) {
    if (!mgr || !mgr->backoffTimer) {
        return;
    }

    loopyTimerCancel(mgr->backoffTimer);
    mgr->backoffTimer = NULL;
}

bool loopyTimerElectionBackoffActive(const loopyTimerMgr *mgr) {
    if (!mgr) {
        return false;
    }
    return mgr->backoffTimer != NULL;
}

/* ============================================================================
 * Maintenance Timer
 * ============================================================================
 */

void loopyTimerStartMaintenance(loopyTimerMgr *mgr) {
    if (!mgr) {
        return;
    }

    /* Skip if interval is 0 (disabled) */
    if (mgr->config.maintenanceIntervalMs == 0) {
        return;
    }

    /* Cancel existing timer if any */
    if (mgr->maintenanceTimer) {
        loopyTimerCancel(mgr->maintenanceTimer);
        mgr->maintenanceTimer = NULL;
    }

    /* Create periodic timer */
    mgr->maintenanceTimer =
        loopyTimerPeriodicMs(mgr->loop, mgr->config.maintenanceIntervalMs,
                             maintenanceTimerCallback, mgr);
}

void loopyTimerStopMaintenance(loopyTimerMgr *mgr) {
    if (!mgr || !mgr->maintenanceTimer) {
        return;
    }

    loopyTimerCancel(mgr->maintenanceTimer);
    mgr->maintenanceTimer = NULL;
}

bool loopyTimerMaintenanceActive(const loopyTimerMgr *mgr) {
    if (!mgr) {
        return false;
    }
    return mgr->maintenanceTimer != NULL;
}

/* ============================================================================
 * Properties
 * ============================================================================
 */

loopyLoop *loopyTimerMgrGetLoop(const loopyTimerMgr *mgr) {
    if (!mgr) {
        return NULL;
    }
    return mgr->loop;
}

void *loopyTimerMgrGetUserData(const loopyTimerMgr *mgr) {
    if (!mgr) {
        return NULL;
    }
    return mgr->userData;
}

void loopyTimerMgrSetUserData(loopyTimerMgr *mgr, void *userData) {
    if (!mgr) {
        return;
    }
    mgr->userData = userData;
}

const loopyTimerConfig *loopyTimerMgrGetConfig(const loopyTimerMgr *mgr) {
    if (!mgr) {
        return NULL;
    }
    return &mgr->config;
}

void loopyTimerMgrSetConfig(loopyTimerMgr *mgr, const loopyTimerConfig *cfg) {
    if (!mgr || !cfg) {
        return;
    }
    mgr->config = *cfg;
}

void loopyTimerMgrGetStats(const loopyTimerMgr *mgr, loopyTimerStats *stats) {
    if (!stats) {
        return;
    }

    if (!mgr) {
        memset(stats, 0, sizeof(*stats));
        return;
    }

    *stats = mgr->stats;
}
