/*
 * UsbJpeg.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include "UsbJpeg.hxx"

#include <string.h>

/* JPEG-STD-BEGIN (iav_policy.c carries the same tables and rule; i_diff.sh compares the two) */
/*
 * The Huffman tables the DSP decoder takes: the four of ITU-T T.81 Annex K (K.3.3.1 and K.3.3.2), each as one DHT
 * table: the class/id byte, the 16 code-length counts, the values.
 */
namespace {

const uint8_t kStdDhtDcLuma[29] = {
    0x00, 0x00, 0x01, 0x05, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
    0x07, 0x08, 0x09, 0x0a, 0x0b,
};
const uint8_t kStdDhtDcChroma[29] = {
    0x01, 0x00, 0x03, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01, 0x01,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06,
    0x07, 0x08, 0x09, 0x0a, 0x0b,
};
const uint8_t kStdDhtAcLuma[179] = {
    0x10, 0x00, 0x02, 0x01, 0x03, 0x03, 0x02, 0x04, 0x03, 0x05, 0x05, 0x04,
    0x04, 0x00, 0x00, 0x01, 0x7d, 0x01, 0x02, 0x03, 0x00, 0x04, 0x11, 0x05,
    0x12, 0x21, 0x31, 0x41, 0x06, 0x13, 0x51, 0x61, 0x07, 0x22, 0x71, 0x14,
    0x32, 0x81, 0x91, 0xa1, 0x08, 0x23, 0x42, 0xb1, 0xc1, 0x15, 0x52, 0xd1,
    0xf0, 0x24, 0x33, 0x62, 0x72, 0x82, 0x09, 0x0a, 0x16, 0x17, 0x18, 0x19,
    0x1a, 0x25, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x34, 0x35, 0x36, 0x37, 0x38,
    0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53, 0x54,
    0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67, 0x68,
    0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x83, 0x84,
    0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95, 0x96, 0x97,
    0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8, 0xa9, 0xaa,
    0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2, 0xc3, 0xc4,
    0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5, 0xd6, 0xd7,
    0xd8, 0xd9, 0xda, 0xe1, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8, 0xe9,
    0xea, 0xf1, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};
const uint8_t kStdDhtAcChroma[179] = {
    0x11, 0x00, 0x02, 0x01, 0x02, 0x04, 0x04, 0x03, 0x04, 0x07, 0x05, 0x04,
    0x04, 0x00, 0x01, 0x02, 0x77, 0x00, 0x01, 0x02, 0x03, 0x11, 0x04, 0x05,
    0x21, 0x31, 0x06, 0x12, 0x41, 0x51, 0x07, 0x61, 0x71, 0x13, 0x22, 0x32,
    0x81, 0x08, 0x14, 0x42, 0x91, 0xa1, 0xb1, 0xc1, 0x09, 0x23, 0x33, 0x52,
    0xf0, 0x15, 0x62, 0x72, 0xd1, 0x0a, 0x16, 0x24, 0x34, 0xe1, 0x25, 0xf1,
    0x17, 0x18, 0x19, 0x1a, 0x26, 0x27, 0x28, 0x29, 0x2a, 0x35, 0x36, 0x37,
    0x38, 0x39, 0x3a, 0x43, 0x44, 0x45, 0x46, 0x47, 0x48, 0x49, 0x4a, 0x53,
    0x54, 0x55, 0x56, 0x57, 0x58, 0x59, 0x5a, 0x63, 0x64, 0x65, 0x66, 0x67,
    0x68, 0x69, 0x6a, 0x73, 0x74, 0x75, 0x76, 0x77, 0x78, 0x79, 0x7a, 0x82,
    0x83, 0x84, 0x85, 0x86, 0x87, 0x88, 0x89, 0x8a, 0x92, 0x93, 0x94, 0x95,
    0x96, 0x97, 0x98, 0x99, 0x9a, 0xa2, 0xa3, 0xa4, 0xa5, 0xa6, 0xa7, 0xa8,
    0xa9, 0xaa, 0xb2, 0xb3, 0xb4, 0xb5, 0xb6, 0xb7, 0xb8, 0xb9, 0xba, 0xc2,
    0xc3, 0xc4, 0xc5, 0xc6, 0xc7, 0xc8, 0xc9, 0xca, 0xd2, 0xd3, 0xd4, 0xd5,
    0xd6, 0xd7, 0xd8, 0xd9, 0xda, 0xe2, 0xe3, 0xe4, 0xe5, 0xe6, 0xe7, 0xe8,
    0xe9, 0xea, 0xf2, 0xf3, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa,
};

const unsigned kDhtN = 4;
struct StdTable {
    const uint8_t *t;
    size_t n;
};
const StdTable kStdDht[kDhtN] = {
    { kStdDhtDcLuma, sizeof(kStdDhtDcLuma) },
    { kStdDhtDcChroma, sizeof(kStdDhtDcChroma) },
    { kStdDhtAcLuma, sizeof(kStdDhtAcLuma) },
    { kStdDhtAcChroma, sizeof(kStdDhtAcChroma) },
};

/* The smallest JFIF APP0 segment of the JFIF 1.02 specification, marker included: 1:1 aspect, no thumbnail. */
const uint8_t kJfifApp0[] = {
    0xFF, 0xE0, 0x00, 0x10, 'J', 'F', 'I', 'F', 0x00, 0x01, 0x02, 0x00, 0x00, 0x01, 0x00, 0x01, 0x00, 0x00,
};

} // namespace
/* JPEG-STD-END */

bool UsbJpeg::inspect(const uint8_t *p, size_t len, Info *out)
{
    Info local{};
    Info *info = out ? out : &local;
    size_t n = len < kHeaderMax ? len : kHeaderMax;
    size_t i = 2;
    bool sof = false, dqt = false, app0 = false;
    unsigned dht = 0;

    info->kind = KindMalformed;
    info->width = 0;
    info->height = 0;
    info->sub422 = false;
    if (!p || n < 4 || p[0] != 0xFF || p[1] != 0xD8)
        return false;

    while (i + 1 < n) {
        unsigned m, l;

        if (p[i] != 0xFF)
            return false;
        m = p[i + 1];
        if (i + 4 > n)
            return false;
        l = (static_cast<unsigned>(p[i + 2]) << 8) | p[i + 3];
        if (l < 2 || i + 2 + l > n)
            return false;
        if (m == 0xDA) {
            if (!sof)
                return false;
            if (!dqt) {
                info->kind = KindMissingTables;
                return false;
            }
            if (dht != (1u << kDhtN) - 1) {
                info->kind = KindCustomHuffman;
                return false;
            }
            info->kind = KindHw;
            return true;
        }
        if (m == 0xC0) {
            const uint8_t *s = p + i + 4;

            if (sof)
                return false;
            if (l < 8 || s[0] != 8) {
                info->kind = KindNotEightBit;
                return false;
            }
            if (l != 8 + 3 * 3 || s[5] != 3) {
                info->kind = KindNotThreeComponent;
                return false;
            }
            info->height = (static_cast<uint32_t>(s[1]) << 8) | s[2];
            info->width = (static_cast<uint32_t>(s[3]) << 8) | s[4];
            if (info->height == 0 || info->width == 0 || info->height > 16384 || info->width > 16384) {
                info->kind = KindMalformed;
                return false;
            }
            if (s[10] != 0x11 || s[13] != 0x11) {
                info->kind = KindSamplingOther;
                return false;
            }
            if (s[7] == 0x11) {
                info->kind = KindSampling444;
                return false;
            }
            if (s[7] != 0x22 && s[7] != 0x21) {
                info->kind = KindSamplingOther;
                return false;
            }
            info->sub422 = s[7] == 0x21;
            sof = true;
        } else if (m == 0xDB) {
            dqt = true;
        } else if (m == 0xC4) {
            size_t k = i + 4, end = i + 2 + l;

            while (k < end) {
                unsigned sum = 0, t;

                if (end - k < 17)
                    return false;
                for (unsigned c = 1; c <= 16; c++)
                    sum += p[k + c];
                if (sum > 256 || end - k < 17 + sum)
                    return false;
                for (t = 0; t < kDhtN; t++)
                    if (kStdDht[t].n == 17 + sum && memcmp(p + k, kStdDht[t].t, kStdDht[t].n) == 0)
                        break;
                if (t == kDhtN || (dht & (1u << t))) {
                    info->kind = KindCustomHuffman;
                    return false;
                }
                dht |= 1u << t;
                k += 17 + sum;
            }
        } else if (m == 0xE0) {
            if (app0 || l < 16 || memcmp(p + i + 4, "JFIF", 5) != 0) {
                info->kind = KindMarkerNotAllowed;
                return false;
            }
            app0 = true;
        } else if (m >= 0xC1 && m <= 0xCF && m != 0xC4) {
            info->kind = (m == 0xC2) ? KindProgressive : KindNotBaseline;
            return false;
        } else if (m == 0xDD) {
            info->kind = KindRestartInterval;
            return false;
        } else {
            info->kind = KindMarkerNotAllowed;
            return false;
        }
        i += 2 + l;
    }
    return false;
}

bool UsbJpeg::normalize(const uint8_t *p, size_t len, std::vector<uint8_t> *out)
{
    size_t n = len < kHeaderMax ? len : kHeaderMax;
    size_t i = 2;
    bool dht = false, app0 = false;

    out->clear();
    if (!p || n < 4 || p[0] != 0xFF || p[1] != 0xD8)
        return false;
    out->reserve(len + sizeof(kJfifApp0) + 512);
    out->push_back(0xFF);
    out->push_back(0xD8);
    while (i + 1 < n) {
        unsigned m, l;

        if (p[i] != 0xFF || i + 4 > n)
            return false;
        m = p[i + 1];
        l = (static_cast<unsigned>(p[i + 2]) << 8) | p[i + 3];
        if (l < 2 || i + 2 + l > n)
            return false;
        if (m == 0xDA) {
            if (!dht) {
                for (unsigned t = 0; t < kDhtN; t++) {
                    const size_t sl = kStdDht[t].n + 2;

                    out->push_back(0xFF);
                    out->push_back(0xC4);
                    out->push_back(static_cast<uint8_t>(sl >> 8));
                    out->push_back(static_cast<uint8_t>(sl));
                    out->insert(out->end(), kStdDht[t].t, kStdDht[t].t + kStdDht[t].n);
                }
            }
            out->insert(out->end(), p + i, p + len);       /* the scan header and everything after it, unchanged */
            return true;
        }
        if (m == 0xE0) {
            if (!app0 && l >= 16 && memcmp(p + i + 4, "JFIF", 5) == 0) {
                app0 = true;
                out->insert(out->end(), p + i, p + i + 2 + l);
            }                                               /* any other APP0 (AVI1, a second JFIF) is dropped */
        } else if ((m >= 0xE1 && m <= 0xEF) || m == 0xFE) {
            /* other APPn and comments are dropped */
        } else {
            if (m == 0xC4)
                dht = true;
            out->insert(out->end(), p + i, p + i + 2 + l);
        }
        i += 2 + l;
    }
    return false;
}

const char *UsbJpeg::kindReason(int kind)
{
    switch (kind) {
    case KindHw:                return "";
    case KindProgressive:       return "progressive JPEG";
    case KindNotBaseline:       return "not a baseline JPEG";
    case KindNotEightBit:       return "not 8-bit JPEG";
    case KindNotThreeComponent: return "not a 3-component JPEG";
    case KindSampling444:       return "4:4:4 JPEG";
    case KindSamplingOther:     return "unsupported chroma sampling";
    case KindRestartInterval:   return "restart intervals";
    case KindMissingTables:     return "no quantisation table";
    case KindCustomHuffman:     return "custom Huffman tables";
    case KindMarkerNotAllowed:  return "unsupported JPEG markers";
    default:                    return "unrecognised JPEG header";
    }
}

unsigned long long UsbJpeg::decodedBytes(uint32_t width, uint32_t height, bool sub422)
{
    const unsigned long long pix = static_cast<unsigned long long>((width + 15u) / 16u * 16u) * ((height + 15u) / 16u * 16u);

    return sub422 ? pix * 2u : pix * 3u / 2u;
}

bool UsbJpeg::aspectIs16x9(uint32_t width, uint32_t height)
{
    return width != 0 && height != 0 && static_cast<uint64_t>(width) * 9 == static_cast<uint64_t>(height) * 16;
}

void UsbJpeg::nv12ToPlanar(const uint8_t *nv12, uint8_t *rgb, unsigned rows)
{
    const unsigned pitch = 640, height = 640;
    const uint8_t *yp = nv12;
    const uint8_t *uvp = nv12 + static_cast<size_t>(pitch) * height;
    uint8_t *r = rgb;
    uint8_t *g = rgb + static_cast<size_t>(pitch) * height;
    uint8_t *b = rgb + 2 * static_cast<size_t>(pitch) * height;

    if (rows > height)
        rows = height;
    for (unsigned row = 0; row < rows; row++) {
        const uint8_t *uv = uvp + static_cast<size_t>(row / 2) * pitch;

        for (unsigned col = 0; col < pitch; col++) {
            int y = yp[static_cast<size_t>(row) * pitch + col];
            int u = uv[col & ~1u] - 128;
            int v = uv[(col & ~1u) + 1] - 128;
            int rr = y + ((359 * v) >> 8);
            int gg = y - ((88 * u + 183 * v) >> 8);
            int bb = y + ((454 * u) >> 8);
            size_t o = static_cast<size_t>(row) * pitch + col;

            r[o] = static_cast<uint8_t>(rr < 0 ? 0 : rr > 255 ? 255 : rr);
            g[o] = static_cast<uint8_t>(gg < 0 ? 0 : gg > 255 ? 255 : gg);
            b[o] = static_cast<uint8_t>(bb < 0 ? 0 : bb > 255 ? 255 : bb);
        }
    }
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
