/*
 * IavRefWinNc.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 *
 * Reference application, window memory type class (plan_iav_usb_yolo Envelope J): the accesses that destroy a VM when a
 * window is Device memory at stage 2 (an unaligned load or store, libc's memcpy and memset, DC ZVA) are made on each
 * window of the virtual driver with libc's own routines. If the memory is Normal-NC the process finishes and every row
 * below is printed; if it is Device, KVM cannot emulate the fault and the guest VM is destroyed, so the gate's own next
 * step (the guest answering again) is what proves the VM stayed up.
 *
 *   IavRefWinNc                           read every window unaligned (offsets 1, 3, 5, 7, 9, 15 and a 4 KiB crossing);
 *                                         write (memset to a pattern, memcpy from an odd offset, libc's zero fill and DC ZVA)
 *                                         only into the last 64 KiB of the decode and the overlay windows, where this
 *                                         application's own earlier work lives nowhere, and read the writes back
 *
 * The same static binary runs on the bare board (where the memory is cached) and in a guest.
 */

#include <string.h>

#include <vector>

#include "IavRefCommon.hxx"

using namespace iavref;

namespace {

void *(*volatile libcMemcpy)(void *, const void *, size_t) = memcpy;
void *(*volatile libcMemset)(void *, int, size_t) = memset;

struct Window {
    const char *name;
    unsigned int pid;
    bool writable;
};

long g_cases = 0, g_ok = 0;

void report(const char *win, const char *what, bool ok, unsigned long long detail)
{
    g_cases++;
    g_ok += ok ? 1 : 0;
    Row("case").add("window", win).add("what", what).add("ok", (long long)ok).add("detail", (long long)detail).print();
}

bool dczvaAllowed(unsigned long *block)
{
    unsigned long v;

    asm volatile("mrs %0, dczid_el0" : "=r"(v));
    *block = 4UL << (v & 15);
    return (v & 16) == 0;
}

void readUnaligned(const Window &w, const unsigned char *p, size_t len)
{
    static const size_t offs[] = { 1, 3, 5, 7, 9, 15, 4093 };
    std::vector<unsigned char> a(8192), b(8192);

    for (size_t i = 0; i < sizeof(offs) / sizeof(offs[0]); i++) {
        const size_t n = 4096 + 5;

        if (offs[i] + n > len) {
            continue;
        }
        libcMemcpy(&a[0], (const void *)(p + offs[i]), n);
        libcMemcpy(&b[0], (const void *)(p + offs[i]), n);
        report(w.name, "unaligned memcpy read twice equal", memcmp(&a[0], &b[0], n) == 0, offs[i]);
    }
}

void writeChecks(const Window &w, volatile unsigned char *base, size_t len)
{
    const size_t tail = len - 65536;
    unsigned char *p = (unsigned char *)base + tail;
    std::vector<unsigned char> pat(8192), back(8192);
    unsigned long block = 0;
    bool zva = dczvaAllowed(&block);

    for (size_t i = 0; i < pat.size(); i++) {
        pat[i] = (unsigned char)(i * 7 + 3);
    }
    libcMemset(p + 1, 0xA5, 4099);
    libcMemcpy(&back[0], p + 1, 4099);
    {
        bool ok = true;

        for (size_t i = 0; i < 4099; i++) {
            ok = ok && back[i] == 0xA5;
        }
        report(w.name, "unaligned memset 0xA5 reads back", ok, 4099);
    }
    libcMemcpy(p + 3, &pat[0], 5000);
    libcMemcpy(&back[0], p + 3, 5000);
    report(w.name, "unaligned memcpy write reads back", memcmp(&pat[0], &back[0], 5000) == 0, 5000);
    libcMemset(p + 8192, 0, 16384);               /* libc zeroes a block this size with DC ZVA when it is allowed */
    {
        std::vector<unsigned char> z(16384);
        bool ok = true;

        libcMemcpy(&z[0], p + 8192, 16384);
        for (size_t i = 0; i < z.size(); i++) {
            ok = ok && z[i] == 0;
        }
        report(w.name, "libc memset zero 16 KiB", ok, 16384);
    }
    if (zva) {
        unsigned char *z = p + 32768;                 /* 64 KiB aligned: any block size up to 64 KiB */
        std::vector<unsigned char> chk(block);
        bool ok = true;

        libcMemset(z, 0xFF, block * 2);
        asm volatile("dc zva, %0" : : "r"(z) : "memory");
        libcMemcpy(&chk[0], z, block);
        for (size_t i = 0; i < block; i++) {
            ok = ok && chk[i] == 0;
        }
        libcMemcpy(&chk[0], z + block, block);
        for (size_t i = 0; i < block; i++) {
            ok = ok && chk[i] == 0xFF;
        }
        report(w.name, "DC ZVA zeroes exactly one block", ok, block);
    } else {
        report(w.name, "DC ZVA not allowed here (DCZID_EL0.DZP)", true, 0);
    }
    libcMemset(p, 0, 65536);                      /* leave the tail zero */
}

} // namespace

int main()
{
    Device dev;
    static const Window wins[] = {
        { "dsp", IAV_PART_DSP, false },
        { "bsb", IAV_PART_BSB, false },
        { "decode", IAV_PART_DEC_BSB, true },
        { "overlay", IAV_PART_OVERLAY, true },
    };

    if (!dev.open()) {
        return 1;
    }
    for (size_t i = 0; i < sizeof(wins) / sizeof(wins[0]); i++) {
        unsigned long addr = 0, length = 0;
        Mapping m;

        if (dev.partition(wins[i].pid, &addr, &length) != 0 || length == 0) {
            report(wins[i].name, "window present", false, 0);
            continue;
        }
        if (!m.map(dev.fd(), addr, length, wins[i].writable ? PROT_READ | PROT_WRITE : PROT_READ)) {
            report(wins[i].name, "window mapped", false, length);
            continue;
        }
        report(wins[i].name, "window mapped", true, length);
        readUnaligned(wins[i], (const unsigned char *)m.bytes(), length);
        if (wins[i].writable && length > 65536 * 2) {
            writeChecks(wins[i], (volatile unsigned char *)m.bytes(), length);
        }
    }
    Row("summary").add("cases", (long long)g_cases).add("ok", (long long)g_ok).print();
    return g_ok == g_cases ? 0 : 1;
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
