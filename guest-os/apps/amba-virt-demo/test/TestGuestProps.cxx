/*
 * TestGuestProps.cxx
 *
 * Unit tests for GuestPropsService and AmbaVirtLayout.hxx.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include "CppUTest/TestHarness.h"

#include "../GuestProps.hxx"
#include "AmbaVirtLayout.hxx"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace {

constexpr size_t kQueryHdrLen = offsetof(struct amba_virt_query_resp, payload);

struct amba_virt_query_resp makeSelfResp(uint32_t cid,
                                         uint32_t tenantIdx,
                                         uint32_t status,
                                         uint32_t memAllocatedMb,
                                         uint32_t caps,
                                         int32_t respStatus = 0,
                                         uint32_t count = 1)
{
    struct amba_virt_query_resp resp;
    std::memset(&resp, 0, sizeof(resp));
    resp.query_op = AMBA_VIRT_QUERY_SELF;
    resp.status = respStatus;
    resp.count = count;

    struct amba_virt_peer_desc desc;
    std::memset(&desc, 0, sizeof(desc));
    desc.cid = cid;
    desc.tenant_idx = tenantIdx;
    desc.status = status;
    desc.mem_allocated_mb = memAllocatedMb;
    desc.caps = caps;

    std::memcpy(resp.payload, &desc, sizeof(desc));
    return resp;
}

struct amba_virt_query_resp makeDevMemResp(
    const std::vector<struct amba_virt_dev_mem_desc> &devs,
    int32_t respStatus = 0,
    uint32_t overrideCount = 0xFFFFFFFFU)
{
    struct amba_virt_query_resp resp;
    std::memset(&resp, 0, sizeof(resp));
    resp.query_op = AMBA_VIRT_QUERY_DEV_MEM;
    resp.status = respStatus;
    resp.count = (overrideCount == 0xFFFFFFFFU)
                     ? static_cast<uint32_t>(devs.size())
                     : overrideCount;

    size_t bytes = devs.size() * sizeof(struct amba_virt_dev_mem_desc);
    if (bytes > sizeof(resp.payload)) {
        bytes = sizeof(resp.payload);
    }
    if (bytes > 0) {
        std::memcpy(resp.payload, devs.data(), bytes);
    }
    return resp;
}

/*
 * Legacy formatter reproducing the exact pre-AmbaVirtLayout.hxx showLayout()
 * implementation in amba-virt-cli.cxx for Task 1.4 before/after comparison.
 */
std::string legacyFormatLayout(const struct amba_virt_peer_desc &selfDesc,
                               std::vector<struct amba_virt_dev_mem_desc> devs,
                               uint32_t barSize)
{
    auto legacyDevTypeName = [](uint32_t devType) -> const char * {
        switch (devType) {
        case AMBA_VIRT_DEV_TYPE_CAVALRY: return "CAVALRY (NPU)";
        case AMBA_VIRT_DEV_TYPE_GDMA:    return "GDMA";
        case AMBA_VIRT_DEV_TYPE_IAV:     return "IAV";
        case AMBA_VIRT_DEV_TYPE_SCRATCH: return "SCRATCH";
        default:                         return "UNKNOWN";
        }
    };

    auto legacyFormatCaps = [](uint32_t caps) -> std::string {
        std::vector<std::string> names;
        if (caps & AMBA_VIRT_CAP_PING)             names.push_back("PING");
        if (caps & AMBA_VIRT_CAP_QUERY_SELF)       names.push_back("QUERY_SELF");
        if (caps & AMBA_VIRT_CAP_QUERY_PEERS)      names.push_back("QUERY_PEERS");
        if (caps & AMBA_VIRT_CAP_QUERY_TOPO)       names.push_back("QUERY_TOPO");
        if (caps & AMBA_VIRT_CAP_MEM_ALLOC)        names.push_back("MEM_ALLOC");
        if (caps & AMBA_VIRT_CAP_MEM_RESIZE)       names.push_back("MEM_RESIZE");
        if (caps & AMBA_VIRT_CAP_DEV_CONFIG)       names.push_back("DEV_CONFIG");
        if (caps & AMBA_VIRT_CAP_GDMA_COPY)        names.push_back("GDMA_COPY");
        if (caps & AMBA_VIRT_CAP_GDMA_PITCH)       names.push_back("GDMA_PITCH");
        if (caps & AMBA_VIRT_CAP_CAVALRY_PATH_B)   names.push_back("CAVALRY_PATH_B");
        if (caps & AMBA_VIRT_CAP_CAVALRY_PATH_A)   names.push_back("CAVALRY_PATH_A");
        if (caps & AMBA_VIRT_CAP_CAVALRY_REGISTER) names.push_back("CAVALRY_REGISTER");
        if (caps & AMBA_VIRT_CAP_IAV_STREAM)       names.push_back("IAV_STREAM");

        if (names.empty()) {
            return "NONE";
        }
        std::ostringstream oss;
        for (size_t i = 0; i < names.size(); ++i) {
            if (i > 0) {
                oss << ", ";
            }
            oss << names[i];
        }
        return oss.str();
    };

    auto toHex = [](uint32_t val) -> std::string {
        std::ostringstream oss;
        oss << "0x" << std::hex << std::setw(8) << std::setfill('0') << val;
        return oss.str();
    };

    auto toMb = [](uint32_t bytes) -> std::string {
        std::ostringstream oss;
        oss << (bytes / (1024 * 1024)) << " MB";
        return oss.str();
    };

    uint64_t totalAllocatedBytes = 0;
    std::sort(devs.begin(), devs.end(),
              [](const struct amba_virt_dev_mem_desc &a,
                 const struct amba_virt_dev_mem_desc &b) {
                  return a.base_offset < b.base_offset;
              });

    for (const auto &d : devs) {
        totalAllocatedBytes += d.size;
        if (d.rpc_arena_size > 0) {
            totalAllocatedBytes += d.rpc_arena_size;
        }
    }

    double allocMb = static_cast<double>(totalAllocatedBytes) / (1024.0 * 1024.0);
    double barMb = static_cast<double>(barSize) / (1024.0 * 1024.0);
    double utilPct = (barMb > 0.0) ? (allocMb / barMb * 100.0) : 0.0;

    std::ostringstream out;
    out << "================================================================================\n"
        << " Ambarella Virtualization Guest Memory Layout (/dev/amba_virt)\n"
        << "================================================================================\n"
        << " Tenant CID:       " << selfDesc.cid << "\n"
        << " Status:           " << (selfDesc.status == 1 ? "ONLINE" : "OFFLINE") << "\n"
        << " ivshmem BAR Size: " << std::fixed << std::setprecision(0) << barMb << " MB ("
        << toHex(barSize) << ")\n"
        << " Memory In Use:    " << std::fixed << std::setprecision(1) << allocMb << " MB / "
        << barMb << " MB (" << std::setprecision(1) << utilPct << "% utilized, "
        << (barMb - allocMb) << " MB free)\n"
        << " Capabilities:     " << toHex(selfDesc.caps) << " [" << legacyFormatCaps(selfDesc.caps) << "]\n"
        << "--------------------------------------------------------------------------------\n"
        << " Virtual Device Partitions:\n"
        << "--------------------------------------------------------------------------------\n"
        << " Dev ID  Device Type    BAR Offset      End Offset      Size       RPC Arena\n";

    if (devs.empty()) {
        out << " (no active virtual device partitions registered)\n";
    } else {
        for (const auto &d : devs) {
            uint32_t endOff = d.base_offset + d.size;
            out << " [" << d.dev_type << "]     "
                << std::left << std::setw(14) << std::setfill(' ') << legacyDevTypeName(d.dev_type) << " "
                << toHex(d.base_offset) << "      "
                << toHex(endOff) << "      "
                << std::right << std::setw(8) << std::setfill(' ') << toMb(d.size) << "   ";
            if (d.rpc_arena_size > 0) {
                out << toHex(d.rpc_arena_offset)
                    << " (" << (d.rpc_arena_size / 1024) << " KB)\n";
            } else {
                out << "N/A\n";
            }
        }
    }

    struct Extent {
        uint32_t start;
        uint32_t end;
        std::string name;
    };

    std::vector<Extent> extents;
    for (const auto &d : devs) {
        Extent e;
        e.start = d.base_offset;
        e.end = d.base_offset + d.size;
        e.name = std::string(legacyDevTypeName(d.dev_type)) + " Buffer";
        extents.push_back(e);

        if (d.rpc_arena_size > 0) {
            Extent a;
            a.start = d.rpc_arena_offset;
            a.end = d.rpc_arena_offset + d.rpc_arena_size;
            a.name = std::string(legacyDevTypeName(d.dev_type)) + " RPC Arena";
            extents.push_back(a);
        }
    }

    std::sort(extents.begin(), extents.end(), [](const Extent &a, const Extent &b) {
        return a.start < b.start;
    });

    out << "--------------------------------------------------------------------------------\n"
        << " Memory Extent Map:\n"
        << " Status  Usage                  Start Offset    End Offset      Size\n";

    uint32_t cur = 0;
    for (const auto &e : extents) {
        if (e.start > cur) {
            uint32_t freeSz = e.start - cur;
            out << " [FREE]  UNALLOCATED            "
                << toHex(cur) << "      "
                << toHex(e.start) << "      "
                << std::right << std::setw(8) << std::setfill(' ') << toMb(freeSz) << "\n";
        }
        uint32_t sz = e.end - e.start;
        out << " [ALLOC] "
            << std::left << std::setw(22) << std::setfill(' ') << e.name << " "
            << toHex(e.start) << "      "
            << toHex(e.end) << "      "
            << std::right << std::setw(8) << std::setfill(' ') << toMb(sz) << "\n";
        cur = (e.end > cur) ? e.end : cur;
    }
    if (cur < barSize) {
        uint32_t freeSz = barSize - cur;
        out << " [FREE]  UNALLOCATED            "
            << toHex(cur) << "      "
            << toHex(barSize) << "      "
            << std::right << std::setw(8) << std::setfill(' ') << toMb(freeSz) << "\n";
    }
    out << "================================================================================\n";
    return out.str();
}

} // namespace

TEST_GROUP(GuestProps)
{
    void setup() override {}
    void teardown() override {}
};

TEST(GuestProps, ZeroPartitionsOnFullBar)
{
    const uint32_t barSize = 1073737728U; // 0x3ffff000
    auto selfResp = makeSelfResp(5, 1, 1, 0, AMBA_VIRT_CAP_PING | AMBA_VIRT_CAP_QUERY_SELF);
    std::vector<struct amba_virt_dev_mem_desc> emptyDevs;
    auto devMemResp = makeDevMemResp(emptyDevs);

    GuestProperties props;
    int rc = GuestPropsService::Decode(
        0, 0, barSize,
        0, selfResp, static_cast<uint32_t>(kQueryHdrLen + sizeof(struct amba_virt_peer_desc)),
        0, devMemResp, static_cast<uint32_t>(kQueryHdrLen),
        props);

    LONGS_EQUAL(0, rc);
    CHECK_TRUE(props.online);
    LONGS_EQUAL(0, props.query_status);
    STRCMP_EQUAL("", props.error.c_str());
    LONGS_EQUAL(5U, props.tenant_cid);
    LONGS_EQUAL(1U, props.tenant_idx);
    LONGS_EQUAL(barSize, props.bar_size_bytes);
    LONGS_EQUAL(0ULL, props.partition_bytes);
    LONGS_EQUAL(0U, props.devices.size());
    LONGS_EQUAL(1U, props.extents.size());
    CHECK_FALSE(props.extents[0].partition);
    STRCMP_EQUAL("NO PARTITION", props.extents[0].usage.c_str());
    LONGS_EQUAL(0ULL, props.extents[0].start_offset);
    LONGS_EQUAL(static_cast<uint64_t>(barSize), props.extents[0].end_offset);
    CHECK_FALSE(props.extents[0].clipped);
    CHECK_FALSE(props.extents[0].overlaps);

    const char *golden =
        "================================================================================\n"
        " Ambarella Virtualization Guest Memory Layout (/dev/amba_virt)\n"
        "================================================================================\n"
        " Tenant CID:       5\n"
        " Status:           ONLINE\n"
        " ivshmem BAR Size: 1024 MB (0x3ffff000)\n"
        " Memory In Use:    0.0 MB / 1024.0 MB (0.0% utilized, 1024.0 MB free)\n"
        " Capabilities:     0x00000003 [PING, QUERY_SELF]\n"
        "--------------------------------------------------------------------------------\n"
        " Virtual Device Partitions:\n"
        "--------------------------------------------------------------------------------\n"
        " Dev ID  Device Type    BAR Offset      End Offset      Size       RPC Arena\n"
        " (no active virtual device partitions registered)\n"
        "--------------------------------------------------------------------------------\n"
        " Memory Extent Map:\n"
        " Status  Usage                  Start Offset    End Offset      Size\n"
        " [FREE]  UNALLOCATED            0x00000000      0x3ffff000       1023 MB\n"
        "================================================================================\n";

    STRCMP_EQUAL(golden, props.layout_text.c_str());
}

TEST(GuestProps, SingleCavalryPartitionWithArena)
{
    const uint32_t barSize = 1073737728U;
    const uint32_t caps = 0x00006bd3U; // 27603
    auto selfResp = makeSelfResp(9, 2, 1, 993, caps);

    struct amba_virt_dev_mem_desc cav{};
    cav.dev_type = AMBA_VIRT_DEV_TYPE_CAVALRY;
    cav.base_offset = 2097152U;      // 0x00200000
    cav.size = 1040187392U;          // 0x3e000000
    cav.rpc_arena_offset = 0U;
    cav.rpc_arena_size = 1048576U;   // 0x00100000
    cav.flags = 0U;

    std::vector<struct amba_virt_dev_mem_desc> devs = { cav };
    auto devMemResp = makeDevMemResp(devs);

    GuestProperties props;
    int rc = GuestPropsService::Decode(
        0, 0, barSize,
        0, selfResp, static_cast<uint32_t>(kQueryHdrLen + sizeof(struct amba_virt_peer_desc)),
        0, devMemResp, static_cast<uint32_t>(kQueryHdrLen + sizeof(cav)),
        props);

    LONGS_EQUAL(0, rc);
    CHECK_TRUE(props.online);
    LONGS_EQUAL(9U, props.tenant_cid);
    LONGS_EQUAL(2U, props.tenant_idx);
    LONGS_EQUAL(993U, props.server_allocated_mb);
    LONGS_EQUAL(barSize, props.bar_size_bytes);
    LONGS_EQUAL(1041235968ULL, props.partition_bytes);

    LONGS_EQUAL(1U, props.devices.size());
    LONGS_EQUAL(1U, props.devices[0].dev_type);
    STRCMP_EQUAL("CAVALRY (NPU)", props.devices[0].dev_type_name.c_str());
    LONGS_EQUAL(2097152U, props.devices[0].base_offset);
    LONGS_EQUAL(1042284544ULL, props.devices[0].end_offset);
    LONGS_EQUAL(1040187392U, props.devices[0].size_bytes);
    LONGS_EQUAL(0U, props.devices[0].rpc_arena_offset);
    LONGS_EQUAL(1048576U, props.devices[0].rpc_arena_size);

    LONGS_EQUAL(4U, props.extents.size());

    CHECK_TRUE(props.extents[0].partition);
    STRCMP_EQUAL("CAVALRY (NPU) RPC Arena", props.extents[0].usage.c_str());
    LONGS_EQUAL(0ULL, props.extents[0].start_offset);
    LONGS_EQUAL(1048576ULL, props.extents[0].end_offset);
    CHECK_FALSE(props.extents[0].clipped);
    CHECK_FALSE(props.extents[0].overlaps);

    CHECK_FALSE(props.extents[1].partition);
    STRCMP_EQUAL("NO PARTITION", props.extents[1].usage.c_str());
    LONGS_EQUAL(1048576ULL, props.extents[1].start_offset);
    LONGS_EQUAL(2097152ULL, props.extents[1].end_offset);

    CHECK_TRUE(props.extents[2].partition);
    STRCMP_EQUAL("CAVALRY (NPU) Buffer", props.extents[2].usage.c_str());
    LONGS_EQUAL(2097152ULL, props.extents[2].start_offset);
    LONGS_EQUAL(1042284544ULL, props.extents[2].end_offset);

    CHECK_FALSE(props.extents[3].partition);
    STRCMP_EQUAL("NO PARTITION", props.extents[3].usage.c_str());
    LONGS_EQUAL(1042284544ULL, props.extents[3].start_offset);
    LONGS_EQUAL(1073737728ULL, props.extents[3].end_offset);

    uint64_t extentSum = 0;
    for (const auto &e : props.extents) {
        extentSum += (e.end_offset - e.start_offset);
    }
    LONGS_EQUAL(static_cast<uint64_t>(barSize), extentSum);
}

TEST(GuestProps, EightPartitionsMixedTypesOutOfOrder)
{
    const uint32_t mb = 1024U * 1024U;
    const uint32_t barSize = 256U * mb;
    auto selfResp = makeSelfResp(12, 0, 1, 80, AMBA_VIRT_CAP_PING);

    // 8 partitions placed out of order in the response buffer, with gaps between them
    const uint32_t types[8] = {
        AMBA_VIRT_DEV_TYPE_IAV,
        AMBA_VIRT_DEV_TYPE_CAVALRY,
        AMBA_VIRT_DEV_TYPE_SCRATCH,
        AMBA_VIRT_DEV_TYPE_GDMA,
        99U,
        AMBA_VIRT_DEV_TYPE_CAVALRY,
        AMBA_VIRT_DEV_TYPE_GDMA,
        AMBA_VIRT_DEV_TYPE_SCRATCH,
    };
    const uint32_t order[8] = { 5, 1, 7, 0, 3, 6, 2, 4 };

    std::vector<struct amba_virt_dev_mem_desc> devs;
    for (size_t i = 0; i < 8; ++i) {
        uint32_t slot = order[i];
        struct amba_virt_dev_mem_desc d{};
        d.dev_type = types[slot];
        d.base_offset = (slot * 20U + 4U) * mb;
        d.size = 10U * mb;
        d.rpc_arena_offset = 0;
        d.rpc_arena_size = 0;
        d.flags = slot;
        devs.push_back(d);
    }

    auto devMemResp = makeDevMemResp(devs);
    GuestProperties props;
    int rc = GuestPropsService::Decode(
        0, 0, barSize,
        0, selfResp, static_cast<uint32_t>(kQueryHdrLen + sizeof(struct amba_virt_peer_desc)),
        0, devMemResp,
        static_cast<uint32_t>(kQueryHdrLen + devs.size() * sizeof(struct amba_virt_dev_mem_desc)),
        props);

    LONGS_EQUAL(0, rc);
    LONGS_EQUAL(8U, props.devices.size());

    for (size_t slot = 0; slot < 8; ++slot) {
        LONGS_EQUAL((slot * 20U + 4U) * mb, props.devices[slot].base_offset);
        LONGS_EQUAL(types[slot], props.devices[slot].dev_type);
    }
    STRCMP_EQUAL("IAV", props.devices[0].dev_type_name.c_str());
    STRCMP_EQUAL("CAVALRY (NPU)", props.devices[1].dev_type_name.c_str());
    STRCMP_EQUAL("SCRATCH", props.devices[2].dev_type_name.c_str());
    STRCMP_EQUAL("GDMA", props.devices[3].dev_type_name.c_str());
    STRCMP_EQUAL("UNKNOWN", props.devices[4].dev_type_name.c_str());

    // 8 partitions + 8 leading/inter-partition gaps + 1 trailing gap = 17 extents
    LONGS_EQUAL(17U, props.extents.size());
    LONGS_EQUAL(80ULL * mb, props.partition_bytes);

    uint64_t totalSpan = 0;
    for (size_t i = 0; i < props.extents.size(); ++i) {
        if (i > 0) {
            LONGS_EQUAL(props.extents[i - 1].end_offset, props.extents[i].start_offset);
        }
        totalSpan += (props.extents[i].end_offset - props.extents[i].start_offset);
    }
    LONGS_EQUAL(static_cast<uint64_t>(barSize), totalSpan);
}

TEST(GuestProps, FullBarCoveredNoGaps)
{
    const uint32_t mb = 1024U * 1024U;
    const uint32_t barSize = 64U * mb;
    auto selfResp = makeSelfResp(3, 0, 1, 64, AMBA_VIRT_CAP_PING);

    struct amba_virt_dev_mem_desc d1{};
    d1.dev_type = AMBA_VIRT_DEV_TYPE_CAVALRY;
    d1.base_offset = 0;
    d1.size = 32U * mb;

    struct amba_virt_dev_mem_desc d2{};
    d2.dev_type = AMBA_VIRT_DEV_TYPE_GDMA;
    d2.base_offset = 32U * mb;
    d2.size = 32U * mb;

    std::vector<struct amba_virt_dev_mem_desc> devs = { d2, d1 };
    auto devMemResp = makeDevMemResp(devs);

    GuestProperties props;
    int rc = GuestPropsService::Decode(
        0, 0, barSize,
        0, selfResp, static_cast<uint32_t>(kQueryHdrLen + sizeof(struct amba_virt_peer_desc)),
        0, devMemResp,
        static_cast<uint32_t>(kQueryHdrLen + devs.size() * sizeof(struct amba_virt_dev_mem_desc)),
        props);

    LONGS_EQUAL(0, rc);
    LONGS_EQUAL(static_cast<uint64_t>(barSize), props.partition_bytes);
    LONGS_EQUAL(2U, props.extents.size());
    CHECK_TRUE(props.extents[0].partition);
    CHECK_TRUE(props.extents[1].partition);
}

TEST(GuestProps, BarSizeZeroAndMaxUint32)
{
    auto selfResp = makeSelfResp(4, 0, 1, 0, AMBA_VIRT_CAP_PING);
    std::vector<struct amba_virt_dev_mem_desc> emptyDevs;
    auto emptyDevMem = makeDevMemResp(emptyDevs);

    // 1. BAR size 0 with 0 partitions
    GuestProperties propsZero;
    int rc = GuestPropsService::Decode(
        0, 0, 0U,
        0, selfResp, static_cast<uint32_t>(kQueryHdrLen + sizeof(struct amba_virt_peer_desc)),
        0, emptyDevMem, static_cast<uint32_t>(kQueryHdrLen),
        propsZero);
    LONGS_EQUAL(0, rc);
    LONGS_EQUAL(0U, propsZero.bar_size_bytes);
    LONGS_EQUAL(0ULL, propsZero.partition_bytes);
    LONGS_EQUAL(0U, propsZero.extents.size());

    // 2. BAR size 0xFFFFFFFF (driver clamp) with a partition near the top
    const uint32_t maxBar = 0xFFFFFFFFU;
    struct amba_virt_dev_mem_desc d{};
    d.dev_type = AMBA_VIRT_DEV_TYPE_SCRATCH;
    d.base_offset = 0xFFF00000U;
    d.size = 0x000FFFFFU;
    std::vector<struct amba_virt_dev_mem_desc> devs = { d };
    auto devMemResp = makeDevMemResp(devs);

    GuestProperties propsMax;
    rc = GuestPropsService::Decode(
        0, 0, maxBar,
        0, selfResp, static_cast<uint32_t>(kQueryHdrLen + sizeof(struct amba_virt_peer_desc)),
        0, devMemResp, static_cast<uint32_t>(kQueryHdrLen + sizeof(d)),
        propsMax);
    LONGS_EQUAL(0, rc);
    LONGS_EQUAL(maxBar, propsMax.bar_size_bytes);
    LONGS_EQUAL(0x000FFFFFULL, propsMax.partition_bytes);
    LONGS_EQUAL(2U, propsMax.extents.size());
    CHECK_FALSE(propsMax.extents[0].partition);
    LONGS_EQUAL(0ULL, propsMax.extents[0].start_offset);
    LONGS_EQUAL(0xFFF00000ULL, propsMax.extents[0].end_offset);
    CHECK_TRUE(propsMax.extents[1].partition);
    LONGS_EQUAL(0xFFF00000ULL, propsMax.extents[1].start_offset);
    LONGS_EQUAL(0xFFFFFFFFULL, propsMax.extents[1].end_offset);
    CHECK_FALSE(propsMax.extents[1].clipped);
}

TEST(GuestProps, PartitionPastBarAndUint32Overflow)
{
    const uint32_t mb = 1024U * 1024U;
    const uint32_t barSize = 64U * mb;
    auto selfResp = makeSelfResp(6, 0, 1, 64, AMBA_VIRT_CAP_PING);

    // Partition 1: ends past 64 MB BAR (48 MB + 32 MB = 80 MB)
    struct amba_virt_dev_mem_desc pastBar{};
    pastBar.dev_type = AMBA_VIRT_DEV_TYPE_IAV;
    pastBar.base_offset = 48U * mb;
    pastBar.size = 32U * mb;

    // Partition 2: base_offset + size overflows 32 bits (0xFFFF0000 + 0x00020000 = 0x100010000)
    struct amba_virt_dev_mem_desc overflow32{};
    overflow32.dev_type = AMBA_VIRT_DEV_TYPE_GDMA;
    overflow32.base_offset = 0xFFFF0000U;
    overflow32.size = 0x00020000U;

    std::vector<struct amba_virt_dev_mem_desc> devs = { pastBar, overflow32 };
    auto devMemResp = makeDevMemResp(devs);

    GuestProperties props;
    int rc = GuestPropsService::Decode(
        0, 0, barSize,
        0, selfResp, static_cast<uint32_t>(kQueryHdrLen + sizeof(struct amba_virt_peer_desc)),
        0, devMemResp,
        static_cast<uint32_t>(kQueryHdrLen + devs.size() * sizeof(struct amba_virt_dev_mem_desc)),
        props);

    LONGS_EQUAL(0, rc);
    LONGS_EQUAL(2U, props.devices.size());
    // 64-bit end_offset preserved without 32-bit wrap
    LONGS_EQUAL(80ULL * mb, props.devices[0].end_offset);
    LONGS_EQUAL(0x100010000ULL, props.devices[1].end_offset);

    // Extent 0: NO PARTITION [0, 48MB]
    // Extent 1: IAV Buffer [48MB, 64MB], clipped=true
    // Extent 2: GDMA Buffer [64MB, 64MB], clipped=true
    LONGS_EQUAL(3U, props.extents.size());
    CHECK_FALSE(props.extents[0].partition);
    CHECK_TRUE(props.extents[1].partition);
    CHECK_TRUE(props.extents[1].clipped);
    LONGS_EQUAL(48ULL * mb, props.extents[1].start_offset);
    LONGS_EQUAL(64ULL * mb, props.extents[1].end_offset);

    CHECK_TRUE(props.extents[2].partition);
    CHECK_TRUE(props.extents[2].clipped);
    LONGS_EQUAL(64ULL * mb, props.extents[2].start_offset);
    LONGS_EQUAL(64ULL * mb, props.extents[2].end_offset);

    LONGS_EQUAL(16ULL * mb, props.partition_bytes);
}

TEST(GuestProps, OverlappingPartitionsFlaggedAndUnionCountedOnce)
{
    const uint32_t mb = 1024U * 1024U;
    const uint32_t barSize = 64U * mb;
    auto selfResp = makeSelfResp(7, 0, 1, 30, AMBA_VIRT_CAP_PING);

    // [10MB, 30MB] and [20MB, 40MB] overlap on [20MB, 30MB]; union is [10MB, 40MB] = 30MB
    struct amba_virt_dev_mem_desc p1{};
    p1.dev_type = AMBA_VIRT_DEV_TYPE_CAVALRY;
    p1.base_offset = 10U * mb;
    p1.size = 20U * mb;

    struct amba_virt_dev_mem_desc p2{};
    p2.dev_type = AMBA_VIRT_DEV_TYPE_GDMA;
    p2.base_offset = 20U * mb;
    p2.size = 20U * mb;

    std::vector<struct amba_virt_dev_mem_desc> devs = { p1, p2 };
    auto devMemResp = makeDevMemResp(devs);

    GuestProperties props;
    int rc = GuestPropsService::Decode(
        0, 0, barSize,
        0, selfResp, static_cast<uint32_t>(kQueryHdrLen + sizeof(struct amba_virt_peer_desc)),
        0, devMemResp,
        static_cast<uint32_t>(kQueryHdrLen + devs.size() * sizeof(struct amba_virt_dev_mem_desc)),
        props);

    LONGS_EQUAL(0, rc);
    LONGS_EQUAL(30ULL * mb, props.partition_bytes);
    LONGS_EQUAL(4U, props.extents.size());

    // [0, 10MB] NO PARTITION
    CHECK_FALSE(props.extents[0].partition);
    CHECK_FALSE(props.extents[0].overlaps);
    // [10MB, 30MB] CAVALRY Buffer (overlaps)
    CHECK_TRUE(props.extents[1].partition);
    CHECK_TRUE(props.extents[1].overlaps);
    // [20MB, 40MB] GDMA Buffer (overlaps)
    CHECK_TRUE(props.extents[2].partition);
    CHECK_TRUE(props.extents[2].overlaps);
    // [40MB, 64MB] NO PARTITION
    CHECK_FALSE(props.extents[3].partition);
    CHECK_FALSE(props.extents[3].overlaps);

    uint64_t gapBytes = (props.extents[0].end_offset - props.extents[0].start_offset) +
                        (props.extents[3].end_offset - props.extents[3].start_offset);
    LONGS_EQUAL(static_cast<uint64_t>(barSize), props.partition_bytes + gapBytes);
}

TEST(GuestProps, CountClampedToPayloadLength)
{
    const uint32_t mb = 1024U * 1024U;
    const uint32_t barSize = 64U * mb;
    auto selfResp = makeSelfResp(8, 0, 1, 16, AMBA_VIRT_CAP_PING);

    struct amba_virt_dev_mem_desc d1{};
    d1.dev_type = AMBA_VIRT_DEV_TYPE_CAVALRY;
    d1.base_offset = 0;
    d1.size = 16U * mb;

    struct amba_virt_dev_mem_desc d2{};
    d2.dev_type = AMBA_VIRT_DEV_TYPE_GDMA;
    d2.base_offset = 16U * mb;
    d2.size = 16U * mb;

    std::vector<struct amba_virt_dev_mem_desc> devs = { d1, d2 };
    // Spoof count = 0xFFFFFFFFU while only supplying 1 entry in devMemLen
    auto devMemResp = makeDevMemResp(devs, 0, 0xFFFFFFFFU);

    GuestProperties props;
    int rc = GuestPropsService::Decode(
        0, 0, barSize,
        0, selfResp, static_cast<uint32_t>(kQueryHdrLen + sizeof(struct amba_virt_peer_desc)),
        0, devMemResp,
        static_cast<uint32_t>(kQueryHdrLen + sizeof(struct amba_virt_dev_mem_desc)),
        props);

    LONGS_EQUAL(0, rc);
    LONGS_EQUAL(1U, props.devices.size());
    LONGS_EQUAL(AMBA_VIRT_DEV_TYPE_CAVALRY, props.devices[0].dev_type);
}

TEST(GuestProps, ShortPayloadsRejectedWithEbadmsg)
{
    auto selfResp = makeSelfResp(1, 0, 1, 0, AMBA_VIRT_CAP_PING);
    std::vector<struct amba_virt_dev_mem_desc> emptyDevs;
    auto devMemResp = makeDevMemResp(emptyDevs);

    GuestProperties props;

    // 1. selfLen shorter than 12-byte query_resp header
    int rc = GuestPropsService::Decode(
        0, 0, 1024,
        0, selfResp, static_cast<uint32_t>(kQueryHdrLen - 1),
        0, devMemResp, static_cast<uint32_t>(kQueryHdrLen),
        props);
    LONGS_EQUAL(-EBADMSG, rc);
    CHECK_FALSE(props.online);
    LONGS_EQUAL(-EBADMSG, props.query_status);

    // 2. QUERY_SELF without a full amba_virt_peer_desc
    rc = GuestPropsService::Decode(
        0, 0, 1024,
        0, selfResp, static_cast<uint32_t>(kQueryHdrLen + sizeof(struct amba_virt_peer_desc) - 1),
        0, devMemResp, static_cast<uint32_t>(kQueryHdrLen),
        props);
    LONGS_EQUAL(-EBADMSG, rc);
    CHECK_FALSE(props.online);

    // 3. QUERY_SELF with count == 0
    auto selfZeroCount = makeSelfResp(1, 0, 1, 0, AMBA_VIRT_CAP_PING, 0, 0);
    rc = GuestPropsService::Decode(
        0, 0, 1024,
        0, selfZeroCount, static_cast<uint32_t>(kQueryHdrLen + sizeof(struct amba_virt_peer_desc)),
        0, devMemResp, static_cast<uint32_t>(kQueryHdrLen),
        props);
    LONGS_EQUAL(-EBADMSG, rc);
    CHECK_FALSE(props.online);

    // 4. devMemLen shorter than 12-byte query_resp header
    rc = GuestPropsService::Decode(
        0, 0, 1024,
        0, selfResp, static_cast<uint32_t>(kQueryHdrLen + sizeof(struct amba_virt_peer_desc)),
        0, devMemResp, static_cast<uint32_t>(kQueryHdrLen - 1),
        props);
    LONGS_EQUAL(-EBADMSG, rc);
    CHECK_FALSE(props.online);
}

TEST(GuestProps, FailingStepsAndNegativeStatus)
{
    auto selfResp = makeSelfResp(1, 0, 1, 0, AMBA_VIRT_CAP_PING);
    std::vector<struct amba_virt_dev_mem_desc> emptyDevs;
    auto devMemResp = makeDevMemResp(emptyDevs);
    uint32_t validSelfLen = static_cast<uint32_t>(kQueryHdrLen + sizeof(struct amba_virt_peer_desc));
    uint32_t validDevMemLen = static_cast<uint32_t>(kQueryHdrLen);

    const int errCodes[] = { -ENOENT, -EIO, -ETIMEDOUT };
    GuestProperties props;

    for (int code : errCodes) {
        // openRc failure (also test first negative rc wins when connectRc is also negative)
        int rc = GuestPropsService::Decode(code, -EIO, 1024, 0, selfResp, validSelfLen,
                                           0, devMemResp, validDevMemLen, props);
        LONGS_EQUAL(code, rc);
        CHECK_FALSE(props.online);
        LONGS_EQUAL(code, props.query_status);
        CHECK(props.error.find("openDevice:") == 0);

        // connectRc failure
        rc = GuestPropsService::Decode(0, code, 1024, 0, selfResp, validSelfLen,
                                       0, devMemResp, validDevMemLen, props);
        LONGS_EQUAL(code, rc);
        CHECK_FALSE(props.online);
        CHECK(props.error.find("connect:") == 0);

        // selfRc failure
        rc = GuestPropsService::Decode(0, 0, 1024, code, selfResp, validSelfLen,
                                       0, devMemResp, validDevMemLen, props);
        LONGS_EQUAL(code, rc);
        CHECK_FALSE(props.online);
        CHECK(props.error.find("querySelf:") == 0);

        // devMemRc failure
        rc = GuestPropsService::Decode(0, 0, 1024, 0, selfResp, validSelfLen,
                                       code, devMemResp, validDevMemLen, props);
        LONGS_EQUAL(code, rc);
        CHECK_FALSE(props.online);
        CHECK(props.error.find("queryDevMem:") == 0);
    }

    // resp.status = -EPERM on QUERY_SELF
    auto selfPerm = makeSelfResp(1, 0, 1, 0, AMBA_VIRT_CAP_PING, -EPERM);
    int rc = GuestPropsService::Decode(0, 0, 1024, 0, selfPerm, validSelfLen,
                                       0, devMemResp, validDevMemLen, props);
    LONGS_EQUAL(-EPERM, rc);
    CHECK_FALSE(props.online);
    CHECK(props.error.find("querySelf:") == 0);

    // resp.status = -EPERM on QUERY_DEV_MEM
    auto devMemPerm = makeDevMemResp(emptyDevs, -EPERM);
    rc = GuestPropsService::Decode(0, 0, 1024, 0, selfResp, validSelfLen,
                                   0, devMemPerm, validDevMemLen, props);
    LONGS_EQUAL(-EPERM, rc);
    CHECK_FALSE(props.online);
    CHECK(props.error.find("queryDevMem:") == 0);
}

TEST(GuestProps, CapabilitiesSingleAllAndUnknownBits)
{
    std::vector<struct amba_virt_dev_mem_desc> emptyDevs;
    auto devMemResp = makeDevMemResp(emptyDevs);
    uint32_t validSelfLen = static_cast<uint32_t>(kQueryHdrLen + sizeof(struct amba_virt_peer_desc));
    uint32_t validDevMemLen = static_cast<uint32_t>(kQueryHdrLen);

    // 1. Every capability bit alone
    for (size_t i = 0; i < 15; ++i) {
        uint32_t bit = kAmbaVirtCapTable[i].bit;
        auto selfResp = makeSelfResp(1, 0, 1, 0, bit);
        GuestProperties props;
        int rc = GuestPropsService::Decode(0, 0, 1024, 0, selfResp, validSelfLen,
                                           0, devMemResp, validDevMemLen, props);
        LONGS_EQUAL(0, rc);
        LONGS_EQUAL(1U, props.cap_names.size());
        STRCMP_EQUAL(kAmbaVirtCapTable[i].name, props.cap_names[0].c_str());
        LONGS_EQUAL(0U, props.caps_unknown);
    }

    // 2. All 15 capability bits together
    auto selfAll = makeSelfResp(1, 0, 1, 0, kAmbaVirtKnownCapsMask);
    GuestProperties propsAll;
    int rc = GuestPropsService::Decode(0, 0, 1024, 0, selfAll, validSelfLen,
                                       0, devMemResp, validDevMemLen, propsAll);
    LONGS_EQUAL(0, rc);
    LONGS_EQUAL(15U, propsAll.cap_names.size());
    LONGS_EQUAL(0U, propsAll.caps_unknown);
    STRCMP_EQUAL("PING", propsAll.cap_names[0].c_str());
    STRCMP_EQUAL("DMA_SLAVE", propsAll.cap_names[13].c_str());
    STRCMP_EQUAL("IAV_DEV", propsAll.cap_names[14].c_str());

    // 3. Undefined capability bit alongside a known bit
    auto selfUnknown = makeSelfResp(1, 0, 1, 0, AMBA_VIRT_CAP_PING | 0x80010000U);
    GuestProperties propsUnknown;
    rc = GuestPropsService::Decode(0, 0, 1024, 0, selfUnknown, validSelfLen,
                                   0, devMemResp, validDevMemLen, propsUnknown);
    LONGS_EQUAL(0, rc);
    LONGS_EQUAL(1U, propsUnknown.cap_names.size());
    STRCMP_EQUAL("PING", propsUnknown.cap_names[0].c_str());
    LONGS_EQUAL(0x80010000U, propsUnknown.caps_unknown);
}

TEST(GuestProps, JsonSerializationAndEscaping)
{
    GuestProperties props;
    props.online = true;
    props.query_status = 0;
    props.error = "";
    props.tenant_cid = 9;
    props.tenant_idx = 2;
    props.caps = AMBA_VIRT_CAP_PING | AMBA_VIRT_CAP_IAV_DEV;
    props.cap_names = { "PING", "IAV_DEV" };
    props.caps_unknown = 0x00008000U;
    props.bar_size_bytes = 1048576U;
    props.partition_bytes = 524288ULL;
    props.server_allocated_mb = 1U;

    GuestMapExtent ext;
    ext.partition = true;
    ext.usage = std::string("quote\"slash\\nl\nctrl") + static_cast<char>(0x01);
    ext.start_offset = 0;
    ext.end_offset = 524288ULL;
    ext.clipped = false;
    ext.overlaps = false;
    props.extents.push_back(ext);
    props.layout_text = "line1\nline2\t\"end\"";

    std::string json = GuestPropsService::ToJson(props);
    CHECK(json.find("\"online\": true") != std::string::npos);
    CHECK(json.find("\"caps_hex\": \"0x00004001\"") != std::string::npos);
    CHECK(json.find("\"caps_unknown_hex\": \"0x00008000\"") != std::string::npos);
    CHECK(json.find("\"usage\": \"quote\\\"slash\\\\nl\\nctrl\\u0001\"") != std::string::npos);
    CHECK(json.find("\"layout_text\": \"line1\\nline2\\t\\\"end\\\"\"") != std::string::npos);
}

TEST(GuestProps, QueryNonexistentDevicePath)
{
    GuestProperties props;
    int rc = GuestPropsService::Query(props, "/nonexistent-dir/amba_virt");
    CHECK(rc < 0);
    CHECK_FALSE(props.online);
    LONGS_EQUAL(rc, props.query_status);
    CHECK(props.error.find("openDevice:") == 0);
}

TEST(GuestProps, CliAndDemoLayoutTextParityAndLegacyComparison)
{
    const uint32_t barSize = 1073737728U;

    struct amba_virt_dev_mem_desc cav{};
    cav.dev_type = AMBA_VIRT_DEV_TYPE_CAVALRY;
    cav.base_offset = 2097152U;
    cav.size = 1040187392U;
    cav.rpc_arena_offset = 0U;
    cav.rpc_arena_size = 1048576U;

    std::vector<struct amba_virt_dev_mem_desc> devs = { cav };
    auto devMemResp = makeDevMemResp(devs);

    // 1. Without DMA_SLAVE / IAV_DEV: legacy and new AmbaVirtLayout output are 100% identical
    const uint32_t legacyCaps = 0x00000bd3U;
    auto selfLegacy = makeSelfResp(9, 2, 1, 993, legacyCaps);
    struct amba_virt_peer_desc peerLegacy{};
    std::memcpy(&peerLegacy, selfLegacy.payload, sizeof(peerLegacy));

    GuestProperties propsLegacy;
    int rc = GuestPropsService::Decode(
        0, 0, barSize,
        0, selfLegacy, static_cast<uint32_t>(kQueryHdrLen + sizeof(struct amba_virt_peer_desc)),
        0, devMemResp, static_cast<uint32_t>(kQueryHdrLen + sizeof(cav)),
        propsLegacy);
    LONGS_EQUAL(0, rc);

    std::string oldText1 = legacyFormatLayout(peerLegacy, devs, barSize);
    std::string newText1 = ambaVirtFormatLayout(peerLegacy, devs, barSize);
    STRCMP_EQUAL(oldText1.c_str(), newText1.c_str());
    STRCMP_EQUAL(newText1.c_str(), propsLegacy.layout_text.c_str());

    // 2. With DMA_SLAVE | IAV_DEV set (0x00006bd3): identical except the Capabilities line
    const uint32_t fullCaps = 0x00006bd3U;
    auto selfFull = makeSelfResp(9, 2, 1, 993, fullCaps);
    struct amba_virt_peer_desc peerFull{};
    std::memcpy(&peerFull, selfFull.payload, sizeof(peerFull));

    GuestProperties propsFull;
    rc = GuestPropsService::Decode(
        0, 0, barSize,
        0, selfFull, static_cast<uint32_t>(kQueryHdrLen + sizeof(struct amba_virt_peer_desc)),
        0, devMemResp, static_cast<uint32_t>(kQueryHdrLen + sizeof(cav)),
        propsFull);
    LONGS_EQUAL(0, rc);

    std::string oldText2 = legacyFormatLayout(peerFull, devs, barSize);
    std::string newText2 = ambaVirtFormatLayout(peerFull, devs, barSize);
    STRCMP_EQUAL(newText2.c_str(), propsFull.layout_text.c_str());

    // Replace the capabilities line in oldText2 with the new 15-cap list and verify exact match
    std::string oldCapLine =
        " Capabilities:     0x00006bd3 [PING, QUERY_SELF, MEM_ALLOC, DEV_CONFIG, GDMA_COPY, GDMA_PITCH, CAVALRY_PATH_B, CAVALRY_REGISTER]\n";
    std::string newCapLine =
        " Capabilities:     0x00006bd3 [PING, QUERY_SELF, MEM_ALLOC, DEV_CONFIG, GDMA_COPY, GDMA_PITCH, CAVALRY_PATH_B, CAVALRY_REGISTER, DMA_SLAVE, IAV_DEV]\n";

    size_t pos = oldText2.find(oldCapLine);
    CHECK(pos != std::string::npos);
    std::string patchedOldText2 = oldText2;
    patchedOldText2.replace(pos, oldCapLine.size(), newCapLine);
    STRCMP_EQUAL(patchedOldText2.c_str(), newText2.c_str());
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
