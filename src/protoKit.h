#ifndef PROTOKIT_H
#define PROTOKIT_H

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef enum protoKitStatus {
    PROTOKIT_STATUS_PDU_AVAILABLE = 1,
    PROTOKIT_STATUS_PDU_POPULATED,
    PROTOKIT_STATUS_NO_PDU,
    PROTOKIT_STATUS_PARSE_ERROR,
    PROTOKIT_STATUS_SYSTEM_ERROR,
} protoKitStatus;
// extern char *protoKitStatuses[];
static char *protoKitStatuses[] = {"_UNDEFINED_", "PDU AVAILABLE",
                                   "POPULATED",   "NO PDU IN BUFFER",
                                   "PARSE ERROR", "SYSTEM ERROR"};

typedef struct protoKitBuffer {
    size_t used;      /* Total bytes used by 'buf' so far */
    size_t allocated; /* Total allocation size of 'buf' */
    uint8_t *buf;     /* buf */
    struct {
        size_t dataSize; /* Expected length of data.  When 'total' == 'dataSize'
                            we have a complete PDU. */
        uint8_t *start;  /* next full PDU in 'buf' — equal to 'buf' unless
                            there are multiple unprocessed PDUs in one stream */
    } pdu;
} protoKitBuffer;

/* Current length is:
 *  TOTAL BYTES IN BUFFER
 *      MINUS
 *  BYTES FROM [START, CURRENT]
 *      LEAVING THE CURRENT LENGTH AS
 *  [CURRENT, up to TOTAL BYTES] */
#define PK_CURRENT_LENGTH(buf) ((buf)->used - ((buf)->pdu.start - (buf)->buf))

typedef struct protoKit {
    bool (*bufferHasValidPDU)(protoKitBuffer *buf);
    void *zoo;

} protoKit;

protoKitStatus protoKitProcessPacketData(protoKit *pk, protoKitBuffer *buf,
                                         uint8_t *data, size_t len);
bool protoKitBufferCleanup(protoKit *pk, protoKitBuffer *buf,
                           ptrdiff_t expectedSize);

#endif /* PROTOKIT_H */
