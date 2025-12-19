#ifndef RPC_CANDIDATE_H
#define RPC_CANDIDATE_H

#include "rpc.h"
#include "rpcFollower.h"

/* Internal API for event loop processing */
bool kraftRpcProcessCandidate(kraftState *state, kraftRpc *rpc,
                              kraftRpcStatus *status);

#endif /* RPC_CANDIDATE_H */
