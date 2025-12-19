#ifndef KRAFT_TEST_H
#define KRAFT_TEST_H

#include "kraft.h"
#include "rpc.h"
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>

/* ====================================================================
 * Kraft Distributed Systems Test Harness
 *
 * This test framework simulates a complete Raft cluster in-process
 * with full control over:
 *   - Network behavior (delays, drops, partitions, reordering)
 *   - Node crashes and recoveries
 *   - Time progression
 *   - Deterministic execution for reproduc able bugs
 *
 * Design Philosophy:
 *   - All communication goes through simulated network
 *   - All timing controlled by virtual clock
 *   - No actual threads (single-threaded deterministic execution)
 *   - All state fully inspectable for assertions
 * ==================================================================== */

#define TEST_MAX_NODES 1000
#define TEST_MAX_PENDING_MSGS 10000

typedef struct testMessage {
    uint64_t deliveryTime; /* Virtual time when msg should be delivered */
    uint8_t *data;
    size_t len;
    uint32_t fromNode;
    uint32_t toNode;
    bool dropped;
} testMessage;

typedef enum testNetworkMode {
    TEST_NETWORK_RELIABLE,    /* No drops, no delays */
    TEST_NETWORK_LOSSY,       /* Random packet loss */
    TEST_NETWORK_DELAYED,     /* Random delays */
    TEST_NETWORK_REORDERING,  /* Messages can arrive out of order */
    TEST_NETWORK_PARTITIONED, /* Specific nodes can't communicate */
} testNetworkMode;

typedef struct testNetwork {
    testNetworkMode mode;

    /* Configuration */
    double dropRate;   /* 0.0 - 1.0 */
    uint64_t minDelay; /* microseconds */
    uint64_t maxDelay; /* microseconds */

    /* Partition configuration: partition[i][j] = true means i->j blocked */
    bool partition[TEST_MAX_NODES][TEST_MAX_NODES];

    /* Message queue (priority queue by deliveryTime) */
    testMessage *messages;
    uint32_t messageCount;
    uint32_t messageCapacity;

    /* Statistics */
    uint64_t totalSent;
    uint64_t totalDropped;
    uint64_t totalDelivered;
} testNetwork;

typedef struct testNode {
    kraftState *state;
    uint32_t nodeId;
    bool crashed;
    bool paused; /* For simulating slow nodes */

    /* Timing */
    uint64_t nextElectionTimeout;
    uint64_t nextHeartbeatTimeout;

    /* Statistics */
    uint64_t msgsReceived;
    uint64_t msgsSent;
    uint64_t electionsStarted;
    uint64_t timesLeader;
} testNode;

typedef struct testCluster {
    testNode nodes[TEST_MAX_NODES];
    uint32_t nodeCount;

    testNetwork network;

    /* Virtual clock (microseconds) */
    uint64_t currentTime;

    /* Test configuration */
    uint32_t electionTimeoutMin;
    uint32_t electionTimeoutMax;
    uint32_t heartbeatInterval;

    /* Cluster state tracking */
    uint32_t
        currentLeader; /* Node ID of current leader, or UINT32_MAX if none */
    uint64_t currentTerm;

    /* Invariant checking */
    bool enableInvariantChecking;
    uint64_t invariantCheckInterval;
    uint64_t lastInvariantCheck;

    /* State Machine Safety Tracking:
     * For each log index, track what command was applied.
     * This allows verifying no two nodes apply different commands at same
     * index. */
    struct {
        uint64_t *commandHashes;      /* Hash of command at each index */
        kraftRpcUniqueIndex maxIndex; /* Highest index tracked */
        uint32_t capacity;            /* Allocated capacity */
    } stateMachineHistory;

    /* Leader Append-Only Tracking:
     * Track each leader's log evolution to ensure monotonic growth. */
    struct {
        kraftRpcUniqueIndex *highWaterMark; /* Max index seen for each node */
        kraftTerm **highWaterTerm;    /* Per-node term tracking [node][index] */
        kraftTerm *highWaterNodeTerm; /* Node's term when high water was set */
    } appendOnlyTracking;

    /* Statistics */
    uint64_t totalElections;
    uint64_t splitVotes;
    uint64_t leaderChanges;

    /* Performance Profiling:
     * Track operation latencies and throughput for performance analysis. */
    struct {
        bool enabled;

        /* Latency tracking (microseconds) */
        struct {
            uint64_t *samples; /* Array of latency samples */
            uint32_t count;    /* Number of samples collected */
            uint32_t capacity; /* Allocated capacity */
            uint64_t sum;      /* Sum of all samples for avg */
            uint64_t min;      /* Minimum latency */
            uint64_t max;      /* Maximum latency */
        } rpcLatency, commitLatency, electionLatency;

        /* Throughput tracking */
        uint64_t rpcProcessed;      /* Total RPCs processed */
        uint64_t commandsCommitted; /* Total commands committed */
        uint64_t bytesTransferred;  /* Total bytes sent over network */

        /* Timing marks for throughput calculation */
        uint64_t profilingStartTime;
        uint64_t profilingEndTime;

        /* Operation counters by type */
        uint64_t appendEntriesCount;
        uint64_t requestVoteCount;
        uint64_t snapshotCount;
    } perfMetrics;
} testCluster;

/* ====================================================================
 * Test Cluster Management
 * ==================================================================== */
testCluster *testClusterNew(uint32_t nodeCount);
void testClusterFree(testCluster *cluster);
void testClusterStart(testCluster *cluster);

/* ====================================================================
 * Node Control
 * ==================================================================== */
void testNodeCrash(testCluster *cluster, uint32_t nodeId);
void testNodeRecover(testCluster *cluster, uint32_t nodeId);
void testNodePause(testCluster *cluster, uint32_t nodeId);
void testNodeResume(testCluster *cluster, uint32_t nodeId);

/* ====================================================================
 * Network Control
 * ==================================================================== */
void testNetworkSetMode(testCluster *cluster, testNetworkMode mode);
void testNetworkSetDropRate(testCluster *cluster, double rate);
void testNetworkSetDelay(testCluster *cluster, uint64_t minUs, uint64_t maxUs);
void testNetworkPartition(testCluster *cluster, uint32_t *group1,
                          uint32_t group1Size, uint32_t *group2,
                          uint32_t group2Size);
void testNetworkHeal(testCluster *cluster);

/* ====================================================================
 * Simulation Control
 * ==================================================================== */
void testClusterTick(testCluster *cluster, uint64_t microseconds);
void testClusterRunUntil(testCluster *cluster, bool (*condition)(testCluster *),
                         uint64_t maxTime);
void testClusterRunUntilLeaderElected(testCluster *cluster, uint64_t maxTime);
void testClusterRunUntilQuiet(testCluster *cluster, uint64_t quietPeriod);

/* ====================================================================
 * State Inspection
 * ==================================================================== */
uint32_t testClusterGetLeader(testCluster *cluster);
uint64_t testClusterGetMaxTerm(testCluster *cluster);
bool testClusterHasConsistentLeader(testCluster *cluster);
bool testClusterLogsMatch(testCluster *cluster);

/* ====================================================================
 * Client Operations
 * ==================================================================== */
bool testClusterSubmitCommand(testCluster *cluster, const void *data,
                              size_t len);
bool testClusterAwaitCommit(testCluster *cluster, kraftRpcUniqueIndex index,
                            uint64_t timeout);

/* ====================================================================
 * Invariant Checking (Raft Safety Properties)
 * ==================================================================== */
/* "Election Safety: at most one leader can be elected in a given term" */
bool testInvariantElectionSafety(testCluster *cluster);

/* "Leader Append-Only: a leader never overwrites or deletes entries in its log;
 * it only appends new entries" */
bool testInvariantLeaderAppendOnly(testCluster *cluster);

/* "Log Matching: if two logs contain an entry with the same index and term,
 * then the logs are identical in all entries up through the given index" */
bool testInvariantLogMatching(testCluster *cluster);

/* "Leader Completeness: if a log entry is committed in a given term, then that
 * entry will be present in the logs of the leaders for all higher-numbered
 * terms" */
bool testInvariantLeaderCompleteness(testCluster *cluster);

/* "State Machine Safety: if a server has applied a log entry at a given index
 * to its state machine, no other server will ever apply a different log entry
 * for the same index" */
bool testInvariantStateMachineSafety(testCluster *cluster);

/* Check all invariants */
void testClusterCheckInvariants(testCluster *cluster);

/* ====================================================================
 * Performance Profiling & Benchmarking
 * ==================================================================== */
/* Enable/disable performance profiling */
void testPerfEnable(testCluster *cluster);
void testPerfDisable(testCluster *cluster);

/* Record performance samples */
void testPerfRecordRpcLatency(testCluster *cluster, uint64_t latencyUs);
void testPerfRecordCommitLatency(testCluster *cluster, uint64_t latencyUs);
void testPerfRecordElectionLatency(testCluster *cluster, uint64_t latencyUs);

/* Get performance statistics */
typedef struct testPerfStats {
    /* RPC Performance */
    uint64_t rpcAvgLatencyUs;
    uint64_t rpcMinLatencyUs;
    uint64_t rpcMaxLatencyUs;
    uint64_t rpcP50LatencyUs; /* Median */
    uint64_t rpcP95LatencyUs;
    uint64_t rpcP99LatencyUs;
    uint64_t rpcThroughput; /* RPCs per second */

    /* Commit Performance */
    uint64_t commitAvgLatencyUs;
    uint64_t commitMinLatencyUs;
    uint64_t commitMaxLatencyUs;
    uint64_t commitP50LatencyUs;
    uint64_t commitP95LatencyUs;
    uint64_t commitP99LatencyUs;
    uint64_t commitThroughput; /* Commits per second */

    /* Election Performance */
    uint64_t electionAvgLatencyUs;
    uint64_t electionMinLatencyUs;
    uint64_t electionMaxLatencyUs;

    /* Network Performance */
    uint64_t networkThroughputBytesPerSec;
    uint64_t messagesPerSecond;

    /* Overall metrics */
    uint64_t totalRpcs;
    uint64_t totalCommits;
    uint64_t totalElections;
    uint64_t durationUs;
} testPerfStats;

testPerfStats testPerfGetStats(testCluster *cluster);
void testPerfPrintReport(testCluster *cluster);
void testPerfReset(testCluster *cluster);

/* ====================================================================
 * Utility Functions
 * ==================================================================== */
uint64_t testRandom(uint64_t min, uint64_t max);
void testSeed(uint64_t seed);

/* Get current wall-clock time in microseconds (for real benchmarking) */
uint64_t testGetWallTimeUs(void);

#endif /* KRAFT_TEST_H */
