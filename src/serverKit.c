#include "serverKit.h"

#include "rpcLeader.h"

/* Kraft has a very forgiving connection mechanism.
 * Every client has the _same_ list of cluster nodes, and on
 * startup every client: listens for other clients, then
 * connects to _every_ client in the cluster list.
 *
 * But, since every client has the same cluster list, Every
 * client will both connect to itself and accept a connection
 * from itself, resulting in TWO false connections.
 * But, we *don't* want to be a client of ourself.
 *
 * Below, the HELLO->VERIFIED or HELLO->GOAWAY flow allows
 * us to detect when we are connected to ourself then
 * send back a GOAWAY message to ourself so we remove ourself
 * from our list of cluster nodes we should keep open
 * connections to.
 *
 * This mechanism could *also* be used to implement other
 * kinds of access control.
 *
 * The types of access control implemented below are:
 *   - if runId of remote matches ourself, disconnect.
 *   - if clusterId if remote *doesn't* match ourself, disconnect.
 */

/* INTERNAL API */
static bool _verified(kraftState *state, kraftNode *node) {
    kraftRpc okay = {0};
    kraftRpcPopulate(state, &okay);
    okay.cmd = KRAFT_RPC_VERIFIED;

    /* Node is good.  Initialize our next entry for sending. */
    kraftRpcSetNextIndex(state, node);

    kraftRpcSendSingle(state, &okay, node);
    return true;
}

static bool __verifyHello(kraftState *state, vecPtr *nodesHolder,
                          kraftRpc *rpc) {
    if (!nodesHolder) {
        assert(NULL && "Nodes doesn't exist!");
        return false;
    }

    uint32_t len = vecPtrCount(nodesHolder);

    if (!rpc->src->runId) {
        assert(NULL && "RPC didn't include valid runId!");
    }

    /* This loop checks 'nodesHolder' to verify our new connection (rpc->src)
     * is *not* an existing connection.  This protects against accidental
     * misconfiguration of users requesting one client process connect to
     * a serverm multiple times. */
    for (uint32_t i = 0; i < len; i++) {
        kraftNode *compareNode = NULL;
        bool found =
            (vecPtrGet(nodesHolder, i, (void **)&compareNode) == VEC_OK);

        if (!found) {
            D("Node not found at index %u even though len is %u!", i, len);
            /* If we didn't find the node in this list, return false so the
             * caller tries the next node list.
             * Nodes are tested in the following order: old, new */
            return false;
        }

        D("Comparing (%p) %" PRIu64 " against %" PRIu64 "", (void *)compareNode,
          compareNode->runId, rpc->runId);
        if (compareNode->runId == rpc->src->runId) {
            /* We found this node's run ID already in our
             * client list! */

            /* If we found an outbound connection,
             * we need to copy the OUTBOUND details to the node we are
             * keeping so we can reconnect outbound when necessary. */
            if (rpc->src->client->ip.outbound &&
                !compareNode->client->ip.outbound) {
                /* We are KEEPING nodes[i]. Copy outbound details there. */
                compareNode->client->reconnectIp =
                    rpc->src->client->reconnectIp;
            } else {
                /* we don't have an outbound details for 'node[i]' (yet)
                 * we won't be able to reconnect if it fails. */
            }

            /* reply GOAWAY to 'node' */
            D("REJECTING DUPLICATE CONNECTION!");
            serverKitSendGoAway(state, rpc->src);
            return false;
        }
    }

    /* runId not found in existing client list.  Good to go. */
    _verified(state, rpc->src);
    return true;
}

static bool _verifyHello(kraftState *state, kraftRpc *rpc) {
    /* Iterate over NODES_{OLD,NEW} and move from pending to verified
     * where appropriate. */
    kraftNodes *nodesOldNew[2] = {_NODES_OLD(state), _NODES_NEW(state)};

    for (uint32_t i = 0; i < sizeof(nodesOldNew) / sizeof(*nodesOldNew); i++) {
        /* Have we verified this INBOUND connection? */
        bool verified = __verifyHello(state, nodesOldNew[i]->nodes, rpc);
        if (verified) {
            kraftNodeMoveFromPendingToVerified(state, rpc->src);
            /* Our node can only exist in the pending list *once* */
            return true;
        }
    }
    return false;
}

/* EXTERNAL API */
void serverKitSendGoAway(kraftState *state, kraftNode *node) {
    D("Closing connection to node (%p) because sending GOAWAY.", (void *)node);
    node->disconnect = true;
    kraftRpc finishHim = {0};
    kraftRpcPopulate(state, &finishHim);
    finishHim.cmd = KRAFT_RPC_GOAWAY;
    kraftRpcSendSingle(state, &finishHim, node);
}

bool serverKitProcessHello(kraftState *state, kraftRpc *rpc) {
    /* If we're connecting to ourself, we need to go away. */
    /* This verifies we will NEVER process RPCs from ourself. */
    if (rpc->runId == state->self->runId) {
        D("Closing connection because of self RunId (%" PRIu64 " vs %" PRIu64
          ")",
          rpc->runId, state->self->runId);
        serverKitSendGoAway(state, rpc->src);
        return false;
    }

    rpc->src->runId = rpc->runId;

    /* If this HELLO RPC has different clusterId from us, then also reject
     * their attempted connection. */
    if (rpc->clusterId != state->self->clusterId) {
        D("Closing connection because NOT same cluster id! us vs them (%" PRIu64
          " vs %" PRIu64 ")",
          rpc->clusterId, state->self->clusterId);
        serverKitSendGoAway(state, rpc->src);
        return false;
    }

    D("Processing handshake RPC %s", kraftRpcCmds[rpc->cmd]);
    switch (rpc->cmd) {
    case KRAFT_RPC_VERIFIED:
        /* VERIFIED = peer GIVING verification */
        rpc->src->status = KRAFT_NODE_VERIFIED;
        /* We've been externally VERIFIED by our peer. */
        kraftNodeMoveFromPendingToVerified(state, rpc->src);
        return true;
    case KRAFT_RPC_HELLO:
        /* HELLO = peer REQUESTING verification */
        return _verifyHello(state, rpc);
    case KRAFT_RPC_GOAWAY:
        /* GOAWAY = peer intentionally REJECTED connection attempt */
        /* We can take a hint. We aren't wanted anymore. */
        rpc->src->disconnect = true;
        return false;
    default:
        D("Failing hello because not hello or goaway or verified.");
        /* REMOVE AFTER TESTING: can't have bad HELLO RPCs crash servers. */
        assert(NULL && "ERROR: Received unexpected RPC command during "
                       "connection agreement!");
        return false;
    }
}

void serverKitSendHello(kraftState *state, kraftNode *node) {
    kraftRpc howdy = {0};
    kraftRpcPopulate(state, &howdy);
    howdy.cmd = KRAFT_RPC_HELLO;
    kraftRpcSendSingle(state, &howdy, node);
    node->status = KRAFT_NODE_HELLO_SENT;
}
