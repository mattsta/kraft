#include "rpcSingleNode.h"

/* Init:
 *  - On load:
 *      - get highest index and term
 *      - increment term
 *      - (store index and term in local state)
 *  - On persist new value:
 *      - increment index
 *      - store by AST of what to store */

/* Also need ability to load from storage for a given filename
 * and populate continuation state! */

bool kraftRpcSingleNodeRpcInboundAdd(kraftState *state, kraftRpcInbound *irpc) {
    return true;
}

bool kraftRpcSingleNodeAppend(kraftState *state, kraftRpcInbound *irpc) {
    return true;
}
