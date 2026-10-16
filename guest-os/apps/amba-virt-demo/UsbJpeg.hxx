/*
 * UsbJpeg.hxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#ifndef AMBA_VIRT_USB_JPEG_HXX
#define AMBA_VIRT_USB_JPEG_HXX

#include <stddef.h>
#include <stdint.h>

#include <vector>

/*
 * Pure helpers of the USB Webcam tab (no device access, host-testable).
 *
 * normalize() copies one camera frame into the form the IAV hardware decoder takes, and inspect() applies the rule the
 * Dom0 server applies to DECODE_JPEG (iav_jpeg_admit in drivers/amba_virt/tools/iav_policy.c) to the result: baseline
 * (SOF0), 8 bit, three components, Y sampled 2x2 (4:2:0) or 2x1 (4:2:2) and Cb, Cr 1x1, the four standard Huffman
 * tables of ITU-T T.81 Annex K, at most one APP0 and only a JFIF one, and no other marker before the scan (no restart
 * interval), all within the first 4096 bytes. A frame the rule refuses stalled the decoder in the board test, and a
 * stalled decoder takes later frames with it, so the application never submits one: the CPU decodes it instead. The
 * rule and the tables are in step with the server's; the check plan_iav_usb_yolo.artifacts/i_diff.sh compares them.
 *
 * The normalizer works on the content of the frame, never on what device sent it: a non-JFIF APP0 (for example AVI1),
 * other APPn and COM segments are dropped, and the four standard Huffman tables are inserted when the frame has no
 * DHT at all (the convention of UVC Motion-JPEG, which leaves them out). A frame with its own tables is not changed:
 * it is refused as custom Huffman tables.
 */
class UsbJpeg {
public:
    static const size_t kHeaderMax = 4096;

    enum Kind {
        KindHw = 0,             /* the hardware decoder takes it */
        KindMalformed,          /* no SOI, bad marker structure, no scan within the header window */
        KindProgressive,        /* SOF2 */
        KindNotBaseline,        /* any other SOF (extended, lossless, arithmetic, hierarchical) */
        KindNotEightBit,
        KindNotThreeComponent,
        KindSampling444,
        KindSamplingOther,
        KindRestartInterval,
        KindMissingTables,      /* no quantisation table */
        KindCustomHuffman,      /* a Huffman table other than the four standard ones, or not all four */
        KindMarkerNotAllowed,   /* any other marker before the scan, or a second or non-JFIF APP0 */
        KindCount
    };

    struct Info {
        int kind;
        uint32_t width;         /* from the SOF, 0 when none was read */
        uint32_t height;
        bool sub422;            /* Y sampled 2x1 (4:2:2), else 2x2 */
    };

    /* Classify the header; true exactly when the hardware decoder can take the frame (kind KindHw). */
    static bool inspect(const uint8_t *data, size_t len, Info *out);

    /*
     * The frame as the decoder is to be given it, in out: the header without the segments the decoder stalls on (see
     * above), then the scan bytes unchanged. False when the header is not a well-formed JPEG header within kHeaderMax
     * (then out is unspecified). The result is not yet admitted: pass it to inspect().
     */
    static bool normalize(const uint8_t *data, size_t len, std::vector<uint8_t> *out);

    /* The bytes the decoder writes for the picture (as the server counts them): 16-aligned, 1.5 or 2 bytes a pixel. */
    static unsigned long long decodedBytes(uint32_t width, uint32_t height, bool sub422);

    /* Why the CPU decodes this kind, as shown to the user: "custom Huffman tables", "progressive JPEG", ... */
    static const char *kindReason(int kind);

    /* The hardware path scales to a 640x360 window of the 640x640 canvas, so it takes 16:9 frames only. */
    static bool aspectIs16x9(uint32_t width, uint32_t height);

    /*
     * 640x640 NV12 (Y plane, then interleaved UV, both pitch 640) to planar RGB (R plane, G plane, B plane, the
     * network input layout), BT.601 full range, integer. Only rows [0, rows) are converted; the caller pre-fills the
     * gray pad below with 114 in every plane.
     */
    static void nv12ToPlanar(const uint8_t *nv12, uint8_t *rgb, unsigned rows);
};

#endif /* AMBA_VIRT_USB_JPEG_HXX */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
