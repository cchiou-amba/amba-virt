/*
 * GuestProps.hxx
 *
 * Guest properties data model and service interface for amba-virt-demo.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#ifndef AMBA_VIRT_DEMO_GUEST_PROPS_HXX
#define AMBA_VIRT_DEMO_GUEST_PROPS_HXX

#include <cstdint>
#include <string>
#include <vector>

#include "amba_virt.h"

struct GuestDevicePartition {
    uint32_t dev_type{0};
    std::string dev_type_name;      // "CAVALRY (NPU)", "GDMA", "IAV", "SCRATCH", "UNKNOWN"
    uint32_t base_offset{0};
    uint64_t end_offset{0};         // base_offset + size, 64-bit
    uint32_t size_bytes{0};
    uint32_t rpc_arena_offset{0};
    uint32_t rpc_arena_size{0};
    uint32_t flags{0};
};

struct GuestMapExtent {
    bool partition{false};          // false: no device partition covers this range
    std::string usage;              // "<type> Buffer", "<type> RPC Arena", or "NO PARTITION"
    uint64_t start_offset{0};
    uint64_t end_offset{0};         // clipped to bar_size_bytes
    bool clipped{false};            // the descriptor ran past the BAR end
    bool overlaps{false};           // shares bytes with another partition extent
};

struct GuestProperties {
    int query_status{0};            // 0, or the negative errno of the failed step
    std::string error;              // empty when query_status == 0
    uint32_t tenant_cid{0};
    uint32_t tenant_idx{0};
    bool online{false};             // true only when both queries succeeded (G4)
    uint32_t caps{0};
    std::vector<std::string> cap_names;
    uint32_t caps_unknown{0};       // bits outside the 15 defined capabilities
    uint32_t bar_size_bytes{0};
    uint64_t partition_bytes{0};    // union of partition and arena ranges inside the BAR
    uint32_t server_allocated_mb{0};// QUERY_SELF mem_allocated_mb, includes MEM_ALLOC extents
    std::vector<GuestDevicePartition> devices;
    std::vector<GuestMapExtent> extents;
    std::string layout_text;        // identical to `amba-virt-cli layout` for the same data
};

class GuestPropsService {
public:
    // Device I/O, then Decode(). Never throws; failures are in out.query_status / out.error.
    static int Query(GuestProperties &out, const char *devPath = "/dev/amba_virt");

    // Pure. rc values are the return codes of the preceding steps; the responses are
    // what rpcTransaction() returned, with their received payload lengths.
    static int Decode(int openRc, int connectRc, uint32_t barSize,
                      int selfRc, const struct amba_virt_query_resp &self, uint32_t selfLen,
                      int devMemRc, const struct amba_virt_query_resp &devMem, uint32_t devMemLen,
                      GuestProperties &out);

    static std::string ToJson(const GuestProperties &props);
};

std::string GuestPropsHttpResponse(const GuestProperties &props);

#endif /* AMBA_VIRT_DEMO_GUEST_PROPS_HXX */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
