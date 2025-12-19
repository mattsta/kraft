#ifndef ENDIAN_FORCE_LITTLE_H
#define ENDIAN_FORCE_LITTLE_H

#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#if 0
/* Previously */
static const int one = 1;
#define _isLittleEndian() (*(char *)(&one))
#endif

/* Determines endianness at compile time */
/* We don't recognize mixed as existing. */
static bool _isLittleEndian(void) {
    /* Populate uint32_t with 1, then access
     * the first byte of four byte quantity, which
     * in little endian context, is 1. */
    /* Compiler should know this at compile time
     * to select proper branches everywhere
     * we end up using _isLittleEndian() */
    const union {
        uint32_t i;
        uint8_t c[4];
    } one = {1};
    return one.c[0];
}

#ifndef GCC_VERSION
#define GCC_VERSION (__GNUC__ * 100 + __GNUC_MINOR__)
#endif

#if defined(_MSC_VER) /* Visual Studio */
#define _endianForce32 _byteswap_ulong
#define _endianForce64 _byteswap_uint64
#elif GCC_VERSION >= 403
#define _endianForce32 __builtin_bswap32
#define _endianForce64 __builtin_bswap64
#else  /* else, if compiler doesn't give us swap intrinsics... */
static uint32_t _endianForce32(uint32_t x) {
    return ((x << 24) & 0xff000000) | ((x << 8) & 0x00ff0000) |
           ((x >> 8) & 0x0000ff00) | ((x >> 24) & 0x000000ff);
}
static uint64_t _endianForce64(uint64_t x) {
    return ((x << 56) & 0xff00000000000000ULL) |
           ((x << 40) & 0x00ff000000000000ULL) |
           ((x << 24) & 0x0000ff0000000000ULL) |
           ((x << 8) & 0x000000ff00000000ULL) |
           ((x >> 8) & 0x00000000ff000000ULL) |
           ((x >> 24) & 0x0000000000ff0000ULL) |
           ((x >> 40) & 0x000000000000ff00ULL) |
           ((x >> 56) & 0x00000000000000ffULL);
}
#endif /* end alternative swap functions */

/* The branches below are decided at compile time, so
 * no actual decisions should be required at runtime. */
static inline uint32_t endianForce32(uint32_t x) {
    if (_isLittleEndian()) {
        return x;
    } else {
        return _endianForce32(x);
    }
}

static inline uint64_t endianForce64(uint64_t x) {
    if (_isLittleEndian()) {
        return x;
    } else {
        return _endianForce64(x);
    }
}

/* We assume we're behaving and only passing 4 or 8 byte quantities here. */
/* All these branches should get evaluated at compile time. */
#define forceEncoding(x) (sizeof(x) == 8 ? endianForce64(x) : endianForce32(x))
#define forceEncodingInPlace(x)                                                \
    do {                                                                       \
        if (sizeof(x) == 8 || sizeof(x) == 4) {                                \
            x = forceEncoding(x);                                              \
        } else {                                                               \
            assert(NULL && "Invalid sizeof() for forceEncodingInPlace()!");    \
        }                                                                      \
    } while (0)

#endif /* ENDIAN_FORCE_LITTLE_H */
