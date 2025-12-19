#ifndef SERVER_KIT_H
#define SERVER_KIT_H

#include "kraft.h"

#define serverKitNodeNeedsHelloProcessing(node)                                \
    ((node)->status != KRAFT_NODE_VERIFIED)

/* Public API for sending initial 'HELLO' */
void serverKitSendHello(kraftState *state, kraftNode *node);

/* Public API for verifying 'node' is allowed to connect. */
bool serverKitProcessHello(kraftState *state, kraftRpc *rpc);

/* Public API for telling a node to GOAWAY */
void serverKitSendGoAway(kraftState *state, kraftNode *node);

#endif /* SERVER_KIT_H */
