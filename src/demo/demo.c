#include "../kraft.h"
#include "entryCoder.h"
#include "kraftStateMachine.h"
// #include "netpack.h"
#include "../protoKit.h"
#include "protocolBinary.h"

#include <unistd.h> /* getpid() */

#include "../serverAdapter.h"
#include "serverAdapterLibuv.h"

#include "../ctest.h"

#if 0
void createCluster(kraftState *state, int size) {
    demoInit(state);
    for (int i = 0; i < size; i++) {
        kraftNodeAdd(state, kraftNodeAllocate(NULL));
    }
}

void destroyCluster(kraftState *state) {
    for (uint32_t i = 0; i < NODES_COUNT(state); i++) {
        kraftNodeFree(state, NODE(state, i));
    }
    kraftFree(state);
}

void runVoteCycle(kraftState *state) {
    for (uint32_t i = 0; i < NODES_COUNT(state); i++) {
        //        kraftRpcVote(state->nodes[i]);
    }
}
#endif

bool demoInit(kraftState *state) {
    if (!state) {
        return false;
    }

    /* CONFIG SETTINGS:
     *   - cluster id
     *   - node id
     *   - election timeout (time without heartbeats before voting)
     *   - cluster nodes at startup
     *   - */

    /* User provides:
     *   - reader for config
     *   - writer for state
     */

    protoKit pk = {.bufferHasValidPDU = bufferHasValidRPC};

    const kvidxAdapterInfo *adapter = kvidxGetAdapterByName("SQLite3");
    if (adapter) {
        state->log.kvidx.interface = *adapter->iface;
    }

    char fname[64] = {0};
    snprintf(fname, sizeof(fname), "datalog-%d.sqlite3",
             state->initialIps[0]->port);

    kvidxOpen(KRAFT_ENTRIES(state), fname, NULL);

    state->protoKit = pk;

    state->internals.protocol.encodeRpc = kraftProtocolBinaryEncode;
    state->internals.protocol.freeRpc = kraftProtocolFree;
    state->internals.protocol.populateRpcFromClient =
        kraftProtocolBinaryPopulateRpc;

    state->internals.network.connectOutbound =
        kraftServerAdapterLibuvConnectOutbound;
    state->internals.callbacks.timeout = NULL;
    state->internals.election.vote = kraftRpcRequestVote;
    state->internals.election.sendHeartbeat = kraftRpcSendHeartbeat;

    /* For followers */
    state->internals.timer.leaderContactTimer =
        kraftServerAdapterLibuvTimerLeaderContact;

    /* For leader */
    state->internals.timer.heartbeatTimer =
        kraftServerAdapterLibuvTimerHeartbeatSending;

    /* For candidates */
    state->internals.timer.electionBackoffTimer =
        kraftServerAdapterLibuvTimerElectionBackoff;

    /* For encoding / decoding RPC AppendEntries entries */
    state->internals.entry.encode = entryCoderEncode;
    state->internals.entry.decode = entryCoderDecode;
    state->internals.entry.release = entryCoderFree;

    /* For encoding / decoding IP:Port inside START_JOINT_CONSENSUS RPCs */
    state->internals.config.netDecode = NULL;

    /* Send RPC to other nodes */
    state->internals.rpc.sendRpc = serverAdapterLibuvSendRpc;

    /* Commit log entry to state machine. */
    state->internals.data.apply = kraftStateUpdate;

    /* Start listening, connecting, and running event loop. */
    state->internals.server.begin = kraftServerAdapterLibuvBegin;

    return true;
}

int main(int argc, char *argv[]) {
    kraftState *state = kraftStateNew();

    /** Dynamic Testing **/

    size_t ipCount = argc - 1;
    /* Init server interface */
    kraftIp **ips = zcalloc(1, sizeof(*ips) * ipCount);

    for (uint32_t i = 0; i < ipCount; i++) {
        ips[i] = zcalloc(1, sizeof(*ips[i]));
        strncpy(ips[i]->ip, "127.0.0.1", sizeof(ips[i]->ip));
        ips[i]->port = atoi(argv[i + 1]);
    }

    kraftStateInitRaft(state, ipCount, ips, 20, 1);

    demoInit(state);

    kraftServerAdapterStart(state);

    /** Static testing **/
    /* Create cluster of 3 */
    //    createCluster(state, 3);

    /* Run vote cycle, elect leader. */
    //    runVoteCycle(state);

    /* Get leader */

    /* send messages to leader */

    /* verify messages replicate to followers */

    /* kill leader */

    /* Run vote cycle */

    /* verify new leader exists */

    /* verify new leader has all data */

    /* send messages to new leader */

    /* verify messages replicate to remaining follower. */

    //    destroyCluster(state);
    return 0;
}
