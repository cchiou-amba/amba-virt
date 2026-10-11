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
 |  - poll in 100 ms slices|                      |  +------------+------------+  |
 |  - mmap of the windows  |                      |               | real ioctl    |
 +------------+------------+                      |               v               |
              | mmap (ivshmem BAR)                |       +---------------+       |
              v                                   |       |    iav.ko     |       |
 +-------------------------+   same physical      |       +-------+-------+       |
 | DSP window, BSB window  |<-------------------->|               v               |
 +-------------------------+   DRAM below 4 GiB   |        DSP / IDSP hardware    |
                                                  +-------------------------------+
```

Control stays on vsock and bulk data stays in shared memory. No bulk data
crosses vsock.

## 2. Backing real driver requirements

| Item | Where |
|---|---|
| `iav.ko` and the camera chain (`dsp`, `imgproc`, `ambnl`, `ambcma`, `hw_timer`, `cavalry`) | loaded by `amba-virt-server`; the live camera pipeline must be up (`/dev/iav` exists only then) |
| `amba_virt.ko` (host) | vsock transport and the data slices |
| `amba_virt_iav_win.ko` | exposes the DSP partition and the bitstream buffer as mappable nodes (section 4) |
| Memory | the IAV driver's own partitions: `IAV_PART_DSP` (live: 1 GiB at `0x40000000`) and `IAV_PART_BSB` (live: 10 MiB at `0x84300000`) on N1-655. Every DSP buffer is below 4 GiB (the DSP is 32-bit) |
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
  them), rebuild, regenerate the ioctl table (`tools/iav-policy/`) and read what changed, since
  a new upstream ioctl starts refused, then re-run the native and guest suites and the
  adversarial run.

## 3. Guest virtual driver interface

**Node:** `/dev/iav`, registered by `iav_virt.ko` (mode 0660). The module needs
`amba_virt.ko` (it uses its vsock channel and its PCI probe leaves the IAV
windows unbound for it).

**Supported ioctls (15):** `GET_IAV_STATE`, `GET_CHIP_ID`, `QUERY_ENC_MODE_CAP`,
`QUERY_ENC_BUF_CAP`, `QUERY_IAV_CAP`, `GET_SYSTEM_RESOURCE`, `GET_CHAN_CONFIG`,
`GET_CANVAS_CONFIG`, `DRV_DSP_INFO`, `QUERY_INFO`, `QUERY_MEMBLOCK`,
`QUERY_DESC` (frame, canvas and pyramid descriptors, never a dma-buf fd),
`SET_POLL_EVENT_CFG`, `CLEAR_POLL_EVENT_CFG`, `WAIT_NEXT_IDSP_FRAME`.
Nothing that changes pipeline state and nothing that carries an address in.
Every other ioctl of the real driver (190 of 205) fails with `EPERM`: a guest can
query and read frames, but cannot configure or run the pipeline, decode, set up
overlays or warp, or use the calls that carry addresses or pointers. A legacy
application that uses only the supported calls runs; the others do not. Extending the
set means adding a reviewed entry to the allow-list in
`drivers/amba_virt/tools/iav_policy.c`; `tools/iav-policy/gen_iav_ioctl_table.py`
regenerates the table of all 205.

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

**poll:** a worker asks the broker in slices of at most 100 ms, so `poll()`
never sleeps in the transport. **Blocking:** a blocking canvas or pyramid query
is forwarded as blocking; the real driver bounds it (two seconds, then `EAGAIN`).
Frame query waits are clamped.

**One channel per guest:** the vsock channel carries one request at a time, so a
blocking call delays other calls from the same guest for the length of its wait.

## 4. The windows

`amba_virt_iav_win.ko` registers four nodes, two sets of the same two windows:

| Node | Covers | Size |
|---|---|---|
| `/dev/amba_virt_iav_dsp`, `/dev/amba_virt_iav_dsp1` | the DSP partition | a power of two (live: 1 GiB) |
| `/dev/amba_virt_iav_bsb`, `/dev/amba_virt_iav_bsb1` | the bitstream buffer, padded | a power of two (live: 16 MiB for a 10 MiB buffer) |

- **Configuration.** `amba-virt-server` configures each node once at start, from
  the partition table the real driver reports (`AMBA_VIRT_IAV_WIN_IOC_CONFIG`,
  root only), before any VM starts. The module refuses a range outside
  `0x28000000` to 4 GiB, a misaligned or overflowing range, padding over 64 MiB,
  a reconfiguration while mapped, and a private mapping.
- **Padding** behind the real range (to reach a power of two, as a PCI BAR needs)
  is private zeroed RAM, cacheable, in 2 MiB chunks. It never reaches the real
  range. The last 64 bytes of padding carry an identity header (`AMIW`, window id,
  length), which lets the guest tell a window from a data slice. A window with no
  padding (the DSP partition when it is already a power of two) is recognized by
  elimination: the unbound ivshmem device of the DSP window's size that carries no
  slice claim.
- **Mapping attributes.** The real range is mapped write-combined, which is what
  makes a frame the DSP wrote visible without cache maintenance.
- **Template and its open item.** The window design follows the DMA32 slice code (a misc node
  per window, a guest that classifies a window by its exact BAR size, requests that carry
  offsets only and are bounds-checked). The DMA32 pool that code was written for has had no
  device-tree backing since 2026-10-01, so none of its memory is used here; restoring or
  retiring the pool is a separate project (section 11).
- **Adapters.** Each node is one model adapter (`IO_TYPE_OTHER`, empty
  `phyaddrs`, `cbattr` `shmpath` and `shmsize`), exactly like `amba_shm`:
  `amba_iav_dsp`, `amba_iav_bsb`, `amba_iav_dsp1`, `amba_iav_bsb1`
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
- **One IAV user of the frame descriptors.** The driver allows one process to
  query frame descriptors of the shared bitstream buffer. While the Dom0 camera
  tap runs it is that user, so a guest's frame-descriptor query fails (`EFAULT`)
  until the tap is not running. Canvas and pyramid queries are not affected.
  Production coexistence of the tap and a guest frame consumer is not solved.
- **One pipeline owner.** The broker serves the consumer profile: queries, frame
  and canvas access, polls. Configuring or starting the pipeline from a guest is
  not supported.

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

Adversarial: `guest-os/client/IavAttack.cxx` (built as `iav-attack`) talks to the broker over
raw vsock without `iav_virt.ko`. It sends malformed and oversized frames, forged and
stale handles, ioctls from the deny list, out-of-window mappings, floods and slow
readers. The gate runs it in one guest (`tools/iav-qual/l8_gate.sh`) while the other
guest runs the suite as the victim, and checks from Dom0 that the server kept its worker,
threads, descriptors and memory, that the kernel log gained no fault, and that the victim's
results are identical to its run without the attacker. Run it only on an image with the
host connection fixes ([AmbaVirtServer.md](AmbaVirtServer.md) section 3.1): earlier
hosts crashed or stalled.

## 9. Measured native versus guest

Measured on 2026-10-11 on `n1-655-devkit` and `n1-655-pro`, in the Ubuntu and the Alpine guest of each,
against the same suite run natively in Dom0 (the Dom0 frame tap not running, so a guest can query frame
descriptors). **Correctness:** the 55 tests give the same result natively and in all four guest and board
combinations, with no difference. **A guest-read frame is the frame the DSP wrote:** 24 consecutive canvas
frames read by the guest have the same Y-plane hash as the Dom0 witness's own read of the same frame
(the witness reads through its own mapping). **Performance:** each figure below is the range of the
medians over the four combinations, from three runs per side (3000 samples per run for the latency rows).

| Operation | Native | Guest | What the guest pays |
|---|---|---|---|
| `GET_IAV_STATE`, `QUERY_MEMBLOCK`, `QUERY_INFO` (one call) | 0.46 to 0.90 us | 38 to 54 us | one vsock round trip per call (about 40 us) |
| `QUERY_DESC` canvas, non-blocking | 1.9 us | 45 to 57 us | the same round trip |
| 4 threads calling at once (mean per call) | 0.36 us | 182 to 241 us | one request at a time on the guest's channel, so calls from several threads queue |
| `poll()` with an event already pending | 0.58 us | 0.78 to 0.98 us | answered from the module's own state |
| `mmap` + `munmap` of the 10 MiB bitstream buffer | 46 us | 135 to 187 us | the mapping call asks the broker |
| Copy of a 3.1 MB 1080p frame out of the mapping | 3.4 ms | 5.6 to 5.7 ms | reading the shared window from a VM |
| Copy of 16 MiB out of the DSP mapping | 18.8 to 19.0 ms | 29.4 to 30.0 ms | the same |

So control calls cost tens of microseconds more per call, and frame data is read at 0.6 of the native
rate with no copy through vsock. An application that makes a few calls per frame is not affected at 30 fps;
one that makes thousands of ioctls per frame is. The full tables, with p95, sample counts and the spread of
the three runs, are generated from the recorded results by `tools/iav-qual/compare_results.py`.

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
- Coexistence of the Dom0 frame tap and a guest frame-descriptor consumer (section 6).
- The adversarial (Level 8) run passed on both boards on the final image after two host `amba_virt` defects that a
  hostile guest process could trigger were fixed (connection replacement racing the receive worker, which
  crashed a board; a guest-send path holding the global connection lock while blocked on a guest that
  stopped reading, which stalled every tenant).
- The supported set is 15 of the real driver's 205 ioctls (section 3): pipeline configuration and run control,
  the decode data path, overlays, warp and video-processing configuration, and the address-carrying calls
  are open. Legacy applications that use only the supported calls run; the rest fail with `EPERM`.
