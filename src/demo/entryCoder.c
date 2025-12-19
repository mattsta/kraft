/* entryCoder.c
 *
 * Encodes user data 'entry' of size 'len' into 'rpc->entry'.
 * Decodes user data from 'rpc->entry' back into individual entries.
 *
 * The 'rpc->entry' is used by protocolBinary.c/kraftProtocolBinaryEncode to
 * populate an on-wire format for the entire RPC.
 *
 * Also decodes user data from 'rpc->entry' back into an RPC. */

#include "entryCoder.h"
#include "endianForceLittle.h"

#include "../../deps/datakit/src/flex.h"
#include "../../deps/xxHash/xxhash.h"

/* Append 'entry' of length 'len' into 'rpc'.
 * Can be called multiple times to append multiple entries to 'rpc' */
bool entryCoderEncode(kraftRpc *rpc, kraftRpcUniqueIndex idx, kraftTerm term,
                      kraftStorageCmd originalCmd, const void *entry,
                      size_t len) {
    if (!rpc) {
        assert(NULL);
        return false;
    }

    /* Our commands must never be zero */
    assert(originalCmd > 0);

    const databox boxIndex = {.data.u = idx, .type = DATABOX_UNSIGNED_64};
    const databox boxTerm = {.data.u = term, .type = DATABOX_UNSIGNED_64};
    const databox boxCmd = {.data.u = originalCmd, .type = DATABOX_UNSIGNED_64};
    const databox boxEntry = {
        .data.bytes.ccstart = entry, .type = DATABOX_BYTES, .len = len};

    const databox *boxBoxes[] = {&boxIndex, &boxTerm, &boxCmd, &boxEntry};

    if (!rpc->entry) {
        rpc->entry = flexNew();
    }

    /* Append this metadata and entry to the aggregate RPC request */
    flexAppendMultiple(&rpc->entry, 4, boxBoxes);

    /* Update bytes inside our entry */
    rpc->len = flexBytes(rpc->entry);
    rpc->remaining++;

    return true;
}

/* Append bulk 'entry' of length 'len' into 'rpc'. */
bool entryCoderEncodeBulk(kraftRpc *rpc, kraftTerm term,
                          kraftStorageCmd originalCmd, const size_t count,
                          const size_t idx[], const void *data[],
                          const size_t len[]) {
    if (!rpc) {
        assert(NULL);
        return false;
    }

    /* Our commands must never be zero */
    assert(originalCmd > 0);

    databox *box;
    databox **boxBoxes;
    bool created = false;
    if (count <= 16) {
        databox box_[count * 4];
        databox *boxBoxes_[count * 4];

        box = box_;
        boxBoxes = boxBoxes_;
    } else {
        box = zmalloc(sizeof(*box) * count * 4);
        boxBoxes = zmalloc(sizeof(*boxBoxes) * count * 4);
        created = true;
    }

    for (size_t i = 0; i < count * 4; i += 4) {
        box[i].data.u = idx[i];
        box[i].type = DATABOX_UNSIGNED_64;

        box[i + 1].data.u = term;
        box[i + 1].type = DATABOX_UNSIGNED_64;

        box[i + 2].data.u = originalCmd;
        box[i + 2].type = DATABOX_UNSIGNED_64;

        box[i + 3].data.bytes.ccstart = data[i];
        box[i + 3].type = DATABOX_BYTES;
        box[i + 3].len = len[i];

        boxBoxes[i] = &box[i];
        boxBoxes[i + 1] = &box[i + 1];
        boxBoxes[i + 2] = &box[i + 2];
        boxBoxes[i + 3] = &box[i + 3];
    }

    if (!rpc->entry) {
        rpc->entry = flexNew();
    }

    /* Append all entries with one write without multiple realloc() required */
    flexAppendMultiple(&rpc->entry, count * 4, (const databox **const)boxBoxes);

    /* Update bytes inside our entry */
    rpc->len = flexBytes(rpc->entry);
    rpc->remaining += count;

    if (created) {
        zfree(box);
        zfree(boxBoxes);
    }

    return true;
}

/* Each call of 'entryCoderDecode' returns ONE entry.
 * Call in a loop to retrieve all entries frpm RPC.
 *
 * Return values:
 *   true - populated 'entry' and 'len'
 *   false - no more data in RPC.  Stop processing. */
bool entryCoderDecode(kraftRpc *rpc, kraftRpcUniqueIndex *idx, kraftTerm *term,
                      kraftStorageCmd *originalCmd, uint8_t **entry,
                      size_t *len) {
    if (!rpc->entry) {
        return false;
    }

    if (!rpc->remaining) {
        return false;
    }

    databox boxIdx;
    databox boxTerm;
    databox boxCmd;
    databox boxEntry;

    flexEntry *current = rpc->resume;
    if (!current) {
        /* If this is the first decode, we need to manually
         * fetch the head and the first element. */
        current = flexHead(rpc->entry);
        flexGetByType(current, &boxIdx);
    } else {
        /* else, we're resuming from a previous decode and
         * 'current' is at the previous read position, so we
         * are allowed to advance it to Next() as expected. */
        flexGetNextByType(rpc->entry, &current, &boxIdx);
    }

    flexGetNextByType(rpc->entry, &current, &boxTerm);
    flexGetNextByType(rpc->entry, &current, &boxCmd);
    flexGetNextByType(rpc->entry, &current, &boxEntry);

    rpc->remaining--;

    /* Save next position to try */
    rpc->resume = current;

    assert(boxTerm.data.u > 0);
    assert(boxIdx.data.u > 0);

    if (idx) {
        *idx = boxIdx.data.u;
    }
    if (term) {
        *term = boxTerm.data.u;
    }
    if (len) {
        *len = boxEntry.len;
    }
    if (entry) {
        *entry = boxEntry.data.bytes.start;
    }

    /* Also, all our commands are > 0 */
    assert(boxCmd.data.u > 0);
    if (originalCmd) {
        *originalCmd = boxCmd.data.u;
    }

    return true;
}

void entryCoderFree(kraftRpc *rpc) {
    flexFree(rpc->entry);
}
