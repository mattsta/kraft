#undef KRAFT_TEST_VERBOSE
#include "protocolBinary.h"
#include "../protoKit.h"

#include "endianForceLittle.h"

#include "../../deps/xxHash/xxhash.h"

/* Encode 'rpc' to wire format in 'buf' of size 'len' */
bool kraftProtocolBinaryEncode(const kraftState *state, const kraftRpc *rpc,
                               uint8_t **inBuf, uint64_t *len) {
    (void)state;

    if (!rpc || !inBuf || !len) {
        return false;
    }

    flex *protocol = flexNew();

#define mku(u_) {.data.u = u_, .type = DATABOX_UNSIGNED_64}
    databox clusterId = mku(state->self->clusterId);
    databox nodeId = mku(state->self->nodeId);
    databox runId = mku(state->self->runId);
    databox term = mku(state->self->consensus.term);
    databox previousIndex = mku(rpc->previousIndex);
    databox previousTerm = mku(rpc->previousTerm);
    databox leaderCommit = mku(rpc->leaderCommit);
    databox timestamp = mku(kraftustime());
    databox rpcCmd = mku(rpc->cmd);

    databox entry = {.data.bytes.start = rpc->entry,
                     .len = rpc->entry ? flexBytes(rpc->entry) : 0,
                     .type = DATABOX_BYTES};

    const databox *fields[] = {
        &clusterId,    &nodeId,       &runId,     &term,   &previousIndex,
        &previousTerm, &leaderCommit, &timestamp, &rpcCmd, &entry};

    /* Populate data for RPC command */
    switch (rpc->cmd) {
    case KRAFT_RPC_APPEND_ENTRIES:
        /* Append client data */
        flexAppendMultiple(&protocol, 10, fields);
        break;
    /* Remaining RPCs have no data to append. */
    case KRAFT_RPC_REQUEST_VOTE:
    case KRAFT_RPC_VOTE_REPLY_GRANTED:
    case KRAFT_RPC_VOTE_REPLY_DENIED:
    case KRAFT_RPC_APPEND_ENTRIES_REPLY_OK:
    case KRAFT_RPC_APPEND_ENTRIES_REPLY_IDX_TERM_MISMATCH:
    case KRAFT_RPC_APPEND_ENTRIES_REPLY_TERM_MISMATCH:
    case KRAFT_RPC_HELLO:
    case KRAFT_RPC_GOAWAY:
    case KRAFT_RPC_VERIFIED:
        flexAppendMultiple(&protocol, 9, fields);
        break;
    default:
        assert(NULL && "Invalid command encoded!");
    }

    *inBuf = protocol;
    *len = flexBytes(protocol);

    return true;
}

bool kraftProtocolBinaryDecode(kraftRpc *rpc, uint8_t *data, uint64_t len) {
    if (!rpc || !data) {
        return false;
    }

    databox clusterId;
    databox nodeId;
    databox runId;
    databox term;
    databox previousIndex;
    databox previousTerm;
    databox leaderCommit;
    databox timestamp;
    databox rpcCmd;
    databox entry;

    assert(len == flexBytes(data));

    flexEntry *current = flexHead(data);
    flexGetByType(current, &clusterId);
    flexGetNextByType(data, &current, &nodeId);
    flexGetNextByType(data, &current, &runId);
    flexGetNextByType(data, &current, &term);
    flexGetNextByType(data, &current, &previousIndex);
    flexGetNextByType(data, &current, &previousTerm);
    flexGetNextByType(data, &current, &leaderCommit);
    flexGetNextByType(data, &current, &timestamp);
    flexGetNextByType(data, &current, &rpcCmd);

    /* Populate header details */
    rpc->term = term.data.u;
    rpc->clusterId = clusterId.data.u;
    rpc->nodeId = nodeId.data.u;
    rpc->runId = runId.data.u;
    rpc->timestamp = timestamp.data.u;
    rpc->previousIndex = previousIndex.data.u;
    rpc->previousTerm = previousTerm.data.u;
    rpc->leaderCommit = leaderCommit.data.u;

    /* RPC is one byte */
    rpc->cmd = rpcCmd.data.u;

    /* Parse remainder based on RPC command */
    switch (rpc->cmd) {
    case KRAFT_RPC_APPEND_ENTRIES:
        flexGetNextByType(data, &current, &entry);
        /* Only populate RPC if we have entries.
         * Otherwise, it's an empty heartbeat. */
        rpc->entry = entry.data.bytes.start;

        if (rpc->entry) {
            assert(flexCount(rpc->entry) % 4 == 0);
            rpc->remaining = flexCount(rpc->entry) / 4;
        }
        /* TODO: copy?
         *
         * Right now this is still INSIDE the client buffer! */
        return true;
        break;
    case KRAFT_RPC_REQUEST_VOTE:
    case KRAFT_RPC_VOTE_REPLY_GRANTED:
    case KRAFT_RPC_VOTE_REPLY_DENIED:
    case KRAFT_RPC_APPEND_ENTRIES_REPLY_OK:
    case KRAFT_RPC_APPEND_ENTRIES_REPLY_IDX_TERM_MISMATCH:
    case KRAFT_RPC_APPEND_ENTRIES_REPLY_TERM_MISMATCH:
    case KRAFT_RPC_HELLO:
    case KRAFT_RPC_GOAWAY:
    case KRAFT_RPC_VERIFIED:
        D("Processed RPC Command %s", kraftRpcCmds[rpc->cmd]);
        break;
    default:
        assert(NULL && "Invalid RPC Command Decoded!");
        break;
    }

    return true;
}

bool bufferHasValidRPC(protoKitBuffer *buf) {
    ptrdiff_t currentLen = PK_CURRENT_LENGTH(buf);

    /* If we don't have a header yet, we can't do anything. */
    if (currentLen < (ptrdiff_t)27) {
        D("No header yet.");
        return KRAFT_PROTOCOL_STATUS_NO_RPC;
    }

    if (!buf->pdu.dataSize) {
        /* If we don't have current data size, read header and populate. */
        ssize_t bytesLength = flexBytesLength(buf->pdu.start);
        if (currentLen >= bytesLength) {
            buf->pdu.dataSize = flexBytes(buf->pdu.start);
            D("buf datasize is: %" PRIu64, buf->pdu.dataSize);
        } else {
            D("Failed to have enough data!\n");
            /* Not enough bytes to read expected length yet.
             * Retry when more data is available. */
            return false;
        }

#if 0
        if (!headerDataSize(buf->pdu.start, currentLen, &buf->pdu.dataSize)) {
            D("ERROR DECODING HEADER!");
            return KRAFT_PROTOCOL_STATUS_PARSE_ERROR; /* err reading header! */
            /* NOTE: Returning error here will cascade up and cause the
             *       client to be disconnected then the rest of this client
             *       buffer will be discarded.
             *       Basically, don't send us invalid RPCs and your
             *       precious client buffer won't be deleted. */
        }
#endif
    }

    ssize_t expectedSize = buf->pdu.dataSize;
    D("Expecting size: %zu, current len: %d", expectedSize, currentLen);

    /* If length is smaller than size we expect, can't process yet. */
    if (currentLen < expectedSize) {
        return false;
    }

    /* else, we have at least one valid RPC in the buffer ready to parse. */
    return true;
}

/* Populate an RPC from unprocessed network data sitting in
 * our 'node->client' buffer. */
kraftProtocolStatus kraftProtocolBinaryPopulateRpc(kraftState *state,
                                                   kraftNode *node,
                                                   kraftRpc *rpc) {
    if (!state || !node || !rpc) {
        return KRAFT_PROTOCOL_STATUS_SYSTEM_ERROR;
    }

    /* We only return one RPC per run.  If we have an RPC, encode it,
     * update buffer offsets, return. */
    /* Possibilities:
     *   - We haven't read an entire RPC yet
     *   - We've read exactly one RPC
     *   - We've read multiple RPCs and need to process all of them. */

    kraftProtocolStatus status;
    protoKitBuffer *buf = &node->client->buf;
    const ptrdiff_t currentLen = PK_CURRENT_LENGTH(buf);
    const ssize_t expectedSize = buf->pdu.dataSize;
    D("Expecting size: %zu, current len: %d", expectedSize, currentLen);

    /* If length is smaller than size we expect, we got called with no RPC! */
    if (!buf->pdu.dataSize || currentLen < expectedSize) {
        return KRAFT_PROTOCOL_STATUS_NO_RPC;
    }

    assert(buf->pdu.dataSize);

    /* else, current length is >= total size we expect for this RPC. */
    if (kraftProtocolBinaryDecode(rpc, buf->pdu.start, expectedSize)) {
        status = KRAFT_PROTOCOL_STATUS_RPC_POPULATED;
    } else {
        D("PROTOCOL ERROR!");
        /* Rewind buffer and accounting data to throw away this bad RPC */
        status = KRAFT_PROTOCOL_STATUS_PARSE_ERROR; /* protocol decode error! */
    }

    protoKitBufferCleanup(&state->protoKit, buf, expectedSize);
    return status;
}

void kraftProtocolFree(void *buf) {
    flexFree(buf);
}
