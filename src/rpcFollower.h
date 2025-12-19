#ifndef RPC_FOLLOWER_H
#define RPC_FOLLOWER_H

#include "rpc.h"

/* Internal API for event loop processing */
/* Internal API for Candidate to transition directly to Follower */
bool kraftRpcProcessFollower(kraftState *state, kraftRpc *rpc,
                             kraftRpcStatus *status);

#endif /* RPC_FOLLOWER_H */
