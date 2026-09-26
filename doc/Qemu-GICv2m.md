# QEMU IVSHMEM MSI-X Interrupt Delivery on ARM GICv2m

*Document Path: `doc/Qemu-GICv2m.md`*
*Copyright (C) 2026, Ambarella International LLC*

---

> [!NOTE]
> **Architectural & Production Context**:
> This document details the technical root-cause analysis and resolution for PCI MSI-X interrupt delivery under ARM GICv2m for virtual PCIe devices (`ivshmem-doorbell`).
> In production Ambarella CV3-AD655 deployments, physical UART passthrough has been standardized on direct sysbus `vfio-platform` mapping paired with GICv2 level-sensitive KVM `irqfd`/`resamplefd` (GSI 144 / vIRQ 50) and dynamic ACPI DSDT generation (`AMBA0001`), bypassing PCI and MSI-X translation entirely. This document remains preserved as an authoritative reference for PCIe MSI-X virtualization.

## 1. Overview & Problem Statement

During physical UART passthrough validation on ARM64 platforms (such as the
Ambarella CV3-AD655 / N1 SoC family) using `ivshmem-doorbell` under EVE OS
(KVM hypervisor, QEMU 8.0.4 / Xen 4.19 toolstack):

* **Register MMIO & Physical TX function normally**: The guest VM accesses the
  physical UART registers via PCI BAR 2, and outbound serial characters
  transmit across physical UART transceivers without error.
* **Physical RX accumulates in FIFO without interrupt assertion**: Incoming
  characters successfully buffer into the physical UART hardware RX FIFO. The
  guest UART status registers indicate a pending RX interrupt (`IER=0xa7`,
  `IIR=0xc4`, `LSR=0x61`, `RFL=0x2e`).
* **Guest ISR never triggers autonomously**: The guest kernel never receives
  the expected MSI-X virtual interrupt to drain the RX FIFO.
* **Manual injection succeeds**: Directly injecting the programmed MSI-X
  message into the GICv2m frame (`address 0x08020040`, `data 0x63`) immediately
  triggers guest Linux IRQ 70, executes the guest ISR, drains the RX FIFO
  (`tx:64,rx:0` became `tx:130,rx:46`), and resumes bidirectional operation.
  This confirms that the guest driver, ISR, MSI-X table programming, and the
  virtual GIC (vGIC) interrupt path are completely operational.
* **KVM eventfd delivery is broken**: Tracing reveals that while manual MSI
  injection generates `kvm_set_irq` events, signaling the host `ivshmem`
  receive `eventfd` produces zero `kvm_set_irq` events. KVM traces the stale
  GSI **65504**. Peer IDs are correct (QEMU receive peer 0, host ACK peer 1)
  and are not part of this defect.

UART is the **validation probe** for the `ivshmem-doorbell` interrupt fabric,
not the end goal. The same MSI-X path is intended for later virtualized
peripherals that need a guest ISR.

The sole defect is a **stale, out-of-range GSI** cached at MSI-X enable time
when the vector table is still zero. In-kernel `irqfd` SPI injection on GICv2
is supported; QEMU `ivshmem` fails to bind it.

This conclusion is **source-derived** from the vendored QEMU 8.0.4 tree
(`qemu/`) and the EVE kernel (`eve-kernel/arch/arm64/kvm/vgic/`). Autonomous
`irqfd` delivery after the repair has **not** been proven on hardware yet.
The only silicon-proven interrupt path so far is the manual GICv2m MSI write.

---

## 2. What Actually Works on GICv2

### 2.1 `vgic_has_its()` gates only `KVM_IRQ_ROUTING_MSI`

In `eve-kernel/arch/arm64/kvm/vgic/vgic-irqfd.c`, direct kernel MSI routing
(`KVM_IRQ_ROUTING_MSI`) is gated on a virtual GICv3 Interrupt Translation
Service (ITS):

```c
int kvm_set_msi(struct kvm_kernel_irq_routing_entry *e,
                struct kvm *kvm, int irq_source_id,
                int level, bool line_status)
{
    struct kvm_msi msi;

    if (!vgic_has_its(kvm))
        return -ENODEV;
    ...
    return vgic_its_inject_msi(kvm, &msi);
}
```

On GICv2 (CV3-AD655), `vgic_has_its()` is **false**, so **MSI routing
entries** cannot be injected in-kernel.

That gate does **not** apply to plain SPI injection. The adjacent
`kvm_arch_set_irq_inatomic()` switch in the same file:

```c
    case KVM_IRQ_ROUTING_MSI: {
        struct kvm_msi msi;

        if (!vgic_has_its(kvm))
            break;              /* MSI routing entries require an ITS */
        ...
    }

    case KVM_IRQ_ROUTING_IRQCHIP:
        /*
         * Injecting SPIs is always possible in atomic context
         * as long as the damn vgic is initialized.
         */
        if (unlikely(!vgic_initialized(kvm)))
            ...
```

`KVM_IRQ_ROUTING_IRQCHIP` (a virtual SPI) is always injectable and is
**atomic** (no kernel thread hop), provided the vGIC is initialized.

On a platform with GICv3 and an ITS, `KVM_IRQ_ROUTING_MSI` works natively via
`vgic_its_inject_cached_translation()`, QEMU sets
`kvm_gsi_direct_mapping = false` (`hw/intc/arm_gicv3_its_kvm.c`), and the
direct-mapping GSI workaround described below is not used.

### 2.2 Default GSI → SPI routing exists

At vGIC init (`eve-kernel/arch/arm64/kvm/vgic/vgic-init.c`),
`kvm_vgic_setup_default_irq_routing()` installs one `IRQCHIP` route per SPI:

```c
    for (i = 0; i < nr; i++) {
        entries[i].gsi = i;
        entries[i].type = KVM_IRQ_ROUTING_IRQCHIP;
        entries[i].u.irqchip.irqchip = 0;
        entries[i].u.irqchip.pin = i;
    }
```

`vgic_irqfd_set_irq()` then translates pin to GIC INTID:

```c
    unsigned int spi_id = e->irqchip.pin + VGIC_NR_PRIVATE_IRQS;

    if (!vgic_valid_spi(kvm, spi_id))
        return -EINVAL;
    return kvm_vgic_inject_irq(kvm, 0, spi_id, level, NULL);
```

`VGIC_NR_PRIVATE_IRQS` is 32 (SGIs + PPIs). Therefore GSI *n* injects INTID
*n* + 32.

This is why `hw/intc/arm_gicv2m.c` sets:

```c
kvm_gsi_direct_mapping = true;
kvm_msi_via_irqfd_allowed = kvm_irqfds_enabled();
```

Under `kvm_gsi_direct_mapping`, the MSI data word **is** the SPI selector.
`KVM_IRQFD` bound to the derived GSI injects in-kernel with no ITS and no
QEMU wakeup.

### 2.3 GICv2m userspace MMIO is for guest-initiated MSI writes

To support PCI MSI/MSI-X on GICv2, QEMU instantiates an emulated **GICv2m**
widget (`hw/intc/arm_gicv2m.c`) at base address `0x08020000`.

GICv2m is a userspace MMIO device. A write to `0x08020040`
(`V2M_MSI_SETSPI_NS`) runs `gicv2m_write()`:

```c
    case V2M_MSI_SETSPI_NS: {
        int spi;

        spi = (value & 0x3ff) - (s->base_spi + 32);
        if (spi >= 0 && spi < s->num_spi) {
            gicv2m_set_irq(s, spi);
        }
```

`gicv2m_set_irq()` calls `qemu_irq_pulse()`, which raises then lowers the
virtual SPI and issues two `ioctl(KVM_IRQ_LINE)` from userspace.

That path is what a **guest-initiated** MSI write uses. It is **not**
required for host-initiated `ivshmem` doorbell delivery once `irqfd` is
bound to the matching GSI.

### 2.4 Both paths target the same INTID

GICv2m's design is that the MSI data word **is** the GIC INTID. For the
observed vector:

| Quantity | Value | Meaning |
|---|---|---|
| MSI data | `0x63` (99) | GIC INTID programmed by the guest |
| `kvm_arch_msi_data_to_gsi(99)` | `99 - 32` = **67** | ARM helper in `qemu/target/arm/kvm.c` |
| Default route GSI 67 | pin 67 | `KVM_IRQ_ROUTING_IRQCHIP` |
| `spi_id = 67 + 32` | **99** | Same INTID as the MSI data word |

The in-kernel `irqfd` path and the userspace GICv2m MMIO path therefore
deliver the **identical** interrupt.

Guest Linux IRQ **70** observed during bringup is a Linux virq (logical
descriptor number), not a GIC INTID. It does not contradict INTID 99.

---

## 3. The Failure Mechanism in QEMU `ivshmem`

The failure of `ivshmem` doorbell interrupts on ARM GICv2 is **one** defect:
the GSI is snapshotted from a zero MSI data word and never recomputed.

`with_irqfd == true` is the **correct** branch on this platform.
Skipping `watch_vector_notifier()` is intended. QEMU's non-irqfd branch in
`setup_interrupt()` remains the legitimate path for TCG, non-KVM, and plain
`ivshmem` without MSI; it is not a recommended fallback for KVM+GICv2m.

### 3.1 Stale GSI from a zero data word

1. When the guest enables MSI-X in PCI configuration space,
   `ivshmem_write_config()` calls `ivshmem_enable_irqfd()`, which calls
   `ivshmem_add_kvm_msi_virq()` and then `kvm_irqchip_add_msi_route()`.

2. Because `kvm_gsi_direct_mapping` is enabled,
   `kvm_irqchip_add_msi_route()` (`qemu/accel/kvm/kvm-all.c`) derives the GSI
   from `msg.data`:

   ```c
   MSIMessage msg = {0, 0};

   if (pci_available && dev) {
       msg = pci_get_msi_message(dev, vector);
   }

   if (kvm_gsi_direct_mapping()) {
       return kvm_arch_msi_data_to_gsi(msg.data);
   }
   ```

   On ARM (`qemu/target/arm/kvm.c`):

   ```c
   int kvm_arch_msi_data_to_gsi(uint32_t data)
   {
       return (data - 32) & 0xffff;
   }
   ```

   **At MSI-X enable the guest has not yet populated the MSI-X vector table
   (`msg.data == 0`)**. This produces:

   $$(0 - 32)\ \&\ \text{0xffff} = \mathbf{65504}$$

   `s->msi_vectors[vector].virq` is recorded as `65504`. That matches the
   observed KVM trace.

3. Later, when the guest unmasks the vector, `ivshmem_vector_unmask()`
   receives the **correct** `MSIMessage` (`address 0x08020040`,
   `data 0x63`) but never recomputes the GSI, because
   `kvm_irqchip_update_msi_route()` short-circuits under direct mapping:

   ```c
   if (kvm_gsi_direct_mapping()) {
       return 0;  /* no-op: v->virq stays 65504 */
   }
   ```

4. `ivshmem_vector_unmask()` then calls
   `kvm_irqchip_add_irqfd_notifier_gsi(kvm_state, n, NULL, 65504)`.
   `ioctl(KVM_IRQFD)` rejects GSI 65504 as invalid (`-EINVAL`): it is not in
   the default routing table, and `vgic_valid_spi()` would also reject the
   corresponding INTID.

### 3.2 VFIO is the precedent

`qemu/hw/vfio/pci.c` (`vfio_add_kvm_msi_virq`) creates the KVM route at the
moment the vector is used, with the live `MSIMessage` in hand. `ivshmem` is
the outlier: it snapshots the route at capability-enable time, before the
guest writes the vector table.

### 3.3 Summary of the failure state

* **In-kernel `irqfd` registration fails** with `-EINVAL` on GSI 65504.
* **No userspace eventfd listener** is registered, because the irqfd branch
  was correctly selected.
* **Result:** The receive `eventfd` is orphaned. Host doorbell signals hit
  the kernel eventfd but trigger neither in-kernel injection nor a QEMU
  callback.

The correct GSI for `data = 0x63` is **67**, which maps to INTID **99**.

---

## 4. The Surgical Fix: Recompute the Direct-Mapped GSI

Do **not** fall back to `watch_vector_notifier()` for KVM+GICv2m. That path
wakes the QEMU main loop under the BQL and is not a product interrupt fabric
(see §5). QEMU's existing non-irqfd branch stays for TCG / non-KVM /
non-MSI; it is simply not the path used here.

### 4.1 Repair in `ivshmem_vector_unmask()`

In `qemu/hw/misc/ivshmem.c`, re-derive the GSI from the message the guest
actually programmed before binding the irqfd.
`kvm_arch_msi_data_to_gsi()` is already public (`qemu/include/sysemu/kvm.h`).
The `kvm_gsi_direct_mapping()` guard leaves x86, GICv3+ITS, and PPC
XICS/XIVE behaviour unchanged.

```c
static int ivshmem_vector_unmask(PCIDevice *dev, unsigned vector,
                                 MSIMessage msg)
{
    IVShmemState *s = IVSHMEM_COMMON(dev);
    EventNotifier *n = &s->peers[s->vm_id].eventfds[vector];
    MSIVector *v = &s->msi_vectors[vector];
    int ret;

    IVSHMEM_DPRINTF("vector unmask %p %d\n", dev, vector);
    if (!v->pdev) {
        error_report("ivshmem: vector %d route does not exist", vector);
        return -EINVAL;
    }
    assert(!v->unmasked);

    if (kvm_gsi_direct_mapping()) {
        /*
         * Under direct GSI mapping the GSI is derived from the MSI data
         * word, which was still zero when the route was created at
         * MSI-X enable time.  Re-derive it now that the guest has
         * programmed the vector table.
         */
        v->virq = kvm_arch_msi_data_to_gsi(msg.data);
    }

    ret = kvm_irqchip_update_msi_route(kvm_state, v->virq, msg, dev);
    if (ret < 0) {
        return ret;
    }
    kvm_irqchip_commit_routes(kvm_state);

    ret = kvm_irqchip_add_irqfd_notifier_gsi(kvm_state, n, NULL, v->virq);
    if (ret < 0) {
        error_report("ivshmem: add_irqfd_notifier_gsi GSI %d failed",
                     v->virq);
        return ret;
    }
    v->unmasked = true;

    return 0;
}
```

`ivshmem_vector_mask()` and `ivshmem_remove_kvm_msi_virq()` consume the same
`v->virq`, so the corrected value stays symmetric across mask/unmask and
MSI-X disable. No extra bookkeeping is required.

An out-of-range GSI must raise a loud `error_report()` instead of silently
orphaning the eventfd, which is how this defect hid.

Changing `kvm_irqchip_update_msi_route()` itself to recompute and return a
GSI under direct mapping would alter the return contract for every caller,
including VFIO and virtio. The ivshmem-local patch is the minimal change.

### 4.2 Known limitation

If the guest rewrites the MSI-X data word while the vector is **unmasked**,
the irqfd stays bound to the previous GSI. Linux masks before changing
affinity, so this is safe in practice.

### 4.3 Post-fix delivery chain (untested on hardware)

```text
host doorbell write
  -> eventfd
  -> KVM irqfd (atomic, IRQCHIP route)
  -> vGIC pending + list register
  -> vCPU kick
  -> guest ISR
```

QEMU is entirely out of the data path. This chain is inferred from kernel
and QEMU source; it has not been run on the target yet.

---

## 5. Interrupt Delivery Latency & Overhead

Nothing in this repository has been measured. Figures below are
**estimates**, labelled as such, and must be replaced with the two-segment
measurement in §5.4 before any qualification claim.

### 5.1 Path comparison

```text
Native (bare metal reference):
  device -> GIC distributor -> CPU interface -> ISR

In-kernel irqfd (after the fix):
  host device ISR -> eventfd write
    -> KVM irqfd, kvm_arch_set_irq_inatomic(), IRQCHIP route (atomic)
    -> vGIC pending + list register
    -> IPI kick / WFI wake of the target vCPU
    -> guest ISR

Rejected userspace path (current behaviour when irqfd is unused):
  host device ISR -> eventfd write
    -> glib ppoll() wakeup of the QEMU main loop thread
    -> BQL acquisition
    -> ivshmem_vector_notify() -> msix_notify() -> msi_send_message()
    -> address_space_stl_le() into emulated GICv2m MMIO
    -> gicv2m_write() -> qemu_irq_pulse()
    -> 2 x ioctl(KVM_IRQ_LINE)
    -> vGIC pending + vCPU kick
    -> guest ISR
```

Userspace path citations (`qemu/`):

* `hw/misc/ivshmem.c` — `watch_vector_notifier()` / `ivshmem_vector_notify()`
* `util/main-loop.c` — `qemu_set_fd_handler()` on `iohandler_ctx` (main loop,
  BQL)
* `hw/pci/msix.c` — `msix_notify()`
* `hw/pci/msi.c` — `msi_send_message()`
* `hw/pci/pci.c` — `pci_msi_trigger()` → `address_space_stl_le()`
* `hw/intc/arm_gicv2m.c` — `gicv2m_write()` / `gicv2m_set_irq()`
* `include/hw/irq.h` — `qemu_irq_pulse()` is raise + lower
* `hw/intc/arm_gic_kvm.c` / `accel/kvm/kvm-all.c` — `kvm_set_irq()` /
  `KVM_IRQ_LINE`

The expensive parts of the rejected path are not the eventfd. They are
(a) scheduling the QEMU main loop thread, (b) BQL contention against vCPU
exits, and (c) two VM ioctls per interrupt.

### 5.2 Order-of-magnitude estimates (not measured)

* **Native hardware IRQ:** low single-digit microseconds.
* **In-kernel `irqfd`:** a few microseconds above native (eventfd write,
  atomic SPI injection, IPI kick, vCPU re-entry to load the list register).
* **Userspace GICv2m path:** tens of microseconds when idle; tail latency
  potentially hundreds of microseconds to milliseconds under load, with no
  upper bound unless QEMU and vCPU threads are pinned and RT-scheduled.

The GSI repair **does not make delivery native**. The host still takes the
physical interrupt, runs its ISR, and writes the eventfd. Residual overhead
is inherent to the `ivshmem-doorbell` design, not to GICv2. `irqfd` removes
QEMU from the path; it does not remove the host hop.

**Register access is not on this path.** Guest access to UART registers
through the ivshmem BAR is a stage-2 mapping, not a trap. Only the interrupt
crosses the hypervisor boundary, so interrupt rate — not MMIO rate — is what
the delivery path affects.

### 5.3 Coalescing (design constraint)

The eventfd is a counter. In-kernel `irqfd` injects one SPI per signal
regardless of the count, and the vGIC pending bit collapses further. The
userspace path collapses identically via `event_notifier_test_and_clear()`.

Any protocol over `ivshmem-doorbell` must be **status/queue driven**, never
"one interrupt equals one event".

Applicability of this transport (estimates only):

* **Adequate:** UART, I2C/SPI transfer completion, GPIO, DMA completion,
  accelerator job completion.
* **Inadequate:** per-byte or very high-frequency interrupts; any hard
  real-time deadline without pinning and RT scheduling.
* **Design rule:** one interrupt per completed batch; completion ring in
  shared memory; interrupt as wakeup hint; NAPI-style
  (interrupt → mask → poll → unmask).

### 5.4 Measurement method (required before qualification)

Split the measurement into two segments. A host driver that signals the
eventfd from a threaded handler or workqueue rather than hardirq context
will dominate everything in the hypervisor path:

1. host IRQ entry → eventfd write
2. eventfd write → guest ISR entry

The ARM architected counter is common to both domains: the host stamps
`CNTVCT` into the shared ivshmem region immediately before ringing the
doorbell, the guest ISR stamps `CNTVCT` on entry, and the fixed guest/host
virtual-counter offset is calibrated once by round trip.

Cross-check with ftrace: host `kvm_set_irq` / `kvm_entry` against guest
`irq_handler_entry`.

Collect at least 100,000 samples. Report p50 / p95 / p99 / max. Run idle
and loaded (host cores stressed, other vCPUs busy), with and without vCPU
pinning plus RT priority. Compare against a native IRQ on the same silicon.

---

## 6. Deployment Path in EVE OS

1. **Submodule Repository**: The QEMU upstream source matching EVE OS is
   tracked under `qemu/` on branch `stable-4.19`
   (`https://xenbits.xen.org/git-http/qemu-xen.git`).
2. **Patch Staging**: Formulate the patch against
   `tools/qemu-xen/hw/misc/ivshmem.c` and place it in
   `eve/pkg/xen-tools/patches-4.19.0/` as
   `0001-ivshmem-recompute-direct-mapped-GSI-on-vector-unmask.patch`.
3. **Build & Validation**: Rebuild `pkg/xen-tools` and BaseOS rootfs:

   ```bash
   make -C eve pkg/xen-tools
   make -C eve rootfs
   ```

4. Deploy the updated firmware via OTA.

**Acceptance criteria:**

* Autonomous guest ISR entry with **zero** manual MSI injection.
* The two-segment latency distribution from §5.4 recorded for the
  qualification record (p50 / p95 / p99 / max, idle and loaded).
