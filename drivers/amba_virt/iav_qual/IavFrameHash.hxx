/*
 * IavFrameHash.hxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 *
 * The hash of a frame's Y plane that the guest suite (TestIavWitness.cxx) and the Dom0 witness
 * (IavQualServer.cxx) both compute, so the two can be compared. The plane is copied out of the
 * mapping in blocks and hashed eight bytes at a time, which is fast enough to keep up with the
 * frame rate on both sides.
 */

#ifndef IAVQUAL_IAVFRAMEHASH_HXX
#define IAVQUAL_IAVFRAMEHASH_HXX

#include <stddef.h>
#include <stdint.h>
#include <string.h>

inline unsigned long long iavFrameHash(const volatile unsigned char *p, size_t n)
{
    unsigned char buf[65536];
    unsigned long long h = 1469598103934665603ULL;
    size_t off = 0;

    while (off < n) {
        size_t c = n - off < sizeof(buf) ? n - off : sizeof(buf);
        size_t i = 0;

        memcpy(buf, (const void *)(p + off), c);
        for (; i + 8 <= c; i += 8) {
            uint64_t w;

            memcpy(&w, buf + i, sizeof(w));
            h ^= w;
            h *= 1099511628211ULL;
            h ^= h >> 29;
        }
        for (; i < c; i++) {
            h ^= buf[i];
            h *= 1099511628211ULL;
        }
        off += c;
    }
    return h;
}

#endif /* IAVQUAL_IAVFRAMEHASH_HXX */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
