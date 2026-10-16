# Ambarella USB Host Controller Passthrough

> **Security boundary:** `amba-virt-server` is a trusted root Dom0
> service in the signed EVE image. The untrusted boundary is the HVM RPC
> and the guest `/dev/amba_virt` UAPI ([Architecture.md](Architecture.md#security-boundary)).

*Copyright (C) 2026, Ambarella International LLC*

---

## 1. Overview

This guide describes physical USB host-controller passthrough for the
Ubuntu 24.04 HVM (`ubuntu_24_04`) on the N1-655 boards (`n1-655-devkit`
and `n1-655-pro`). The guest takes the Cadence xHCI platform device
`xhci-hcd.0.auto` through Stage-2 MMIO (`vfio-platform`) and a dedicated
32 MiB DRAM carveout. USB transfers stay on the controller's AXI path.
They do not pass through Dom0's USB stack or QEMU userspace.

The rejected path is QEMU `usb-host`, `qemu-xhci`, `libusb` redirection,
and EVE `IO_TYPE_USB` (PhyIo type 2) device hotplug. The model adapter is
`IO_TYPE_USB_CONTROLLER` (PhyIo type 13). The Ubuntu manifest interface
is named `USB` with numeric type `3` and `directattach: true`. Its
`phyaddrs.path` is `xhci-hcd.0.auto`.

Related documents:
- [EVE-Ambarella-Models.md](EVE-Ambarella-Models.md) (adapter inventory)
- [EVE-BaseOS-AmbarellaDrivers.md](EVE-BaseOS-AmbarellaDrivers.md) (§3.1 memory map)
- [UART-Passthrough.md](UART-Passthrough.md) (the same `vfio-platform` shape for UART)
- [AmbaVirtDMA.md](AmbaVirtDMA.md) (the separate UART DMA32 pool)

## 2. Controller hierarchy

On `n1_655.dts` the USB3 block has three layers. Only the xHCI child is
handed to the guest.

| Layer | Node | Role while the guest owns xHCI |
|---|---|---|
| Glue | `cdns3@ff3c000000` (`ambarella,cdns-usb3`) | Stays bound in Dom0. Programs the scratchpad strap and the onboard hub power and reset GPIOs. |
| DRD / PHY | child `cdns,usb3` at `0xff3c008000` | Stays bound in Dom0 with runtime PM held on, so the Torrent PHY (`cdns,torrent-phy` at `0xfffe100000`) and the OTG host role stay powered. Its `host`, `peripheral`, and `otg` interrupts are the same GIC SPI 198. While `xhci-hcd.0.auto` is not bound to `xhci-hcd`, the DRD drops that handler so `vfio-platform` can request the line without `IRQF_SHARED`. |
| xHCI | `xhci-hcd.0.auto` | Bound to `vfio-platform`. MMIO `0xff3c008000`–`0xff3c017fff` (64 KiB). GIC SPI 198 (`0xc6`), level-high. |

`xhci-hcd.0.auto` is created by `platform_device_alloc` and has no OF
`compatible` string. The in-tree VFIO platform probe falls back to the
device name, and `reset_required` is false so the bind does not look for
an out-of-tree reset module. `amba_virt_usb_dma` only scrubs the
carveout when the guest mapping is released. It must not read or write
the xHCI registers: that load raised an asynchronous SError and panicked
Dom0. The guest driver resets the controller on probe.

Unbinding the DRD parent powers the PHY off. That is not the passthrough
sequence.

## 3. DMA carveout

The xHCI controller has no SMMU stream. It writes host physical addresses.
Every xHCI structure and every URB buffer the guest allocates must sit in
this window:

| Region | Physical range | Size | Use |
|---|---|---|---|
| `virt_usb_dma@15c00000` | `0x15c00000`–`0x17bfffff` | 32 MiB | Whole carveout. `no-map`, below 4 GiB, 2 MiB aligned. Above U-Boot `reloc-top` (`0x13000000`) and below `dma32@17c00000`. |
| Header | `0x15c00000`–`0x15c00fff` | 4 KiB | `"AVUD"` descriptor (`magic` `0x41565544`). |
| Rings | `0x15c01000`–`0x15ffffff` | 4 MiB − 4 KiB | Guest `dma_map_ops` for xHCI rings and contexts. |
| URB pool | `0x16000000`–`0x17bfffff` | 28 MiB | `hcd->localmem_pool` for `usbcore` URB bounce buffers, and `dma_alloc_pages` for the `uvcvideo` isochronous buffers. |

Dom0 exposes the window as `/dev/amba_virt_usb_dma` (`amba_virt_usb_dma.ko`,
loaded from `/etc/amba-virt/modules.conf`). Pillar maps that character
device into the guest as an `ivshmem-plain` BAR (`1af4:1110`, 32 MiB,
QEMU id `amba-virt-usb-dma-shm`). This window is not one of the four
16 MiB UART leases in `dma32@17c00000`.

The mmap sets `VM_ALLOW_ANY_UNCACHED`, so KVM maps the BAR Normal-NC at
stage 2. Without it, a `no-map` page is mapped Device. Guest `memset`
then zeroes with `DC ZVA`, which raises an alignment fault on Device
memory. KVM logs `Unsupported FSC: EC=0x24 xFSC=0x21` and QEMU exits
with `kvm run failed Bad address`. The guest maps the BAR write-combine.
No CPU caches the window, and the controller has no hardware coherency.

The DRAM Address Segment Table is not programmed to confine `USB3H0`.
The fault register for host controller 0 is `0xff0800dd00` (DRAM
controller `0xff08000000`, host-0 table at `0xff0800c000`, fault word at
offset `0x1d00`). A qualification read expects that word to stay 0 for
the capture. The table itself remains the identity map described in the
plan research notes; passthrough does not turn it into an isolation
boundary.

## 4. Guest enumeration

QEMU emits ACPI `Device (USB0)` with `_HID "AMBA0002"` only when the
VFIO platform host name is `xhci-hcd.0.auto`. The `_CRS` entry is a
64 KiB `Memory32Fixed` plus a level-high interrupt. `_CCA` is 0. `_DSD`
carries `dma-phys-base` `0x15c00000` and `dma-phys-size` `0x02000000`.
UART devices still match `ambarella,uart` / `AMBA0001`.

`amba_virt_usb.ko` in the Ubuntu HVM matches `AMBA0002` as a platform
driver, finds the 32 MiB BAR by the `AVUD` header, and registers the stock
xHCI HCD. Its `dma_map_ops` hand out the rings window to
`dma_alloc_coherent`. Single-buffer maps are bounced through the URB
pool. Scatter-gather is not advertised (`sg_tablesize` 0): with
`localmem_pool` set, usbcore bounces only `transfer_buffer`, so an SG
URB would fail. Mass storage still works one URB per segment, and UAS
falls back to `usb-storage`.

`uvcvideo` does not use `usb_alloc_coherent`. It allocates its URB buffers
with `dma_alloc_noncontiguous` on the HCD device. That becomes a
`dma_alloc_pages` call, whose CPU address is `page_address()`. The driver
therefore gives the URB pool `struct page`s with `devm_memremap_pages`
(`MEMORY_DEVICE_GENERIC`). The guest linear alias is Normal WB at stage 1.
Combined with Normal-NC at stage 2, it is still uncached.

`uvcvideo` and `v4l-utils` come from `linux-modules-extra-<kernel>` and
`v4l-utils`. The lean Ubuntu guest image does not ship them;
`guest-os/deploy_guest.sh <node>-ubuntu --provision --reload` installs
them for the running kernel, along with `amba_virt_usb.ko` and the demo. Each camera has two `uvcvideo` nodes with the same
name. The second is a metadata node. The demo picks the lowest-numbered
node whose `VIDIOC_QUERYCAP` reports `V4L2_CAP_VIDEO_CAPTURE`. The demo's USB Webcam tab feeds
`/dev/video0` MJPEG (`1280x720` default, `1920x1080` selectable) through
a JPEG decode and letterbox resize into YOLOX-S. The decode runs on the IAV
DSP through the virtual `/dev/iav` when the server admits the frame, and on
the CPU otherwise (`doc/IavVirtualization.md`, section 9). Measured on
`n1-655-pro` (fixed exposure, 60 s per run):

| Fixed exposure, 60 s | fps | Decode p50 / p99 (ms) | Total p50 / p99 (ms) | Guest CPU (2 vCPUs) | Demo, share of one core |
|---|---|---|---|---|---|
| 1280x720, IAV hardware | 30.0 | 8.1 / 19.4 | 16.8 / 28.0 | 12.0 % | 18.0 % |
| 1280x720, CPU | 31.0 | 16.0 / 20.5 | 22.7 / 28.4 | 35.0 % | 63.3 % |
| 1920x1080, IAV hardware | 30.0 | 11.9 / 23.6 | 22.3 / 33.4 | 14.5 % | 19.4 % |
| 1920x1080, CPU | 23.5 | 35.6 / 36.6 | 42.2 / 45.6 | 50.1 % | 94.2 % |

When the adapter is assigned, Pillar suppresses the emulated `qemu-xhci`
controller on that domain. The cloud-init ISO remains a CD, and on that
domain its QEMU device is `virtio-scsi-pci` with `scsi-cd`. Image
`66ecff22` still uses `usb-storage` for that CD and will not start the
domain.

## 5. Assignment

`USB` belongs on `ubuntu_24_04`. The controller rejects a new interface
while that app still has instances
([EVE-ReconfigureEdgeApps.md](EVE-ReconfigureEdgeApps.md)). Delete
`ubuntu_24_04.n1-655-pro` and `ubuntu_24_04.n1-655-devkit` first (their
volumes go with them), add `USB` to `ubuntu_24_04`, then create those
two instances again with the adapter. Do not create an `IO_TYPE_USB`
(type 2) adapter, and do not invent a second edge-app.

```bash
./scripts/pull_app.sh ubuntu_24_04
./scripts/add_app_direct.sh ubuntu_24_04 --if=USB --force
./scripts/create_instance.sh ubuntu_24_04.n1-655-pro \
  --edge-app=ubuntu_24_04 \
  --edge-node=n1-655-pro \
  --network-instance=eth0:defaultLocal-n1-655-pro \
  --adapter=amba_shm:amba_shm0 \
  --adapter=UART2:UART2 \
  --adapter=USB:USB
```

Repeat the instance create for `n1-655-devkit`. The model `assigngrp`
is `USB`. `phyaddrs.path` is `xhci-hcd.0.auto`.

BaseOS delivery is the A/B update in
[EVE-UpdateEVE-Firmware.md](EVE-UpdateEVE-Firmware.md). Guest modules and
`amba-virt-demo` are copied by `make deploy-hvm-ubuntu
TARGET=<node>-ubuntu`. A device-tree change that adds
`virt_usb_dma@15c00000` also requires the boot0 procedure in
[EVE-Installation.md](EVE-Installation.md) §6, because GRUB forwards the
tree U-Boot already patched.

## 6. What Dom0 and the guest should show

On `n1-655-pro`, with the AVerMedia PW313 (`07ca:313a`) on the onboard hub:

- Dom0 has `xhci-hcd.0.auto` under `vfio-platform` and a character device
  `/dev/amba_virt_usb_dma`. `lsusb -d 07ca:313a` in Dom0 does not see the
  camera.
- The Ubuntu guest has `/sys/bus/platform/drivers/amba_virt_usb/AMBA0002:00`,
  `lsusb -d 07ca:313a` lists the camera, and `/dev/video0` exists.
- `v4l2-ctl --stream-mmap` at `1280x720` and `1920x1080` MJPEG delivers
  distinct, decodable frames. The AVerMedia camera holds 30 fps with a
  fixed exposure. In dim light its default auto exposure (aperture
  priority) slows it to about 20 fps.
