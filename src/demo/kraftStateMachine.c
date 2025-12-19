#include "kraftStateMachine.h"
#include <stdio.h>

// static uint64_t *kraftStateMachineGlobalStorage = NULL;

void kraftStateUpdate(const void *inLog, const uint64_t len) {
    if (!inLog || !len) {
        return;
    }

    const uint8_t *log = inLog;
    printf("SET STATE MACHINE TO %s!\n", log + 1);

#if 0
    switch (log[0]) {
    case KRAFT_STATE_MACHINE_GET:
        /* Our state updating doesn't handle gets.  It doesn't
         * make sense to 'get' an update to the database since updates
         * change state, and a 'get' will never change state. */
        break;
    case KRAFT_STATE_MACHINE_SET:
//        dumbAppendGrow(&kraftStateMachineGlobalStorage, *(uint64_t *)(log + 1));
        break;
    }
#endif
}
