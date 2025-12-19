#ifndef ENTRY_CODER_H
#define ENTRY_CODER_H

#include "../kraft.h"

bool entryCoderEncodeBulk(kraftRpc *rpc, kraftTerm term,
                          kraftStorageCmd originalCmd, const size_t count,
                          const size_t idx[], const void *data[],
                          const size_t len[]);

bool entryCoderEncode(kraftRpc *rpc, kraftRpcUniqueIndex idx, kraftTerm term,
                      kraftStorageCmd originalCmd, const void *entry,
                      size_t len);
bool entryCoderDecode(kraftRpc *rpc, kraftRpcUniqueIndex *idx, kraftTerm *term,
                      kraftStorageCmd *originalCmd, uint8_t **entry,
                      size_t *len);
void entryCoderFree(kraftRpc *rpc);
#endif /* ENTRY_CODER_H */
