# Virtual `/dev/iav` for HVM guests

> **Security boundary:** `amba-virt-server` is a trusted root Dom0
> service in the signed EVE image. The untrusted boundary is the HVM RPC
> and the guest `/dev/amba_virt` UAPI ([Architecture.md](Architecture.md#security-boundary)).
> The IAV broker described here sits on that boundary and treats every guest
> as hostile.

*Document Path: `doc/IavVirtualization.md`*  
*Copyright (C) 2026, Ambarella International LLC*

---

## 1. What this is

A legacy bare-metal application, or `libiav`, opens `/dev/iav` in an Ubuntu
or Alpine HVM guest and runs unmodified. The guest module `iav_virt.ko`
carries its calls over vsock to the **IAV broker** inside `amba-virt-server` in
EVE Dom0. The broker validates them and runs them on the real `/dev/iav` driver.
Frames and bitstream are not copied: the guest maps the same DRAM through
ivshmem windows.

```
 Guest (Ubuntu / Alpine HVM)                      EVE Dom0
 +-------------------------+                      +-------------------------------+
 | legacy IAV app / libiav |                      | amba-virt-server              |
 +------------+------------+                      |  +-------------------------+  |
              | open/ioctl/mmap/poll              |  | IAV broker              |  |
              v                                   |  |  - ioctl table + allow  |  |
 +-------------------------+   framed vsock 5555  |  |  - one real fd / session|  |
 | iav_virt.ko  (/dev/iav) |--------------------->|  |  - handles, quotas      |  |
 |  - ioctl marshaling     |<---------------------|  |  - mapping authority    |  |
 |  - poll in 20 ms slices |                      |  +------------+------------+  |
 |  - mmap of the windows  |                      |               | real ioctl    |
 +------------+------------+                      |               v               |
              | mmap (ivshmem BAR)                |       +---------------+       |
              v                                   |       |    iav.ko     |       |
 +-------------------------+   same physical      |       +-------+-------+       |
 | DSP,BSB,DEC,OVL windows |<-------------------->|               v               |
 +-------------------------+   DRAM below 4 GiB   |        DSP / IDSP hardware    |
                                                  +-------------------------------+
```

Control stays on vsock and bulk data stays in shared memory. No bulk data
crosses vsock.

**What a guest can do (verified on `n1-655-devkit`, 2026-10-12).** The primary guest configures and runs the pipeline,
reads encoded frames, decodes JPEGs with the hardware decoder in a mapped decode window, and blends overlay pictures
into a stream from a mapped overlay window. A consumer guest reads frames and canvases. Each of these is shown by a
reference application (section 12) that gives the same result in a guest as on the bare board. What does not work, and
the cause of each limit, is in section 6; which kinds of bare-metal application run unmodified is the compatibility
statement in section 12. The cost of a call is in section 9.

## 2. Backing real driver requirements

| Item | Where |
|---|---|
| `iav.ko` and the camera chain (`dsp`, `imgproc`, `ambnl`, `ambcma`, `hw_timer`, `cavalry`) | loaded by `amba-virt-server`; the live camera pipeline must be up (`/dev/iav` exists only then) |
| `amba_virt.ko` (host) | vsock transport and the data slices |
| `amba_virt_iav_win.ko` | exposes the DSP partition, the bitstream buffer, the decode partition and the overlay partition as mappable nodes (section 4) |
| Memory | the IAV driver's own partitions on N1-655: `IAV_PART_DSP` (live: 1 GiB at `0x40000000`), `IAV_PART_BSB` (live: 10 MiB at `0x84300000`), `IAV_PART_OVERLAY` (live: 8 MiB of content at `0x28000000`) and, only when the resource configuration has a decoder channel, `IAV_PART_DEC_BSB` (live: 6 MiB at `0x91900000`). Every DSP buffer is below 4 GiB (the DSP is 32-bit) |
| Decoder channel | decode needs a channel with `pre_dec_enable` on a virtual VIN, which the `ambds` module provides (`vin_virtual_flag=1 custom_format_enable=1`), in the resource script `camera.conf` names (section 8) |
| No SMMU | N1-655 has none, so a window gives the guest direct access to that memory (section 7) |

The IAV driver is **not modified**. The window is a second view of memory the
real driver already owns (the DSP firmware places canvas, pyramid and frame
output inside `IAV_PART_DSP`), so no fork hooks are needed. The fork policy
stands for any future hook, and this is its procedure:

- **Inventory.** The patch inventory (file, hook, reason) is empty today. A hook goes in a new
  file with a minimal, marked call from the upstream file, so the series can be listed with
  one search for the marker, and is added to the inventory in the same change.
- **Behavior.** A hook changes nothing when idle: the native suite must pass identically
  before and after.
- **Scope.** Virtualization hooks only. A defect in the IAV driver itself goes to its owners
  as a finding; the broker validates what reaches the driver instead.
- **Upstream merge.** Take the new upstream tree, re-apply the marked hooks (the marker lists
  them), rebuild, regenerate the ioctl table and the allow-lists (`tools/iav-policy/`) and read
  the changes in the review record, since a new upstream ioctl starts refused, then re-run the
  native and guest suites and the adversarial run.

## 3. Guest virtual driver interface

**Node:** `/dev/iav`, registered by `iav_virt.ko` (mode 0660). The module needs
`amba_virt.ko` (it uses its vsock channel and its PCI probe leaves the IAV
windows unbound for it).

**Supported ioctls.** Two sets, by who is calling:

- **Every guest (consumer set, 15):** `GET_IAV_STATE`, `GET_CHIP_ID`,
  `QUERY_ENC_MODE_CAP`, `QUERY_ENC_BUF_CAP`, `QUERY_IAV_CAP`,
  `GET_SYSTEM_RESOURCE`, `GET_CHAN_CONFIG`, `GET_CANVAS_CONFIG`, `DRV_DSP_INFO`,
  `QUERY_INFO`, `QUERY_MEMBLOCK`, `QUERY_DESC` (frame, canvas and pyramid
  descriptors, never a dma-buf fd), `SET_POLL_EVENT_CFG`, `CLEAR_POLL_EVENT_CFG`,
  `WAIT_NEXT_IDSP_FRAME`. Nothing that changes pipeline state.
- **The primary guest (128, the 15 above and 113 more):** the guest the kernel
  has bound to the slice named by `primary-slice` in `/etc/amba-virt/iav.conf`
  (the image ships `primary-slice 0`. The `amba_shm` ticket is reserved at
  Cavalry index 0).
  It may configure and run the pipeline: enter idle and preview, start and stop
  encode, system, stream, pyramid, VCAP, MJPEG, H.264 and H.265 configuration,
  frame-sync and DPTZ controls, VIN and VOUT controls, and decode: enter and leave decode mode, create and destroy a
  decoder, decode a JPEG or video bitstream, and query the decode status and buffer. The calls that name offsets
  (`CREATE_DECODER`, `DECODE_JPEG`, `DECODE_VIDEO`, `QUERY_DECODE_BSB`, `WAIT_DECODE_BSB`) are checked against the
  decode window (below), `WAIT_PLAYBACK_EOS` is bounded and `QUERY_DECODE_STATUS` never returns a physical address. It may also
  set and read a stream's overlays (`SET_OVERLAY_INSERT`, `GET_OVERLAY_INSERT`): each enabled area's colour table and pixels
  are checked against the overlay window, and a canvas overlay is refused. The guest is identified by the kernel on every call, never by anything
  the request says, and guest ids change at every VM start, so the designation
  is by slice. Without the setting no guest is primary.

Consumer DSP and bitstream windows (`amba_iav_dsp1`, `amba_iav_bsb1`) are writable BARs, the same as the primary's unsuffixed DSP, bitstream, overlay, and decode windows. The N1-655 has no SMMU. QEMU `memory-backend-file` `readonly=on` opens the node read-only, the kernel clears `VM_SHARED` on that mapping, and `iav_win_mmap` rejects it, so QEMU never starts. A store from the guest through those BARs reaches the live frame and bitstream. Ioctl checks do not stop that store. Private JPEG memory is a 512 MiB `ambcma` partition inside `iav@0`, leased as eight 64 MiB slices, one device node `/dev/amba_virt_iav_sliceN` per guest. A private submission is admitted against the caller's slice before any IAV ioctl.

Both sets are checked in as generated lists with a review record
(`tools/iav-policy/`, `iav_allow_primary.inc`; the record lists, for every held
back ioctl, why). An ioctl is in the primary set only if its argument has no
pointer, address, offset into a partition or fd, or its offsets were read and
are not memory (sensor line and gain-index offsets, QP offsets).

**Not supported yet (`EPERM`):**

- Held back on purpose: DRAM test, raw DSP commands, DSP reset, debug and DSP log
  controls, partition allocation and layout changes, external buffer feeds
  (`FEED_*`, `EFM_*`), raw encode with raw buffers, and blocking waits whose
  bound has not been read (`WAIT_IMG_SCALE`, `WAIT_RAW_ENCODE`,
  `WAIT_SKIP_IDSP_CFG`, `FLUSH_FRAMEDESC`).
- Needing a window and a check for each offset: blur, warp, and video
  processing and sync-processing configuration. (Overlay and decode have their windows and checks; see the support
  table.) Canvas overlays are refused: their partition has no window.
- Carrying addresses or pointers: `GDMA_COPY`, `SET_IMG_SCALE` (the real driver
  does not fully range-check a physical address there), the IMAGE statistics
  calls (nine user pointers each), `SET_RAW_ENC_CFG`, `*_ISP_NN_CFG`, VOUT OSD
  sync.

Extending a set means regenerating the list and reading the review record.

| Call | Result |
|---|---|
| An ioctl the driver does not have | `ENOTTY` |
| An ioctl the driver has but the broker does not allow | `EPERM` |
| An argument of the wrong length or a malformed argument | `EMSGSIZE`, `EINVAL` or `EPERM` (refused before the real driver sees it) |
| The guest lacks the `IAV_DEV` capability | `open` fails with `EPERM` |
| The server restarted or forgot the descriptor | the module reopens the session once, restores the poll configuration, and retries |
| The server is unreachable | the call fails with the transport error (`ETIMEDOUT`, `ENOTCONN`); the next call reconnects |

**mmap:** as on bare metal, the application maps a partition at the address
`QUERY_MEMBLOCK` reported (`MAP_SHARED`, no `PROT_EXEC`). The broker authorizes
the mapping (an address that is not the base of a window, a length over the
window, or an exec mapping is refused), and the module maps the window's BAR.
The bitstream ring may ask for twice the window, which maps the same BAR twice in
a row.

**poll:** a worker asks the broker in slices of at most 20 ms (so a poller delays
other calls from the guest by one slice at most), so `poll()` never sleeps in the transport; clearing the poll
configuration stops the worker. **Blocking:** a blocking canvas or pyramid query
is forwarded as blocking; the real driver bounds it (two seconds, then `EAGAIN`).
Frame query waits are clamped.

**Reading a mapped window (guest).** The windows are device memory at stage 2, so an application must copy out of a
window with aligned accesses of at most eight bytes (byte loads for the unaligned head and tail, aligned 8-byte
loads in between). An unaligned access, which libc `memcpy` makes for an unaligned source, raises an alignment
fault that KVM cannot emulate and **the guest VM is destroyed** (the host log shows `kvm: Unsupported FSC:
EC=0x24 xFSC=0x21`; the controller shows the instance in `Error` until it is restarted). The other guest and
Dom0 are not affected. Frame planes start at aligned offsets; encoded frames in the bitstream partition start at
arbitrary offsets, so read them with such a copy (`guest-os/apps/iav-ref/IavRefCommon.hxx`, `windowCopy`). A
bare-metal application that copies from a mapping with `memcpy` at arbitrary offsets does not run unmodified.

**One channel per guest:** the vsock channel carries one request at a time, so a
blocking call delays other calls from the same guest for the length of its wait.

## 4. The windows

`amba_virt_iav_win.ko` registers eight nodes, two sets of the same four windows:

| Node | Covers | Size |
|---|---|---|
| `/dev/amba_virt_iav_dsp`, `/dev/amba_virt_iav_dsp1` | the DSP partition | a power of two (live: 1 GiB) |
| `/dev/amba_virt_iav_bsb`, `/dev/amba_virt_iav_bsb1` | the bitstream buffer, padded | a power of two (live: 16 MiB for a 10 MiB buffer) |
| `/dev/amba_virt_iav_dec`, `/dev/amba_virt_iav_dec1` | the decode partition (`IAV_PART_DEC_BSB`): the JPEG or bitstream in, the decoded picture out | a power of two (live: 8 MiB for the 6 MiB of one decoder) |
| `/dev/amba_virt_iav_ovl`, `/dev/amba_virt_iav_ovl1` | the overlay partition (`IAV_PART_OVERLAY`): colour tables and pixels of stream overlays | a power of two (live: exactly 8 MiB, no padding) |

- **The decode window exists only with a decoder.** The real driver allocates `IAV_PART_DEC_BSB` (6 MiB per decoder)
  when preview is enabled and the pipeline's resource configuration has a decoder channel (a channel with `pre_dec_enable`
  on a virtual VIN, which the `ambds` module provides). Without one the partition does not exist, the server logs it and
  configures no decode window, and every call that names an offset is refused. A guest cannot allocate, free or move
  the partition (`ALLOC_MEM_PART` and `FREE_MEM_PART` are refused), and cannot add a decoder (the resource configuration
  calls are refused), so it stays where the server found it.
- **The overlay window is the partition's content only.** The real driver keeps its own DSP command buffers for overlays
  at the end of the same partition and reports only the content part to applications (`QUERY_MEMBLOCK` on
  `IAV_PART_OVERLAY`), so the window covers what an application may use and a guest cannot reach the command buffers.
  Only the primary guest may map the decode and overlay windows; a consumer is refused (`EPERM`).
- **Configuration.** `amba-virt-server` configures each node once at start, from
  the partition table the real driver reports (`AMBA_VIRT_IAV_WIN_IOC_CONFIG`,
  root only), before any VM starts. The module refuses a range outside
  `0x28000000` to 4 GiB, a misaligned or overflowing range, padding over 64 MiB,
  a reconfiguration while mapped, and a private mapping.
- **Padding** behind the real range (to reach a power of two, as a PCI BAR needs)
  is private zeroed RAM, cacheable, in 2 MiB chunks. It never reaches the real
  range. The last 64 bytes of padding carry an identity header (`AMIW`, window id,
  length), which lets the guest tell a window from a data slice. A window with no
  padding (the DSP and overlay partitions when they are already a power of two) is recognized by
  elimination: the unbound ivshmem device whose size is that of exactly one unpadded window the broker
  announced and that carries no slice claim.
- **Mapping attributes.** The real range is mapped write-combined, which is what
  makes a frame the DSP wrote visible without cache maintenance.
- **Template and its open item.** The window design follows the DMA32 slice code (a misc node
  per window, a guest that classifies a window by its exact BAR size, requests that carry
  offsets only and are bounds-checked). The DMA32 pool that code was written for has had no
  device-tree backing since 2026-10-01, so none of its memory is used here; restoring or
  retiring the pool is a separate project (section 11).
- **Adapters.** Each node is one model adapter (`IO_TYPE_OTHER`, empty
  `phyaddrs`, `cbattr` `shmpath` and `shmsize`), exactly like `amba_shm`:
  `amba_iav_dsp`, `amba_iav_bsb`, `amba_iav_dec`, `amba_iav_ovl`, `amba_iav_dsp1`, `amba_iav_bsb1`, `amba_iav_dec1`, `amba_iav_ovl1`
  ([EVE-Ambarella-Models.md](EVE-Ambarella-Models.md)). `shmsize` must equal the
  size the server configures. An adapter belongs to one app instance at a time, so
  the second set lets a second guest on the same board have its own.

## 5. Wire protocol

The IAV request and response ride in the common `struct amba_virt_msg` framing as
fixed-layout little-endian frames (`include/uapi/amba_virt_iav.h`): no pointers,
no guest virtual or host physical address on the wire, handles and
window-relative offsets only. This is a fixed frame, not Nanopb, on purpose; the
rule is in [VsockMessageFormats.md](VsockMessageFormats.md).

## 6. Capabilities and limits

- **Capability.** `AMBA_VIRT_CAP_IAV_DEV` is part of the standard guest role. A
  guest sees windows only if its app instance was given the window adapters.
- **Per-guest limits.** Open sessions are capped, requests are rate limited
  (20000 per second, burst 5000), argument and output sizes are bounded, every
  blocking call has a deadline, and a guest that disconnects has its sessions
  released.
- **Two roles.** The consumer reads frames, canvases and queries; the primary guest also configures and runs the
  pipeline, decodes and draws overlays (section 3). Which guest is primary is the hypervisor's decision, not the guest's.

### 6.0 Limits and what causes each

Each limit below is observed (on the devkit, with the gates of section 8) unless it says otherwise. The cause is one of
**no SMMU on N1-655** (a guest-supplied address cannot be confined, so memory is limited to windows granted in advance
and nothing carrying an address is served), **inherent to virtualization** (it follows from several guests sharing one
DSP pipeline, or from a call crossing a VM boundary), or a **choice of this design** that more work could remove. A
platform with an SMMU, such as T1, is expected to relax the first kind only; that is an expectation, not a measurement.

| Limit | Cause | What the guest sees |
|---|---|---|
| Calls that carry a physical address, a pointer or a file descriptor are refused: `GDMA_COPY`, `SET_IMG_SCALE`, the IMAGE statistics calls, `SET_RAW_ENC_CFG`, `*_ISP_NN_CFG`, VOUT OSD sync, buffer feeds | **No SMMU.** The broker cannot let a guest name memory the DSP or the GDMA engine will then touch: the driver does not fully range-check these addresses, and without an SMMU nothing else confines the device. With an SMMU the engine could be confined to the guest's own mapping and many of these could be served | `EPERM` |
| Memory is reached only through the windows (DSP, bitstream, decode, overlay); a guest cannot allocate, free or move a partition | **No SMMU** (memory is granted in advance because it cannot be confined later) and **inherent** (one set of partitions serves all guests) | `ALLOC_*` and `FREE_MEM_PART` give `EPERM`; a mapping outside a window gives `EINVAL` |
| A guest with a window can read and corrupt what the window holds, including the DSP's working memory | **No SMMU.** The window is a direct mapping of real memory; see section 7 | no error; a documented limit |
| Hazardous calls are refused for every guest: DRAM test, raw DSP commands, DSP reset, debug and log controls | **Inherent.** One DSP serves every guest and Dom0 | `EPERM` |
| A consumer cannot configure the pipeline; the primary's changes reach every guest | **Inherent.** There is one shared pipeline, so one guest owns it. Stopping a stream sends its reader an end-of-stream descriptor (size 0) | `EPERM` for a consumer; the reader's call returns the end marker |
| One reader of a stream's frame descriptors at a time | **The driver, and inherent.** The driver in shared-bitstream mode accepts descriptor queries from one file only; the second reader gets `EFAULT`, on the bare board too (observed natively). Canvas and pyramid queries have no such limit | `EFAULT` for the second reader; the first is not disturbed |
| The Dom0 frame tap and a guest cannot both read stream descriptors | **The same driver rule.** While the tap runs it is the reader (production coexistence is not solved). Observed on the final image: `IavRefStream` in the consumer guest gets `EFAULT`, while the demos in both guests receive the tap's hardware-encoded MJPEG frames, which carry an overlay the primary guest set | `EFAULT` |
| A call costs about 40 us more than on the bare board | **Inherent.** One vsock round trip per call (section 9) | slower calls |
| One request at a time per guest, so a blocking call delays the guest's other calls | **Design choice.** The channel is one vsock connection; several channels per guest would remove it | queued calls |
| An application must copy out of a window with aligned accesses of at most 8 bytes | **Inherent to a PCI window.** A BAR is device memory at stage 2 and KVM cannot emulate an unaligned access (observed: the guest VM is destroyed). An SMMU does not change this (inferred) | a bare-metal `memcpy` from a mapping does not run unmodified |
| The decode window exists only when the pipeline has a decoder channel | **Design choice.** The resource configuration belongs to Dom0 (`camera.conf`); a guest cannot add a decoder | decode calls give `EPERM` |
| Decoding 1080p with a full read-out of the frame in the guest is slower than on the bare board (section 9) | **Inherent and a design choice.** The decoded frame is read through the window at about 0.6 of the native rate; scaling it on the DSP first needs `SET_IMG_SCALE`, which is refused (first row) | lower decode rate with read-out |
| Blur, warp, video-processing and sync-processing configuration, and canvas overlays, are refused | **Not done.** They need a window or a field-by-field read of the argument; no reference application needs them (section 12). Canvas overlays live in a partition with no window | `EPERM` |

### 6.0.1 Concurrent use that works

Within the limits above, the two guests work at the same time: the consumer reads stream frames while the primary decodes,
the consumer reads the canvas while the primary draws an overlay and changes the stream quality, and a second reader of
the same stream is refused without disturbing the first. After a restart of the Dom0 server a read that was in progress
ends, both guests stay up, and both are served again (the end-to-end gate, section 8).

### 6.1 Which calls are served

The table lists every ioctl of the real IAV driver, grouped as the driver's header groups them (the encode calls by
number range; decode, VIN device, VOUT and IMAGE by their own magic numbers) and in the header's order. "Supported"
says who may call it: **every guest**, the **primary guest only** (the guest holding the primary ivshmem slice), or
**refused** (`EPERM`, with the reason in the last column). Nothing outside this table is served: the supported
surface is the real `/dev/iav` ABI, so a refused call fails with an error and no other call stands in for it.

The table is generated from the broker's own table with its allow-list applied and from the review record
`drivers/amba_virt/tools/iav_ioctl_review.tsv` (a description and a reason per call), by
`tools/iav-policy/gen_iav_support_table.py`. After changing what is served, edit the review record and run
`make -C drivers/amba_virt/tools iav-support-table`; `make -C drivers/amba_virt/tools test_iav_support_table_run`
fails when the document, the review record and the allow-list disagree.

<!-- iav-support-table:begin (generated by tools/iav-policy/gen_iav_support_table.py; do not edit) -->

All 205 ioctls of the IAV driver on this platform: 15 served to every guest, 113 served to the primary guest only, 77 refused (`EPERM`).

#### Driver and DSP (0x00 to 0x0F)

| Ioctl | What it does | Supported | Reason when not served |
|---|---|---|---|
| `IAV_IOC_GET_IAV_STATE` | Read the driver state (idle, preview, encoding, decoding and so on). | every guest |  |
| `IAV_IOC_GET_CHIP_ID` | Read the chip identification once the DSP has booted. | every guest |  |
| `IAV_IOC_SET_DSP_LOG` | Set the DSP firmware log level. | refused | debug and test only |
| `IAV_IOC_DRV_DSP_INFO` | Read driver and DSP information. | every guest |  |
| `IAV_IOC_QUERY_INFO` | Query driver information (boot mode, version, per-stream and per-channel details) selected by an info id. | every guest |  |
| `IAV_IOC_QUERY_ENC_MODE_CAP` | Query which encode functions the system supports in a given mode. | every guest |  |
| `IAV_IOC_QUERY_ENC_BUF_CAP` | Query the capabilities of a source buffer. | every guest |  |
| `IAV_IOC_QUERY_IAV_CAP` | Query the unified system capabilities. | every guest |  |

#### System (0x10 to 0x3F)

| Ioctl | What it does | Supported | Reason when not served |
|---|---|---|---|
| `IAV_IOC_ENTER_IDLE` | Stop preview and bring the driver to the idle state. | primary guest only |  |
| `IAV_IOC_ENABLE_PREVIEW` | Start the preview pipeline. | primary guest only |  |
| `IAV_IOC_START_ENCODE` | Start encoding one or more streams. | primary guest only |  |
| `IAV_IOC_STOP_ENCODE` | Stop encoding one or more streams. | primary guest only |  |
| `IAV_IOC_ABORT_ENCODE` | Abort a stream start or stop that is still in transition. | primary guest only |  |
| `IAV_IOC_GET_SYSTEM_RESOURCE` | Read the system resource limits (channels, streams, buffers). | every guest |  |
| `IAV_IOC_SET_SYSTEM_RESOURCE` | Set the system resource limits; idle state only. | primary guest only |  |
| `IAV_IOC_GET_CHAN_CONFIG` | Read the configuration of a channel. | every guest |  |
| `IAV_IOC_SET_CHAN_CONFIG` | Set the configuration of a channel; idle state only. | primary guest only |  |
| `IAV_IOC_GET_CANVAS_CONFIG` | Read the configuration of a canvas. | every guest |  |
| `IAV_IOC_SET_CANVAS_CONFIG` | Set the configuration of a canvas; idle state only. | primary guest only |  |
| `IAV_IOC_GET_PYRAMID_CFG` | Read the pyramid scaler configuration of a channel. | primary guest only |  |
| `IAV_IOC_SET_PYRAMID_CFG` | Set the pyramid scaler configuration of a channel; idle state only. | primary guest only |  |
| `IAV_IOC_GET_STREAM_RESOURCE` | Read the resources reserved for streams. | primary guest only |  |
| `IAV_IOC_SET_STREAM_RESOURCE` | Set the resources reserved for streams; idle state only. | primary guest only |  |
| `IAV_IOC_GDMA_COPY` | Copy a 2-D block between memory partitions with the GDMA engine. | refused | carries physical addresses a guest must not supply; planned by offset only (Phase D) |
| `IAV_IOC_QUERY_DESC` | Query frame descriptors: encoded frames, canvas, pyramid, raw and other frame-based data. | every guest |  |
| `IAV_IOC_QUERY_MEMBLOCK` | Query the address and size of the memory partitions shared with applications. | every guest |  |
| `IAV_IOC_EFM_GET_POOL_INFO` | Read the external frame manager (EFM) pool information. | refused | hands buffers between the application and the DSP by offset or file descriptor; needs windows over those buffers (Phase D) |
| `IAV_IOC_EFM_HANDSHAKE_FRAME` | Hand a processed external frame back to the DSP for encoding. | refused | hands buffers between the application and the DSP by offset or file descriptor; needs windows over those buffers (Phase D) |
| `IAV_IOC_EFM_RELEASE_ALL_FRAMES` | Release every EFM frame buffer the application holds. | refused | hands buffers between the application and the DSP by offset or file descriptor; needs windows over those buffers (Phase D) |
| `IAV_IOC_EFM_REQUEST_FRAME` | Take one EFM frame buffer to fill or process. | refused | hands buffers between the application and the DSP by offset or file descriptor; needs windows over those buffers (Phase D) |
| `IAV_IOC_EFM_SET_EXT_POOL` | Provide an application-owned EFM buffer pool. | refused | hands buffers between the application and the DSP by offset or file descriptor; needs windows over those buffers (Phase D) |
| `IAV_IOC_GET_RAW_ENCODE` | Read the raw-encode configuration. | primary guest only |  |
| `IAV_IOC_SET_RAW_ENCODE` | Set the raw-encode configuration; idle state only. | refused | raw encode with raw buffers; needs the raw windows (Phase D) |
| `IAV_IOC_SET_RAW_ENC_CFG` | Initialize the raw-encode procedure; idle state only. | refused | carries physical addresses a guest must not supply; planned by offset only if an application needs it (Phase D) |
| `IAV_IOC_WAIT_RAW_ENCODE` | Wait until the raw-encode path can accept the next buffer. | refused | blocking wait that is unbounded when back-pressure is on; the broker needs its own bound |
| `IAV_IOC_ALLOC_ANON_MEM_PART_FD` | Allocate an anonymous partition and return a dma-buf file descriptor for it. | refused | returns a file descriptor a guest cannot hold |
| `IAV_IOC_ALLOC_MEM_PART` | Allocate a named IAV/DSP memory partition. | refused | allocates or frees memory the guest windows are built from; the server owns it |
| `IAV_IOC_FREE_MEM_PART` | Free a named or anonymous memory partition. | refused | allocates or frees memory the guest windows are built from; the server owns it |
| `IAV_IOC_FEED_PYRAMID_BUF` | Feed a buffer to the pyramid scaler in manual-feed mode. | refused | hands buffers between the application and the DSP by offset or file descriptor; needs windows over those buffers (Phase D) |
| `IAV_IOC_RELEASE_PYRAMID_BUF` | Release a pyramid buffer locked by a descriptor query. | refused | hands buffers between the application and the DSP by offset or file descriptor; needs windows over those buffers (Phase D) |
| `IAV_IOC_SYNC_CACHE_MEM_PART` | Clean or invalidate the CPU cache for a partition shared with the DSP. | refused | cache maintenance by partition id; planned with the windows it addresses (Phase D) |
| `IAV_IOC_FEED_CANVAS_BUF` | Feed a buffer to a canvas in manual-feed mode. | refused | hands buffers between the application and the DSP by offset or file descriptor; needs windows over those buffers (Phase D) |
| `IAV_IOC_RELEASE_CANVAS_BUF` | Release a canvas buffer locked by a descriptor query. | refused | hands buffers between the application and the DSP by offset or file descriptor; needs windows over those buffers (Phase D) |
| `IAV_IOC_RESET` | Reset the driver and the DSP to the initial state. | refused | resets the driver and the DSP for every client |
| `IAV_IOC_GET_MEM_LAYOUT_RESOURCE` | Read the memory layout resource configuration. | primary guest only |  |
| `IAV_IOC_QUERY_MEM_LAYOUT_INFO` | Query the resulting memory layout (partition addresses and sizes). | refused | the argument struct has offset or file-descriptor fields that have not been classified yet; planned once each field is read (Phase D) |
| `IAV_IOC_SET_MEM_LAYOUT_RESOURCE` | Configure the memory layout; init state only. | refused | allocates or frees memory the guest windows are built from; the server owns it |
| `IAV_IOC_ALLOC_ANON_MEM_PART` | Allocate an anonymous memory partition the application can map. | refused | allocates or frees memory the guest windows are built from; the server owns it |
| `IAV_IOC_REGISTER_ANON_MEM_PART` | Register an existing memory range as an anonymous partition. | refused | allocates or frees memory the guest windows are built from; the server owns it |
| `IAV_IOC_UNREGISTER_ANON_MEM_PART` | Unregister an anonymous partition. | refused | allocates or frees memory the guest windows are built from; the server owns it |
| `IAV_IOC_CLEAR_POLL_EVENT_CFG` | Clear the descriptor events configured for poll. | every guest |  |
| `IAV_IOC_SET_POLL_EVENT_CFG` | Select which descriptor events wake a poll on the device. | every guest |  |
| `IAV_IOC_FEED_RAW_BUF` | Feed a raw buffer to a channel in manual-feed mode. | refused | hands buffers between the application and the DSP by offset or file descriptor; needs windows over those buffers (Phase D) |
| `IAV_IOC_RELEASE_RAW_BUF` | Release a raw buffer locked by a descriptor query. | refused | hands buffers between the application and the DSP by offset or file descriptor; needs windows over those buffers (Phase D) |
| `IAV_IOC_SET_VOUT_MAX_RES_CFG` | Set the maximum VOUT resources; init state only. | primary guest only |  |
| `IAV_IOC_CE_GO_MAP_HANDSHAKE_BUFFER` | Hand a contrast-enhancement gain/offset map buffer to the DSP. | refused | hands buffers between the application and the DSP by offset or file descriptor; needs windows over those buffers (Phase D) |
| `IAV_IOC_CE_GO_MAP_REQUEST_BUFFER` | Take a contrast-enhancement gain/offset map buffer for processing. | refused | hands buffers between the application and the DSP by offset or file descriptor; needs windows over those buffers (Phase D) |

#### Encode control (0x40 to 0x5F)

| Ioctl | What it does | Supported | Reason when not served |
|---|---|---|---|
| `IAV_IOC_GET_STREAM_CONFIG` | Read the configuration of a stream. | refused | the argument struct has offset or file-descriptor fields that have not been classified yet; planned once each field is read (Phase D) |
| `IAV_IOC_SET_STREAM_CONFIG` | Change the configuration of a stream while it runs. | refused | the argument struct has offset or file-descriptor fields that have not been classified yet; planned once each field is read (Phase D) |
| `IAV_IOC_GET_H264_CONFIG` | Read the H.264 encoding configuration of a stream. | primary guest only |  |
| `IAV_IOC_SET_H264_CONFIG` | Set the H.264 encoding configuration of an idle stream. | primary guest only |  |
| `IAV_IOC_GET_H265_CONFIG` | Read the H.265 encoding configuration of a stream. | primary guest only |  |
| `IAV_IOC_SET_H265_CONFIG` | Set the H.265 encoding configuration of an idle stream. | primary guest only |  |
| `IAV_IOC_GET_MJPEG_CONFIG` | Read the MJPEG quality and chroma format of a stream. | primary guest only |  |
| `IAV_IOC_SET_MJPEG_CONFIG` | Set the MJPEG quality and chroma format of an idle stream. | primary guest only |  |
| `IAV_IOC_APPLY_FRAME_SYNC_PROC` | Apply frame-synchronized operations for the video DSP. | primary guest only |  |
| `IAV_IOC_APPLY_IDSP_ENC_SYNC_PROC` | Apply frame-synchronized operations between the image DSP and the encoder. | primary guest only |  |
| `IAV_IOC_CFG_FRAME_SYNC_PROC` | Configure frame-synchronized operations. | refused | the argument struct has offset or file-descriptor fields that have not been classified yet; planned once each field is read (Phase D) |
| `IAV_IOC_GET_FRAME_SYNC_PROC` | Read the frame-synchronized operation configuration. | refused | the argument struct has offset or file-descriptor fields that have not been classified yet; planned once each field is read (Phase D) |
| `IAV_IOC_GET_OVERLAY_INSERT` | Read the overlays inserted into a stream. | primary guest only |  |
| `IAV_IOC_SET_OVERLAY_INSERT` | Insert picture overlays into a stream. | primary guest only |  |
| `IAV_IOC_GET_FPS_SUGGEST` | Read the suggested frame rate of each stream. | primary guest only |  |
| `IAV_IOC_FLUSH_FRAMEDESC` | Flush a stream's queued frame descriptors, optionally forcing an IDR frame first. | refused | drops queued frame descriptors for every consumer of a stream and can wait 5 s, as long as the guest call deadline; needs a longer per-call deadline (Phase B) |
| `IAV_IOC_GET_BLUR_COLOR` | Read the blur color tables. | primary guest only |  |
| `IAV_IOC_GET_BLUR_INSERT` | Read the blur (privacy) areas inserted into a stream. | refused | carries offsets into a data partition the guest has no window for; planned with that window (Phase D) |
| `IAV_IOC_SET_BLUR_COLOR` | Set the blur color tables. | primary guest only |  |
| `IAV_IOC_SET_BLUR_INSERT` | Insert blur (privacy) areas into a stream. | refused | carries offsets into a data partition the guest has no window for; planned with that window (Phase D) |
| `IAV_IOC_GET_YUV_REMAP_INSERT` | Read the YUV remap areas inserted into a stream. | refused | carries offsets into a data partition the guest has no window for; planned with that window (Phase D) |
| `IAV_IOC_SET_YUV_REMAP_INSERT` | Insert YUV remap areas into a stream. | refused | carries offsets into a data partition the guest has no window for; planned with that window (Phase D) |
| `IAV_IOC_APPLY_CANVAS_FRAME_SYNC_PROC` | Apply canvas frame-synchronized operations. | primary guest only |  |
| `IAV_IOC_CFG_CANVAS_FRAME_SYNC_PROC` | Configure canvas frame-synchronized operations. | refused | the argument struct has offset or file-descriptor fields that have not been classified yet; planned once each field is read (Phase D) |
| `IAV_IOC_GET_CANVAS_FRAME_SYNC_PROC` | Read the canvas frame-synchronized operation configuration. | refused | the argument struct has offset or file-descriptor fields that have not been classified yet; planned once each field is read (Phase D) |
| `IAV_IOC_GET_POSTP_SCALE_CTRL` | Read the post-processing scale control of a stream. | primary guest only |  |
| `IAV_IOC_SET_POSTP_SCALE_CTRL` | Set the post-processing scale control of a stream. | primary guest only |  |

#### VIN, IDSP and VCAP control (0x60 to 0x6F)

| Ioctl | What it does | Supported | Reason when not served |
|---|---|---|---|
| `IAV_IOC_GET_VCAP_CFG` | Read the video capture (VCAP) configuration. | primary guest only |  |
| `IAV_IOC_SET_VCAP_CFG` | Set the video capture (VCAP) configuration. | primary guest only |  |
| `IAV_IOC_APPLY_WARP_CTRL` | Apply lens-distortion (LDC) and fisheye warp parameters. | primary guest only |  |
| `IAV_IOC_APPLY_WARP_CTRL_IDSP_FRAME_SYNC` | Apply warp parameters synchronized to an image DSP frame. | primary guest only |  |
| `IAV_IOC_CFG_WARP_CTRL` | Configure lens-distortion and fisheye warp parameters. | refused | carries offsets into a data partition the guest has no window for; planned with that window (Phase D) |
| `IAV_IOC_GET_WARP_CTRL` | Read the warp parameters. | refused | carries offsets into a data partition the guest has no window for; planned with that window (Phase D) |
| `IAV_IOC_APPLY_VIDEO_PROC` | Apply the video processing set up by the configuration call (privacy mask, DPTZ, canvas and others). | primary guest only |  |
| `IAV_IOC_CFG_VIDEO_PROC` | Configure privacy mask, digital pan/tilt/zoom and related processing. | refused | the argument struct has offset or file-descriptor fields that have not been classified yet; planned once each field is read (Phase D) |
| `IAV_IOC_GET_VIDEO_PROC` | Read the video processing configuration. | refused | the argument struct has offset or file-descriptor fields that have not been classified yet; planned once each field is read (Phase D) |
| `IAV_IOC_GET_VIN_IDSP_CFG` | Read the VIN image DSP configuration. | refused | the argument struct has offset or file-descriptor fields that have not been classified yet; planned once each field is read (Phase D) |
| `IAV_IOC_SET_VIN_IDSP_CFG` | Set the VIN image DSP configuration. | refused | the argument struct has offset or file-descriptor fields that have not been classified yet; planned once each field is read (Phase D) |
| `IAV_IOC_SET_VIN_IDSP_DELAY_INFO` | Set the image DSP delay information used by warp. | primary guest only |  |
| `IAV_IOC_WAIT_NEXT_IDSP_FRAME` | Wait for the next valid image DSP frame. | every guest |  |
| `IAV_IOC_GET_VIN_COMPAND_CFG` | Read the VIN companding configuration. | primary guest only |  |
| `IAV_IOC_SET_VIN_COMPAND_CFG` | Set the VIN companding configuration. | primary guest only |  |
| `IAV_IOC_SET_IMG_SCALE` | Submit an arbitrary-size image scaling job to the DSP. | refused | carries physical addresses a guest must not supply; planned by offset only (Phase D) |
| `IAV_IOC_WAIT_IMG_SCALE` | Wait for an image scaling job to finish. | refused | blocking wait bounded at 2 s by the driver; planned with its feature (Phase D) |
| `IAV_IOC_WAIT_SKIP_IDSP_CFG` | Wait until a VIN switch of a channel has completed. | refused | blocking wait bounded at 2 s by the driver; planned with its feature (Phase D) |
| `IAV_IOC_SYNC_VIN_FRAME` | Wait for the next valid image DSP frame. | primary guest only |  |

#### AISP (0x70 to 0x7F)

| Ioctl | What it does | Supported | Reason when not served |
|---|---|---|---|
| `IAV_IOC_GET_ISP_NN_CFG` | Read the neural-network parameters of the image pipeline. | refused | carries physical addresses a guest must not supply; planned by offset only if an application needs it (Phase D) |
| `IAV_IOC_SET_ISP_NN_CFG` | Set the neural-network parameters of the image pipeline; idle state only. | refused | carries physical addresses a guest must not supply; planned by offset only if an application needs it (Phase D) |
| `IAV_IOC_APPLY_MULTI_REGION_DPTZ_CTRL` | Apply multi-region digital pan/tilt/zoom parameters. | primary guest only |  |
| `IAV_IOC_CFG_MULTI_REGION_DPTZ_CTRL` | Configure multi-region digital pan/tilt/zoom parameters. | primary guest only |  |
| `IAV_IOC_GET_MULTI_REGION_DPTZ_CTRL` | Read the multi-region digital pan/tilt/zoom parameters. | primary guest only |  |
| `IAV_IOC_APPLY_IDSP_SYNC_PROC` | Apply frame-synchronized operations at the image DSP stage. | primary guest only |  |

#### Miscellaneous (0xD0 and up)

| Ioctl | What it does | Supported | Reason when not served |
|---|---|---|---|
| `IAV_IOC_GET_DEBUG_CONFIG` | Read the driver debug configuration. | refused | debug and test only |
| `IAV_IOC_SET_DEBUG_CONFIG` | Set the driver debug configuration. | refused | debug and test only |
| `IAV_IOC_GET_AUDIT_CONFIG` | Read the driver audit (call logging) configuration. | primary guest only |  |
| `IAV_IOC_SET_AUDIT_CONFIG` | Set the driver audit (call logging) configuration. | primary guest only |  |
| `IAV_IOC_ENABLE_DRAM_TEST` | Run a DRAM stress test: the DSP reads and writes DRAM continuously. | refused | saturates DRAM for the DSP; test only |
| `IAV_IOC_GET_DSP_HEALTH_CONFIG` | Read the DSP health guard configuration. | primary guest only |  |
| `IAV_IOC_SET_DSP_HEALTH_CONFIG` | Set the DSP health guard configuration. | primary guest only |  |
| `IAV_IOC_CUSTOM_CMDS` | Send customized commands to the driver. | refused | raw driver command channel; refused for any guest |

#### Decode

| Ioctl | What it does | Supported | Reason when not served |
|---|---|---|---|
| `IAV_IOC_ENTER_DECODE_MODE` | Put the video system into decode mode. | primary guest only |  |
| `IAV_IOC_LEAVE_DECODE_MODE` | Leave decode mode. | primary guest only |  |
| `IAV_IOC_CREATE_DECODER` | Create a decoder instance (type, size and its part of the decode buffer). | primary guest only |  |
| `IAV_IOC_DESTROY_DECODER` | Destroy a decoder instance. | primary guest only |  |
| `IAV_IOC_DECODE_START` | Start the decode process. | primary guest only |  |
| `IAV_IOC_DECODE_STOP` | Stop the decode process. | primary guest only |  |
| `IAV_IOC_DECODE_VIDEO` | Submit video bitstream data to a decoder. | primary guest only |  |
| `IAV_IOC_DECODE_JPEG` | Decode one JPEG picture from the decode buffer into a frame. | primary guest only |  |
| `IAV_IOC_WAIT_DECODE_BSB` | Wait for room in the decode bitstream buffer. | primary guest only |  |
| `IAV_IOC_DECODE_TRICK_PLAY` | Pause, step or otherwise control playback. | primary guest only |  |
| `IAV_IOC_DECODE_SPEED` | Change the playback speed and direction. | primary guest only |  |
| `IAV_IOC_WAIT_PLAYBACK_EOS` | Wait until the whole stream has been decoded. | primary guest only |  |
| `IAV_IOC_QUERY_DECODE_STATUS` | Query the decoding status and the decoded picture information. | primary guest only |  |
| `IAV_IOC_QUERY_DECODE_BSB` | Query the state of a decoder's bitstream buffer. | primary guest only |  |

#### VIN device

| Ioctl | What it does | Supported | Reason when not served |
|---|---|---|---|
| `IAV_IOC_VIN_GET_DEVINFO` | Read a video source's identity: type, name and its VIN controller. | primary guest only |  |
| `IAV_IOC_VIN_SET_DEVINFO` | Set the sensor id and gain step of a virtual video source. | refused | debug: overrides the sensor device description |
| `IAV_IOC_VIN_GET_MODE` | Read the current mode of a video source. | primary guest only |  |
| `IAV_IOC_VIN_SET_MODE` | Set the mode of a video source. | primary guest only |  |
| `IAV_IOC_VIN_GET_FPS` | Read the frame rate of a video source. | primary guest only |  |
| `IAV_IOC_VIN_SET_FPS` | Set the frame rate of a video source. | primary guest only |  |
| `IAV_IOC_VIN_GET_VIDEOINFO` | Read the video information of a video source. | primary guest only |  |
| `IAV_IOC_VIN_GET_SHUTTER` | Read the sensor shutter time. | primary guest only |  |
| `IAV_IOC_VIN_SET_SHUTTER` | Set the sensor shutter time. | primary guest only |  |
| `IAV_IOC_VIN_GET_AGC` | Read the sensor gain. | primary guest only |  |
| `IAV_IOC_VIN_SET_AGC` | Set the sensor gain. | primary guest only |  |
| `IAV_IOC_VIN_SET_AGC_SHUTTER` | Set the sensor gain index and shutter together. | primary guest only |  |
| `IAV_IOC_VIN_GET_DGAIN_RATIO` | Read the sensor digital gain. | primary guest only |  |
| `IAV_IOC_VIN_SET_DGAIN_RATIO` | Set the sensor digital gain. | primary guest only |  |
| `IAV_IOC_VIN_GET_MIRROR` | Read the sensor mirror and flip mode. | primary guest only |  |
| `IAV_IOC_VIN_SET_MIRROR` | Set the sensor mirror and flip mode. | primary guest only |  |
| `IAV_IOC_VIN_GET_EISINFO` | Read the sensor electronic image stabilization information. | primary guest only |  |
| `IAV_IOC_VIN_GET_REG` | Read a sensor register. | refused | raw sensor register access; owned by the Dom0 sensor driver |
| `IAV_IOC_VIN_SET_REG` | Write a sensor register. | refused | raw sensor register access; owned by the Dom0 sensor driver |
| `IAV_IOC_VIN_SHUTTER2ROW` | Convert a shutter time to sensor rows. | primary guest only |  |
| `IAV_IOC_VIN_GET_WDR_AGAIN_INDEX_GP` | Read the analog gain index of a wide-dynamic-range sensor. | primary guest only |  |
| `IAV_IOC_VIN_GET_WDR_DGAIN_INDEX_GP` | Read the digital gain of a wide-dynamic-range sensor. | primary guest only |  |
| `IAV_IOC_VIN_GET_WDR_SHUTTER_ROW_GP` | Read the shutter rows of a wide-dynamic-range sensor. | primary guest only |  |
| `IAV_IOC_VIN_WDR_SHUTTER2ROW` | Convert shutter time to rows for a wide-dynamic-range sensor. | primary guest only |  |
| `IAV_IOC_VIN_GET_AAAINFO` | Read the sensor information the 3A algorithms need. | primary guest only |  |
| `IAV_IOC_VIN_GET_CHIP_STATUS` | Read the sensor chip status. | primary guest only |  |
| `IAV_IOC_VIN_SET_WDR_GP` | Set the parameters of a wide-dynamic-range sensor. | primary guest only |  |
| `IAV_IOC_VIN_SET_AE_CHECK` | Set the sensor auto-exposure check flag. | primary guest only |  |
| `IAV_IOC_VIN_AAA_COMPENSATION` | Set the 3A compensation of a sensor. | primary guest only |  |
| `IAV_IOC_VIN_SET_STREAM_MODE` | Put the sensor into test-stream mode. | primary guest only |  |
| `IAV_IOC_VIN_SET_LOW_LIGHT_MODE` | Set the sensor low-light mode. | primary guest only |  |
| `IAV_IOC_VIN_SWITCH_VSRC_CTX` | Switch contexts on sensors that support two. | primary guest only |  |
| `IAV_IOC_VIN_LOCK_VSRC_CTX_FRAME_CNT` | Bind the frame counter to each context of a two-context sensor. | primary guest only |  |
| `IAV_IOC_VIN_UNLOCK_VSRC_CTX_FRAME_CNT` | Unbind the frame counter from the contexts. | primary guest only |  |
| `IAV_IOC_VIN_WDR_SHUTTER2OFFSET` | Convert a shutter time to sensor row and offset values in HDR mode. | primary guest only |  |
| `IAV_IOC_VIN_SET_TP_MODE` | Put the sensor into test-pattern mode. | primary guest only |  |
| `IAV_IOC_VIN_GET_VIRT_MODE` | Read the virtual format of a virtual video source. | primary guest only |  |
| `IAV_IOC_VIN_SET_VIRT_MODE` | Set the virtual format of a virtual video source. | primary guest only |  |
| `IAV_IOC_VIN_SET_ROI_INFO` | Set the sensor region-of-interest offset. | primary guest only |  |
| `IAV_IOC_VIN_GET_MSYNC` | Read the master sync settings. | primary guest only |  |
| `IAV_IOC_VIN_SET_MSYNC` | Set master sync. | primary guest only |  |
| `IAV_IOC_VIN_GET_GLOBAL_INFO` | Read the global VIN information: number of video sources and their VIN controllers. | primary guest only |  |
| `IAV_IOC_VIN_SET_GLOBAL_MODE` | Set the mode of several VINs at once. | primary guest only |  |

#### VOUT

| Ioctl | What it does | Supported | Reason when not served |
|---|---|---|---|
| `IAV_IOC_VOUT_GET_MODE` | Read the VOUT device format. | primary guest only |  |
| `IAV_IOC_VOUT_SET_MODE` | Set the VOUT device format. | primary guest only |  |
| `IAV_IOC_VOUT_GET_PARAMS` | Read the VOUT parameters. | primary guest only |  |
| `IAV_IOC_VOUT_SET_PARAMS` | Set the VOUT parameters. | primary guest only |  |
| `IAV_IOC_VOUT_SWITCH_VIDEO` | Turn a VOUT on or off. | primary guest only |  |
| `IAV_IOC_VOUT_HALT` | Halt a VOUT. | primary guest only |  |
| `IAV_IOC_VOUT_SYNC_IMG_OSD` | Synchronize the image and the on-screen display of a VOUT. | refused | carries a physical address a guest must not supply; display output is owned by Dom0 |
| `IAV_IOC_VOUT_CVBS_GET_PARAMS` | Read the analog (CVBS) output parameters. | primary guest only |  |
| `IAV_IOC_VOUT_CVBS_SET_COLORBAR` | Show a color bar on the analog output. | refused | analog and MIPI electrical tuning of the display output; owned by Dom0 |
| `IAV_IOC_VOUT_CVBS_SET_BLACKLEVEL` | Set the analog output black level. | refused | analog and MIPI electrical tuning of the display output; owned by Dom0 |
| `IAV_IOC_VOUT_CVBS_SET_BLANKLEVEL` | Set the analog output blank level. | refused | analog and MIPI electrical tuning of the display output; owned by Dom0 |
| `IAV_IOC_VOUT_CVBS_SET_SYNCLEVEL` | Set the analog output sync level. | refused | analog and MIPI electrical tuning of the display output; owned by Dom0 |
| `IAV_IOC_VOUT_CVBS_SET_YGAIN` | Set the analog output luma gain. | refused | analog and MIPI electrical tuning of the display output; owned by Dom0 |
| `IAV_IOC_VOUT_CVBS_SET_CGAIN` | Set the analog output chroma gain. | refused | analog and MIPI electrical tuning of the display output; owned by Dom0 |
| `IAV_IOC_VOUT_MIPI_GET_PARAMS` | Read the MIPI DSI output parameters. | primary guest only |  |
| `IAV_IOC_VOUT_MIPI_SET_TXVCMSET` | Set the MIPI DSI transmitter common-mode voltage. | refused | analog and MIPI electrical tuning of the display output; owned by Dom0 |
| `IAV_IOC_VOUT_MIPI_SET_TXIBCTRL` | Set the MIPI DSI transmitter bias current. | refused | analog and MIPI electrical tuning of the display output; owned by Dom0 |
| `IAV_IOC_VOUT_MIPI_SET_TXPIB` | Set the MIPI DSI transmitter pre-bias. | refused | analog and MIPI electrical tuning of the display output; owned by Dom0 |
| `IAV_IOC_VOUT_MIPI_SET_TXTERM` | Set the MIPI DSI transmitter termination. | refused | analog and MIPI electrical tuning of the display output; owned by Dom0 |
| `IAV_IOC_VOUT_SYNC_VIDEO_OSD` | Synchronize video and on-screen display on a VOUT. | refused | carries a physical address a guest must not supply; display output is owned by Dom0 |

#### IMAGE (3A statistics and image DSP debug)

| Ioctl | What it does | Supported | Reason when not served |
|---|---|---|---|
| `IAV_IOC_IMG_GET_LATEST_STATISTICS` | Copy out the latest 3A statistics (CFA, RGB and YUV domains). | refused | 3A statistics belong to the Dom0 3A service; the calls carry user pointers that need inline marshaling (Phase D) |
| `IAV_IOC_IMG_GET_STATISTICS` | Copy out the 3A statistics of a given frame. | refused | 3A statistics belong to the Dom0 3A service; the calls carry user pointers that need inline marshaling (Phase D) |
| `IAV_IOC_IMG_GET_STATISTICS_REPORT_RATE` | Read how often statistics are reported. | primary guest only |  |
| `IAV_IOC_IMG_SET_STATISTICS_REPORT_RATE` | Set how often statistics are reported. | primary guest only |  |
| `IAV_IOC_IMG_GET_IDSP_CFG` | Read the image DSP debug information. | refused | debug and test only |
| `IAV_IOC_IMG_SETUP_IDSP_DEBUG` | Configure image DSP debugging. | refused | debug and test only |
| `IAV_IOC_IMG_GET_MISC_INFO` | Read miscellaneous image pipeline information (extra raw). | refused | 3A statistics belong to the Dom0 3A service; the calls carry user pointers that need inline marshaling (Phase D) |
| `IAV_IOC_IMG_GET_STATISTICS_MAP` | Map the 3A statistics of a given frame. | refused | 3A statistics belong to the Dom0 3A service; the calls carry user pointers that need inline marshaling (Phase D) |
| `IAV_IOC_IMG_GET_VIN_PTS_INFO` | Read VIN timestamp information. | primary guest only |  |
| `IAV_IOC_IMG_GET_LATEST_STATISTICS_MAP` | Map the latest 3A statistics. | refused | 3A statistics belong to the Dom0 3A service; the calls carry user pointers that need inline marshaling (Phase D) |
| `IAV_IOC_IMG_GET_IDSP_CFG_V2` | Read the second version of the image DSP debug information. | refused | debug and test only |

<!-- iav-support-table:end -->

## 7. Trust model

`amba-virt-server` is a single point of failure by design and is built and tested
to withstand a hostile guest (the accepted risk for N1-655, which has no SMMU).

- **What the broker guarantees.** A guest cannot corrupt the server, the real
  driver's state, or another guest's memory through anything the broker mediates.
  Nothing a guest supplies reaches the real driver unvalidated. Handles are
  opaque, bound to the kernel-reported guest id, carry a generation, and a stale
  one is refused.
- **What it cannot.** Without an SMMU, the contents of a window the guest maps
  read-write can be corrupted by that guest. For the DSP window that is DSP working
  memory, so a hostile guest kernel can alter the DSP's output and whoever consumes
  it, and can read it. A careless application corrupting its own window is not the
  broker's concern. This is wider than the RPC boundary and is a documented limit of
  giving a guest a window.
- **The primary guest is trusted more than the others.** It may change the pipeline
  state, which the Dom0 camera pipeline, the frame tap and every other guest's frames
  depend on. The broker still validates every argument and refuses the hazardous
  calls for it (DRAM test, raw DSP commands, DSP reset, debug, partition allocation,
  anything carrying an address), but a primary guest can stop the pipeline or reconfigure
  it, and a hostile primary guest kernel can do that. Which guest is primary is
  decided by the hypervisor (the slice it was given), not by the guest; see section 3.
  The primary set was qualified by the host tests, the reference applications and the
  adversarial run: the guest drives the real pipeline, decodes and draws overlays on the
  devkit, and the adversarial client attacks from the primary role while the consumer runs
  its suite (section 8).
- **The decode and overlay windows hold the primary guest's own pictures.** A consumer cannot map either.
  The overlay window is exactly the partition's content: the real driver keeps its DSP overlay
  command buffers behind it in the same partition and does not report them, so a guest cannot
  reach them. The broker checks every offset a decode or overlay call names against the window
  (the overlay check uses the larger of `total_size` and pitch times height, which the real
  driver does not compare, so a short `total_size` cannot make the DSP read past the buffer).
- **Not the broker's concern.** A defect inside the real IAV driver or the DSP
  firmware, reached with a request the broker validated, belongs to the IAV driver
  owners.

## 8. Verification runbook

Host (no board): `make -C drivers/amba_virt/tools test_host_iav_broker_run`
(table, handles, protocol, lifecycle, structure-aware and stateful fuzzing, with
the mock driver as oracle), `fuzz_iav_broker_run`, `test_host_iav_broker_tsan_run`,
`test_iav_broker_negctl MUTANT=<name>` (each broken copy must fail), and
`test_iav_qual_host_run`.

Native (Dom0, real driver): `amba-virt-server-qual --iav-qual --mode tests|bench`
built by `make -C drivers/amba_virt/tools amba-virt-server-qual` and run by hand from
`/persist/amba-virt-dev` with `nsenter -t 1 -m --`; it holds the production worker
and resumes it after. `--mode windows` runs the window group.

Guest: `amba_virt_demo --iav-qual --backend real --mode tests|bench --results <file>`
inside the guest, against `/dev/iav`. The same suite runs natively and in a guest, so
their results compare row by row.

Role: add `--role primary` or `--role consumer` to a guest run to say which role it
expects. The role-dependent group (`IavPrimary`, read-only configuration calls) then
requires the primary to be served and a consumer to be refused with `EPERM`; without
a role it records what it sees and judges nothing. The guest that holds slice 0 is
the primary.

Adversarial: `guest-os/client/IavAttack.cxx` (built as `iav-attack`, with
`--role primary|consumer` for the role of the guest it runs in) talks to the broker over
raw vsock without `iav_virt.ko`. It sends malformed and oversized frames, forged and
stale handles, ioctls from the deny list, out-of-window mappings, floods and slow
readers. The gate runs it in one guest (`tools/iav-qual/l8_gate.sh`) while the other
guest runs the suite as the victim, and checks from Dom0 that the server kept its worker,
threads, descriptors and memory, that the kernel log gained no fault, and that the victim's
results are identical to its run without the attacker. Run it only on an image with the
host connection fixes ([AmbaVirtServer.md](AmbaVirtServer.md) section 3.1): earlier
hosts crashed or stalled.

### 8.1 Reference applications and the gates that run them

The applications that show the virtualization works are amba-virt's own, in `guest-os/apps/iav-ref/`, built from this
tree alone by `make iav-ref` as static aarch64 executables. The same binary runs on the bare board (Dom0) and in a guest;
a guest is told apart only by reading `/sys/class/misc/iav/virtualized` (1 in a guest, absent on bare metal), and no
application changes what it does because of it.

| Application | Class | What it shows |
|---|---|---|
| `IavRefCapture` | consumer | canvas frames read in a guest have the same hash as the native copy read at the same moment |
| `IavRefEvents` | consumer | frame events arrive at the camera's rate; the cost of a call and of a wait (section 9) |
| `IavRefStream` | consumer | an MJPEG stream read through the descriptors and the bitstream window is a valid JPEG per frame, in order, at the camera rate |
| `IavRefPipeline` | primary | the guest stops the stream, changes the quality, restarts and restores it; the step sequence equals the native run's. `--probe denied` and `--probe hazard` show what is refused |
| `IavRefDecode` | primary | a JPEG is decoded by the hardware decoder from the guest; the frame equals the native decode bit for bit and agrees with a software decode (PSNR on luma) |
| `IavRefOverlay` | primary | a white box with a black border is blended into the stream: the box is there in the guest-read frame, the rest of the frame is unchanged; overlays outside the window are refused |

`tools/iav-ref/ref_gate.py` runs one application on both sides and compares them; `tools/iav-ref/concurrent_gate.py`
runs both guests at once and restarts the server under load. `tools/iav-qual/l8_gate.sh`
runs the adversarial client (`iav-attack --role primary`) in the primary guest while the other guest runs its suite.
The candidate server for these runs is the qualification build started by `tools/iav-ref/dev_server.sh`
([AmbaVirtServer.md](AmbaVirtServer.md) section 14.3); it serves the guests without the Dom0 frame tap and starts stream 1 itself.
With the production server on the final image the same applications run for what the tap leaves free: `capture` in both guests,
`decode` in the primary, the role probes, and the overlay (`tools/iav-ref/overlay_prod_gate.py`: the primary sets the overlay with
`IavRefOverlay --no-frames`, and a frame taken from each guest's demo stream at `http://127.0.0.1:8080/live/stream` (the Alpine
demo listens on 8081) shows the box while it is held and not after). Each envelope of the plan was checked by a gate sealed with the hub's `scripts/plan_gate.py`; the raw logs of every run are
the evidence (`plan/plan_iav_virtualization_phase2.artifacts/gates/`).

**Steps a new guest needs.** A guest created from an app instance with the window adapters starts with no `/dev/iav`:
`sudo modprobe iav_virt` (the demo service does it at start). Ubuntu needs cloud-init user data at create
([EVE-ReconfigureEdgeApps.md](EVE-ReconfigureEdgeApps.md) section 4.2). The decoder and the overlay windows need the host
`amba_virt_iav_win.ko` with their nodes before the guest starts.

## 9. Measured native versus guest

Measured on 2026-10-12 on `n1-655-devkit` (the pro board has the same SoC and was not repeated), on the final build
(gate `F-attempt1`, raw logs in the plan artifacts), in the Ubuntu guest (the primary) and the Alpine guest (a consumer),
against the same qualification suite run natively in Dom0. The native runs were made with the camera pipeline of the
shipped configuration and the guest runs with the candidate server serving the pipeline with the decoder channel; no guest
or Dom0 reader was running during a measurement. **Correctness:** the 56 tests that run give the same result natively and
in each guest (0 differences of 56). **Performance:** each figure is the median over three runs of the suite (3000 samples
per run for the latency rows, 600 for the 1080p frame copy, 150 for the 16 MiB copy), with the range of the three run
medians in the generated reports (`gates/F-work/parity_ubuntu.md`, `parity_alpine.md`, made by
`tools/iav-qual/compare_results.py`); the ranges are narrow, a few per cent.

| Operation | Native | Ubuntu guest | Alpine guest | What the guest pays |
|---|---|---|---|---|
| `GET_IAV_STATE` (one call) | 0.46 us | 50.6 us | 39.6 us | one vsock round trip per call |
| `QUERY_MEMBLOCK`, `QUERY_INFO` (one call) | 0.78 to 0.90 us | 53.6 to 55.4 us | 43.5 to 43.8 us | the same |
| `QUERY_DESC` canvas, non-blocking | 1.9 us | 55.3 us | 49.4 us | the same |
| `GET_IAV_STATE`, 1 / 2 / 4 threads calling at once (mean per call) | 0.36 us | 45.9 / 107 / 236 us | 40.7 / 91.2 / 186 us | one request at a time on the guest's channel, so calls from several threads queue |
| `poll()` with an event already pending | 0.60 us | 0.90 us | 0.76 us | answered from the module's own state |
| `mmap` + `munmap` of the 10 MiB bitstream buffer | 46 us | 180 us | 135 us | the mapping call asks the broker |
| Copy of a 3.1 MB 1080p frame out of the mapping | 3.37 ms | 5.68 ms | 5.64 ms | reading a window from a VM |
| Copy of 16 MiB out of the DSP mapping | 18.7 ms | 30.2 ms | 31.4 ms | the same |

The reference application `IavRefEvents` gives the same picture on the stream's frame events (one 60-event, 300-wait run
each side): 30.2 events per second in the guest against 30.3 natively, and a `GET_IAV_STATE` call at a median of 42 us
(99th percentile 116 us) in the guest against 0.40 us (0.66 us) natively. The longest wait for the next frame after a
poll phase was 35 ms in the guest and 44 ms natively (one frame interval is 33 ms): in these 300 waits the 20 ms poll
slice added no tail beyond the native one.

**Decode** (`IavRefDecode`, 200 passes per row, one run per row, so no spread is available; the first figure of each pair
is native, the second is the Ubuntu guest, medians):

| JPEG | Copy the JPEG into the window | Hardware decode | Read the NV12 frame out | One pass with read-out |
|---|---|---|---|---|
| 1280x720, 107 KB | 25 us, 520 us | 11.4 ms, 8.7 ms | 4.1 ms, 7.2 ms | about 15.5 ms, 16.4 ms |
| 1920x1080 | 39 us, 871 us | 6.9 ms, 15.7 ms | 9.3 ms, 16.5 ms | about 16.2 ms, 33.1 ms |

Without the read-out the 1080p pass is 11.7 ms natively and 15.4 ms in the guest. The decode time itself is not a stable
difference: the four native rows (two sizes, with and without read-out) range from 6.9 ms to 13.4 ms and the four guest rows
from 8.7 ms to 15.7 ms, and one run per row cannot separate the platform from the camera load on the DSP; no claim is made
that the guest decodes faster or slower. What is clear is the window: copying in costs about 20 times more and reading out
about 1.8 times more in the guest, which makes a 1080p pass with a full read-out about 33 ms there (30 fps is the limit for
that case) against about 16 ms natively. A guest that needs the decoded frame at a higher rate than that would scale it on the
DSP first, which needs `SET_IMG_SCALE` (refused; section 6.0).

So control calls cost tens of microseconds more per call, and frame data is read at about 0.6 of the native rate with no copy
through vsock. An application that makes a few calls per frame is not affected at 30 fps; one that makes thousands of
ioctls per frame is.

## 10. QNX (design only)

The protocol is little-endian, fixed-width, free of Linux types, and carries no
kernel pointers. The broker has no Linux-guest assumption, so the ioctl set can map
onto a resource manager (`devctl` or message passing) later. The guest-visible
address model (offsets into a BAR) does not depend on Linux `remap_pfn_range`. The
suite uses POSIX calls and the IAV UAPI, so it can run on QNX. QNX is not
implemented here.

## 11. Open items

- The DMA32 pool that older documents describe (and that the data-slice design once used as a template) has no
  backing in the device tree since 2026-10-01; restoring or retiring it is a separate project and not part of this
  design.
- Coexistence of the Dom0 frame tap and a guest frame-descriptor consumer (section 6): the driver admits one
  reader, so a guest reads descriptors only while the tap is not running.
- Calls still refused that the ranking of section 12 puts first: `GET_STREAM_CONFIG`, `GET_VIDEO_PROC`,
  `CFG_VIDEO_PROC`, `SET_STREAM_CONFIG`, `QUERY_MEM_LAYOUT_INFO` and `CFG_FRAME_SYNC_PROC` carry argument structs whose
  fields have not been classified as memory or not (each is a read of the struct and a check, not a new window);
  `ALLOC_ANON_MEM_PART`, `FREE_MEM_PART` and `GDMA_COPY` need a server-owned staging pool (no SMMU).
- Video decode (H.264 and H.265) is served by name but was exercised only with JPEG; the reference application
  for it is not written.
- One decoder channel exists, on a virtual VIN (`ambds`). The pro board's models carry the new adapters, but the pro
  board's guests were not replaced and its image was not changed (the SoC is the same as the devkit's).
- The adversarial (Level 8) run passed on both boards on the final image after two host `amba_virt` defects that a
  hostile guest process could trigger were fixed (connection replacement racing the receive worker, which
  crashed a board; a guest-send path holding the global connection lock while blocked on a guest that
  stopped reading, which stalled every tenant). The attacker's table sweep knows the decode and overlay calls as primary calls
  and skips answers to earlier, timed-out requests.

## 12. Compatibility statement

**What the reference applications show.** The six applications of section 8.1 give the same result in a guest as on the
bare board, from the same binary: frames read (consumer), the pipeline driven, a JPEG decoded by the hardware decoder, and
an overlay blended into a stream (primary). That is the evidence that the served classes of calls work from a guest.

**Which bare-metal applications run unmodified.** An application runs in a guest, unmodified, when all of these hold:

1. every `IAV_IOC_*` call it makes is served to its role (section 6.1): a consumer application uses only the 15 calls
   served to every guest; an application that configures or runs the pipeline, decodes or draws overlays must run in the
   primary guest;
2. it uses no other channel than `/dev/iav`: `/dev/ucode`, netlink, `/dev/mem`, Lua and `/proc/ambarella/*` are
   not served (a guest has neither `/dev/ucode` nor `/proc/ambarella`, observed in the Alpine guest), so an application
   that reads `/proc/ambarella/clock` must tolerate its absence;
3. it reads and writes a mapped window with accesses the guest can make (aligned, at most eight bytes at a time; section 6.0),
   which is the case for an application that copies whole frame planes from aligned offsets but not for one that
   `memcpy`s encoded frames from arbitrary offsets of the bitstream window;
4. it does not need a pointer, address or file-descriptor argument that the table refuses (section 6.0, first row).

**What the Cooper test applications ask.** A static reading of the sources of the 43 Cooper test applications that call the
IAV driver (the applications were not run; names in source may sit on paths an application does not take, and the
reading cannot see the arguments) gives these counts against the support table of section 6.1:

- 9 applications name only served calls: `test_canvas_overlay`, `test_decode`, `test_dsp_health`, `test_fps_suggest`, `test_irq_sync`,
  `test_loadbmp`, `test_osd`, `test_srs_stream` and `test_vin_cap`. This does not mean they run: for example
  `test_canvas_overlay` makes canvas overlays, which are refused by argument, `test_decode` also decodes video, which
  was not exercised, and `test_vin_cap` uses netlink.
- 34 applications name at least one refused call.

The refused calls by the number of applications that name them, with the cause:

| Applications | Call | Cause |
|---|---|---|
| 11 | `GET_STREAM_CONFIG` | argument struct not yet classified field by field (a read-only get; a read of the struct and a check, no new window) |
| 11 | `GET_VIDEO_PROC` | the same |
| 8 | `ALLOC_ANON_MEM_PART` | allocates memory the windows are built from; needs a server-owned staging pool (**no SMMU**) |
| 7 | `FREE_MEM_PART` | the same |
| 7 | `GDMA_COPY` | carries physical addresses (**no SMMU**); would be served by offset into a window |
| 6 | `CFG_VIDEO_PROC` | argument struct not yet classified |
| 6 | `QUERY_MEM_LAYOUT_INFO` | argument struct not yet classified |
| 4 | `CFG_FRAME_SYNC_PROC`, `SET_STREAM_CONFIG` | argument struct not yet classified (it contains the overlay, blur and remap unions) |
| 3 | `ALLOC_ANON_MEM_PART_FD`, `CFG_WARP_CTRL`, `GET_WARP_CTRL`, `RELEASE_CANVAS_BUF` | file-descriptor memory (**no SMMU**); warp needs a window (not done); canvas buffer release belongs with the feeds |
| 2 | `ALLOC_MEM_PART`, `FEED_CANVAS_BUF`, `FEED_PYRAMID_BUF`, `RELEASE_PYRAMID_BUF`, `SET_IMG_SCALE`, `WAIT_IMG_SCALE`, `VOUT_SYNC_VIDEO_OSD`, `IMG_SETUP_IDSP_DEBUG`, `CFG_CANVAS_FRAME_SYNC_PROC`, `GET_CANVAS_FRAME_SYNC_PROC`, `GET_FRAME_SYNC_PROC` | partition allocation and buffer feeds (**no SMMU**), address-carrying calls (**no SMMU**), a debug call (**inherent**: hazardous), unclassified structs |
| 1 | `ENABLE_DRAM_TEST`, and the rest of the list | hazardous (**inherent**) or as above |

The full list is `plan/plan_iav_virtualization_phase2.artifacts/app_ioctl_rank_final.txt`, made by `app_ioctl_rank.py`
from the same sources and the support table.

**What to serve next.** In this order: the unclassified read-only gets (`GET_STREAM_CONFIG`, `GET_VIDEO_PROC`,
`QUERY_MEM_LAYOUT_INFO`), which need only a read of each struct and a check; then the configuration calls that share
those structs; then a server-owned staging pool for `ALLOC_ANON_MEM_PART`, `FREE_MEM_PART` and `GDMA_COPY`, which is the
largest single group and the one an SMMU would make unnecessary.
