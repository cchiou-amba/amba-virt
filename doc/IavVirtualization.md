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
| Memory | the IAV driver's own partitions on N1-655: `IAV_PART_DSP` (live: 1 GiB at `0x40000000`), `IAV_PART_BSB` (live: 10 MiB at `0x84300000`), `IAV_PART_OVERLAY` (live: 8 MiB of content at `0x28000000`) and, only when the resource configuration has a decoder channel, `IAV_PART_DEC_BSB` (live on `n1-655-pro`, image 14.31: 6 MiB at `0xb4400000`; the address follows the boot's allocation order, so read it from `/proc/ambarella/cma`). Every DSP buffer is below 4 GiB (the DSP is 32-bit) |
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
  (the image ships `primary-slice 0`. The `amba_shm0` ticket is reserved at
  Cavalry index 0; `amba_shm0`..`amba_shm3` carry `camera=on` and `amba_shm4`..`amba_shm7` have camera off).
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

The DSP and bitstream windows of every attached guest (and the decode and overlay windows of the primary) are writable BARs. The N1-655 has no SMMU. QEMU `memory-backend-file` `readonly=on` opens the node read-only, the kernel clears `VM_SHARED` on that mapping, and `iav_win_mmap` rejects it, so QEMU never starts. A store from the guest through those BARs reaches the live frame and bitstream. Ioctl checks do not stop that store. Private JPEG memory is a 512 MiB `ambcma` partition inside `iav@0`, leased as eight 64 MiB slices, one device node `/dev/amba_virt_iav_sliceN` per guest. A private submission is admitted against the caller's slice before any IAV ioctl.

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

`amba_virt_iav_win.ko` registers four nodes, one set of the four windows. Every guest the camera policy attaches the
windows to maps the same four nodes:

| Node | Covers | Size |
|---|---|---|
| `/dev/amba_virt_iav_dsp` | the DSP partition | a power of two (live: 1 GiB) |
| `/dev/amba_virt_iav_bsb` | the bitstream buffer, padded | a power of two (live: 16 MiB for a 10 MiB buffer) |
| `/dev/amba_virt_iav_dec` | the decode partition (`IAV_PART_DEC_BSB`): the JPEG or bitstream in, the decoded picture out | a power of two (live: 8 MiB for the 6 MiB of one decoder) |
| `/dev/amba_virt_iav_ovl` | the overlay partition (`IAV_PART_OVERLAY`): colour tables and pixels of stream overlays | a power of two (live: exactly 8 MiB, no padding) |

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
- **Attached from the lease, not assigned.** The windows are not model adapters. When a guest's admission ticket is
  leased, `amba-virt-server` decides from the ticket's `camera` intent, a per-ticket Dom0 override and whether the nodes
  were configured whether the guest gets the windows, and answers with the sizes; `kvm.go` emits one `ivshmem-plain`
  device per window over the one set of nodes ([EVE-Ambarella-Models.md](EVE-Ambarella-Models.md), "Camera windows and the
  camera policy"). Every attached guest maps the same nodes, so the number of guests is not limited by the number of
  window sets. The decode and overlay windows go only to the guest on Cavalry slice 0.
- **Shared padding, and reconfiguring.** The padding behind a window's real range is one allocation per node, mapped by
  every attached guest, so it is memory the attached guests share and can write. The module refuses to reconfigure a node
  while any guest maps it: reconfiguring the windows (a camera resource change) needs every attached guest stopped.

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

All 205 ioctls of the IAV driver on this platform: 15 served to every guest, 146 served to the primary guest only, 44 refused (`EPERM`).

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
| `IAV_IOC_GDMA_COPY` | Copy a 2-D block between memory partitions with the GDMA engine. | primary guest only |  |
| `IAV_IOC_QUERY_DESC` | Query frame descriptors: encoded frames, canvas, pyramid, raw and other frame-based data. | every guest |  |
| `IAV_IOC_QUERY_MEMBLOCK` | Query the address and size of the memory partitions shared with applications. | every guest |  |
| `IAV_IOC_EFM_GET_POOL_INFO` | Read the external frame manager (EFM) pool information. | primary guest only |  |
| `IAV_IOC_EFM_HANDSHAKE_FRAME` | Hand a processed external frame back to the DSP for encoding. | refused | names frame buffers of the external pool (IAV_PART_USR) by index and offset; the pool and its pin tracking are not served yet (phase 2) |
| `IAV_IOC_EFM_RELEASE_ALL_FRAMES` | Release every EFM frame buffer the application holds. | primary guest only |  |
| `IAV_IOC_EFM_REQUEST_FRAME` | Take one EFM frame buffer to fill or process. | refused | names frame buffers of the external pool (IAV_PART_USR) by index and offset, with a dma-buf file descriptor arm; the pool and its pin tracking are not served yet (phase 2) |
| `IAV_IOC_EFM_SET_EXT_POOL` | Provide an application-owned EFM buffer pool. | refused | binds an external pool by partition id; external pools and their pin tracking are not served yet (phase 2) |
| `IAV_IOC_GET_RAW_ENCODE` | Read the raw-encode configuration. | primary guest only |  |
| `IAV_IOC_SET_RAW_ENCODE` | Set the raw-encode configuration; idle state only. | refused | raw encode needs the raw buffer pool, which is not served yet (phase 2) |
| `IAV_IOC_SET_RAW_ENC_CFG` | Initialize the raw-encode procedure; idle state only. | refused | carries physical addresses of the raw and half-decimated buffers; needs the raw pool with pin tracking (phase 2) |
| `IAV_IOC_WAIT_RAW_ENCODE` | Wait until the raw-encode path can accept the next buffer. | refused | blocking wait that is unbounded when back-pressure is on; the broker needs its own bound |
| `IAV_IOC_ALLOC_ANON_MEM_PART_FD` | Allocate an anonymous partition and return a dma-buf file descriptor for it. | refused | returns a file descriptor a guest cannot hold |
| `IAV_IOC_ALLOC_MEM_PART` | Allocate a named IAV/DSP memory partition. | refused | allocates or frees memory the guest windows are built from; the server owns it |
| `IAV_IOC_FREE_MEM_PART` | Free a named or anonymous memory partition. | refused | allocates or frees memory the guest windows are built from; the server owns it |
| `IAV_IOC_FEED_PYRAMID_BUF` | Feed a buffer to the pyramid scaler in manual-feed mode. | primary guest only |  |
| `IAV_IOC_RELEASE_PYRAMID_BUF` | Release a pyramid buffer locked by a descriptor query. | primary guest only |  |
| `IAV_IOC_SYNC_CACHE_MEM_PART` | Clean or invalidate the CPU cache for a partition shared with the DSP. | primary guest only |  |
| `IAV_IOC_FEED_CANVAS_BUF` | Feed a buffer to a canvas in manual-feed mode. | primary guest only |  |
| `IAV_IOC_RELEASE_CANVAS_BUF` | Release a canvas buffer locked by a descriptor query. | primary guest only |  |
| `IAV_IOC_RESET` | Reset the driver and the DSP to the initial state. | refused | resets the driver and the DSP for every client |
| `IAV_IOC_GET_MEM_LAYOUT_RESOURCE` | Read the memory layout resource configuration. | primary guest only |  |
| `IAV_IOC_QUERY_MEM_LAYOUT_INFO` | Query the resulting memory layout (partition addresses and sizes). | primary guest only |  |
| `IAV_IOC_SET_MEM_LAYOUT_RESOURCE` | Configure the memory layout; init state only. | refused | allocates or frees memory the guest windows are built from; the server owns it |
| `IAV_IOC_ALLOC_ANON_MEM_PART` | Allocate an anonymous memory partition the application can map. | refused | allocates or frees memory the guest windows are built from; the server owns it |
| `IAV_IOC_REGISTER_ANON_MEM_PART` | Register an existing memory range as an anonymous partition. | refused | allocates or frees memory the guest windows are built from; the server owns it |
| `IAV_IOC_UNREGISTER_ANON_MEM_PART` | Unregister an anonymous partition. | refused | allocates or frees memory the guest windows are built from; the server owns it |
| `IAV_IOC_CLEAR_POLL_EVENT_CFG` | Clear the descriptor events configured for poll. | every guest |  |
| `IAV_IOC_SET_POLL_EVENT_CFG` | Select which descriptor events wake a poll on the device. | every guest |  |
| `IAV_IOC_FEED_RAW_BUF` | Feed a raw buffer to a channel in manual-feed mode. | primary guest only |  |
| `IAV_IOC_RELEASE_RAW_BUF` | Release a raw buffer locked by a descriptor query. | primary guest only |  |
| `IAV_IOC_SET_VOUT_MAX_RES_CFG` | Set the maximum VOUT resources; init state only. | primary guest only |  |
| `IAV_IOC_CE_GO_MAP_HANDSHAKE_BUFFER` | Hand a contrast-enhancement gain/offset map buffer to the DSP. | primary guest only |  |
| `IAV_IOC_CE_GO_MAP_REQUEST_BUFFER` | Take a contrast-enhancement gain/offset map buffer for processing. | primary guest only |  |

#### Encode control (0x40 to 0x5F)

| Ioctl | What it does | Supported | Reason when not served |
|---|---|---|---|
| `IAV_IOC_GET_STREAM_CONFIG` | Read the configuration of a stream. | primary guest only |  |
| `IAV_IOC_SET_STREAM_CONFIG` | Change the configuration of a stream while it runs. | primary guest only |  |
| `IAV_IOC_GET_H264_CONFIG` | Read the H.264 encoding configuration of a stream. | primary guest only |  |
| `IAV_IOC_SET_H264_CONFIG` | Set the H.264 encoding configuration of an idle stream. | primary guest only |  |
| `IAV_IOC_GET_H265_CONFIG` | Read the H.265 encoding configuration of a stream. | primary guest only |  |
| `IAV_IOC_SET_H265_CONFIG` | Set the H.265 encoding configuration of an idle stream. | primary guest only |  |
| `IAV_IOC_GET_MJPEG_CONFIG` | Read the MJPEG quality and chroma format of a stream. | primary guest only |  |
| `IAV_IOC_SET_MJPEG_CONFIG` | Set the MJPEG quality and chroma format of an idle stream. | primary guest only |  |
| `IAV_IOC_APPLY_FRAME_SYNC_PROC` | Apply frame-synchronized operations for the video DSP. | primary guest only |  |
| `IAV_IOC_APPLY_IDSP_ENC_SYNC_PROC` | Apply frame-synchronized operations between the image DSP and the encoder. | primary guest only |  |
| `IAV_IOC_CFG_FRAME_SYNC_PROC` | Configure frame-synchronized operations. | primary guest only |  |
| `IAV_IOC_GET_FRAME_SYNC_PROC` | Read the frame-synchronized operation configuration. | primary guest only |  |
| `IAV_IOC_GET_OVERLAY_INSERT` | Read the overlays inserted into a stream. | primary guest only |  |
| `IAV_IOC_SET_OVERLAY_INSERT` | Insert picture overlays into a stream. | primary guest only |  |
| `IAV_IOC_GET_FPS_SUGGEST` | Read the suggested frame rate of each stream. | primary guest only |  |
| `IAV_IOC_FLUSH_FRAMEDESC` | Flush a stream's queued frame descriptors, optionally forcing an IDR frame first. | refused | drops queued frame descriptors for every consumer of a stream and can wait 5 s, as long as the guest call deadline; needs a longer per-call deadline |
| `IAV_IOC_GET_BLUR_COLOR` | Read the blur color tables. | primary guest only |  |
| `IAV_IOC_GET_BLUR_INSERT` | Read the blur (privacy) areas inserted into a stream. | primary guest only |  |
| `IAV_IOC_SET_BLUR_COLOR` | Set the blur color tables. | primary guest only |  |
| `IAV_IOC_SET_BLUR_INSERT` | Insert blur (privacy) areas into a stream. | primary guest only |  |
| `IAV_IOC_GET_YUV_REMAP_INSERT` | Read the YUV remap areas inserted into a stream. | primary guest only |  |
| `IAV_IOC_SET_YUV_REMAP_INSERT` | Insert YUV remap areas into a stream. | primary guest only |  |
| `IAV_IOC_APPLY_CANVAS_FRAME_SYNC_PROC` | Apply canvas frame-synchronized operations. | primary guest only |  |
| `IAV_IOC_CFG_CANVAS_FRAME_SYNC_PROC` | Configure canvas frame-synchronized operations. | primary guest only |  |
| `IAV_IOC_GET_CANVAS_FRAME_SYNC_PROC` | Read the canvas frame-synchronized operation configuration. | primary guest only |  |
| `IAV_IOC_GET_POSTP_SCALE_CTRL` | Read the post-processing scale control of a stream. | primary guest only |  |
| `IAV_IOC_SET_POSTP_SCALE_CTRL` | Set the post-processing scale control of a stream. | primary guest only |  |

#### VIN, IDSP and VCAP control (0x60 to 0x6F)

| Ioctl | What it does | Supported | Reason when not served |
|---|---|---|---|
| `IAV_IOC_GET_VCAP_CFG` | Read the video capture (VCAP) configuration. | primary guest only |  |
| `IAV_IOC_SET_VCAP_CFG` | Set the video capture (VCAP) configuration. | primary guest only |  |
| `IAV_IOC_APPLY_WARP_CTRL` | Apply lens-distortion (LDC) and fisheye warp parameters. | primary guest only |  |
| `IAV_IOC_APPLY_WARP_CTRL_IDSP_FRAME_SYNC` | Apply warp parameters synchronized to an image DSP frame. | primary guest only |  |
| `IAV_IOC_CFG_WARP_CTRL` | Configure lens-distortion and fisheye warp parameters. | primary guest only |  |
| `IAV_IOC_GET_WARP_CTRL` | Read the warp parameters. | primary guest only |  |
| `IAV_IOC_APPLY_VIDEO_PROC` | Apply the video processing set up by the configuration call (privacy mask, DPTZ, canvas and others). | primary guest only |  |
| `IAV_IOC_CFG_VIDEO_PROC` | Configure privacy mask, digital pan/tilt/zoom and related processing. | primary guest only |  |
| `IAV_IOC_GET_VIDEO_PROC` | Read the video processing configuration. | primary guest only |  |
| `IAV_IOC_GET_VIN_IDSP_CFG` | Read the VIN image DSP configuration. | primary guest only |  |
| `IAV_IOC_SET_VIN_IDSP_CFG` | Set the VIN image DSP configuration. | refused | the DSP reads iso_addr_offset without a range check, and the call is the 3A control output that the Dom0 3A service writes, so a guest would be a second writer |
| `IAV_IOC_SET_VIN_IDSP_DELAY_INFO` | Set the image DSP delay information used by warp. | primary guest only |  |
| `IAV_IOC_WAIT_NEXT_IDSP_FRAME` | Wait for the next valid image DSP frame. | every guest |  |
| `IAV_IOC_GET_VIN_COMPAND_CFG` | Read the VIN companding configuration. | primary guest only |  |
| `IAV_IOC_SET_VIN_COMPAND_CFG` | Set the VIN companding configuration. | primary guest only |  |
| `IAV_IOC_SET_IMG_SCALE` | Submit an arbitrary-size image scaling job to the DSP. | primary guest only |  |
| `IAV_IOC_WAIT_IMG_SCALE` | Wait for an image scaling job to finish. | primary guest only |  |
| `IAV_IOC_WAIT_SKIP_IDSP_CFG` | Wait until a VIN switch of a channel has completed. | primary guest only |  |
| `IAV_IOC_SYNC_VIN_FRAME` | Wait for the next valid image DSP frame. | primary guest only |  |

#### AISP (0x70 to 0x7F)

| Ioctl | What it does | Supported | Reason when not served |
|---|---|---|---|
| `IAV_IOC_GET_ISP_NN_CFG` | Read the neural-network parameters of the image pipeline. | primary guest only |  |
| `IAV_IOC_SET_ISP_NN_CFG` | Set the neural-network parameters of the image pipeline; idle state only. | refused | carries physical addresses of the DAG and port maps that the DSP follows and the broker cannot bound; refused regardless |
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
| `IAV_IOC_IMG_GET_LATEST_STATISTICS` | Copy out the latest 3A statistics (CFA, RGB and YUV domains). | refused | every statistics call frees a frame or clears the resend flag of the Dom0 3A service, so a guest read would starve it; needs a non-consuming read |
| `IAV_IOC_IMG_GET_STATISTICS` | Copy out the 3A statistics of a given frame. | refused | every statistics call frees a frame or clears the resend flag of the Dom0 3A service, so a guest read would starve it; needs a non-consuming read |
| `IAV_IOC_IMG_GET_STATISTICS_REPORT_RATE` | Read how often statistics are reported. | primary guest only |  |
| `IAV_IOC_IMG_SET_STATISTICS_REPORT_RATE` | Set how often statistics are reported. | primary guest only |  |
| `IAV_IOC_IMG_GET_IDSP_CFG` | Read the image DSP debug information. | refused | debug and test only |
| `IAV_IOC_IMG_SETUP_IDSP_DEBUG` | Configure image DSP debugging. | refused | debug and test only |
| `IAV_IOC_IMG_GET_MISC_INFO` | Read miscellaneous image pipeline information (extra raw). | primary guest only |  |
| `IAV_IOC_IMG_GET_STATISTICS_MAP` | Map the 3A statistics of a given frame. | refused | every statistics call frees a frame or clears the resend flag of the Dom0 3A service, so a guest read would starve it; needs a non-consuming read |
| `IAV_IOC_IMG_GET_VIN_PTS_INFO` | Read VIN timestamp information. | primary guest only |  |
| `IAV_IOC_IMG_GET_LATEST_STATISTICS_MAP` | Map the latest 3A statistics. | refused | every statistics call frees a frame or clears the resend flag of the Dom0 3A service, so a guest read would starve it; needs a non-consuming read |
| `IAV_IOC_IMG_GET_IDSP_CFG_V2` | Read the second version of the image DSP debug information. | refused | debug and test only |

<!-- iav-support-table:end -->

#### Calls served since the first table, and the restriction each carries

All of these are for the primary guest. A guest that has not negotiated the arena or the aperture gets only the calls whose arguments need neither. No call serves `use_phys = 1`, a dma-buf file descriptor, or a physical address a guest supplied; each is refused before the driver sees it.

| Call | Restriction the broker enforces |
|---|---|
| `DECODE_JPEG` | Offsets inside the decode window; the JPEG header, read by the broker from the window before the call, must be baseline sequential, 8 bit, three components sampled 4:2:0 or 4:2:2, the four standard Huffman tables, no restart interval, no APPn other than one JFIF APP0 and no COM; a decode-only call's `yuv_size` must hold the whole picture the header describes; else `EINVAL` (a JPEG the decoder does not take stalls it, and the DSP writes the whole picture whatever `yuv_size` says; section 9). |
| `SET_IMG_SCALE`, `WAIT_IMG_SCALE` | Both buffers in the decode window (`IAV_PART_DEC_BSB`), `use_phys = 0`, `use_dma_buf_fd = 0`; every plane and the crop and output windows (as the DSP rounds them) inside the buffer. |
| `GDMA_COPY` | Source and destination inside windows or the caller's aperture entries; destination decode or overlay only; width and height non-zero. |
| `SYNC_CACHE_MEM_PART` | Windows and the caller's aperture entries only; invalidate only over the guest's own content (an invalidate discards Dom0's dirty lines). |
| `SET_BLUR_INSERT`, `GET_BLUR_INSERT`, `CFG_CANVAS_FRAME_SYNC_PROC`, `CFG_FRAME_SYNC_PROC`, `GET_CANVAS_FRAME_SYNC_PROC`, `GET_FRAME_SYNC_PROC`, `SET_STREAM_CONFIG`, `GET_STREAM_CONFIG` | Ids bounded (stream below 32, canvas below 20); normal blur only when the BLUR partition is reported; arbitrary blur only against the ARB_BLUR aperture entry; YUV remap against its entry; QP ROI with `qpm_raw = 0` against its entry; the canvas overlay against its entry; `CUSTOM_SEI` and `qpm_raw = 1` refused. |
| `SET_YUV_REMAP_INSERT`, `GET_YUV_REMAP_INSERT` | Entry present, stream id below 32, the CLUT inside the entry. |
| `CFG_VIDEO_PROC`, `GET_VIDEO_PROC` | Listed cids only (mask, DPTZ, canvas fps and VV sync, VSRC update, canvas overlay, pyramid ROI and fps, canvas order, VCAP flush, post-processing scale, dummy latency, CAWARP); `MULTI_BLEND` and `BLEND_WARP` (both set and get) and every other cid are refused because the partitions behind them are absent. |
| `CFG_WARP_CTRL`, `GET_WARP_CTRL` | MAIN, DPTZ and MAX_CFG; map offsets checked in 64 bits against the entry; `ENCOFS` and unknown cids refused. |
| `CE_GO_MAP_REQUEST_BUFFER`, `CE_GO_MAP_HANDSHAKE_BUFFER` | Entry present; `buffer_idx` below 8, channel below 4. |
| `IMG_GET_MISC_INFO` | Inline marshaling, negotiated guests only. |
| `FEED_RAW_BUF`, `FEED_CANVAS_BUF`, `FEED_PYRAMID_BUF`, `RELEASE_RAW_BUF`, `RELEASE_CANVAS_BUF`, `RELEASE_PYRAMID_BUF`, `EFM_GET_POOL_INFO`, `EFM_RELEASE_ALL_FRAMES` | Ids bounded below the driver's feed tables; stream in the current stream map. |
| `GET_ISP_NN_CFG` | Physical addresses cleared in the answer. |
| `GET_VIN_IDSP_CFG`, `QUERY_MEM_LAYOUT_INFO`, `CFG_VIDEO_PROC` and the apply calls | Channel and VIN ids below 32; apply flags limited to the audited maps. |
| `WAIT_SKIP_IDSP_CFG` | Wait bounded by the driver at 2 s. |

#### Calls that stay refused, and why

| Call | Reason |
|---|---|
| `SET_ISP_NN_CFG` | Physical addresses of the DAG and port maps, which the DSP follows and the broker cannot bound. |
| `SET_RAW_ENC_CFG`, `SET_RAW_ENCODE` | Physical addresses of raw buffers; needs the raw pool with pin tracking (phase 2). |
| `EFM_HANDSHAKE_FRAME`, `EFM_REQUEST_FRAME`, `EFM_SET_EXT_POOL` | External pool `IAV_PART_USR` and its pin tracking, and the dma-buf arm (phase 2). |
| `SET_VIN_IDSP_CFG` | Unchecked `iso_addr_offset` the DSP reads; the Dom0 3A service is the writer. |
| `IMG_GET_STATISTICS`, `IMG_GET_STATISTICS_MAP`, `IMG_GET_LATEST_STATISTICS`, `IMG_GET_LATEST_STATISTICS_MAP` | Each call frees a frame or clears the resend flag of the Dom0 3A service; needs a non-consuming read. |
| `FLUSH_FRAMEDESC` | Can wait 5 s, as long as the guest call deadline; needs a longer per-call deadline. |
| `ALLOC_MEM_PART`, `ALLOC_ANON_MEM_PART`, `FREE_MEM_PART`, `REGISTER_ANON_MEM_PART`, `UNREGISTER_ANON_MEM_PART`, `SET_MEM_LAYOUT_RESOURCE` | The server owns memory allocation. |
| `CFG_VIDEO_PROC` and `GET_VIDEO_PROC` with `MULTI_BLEND` or `BLEND_WARP` | The partitions behind these cids are absent. |

The generated table above is authoritative for every other call and its reason.

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

**Steps a new guest needs.** A guest created with a ticket that gets the camera windows starts with no `/dev/iav`:
`sudo modprobe iav_virt` (the demo service does it at start). Ubuntu needs cloud-init user data at create
([EVE-ReconfigureEdgeApps.md](EVE-ReconfigureEdgeApps.md), the ticket recipe). The decoder and the overlay windows need the host
`amba_virt_iav_win.ko` with their nodes before the guest starts; the server attaches them only when its nodes were configured.

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
that case) against about 16 ms natively. A guest that needs the decoded frame at a higher rate than that scales it on the
DSP first (`SET_IMG_SCALE`, served since plan_iav_usb_yolo Envelope D); the next subsection measures that chain.

**Sustained decode + scale, and the USB webcam** (`IavRefPreproc`, plan_iav_usb_yolo Envelope H; observed on 2026-10-16 on
`n1-655-pro`, the Ubuntu primary guest and Dom0, gate `H-attempt2`, 300 counted frames after 10 warm-up frames per run,
medians, the deployed memory type; Dom0 has one CPU, the guest two). One decoder is kept; each frame is copied into the
decode window, decoded (`DECODE_JPEG`), scaled to a 640x360 window of a 640x640 NV12 buffer (`SET_IMG_SCALE`,
`WAIT_IMG_SCALE`), read out whole (640x640 NV12, 614400 bytes) and converted to planar RGB on the CPU. The software path is
the demo's own `stbi_load_from_memory` decode and `ResizeLetterbox640`, linked from the demo source, on the same frames.
The frames are 16 camera frames per size (AVerMedia 07ca:313a, auto exposure) re-encoded as 4:2:0 JPEG (see below), used
cyclically; native and guest runs produce the same output hashes (chain hash equal for all four pairs).

| 4:2:0 frames (ms, p50) | Copy in | Decode | Scale | Read out | NV12 to RGB | Hardware sum (p99) | Software: decode + resize |
|---|---|---|---|---|---|---|---|
| 1280x720, guest | 0.93 | 9.20 | 1.22 | 3.08 | 1.45 | 15.9 (23.3) | 18.5 + 1.0 = 19.5 |
| 1280x720, Dom0 | 0.04 | 8.01 | 1.08 | 1.61 | 1.44 | 14.4 (32.6) | 65.9 + 1.0 = 67.9 (one CPU, shared) |
| 1920x1080, guest | 1.84 | 8.74 | 2.13 | 3.06 | 1.44 | 17.8 (32.6) | 40.5 + 1.1 = 41.6 |
| 1920x1080, Dom0 | 0.08 | 18.36 | 2.00 | 1.61 | 1.44 | 24.3 (57.5) | 153.4 + 1.0 = 157.9 (one CPU, shared) |

No frame failed in any run and the pad around the scaled window was never touched. In the guest the hardware chain is
faster than the software path by 1.2 times at 720p (15.9 against 19.5 ms, about 63 against 51 frames per second as a
ceiling of this stage) and by 2.3 times at 1080p (17.8 against 41.6 ms, about 56 against 24 per second). The hardware
decode time itself varied between runs (Dom0 1080p 18.4 ms here against 6.9 to 13.4 ms in the table above); no claim is
made about why.

**The webcam's frames do not decode as they are.** Every MJPEG size the camera offers (320x240 to 1920x1080) is 4:2:2 (SOF sampling `0x21`),
observed from real frames at 320x240, 640x480, 800x600, 1280x720, 1280x960 and 1920x1080; the camera's only other format
is raw YUYV (10 frames per second at 720p, 5 at 1080p). The DSP decoder did not decode them: the first `DECODE_JPEG` of a
4:2:2 camera frame timed out after 5 s (`-EFAULT`), further calls timed out the same way, and after about 170 s of such calls
the DSP asserted (`DSP assertion happened!`), after which canvas queries and the VIN wait timed out and the camera pipeline
could not be restarted; the board was rebooted. The same camera content re-encoded as 4:2:0 decodes (table above). The
driver does not check the JPEG (`iav_ioc_decode_jpeg`, `iav_dec_api.c:1649` to 1790): it hands the offsets to the DSP, and
its reporting of the decoded format (`dec_ch_fmt_to_iav_yuv_format`, `iav_dec_api.c:464` to 483) lists mono, 4:2:0 and 4:2:2,
so no source states that 4:2:2 is unsupported; what is observed is that this camera's frames stall the decoder. Single-call
probes on Pro (one `DECODE_JPEG` each, natively in Dom0, fresh decoder session, raw output in
`plan_iav_usb_yolo.artifacts/j_work/jpeg422/`) separate the cause from the sampling. A clean 4:2:2 JPEG (re-encoded with
libjpeg, SOF `0x21`, JFIF APP0, standard Huffman tables) decoded (format 0, YUV422). The camera frame with its APP0 segment
replaced by a 16-byte JFIF APP0 decoded, and with its APP0 removed decoded; the camera frame itself (APP0 `AVI1`, 33 bytes)
timed out. A 4:2:0 frame given the camera's `AVI1` APP0 timed out. A camera frame losslessly transcoded with optimized
Huffman tables (same quantization tables and coefficients) timed out, and so did a 4:2:0 frame transcoded the same way. After
a timeout the decoder session stays stalled: in that session later calls, including a frame known to decode, also timed out;
a new session (the candidate server restarted) decoded again. What is not known: whether destroying and recreating the decoder
inside the same server session recovers it, and which other APP or COM segments stall it (the rule below refuses all).

The server therefore reads the JPEG header in the decode window before `DECODE_JPEG` reaches the driver and refuses
(`EINVAL`) everything but this (`iav_jpeg_admit`, `iav_policy.c`): baseline sequential (SOF0), 8 bit, three components with Y
sampled 2x2 (4:2:0) or 2x1 (4:2:2) and Cb, Cr 1x1; the four standard Huffman tables of ITU-T T.81 Annex K, byte for byte,
all present; one quantization table at least; no marker before the scan except SOI, DQT, SOF0, DHT and at most one APP0 that
is JFIF (no other APPn, no COM, no DRI). The read is `read_dec` of the broker, an invalidate of the host's cache lines
followed by a read of a read-only mapping of the decode partition. The application (`UsbJpeg::normalize`) makes a frame fit
that rule by its content, never by the device that sent it: it drops a non-JFIF APP0, other APPn and COM segments and inserts
the four standard tables when the frame has none (the UVC Motion-JPEG convention), then applies the same rule
(`UsbJpeg::inspect`) before it submits the frame; a frame the rule refuses (own Huffman tables, progressive, restart
intervals) is decoded by the CPU, with the reason shown. After one failed decode the application stops using the hardware
path until the backend is selected again (a new decoder session). This is a check of the bytes at the time of the call; the
guest still holds the window until the DSP reads it, so it stops a wrong JPEG, not a hostile guest that rewrites the window
in between.

**The decoded picture is bounded too.** The DSP writes the whole picture the header describes, whatever `yuv_size` the
call names (observed on Pro: a 1920x1080 4:2:2 decode given 3 MiB wrote planes ending about 1 MiB past the 6 MiB decode
partition, into memory that was unallocated at the time). The server therefore also refuses (`EINVAL`) a decode-only call
whose `yuv_size` is below the size the header implies (`iav_jpeg_decoded_bytes`: width and height aligned to 16, times 1.5
for 4:2:0 or 2 for 4:2:2). Servers before this change (image 22.08) did not read the header and let a guest with decode
access make the DSP write past the window; update them. The application places the decoded picture 2 MiB into the window
(4 MiB of room) and decodes on the CPU a picture that does not fit.

**Replacing `iav_virt.ko` in a running guest.** The server keeps the protocol version a guest negotiated (`OP_HELLO`) per
vsock CID until the guest goes away. A module of an older protocol loaded into a guest that ran a newer one is answered in
the newer format and fails (observed: `EPROTO` on the decode window mapping). Restart the guest after changing the module
in either direction; a guest that boots with an older module is served in the older format (observed before this plan's server changes; not rechecked on image 14.31).

**The camera's own rate** (V4L2 dequeue, 300 frames, capture only): auto exposure (default) 19.9 frames per second at both
720p and 1080p (interval p50 51.9 ms), fixed exposure (`auto_exposure=1`, `exposure_time_absolute=100`) 30.0 frames per
second (p50 32.0 ms, p99 36.1 ms), no frame dropped. With the software path in the loop at fixed exposure: 720p 30.0 frames
per second (decode + resize 16.8 ms, no drop), 1080p 27.7 (35.3 ms; 24 camera frames dropped, because the stage is slower than
the 33 ms interval); with auto exposure the camera caps both at 19.9. The fixed-exposure frames are dark and small
(23 KB at 720p, 49 KB at 1080p, against 164 KB and 328 KB with auto exposure), which makes their software decode faster
(15.7 and 34.2 ms against 19.8 and 43.3 ms). For the webcam tab the pre-processing stage alone allows about 51 frames per
second at 720p and 23 at 1080p on the software path (auto-exposure frames); the camera's 20 to 30 frames per second is the
cap at 720p, and at 1080p the software stage is below the camera's rate.

**Live webcam to YOLOX-S, hardware against CPU** (plan_iav_usb_yolo gate J; observed on 2026-10-16 on `n1-655-pro`, image
14.31, Ubuntu primary guest, the demo's USB Webcam tab, camera at fixed exposure 100 and gain 200, 60 s per run). Decode is
`DECODE_JPEG` for the hardware rows and the software decode for the CPU rows; total is decode, scale or resize, conversion
and the NPU. No frame was refused (`bad_jpeg` 0), with no Dom0 decode timeout and no DSP assertion.

| Fixed exposure, 60 s | fps | Decode p50 / p99 (ms) | Total p50 / p99 (ms) | Guest CPU (2 vCPUs) | Demo, share of one core |
|---|---|---|---|---|---|
| 1280x720, IAV hardware | 30.0 | 8.1 / 19.4 | 16.8 / 28.0 | 12.0 % | 18.0 % |
| 1280x720, CPU | 31.0 | 16.0 / 20.5 | 22.7 / 28.4 | 35.0 % | 63.3 % |
| 1920x1080, IAV hardware | 30.0 | 11.9 / 23.6 | 22.3 / 33.4 | 14.5 % | 19.4 % |
| 1920x1080, CPU | 23.5 | 35.6 / 36.6 | 42.2 / 45.6 | 50.1 % | 94.2 % |

The hardware path keeps the camera's 30 frames per second at both sizes and uses about a third of the CPU. On the CPU path,
1080p falls below the camera rate. Auto exposure (the camera's default, about 20 frames per second) was not measured on this
image. In this dark scene YOLOX-S detected nothing, so the gate's detection count failed. That is a defect in the gate's
specification, not a decode failure.

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
