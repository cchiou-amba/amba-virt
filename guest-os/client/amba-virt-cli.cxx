/*
 * amba-virt-cli.cxx
 *
 * Userspace CLI tool for Ambarella Virtualization Guest Memory Layout & Introspection.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include "amba_virt.h"
#include "amba_virt_dev.hxx"

namespace {

const char *getDevTypeName(uint32_t devType) {
    switch (devType) {
    case AMBA_VIRT_DEV_TYPE_CAVALRY: return "CAVALRY (NPU)";
    case AMBA_VIRT_DEV_TYPE_GDMA:    return "GDMA";
    case AMBA_VIRT_DEV_TYPE_IAV:     return "IAV";
    case AMBA_VIRT_DEV_TYPE_SCRATCH: return "SCRATCH";
    default:                         return "UNKNOWN";
    }
}

std::string formatCaps(uint32_t caps) {
    std::vector<std::string> names;
    if (caps & AMBA_VIRT_CAP_PING)           names.push_back("PING");
    if (caps & AMBA_VIRT_CAP_QUERY_SELF)     names.push_back("QUERY_SELF");
    if (caps & AMBA_VIRT_CAP_QUERY_PEERS)    names.push_back("QUERY_PEERS");
    if (caps & AMBA_VIRT_CAP_QUERY_TOPO)     names.push_back("QUERY_TOPO");
    if (caps & AMBA_VIRT_CAP_MEM_ALLOC)      names.push_back("MEM_ALLOC");
    if (caps & AMBA_VIRT_CAP_MEM_RESIZE)     names.push_back("MEM_RESIZE");
    if (caps & AMBA_VIRT_CAP_DEV_CONFIG)     names.push_back("DEV_CONFIG");
    if (caps & AMBA_VIRT_CAP_GDMA_COPY)      names.push_back("GDMA_COPY");
    if (caps & AMBA_VIRT_CAP_GDMA_PITCH)     names.push_back("GDMA_PITCH");
    if (caps & AMBA_VIRT_CAP_CAVALRY_PATH_B) names.push_back("CAVALRY_PATH_B");
    if (caps & AMBA_VIRT_CAP_CAVALRY_PATH_A) names.push_back("CAVALRY_PATH_A");
    if (caps & AMBA_VIRT_CAP_CAVALRY_REGISTER) names.push_back("CAVALRY_REGISTER");
    if (caps & AMBA_VIRT_CAP_IAV_STREAM)     names.push_back("IAV_STREAM");

    if (names.empty())
        return "NONE";

    std::ostringstream oss;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i > 0) oss << ", ";
        oss << names[i];
    }
    return oss.str();
}

std::string toHex(uint32_t val) {
    std::ostringstream oss;
    oss << "0x" << std::hex << std::setw(8) << std::setfill('0') << val;
    return oss.str();
}

std::string toMb(uint32_t bytes) {
    std::ostringstream oss;
    oss << (bytes / (1024 * 1024)) << " MB";
    return oss.str();
}

void showLayout(AmbaVirtDevice &dev, bool json) {
    struct amba_virt_peer_desc selfDesc;
    memset(&selfDesc, 0, sizeof(selfDesc));
    int ret = dev.querySelf(selfDesc);
    if (ret < 0) {
        std::cerr << "Error: querySelf failed (" << ret << ")\n";
        return;
    }

    std::vector<struct amba_virt_dev_mem_desc> devs;
    ret = dev.queryDevMem(devs);
    if (ret < 0) {
        std::cerr << "Error: queryDevMem failed (" << ret << ")\n";
        return;
    }

    uint32_t barSize = dev.getShmSize();
    uint64_t totalAllocatedBytes = 0;

    // Sort devices by base offset
    std::sort(devs.begin(), devs.end(), [](const struct amba_virt_dev_mem_desc &a,
                                           const struct amba_virt_dev_mem_desc &b) {
        return a.base_offset < b.base_offset;
    });

    for (const auto &d : devs) {
        totalAllocatedBytes += d.size;
        if (d.rpc_arena_size > 0) {
            totalAllocatedBytes += d.rpc_arena_size;
        }
    }

    if (json) {
        std::cout << "{\n"
                  << "  \"tenant_cid\": " << selfDesc.cid << ",\n"
                  << "  \"status\": " << selfDesc.status << ",\n"
                  << "  \"caps\": \"" << toHex(selfDesc.caps) << "\",\n"
                  << "  \"bar_size_bytes\": " << barSize << ",\n"
                  << "  \"allocated_bytes\": " << totalAllocatedBytes << ",\n"
                  << "  \"free_bytes\": " << (barSize > totalAllocatedBytes ? (barSize - totalAllocatedBytes) : 0) << ",\n"
                  << "  \"devices\": [\n";

        for (size_t i = 0; i < devs.size(); ++i) {
            const auto &d = devs[i];
            std::cout << "    {\n"
                      << "      \"dev_type\": \"" << getDevTypeName(d.dev_type) << "\",\n"
                      << "      \"dev_id\": " << d.dev_type << ",\n"
                      << "      \"base_offset\": " << d.base_offset << ",\n"
                      << "      \"base_offset_hex\": \"" << toHex(d.base_offset) << "\",\n"
                      << "      \"size_bytes\": " << d.size << ",\n"
                      << "      \"size_mb\": " << (d.size / (1024 * 1024)) << ",\n"
                      << "      \"rpc_arena_offset\": " << d.rpc_arena_offset << ",\n"
                      << "      \"rpc_arena_size\": " << d.rpc_arena_size << "\n"
                      << "    }" << (i + 1 < devs.size() ? "," : "") << "\n";
        }
        std::cout << "  ]\n}\n";
        return;
    }

    double allocMb = static_cast<double>(totalAllocatedBytes) / (1024.0 * 1024.0);
    double barMb = static_cast<double>(barSize) / (1024.0 * 1024.0);
    double utilPct = (barMb > 0.0) ? (allocMb / barMb * 100.0) : 0.0;

    std::cout << "================================================================================\n"
              << " Ambarella Virtualization Guest Memory Layout (/dev/amba_virt)\n"
              << "================================================================================\n"
              << " Tenant CID:       " << selfDesc.cid << "\n"
              << " Status:           " << (selfDesc.status == 1 ? "ONLINE" : "OFFLINE") << "\n"
              << " ivshmem BAR Size: " << std::fixed << std::setprecision(0) << barMb << " MB ("
              << toHex(barSize) << ")\n"
              << " Memory In Use:    " << std::fixed << std::setprecision(1) << allocMb << " MB / "
              << barMb << " MB (" << std::setprecision(1) << utilPct << "% utilized, "
              << (barMb - allocMb) << " MB free)\n"
              << " Capabilities:     " << toHex(selfDesc.caps) << " [" << formatCaps(selfDesc.caps) << "]\n"
              << "--------------------------------------------------------------------------------\n"
              << " Virtual Device Partitions:\n"
              << "--------------------------------------------------------------------------------\n"
              << " Dev ID  Device Type    BAR Offset      End Offset      Size       RPC Arena\n";

    if (devs.empty()) {
        std::cout << " (no active virtual device partitions registered)\n";
    } else {
        for (const auto &d : devs) {
            uint32_t endOff = d.base_offset + d.size;
            std::cout << " [" << d.dev_type << "]     "
                      << std::left << std::setw(14) << std::setfill(' ') << getDevTypeName(d.dev_type) << " "
                      << toHex(d.base_offset) << "      "
                      << toHex(endOff) << "      "
                      << std::right << std::setw(8) << std::setfill(' ') << toMb(d.size) << "   ";
            if (d.rpc_arena_size > 0) {
                std::cout << toHex(d.rpc_arena_offset)
                          << " (" << (d.rpc_arena_size / 1024) << " KB)\n";
            } else {
                std::cout << "N/A\n";
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
        e.name = std::string(getDevTypeName(d.dev_type)) + " Buffer";
        extents.push_back(e);

        if (d.rpc_arena_size > 0) {
            Extent a;
            a.start = d.rpc_arena_offset;
            a.end = d.rpc_arena_offset + d.rpc_arena_size;
            a.name = std::string(getDevTypeName(d.dev_type)) + " RPC Arena";
            extents.push_back(a);
        }
    }

    std::sort(extents.begin(), extents.end(), [](const Extent &a, const Extent &b) {
        return a.start < b.start;
    });

    std::cout << "--------------------------------------------------------------------------------\n"
              << " Memory Extent Map:\n"
              << " Status  Usage                  Start Offset    End Offset      Size\n";

    uint32_t cur = 0;
    for (const auto &e : extents) {
        if (e.start > cur) {
            uint32_t freeSz = e.start - cur;
            std::cout << " [FREE]  UNALLOCATED            "
                      << toHex(cur) << "      "
                      << toHex(e.start) << "      "
                      << std::right << std::setw(8) << std::setfill(' ') << toMb(freeSz) << "\n";
        }
        uint32_t sz = e.end - e.start;
        std::cout << " [ALLOC] "
                  << std::left << std::setw(22) << std::setfill(' ') << e.name << " "
                  << toHex(e.start) << "      "
                  << toHex(e.end) << "      "
                  << std::right << std::setw(8) << std::setfill(' ') << toMb(sz) << "\n";
        cur = (e.end > cur) ? e.end : cur;
    }
    if (cur < barSize) {
        uint32_t freeSz = barSize - cur;
        std::cout << " [FREE]  UNALLOCATED            "
                  << toHex(cur) << "      "
                  << toHex(barSize) << "      "
                  << std::right << std::setw(8) << std::setfill(' ') << toMb(freeSz) << "\n";
    }
    std::cout << "================================================================================\n";
}

void showInfo(AmbaVirtDevice &dev, bool json) {
    struct amba_virt_peer_desc selfDesc;
    memset(&selfDesc, 0, sizeof(selfDesc));
    int ret = dev.querySelf(selfDesc);
    if (ret < 0) {
        std::cerr << "Error: querySelf failed (" << ret << ")\n";
        return;
    }

    const auto &info = dev.getInfo();

    if (json) {
        std::cout << "{\n"
                  << "  \"cid\": " << selfDesc.cid << ",\n"
                  << "  \"tenant_idx\": " << selfDesc.tenant_idx << ",\n"
                  << "  \"status\": " << selfDesc.status << ",\n"
                  << "  \"mem_allocated_mb\": " << selfDesc.mem_allocated_mb << ",\n"
                  << "  \"caps\": \"0x" << std::hex << std::setw(8) << std::setfill('0') << selfDesc.caps << std::dec << "\",\n"
                  << "  \"vsock_port\": " << info.vsock_port << ",\n"
                  << "  \"protocol_version\": " << info.proto << ",\n"
                  << "  \"bar_size_bytes\": " << info.shm_size << "\n"
                  << "}\n";
        return;
    }

    std::cout << "Ambarella Virtualization Guest Tenant Status:\n"
              << "  Tenant CID:         " << selfDesc.cid << "\n"
              << "  Tenant Index:       " << selfDesc.tenant_idx << "\n"
              << "  Tenant Status:      " << (selfDesc.status == 1 ? "ONLINE" : "OFFLINE") << "\n"
              << "  Memory Allocated:   " << selfDesc.mem_allocated_mb << " MB\n"
              << "  ivshmem BAR Size:   " << (info.shm_size / (1024 * 1024)) << " MB\n"
              << "  Vsock Server Port:  " << info.vsock_port << "\n"
              << "  Protocol Version:   " << info.proto << "\n"
              << "  Capabilities:       0x" << std::hex << std::setw(8) << std::setfill('0')
              << selfDesc.caps << std::dec << "\n"
              << "  Capability List:    " << formatCaps(selfDesc.caps) << "\n";
}

void showTopology(AmbaVirtDevice &dev, bool json) {
    struct amba_virt_topo_desc topo;
    memset(&topo, 0, sizeof(topo));
    int ret = dev.queryTopology(topo);
    if (ret < 0) {
        if (ret == -EPERM)
            std::cerr << "Error: Permission denied. Caller lacks QUERY_TOPO capability.\n";
        else
            std::cerr << "Error: queryTopology failed (" << ret << ")\n";
        return;
    }

    if (json) {
        std::cout << "{\n"
                  << "  \"chip_id\": \"0x" << std::hex << topo.chip_id << std::dec << "\",\n"
                  << "  \"npu_core_cnt\": " << topo.npu_core_cnt << ",\n"
                  << "  \"npu_freq_mhz\": " << topo.npu_freq_mhz << ",\n"
                  << "  \"gdma_channels\": " << topo.gdma_channels << ",\n"
                  << "  \"total_cvmem_mb\": " << topo.total_cvmem_mb << ",\n"
                  << "  \"host_phys_addr\": " << topo.host_phys_addr << "\n"
                  << "}\n";
        return;
    }

    std::cout << "Host Hardware Silicon Topology:\n"
              << "  SoC Chip ID:        0x" << std::hex << topo.chip_id << std::dec << " (Ambarella N1-655)\n"
              << "  VisORC NPU Cores:   " << topo.npu_core_cnt << " @ " << topo.npu_freq_mhz << " MHz\n"
              << "  GDMA Channels:      " << topo.gdma_channels << "\n"
              << "  Host CVMEM Size:    " << topo.total_cvmem_mb << " MB\n"
              << "  Host Physical Base: " << (topo.host_phys_addr == 0 ? "(masked for security)" : "0x...") << "\n";
}

void showPeers(AmbaVirtDevice &dev, bool json) {
    std::vector<struct amba_virt_peer_desc> peers;
    int ret = dev.queryPeers(peers);
    if (ret < 0) {
        if (ret == -EPERM)
            std::cerr << "Error: Permission denied. Caller lacks QUERY_PEERS capability.\n";
        else
            std::cerr << "Error: queryPeers failed (" << ret << ")\n";
        return;
    }

    if (json) {
        std::cout << "[\n";
        for (size_t i = 0; i < peers.size(); ++i) {
            const auto &p = peers[i];
            std::cout << "  {\n"
                      << "    \"cid\": " << p.cid << ",\n"
                      << "    \"tenant_idx\": " << p.tenant_idx << ",\n"
                      << "    \"status\": " << p.status << ",\n"
                      << "    \"mem_allocated_mb\": " << p.mem_allocated_mb << ",\n"
                      << "    \"caps\": \"0x" << std::hex << std::setw(8) << std::setfill('0') << p.caps << std::dec << "\"\n"
                      << "  }" << (i + 1 < peers.size() ? "," : "") << "\n";
        }
        std::cout << "]\n";
        return;
    }

    std::cout << "Registered Peer Tenants (" << peers.size() << " active):\n";
    std::cout << " CID     Tenant Index   Status     Allocated Memory   Capabilities\n";
    std::cout << "--------------------------------------------------------------------------------\n";
    for (const auto &p : peers) {
        std::cout << " " << std::left << std::setw(7) << p.cid << " "
                  << std::setw(14) << p.tenant_idx << " "
                  << std::setw(10) << (p.status == 1 ? "ONLINE" : "OFFLINE") << " "
                  << std::right << std::setw(5) << p.mem_allocated_mb << " MB            0x"
                  << std::hex << std::setw(8) << std::setfill('0') << p.caps << std::dec << "\n";
    }
}

void pingServer(AmbaVirtDevice &dev) {
    struct amba_virt_msg msg;
    memset(&msg, 0, sizeof(msg));
    msg.type = AMBA_VIRT_MSG_PING;
    msg.seq = 42;

    auto t0 = std::chrono::high_resolution_clock::now();
    if (dev.send(&msg, sizeof(msg)) < 0) {
        std::cerr << "Error: send PING failed\n";
        return;
    }

    struct amba_virt_msg resp;
    uint32_t rxLen = 0;
    if (dev.recv(&resp, sizeof(resp), rxLen, 3000) < 0) {
        std::cerr << "Error: recv PONG failed\n";
        return;
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    double rttUs = std::chrono::duration<double, std::micro>(t1 - t0).count();

    if (resp.type == AMBA_VIRT_MSG_PONG) {
        std::cout << "PONG received from amba-virt-server: seq=" << resp.seq
                  << " (RTT: " << std::fixed << std::setprecision(1) << rttUs << " us)\n";
    } else {
        std::cerr << "Unexpected response type: " << resp.type << "\n";
    }
}

void printUsage(const char *prog) {
    std::cout << "Usage: " << prog << " [command] [options]\n\n"
              << "Commands:\n"
              << "  layout, mem     Display virtual device memory layout and partition map (default)\n"
              << "  info, status    Display guest tenant identity, quota, and assigned capabilities\n"
              << "  topo, topology  Display physical Ambarella silicon accelerator topology\n"
              << "  peers           List registered peer tenant VMs (requires QUERY_PEERS capability)\n"
              << "  ping            Send vsock ping to amba-virt-server and measure round-trip time\n\n"
              << "Options:\n"
              << "  --json          Output structured JSON instead of formatted text\n"
              << "  -h, --help      Show this help message\n";
}

} // namespace

int main(int argc, char **argv) {
    std::string cmd = "layout";
    bool json = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h" || arg == "help") {
            printUsage(argv[0]);
            return 0;
        } else if (arg == "--json") {
            json = true;
        } else if (arg[0] != '-') {
            cmd = arg;
        }
    }

    AmbaVirtDevice dev;
    if (dev.openDevice() != 0) {
        std::cerr << "Error: Cannot open /dev/amba_virt (device missing or driver not loaded)\n";
        return 1;
    }
    if (dev.connect() != 0) {
        std::cerr << "Error: Cannot connect to amba-virt-server over vsock\n";
        return 1;
    }
    dev.flushRx();

    if (cmd == "layout" || cmd == "mem") {
        showLayout(dev, json);
    } else if (cmd == "info" || cmd == "status") {
        showInfo(dev, json);
    } else if (cmd == "topo" || cmd == "topology") {
        showTopology(dev, json);
    } else if (cmd == "peers") {
        showPeers(dev, json);
    } else if (cmd == "ping") {
        pingServer(dev);
    } else {
        std::cerr << "Unknown command: " << cmd << "\n\n";
        printUsage(argv[0]);
        return 1;
    }

    dev.closeDevice();
    return 0;
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
