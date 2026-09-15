# Ambarella GDMA for Linux HVM

`ambarella-gdma.ko` provides the native Ambarella kernel API in an
Ubuntu or Alpine HVM:

- `dma_memcpy()` accepts physical addresses in ordinary guest System
  RAM. It stages through the shared BAR, so it is compatible but not
  zero-copy.
- `dma_noncache_memcpy()` accepts physical addresses inside the
  `amba_virt` ivshmem BAR and performs an in-place host GDMA copy.
- `dma_pitch_memcpy()` accepts non-cacheable BAR buffers. Both
  `src_non_cached` and `dest_non_cached` must be set.

The `soc/ambarella/gdma.h` header provides the native Ambarella GDMA API.
Guest drivers that need non-cacheable buffers can include `amba_gdma_window.h`
and use `gdma_window_alloc()` / `gdma_window_free()`. Those helpers are
GPL-only and are not a userspace ABI.

Load `amba_virt.ko` first, then `ambarella-gdma.ko`. Unknown physical
addresses, MMIO outside the BAR, odd linear-copy lengths, overlap, and
out-of-range requests are rejected. Guest physical addresses are never
sent to the host. The transport device is mode `0600`; the host relay
and validation tools run as root.
