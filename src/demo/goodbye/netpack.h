#ifndef NETPACK_H
#define NETPACK_H

#include <assert.h> /* assert */
#include <stddef.h> /* ptrdiff_t, etc */
#include <stdint.h>
#include <stdio.h>  /* snprintf */
#include <stdlib.h> /* NULL */
#include <string.h> /* memcpy */

#define NETPACK_INET6_ADDRSTRLEN 48
#define NETPACK_PORTSTRLEN 5

uint32_t netpackEncode(const uint8_t *ip, const uint16_t port, void *buf,
                       const size_t bufLen);
uint32_t netpackFinalize(uint8_t *buf, const uint32_t bufLen);
uint32_t netpackDecode(uint8_t **ip, uint16_t *port, const void *buf,
                       const size_t bufLen);

#endif /* NETPACK_H */
