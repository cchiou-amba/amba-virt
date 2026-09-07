# PoC — vsock + ivshmem transport

Matching kernel modules and userspace on both ends of the EVE split:

| Side | Build | Load |
|---|---|---|
| Ubuntu HVM (EL1) | `make kmod-hvm` + `amba-virt-cli` against **Ubuntu** headers | `insmod kmod/hvm/amba_virt.ko` |
| NOHYPER container (EL2 host) | `make kmod-nohyper` + `amba-virt-server` against **eve-kernel** (or EVE `/lib/modules/$(uname -r)/build`) | create shm file, `insmod kmod/nohyper/amba_virt.ko` |

Not Cavalry. Same `/dev/amba_virt` UAPI on both sides: `mmap` of shared memory
plus framed vsock send/recv. Background: [../doc/Architecture.md](../doc/Architecture.md),
[../doc/VirtualDrivers.md](../doc/VirtualDrivers.md).

**vsock:** HVM guest → host **CID 2**, port **5555**. Never port **2000** (EVE VComLink).

**NOHYPER `insmod` loads into the EVE kernel.** Build `kmod-nohyper` against
`eve-kernel` (`../eve/eve-kernel` from `amba-virt`) or, on the device,
`/lib/modules/$(uname -r)/build`. A mismatched kmod can panic the host.

**HVM `insmod` loads into the Ubuntu HVM kernel.** Build `kmod-hvm` against
that VM’s `linux-headers-$(uname -r)`. Do not point `KDIR_HVM` at
`eve-kernel` and do not mount that tree into the HVM. HVM UAPI is
`include/uapi/` in this repo.

ivshmem is **not** on stock EVE HVMs; vsock is. Use local QEMU first, then add
ivshmem to the EVE VM spec / device-model.

## Layout

```text
poc/
  include/uapi/amba_virt.h
  kmod/common/          chardev + vsock framing
  kmod/hvm/             PCI ivshmem (vendor 1af4 device 1110, BAR 2)
  kmod/nohyper/         mmap backing file + vsock listen
  userspace/hvm/amba-virt-cli.c
  userspace/nohyper/amba-virt-server.c
```

## Build

Two kernels, two `KDIR`s. `make help` prints the paths in use.

| Variable | Default | Used by |
|---|---|---|
| `KDIR_HVM` | `/lib/modules/$(uname -r)/build` | `kmod-hvm` (Ubuntu HVM) |
| `EVE` | sibling `../eve` (override if the tree lives elsewhere) | `eve-flags.mk` |
| `KDIR_NOHYPER` | `$(EVE)/build/usr/src/linux-headers-<ver>-linuxkit-<git12>[-<user>][-dirty]` | `kmod-nohyper` (NOHYPER / EVE) |
| `ARCH` / `CROSS_COMPILE` / `NOHYPER_CC` | `arm64` / `aarch64-linux-gnu-` / `$(CROSS_COMPILE)gcc` | `kmod-nohyper`, `amba-virt-server` |

The HVM target **refuses** a tree that contains `ambarella/` (eve-kernel).
The old single `KDIR` override is rejected so one tree cannot be used for both.

### Builder (`make build-nohyper`)

Cross-compile the NOHYPER kmod against EVE linux-headers, not Ubuntu.

1. EVE tree with `eve-kernel/Makefile.eve`. Default: sibling of this repo
   (`../eve` from `amba-virt`, `../../eve` from `poc`). Override: `EVE=/path/to/eve`.
2. Once per kernel build (needs a prior `make eve-kernel` so the image is in
   the linuxkit cache):

```bash
make -C $EVE/build eve-kernel-headers
```

3. `make help` — `EVE_KERNEL` exists (not `poc/eve-kernel`); `KDIR_NOHYPER` is
   `…/build/usr/src/linux-headers-*-linuxkit-*`.
4. `make build-nohyper` (or `make build-nohyper EVE=/path/to/eve`).

```bash
cd poc
make help
make build-hvm            # on the Ubuntu HVM: CLI + HVM kmod
make build-nohyper        # builder or NOHYPER: server + NOHYPER kmod (eve-flags.mk)
make                      # userspace; kmods only if the matching KDIR is usable
make userspace
make kmod-hvm             # Ubuntu headers
make kmod-nohyper         # eve-kernel or EVE build dir
make kmod-nohyper KDIR_NOHYPER=/lib/modules/$(uname -r)/build   # local QEMU
make clean-hvm            # HVM kmod + amba-virt-cli
make distclean-hvm
make clean-nohyper        # NOHYPER kmod + amba-virt-server
make distclean-nohyper
```

On EVE/NOHYPER, after `eve-kernel` is prepared (or using the running EVE
headers):

```bash
make kmod-nohyper KDIR_NOHYPER=/lib/modules/$(uname -r)/build
```

A raw `eve-kernel` checkout with no `.config` is skipped by `make all`;
`make kmod-nohyper` will tell you to `modules_prepare` or to use the on-device
build directory.

Produces `bin/amba-virt-cli`, `bin/amba-virt-server`,
`kmod/hvm/amba_virt.ko`, `kmod/nohyper/amba_virt.ko`.

## Local QEMU

Create the backing file on the hypervisor (same path the NOHYPER kmod will open):

```bash
dd if=/dev/zero of=/dev/shm/amba-virt bs=1M count=16
```

QEMU extras (plus whatever you already use for the VM):

```text
-object memory-backend-file,id=amba_shm,size=16M,mem-path=/dev/shm/amba-virt,share=on
-device ivshmem-plain,memdev=amba_shm,master=on
-device vhost-vsock-pci,guest-cid=3
```

Host CID is 2. Guest CID must be >= 3.

### Hypervisor / container (NOHYPER kmod + server)

```bash
# file must exist and stay mapped
insmod kmod/nohyper/amba_virt.ko shm_path=/dev/shm/amba-virt vsock_port=5555
./bin/amba-virt-server
```

If the container is not the QEMU host, bind-mount `/dev/shm/amba-virt` into
the container. Privileged NOHYPER + matching kernel headers for `insmod`.

### HVM Guest (HVM kmod + CLI)

```bash
modprobe virtio_vsock    # if /dev/vsock is missing
insmod kmod/hvm/amba_virt.ko
./bin/amba-virt-cli info
./bin/amba-virt-cli ping
./bin/amba-virt-cli shm
```

`ping` is vsock-only. `shm` writes a pattern into ivshmem and waits for ACK.

## EVE

1. Guest: `ls /dev/vsock`. Optional: connect to CID 2 port 2000 (VComLink) to
   prove vsock; do not use 2000 for this transport.
2. Container: host kmod listen on 5555. Confirm `dmesg | grep amba_virt`.
3. ivshmem: add QEMU ivshmem + shared file via device-model / Zedcontroller;
   assign the file into the NOHYPER app. Until that exists, vsock `ping`
   still validates the control path.

## UAPI (`/dev/amba_virt`)

- `AMBA_VIRT_IOC_GET_INFO` — role, shm size, connected
- `AMBA_VIRT_IOC_CONNECT` — guest: connect vsock (also implied by SEND)
- `AMBA_VIRT_IOC_SEND` / `AMBA_VIRT_IOC_RECV` — 4-byte LE length + payload
- `mmap` — whole shared region

Payload types (userspace): `PING`/`PONG`, `SHM_NOTIFY`/`SHM_ACK`.
