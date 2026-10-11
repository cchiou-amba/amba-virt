# vsock Message Format Policy

> **Security boundary:** the receiver of a vsock message in `amba-virt-server`
> faces an untrusted guest ([Architecture.md](Architecture.md#security-boundary)).
> This policy decides how that input is encoded so that it can be validated.

*Document Path: `doc/VsockMessageFormats.md`*  
*Copyright (C) 2026, Ambarella International LLC*

---

## 1. Rule

Every message that crosses virtio-vsock is one of two kinds. Choose by the
criteria below, not by habit. Do not use protobuf for its own sake, and do not
invent a third format.

| Kind | Format | Use when |
|---|---|---|
| **Structured message** | Nanopb (`protocol/amba_virt/amba_virt.proto`, bounded with `.options`, static allocation) | The content is structured, evolves independently of any external ABI, or is consumed by userspace code in more than one language. Camera, Cavalry, DMA, admin and the IAV tap are this kind. |
| **Fixed-layout frame** | Fixed header plus an exact-length payload, little-endian, no pointers | The payload is an external ABI struct that the receiver validates by length and field (for example an ioctl argument), **or** the receiver is a hostile-input boundary where an exact-size check is the safest parser, **or** the code runs in a kernel. The IAV `/dev/iav` proxy is this kind (`include/uapi/amba_virt_iav.h`). |

Both kinds ride inside the existing `struct amba_virt_msg` framing
(`[len u32 LE][payload]`, at most 4096 bytes), so one reader handles both.

## 2. Why

- **Structured messages** gain schema versioning, explicit endianness, one source
  of truth that doubles as documentation, and generated code that already exists
  in the tree.
- **Fixed-layout frames** are checked exactly: one length, no variable-length
  integers, no unknown, repeated or reordered fields. A protobuf decoder skips
  unknown fields and accepts fields in any order, so it needs extra checks to be
  as strict, and its attack surface is larger. A fixed frame is also trivial to
  implement in a kernel module or on another OS (QNX) with no library.
- The two together keep the Nanopb schema free of messages that only wrap an
  external ABI struct, where Nanopb would add an envelope and nothing else.

## 3. Rules for each kind

**Nanopb messages**
1. Add the message to `amba_virt.proto` and bound every `bytes`, `string` and
   `repeated` field in `amba_virt.options`.
2. Static allocation only. No callbacks that allocate.
3. Treat decode success as syntax only. Validate every field's value and range
   before use, as for any untrusted input.
4. Do not run Nanopb in a kernel module until stack use per call, allocation
   and a bounded decode have been measured and recorded.

**Fixed-layout frames**
1. A version field and a type field in a fixed header. Reserved fields are zero
   and a non-zero value is refused.
2. Exact-length checks before any field is read. Lengths are bounded by a
   named constant.
3. Little-endian fixed-width fields only. No pointers, no host addresses, no
   guest virtual addresses. References are opaque handles plus validated
   offsets and lengths.
4. A struct that is a driver ABI is carried as opaque bytes and validated per
   field by a generated table, never re-encoded.
5. The receiver has the structure-aware fuzz, stateful sequence and
   negative-control tests that the IAV broker has (see
   `drivers/amba_virt/tools/test/`).

## 4. Current classification

| Traffic | Kind | Where |
|---|---|---|
| Control RPC (camera, Cavalry, DMA, admin, IAV tap) | Nanopb | `protocol/amba_virt/` |
| IAV `/dev/iav` proxy (`AMBA_VIRT_MSG_IAV_REQ` / `_RESP`) | Fixed-layout frame | `drivers/amba_virt/include/uapi/amba_virt_iav.h` |

## 5. Revisit

Change this policy only with the owner's approval. Reasons to reopen it: a
measurement showing Nanopb is safe and cheap in a guest kernel; a need to
evolve the IAV frame in ways a version field cannot express; or a second
consumer of the fixed frame in a language without an easy binary reader.

*Decided 2026-10-11 during the IAV virtualization plan. It applies to all later
work, not only that plan.*
