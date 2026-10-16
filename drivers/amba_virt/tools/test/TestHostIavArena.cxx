/*
 * TestHostIavArena.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 *
 * The guest module's arena allocator (guest-os/linux/amba-iav/iav_arena.c), on the build host: first fit with 64 KiB
 * alignment over the guest's slice, bare-metal anonymous ids, and the bare-metal user rules (FREE refuses a mapped
 * block, UNREGISTER refuses a descriptor that maps it, a released descriptor frees the blocks nobody holds). Zero and
 * maximum lengths, overflow, double free, stale and foreign ids, teardown with mappings and concurrent
 * allocation and release are covered; every test checks that no two live blocks overlap and that every block lies
 * inside the arena.
 */

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <set>
#include <thread>
#include <vector>

#include "CppUTest/TestHarness.h"

extern "C" {
#include "iav_arena.h"
}

namespace {

const uint64_t Usable = 0x4000000ull - 0x1000ull;	/* a 64 MiB slice less its claim page */
const uint64_t Page = 4096;

void *zalloc(size_t n)
{
    return calloc(1, n);
}

/* No overlap, every block aligned and inside the arena, the used total equal to the spans. */
void checkInvariants(struct iav_arena *a)
{
    uint64_t used = 0, end = 0;
    uint32_t n = 0;
    std::set<uint32_t> ids;

    for (struct iav_arena_blk *b = a->head; b; b = b->next) {
        CHECK_EQUAL(0u, b->off % IAV_ARENA_ALIGN);
        CHECK(b->off >= end);
        CHECK(b->len > 0 && b->len <= b->span && b->span % IAV_ARENA_ALIGN == 0);
        CHECK(b->off + b->span <= a->usable);
        CHECK(iav_arena_is_anon(b->vid));
        CHECK(ids.insert(b->vid).second);
        end = b->off + b->span;
        used += b->span;
        n++;
    }
    UNSIGNED_LONGS_EQUAL(used, a->used);
    UNSIGNED_LONGS_EQUAL(n, a->nblk);
}

/* The same invariants without asserting, for threads (a failed check must not throw across a thread). */
bool invariantsHold(struct iav_arena *a)
{
    uint64_t used = 0, end = 0;
    uint32_t n = 0;
    std::set<uint32_t> ids;

    for (struct iav_arena_blk *b = a->head; b; b = b->next) {
        if (b->off % IAV_ARENA_ALIGN || b->off < end || b->len == 0 || b->len > b->span ||
            b->span % IAV_ARENA_ALIGN || b->off + b->span > a->usable || !iav_arena_is_anon(b->vid) ||
            !ids.insert(b->vid).second) {
            return false;
        }
        end = b->off + b->span;
        used += b->span;
        n++;
    }
    return used == a->used && n == a->nblk;
}

int fileA, fileB;

} // namespace

TEST_GROUP(IavArena)
{
    struct iav_arena a;

    void setup() override
    {
        LONGS_EQUAL(0, iav_arena_init(&a, Usable, Page, zalloc, free));
    }

    void teardown() override
    {
        checkInvariants(&a);
        iav_arena_destroy(&a);
    }
};

TEST(IavArena, InitRefusesAnUnusableGeometry)
{
    struct iav_arena b;

    LONGS_EQUAL(-EINVAL, iav_arena_init(&b, 0, Page, zalloc, free));
    LONGS_EQUAL(-EINVAL, iav_arena_init(&b, Usable, 0, zalloc, free));
    LONGS_EQUAL(-EINVAL, iav_arena_init(&b, Usable, 3000, zalloc, free));
    LONGS_EQUAL(-EINVAL, iav_arena_init(&b, Usable, 0x20000, zalloc, free));
    LONGS_EQUAL(-EINVAL, iav_arena_init(&b, Usable, Page, NULL, free));
}

TEST(IavArena, ABlockHasABareMetalIdAPageRoundedLengthAndAnAlignedOffset)
{
    uint32_t vid = 0;
    uint64_t off = 1, len = 0;

    LONGS_EQUAL(0, iav_arena_alloc(&a, &fileA, 1, &vid, &off, &len));
    UNSIGNED_LONGS_EQUAL((1u << 8) | 0xffu, vid);
    UNSIGNED_LONGS_EQUAL(0, off);
    UNSIGNED_LONGS_EQUAL(Page, len);
    LONGS_EQUAL(0, iav_arena_alloc(&a, &fileA, 0x10001, &vid, &off, &len));
    UNSIGNED_LONGS_EQUAL((2u << 8) | 0xffu, vid);
    UNSIGNED_LONGS_EQUAL(0x10000, off);
    UNSIGNED_LONGS_EQUAL(0x11000, len);
    LONGS_EQUAL(0, iav_arena_alloc(&a, &fileA, 4096, &vid, &off, &len));
    UNSIGNED_LONGS_EQUAL(0x30000, off);
}

TEST(IavArena, AZeroLengthIsInvalidAndTooMuchIsTheDriversFault)
{
    uint32_t vid = 0;
    uint64_t off = 0, len = 0;

    LONGS_EQUAL(-EINVAL, iav_arena_alloc(&a, &fileA, 0, &vid, &off, &len));
    /* the largest block: the arena rounded down to 64 KiB */
    LONGS_EQUAL(-EFAULT, iav_arena_alloc(&a, &fileA, 0x3ff0001, &vid, &off, &len));
    LONGS_EQUAL(0, iav_arena_alloc(&a, &fileA, 0x3ff0000, &vid, &off, &len));
    UNSIGNED_LONGS_EQUAL(0x3ff0000, len);
    LONGS_EQUAL(-EFAULT, iav_arena_alloc(&a, &fileA, 1, &vid, &off, &len));
    LONGS_EQUAL(0, iav_arena_free(&a, vid));
    /* lengths that overflow when rounded */
    LONGS_EQUAL(-EFAULT, iav_arena_alloc(&a, &fileA, UINT64_MAX, &vid, &off, &len));
    LONGS_EQUAL(-EFAULT, iav_arena_alloc(&a, &fileA, UINT64_MAX - 0xfff, &vid, &off, &len));
    LONGS_EQUAL(-EFAULT, iav_arena_alloc(&a, &fileA, 1ull << 63, &vid, &off, &len));
    UNSIGNED_LONGS_EQUAL(0, a.nblk);
}

TEST(IavArena, FirstFitReusesTheLowestHole)
{
    uint32_t v[4];
    uint64_t off = 0, len = 0;

    for (int i = 0; i < 4; i++) {
        LONGS_EQUAL(0, iav_arena_alloc(&a, &fileA, 0x20000, &v[i], &off, &len));
        UNSIGNED_LONGS_EQUAL((uint64_t)i * 0x20000, off);
    }
    LONGS_EQUAL(0, iav_arena_free(&a, v[1]));
    LONGS_EQUAL(0, iav_arena_free(&a, v[2]));
    /* a smaller block takes the start of the hole, a larger one too, the next one past the hole */
    LONGS_EQUAL(0, iav_arena_alloc(&a, &fileA, 0x10000, &v[1], &off, &len));
    UNSIGNED_LONGS_EQUAL(0x20000, off);
    LONGS_EQUAL(0, iav_arena_alloc(&a, &fileA, 0x30000, &v[2], &off, &len));
    UNSIGNED_LONGS_EQUAL(0x30000, off);
    LONGS_EQUAL(0, iav_arena_alloc(&a, &fileA, 0x10000, &v[2], &off, &len));
    UNSIGNED_LONGS_EQUAL(0x80000, off);
}

TEST(IavArena, AFreedIdAnotherGuestsIdAndANamedPartitionAreUnknown)
{
    uint32_t vid = 0;
    uint64_t off = 0, len = 0;

    LONGS_EQUAL(0, iav_arena_alloc(&a, &fileA, 0x1000, &vid, &off, &len));
    LONGS_EQUAL(0, iav_arena_free(&a, vid));
    /* double free, and the freed id is not handed out again at once */
    LONGS_EQUAL(-EINVAL, iav_arena_free(&a, vid));
    CHECK(iav_arena_find(&a, vid) == NULL);
    LONGS_EQUAL(0, iav_arena_alloc(&a, &fileA, 0x1000, &vid, &off, &len));
    UNSIGNED_LONGS_EQUAL((2u << 8) | 0xffu, vid);
    /* an id this arena never issued (another guest's), a named partition, the bare tag */
    LONGS_EQUAL(-EINVAL, iav_arena_free(&a, (77u << 8) | 0xffu));
    LONGS_EQUAL(-EINVAL, iav_arena_free(&a, 39));
    LONGS_EQUAL(-EINVAL, iav_arena_free(&a, 0xff));
    LONGS_EQUAL(-EINVAL, iav_arena_register(&a, 193, &fileB));
    LONGS_EQUAL(-EINVAL, iav_arena_unregister(&a, (1u << 8) | 0xffu, &fileA));
    CHECK(iav_arena_find_at(&a, 0x1000) == NULL);
    CHECK(iav_arena_find_at(&a, 0) != NULL);
}

TEST(IavArena, AMappedBlockCannotBeFreedAndItsMapperCannotUnregister)
{
    uint32_t vid = 0;
    uint64_t off = 0, len = 0;
    struct iav_arena_blk *b;

    LONGS_EQUAL(0, iav_arena_alloc(&a, &fileA, 0x1000, &vid, &off, &len));
    b = iav_arena_find(&a, vid);
    LONGS_EQUAL(0, iav_arena_map(&a, b, &fileB));
    LONGS_EQUAL(-EFAULT, iav_arena_free(&a, vid));
    LONGS_EQUAL(-EPERM, iav_arena_unregister(&a, vid, &fileB));
    LONGS_EQUAL(0, iav_arena_unregister(&a, vid, &fileA));
    iav_arena_unmap(&a, b, &fileB);
    LONGS_EQUAL(0, iav_arena_unregister(&a, vid, &fileB));
    /* nobody holds it, but it stays until a descriptor is released (the bare-metal rule) */
    CHECK(iav_arena_find(&a, vid) != NULL);
    iav_arena_release_file(&a, &fileB);
    CHECK(iav_arena_find(&a, vid) == NULL);
}

TEST(IavArena, AReleasedDescriptorFreesOnlyWhatNobodyElseHolds)
{
    uint32_t v1 = 0, v2 = 0, v3 = 0;
    uint64_t off = 0, len = 0;

    LONGS_EQUAL(0, iav_arena_alloc(&a, &fileA, 0x1000, &v1, &off, &len));
    LONGS_EQUAL(0, iav_arena_alloc(&a, &fileA, 0x1000, &v2, &off, &len));
    LONGS_EQUAL(0, iav_arena_alloc(&a, &fileB, 0x1000, &v3, &off, &len));
    LONGS_EQUAL(0, iav_arena_register(&a, v2, &fileB));
    LONGS_EQUAL(0, iav_arena_register(&a, v2, &fileB));	/* twice is one user */
    iav_arena_release_file(&a, &fileA);
    CHECK(iav_arena_find(&a, v1) == NULL);
    CHECK(iav_arena_find(&a, v2) != NULL);
    CHECK(iav_arena_find(&a, v3) != NULL);
    iav_arena_release_file(&a, &fileB);
    UNSIGNED_LONGS_EQUAL(0, a.nblk);
    UNSIGNED_LONGS_EQUAL(0, a.used);
}

TEST(IavArena, TeardownDuringUseKeepsAMappedBlockUntilItIsUnmapped)
{
    uint32_t vid = 0;
    uint64_t off = 0, len = 0;
    struct iav_arena_blk *b;

    LONGS_EQUAL(0, iav_arena_alloc(&a, &fileA, 0x1000, &vid, &off, &len));
    b = iav_arena_find(&a, vid);
    /* a mapping and a copy of it (fork): the block outlives a release of another descriptor */
    LONGS_EQUAL(0, iav_arena_map(&a, b, &fileA));
    LONGS_EQUAL(0, iav_arena_map(&a, b, &fileA));
    iav_arena_release_file(&a, &fileB);
    CHECK(iav_arena_find(&a, vid) != NULL);
    iav_arena_unmap(&a, b, &fileA);
    LONGS_EQUAL(-EFAULT, iav_arena_free(&a, vid));
    iav_arena_unmap(&a, b, &fileA);
    /* a duplicate unmap does not underflow */
    iav_arena_unmap(&a, b, &fileA);
    UNSIGNED_LONGS_EQUAL(0, b->maps);
    LONGS_EQUAL(0, iav_arena_free(&a, vid));
}

TEST(IavArena, TheUserTableIsBounded)
{
    uint32_t vid = 0;
    uint64_t off = 0, len = 0;
    static int files[IAV_ARENA_MAX_USERS + 1];

    LONGS_EQUAL(0, iav_arena_alloc(&a, &files[0], 0x1000, &vid, &off, &len));
    for (unsigned int i = 1; i < IAV_ARENA_MAX_USERS; i++) {
        LONGS_EQUAL(0, iav_arena_register(&a, vid, &files[i]));
    }
    LONGS_EQUAL(-EINVAL, iav_arena_register(&a, vid, &files[IAV_ARENA_MAX_USERS]));
    LONGS_EQUAL(-EINVAL, iav_arena_map(&a, iav_arena_find(&a, vid), &files[IAV_ARENA_MAX_USERS]));
}

TEST(IavArena, TheArenaFillsWithSmallBlocksAndEmptiesAgain)
{
    std::vector<uint32_t> ids;
    uint32_t vid = 0;
    uint64_t off = 0, len = 0;

    while (ids.size() <= Usable / IAV_ARENA_ALIGN + 8 && iav_arena_alloc(&a, &fileA, 1, &vid, &off, &len) == 0) {
        ids.push_back(vid);
    }
    /* 64 KiB granules in the arena (the last, partial one cannot hold a block) */
    UNSIGNED_LONGS_EQUAL(Usable / IAV_ARENA_ALIGN, ids.size());
    checkInvariants(&a);
    for (size_t i = 0; i < ids.size(); i += 2) {
        LONGS_EQUAL(0, iav_arena_free(&a, ids[i]));
    }
    checkInvariants(&a);
    iav_arena_release_file(&a, &fileA);
    UNSIGNED_LONGS_EQUAL(0, a.nblk);
}

TEST(IavArena, ConcurrentAllocationAndReleaseKeepTheInvariants)
{
    pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
    std::vector<std::thread> t;
    static int files[8];
    std::atomic<int> broken(0), freeFailed(0);

    for (int i = 0; i < 8; i++) {
        t.push_back(std::thread([&, i]() {
            unsigned int seed = 1234u + (unsigned int)i;
            std::vector<uint32_t> mine;

            for (int k = 0; k < 3000; k++) {
                uint32_t vid = 0;
                uint64_t off = 0, len = 0;
                int r = rand_r(&seed) % 4;

                pthread_mutex_lock(&lock);
                if (r < 2 || mine.empty()) {
                    if (iav_arena_alloc(&a, &files[i], 1 + (uint64_t)(rand_r(&seed) % 0x80000), &vid, &off, &len) == 0) {
                        mine.push_back(vid);
                    }
                } else if (r == 2) {
                    size_t j = (size_t)rand_r(&seed) % mine.size();

                    if (iav_arena_free(&a, mine[j]) != 0) {
                        freeFailed++;
                    }
                    mine.erase(mine.begin() + (long)j);
                } else {
                    iav_arena_release_file(&a, &files[i]);
                    mine.clear();
                }
                if (k % 100 == 0 && !invariantsHold(&a)) {
                    broken++;
                }
                pthread_mutex_unlock(&lock);
            }
        }));
    }
    for (auto &x : t) {
        x.join();
    }
    LONGS_EQUAL(0, broken.load());
    LONGS_EQUAL(0, freeFailed.load());
    checkInvariants(&a);
}

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
