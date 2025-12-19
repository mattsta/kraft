#include "serverAdapter.h"

bool kraftServerAdapterStart(kraftState *state) {
    if (!state) {
        return false;
    }

    //    kraftServerConfig *config = state->config;
    D("Starting server using kraftAdapter...");
    kraftIp *server = state->initialIps[0];
    state->internals.server.begin(state, server->ip, server->port);
    D("Server exited...");
    return true;
}
