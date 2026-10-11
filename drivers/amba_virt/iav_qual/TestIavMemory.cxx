/*
 * TestIavMemory.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 *
 * Partition queries and mappings. The partition address is the mmap offset, as applications
 * use it. Mappings are read-only: the suite never writes to memory the running pipeline uses.
 */

#include "IavTestSupport.hxx"

#include <vector>

#include <CppUTest/TestHarness.h>

using namespace iavtest;

namespace {

const unsigned long PageBytes = 4096UL;

struct Range
{
    unsigned int pid;
    unsigned long begin;
    unsigned long end;
};

/* Every partition the driver reports. */
void collectPartitions(int fd, std::vector<Range> &out)
{
    for (unsigned int pid = 0; pid < (unsigned int)IAV_PART_LAST; pid++) {
        unsigned long addr = 0, length = 0;

        if (queryPartition(fd, pid, &addr, &length) == 0 && length > 0) {
            Range r;

            r.pid = pid;
            r.begin = addr;
            r.end = addr + length;
            out.push_back(r);
        }
    }
}

} // namespace

TEST_GROUP(IavMemory)
{
    ScopedFd _f;

    void setup() { CHECK_TRUE(_f.open()); }
};

TEST(IavMemory, QueryMemblockDsp)
{
    unsigned long addr = 0, length = 0;

    LONGS_EQUAL(0, queryPartition(_f.fd(), IAV_PART_DSP, &addr, &length));
    CHECK_TRUE(addr != 0);
    CHECK_TRUE(length > 0);
    note("length", (long long)length);
    note("addr_page_aligned", (addr % PageBytes) == 0);
}

TEST(IavMemory, QueryMemblockBsb)
{
    unsigned long addr = 0, length = 0;

    LONGS_EQUAL(0, queryPartition(_f.fd(), IAV_PART_BSB, &addr, &length));
    CHECK_TRUE(addr != 0);
    CHECK_TRUE(length > 0);
    note("length", (long long)length);
    note("addr_page_aligned", (addr % PageBytes) == 0);
}

TEST(IavMemory, PartitionsPageAligned)
{
    std::vector<Range> parts;

    collectPartitions(_f.fd(), parts);
    CHECK_TRUE(parts.size() >= 2);
    for (size_t i = 0; i < parts.size(); i++) {
        CHECK_TEXT((parts[i].begin % PageBytes) == 0, "partition address is not page aligned");
        CHECK_TEXT((parts[i].end % PageBytes) == 0, "partition end is not page aligned");
    }
    note("partitions", (long long)parts.size());
}

/*
 * The driver reports nested partitions (a partition inside a larger one, observed on the
 * devkit), so partitions may nest. What must never happen is a partial overlap, where two ranges
 * share memory and neither contains the other. The nesting count is recorded.
 */
TEST(IavMemory, PartitionsDoNotOverlap)
{
    std::vector<Range> parts;
    long long nested = 0;

    collectPartitions(_f.fd(), parts);
    CHECK_TRUE(parts.size() >= 2);
    for (size_t i = 0; i < parts.size(); i++) {
        for (size_t j = i + 1; j < parts.size(); j++) {
            const Range &a = parts[i];
            const Range &b = parts[j];
            bool disjoint = a.end <= b.begin || b.end <= a.begin;
            bool aInB = b.begin <= a.begin && a.end <= b.end;
            bool bInA = a.begin <= b.begin && b.end <= a.end;

            if (!disjoint) {
                CHECK_TEXT(aInB || bInA, "two partitions partially overlap");
                nested++;
            }
        }
    }
    note("partitions", (long long)parts.size());
    note("nested_pairs", nested);
}

TEST(IavMemory, MapDspReadOnlyTouchAllPages)
{
    unsigned long addr = 0, length = 0;
    ScopedMap map;
    unsigned long sum = 0;

    LONGS_EQUAL(0, queryPartition(_f.fd(), IAV_PART_DSP, &addr, &length));
    CHECK_TRUE(map.map(_f.fd(), addr, length, PROT_READ));
    for (unsigned long off = 0; off < length; off += PageBytes) {
        sum += map.bytes()[off];
    }
    note("pages", (long long)(length / PageBytes));
    note("touched_pages", (long long)(length / PageBytes));
    (void)sum;
}

TEST(IavMemory, MapBsbDoubleMapping)
{
    unsigned long addr = 0, length = 0;
    ScopedMap map;
    int aliased = 0;

    LONGS_EQUAL(0, queryPartition(_f.fd(), IAV_PART_BSB, &addr, &length));
    CHECK_TRUE(map.map(_f.fd(), addr, 2 * length, PROT_READ));
    /*
     * The second half is the same memory as the first, so a word and its twin read equal
     * unless the encoder wrote between the two reads. Retry until one pair is stable.
     */
    for (int attempt = 0; attempt < 200 && !aliased; attempt++) {
        unsigned long off = ((unsigned long)attempt * 4099UL) % (length - 8);
        unsigned char a = map.bytes()[off];
        unsigned char b = map.bytes()[length + off];
        unsigned char a2 = map.bytes()[off];

        if (a == a2 && b == a2) {
            aliased = 1;
        }
    }
    CHECK_TEXT(aliased, "the second half of the double mapping does not match the first");
    note("length", (long long)length);
}

TEST(IavMemory, MapUnmapRepeated)
{
    unsigned long addr = 0, length = 0;

    LONGS_EQUAL(0, queryPartition(_f.fd(), IAV_PART_BSB, &addr, &length));
    for (int i = 0; i < 20; i++) {
        ScopedMap map;

        CHECK_TRUE(map.map(_f.fd(), addr, length, PROT_READ));
        volatile unsigned char first = map.bytes()[0];

        (void)first;
    }
    note("cycles", 20);
}

TEST(IavMemory, MapSizeMatchesQuery)
{
    unsigned long addr = 0, length = 0;
    ScopedMap exact;
    ScopedMap tooBig;

    LONGS_EQUAL(0, queryPartition(_f.fd(), IAV_PART_BSB, &addr, &length));
    CHECK_TRUE(exact.map(_f.fd(), addr, length, PROT_READ));
    CHECK_FALSE(tooBig.map(_f.fd(), addr, 2 * length + PageBytes, PROT_READ));
    LONGS_EQUAL(EINVAL, errno);
    ctx().setResult(-1, errno);
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
