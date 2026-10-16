/*
 * TestHostIavAperture.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 *
 * The server side of the aux aperture and the new partitions (plan_iav_usb_yolo, Envelope F): the composition of an
 * aperture from the partitions (fixed order, 64 KiB offsets, the header page, sizes an old module would mistake for a
 * window avoided), the node lifecycle against a fake window module that enforces the module's rules (sources, no
 * reconfiguration while mapped), adoption after a server restart, the aux=<size> of the grant for the primary only, and
 * the allocation of BLUR, ARB_BLUR and YUV_REMAP against a fake IAV driver (reuse, mismatch left alone, the qualification
 * free).
 */

#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <map>
#include <string>
#include <vector>

#include "CppUTest/TestHarness.h"

#include <iav_ioctl.h>

extern "C" {
#include "amba_virt_iav.h"
#include "iav_new_parts.h"
#include "virt_camera_policy.h"
#include "virt_iav_aperture.h"
#include "virt_slice_lease.h"
}

namespace {

/* The partitions as observed on n1-655-pro (b_board_reads), and the new ones where an allocator might put them. */
const struct virt_iav_aperture_part Parts[] = {
    { IAV_PART_DSP, 0x40000000ull, 0x40000000ull },	/* not an aperture partition: ignored */
    { IAV_PART_YUV_REMAP, 0xb4a00000ull, 0x10000ull },
    { IAV_PART_MASK, 0x87900000ull, 0x2000000ull },
    { IAV_PART_WARP, 0x86100000ull, 0x360000ull },
    { IAV_PART_QPMATRIX, 0x84d00000ull, 0x400000ull },
    { IAV_PART_CE_GO_MAP, 0x84259000ull, 0x1000ull },
    { IAV_PART_CANVAS_OVERLAY, 0x28900000ull, 0x200000ull },
    { IAV_PART_ARB_BLUR, 0xb4800000ull, 0x100000ull },
};
const unsigned int NParts = sizeof(Parts) / sizeof(Parts[0]);
const uint64_t Avoid[] = { 0x40000000ull, 0x800000ull };	/* the unpadded DSP and overlay windows */

/* A window module in memory: the rules of amba_virt_iav_win.c as far as the server can observe them. */
struct FakeNodes
{
    struct amba_virt_iav_aux_cfg cfg[AMBA_VIRT_IAV_AUX_NODES];
    unsigned int maps[AMBA_VIRT_IAV_AUX_NODES];
    struct amba_virt_iav_aux_sources sources;
    bool present;
    unsigned int configs, clears;

    FakeNodes() { reset(); }

    void reset()
    {
        memset(cfg, 0, sizeof(cfg));
        memset(maps, 0, sizeof(maps));
        memset(&sources, 0, sizeof(sources));
        present = true;
        configs = clears = 0;
    }

    bool isSource(const struct amba_virt_iav_aux_range &r) const
    {
        for (unsigned int i = 0; i < sources.count; i++) {
            if (sources.src[i].phys == r.phys && sources.src[i].len == r.len && sources.src[i].pid == r.pid) {
                return true;
            }
        }
        return false;
    }

    int ioctl(uint32_t node, unsigned long cmd, void *arg)
    {
        if (!present) {
            return -ENOENT;
        }
        if (node >= AMBA_VIRT_IAV_AUX_NODES) {
            return -ENODEV;
        }
        switch (cmd) {
        case AMBA_VIRT_IAV_WIN_IOC_AUX_GET: {
            struct amba_virt_iav_aux_cfg *c = static_cast<struct amba_virt_iav_aux_cfg *>(arg);

            *c = cfg[node];
            c->maps = maps[node];
            return 0;
        }
        case AMBA_VIRT_IAV_WIN_IOC_AUX_CONFIG: {
            const struct amba_virt_iav_aux_cfg *c = static_cast<const struct amba_virt_iav_aux_cfg *>(arg);

            if (maps[node]) {
                return -EBUSY;
            }
            for (unsigned int i = 0; i < c->count; i++) {
                if (!isSource(c->range[i])) {
                    return -EPERM;
                }
            }
            cfg[node] = *c;
            configs++;
            return 0;
        }
        case AMBA_VIRT_IAV_WIN_IOC_AUX_SOURCES:
            sources = *static_cast<const struct amba_virt_iav_aux_sources *>(arg);
            return 0;
        case AMBA_VIRT_IAV_WIN_IOC_CLEAR:
            if (maps[node]) {
                return -EBUSY;
            }
            memset(&cfg[node], 0, sizeof(cfg[node]));
            clears++;
            return 0;
        default:
            return -ENOTTY;
        }
    }
};

FakeNodes g_nodes;

int nodeIoctl(void *ctx, uint32_t node, unsigned long cmd, void *arg)
{
    (void)ctx;
    return g_nodes.ioctl(node, cmd, arg);
}

void useFakeNodes()
{
    struct virt_iav_aperture_ops ops = { NULL, nodeIoctl };

    virt_iav_aperture_reset();
    g_nodes.reset();
    virt_iav_aperture_set_ops(&ops);
}

} // namespace

TEST_GROUP(IavApertureCompose)
{
};

TEST(IavApertureCompose, ThePrimarySetIsPlacedInItsOrderAt64KiBBoundariesWithTheHeaderPageLast)
{
    struct amba_virt_iav_aux_cfg c;
    const uint32_t order[] = { IAV_PART_MASK, IAV_PART_WARP, IAV_PART_QPMATRIX, IAV_PART_CE_GO_MAP,
                               IAV_PART_CANVAS_OVERLAY, IAV_PART_ARB_BLUR, IAV_PART_YUV_REMAP };
    const uint64_t offsets[] = { 0x0, 0x2000000, 0x2360000, 0x2760000, 0x2770000, 0x2970000, 0x2a70000 };

    LONGS_EQUAL(7, virt_iav_aperture_compose(Parts, NParts, Avoid, 2, 9, &c));
    UNSIGNED_LONGS_EQUAL(0x4000000ull, c.size);
    UNSIGNED_LONGS_EQUAL(9, c.epoch);
    LONGS_EQUAL(7, c.count);
    for (unsigned int i = 0; i < 7; i++) {
        UNSIGNED_LONGS_EQUAL(order[i], c.range[i].pid);
        UNSIGNED_LONGS_EQUAL(offsets[i], c.range[i].offset);
        LONGS_EQUAL(0, c.range[i].offset % AMBA_VIRT_IAV_AUX_ALIGN);
        CHECK(c.range[i].offset + c.range[i].len <= c.size - 4096);
        LONGS_EQUAL(0, c.range[i].flags);
    }
    for (unsigned int i = 7; i < AMBA_VIRT_IAV_AUX_MAX_RANGES; i++) {
        UNSIGNED_LONGS_EQUAL(0, c.range[i].len);
    }
}

TEST(IavApertureCompose, MissingEmptyOrUnalignedPartitionsAreLeftOut)
{
    struct virt_iav_aperture_part p[3] = {
        { IAV_PART_WARP, 0x86100000ull, 0x360000ull },
        { IAV_PART_ARB_BLUR, 0xb4800800ull, 0x100000ull },	/* not page aligned */
        { IAV_PART_YUV_REMAP, 0xb4a00000ull, 0 },		/* absent */
    };
    struct amba_virt_iav_aux_cfg c;

    LONGS_EQUAL(1, virt_iav_aperture_compose(p, 3, NULL, 0, 1, &c));
    UNSIGNED_LONGS_EQUAL(IAV_PART_WARP, c.range[0].pid);
    UNSIGNED_LONGS_EQUAL(0, c.range[0].offset);
    UNSIGNED_LONGS_EQUAL(0x400000ull, c.size);
    LONGS_EQUAL(0, virt_iav_aperture_compose(p + 1, 2, NULL, 0, 1, &c));
    LONGS_EQUAL(0, virt_iav_aperture_compose(NULL, 0, NULL, 0, 1, &c));
    LONGS_EQUAL(-EINVAL, virt_iav_aperture_compose(p, 3, NULL, 0, 0, &c));
}

TEST(IavApertureCompose, ASizeAnOldModuleWouldTakeForAnUnpaddedWindowIsDoubled)
{
    struct virt_iav_aperture_part p = { IAV_PART_CANVAS_OVERLAY, 0x28900000ull, 0x200000ull };
    struct amba_virt_iav_aux_cfg c;
    const uint64_t avoid4[] = { 0x400000ull, 0x800000ull };

    LONGS_EQUAL(1, virt_iav_aperture_compose(&p, 1, NULL, 0, 1, &c));
    UNSIGNED_LONGS_EQUAL(0x400000ull, c.size);
    LONGS_EQUAL(1, virt_iav_aperture_compose(&p, 1, avoid4, 2, 1, &c));
    UNSIGNED_LONGS_EQUAL(0x1000000ull, c.size);
}

TEST_GROUP(IavApertureNodes)
{
    void setup() override { useFakeNodes(); }
    void teardown() override { virt_iav_aperture_reset(); }
};

TEST(IavApertureNodes, WithoutApertureNodesNothingIsEverAttached)
{
    uint64_t size = 1;

    g_nodes.present = false;
    CHECK(virt_iav_aperture_init(Parts, NParts, Avoid, 2) != 0);
    CHECK(virt_iav_aperture_attach(0, 3, 1, &size) != 0);
    UNSIGNED_LONGS_EQUAL(0, size);
    LONGS_EQUAL(0, g_nodes.configs);
}

TEST(IavApertureNodes, ThePrimaryGetsItsApertureAConsumerNoneAndAReleaseClearsIt)
{
    struct virt_iav_aperture_view v;
    uint64_t size = 0;

    LONGS_EQUAL(0, virt_iav_aperture_init(Parts, NParts, Avoid, 2));
    LONGS_EQUAL(NParts, g_nodes.sources.count);
    LONGS_EQUAL(0, virt_iav_aperture_attach(2, 4, 1, &size));
    UNSIGNED_LONGS_EQUAL(0x4000000ull, size);
    UNSIGNED_LONGS_EQUAL(4, g_nodes.cfg[2].epoch);
    LONGS_EQUAL(0, virt_iav_aperture_get(2, &v));
    LONGS_EQUAL(7, v.count);
    UNSIGNED_LONGS_EQUAL(0x86100000ull, v.e[1].base);
    UNSIGNED_LONGS_EQUAL(0x2000000ull, v.e[1].offset);
    /* the same reserve again while the guest's QEMU maps it: kept, not reconfigured */
    g_nodes.maps[2] = 1;
    LONGS_EQUAL(0, virt_iav_aperture_attach(2, 4, 1, &size));
    UNSIGNED_LONGS_EQUAL(0x4000000ull, size);
    LONGS_EQUAL(1, g_nodes.configs);
    /* a consumer on another index: nothing */
    LONGS_EQUAL(0, virt_iav_aperture_attach(3, 5, 0, &size));
    UNSIGNED_LONGS_EQUAL(0, size);
    LONGS_EQUAL(0, virt_iav_aperture_get(3, &v));
    LONGS_EQUAL(0, v.count);
    /* the release while still mapped: the table forgets it, the node waits */
    virt_iav_aperture_detach(2);
    LONGS_EQUAL(0, virt_iav_aperture_get(2, &v));
    LONGS_EQUAL(0, v.count);
    CHECK(g_nodes.cfg[2].size != 0);
    /* the next holder of index 2 is a primary with another epoch while the old QEMU still maps: no aperture */
    CHECK(virt_iav_aperture_attach(2, 6, 1, &size) != 0);
    UNSIGNED_LONGS_EQUAL(0, size);
    /* once unmapped: configured for the new holder */
    g_nodes.maps[2] = 0;
    LONGS_EQUAL(0, virt_iav_aperture_attach(2, 6, 1, &size));
    UNSIGNED_LONGS_EQUAL(6, g_nodes.cfg[2].epoch);
    /* released unmapped: cleared at once; a consumer next on the index finds nothing of it */
    virt_iav_aperture_detach(2);
    UNSIGNED_LONGS_EQUAL(0, g_nodes.cfg[2].size);
}

TEST(IavApertureNodes, AConsumerReserveClearsWhatAPreviousPrimaryLeftOnTheIndex)
{
    uint64_t size = 0;

    LONGS_EQUAL(0, virt_iav_aperture_init(Parts, NParts, Avoid, 2));
    LONGS_EQUAL(0, virt_iav_aperture_attach(1, 4, 1, &size));
    LONGS_EQUAL(0, virt_iav_aperture_attach(1, 7, 0, &size));
    UNSIGNED_LONGS_EQUAL(0, size);
    UNSIGNED_LONGS_EQUAL(0, g_nodes.cfg[1].size);
}

TEST(IavApertureNodes, AnEpochTheWireCannotCarryGetsNoAperture)
{
    uint64_t size = 1;

    LONGS_EQUAL(0, virt_iav_aperture_init(Parts, NParts, Avoid, 2));
    LONGS_EQUAL(0, virt_iav_aperture_attach(0, 0x100000000ull, 1, &size));
    UNSIGNED_LONGS_EQUAL(0, size);
    LONGS_EQUAL(0, g_nodes.configs);
    LONGS_EQUAL(-EINVAL, virt_iav_aperture_attach(AMBA_VIRT_IAV_AUX_NODES, 1, 1, &size));
}

TEST(IavApertureNodes, ARestartAdoptsWhatARunningGuestMapsAndClearsWhatNobodyDoes)
{
    struct virt_iav_aperture_view v;
    uint64_t size = 0;

    LONGS_EQUAL(0, virt_iav_aperture_init(Parts, NParts, Avoid, 2));
    LONGS_EQUAL(0, virt_iav_aperture_attach(0, 3, 1, &size));
    LONGS_EQUAL(0, virt_iav_aperture_attach(4, 8, 1, &size));
    g_nodes.maps[0] = 1;
    /* the server restarts: its table is gone, the nodes are not */
    virt_iav_aperture_reset();
    {
        struct virt_iav_aperture_ops ops = { NULL, nodeIoctl };

        virt_iav_aperture_set_ops(&ops);
    }
    LONGS_EQUAL(0, virt_iav_aperture_init(Parts, NParts, Avoid, 2));
    LONGS_EQUAL(1, virt_iav_aperture_adopt(0, 3, 1));
    LONGS_EQUAL(0, virt_iav_aperture_get(0, &v));
    LONGS_EQUAL(7, v.count);
    /* a lease whose epoch the node does not carry, unmapped: cleared, not adopted */
    LONGS_EQUAL(0, virt_iav_aperture_adopt(4, 9, 1));
    UNSIGNED_LONGS_EQUAL(0, g_nodes.cfg[4].size);
    /* a lease that became a consumer while its QEMU maps the old aperture: not adopted, not touched */
    g_nodes.maps[0] = 1;
    LONGS_EQUAL(0, virt_iav_aperture_adopt(0, 3, 0));
    CHECK(g_nodes.cfg[0].size != 0);
}

TEST(IavApertureNodes, ThePartitionsMustBeSourcesOfTheModule)
{
    uint64_t size = 0;

    LONGS_EQUAL(0, virt_iav_aperture_init(Parts, NParts, Avoid, 2));
    /* the module refuses a range that is not a registered source (here the module's list changed under the server) */
    g_nodes.sources.src[2].phys += 0x200000;
    CHECK(virt_iav_aperture_attach(0, 1, 1, &size) != 0);
    UNSIGNED_LONGS_EQUAL(0, size);
}

TEST(IavApertureNodes, TheStatusLineNamesTheAttachedNodes)
{
    char buf[256];
    uint64_t size = 0;

    CHECK(virt_iav_aperture_format(buf, sizeof(buf)) > 0);
    CHECK(strstr(buf, "state=off") != NULL);
    LONGS_EQUAL(0, virt_iav_aperture_init(Parts, NParts, Avoid, 2));
    LONGS_EQUAL(0, virt_iav_aperture_attach(5, 2, 1, &size));
    CHECK(virt_iav_aperture_format(buf, sizeof(buf)) > 0);
    CHECK(strstr(buf, "state=ready sources=8 nodes=5:0x4000000/7") != NULL);
    LONGS_EQUAL(-1, virt_iav_aperture_format(buf, 8));
}

/* ---- the grant ---- */

TEST_GROUP(IavApertureGrant)
{
    char dir[64];
    char leases[96];
    char roles[96];
    char store[96];

    void setup() override
    {
        struct virt_camera_window w[2] = { { AMBA_VIRT_IAV_WIN_DSP, 1ull << 30 }, { AMBA_VIRT_IAV_WIN_BSB, 16ull << 20 } };

        snprintf(dir, sizeof(dir), "/tmp/amba-aperture-XXXXXX");
        CHECK(mkdtemp(dir) != nullptr);
        snprintf(leases, sizeof(leases), "%s/leases", dir);
        snprintf(roles, sizeof(roles), "%s/roles", dir);
        snprintf(store, sizeof(store), "%s/tickets", dir);
        virt_camera_policy_reset();
        virt_camera_policy_set_store(store);
        virt_iav_role_set_store(roles);
        virt_slice_lease_reset();
        virt_slice_lease_set_store(leases);
        virt_slice_lease_set_geometry(1, 8, 1ull << 30);
        virt_camera_policy_set_windows(w, 2, 1);
        useFakeNodes();
        LONGS_EQUAL(0, virt_iav_aperture_init(Parts, NParts, Avoid, 2));
        virt_slice_lease_set_aux_hooks(virt_iav_aperture_attach, virt_iav_aperture_detach);
    }

    void teardown() override
    {
        virt_slice_lease_reset();
        virt_camera_policy_reset();
        virt_iav_aperture_reset();
        unlink(leases);
        unlink(roles);
        unlink(store);
        rmdir(dir);
    }

    std::string lease(const char *req)
    {
        char resp[512];

        memset(resp, 0, sizeof(resp));
        LONGS_EQUAL(0, virt_slice_lease_admin(req, resp, sizeof(resp)));
        return resp;
    }
};

TEST(IavApertureGrant, ThePrimaryGrantCarriesAuxAndTheConsumerGrantDoesNot)
{
    std::string p = lease("RESERVE_SLICE_AT 11111111-1111-4111-8111-111111111111 1073741824 0 amba_shm0 on");
    std::string c = lease("RESERVE_SLICE_MIN 22222222-2222-4222-8222-222222222222 1073741824 1 amba_shm1 on");
    std::string off = lease("RESERVE_SLICE_MIN 33333333-3333-4333-8333-333333333333 1073741824 1 amba_shm2 off");

    CHECK_TEXT(p.find(" aux=67108864") != std::string::npos, p.c_str());
    CHECK_TEXT(p.find(" role=primary") != std::string::npos, p.c_str());
    CHECK_TEXT(c.find("aux=") == std::string::npos, c.c_str());
    CHECK_TEXT(off.find("aux=") == std::string::npos, off.c_str());
    CHECK(g_nodes.cfg[0].size == 0x4000000ull);
    UNSIGNED_LONGS_EQUAL(0, g_nodes.cfg[1].size);
    /* a repeated reserve of the running primary keeps its aperture */
    g_nodes.maps[0] = 1;
    p = lease("RESERVE_SLICE_AT 11111111-1111-4111-8111-111111111111 1073741824 0 amba_shm0 on");
    CHECK_TEXT(p.find(" aux=67108864") != std::string::npos, p.c_str());
    /* the release forgets it; the node is cleared once nothing maps it */
    {
        struct virt_iav_aperture_view v;
        std::string r;
        char req[160];

        snprintf(req, sizeof(req), "RELEASE_SLICE 11111111-1111-4111-8111-111111111111 %llu",
                 (unsigned long long)g_nodes.cfg[0].epoch);
        r = lease(req);
        STRCMP_EQUAL("OK\n", r.c_str());
        LONGS_EQUAL(0, virt_iav_aperture_get(0, &v));
        LONGS_EQUAL(0, v.count);
    }
}

/* ---- the new partitions ---- */

namespace {

/* An IAV driver with a partition table, as far as QUERY_MEMBLOCK, ALLOC_MEM_PART and FREE_MEM_PART go. */
struct FakeIav
{
    std::map<uint32_t, uint64_t> size;	/* allocated sizes */
    std::vector<unsigned long> calls;
    int allocErr;

    FakeIav() : allocErr(0) {}

    static uint64_t reserveOf(uint32_t pid)
    {
        return pid == IAV_PART_BLUR ? 0x81000ull : pid == IAV_PART_YUV_REMAP ? 0x38000ull : 0;
    }

    int ioctl(unsigned long cmd, void *arg)
    {
        calls.push_back(cmd);
        if (cmd == IAV_IOC_QUERY_MEMBLOCK) {
            struct iav_querymem *q = static_cast<struct iav_querymem *>(arg);
            uint32_t pid = q->arg.partition.pid;

            if (!size.count(pid)) {
                return 0;	/* absent: the driver leaves the zeros */
            }
            if (size[pid] <= reserveOf(pid)) {
                return -EINVAL;
            }
            q->arg.partition.mem.addr = 0xb4800000ull + pid * 0x100000ull;
            q->arg.partition.mem.length = size[pid] - reserveOf(pid);
            return 0;
        }
        if (cmd == IAV_IOC_ALLOC_MEM_PART) {
            struct iav_alloc_mem_part *a = static_cast<struct iav_alloc_mem_part *>(arg);

            /* the flags the byte map says: the private pool, ARM and DSP, uncached */
            CHECK(a->from_private == 1);
            CHECK(a->accessibility == (IAV_MEM_PART_ACS_ARM | IAV_MEM_PART_ACS_DSP));
            CHECK(a->enable_cache == 0);
            if (allocErr) {
                return -allocErr;
            }
            if (size.count(a->pid)) {
                return -EINVAL;
            }
            size[a->pid] = a->length;
            return 0;
        }
        if (cmd == IAV_IOC_FREE_MEM_PART) {
            struct iav_free_mem_part *f = static_cast<struct iav_free_mem_part *>(arg);

            if (!size.count(f->pid)) {
                return -EINVAL;
            }
            size.erase(f->pid);
            return 0;
        }
        return -ENOTTY;
    }
};

int fakeIavIoctl(void *ctx, unsigned long cmd, void *arg)
{
    return static_cast<FakeIav *>(ctx)->ioctl(cmd, arg);
}

} // namespace

TEST_GROUP(IavNewParts)
{
};

TEST(IavNewParts, TheThreePartitionsAre0x2C9000BytesAndAreAllocatedOnce)
{
    FakeIav d;
    struct iav_new_parts_state st;
    uint64_t total = 0;

    for (unsigned int i = 0; i < IAV_NEW_PARTS_COUNT; i++) {
        total += iav_new_parts[i].alloc_len;
    }
    UNSIGNED_LONGS_EQUAL(0x2c9000ull, total);
    LONGS_EQUAL(3, iav_new_parts_ensure(fakeIavIoctl, &d, &st));
    for (unsigned int i = 0; i < IAV_NEW_PARTS_COUNT; i++) {
        LONGS_EQUAL(IAV_NEW_PART_ALLOCATED, st.state[i]);
        UNSIGNED_LONGS_EQUAL(iav_new_parts[i].report_len, st.len[i]);
    }
    UNSIGNED_LONGS_EQUAL(0x181000ull, d.size[IAV_PART_BLUR]);
    UNSIGNED_LONGS_EQUAL(0x100000ull, d.size[IAV_PART_ARB_BLUR]);
    UNSIGNED_LONGS_EQUAL(0x48000ull, d.size[IAV_PART_YUV_REMAP]);
    /* a restart reuses them */
    LONGS_EQUAL(3, iav_new_parts_ensure(fakeIavIoctl, &d, &st));
    for (unsigned int i = 0; i < IAV_NEW_PARTS_COUNT; i++) {
        LONGS_EQUAL(IAV_NEW_PART_REUSED, st.state[i]);
    }
}

TEST(IavNewParts, APartitionOfAnotherLengthIsLeftAloneAndAFailureIsReported)
{
    FakeIav d;
    struct iav_new_parts_state st;

    d.size[IAV_PART_ARB_BLUR] = 0x200000;	/* someone else's */
    d.size[IAV_PART_BLUR] = 0x81000;	/* no larger than the reserve: the driver's query refuses */
    LONGS_EQUAL(1, iav_new_parts_ensure(fakeIavIoctl, &d, &st));
    LONGS_EQUAL(IAV_NEW_PART_MISMATCH, st.state[0]);
    LONGS_EQUAL(IAV_NEW_PART_MISMATCH, st.state[1]);
    LONGS_EQUAL(IAV_NEW_PART_ALLOCATED, st.state[2]);
    UNSIGNED_LONGS_EQUAL(0x200000ull, d.size[IAV_PART_ARB_BLUR]);
    /* the qualification free frees only what has the expected length */
    LONGS_EQUAL(1, iav_new_parts_free(fakeIavIoctl, &d, &st));
    LONGS_EQUAL(IAV_NEW_PART_FREED, st.state[2]);
    LONGS_EQUAL(0, d.size.count(IAV_PART_YUV_REMAP));
    UNSIGNED_LONGS_EQUAL(0x200000ull, d.size[IAV_PART_ARB_BLUR]);

    FakeIav e;
    e.allocErr = ENOMEM;
    LONGS_EQUAL(0, iav_new_parts_ensure(fakeIavIoctl, &e, &st));
    LONGS_EQUAL(IAV_NEW_PART_ABSENT, st.state[0]);
    LONGS_EQUAL(-ENOMEM, st.rc[0]);
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
