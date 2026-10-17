/*
 * AmbaVirtLayout.hxx
 *
 * Shared layout formatter, device-type names, and capability table for
 * Ambarella Virtualization guest tools and demo services.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#ifndef AMBA_VIRT_LAYOUT_HXX
#define AMBA_VIRT_LAYOUT_HXX

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

#include "amba_virt.h"

struct AmbaVirtCapDef {
    uint32_t bit;
    const char *name;
};

inline constexpr AmbaVirtCapDef kAmbaVirtCapTable[] = {
    { AMBA_VIRT_CAP_PING,             "PING" },
    { AMBA_VIRT_CAP_QUERY_SELF,       "QUERY_SELF" },
    { AMBA_VIRT_CAP_QUERY_PEERS,      "QUERY_PEERS" },
    { AMBA_VIRT_CAP_QUERY_TOPO,       "QUERY_TOPO" },
    { AMBA_VIRT_CAP_MEM_ALLOC,        "MEM_ALLOC" },
    { AMBA_VIRT_CAP_MEM_RESIZE,       "MEM_RESIZE" },
    { AMBA_VIRT_CAP_DEV_CONFIG,       "DEV_CONFIG" },
    { AMBA_VIRT_CAP_GDMA_COPY,        "GDMA_COPY" },
    { AMBA_VIRT_CAP_GDMA_PITCH,       "GDMA_PITCH" },
    { AMBA_VIRT_CAP_CAVALRY_PATH_B,   "CAVALRY_PATH_B" },
    { AMBA_VIRT_CAP_CAVALRY_PATH_A,   "CAVALRY_PATH_A" },
    { AMBA_VIRT_CAP_CAVALRY_REGISTER, "CAVALRY_REGISTER" },
    { AMBA_VIRT_CAP_IAV_STREAM,       "IAV_STREAM" },
    { AMBA_VIRT_CAP_DMA_SLAVE,        "DMA_SLAVE" },
    { AMBA_VIRT_CAP_IAV_DEV,          "IAV_DEV" },
};

inline constexpr uint32_t kAmbaVirtKnownCapsMask = 0x00007FFFU;

inline const char *ambaVirtGetDevTypeName(uint32_t devType)
{
    switch (devType) {
    case AMBA_VIRT_DEV_TYPE_CAVALRY: return "CAVALRY (NPU)";
    case AMBA_VIRT_DEV_TYPE_GDMA:    return "GDMA";
    case AMBA_VIRT_DEV_TYPE_IAV:     return "IAV";
    case AMBA_VIRT_DEV_TYPE_SCRATCH: return "SCRATCH";
    default:                         return "UNKNOWN";
    }
}

inline void ambaVirtDecodeCaps(uint32_t caps,
                               std::vector<std::string> &outNames,
                               uint32_t &outUnknown)
{
    outNames.clear();
    for (const auto &entry : kAmbaVirtCapTable) {
        if ((caps & entry.bit) != 0U) {
            outNames.push_back(entry.name);
        }
    }
    outUnknown = caps & ~kAmbaVirtKnownCapsMask;
}

inline std::string ambaVirtFormatCaps(uint32_t caps)
{
    std::vector<std::string> names;
    uint32_t unknown = 0;
    ambaVirtDecodeCaps(caps, names, unknown);

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
}

inline std::string ambaVirtToHex(uint64_t val)
{
    std::ostringstream oss;
    oss << "0x" << std::hex << std::setw(8) << std::setfill('0') << val;
    return oss.str();
}

inline std::string ambaVirtToMb(uint64_t bytes)
{
    std::ostringstream oss;
    oss << (bytes / (1024ULL * 1024ULL)) << " MB";
    return oss.str();
}

inline std::string ambaVirtFormatLayout(
    const struct amba_virt_peer_desc &selfDesc,
    const std::vector<struct amba_virt_dev_mem_desc> &inDevs,
    uint32_t barSize)
{
    std::vector<struct amba_virt_dev_mem_desc> devs = inDevs;
    std::sort(devs.begin(), devs.end(),
              [](const struct amba_virt_dev_mem_desc &a,
                 const struct amba_virt_dev_mem_desc &b) {
                  return a.base_offset < b.base_offset;
              });

    uint64_t totalAllocatedBytes = 0;
    for (const auto &d : devs) {
        totalAllocatedBytes += d.size;
        if (d.rpc_arena_size > 0) {
            totalAllocatedBytes += d.rpc_arena_size;
        }
    }

    double allocMb = static_cast<double>(totalAllocatedBytes) / (1024.0 * 1024.0);
    double barMb = static_cast<double>(barSize) / (1024.0 * 1024.0);
    double utilPct = (barMb > 0.0) ? (allocMb / barMb * 100.0) : 0.0;

    std::ostringstream oss;
    oss << "================================================================================\n"
        << " Ambarella Virtualization Guest Memory Layout (/dev/amba_virt)\n"
        << "================================================================================\n"
        << " Tenant CID:       " << selfDesc.cid << "\n"
        << " Status:           " << (selfDesc.status == 1 ? "ONLINE" : "OFFLINE") << "\n"
        << " ivshmem BAR Size: " << std::fixed << std::setprecision(0) << barMb << " MB ("
        << ambaVirtToHex(barSize) << ")\n"
        << " Memory In Use:    " << std::fixed << std::setprecision(1) << allocMb << " MB / "
        << barMb << " MB (" << std::setprecision(1) << utilPct << "% utilized, "
        << (barMb - allocMb) << " MB free)\n"
        << " Capabilities:     " << ambaVirtToHex(selfDesc.caps)
        << " [" << ambaVirtFormatCaps(selfDesc.caps) << "]\n"
        << "--------------------------------------------------------------------------------\n"
        << " Virtual Device Partitions:\n"
        << "--------------------------------------------------------------------------------\n"
        << " Dev ID  Device Type    BAR Offset      End Offset      Size       RPC Arena\n";

    if (devs.empty()) {
        oss << " (no active virtual device partitions registered)\n";
    } else {
        for (const auto &d : devs) {
            uint64_t endOff = static_cast<uint64_t>(d.base_offset) +
                              static_cast<uint64_t>(d.size);
            oss << " [" << d.dev_type << "]     "
                << std::left << std::setw(14) << std::setfill(' ')
                << ambaVirtGetDevTypeName(d.dev_type) << " "
                << ambaVirtToHex(d.base_offset) << "      "
                << ambaVirtToHex(endOff) << "      "
                << std::right << std::setw(8) << std::setfill(' ')
                << ambaVirtToMb(d.size) << "   ";
            if (d.rpc_arena_size > 0) {
                oss << ambaVirtToHex(d.rpc_arena_offset)
                    << " (" << (d.rpc_arena_size / 1024) << " KB)\n";
            } else {
                oss << "N/A\n";
            }
        }
    }

    struct Extent {
        uint64_t start;
        uint64_t end;
        std::string name;
    };

    std::vector<Extent> extents;
    for (const auto &d : devs) {
        Extent e;
        e.start = d.base_offset;
        e.end = static_cast<uint64_t>(d.base_offset) + static_cast<uint64_t>(d.size);
        e.name = std::string(ambaVirtGetDevTypeName(d.dev_type)) + " Buffer";
        extents.push_back(e);

        if (d.rpc_arena_size > 0) {
            Extent a;
            a.start = d.rpc_arena_offset;
            a.end = static_cast<uint64_t>(d.rpc_arena_offset) +
                    static_cast<uint64_t>(d.rpc_arena_size);
            a.name = std::string(ambaVirtGetDevTypeName(d.dev_type)) + " RPC Arena";
            extents.push_back(a);
        }
    }

    std::sort(extents.begin(), extents.end(), [](const Extent &a, const Extent &b) {
        return a.start < b.start;
    });

    oss << "--------------------------------------------------------------------------------\n"
        << " Memory Extent Map:\n"
        << " Status  Usage                  Start Offset    End Offset      Size\n";

    uint64_t cur = 0;
    for (const auto &e : extents) {
        if (e.start > cur) {
            uint64_t freeSz = e.start - cur;
            oss << " [FREE]  UNALLOCATED            "
                << ambaVirtToHex(cur) << "      "
                << ambaVirtToHex(e.start) << "      "
                << std::right << std::setw(8) << std::setfill(' ')
                << ambaVirtToMb(freeSz) << "\n";
        }
        uint64_t sz = e.end - e.start;
        oss << " [ALLOC] "
            << std::left << std::setw(22) << std::setfill(' ') << e.name << " "
            << ambaVirtToHex(e.start) << "      "
            << ambaVirtToHex(e.end) << "      "
            << std::right << std::setw(8) << std::setfill(' ')
            << ambaVirtToMb(sz) << "\n";
        cur = (e.end > cur) ? e.end : cur;
    }
    if (cur < static_cast<uint64_t>(barSize)) {
        uint64_t freeSz = static_cast<uint64_t>(barSize) - cur;
        oss << " [FREE]  UNALLOCATED            "
            << ambaVirtToHex(cur) << "      "
            << ambaVirtToHex(barSize) << "      "
            << std::right << std::setw(8) << std::setfill(' ')
            << ambaVirtToMb(freeSz) << "\n";
    }
    oss << "================================================================================\n";

    return oss.str();
}

#endif /* AMBA_VIRT_LAYOUT_HXX */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
