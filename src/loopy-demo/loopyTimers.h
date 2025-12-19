/* loopyTimers - Centralized timer management for Raft
 *
 * Provides election timeout, heartbeat, and maintenance timers
 * with proper randomization and lifecycle management.
 *
 * This is part of loopy-demo, a consumer of kraft (not part of kraft itself).
 */

#ifndef LOOPY_TIMERS_H
#define LOOPY_TIMERS_H

#include <stdbool.h>
#include <stdint.h>

/* Forward declarations */
struct loopyLoop;

/* ============================================================================
 * Types
 * ============================================================================
 */

/**
 * Timer manager handle.
 *
 * Manages all Raft-related timers for a single node.
 */
typedef struct loopyTimerMgr loopyTimerMgr;

/**
 * Timer types for callbacks.
 */
typedef enum loopyTimerType {
    LOOPY_TIMER_ELECTION,         /* Election timeout fired */
    LOOPY_TIMER_HEARTBEAT,        /* Heartbeat interval fired */
    LOOPY_TIMER_ELECTION_BACKOFF, /* Election backoff period ended */
    LOOPY_TIMER_MAINTENANCE       /* Periodic maintenance */
} loopyTimerType;

/**
 * Timer manager callback.
 *
 * @param mgr      Timer manager
 * @param type     Which timer fired
 * @param userData User context
 */
typedef void loopyTimerMgrCallback(loopyTimerMgr *mgr, loopyTimerType type,
                                   void *userData);

/**
 * Timer configuration.
 */
typedef struct loopyTimerConfig {
    /* Election timeout range (milliseconds) - randomized between min and max */
    uint32_t electionTimeoutMinMs;
    uint32_t electionTimeoutMaxMs;

    /* Heartbeat interval (milliseconds) - should be << election timeout */
    uint32_t heartbeatIntervalMs;

    /* Election backoff after losing (milliseconds) */
    uint32_t electionBackoffMs;

    /* Maintenance interval (milliseconds) - for periodic cleanup, 0 to disable
     */
    uint32_t maintenanceIntervalMs;
} loopyTimerConfig;

/* ============================================================================
 * Configuration
 * ============================================================================
 */

/**
 * Initialize config with Raft-recommended defaults.
 *
 * Default values (from Raft paper recommendations):
 *   - electionTimeoutMinMs: 150
 *   - electionTimeoutMaxMs: 300
 *   - heartbeatIntervalMs: 50 (1/3 of min election timeout)
 *   - electionBackoffMs: 100
 *   - maintenanceIntervalMs: 1000
 */
void loopyTimerConfigInit(loopyTimerConfig *cfg);

/* ============================================================================
 * Lifecycle
 * ============================================================================
 */

/**
 * Create a timer manager.
 *
 * @param loop     Event loop
 * @param cb       Timer callback
 * @param userData User context for callback
 * @param cfg      Configuration (NULL for defaults)
 * @return Timer manager, or NULL on error
 */
loopyTimerMgr *loopyTimerMgrCreate(struct loopyLoop *loop,
                                   loopyTimerMgrCallback *cb, void *userData,
                                   const loopyTimerConfig *cfg);

/**
 * Destroy timer manager.
 *
 * Cancels all active timers.
 *
 * @param mgr Timer manager (may be NULL)
 */
void loopyTimerMgrDestroy(loopyTimerMgr *mgr);

/* ============================================================================
 * Election Timer
 *
 * The election timer is used by followers and candidates. It fires when
 * no heartbeat is received within the election timeout period, triggering
 * an election. The timeout is randomized to prevent split votes.
 * ============================================================================
 */

/**
 * Start or reset the election timer.
 *
 * Cancels any existing election timer and starts a new one with a
 * randomized timeout between electionTimeoutMinMs and electionTimeoutMaxMs.
 *
 * Call this:
 *   - When becoming a follower
 *   - When receiving a valid heartbeat
 *   - When granting a vote
 *
 * @param mgr Timer manager
 */
void loopyTimerStartElection(loopyTimerMgr *mgr);

/**
 * Stop the election timer.
 *
 * Call this when becoming a leader (leaders don't need election timeouts).
 *
 * @param mgr Timer manager
 */
void loopyTimerStopElection(loopyTimerMgr *mgr);

/**
 * Check if election timer is active.
 */
bool loopyTimerElectionActive(const loopyTimerMgr *mgr);

/**
 * Get time remaining until election timer fires (milliseconds).
 *
 * Returns 0 if timer is not active.
 */
uint64_t loopyTimerElectionRemaining(const loopyTimerMgr *mgr);

/* ============================================================================
 * Heartbeat Timer
 *
 * The heartbeat timer is used by leaders to periodically send AppendEntries
 * RPCs to maintain authority and prevent elections.
 * ============================================================================
 */

/**
 * Start the heartbeat timer.
 *
 * Starts a periodic timer that fires every heartbeatIntervalMs.
 *
 * Call this when becoming a leader.
 *
 * @param mgr Timer manager
 */
void loopyTimerStartHeartbeat(loopyTimerMgr *mgr);

/**
 * Stop the heartbeat timer.
 *
 * Call this when stepping down as leader.
 *
 * @param mgr Timer manager
 */
void loopyTimerStopHeartbeat(loopyTimerMgr *mgr);

/**
 * Check if heartbeat timer is active.
 */
bool loopyTimerHeartbeatActive(const loopyTimerMgr *mgr);

/* ============================================================================
 * Election Backoff Timer
 *
 * Used after losing an election to prevent immediate re-election attempts.
 * ============================================================================
 */

/**
 * Start election backoff timer.
 *
 * One-shot timer that fires after electionBackoffMs.
 *
 * Call this after losing an election (receiving higher term response
 * or discovering another leader).
 *
 * @param mgr Timer manager
 */
void loopyTimerStartElectionBackoff(loopyTimerMgr *mgr);

/**
 * Stop election backoff timer.
 */
void loopyTimerStopElectionBackoff(loopyTimerMgr *mgr);

/**
 * Check if election backoff is active.
 */
bool loopyTimerElectionBackoffActive(const loopyTimerMgr *mgr);

/* ============================================================================
 * Maintenance Timer
 *
 * Periodic timer for housekeeping tasks like log compaction checks,
 * stale connection cleanup, etc.
 * ============================================================================
 */

/**
 * Start maintenance timer.
 *
 * Periodic timer that fires every maintenanceIntervalMs.
 *
 * @param mgr Timer manager
 */
void loopyTimerStartMaintenance(loopyTimerMgr *mgr);

/**
 * Stop maintenance timer.
 */
void loopyTimerStopMaintenance(loopyTimerMgr *mgr);

/**
 * Check if maintenance timer is active.
 */
bool loopyTimerMaintenanceActive(const loopyTimerMgr *mgr);

/* ============================================================================
 * Properties
 * ============================================================================
 */

/**
 * Get the event loop.
 */
struct loopyLoop *loopyTimerMgrGetLoop(const loopyTimerMgr *mgr);

/**
 * Get/set user data.
 */
void *loopyTimerMgrGetUserData(const loopyTimerMgr *mgr);
void loopyTimerMgrSetUserData(loopyTimerMgr *mgr, void *userData);

/**
 * Get current configuration.
 */
const loopyTimerConfig *loopyTimerMgrGetConfig(const loopyTimerMgr *mgr);

/**
 * Update configuration.
 *
 * Note: Does not affect currently running timers until they are restarted.
 */
void loopyTimerMgrSetConfig(loopyTimerMgr *mgr, const loopyTimerConfig *cfg);

/**
 * Timer statistics.
 */
typedef struct loopyTimerStats {
    uint64_t electionTimeouts;  /* Number of election timeouts */
    uint64_t heartbeatsSent;    /* Number of heartbeat fires */
    uint64_t electionBackoffs;  /* Number of backoffs */
    uint64_t maintenanceCycles; /* Number of maintenance cycles */
} loopyTimerStats;

void loopyTimerMgrGetStats(const loopyTimerMgr *mgr, loopyTimerStats *stats);

#endif /* LOOPY_TIMERS_H */
