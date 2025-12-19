#ifndef KRAFT_H
#define KRAFT_H

#include <assert.h>
#include <inttypes.h> /* PRId64, PRIu64, etc */
#include <limits.h>   /* UINT_MAX, etc */
#include <stdbool.h>
#include <stddef.h> /* ptrdiff_t */
#include <stdint.h>
#include <stdlib.h>
#include <string.h>   /* memcpy, etc */
#include <sys/time.h> /* gettimeofday */

#include "../deps/kvidxkit/src/kvidxkit.h"
#include "../deps/kvidxkit/src/kvidxkitRegistry.h"
#include "vec.h"

#include "../deps/datakit/src/flex.h"

#include "asyncCommit.h"
#include "batching.h"
#include "bufferPool.h"
#include "chaos.h"
#include "crdt.h"
#include "metrics.h"
#include "profile.h"
#include "protoKit.h"
#include "session.h"
#include "snapshot.h"
#include "witness.h"

#if defined(KRAFT_TEST) || defined(KRAFT_TEST_VERBOSE)
#include <stdio.h> /* for printf (debug printing), snprintf (genstr) */
#endif

#if 0
        char *filenameOnly = strrchr(__FILE__, '/');                           \
        char *filePos = filenameOnly ? ++filenameOnly : __FILE__;              \
printf("%" PRIu64 ":%s:%s:%d:\t", kraftmstime(), filePos, __func__, __LINE__);
#endif

/* If not verbose testing, remove all debug printing. */
#ifndef KRAFT_TEST_VERBOSE
#define D(...)
#else
#undef D
/* Only print the filename as debug prefix, not entire file path.
 * Just: detect the last '/' in file path, then increment one after it.
 *       if no '/' detected, use entire filename. */
#define D(...)                                                                 \
    do {                                                                       \
        printf("%s:%d:\t", __func__, __LINE__);                                \
        printf(__VA_ARGS__);                                                   \
        printf("\n");                                                          \
        fflush(stdout);                                                        \
    } while (0)
#endif

typedef uint64_t kraftTerm;
typedef uint64_t kraftClusterId;
typedef uint64_t kraftNodeId;
typedef uint64_t kraftRunId;
typedef uint64_t kraftRpcUniqueIndex;
typedef uint64_t kraftTimestamp;

typedef void kraftRpcEventLoop;

/* "Raft uses randomized election timeouts to ensure that split votes are rare
 * and that they are resolved quickly. To prevent split votes in the first
 * place, election timeouts are chosen randomly from a fixed interval (e.g.,
 * 150–300ms). This spreads out the servers so that in most cases only a single
 * server will time out; it wins the election and sends heartbeats before any
 * other servers time out. The same mechanism is used to handle split votes.
 * Each candidate restarts its randomized election timeout at the start of an
 * election, and it waits for that timeout to elapse before starting the next
 * election; this reduces the likelihood of another split vote in the new
 * election. Section 9.3 shows that this approach elects a leader rapidly." */
static const uint_fast32_t kraftDelayTimeouts[] = {150, 200, 250, 300,
                                                   350, 400, 450, 500};
static const size_t kraftDelayTimeoutMax =
    sizeof(kraftDelayTimeouts) / sizeof(*kraftDelayTimeouts);
#define kraftRandomDelay() (kraftDelayTimeouts[rand() % kraftDelayTimeoutMax])

typedef enum kraftRpcStatus {
    KRAFT_RPC_STATUS_OK = 1,
    KRAFT_RPC_STATUS_REJECTED_SELF_RUNID,
    KRAFT_RPC_STATUS_REJECTED_IDX_TERM_MISMATCH,
    KRAFT_RPC_STATUS_REJECTED_RPC_TERM_TOO_OLD,
    KRAFT_RPC_STATUS_REJECTED_VOTE_TERM_TOO_OLD,
    KRAFT_RPC_STATUS_REJECTED_VOTE_ALREADY_VOTED,
    KRAFT_RPC_STATUS_REJECTED_VOTE_LOG_NOT_CURRENT,
    KRAFT_RPC_STATUS_INVALID_SELF_ROLE,
} kraftRpcStatus;
// extern char *kraftRpcStatuses[];
static char *kraftRpcStatuses[] = {
    "_UNDEFINED_",
    "OK",
    "REJECTED — SELF RUN ID",
    "REJECTED — INDEX+TERM MISMATCH",
    "REJECTED — RPC VOTING TERM TOO OLD",
    "REJECTED — ALREADY VOTED THIS TERM",
    "REJECTED — RPC LOG OLDER THAN SELF LOG",
    "INVALID — WRONG SELF ROLE",
};

typedef enum kraftProtocolStatus {
    KRAFT_PROTOCOL_STATUS_RPC_POPULATED = 1,
    KRAFT_PROTOCOL_STATUS_NO_RPC,
    KRAFT_PROTOCOL_STATUS_PARSE_ERROR,
    KRAFT_PROTOCOL_STATUS_SYSTEM_ERROR,
} kraftProtocolStatus;
// extern char *kraftProtocolStatuses[];
static char *kraftProtocolStatuses[] = {"_UNDEFINED_", "POPULATED",
                                        "NO RPC IN BUFFER", "PARSE ERROR",
                                        "SYSTEM ERROR"};

typedef enum kraftRole {
    KRAFT_CANDIDATE = 1, /* Passive, voting for self to become Leader */
    KRAFT_FOLLOWER = 2,  /* Passive, only responds to Leader */
    KRAFT_LEADER = 3,    /* Active, interacts with clients, updates Followers */
    KRAFT_PRE_CANDIDATE = 4, /* Pre-vote phase before becoming candidate */
} kraftRole;
// extern char *kraftRoles[];
static char *kraftRoles[] = {"_UNDEFINED_", "CANDIDATE", "FOLLOWER", "LEADER",
                             "PRE_CANDIDATE"};

typedef enum kraftNodeStatus {
    KRAFT_NODE_UNVERIFIED = 1, /* Client exists, but no action taken yet. */
    KRAFT_NODE_HELLO_SENT,     /* Requested to join client, no response yet. */
    KRAFT_NODE_VERIFIED,       /* Client responded we're all good */
} kraftNodeStatus;
// extern char *kraftRoles[];
static char *kraftNodeStatuses[] = {"_UNDEFINED_", "UNVERIFIED", "HELLO SENT",
                                    "VERIFIED"};

typedef enum kraftTimerAction {
    KRAFT_TIMER_START = 0xbe,
    KRAFT_TIMER_STOP,
    KRAFT_TIMER_AGAIN,
} kraftTimerAction;

/* Note: RPC CMD is limited to 1 byte (255 max values). */
/* These numbers establish the Kraft Line Protocol and should
 * be incorporated into a formal protocol design document. */
typedef enum kraftRpcCmd {
    /* RPCs definied by Raft */
    /* "Invoked by leader to replicate log entries (§5.3)" */
    /* "also used as heartbeat (§5.2)" */
    KRAFT_RPC_APPEND_ENTRIES = 1, /* actual data */
    /* "Invoked by candidates to gather votes (§5.2)" */
    KRAFT_RPC_REQUEST_VOTE = 2, /* voting */

    /* RPCs only used for Kraft-specific replies */
    /* (RequestVote receiver implementation) */
    /* "grant vote (§5.2, §5.4)" */
    KRAFT_RPC_VOTE_REPLY_GRANTED = 4,
    /* "Reply false if term < currentTerm (§5.1)" */
    KRAFT_RPC_VOTE_REPLY_DENIED = 5,

    /* Multi-valued success result:
     * "true if follower contained entry matching
     * prevLogIndex and prevLogTerm" */
    /* (AppendEntries receiver implementation, these
     *  replies go back to the Leader) */
    /* "Reply false if term < currentTerm (§5.1)" */
    KRAFT_RPC_APPEND_ENTRIES_REPLY_TERM_MISMATCH = 6,
    /* "Reply false if log doesn’t contain an entry at prevLogIndex whose term
     *  matches prevLogTerm (§5.3)" */
    KRAFT_RPC_APPEND_ENTRIES_REPLY_IDX_TERM_MISMATCH = 7,

    KRAFT_RPC_APPEND_ENTRIES_REPLY_OK = 8,

    /* Snapshot transfer (§7 - Log compaction) */
    KRAFT_RPC_INSTALL_SNAPSHOT = 12,
    KRAFT_RPC_INSTALL_SNAPSHOT_REPLY_OK = 13,

    /* Pre-Vote Protocol (Raft thesis §9.6)
     * Prevents disruption when partitioned nodes reconnect */
    KRAFT_RPC_REQUEST_PREVOTE = 14,
    KRAFT_RPC_PREVOTE_REPLY_GRANTED = 15,
    KRAFT_RPC_PREVOTE_REPLY_DENIED = 16,

    /* Leadership Transfer (Raft thesis §3.10)
     * Enables zero-downtime leader changes for maintenance */
    KRAFT_RPC_TIMEOUT_NOW = 17,

    /* Connection Control; not actual Raft RPCs, but
     * still commands we use in our RPCs.
     * These could potentially be part of an independent/reusable
     * connection establishment framework. */
    KRAFT_RPC_HELLO = 9,
    KRAFT_RPC_GOAWAY = 10,
    KRAFT_RPC_VERIFIED = 11,
} kraftRpcCmd;
// extern char *kraftRpcCmds[];
static char *kraftRpcCmds[] = {"_UNDEFINED_",
                               "AppendEntries",
                               "RequestVote",
                               "REMOVED DEFINITION",
                               "Reply to vote — GRANTED",
                               "Reply to vote — DENIED",
                               "ERROR — Term Mismatch",
                               "ERROR — Log Mismatch",
                               "AppendEntries Reply OK",
                               "CLIENT HELLO",
                               "CLIENT GOAWAY",
                               "CLIENT VERIFIED",
                               "InstallSnapshot",
                               "InstallSnapshot Reply OK",
                               "RequestPreVote",
                               "PreVote Reply — GRANTED",
                               "PreVote Reply — DENIED",
                               "TimeoutNow"};

typedef enum kraftStorageCmd {
    /* Commands persisted to storage for later replay. */
    KRAFT_STORAGE_CMD_APPEND_ENTRIES = 100, /* actual data */
    KRAFT_STORAGE_CMD_JOINT_CONSENSUS_START = 101,
    KRAFT_STORAGE_CMD_JOINT_CONSENSUS_END = 102,
    KRAFT_STORAGE_CMD_MAX,
} kraftStorageCmd;
// extern char *kraftStorageCmds[];
/* static char *kraftStorageCmds[KRAFT_STORAGE_CMD_MAX];
kraftStorageCmds[KRAFT_STORAGE_CMD_APPEND_ENTRIES-1] = "STORAGE — APPEND
ENTRIES";
kraftStorageCmds[KRAFT_STORAGE_CMD_JOINT_CONSENSUS_START-1] = "STORAGE — START
JOINT CONSENSUS";
*/

typedef struct kraftConsensus {
    /* "Each server stores a current term number,
     * which increases monotonically over time." */
    kraftTerm term;

    /* Highest index and term written to log. */
    kraftRpcUniqueIndex currentIndex;
    kraftRpcUniqueIndex currentTerm;

    kraftRole role;

    struct {
        kraftRpcUniqueIndex index;
        kraftTerm term;
    } previousEntry;

    struct {
        /* "index of the next log entry to send to that server
         * (initialized to leader last log index + 1)" */
        kraftRpcUniqueIndex nextIndex;

        /* "index of highest log entry known to be replicated on server
         * (initialized to 0, increases monotonically)" */
        kraftRpcUniqueIndex matchIndex;
    } leaderData;
} kraftConsensus;

typedef enum kraftIpFamily { KRAFT_IPv4 = 4, KRAFT_IPv6 = 6 } kraftIpFamily;
#define KRAFT_PORTSTRLEN 5        /* value of strlen("65536") */
#define KRAFT_INET6_ADDRSTRLEN 48 /* value of INET6_ADDRSTRLEN */
typedef struct kraftIp {
    char ip[KRAFT_INET6_ADDRSTRLEN];
    int port;
    kraftIpFamily family;
    bool outbound; /* 'true' if IP/Port is in our official client list.
                        (meaning: we can reconnect to this exact IP/Port)
                      'false' if the connection is on an inbound ephemerial port
                        (meaning: we can't reconnect to this IP/Port) */
} kraftIp;

typedef struct kraftVote {
    kraftRunId votedFor;
    vec *votesFromOld;
    vec *votesFromNew;
    /* Pre-vote tracking (Raft thesis §9.6) */
    vec *preVotesFromOld;
    vec *preVotesFromNew;
    /* Term when pre-vote was initiated.
     * Bug fix: Late pre-vote replies from previous attempts can trigger
     * double elections if not filtered by term. Track the pre-vote term
     * and reject replies that don't match. */
    kraftTerm preVoteTerm;
    /* Term when real election was initiated.
     * Used to reject stale vote replies from previous elections. */
    kraftTerm electionTerm;
} kraftVote;

#define KRAFT_VOTE(state) ((state)->vote)
#define KRAFT_VOTED_FOR(state) ((state)->vote.votedFor)
#define KRAFT_SELF_VOTES_FROM_OLD(state) (KRAFT_VOTE(state).votesFromOld)
#define KRAFT_SELF_VOTES_FROM_NEW(state) (KRAFT_VOTE(state).votesFromNew)
#define KRAFT_SELF_PREVOTES_FROM_OLD(state) (KRAFT_VOTE(state).preVotesFromOld)
#define KRAFT_SELF_PREVOTES_FROM_NEW(state) (KRAFT_VOTE(state).preVotesFromNew)
typedef struct kraftNode {
    kraftConsensus consensus;
    kraftClusterId clusterId;
    kraftNodeId nodeId;
    kraftRunId runId;
    kraftTimestamp lastContact;
    struct kraftClient *client;
    kraftNodeStatus status;
    bool disconnect; /* false by default; true if GOAWAY */
    bool isOurself;  /* FIXME placeholder for local serer, don't send data. */
} kraftNode;

/* Reference to a timer for each node to track hearbeats, election
 * timeouts, etc. */
typedef void kraftNodeTimer;

struct kraftState;
struct kraftClient;
struct kraftRpc;
typedef struct kraftInternals {
    struct {
        void (*sendRpc)(struct kraftState *ks, struct kraftRpc *rpc,
                        uint8_t *buf, uint64_t len, kraftNode *node);
    } rpc;
    struct {
        bool (*begin)(struct kraftState *state, char *listenIP, int listenPort);
    } server;
    /*
    struct {
        bool (*logAppend)(struct kraftState *state, kraftTerm term,
                kraftRpcUniqueIndex index, kraftRpc *rpc);
        bool (*logCompact)(struct kraftState *state);
    } entriesLog;
    */
    struct {
        void (*timeout)(struct kraftState *state, void *data);
    } callbacks;
    struct {
        bool (*encodeRpc)(const struct kraftState *state,
                          const struct kraftRpc *rpc, uint8_t **buf,
                          uint64_t *len);
        void (*freeRpc)(void *buf);
        kraftProtocolStatus (*populateRpcFromClient)(struct kraftState *state,
                                                     struct kraftNode *node,
                                                     struct kraftRpc *rpc);
    } protocol;
    struct {
        void (*connectOutbound)(struct kraftState *state, struct kraftIp *ip);
    } network;
    struct {
        bool (*vote)(struct kraftState *state);
        void (*sendHeartbeat)(struct kraftState *state);
    } election;
    struct {
        bool (*leaderContactTimer)(struct kraftState *state,
                                   kraftTimerAction timerAction);
        bool (*heartbeatTimer)(struct kraftState *state,
                               kraftTimerAction timerAction);
        bool (*electionBackoffTimer)(struct kraftState *state,
                                     kraftTimerAction timerAction);
    } timer;
    struct {
        void (*apply)(const void *data, const uint64_t len);
    } data;
    struct {
        bool (*encodeBulk)(struct kraftRpc *rpc, kraftTerm term,
                           kraftStorageCmd originalCmd, const size_t count,
                           const kraftRpcUniqueIndex idx[], const void *data[],
                           const size_t len[]);
        bool (*encode)(struct kraftRpc *rpc, kraftRpcUniqueIndex idx,
                       kraftTerm term, kraftStorageCmd originalCmd,
                       const void *entry, size_t len);
        bool (*decode)(struct kraftRpc *rpc, kraftRpcUniqueIndex *idx,
                       kraftTerm *term, kraftStorageCmd *originalCmd,
                       uint8_t **entry, size_t *len);
        void (*release)(struct kraftRpc *rpc);
    } entry;
    struct {
        bool (*netEncode)(void);
        uint32_t (*netDecode)(uint8_t **ip, uint16_t *port, const void *entry,
                              const size_t len);
    } config;
} kraftInternals;

/* "Servers retry RPCs if they do not receive a response in a timely manner,
 * and they issue RPCs in parallel for best performance." */
typedef struct kraftRpc {
    kraftRpcCmd cmd;
    bool disconnectSource; /* allows code to trigger a node disconnect
                              if there were errors processing the RPC. */
    kraftNode *src;
    kraftClusterId clusterId;
    kraftRunId runId;
    kraftTerm term;
    kraftRpcUniqueIndex leaderCommit;  /* leaderCommit */
    kraftRpcUniqueIndex previousIndex; /* prevLogIndex || lastLogIndex */
    kraftTerm previousTerm;            /* prevLogTerm || lastLogTerm */
    kraftNodeId nodeId;
    kraftTimestamp timestamp;

    /* The RPC data itself */
    flex *entry;
    size_t len;        /* flexBytes(entry); updated after each addition. */
    flexEntry *resume; /* where to resume to get next concat entry */
    size_t remaining;
} kraftRpc;

typedef struct kraftClient {
    void *stream; /* reference to whatever holds the connection. */
    kraftIp ip;
    kraftIp reconnectIp;
    protoKitBuffer buf;
} kraftClient;

typedef uint64_t kraftTimerId;
#if 0
/* Usage:
 *   - Provide your own KRAFT_RECEIVE_DATA function.
 *     - populates a kraftRpc and passes kraftRpc to kraftRpcProcess.
 *   - Provide your own callbacks for functions below.
 *   - Kraft will contact other nodes using sendRpc* functions.
 *   - Kraft will persist to disk using log* functions.
 *   - Kraft will, once a log is replicated, send the log to your state updater.
 */
typedef struct kraftServerAdapter {
    /* callback continuity data */
    void *data;

    /* event loop integration */
    void (*write)(struct kraftState *state, void *client, void *data,
                  size_t len);
    void (*close)(void *data);
    void *(*timerStart)(struct kraftState *state, void *callback,
                        uint64_t timeout, uint64_t repeat);
    void (*timerStop)(void *timerRef);
    void (*timerAgain)(void *data);
} kraftServerAdapter;
#endif

typedef struct kraftNodes {
    /* vec ptr arrays, guaranteed to be unique servers per array. */
    vecPtr *nodes;
    vecPtr *reconnect; /* nodes we want, but aren't connected to */

    vecPtr *pending; /* vec ptr array; unverified connections */
} kraftNodes;

#define _NODES_OLD(state) (&(state)->nodesOld)
#define _NODES_NEW(state) (&(state)->nodesNew)
#define NODES_OLD_PENDING(state) (_NODES_OLD(state)->pending)
#define NODES_NEW_PENDING(state) (_NODES_NEW(state)->pending)
#define NODES_OLD_NODES(state) (_NODES_OLD(state)->nodes)
#define NODES_NEW_NODES(state) (_NODES_NEW(state)->nodes)
// #define kraftOperatingUnderJointConsensus(state) (NODES_NEW_NODES(state) !=
//  NULL)
#define kraftOperatingUnderJointConsensusOldNew(state)                         \
    (!!(state)->jointConsensusStartIndex)
#define kraftOperatingUnderJointConsensusNew(state)                            \
    (!!(state)->jointConsensusEndIndex)
#define kraftOperatingUnderJointConsensusAny(state)                            \
    (kraftOperatingUnderJointConsensusOldNew(state) ||                         \
     kraftOperatingUnderJointConsensusNew(state))

#define kraftRpcHasJointConsensusStart(state, rpc)                             \
    ((rpc)->previousIndex >= (state)->jointConsensusStartIndex)
#if 0
#define NODES_OLD_PENDING(state) (NODES_OLD(state)->pending)
#define NODES_NEW_PENDING(state) (NODES_NEW(state)->pending)
#endif

typedef enum kraftRunMode {
    KRAFT_RUN_MODE_INVALID = 0,
    KRAFT_RUN_MODE_RAFT = 1,
    KRAFT_RUN_MODE_STANDALONE,
} kraftRunMode;

/* Leadership Transfer (Raft thesis §3.10)
 * Enables zero-downtime leader changes for maintenance operations */
typedef enum kraftTransferState {
    KRAFT_TRANSFER_NONE = 0,
    KRAFT_TRANSFER_CATCHING_UP, /* Target follower is catching up to leader's
                                   log */
    KRAFT_TRANSFER_READY,       /* Target is caught up, ready to take over */
    KRAFT_TRANSFER_TIMEOUT      /* Transfer failed due to timeout */
} kraftTransferState;

typedef struct kraftLeadershipTransfer {
    kraftNode *targetNode;    /* Node that will become the new leader */
    kraftTransferState state; /* Current state of the transfer */
    uint64_t startTime;       /* When the transfer was initiated */
    uint64_t timeout; /* Transfer deadline (typically 2x election timeout) */
    kraftRpcUniqueIndex targetMatchIndex; /* Target's last replicated index */
} kraftLeadershipTransfer;

/* ReadIndex Protocol (Raft thesis §6.4)
 * Enables linearizable reads without writing to the log */
typedef struct kraftReadIndexRequest {
    kraftRpcUniqueIndex readIndex; /* commitIndex when read was initiated */
    uint64_t requestTime;       /* When the request was created (for timeout) */
    uint64_t confirmationRound; /* Which heartbeat round confirmed this read */
    bool confirmed;             /* True once majority confirms leadership */
} kraftReadIndexRequest;

typedef struct kraftReadIndex {
    vecPtr *pendingReads;        /* vecPtr of kraftReadIndexRequest* */
    uint64_t currentRound;       /* Current heartbeat round number */
    uint64_t lastConfirmedRound; /* Last round that achieved quorum */
} kraftReadIndex;

#define KRAFT_ENTRIES(state) (&(state->log.kvidx))
#define kraftHasLeader(state) (!!state->leader)
typedef struct kraftState {
    /* Voting details for ourself */
    kraftVote vote;

    /* Node details */
    kraftNodes nodesOld; /* old / current cluster nodes */
    kraftNodes nodesNew; /* new / joint consensus cluster nodes */

    /* Pluggable Functions */
    kraftInternals internals;
    protoKit protoKit;

    /* Maint Timer */
    void *maintTimer; /* connection maintenance */

    /* Reference back to networking adapter. */
    void *adapterState;

    /* Election Timer */
    kraftNodeTimer *leaderContactTimer; /* begin election if timeout triggers */
    kraftNodeTimer
        *leaderHeartbeatSending; /* send heartbeat to nodes when timeout */
    kraftNodeTimer
        *candidateElectionBackoff; /* restarting vote after failed election */

    /* Initial nodes to contact for bootstrap at startup */
    /* REPLACE WITH BETTER CONFIG MANAGEMENT */
    kraftIp **initialIps;
    uint32_t initialIpsCount;

    /* Node for ourself to track our own consensus data. */
    kraftNode *self;
    kraftNode *leader;

    /* Event loop */
    kraftRpcEventLoop *loop;

    /* "Volatile state on all servers" */
    /* "index of highest log entry known to be committed
     * (initialized to 0, increases monotonically)" */
    kraftRpcUniqueIndex commitIndex;

    /* "index of highest log entry applied to state machine
     * (initialized to 0, increases monotonically)" */
    kraftRpcUniqueIndex lastApplied;

    /* Custom Kraft field:
     *   - record index of JOINT_CONSENSUS_START.
     *   - when start is committed (i.e. replicated to majority),
     *     we send JOINT_CONSENSUS_STOP.
     */
    kraftRpcUniqueIndex jointConsensusStartIndex;
    kraftRpcUniqueIndex jointConsensusEndIndex;

    /* Snapshotting for log compaction */
    struct {
        kraftRpcUniqueIndex lastIncludedIndex; /* last log index in snapshot */
        kraftTerm lastIncludedTerm;            /* term of last included index */
        uint64_t triggerThreshold; /* log size to trigger snapshot */
    } snapshot;

    /* Logging and Persistence */
    struct {
        kvidxInstance kvidx; /* entry log persisted to disk */
    } log;

    /* Runtime mode; circumvents some direct Raft logic so we
     * can still use Kraft as a local persist framework without
     * any replicas/followers needed. */
    struct {
        kraftRunMode runningAs;
    } mode;

    /* Configuration: Feature flags and tuning parameters */
    struct {
        /* Pre-Vote Protocol (Raft thesis §9.6)
         * Prevents election disruption when partitioned nodes reconnect.
         * Enabled by default for production stability. */
        bool enablePreVote;

        /* Leadership Transfer (Raft thesis §3.10)
         * Enables zero-downtime leadership handoff for maintenance.
         * TEMPORARILY DISABLED for debugging 7-node invariant violation. */
        bool enableLeadershipTransfer;

        /* ReadIndex Protocol (Raft thesis §6.4)
         * Enables linearizable reads without log writes for 100x read
         * throughput. Enabled by default for performance. */
        bool enableReadIndex;

        /* Test Mode: When true, disables timing-based checks that don't work
         * in state-machine simulation tests (e.g., fuzzer).
         * Set by test harness, default false for production. */
        bool testMode;
    } config;

    /* Leadership Transfer (Raft thesis §3.10)
     * Enables zero-downtime leadership changes for maintenance operations.
     * When active, the leader helps a specific follower catch up, then
     * sends it a TimeoutNow RPC to immediately start an election. */
    kraftLeadershipTransfer transfer;

    /* ReadIndex Protocol (Raft thesis §6.4)
     * Enables linearizable reads without writing to the log.
     * Leader confirms it's still leader via heartbeat, then executes read. */
    kraftReadIndex readIndex;

    /* Metrics & Observability
     * Tracks counters, gauges, and histograms for monitoring and debugging.
     * Zero-allocation after init, minimal overhead in hot path. */
    kraftMetrics metrics;

    /* Witness Nodes
     * Non-voting nodes that receive log entries but don't participate
     * in elections or quorum. Useful for read replicas, geo-distribution,
     * and catching up new nodes before adding to cluster. */
    kraftWitnessManager witnesses;

    /* Async Commit Modes
     * Configurable durability levels that trade consistency for latency.
     * Supports STRONG (default), LEADER_ONLY, ASYNC, and FLEXIBLE modes. */
    kraftAsyncCommitManager asyncCommit;

    /* Intelligent Snapshots
     * Log compaction through automatic snapshot triggering, incremental
     * snapshots, and parallel transfer to followers. */
    kraftSnapshotManager snapshots;
} kraftState;

/* Init (state) */
kraftState *kraftStateNew(void);
void kraftStateFree(kraftState *state);

/* Init (functionality) */
bool kraftStateInitRaft(kraftState *state, size_t ipCount, kraftIp **ip,
                        const kraftClusterId clusterId,
                        const kraftNodeId selfNodeId);
bool kraftStateInitStandalone(kraftState *state);

void kraftStateSetPreviousIndexTerm(kraftState *kraft,
                                    kraftRpcUniqueIndex previousIndex,
                                    kraftTerm previousTerm);

/* Refresh previousEntry from storage after log modifications.
 * Must be called after log deletions/truncations to maintain Election
 * Restriction (§5.4.1) invariants. */
bool kraftStateRefreshPreviousEntry(kraftState *state);

/* Node Managment */
kraftNode *kraftNodeNew(kraftClient *client);
kraftNode *kraftNodeAllocateSelf(kraftClusterId clusterId, kraftNodeId nodeId);

kraftClient *kraftClientNew(kraftState *state, void *stream);
void kraftNodeFree(kraftState *state, kraftNode *node);

void kraftNodeForceVerify(kraftNodes *nodes, kraftNode *node);
bool kraftNodeMoveFromPendingToVerified(kraftState *state, kraftNode *pending);
kraftNode *kraftNodeAddPending(kraftState *state, kraftNodes *nodes,
                               kraftNode *node);
kraftNode *kraftNodeCreateOld(kraftState *state, kraftClient *client);
kraftNode *kraftNodeCreateNew(kraftState *state, kraftClient *client);

#if 0
#define kraftNodeMoveFromPendingToVerifiedOld(state, pending)                  \
    (_kraftNodeMoveFromPendingToVerified(NODES_OLD(state), pending))
#define kraftNodeMoveFromPendingToVerifiedNew(state, pending)                  \
    (_kraftNodeMoveFromPendingToVerified(NODES_NEW(state), pending))
#define kraftNodeAddPendingOld(state, node)                                    \
    (kraftNodeAddPending(NODES_OLD(state), node))
#define kraftNodeADdPendingNew(state, node)                                    \
    (kraftNodeAddPending(NODES_NEW(state), node))
#define kraftNodeCreateOld(state, client)                                      \
    (kraftNodeCreate(NODES_OLD(state), client))
#define kraftNodeCreateNew(state, client)                                      \
    (kraftNodeCreate(NODES_NEW(state), client))
#endif

/* internal utils */
uint64_t kraftustime(void);
uint64_t kraftmstime(void);

#endif /* KRAFT_H */
