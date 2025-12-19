#ifndef KRAFT_WITNESS_H
#define KRAFT_WITNESS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Kraft Witness Nodes
 * ================================
 * Non-voting nodes that receive log entries but don't participate
 * in elections or quorum decisions. Also called "learners" or "observers".
 *
 * Use Cases:
 * 1. Read Replicas - Serve stale reads without leader contact
 * 2. Geo-Distribution - Replicate to remote datacenters without write latency
 * 3. Backup/Disaster Recovery - Keep warm standby without affecting quorum
 * 4. Node Promotion - Catch up new nodes before adding to cluster
 *
 * Properties:
 * - Receive AppendEntries from leader (replicate log)
 * - Do NOT vote in elections (can't grant votes)
 * - Do NOT count toward commit quorum
 * - Can be promoted to full voter after catching up
 * - Can serve stale reads (configurable staleness bound)
 *
 * Based on:
 * - Raft thesis Section 4.2.1 (Catching up new servers)
 * - etcd "learner" concept
 * - CockroachDB "non-voting replica" concept
 */

/* Forward declarations */
struct kraftState;
struct kraftNode;

/* Witness node configuration */
typedef struct kraftWitnessConfig {
    uint32_t maxWitnesses;   /* Max witness nodes (default: 16) */
    uint64_t maxStalenessUs; /* Max read staleness (default: 5s) */
    uint64_t
        catchupThreshold; /* Entries behind before considered "caught up" */
    bool autoPromote; /* Auto-promote caught-up witnesses on config change */
} kraftWitnessConfig;

/* Default configuration values */
#define KRAFT_WITNESS_DEFAULT_MAX 16
#define KRAFT_WITNESS_DEFAULT_STALENESS_US (5 * 1000000ULL) /* 5 seconds */
#define KRAFT_WITNESS_DEFAULT_CATCHUP 100                   /* entries behind */

/* Witness node state */
typedef enum kraftWitnessStatus {
    KRAFT_WITNESS_ACTIVE = 1,  /* Receiving replication, healthy */
    KRAFT_WITNESS_CATCHING_UP, /* Far behind leader, catching up */
    KRAFT_WITNESS_STALE,       /* Hasn't received updates recently */
    KRAFT_WITNESS_DISCONNECTED /* Not connected to cluster */
} kraftWitnessStatus;

/* Single witness node tracking */
typedef struct kraftWitness {
    struct kraftNode *node;    /* Reference to the underlying node */
    kraftWitnessStatus status; /* Current status */
    uint64_t lastReplication;  /* Timestamp of last successful replication */
    uint64_t lastHeartbeat;    /* Timestamp of last heartbeat */
    uint64_t replicatedIndex; /* Highest log index replicated to this witness */
    uint64_t entriesBehind;   /* How many entries behind leader */
    bool active;              /* Whether this witness slot is in use */
} kraftWitness;

/* Witness manager (owned by kraftState) */
typedef struct kraftWitnessManager {
    kraftWitness *witnesses;   /* Array of witness slots */
    uint32_t witnessCount;     /* Current active witnesses */
    uint32_t capacity;         /* Allocated capacity */
    kraftWitnessConfig config; /* Configuration */

    /* Statistics */
    uint64_t totalAdded;    /* Witnesses added over lifetime */
    uint64_t totalRemoved;  /* Witnesses removed over lifetime */
    uint64_t totalPromoted; /* Witnesses promoted to voter */
    uint64_t readsServed;   /* Stale reads served by witnesses */
} kraftWitnessManager;

/* =========================================================================
 * Configuration
 * ========================================================================= */

/* Get default configuration */
kraftWitnessConfig kraftWitnessConfigDefault(void);

/* =========================================================================
 * Initialization & Lifecycle
 * ========================================================================= */

/* Initialize witness manager with configuration */
bool kraftWitnessManagerInit(kraftWitnessManager *mgr,
                             const kraftWitnessConfig *config);

/* Free witness manager */
void kraftWitnessManagerFree(kraftWitnessManager *mgr);

/* =========================================================================
 * Witness Management
 * ========================================================================= */

/* Add a node as a witness.
 * Returns true on success, false if at capacity or already a witness. */
bool kraftWitnessAdd(kraftWitnessManager *mgr, struct kraftNode *node);

/* Remove a witness node.
 * Returns true if found and removed, false if not a witness. */
bool kraftWitnessRemove(kraftWitnessManager *mgr, struct kraftNode *node);

/* Check if a node is a witness */
bool kraftWitnessIsWitness(const kraftWitnessManager *mgr,
                           const struct kraftNode *node);

/* Get witness for a node, or NULL if not a witness */
kraftWitness *kraftWitnessGet(kraftWitnessManager *mgr,
                              const struct kraftNode *node);

/* =========================================================================
 * Replication Tracking
 * ========================================================================= */

/* Called when entries are successfully replicated to a witness.
 * Updates replicatedIndex and lastReplication timestamp. */
void kraftWitnessUpdateReplicated(kraftWitnessManager *mgr,
                                  struct kraftNode *node,
                                  uint64_t replicatedIndex);

/* Called on heartbeat response from witness.
 * Updates lastHeartbeat and status. */
void kraftWitnessUpdateHeartbeat(kraftWitnessManager *mgr,
                                 struct kraftNode *node, uint64_t nowUs);

/* Update staleness for all witnesses based on leader's current index.
 * Should be called periodically (e.g., on heartbeat timer). */
void kraftWitnessUpdateStaleness(kraftWitnessManager *mgr, uint64_t leaderIndex,
                                 uint64_t nowUs);

/* =========================================================================
 * Read Serving
 * ========================================================================= */

/* Check if witness can serve a read with acceptable staleness.
 * Returns true if the witness has data within maxStalenessUs. */
bool kraftWitnessCanServeRead(const kraftWitnessManager *mgr,
                              const kraftWitness *witness, uint64_t nowUs);

/* Get best witness for serving a read (most up-to-date).
 * Returns NULL if no suitable witness available. */
kraftWitness *kraftWitnessGetBestForRead(kraftWitnessManager *mgr,
                                         uint64_t nowUs);

/* Record that a read was served by a witness (for statistics) */
void kraftWitnessRecordRead(kraftWitnessManager *mgr);

/* =========================================================================
 * Promotion (Witness -> Voter)
 * ========================================================================= */

/* Check if witness is caught up enough to be promoted to voter.
 * Caught up = within catchupThreshold entries of leader. */
bool kraftWitnessIsCaughtUp(const kraftWitnessManager *mgr,
                            const kraftWitness *witness, uint64_t leaderIndex);

/* Promote a witness to a full voting member.
 * This removes it from the witness list.
 * The caller must then add it to the voting configuration.
 * Returns true on success, false if not a witness or not caught up. */
bool kraftWitnessPromote(kraftWitnessManager *mgr, struct kraftNode *node,
                         uint64_t leaderIndex);

/* Get list of witnesses eligible for promotion.
 * Caller provides array and receives count of eligible witnesses. */
uint32_t kraftWitnessGetPromotable(kraftWitnessManager *mgr,
                                   kraftWitness **outWitnesses, uint32_t maxOut,
                                   uint64_t leaderIndex);

/* =========================================================================
 * Statistics
 * ========================================================================= */

typedef struct kraftWitnessStats {
    uint32_t activeWitnesses;  /* Currently active witnesses */
    uint32_t catchingUp;       /* Witnesses in CATCHING_UP state */
    uint32_t stale;            /* Witnesses in STALE state */
    uint32_t disconnected;     /* Witnesses in DISCONNECTED state */
    uint64_t totalAdded;       /* Total witnesses added */
    uint64_t totalRemoved;     /* Total witnesses removed */
    uint64_t totalPromoted;    /* Total witnesses promoted */
    uint64_t readsServed;      /* Total reads served by witnesses */
    uint64_t avgEntriesBehind; /* Average entries behind leader */
} kraftWitnessStats;

/* Get witness manager statistics */
void kraftWitnessGetStats(const kraftWitnessManager *mgr,
                          kraftWitnessStats *stats);

/* Format statistics as string */
int kraftWitnessFormatStats(const kraftWitnessStats *stats, char *buf,
                            size_t bufLen);

/* =========================================================================
 * Integration Helpers
 * ========================================================================= */

/* Check if leader should send entries to witnesses.
 * Called from heartbeat loop - witnesses receive entries but don't count for
 * commit. */
bool kraftWitnessShouldReplicate(const kraftWitnessManager *mgr,
                                 const kraftWitness *witness,
                                 uint64_t leaderIndex);

/* Called when a node votes - witnesses should not be able to vote.
 * Returns true if the node is NOT a witness (i.e., can vote).
 * Returns false if the node IS a witness (should not process vote). */
bool kraftWitnessAllowVote(const kraftWitnessManager *mgr,
                           const struct kraftNode *node);

#endif /* KRAFT_WITNESS_H */
