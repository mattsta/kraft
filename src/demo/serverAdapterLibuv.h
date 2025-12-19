#ifndef SERVER_ADAPTER_LIBUV_H
#define SERVER_ADAPTER_LIBUV_H

#include "../kraft.h"
#include <stdbool.h>

bool kraftServerAdapterLibuvTimerLeaderContact(struct kraftState *state,
                                               kraftTimerAction timerAction);
bool kraftServerAdapterLibuvTimerHeartbeatSending(kraftState *state,
                                                  kraftTimerAction timerAction);
bool kraftServerAdapterLibuvTimerElectionBackoff(kraftState *state,
                                                 kraftTimerAction timerAction);

void kraftServerAdapterLibuvConnectOutbound(kraftState *state, kraftIp *ip);

bool kraftServerAdapterLibuvBegin(kraftState *state, char *listenIP,
                                  int listenPort);

void serverAdapterLibuvSendRpc(kraftState *state, kraftRpc *rpc, uint8_t *buf,
                               uint64_t len, kraftNode *node);

#endif /* SERVER_ADAPTER_LIBUV_H */
