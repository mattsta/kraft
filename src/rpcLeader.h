#ifndef RPC_LEADER_H
#define RPC_LEADER_H

#include "rpc.h"

/* Append 'data' to 'rpc' as new entry */
bool kraftRpcPopulateEntryAppend(kraftState *state, kraftRpc *rpc,
                                 kraftStorageCmd cmd, const void *data,
                                 size_t len);

/* Interal API for event loop processing */
bool kraftRpcProcessLeader(kraftState *state, kraftRpc *rpc,
                           kraftRpcStatus *status);

/* Interal API for updating nextIndex per-node */
kraftRpcUniqueIndex kraftRpcSetNextIndex(kraftState *state, kraftNode *node);

/* User function for sending an RPC to other nodes from the Leader */
bool kraftRpcLeaderSendRpc(kraftState *state, kraftRpc *rpc);

/* Send a custom RPC where user RPC source may have multiple entries we can
 * form into an RPC _once_ instead of appending N times */
bool kraftRpcLeaderSendRpcCustom(kraftState *state, void *customSource,
                                 const size_t sourceLength,
                                 bool (*sourceGetEntry)(void *customSource,
                                                        size_t i, void **data,
                                                        size_t *len),
                                 void (*sourceFreeEntry)(void *entry));

/* Persist user data to disk as appropriate (i.e. if not replicating,
 * just commit to local db, else run the kraft state machine) */
bool kraftRpcLeaderPersistCustom(
    kraftState *restrict state, void *restrict customSource,
    const size_t sourceLength,
    bool (*sourceGetEntry)(void *customSource, size_t i, void **data,
                           size_t *len),
    void (*sourceFreeEntry)(void *entry),
    uint64_t (*sourceWriteEntriesStandalone)(kvidxInstance *restrict ki,
                                             void *restrict customSource,
                                             uint64_t startIdx, uint64_t term));
#endif /* RPC_LEADER_H */
