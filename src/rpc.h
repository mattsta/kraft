#ifndef KRAFT_RPC_H
#define KRAFT_RPC_H

#include "kraft.h"

#define IS_FOLLOWER(node) ((node)->consensus.role == KRAFT_FOLLOWER)
#define IS_CANDIDATE(node) ((node)->consensus.role == KRAFT_CANDIDATE)
#define IS_LEADER(node) ((node)->consensus.role == KRAFT_LEADER)
#define IS_PRE_CANDIDATE(node) ((node)->consensus.role == KRAFT_PRE_CANDIDATE)

/* ROLE becomes KRAFT_[ROLE] due to the '##' concatentation operator */
#define BECOME(ROLE, node)                                                     \
    do {                                                                       \
        assert(KRAFT_##ROLE != (node)->consensus.role);                        \
        D("\tCONVERTING from %s to %s", kraftRoles[(node)->consensus.role],    \
          #ROLE);                                                              \
        (node)->consensus.role = KRAFT_##ROLE;                                 \
    } while (0)

#define MIN(X, Y) ((X) < (Y) ? (X) : (Y))
#define MAJORITY(X) (((X) / 2) + 1)

#define kraftErrorReply(code)                                                  \
    do {                                                                       \
        D("REPLYING ERROR");                                                   \
        *(status) = (code);                                                    \
        kraftRpcReplyError(state, (code), rpc);                                \
        return false;                                                          \
    } while (0)

#define kraftOK()                                                              \
    do {                                                                       \
        *(status) = KRAFT_RPC_STATUS_OK;                                       \
        return true;                                                           \
    } while (0)

#define kraftOKReply(cmd)                                                      \
    do {                                                                       \
        *(status) = KRAFT_RPC_STATUS_OK;                                       \
        D("Sending ok reply...");                                              \
        kraftRpcReplyNoBody(state, (cmd), rpc);                                \
        return true;                                                           \
    } while (0)

/* Public API for node management */
uint32_t _majorityOfNodes(const kraftNodes *nodes);
uint32_t clusterNodesCountOld(kraftState *state);
uint32_t clusterNodesCountNew(kraftState *state);
uint32_t majorityOfNodesOld(kraftState *state);
uint32_t majorityOfNodesNew(kraftState *state);
uint32_t majorityOfNodesAll(kraftState *state);

/* Node Change Manipulation APIs */
void kraftRpcJointConsensusIfNecessary(kraftState *state,
                                       kraftRpcUniqueIndex idx,
                                       kraftStorageCmd cmd, uint8_t *entry,
                                       uint32_t entryLen);
void kraftRpcJointConsensusBegin(kraftState *state, uint8_t *entry,
                                 uint32_t entryLen);
void kraftRpcJointConsensusFinalize(kraftState *state);

/* Public API to server event loop callbacks */
bool kraftRpcProcess(kraftState *state, kraftRpc *rpc, kraftRpcStatus *status);
bool kraftRpcRequestVote(kraftState *state);
bool kraftRpcRequestPreVote(kraftState *state);
void kraftRpcSendHeartbeat(kraftState *state);

/* Private API for RPC Sending */
void kraftRpcSendSingle(kraftState *state, kraftRpc *rpc, kraftNode *node);
void kraftRpcSendBroadcast(kraftState *state, kraftRpc *rpc);

/* Private API for log appending */
bool kraftLogAppend(kraftState *state, kraftRpcUniqueIndex index,
                    kraftTerm term, kraftStorageCmd cmd, const void *data,
                    uint64_t len);

/* Private API for updating record of majority-replicated index points
 * as well as applying properly replicated index points to local state machine.
 */
void kraftUpdateCommitIndex(kraftState *state, kraftRpcUniqueIndex N);

/* Snapshot and log compaction APIs */
bool kraftSnapshotCreate(kraftState *state);
bool kraftSnapshotInstall(kraftState *state,
                          kraftRpcUniqueIndex lastIncludedIndex,
                          kraftTerm lastIncludedTerm);
void kraftLogCompact(kraftState *state, kraftRpcUniqueIndex upToIndex);
void kraftSnapshotCheckAndTrigger(kraftState *state);

/* Private API to other Kraft RPC Types */
void kraftRpcReplyError(kraftState *state, kraftRpcStatus status,
                        kraftRpc *rpc);
void kraftRpcReplyNoBody(kraftState *state, kraftRpcCmd cmd, kraftRpc *rpc);
void kraftRpcReplyAppendEntriesOK(kraftState *state, kraftRpc *rpc,
                                  kraftRpcUniqueIndex matchIndex,
                                  kraftTerm matchTerm);
bool kraftRpcPopulate(kraftState *state, kraftRpc *rpc);

#endif /* KRAFT_RPC_H */
