#include "protoKit.h"

/* Accumulate packet data.
 * Returns AVAILABLE when an RPC is in the buffer. */
protoKitStatus protoKitProcessPacketData(protoKit *restrict pk,
                                         protoKitBuffer *restrict buf,
                                         uint8_t *restrict data, size_t len) {
    if (!buf || !data || !len) {
        return PROTOKIT_STATUS_SYSTEM_ERROR;
    }

    const ptrdiff_t usedBufferBytes = buf->used;
    ptrdiff_t availableBufferBytes = buf->allocated - usedBufferBytes;

    /* FUTURE OPTIMIZATION:
     *   If buffer is empty and 'data' contains a complete RPC,
     *   don't copy 'data' to bufer, just use 'data' directly. */

    /* If remaining buffer allocation can't fit new data, grow buffer. */
    if (availableBufferBytes < (ptrdiff_t)len) {
        /* If no current allocation, initialize to current length. */
        if (!buf->buf) {
            buf->allocated = 64;
            availableBufferBytes = buf->allocated - usedBufferBytes;
        }

        while (availableBufferBytes < (ptrdiff_t)len) {
            buf->allocated *= 2;
            availableBufferBytes = buf->allocated - usedBufferBytes;
        }

        ptrdiff_t startOffset = buf->pdu.start - buf->buf;

        void *const preBuffer = buf->buf;
        buf->buf = realloc(buf->buf, buf->allocated);
        if (!buf->buf) {
            free(preBuffer);
            assert(NULL);
            return PROTOKIT_STATUS_SYSTEM_ERROR;
        }

        buf->pdu.start = buf->buf + startOffset;
    }

    assert(availableBufferBytes >= len);

    /* Append client data to buffer */
    memcpy(buf->buf + buf->used, data, len);
    buf->used += len;

    /* Note: HasValidPDU is what sets buf->pdu.dataSize since it is and
     * encoding-level parser and knows the actual data layout. */
    if (pk->bufferHasValidPDU(buf)) {
        return PROTOKIT_STATUS_PDU_AVAILABLE;
    }

    return PROTOKIT_STATUS_NO_PDU;
}

bool protoKitBufferCleanup(protoKit *restrict pk, protoKitBuffer *restrict buf,
                           ptrdiff_t expectedSize) {
    /* FUTURE OPTIMIZATION: If buffer gets too big after processing,
     * memmove remaining data back to beginning of buffer to prevent
     * growing buffer forever if we constantly have unprocessed
     * data in buffer. */

    buf->used -= expectedSize;

    /* If next write position is the end of all data in the buffer, we've
     * used this entire buffer and can reset it back to zero. */
    if (buf->used == 0) {
        buf->pdu.start = buf->buf;
        buf->pdu.dataSize = 0;
        return false; /* no more PDUs in buffer */
    }

    /* else, we have one or more PDUs still in our buffer. */

    /* Reset data size so we populate it on next loop */
    buf->pdu.dataSize = 0;

    /* Increment next PDU Start position after processed RPC */
    buf->pdu.start += expectedSize;

    /* Test if our next PDU is valid */
    return pk->bufferHasValidPDU(buf);
}
