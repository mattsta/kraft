#ifndef KRAFT_PROTOCOL_BINARY_H
#define KRAFT_PROTOCOL_BINARY_H

#include "../kraft.h"
#include "../rpc.h"

bool kraftProtocolBinaryEncode(const kraftState *state, const kraftRpc *rpc,
                               uint8_t **buf, uint64_t *len);
bool kraftProtocolBinaryParse(kraftState *state, kraftRpc *rpc, void *input,
                              uint64_t len);
bool bufferHasValidRPC(protoKitBuffer *buf);
void kraftProtocolFree(void *buf);

kraftProtocolStatus kraftProtocolBinaryPopulateRpc(kraftState *state,
                                                   kraftNode *node,
                                                   kraftRpc *rpc);
#endif /* KRAFT_PROTOCOL_BINARY_H */
