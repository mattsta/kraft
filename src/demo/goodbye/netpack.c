#include "netpack.h"

/* About:
 *   - pack [IP][PORT] into a buffer.
 *   - format is (all as ASCII strings):
 *     IP NULL PORT NULL IP NULL PORT NULL ... IP NULL PORT NULL NULL
 */

/* TODO:
 *   - Add normalization for IPv6 addresses. */

/* Returns number of bytes added to 'buf' */
/* Our packed IP Port format looks like:
 * IP NULL PORT NULL IP NULL PORT NULL IP NULL ... PORT NULL NULL */
uint32_t netpackEncode(const uint8_t *ip, const uint16_t port, void *inBuf,
                       const size_t bufLen) {
    /* The +2 is for NULLs after ADDR and PORT */
    const uint8_t maxWrite =
        (NETPACK_INET6_ADDRSTRLEN + NETPACK_PORTSTRLEN + 2);

    uint8_t *buf = inBuf;

    if (bufLen < maxWrite) {
        assert(NULL && "Packing IP Port buffer too small!");
        return 0;
    }

    size_t ipStrLen = strlen((const char *)ip);
    if (ipStrLen > NETPACK_INET6_ADDRSTRLEN) {
        assert(NULL && "Invalid IP girven to Pack IP Port!");
        return 0;
    }

    char *bufWrite = (char *)buf;
    strcpy(bufWrite, (const char *)ip);
    buf += (ipStrLen + 1); /* advance buf past inserted IP + NULL */

    int portStrLen = snprintf(bufWrite, bufLen, "%d", port);
    buf += (portStrLen + 1); /* port + NULL */

    ptrdiff_t written = (uintptr_t)bufWrite - (uintptr_t)buf;
    assert(written <= maxWrite);

    return written;
}

/* Finalize our IPPort string by appending a second NULL.
 * So, our packed IPPort format ends with NULL NULL */
uint32_t netpackFinalize(uint8_t *buf, const uint32_t bufLen) {
    if (bufLen < 1) {
        return 0;
    } else {
        *buf = '\0';
        return 1;
    }
}

uint32_t netpackDecode(uint8_t **ip, uint16_t *port, const void *inBuf,
                       const size_t bufLen) {
    const uint8_t *buf = inBuf;

    if (!buf || bufLen == 0) {
        assert(NULL && "No buffer!");
        return 0;
    }

    if (bufLen == 1 && *buf == '\0') {
        /* We are at end of buffer. */
        return 0;
    }

    char *bufRead = (char *)buf;
    /* IP is first entry in buffer */
    if (ip) {
        *ip = (uint8_t *)bufRead;
    }
    bufRead += strlen(bufRead) + 1;

    /* Port is second entry in buffer */
    if (port) {
        *port = atoi(bufRead);
    }
    bufRead += strlen(bufRead) + 1;

    ptrdiff_t readLen = (uintptr_t)bufRead - (uintptr_t)buf;
    return readLen;
}
