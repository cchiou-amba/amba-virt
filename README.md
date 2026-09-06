# amba-virt

Virtualize Ambarella N1-655 devices for **EVE-OS** (KVM). Hardware stays on
the hypervisor. An Ubuntu **HVM** (EL1) talks to a privileged **NOHYPER**
container (EL2 host) over **virtio-vsock** (control, CID 2 port 5555) and
**ivshmem** (bulk). Cavalry / VisORC is assigned only to NOHYPER.

| Path | What it is |
|---|---|
| [doc/](doc/) | Architecture, transport, Cavalry, cloud models, zcli scripts |
| [poc/](poc/) | Matching `amba_virt` kernel modules and userspace on both sides |
| [models/](models/) | Hardware-details JSON for the two Cooper cloud models |
| [apps/](apps/) | Pulled edge-app manifests (`pull_app.sh`; local JSON) |
| [scripts/](scripts/) | `zcli` wrappers (token in `$ZCLI_TOKEN`, never committed). In-place update attempts: [scripts/failed/](scripts/failed/README.md) |

## Documentation

- [doc/Architecture.md](doc/Architecture.md) — EVE split (HVM + NOHYPER),
  device assignment, vsock/ivshmem placement.
- [doc/VirtualDrivers.md](doc/VirtualDrivers.md) — virtio-vsock + ivshmem
  path and `/dev/amba_virt` on both ends.
- [doc/CavalryVirtualization.md](doc/CavalryVirtualization.md) — why
  Cavalry stays in NOHYPER and how a later ioctl frontend sits on the
  transport.
- [doc/EVE-Ambarella-Models.md](doc/EVE-Ambarella-Models.md) — ZEDEDA Cloud
  hardware models (`ioMemberList`), PhyIo rules, and how to assign adapters
  to the NOHYPER instance only.
- [doc/EVE-ReconfigureEdgeApps.md](doc/EVE-ReconfigureEdgeApps.md) —
  do not edit `ubuntu_24_04-container` in place; create a new VisORC
  container with adapters at instance create.
- [doc/ZedControl-scripts.md](doc/ZedControl-scripts.md) — every wrapper
  under `scripts/` (zcli, pull models, show/set instance adapters, restart).
- [poc/README.md](poc/README.md) — build and load the transport PoC
  (guest vs host `KDIR`, vsock port 5555, ivshmem).
