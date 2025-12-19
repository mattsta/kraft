#ifndef KRAFT_STATE_MACHINE_H
#define KRAFT_STATE_MACHINE_H

#include "../kraft.h"

typedef enum kraftStateMachineState {
    KRAFT_STATE_MACHINE_GET = 7,
    KRAFT_STATE_MACHINE_SET = 8,
} kraftStateMachineState;

void kraftStateUpdate(const void *log, const uint64_t len);

#endif /* KRAFT_STATE_MACHINE_H */
