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
#include "AmbaVirtLayout.hxx"

namespace {

const char *getDevTypeName(uint32_t devType) {
    return ambaVirtGetDevTypeName(devType);
}

std::string formatCaps(uint32_t caps) {
    return ambaVirtFormatCaps(caps);
}

std::string toHex(uint32_t val) {
    return ambaVirtToHex(val);
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

    std::cout << ambaVirtFormatLayout(selfDesc, devs, barSize);
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
                  << "  \"bar_size_bytes\": " << info.shm_size << ",\n"
                  << "  \"bar_size_mb\": " << (info.shm_size / (1024 * 1024)) << "\n"
                  << "}\n";
        return;
    }

    std::cout << "Ambarella Virtualization Guest Tenant Status:\n"
              << "  Tenant CID:         " << selfDesc.cid << "\n"
              << "  Slice Index:        " << selfDesc.tenant_idx << "\n"
              << "  Tenant Status:      " << (selfDesc.status == 1 ? "ONLINE" : "OFFLINE") << "\n"
              << "  Memory Allocated:   " << selfDesc.mem_allocated_mb << " MB\n"
              << "  Slice Usable Size:  " << info.shm_size << " bytes (" << (info.shm_size / (1024 * 1024)) << " MB)\n"
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

void showDrivers(AmbaVirtDevice &dev, bool json) {
    struct amba_virt_driver_caps_resp caps;
    memset(&caps, 0, sizeof(caps));
    int ret = dev.queryDriverCaps(caps);
    if (ret < 0) {
        std::cerr << "Error: queryDriverCaps failed (" << ret << ")\n";
        return;
    }

    if (json) {
        std::cout << "{\n"
                  << "  \"host_mod_mask\": \"" << toHex(caps.host_mod_mask) << "\",\n"
                  << "  \"drivers\": [\n";
        for (uint32_t i = 0; i < caps.count; ++i) {
            const auto &e = caps.entries[i];
            std::cout << "    {\n"
                      << "      \"dev_name\": \"" << e.dev_name << "\",\n"
                      << "      \"dev_id\": " << e.dev_id << ",\n"
                      << "      \"state\": \"" << (e.state == AMBA_VIRT_DEV_STATE_ONLINE ? "ONLINE" : "OFFLINE") << "\",\n"
                      << "      \"module_mask\": \"" << toHex(e.module_mask) << "\"\n"
                      << "    }" << (i + 1 < caps.count ? "," : "") << "\n";
        }
        std::cout << "  ]\n}\n";
        return;
    }

    std::cout << "Ambarella Virtual Driver Capabilities & Availability:\n";
    std::cout << " Host Module Mask: " << toHex(caps.host_mod_mask) << "\n";
    std::cout << " Device Node      ID   Status     Module Mask\n";
    std::cout << "--------------------------------------------------------------------------------\n";
    for (uint32_t i = 0; i < caps.count; ++i) {
        const auto &e = caps.entries[i];
        std::cout << " " << std::left << std::setw(16) << e.dev_name << " "
                  << std::setw(4) << e.dev_id << " "
                  << std::setw(10) << (e.state == AMBA_VIRT_DEV_STATE_ONLINE ? "ONLINE" : "OFFLINE") << " "
                  << toHex(e.module_mask) << "\n";
    }
}

void showModules(AmbaVirtDevice &dev, bool json) {
    struct amba_virt_driver_caps_resp caps;
    memset(&caps, 0, sizeof(caps));
    int ret = dev.queryDriverCaps(caps);
    if (ret < 0) {
        std::cerr << "Error: queryDriverCaps failed (" << ret << ")\n";
        return;
    }

    if (json) {
        std::cout << "{\n"
                  << "  \"host_mod_mask\": \"" << toHex(caps.host_mod_mask) << "\"\n"
                  << "}\n";
        return;
    }

    std::cout << "Host Module Inventory (Bitmask: " << toHex(caps.host_mod_mask) << ")\n";
    std::cout << "  ambcma.ko:       " << ((caps.host_mod_mask & (1U << 0)) ? "LOADED" : "ABSENT") << "\n";
    std::cout << "  cavalry.ko:      " << ((caps.host_mod_mask & (1U << 1)) ? "LOADED" : "ABSENT") << "\n";
    std::cout << "  dsp.ko:          " << ((caps.host_mod_mask & (1U << 2)) ? "LOADED" : "ABSENT") << "\n";
    std::cout << "  iav.ko:          " << ((caps.host_mod_mask & (1U << 3)) ? "LOADED" : "ABSENT") << "\n";
    std::cout << "  amba_virt.ko:    " << ((caps.host_mod_mask & (1U << 4)) ? "LOADED" : "ABSENT") << "\n";
    std::cout << "  amba_otp.ko:     " << ((caps.host_mod_mask & (1U << 5)) ? "LOADED" : "ABSENT") << "\n";
}

void monitorEvents(AmbaVirtDevice &dev) {
    std::cout << "Monitoring asynchronous hardware state events from host (Ctrl+C to stop)...\n";
    for (;;) {
        uint8_t rxBuf[512];
        uint32_t rxLen = 0;
        int ret = dev.recv(rxBuf, sizeof(rxBuf), rxLen, 10000);
        if (ret < 0) {
            if (ret == -ETIMEDOUT)
                continue;
            std::cerr << "Error receiving event (" << ret << ")\n";
            break;
        }

        if (rxLen >= sizeof(struct amba_virt_msg)) {
            const struct amba_virt_msg *msg = reinterpret_cast<const struct amba_virt_msg *>(rxBuf);
            if (msg->type == AMBA_VIRT_MSG_DEV_STATE_EVENT &&
                rxLen >= sizeof(struct amba_virt_msg) + sizeof(struct amba_virt_dev_state_event)) {
                const struct amba_virt_dev_state_event *evt =
                    reinterpret_cast<const struct amba_virt_dev_state_event *>(rxBuf + sizeof(struct amba_virt_msg));
                std::cout << "[EVENT] dev_id=" << evt->dev_id << " (" << getDevTypeName(evt->dev_id) << ") "
                          << "state=" << (evt->state == AMBA_VIRT_DEV_STATE_ONLINE ? "ONLINE" : "OFFLINE") << " "
                          << "reason=" << evt->reason_code << " "
                          << "host_mask=" << toHex(evt->host_mod_mask) << " "
                          << "timestamp_ns=" << evt->timestamp_ns << "\n";
            }
        }
    }
}

void printUsage(const char *prog) {
    std::cout << "Usage: " << prog << " [command] [options]\n\n"
              << "Commands:\n"
              << "  layout, mem     Display virtual device memory layout and partition map (default)\n"
              << "  drivers, caps   Display virtual driver availability and host readiness state\n"
              << "  modules, kmods  Display host kernel module inventory bitmask\n"
              << "  monitor, events Stream real-time hardware state change events asynchronously\n"
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
    } else if (cmd == "drivers" || cmd == "caps") {
        showDrivers(dev, json);
    } else if (cmd == "modules" || cmd == "kmods") {
        showModules(dev, json);
    } else if (cmd == "monitor" || cmd == "events") {
        monitorEvents(dev);
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
