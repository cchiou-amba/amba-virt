/*
 * GuestProps.cxx
 *
 * In-process guest properties query, decoder, extent map builder, and JSON
 * serializer for amba-virt-demo.
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include "GuestProps.hxx"

#include <algorithm>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <sstream>

#include "AmbaVirtLayout.hxx"
#include "amba_virt_dev.hxx"

namespace {

constexpr size_t kQueryRespHdrLen = offsetof(struct amba_virt_query_resp, payload);

int failStep(GuestProperties &out, const char *step, int rc)
{
    int errCode = (rc < 0) ? rc : -EIO;
    out.query_status = errCode;
    out.online = false;
    const char *errStr = std::strerror(-errCode);
    out.error = std::string(step) + ": " + (errStr ? errStr : "unknown error");
    return errCode;
}

std::string escapeJsonString(const std::string &input)
{
    std::ostringstream oss;
    for (unsigned char c : input) {
        switch (c) {
        case '"':
            oss << "\\\"";
            break;
        case '\\':
            oss << "\\\\";
            break;
        case '\n':
            oss << "\\n";
            break;
        case '\r':
            oss << "\\r";
            break;
        case '\t':
            oss << "\\t";
            break;
        default:
            if (c < 0x20U) {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\u%04x", static_cast<unsigned int>(c));
                oss << buf;
            } else {
                oss << static_cast<char>(c);
            }
            break;
        }
    }
    return oss.str();
}

} // namespace

int GuestPropsService::Query(GuestProperties &out, const char *devPath)
{
    const char *effectivePath = (devPath && devPath[0] != '\0') ? devPath : AMBA_VIRT_DEV_PATH;
    AmbaVirtDevice dev(effectivePath);

    int openRc = dev.openDevice();
    int connectRc = 0;
    uint32_t barSize = 0;

    int selfRc = 0;
    struct amba_virt_query_resp selfResp;
    std::memset(&selfResp, 0, sizeof(selfResp));
    uint32_t selfLen = 0;

    int devMemRc = 0;
    struct amba_virt_query_resp devMemResp;
    std::memset(&devMemResp, 0, sizeof(devMemResp));
    uint32_t devMemLen = 0;

    if (openRc == 0) {
        barSize = dev.getShmSize();
        connectRc = dev.connect();
        if (connectRc == 0) {
            dev.flushRx();

            struct amba_virt_query_req selfReq;
            std::memset(&selfReq, 0, sizeof(selfReq));
            selfReq.query_op = AMBA_VIRT_QUERY_SELF;
            selfRc = dev.rpcTransaction(AMBA_VIRT_MSG_QUERY_REQ, 1,
                                        &selfReq, sizeof(selfReq),
                                        AMBA_VIRT_MSG_QUERY_RESP,
                                        &selfResp, sizeof(selfResp),
                                        &selfLen);
            if (selfRc == 0) {
                struct amba_virt_query_req devMemReq;
                std::memset(&devMemReq, 0, sizeof(devMemReq));
                devMemReq.query_op = AMBA_VIRT_QUERY_DEV_MEM;
                devMemRc = dev.rpcTransaction(AMBA_VIRT_MSG_QUERY_REQ, 2,
                                              &devMemReq, sizeof(devMemReq),
                                              AMBA_VIRT_MSG_QUERY_RESP,
                                              &devMemResp, sizeof(devMemResp),
                                              &devMemLen);
            }
        }
    }

    dev.closeDevice();
    return Decode(openRc, connectRc, barSize,
                  selfRc, selfResp, selfLen,
                  devMemRc, devMemResp, devMemLen,
                  out);
}

int GuestPropsService::Decode(int openRc, int connectRc, uint32_t barSize,
                              int selfRc, const struct amba_virt_query_resp &self, uint32_t selfLen,
                              int devMemRc, const struct amba_virt_query_resp &devMem, uint32_t devMemLen,
                              GuestProperties &out)
{
    out = GuestProperties{};
    out.bar_size_bytes = barSize;

    if (openRc < 0) {
        return failStep(out, "openDevice", openRc);
    }
    if (connectRc < 0) {
        return failStep(out, "connect", connectRc);
    }
    if (selfRc < 0) {
        return failStep(out, "querySelf", selfRc);
    }
    if (selfLen < kQueryRespHdrLen) {
        return failStep(out, "querySelf", -EBADMSG);
    }
    if (self.status < 0) {
        return failStep(out, "querySelf", self.status);
    }
    if (self.count == 0 ||
        selfLen < kQueryRespHdrLen + sizeof(struct amba_virt_peer_desc)) {
        return failStep(out, "querySelf", -EBADMSG);
    }

    if (devMemRc < 0) {
        return failStep(out, "queryDevMem", devMemRc);
    }
    if (devMemLen < kQueryRespHdrLen) {
        return failStep(out, "queryDevMem", -EBADMSG);
    }
    if (devMem.status < 0) {
        return failStep(out, "queryDevMem", devMem.status);
    }

    struct amba_virt_peer_desc selfDesc;
    std::memset(&selfDesc, 0, sizeof(selfDesc));
    std::memcpy(&selfDesc, self.payload, sizeof(selfDesc));

    out.query_status = 0;
    out.error.clear();
    out.online = true;
    out.tenant_cid = selfDesc.cid;
    out.tenant_idx = selfDesc.tenant_idx;
    out.caps = selfDesc.caps;
    ambaVirtDecodeCaps(selfDesc.caps, out.cap_names, out.caps_unknown);
    out.server_allocated_mb = selfDesc.mem_allocated_mb;

    size_t clampedDevMemLen = std::min<size_t>(devMemLen, sizeof(struct amba_virt_query_resp));
    size_t availPayload = clampedDevMemLen - kQueryRespHdrLen;
    size_t maxEntriesByLen = availPayload / sizeof(struct amba_virt_dev_mem_desc);
    size_t entryCount = std::min<size_t>(static_cast<size_t>(devMem.count), maxEntriesByLen);

    std::vector<struct amba_virt_dev_mem_desc> rawDevs(entryCount);
    if (entryCount > 0) {
        std::memcpy(rawDevs.data(),
                    devMem.payload,
                    entryCount * sizeof(struct amba_virt_dev_mem_desc));
    }

    std::sort(rawDevs.begin(), rawDevs.end(),
              [](const struct amba_virt_dev_mem_desc &a,
                 const struct amba_virt_dev_mem_desc &b) {
                  return a.base_offset < b.base_offset;
              });

    out.devices.reserve(rawDevs.size());
    for (const auto &d : rawDevs) {
        GuestDevicePartition part;
        part.dev_type = d.dev_type;
        part.dev_type_name = ambaVirtGetDevTypeName(d.dev_type);
        part.base_offset = d.base_offset;
        part.end_offset = static_cast<uint64_t>(d.base_offset) + static_cast<uint64_t>(d.size);
        part.size_bytes = d.size;
        part.rpc_arena_offset = d.rpc_arena_offset;
        part.rpc_arena_size = d.rpc_arena_size;
        part.flags = d.flags;
        out.devices.push_back(part);
    }

    struct RawPartitionExtent {
        uint64_t rawStart{0};
        uint64_t rawEnd{0};
        GuestMapExtent ext;
    };

    uint64_t barSize64 = static_cast<uint64_t>(barSize);
    std::vector<RawPartitionExtent> partExtents;

    auto addRawExtent = [&](uint64_t start, uint64_t end, const std::string &usage) {
        RawPartitionExtent item;
        item.rawStart = start;
        item.rawEnd = end;
        item.ext.partition = true;
        item.ext.usage = usage;
        item.ext.clipped = (start > barSize64) || (end > barSize64);
        item.ext.start_offset = std::min(start, barSize64);
        item.ext.end_offset = std::min(end, barSize64);
        item.ext.overlaps = false;
        partExtents.push_back(item);
    };

    for (const auto &part : out.devices) {
        addRawExtent(static_cast<uint64_t>(part.base_offset),
                     part.end_offset,
                     part.dev_type_name + " Buffer");
        if (part.rpc_arena_size > 0) {
            uint64_t arenaStart = static_cast<uint64_t>(part.rpc_arena_offset);
            uint64_t arenaEnd = arenaStart + static_cast<uint64_t>(part.rpc_arena_size);
            addRawExtent(arenaStart, arenaEnd, part.dev_type_name + " RPC Arena");
        }
    }

    std::sort(partExtents.begin(), partExtents.end(),
              [](const RawPartitionExtent &a, const RawPartitionExtent &b) {
                  if (a.ext.start_offset != b.ext.start_offset) {
                      return a.ext.start_offset < b.ext.start_offset;
                  }
                  return a.ext.end_offset < b.ext.end_offset;
              });

    for (size_t i = 0; i < partExtents.size(); ++i) {
        for (size_t j = i + 1; j < partExtents.size(); ++j) {
            bool rawOverlap = std::max(partExtents[i].rawStart, partExtents[j].rawStart) <
                              std::min(partExtents[i].rawEnd, partExtents[j].rawEnd);
            bool clippedOverlap = std::max(partExtents[i].ext.start_offset,
                                           partExtents[j].ext.start_offset) <
                                  std::min(partExtents[i].ext.end_offset,
                                           partExtents[j].ext.end_offset);
            if (rawOverlap || clippedOverlap) {
                partExtents[i].ext.overlaps = true;
                partExtents[j].ext.overlaps = true;
            }
        }
    }

    uint64_t cur = 0;
    uint64_t unionBytes = 0;
    for (const auto &pe : partExtents) {
        if (pe.ext.start_offset > cur) {
            GuestMapExtent gap;
            gap.partition = false;
            gap.usage = "NO PARTITION";
            gap.start_offset = cur;
            gap.end_offset = pe.ext.start_offset;
            gap.clipped = false;
            gap.overlaps = false;
            out.extents.push_back(gap);
        }
        out.extents.push_back(pe.ext);
        if (pe.ext.end_offset > cur) {
            uint64_t contribStart = std::max(cur, pe.ext.start_offset);
            unionBytes += (pe.ext.end_offset - contribStart);
            cur = pe.ext.end_offset;
        }
    }

    if (cur < barSize64) {
        GuestMapExtent gap;
        gap.partition = false;
        gap.usage = "NO PARTITION";
        gap.start_offset = cur;
        gap.end_offset = barSize64;
        gap.clipped = false;
        gap.overlaps = false;
        out.extents.push_back(gap);
    }

    out.partition_bytes = unionBytes;
    out.layout_text = ambaVirtFormatLayout(selfDesc, rawDevs, barSize);
    return 0;
}

std::string GuestPropsService::ToJson(const GuestProperties &props)
{
    std::ostringstream oss;
    oss << "{\n"
        << "  \"online\": " << (props.online ? "true" : "false") << ",\n"
        << "  \"query_status\": " << props.query_status << ",\n"
        << "  \"error\": \"" << escapeJsonString(props.error) << "\",\n"
        << "  \"tenant_cid\": " << props.tenant_cid << ",\n"
        << "  \"tenant_idx\": " << props.tenant_idx << ",\n"
        << "  \"caps\": " << props.caps << ",\n"
        << "  \"caps_hex\": \"" << ambaVirtToHex(props.caps) << "\",\n"
        << "  \"cap_names\": [";

    for (size_t i = 0; i < props.cap_names.size(); ++i) {
        if (i > 0) {
            oss << ", ";
        }
        oss << "\"" << escapeJsonString(props.cap_names[i]) << "\"";
    }

    oss << "],\n"
        << "  \"caps_unknown_hex\": \"" << ambaVirtToHex(props.caps_unknown) << "\",\n"
        << "  \"bar_size_bytes\": " << props.bar_size_bytes << ",\n"
        << "  \"partition_bytes\": " << props.partition_bytes << ",\n"
        << "  \"server_allocated_mb\": " << props.server_allocated_mb << ",\n"
        << "  \"devices\": [\n";

    for (size_t i = 0; i < props.devices.size(); ++i) {
        const auto &d = props.devices[i];
        oss << "    {"
            << "\"dev_type\": " << d.dev_type << ", "
            << "\"dev_type_name\": \"" << escapeJsonString(d.dev_type_name) << "\", "
            << "\"base_offset\": " << d.base_offset << ", "
            << "\"end_offset\": " << d.end_offset << ", "
            << "\"size_bytes\": " << d.size_bytes << ", "
            << "\"rpc_arena_offset\": " << d.rpc_arena_offset << ", "
            << "\"rpc_arena_size\": " << d.rpc_arena_size << ", "
            << "\"flags\": " << d.flags
            << "}" << (i + 1 < props.devices.size() ? "," : "") << "\n";
    }

    oss << "  ],\n"
        << "  \"extents\": [\n";

    for (size_t i = 0; i < props.extents.size(); ++i) {
        const auto &e = props.extents[i];
        oss << "    {"
            << "\"partition\": " << (e.partition ? "true" : "false") << ", "
            << "\"usage\": \"" << escapeJsonString(e.usage) << "\", "
            << "\"start_offset\": " << e.start_offset << ", "
            << "\"end_offset\": " << e.end_offset << ", "
            << "\"clipped\": " << (e.clipped ? "true" : "false") << ", "
            << "\"overlaps\": " << (e.overlaps ? "true" : "false")
            << "}" << (i + 1 < props.extents.size() ? "," : "") << "\n";
    }

    oss << "  ],\n"
        << "  \"layout_text\": \"" << escapeJsonString(props.layout_text) << "\"\n"
        << "}\n";

    return oss.str();
}

std::string GuestPropsHttpResponse(const GuestProperties &props)
{
    std::string body = GuestPropsService::ToJson(props);
    std::ostringstream oss;
    oss << "HTTP/1.1 200 OK\r\n"
        << "Content-Type: application/json\r\n"
        << "Content-Length: " << body.size() << "\r\n"
        << "Cache-Control: no-store\r\n"
        << "Access-Control-Allow-Origin: *\r\n"
        << "Connection: close\r\n\r\n"
        << body;
    return oss.str();
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
