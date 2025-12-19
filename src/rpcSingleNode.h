#pragma once

#include "rpc.h"

/* When persisting a log to disk _without_ current replication,
 * but you may want replication in the futre, start here. */

typedef struct kraftRpcInbound {
    void *wubbalubba;
} kraftRpcInbound;

bool kraftRpcSingleNodeRpcInboundAdd(kraftState *state, kraftRpcInbound *irpc);
bool kraftRpcSingleNodeAppend(kraftState *state, kraftRpcInbound *irpc);
