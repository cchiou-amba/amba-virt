/*
 * UsbIavDecoder.hxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#ifndef AMBA_VIRT_USB_IAV_DECODER_HXX
#define AMBA_VIRT_USB_IAV_DECODER_HXX

#include <stddef.h>
#include <stdint.h>

#include <vector>

/*
 * The hardware pre-processing of the USB Webcam tab through the virtual /dev/iav of the primary guest: one JPEG
 * decoder, and for each frame
 *
 *   copy the JPEG into the decode window -> IAV_IOC_DECODE_JPEG -> IAV_IOC_SET_IMG_SCALE (non-blocking) ->
 *   IAV_IOC_WAIT_IMG_SCALE -> read the scaled 640x640 NV12 canvas out -> NV12 to planar RGB (the network input)
 *
 * (the layout of guest-os/apps/iav-ref/IavRefPreproc.cxx: JPEG at offset 0 of the decode partition, the decoder output
 * from 2 MiB (4 MiB of room: a 1920x1080 4:2:2 picture is 4177920 bytes), the 640x640 NV12 output buffer at 1 MiB; the 16:9 picture is scaled to the 640x360 window at the top
 * and the gray pad below it is written once at open and not read again).
 *
 * The caller must pass only frames UsbJpeg::normalize() and UsbJpeg::inspect() produced and accepted, of the size given to open(): the decoder stalls on
 * other frames. Nothing here retries a failed call.
 */
class UsbIavDecoder {
public:
    struct Times {
        double copyMs;          /* JPEG into the window and the scaled picture out of it */
        double decodeMs;        /* DECODE_JPEG until the picture is reported */
        double scaleMs;         /* SET_IMG_SCALE and WAIT_IMG_SCALE */
        double convertMs;       /* NV12 to planar RGB */
    };

    /* The room the decoder output has in the window (the partition is 6 MiB): pictures that need more go to the CPU. */
    static const unsigned long kYuvRoom = 0x400000;

    UsbIavDecoder();
    ~UsbIavDecoder();

    UsbIavDecoder(const UsbIavDecoder &) = delete;
    UsbIavDecoder &operator=(const UsbIavDecoder &) = delete;

    /* Enter decode mode and create the decoder for frames of width x height. 0, or a negative errno. */
    int open(uint32_t width, uint32_t height);
    void close();
    bool isOpen() const { return _created; }
    uint32_t width() const { return _w; }
    uint32_t height() const { return _h; }

    /* One frame into planar RGB (3 x 640 x 640 bytes, pad rows pre-filled by this call's owner via padFill()). */
    int run(const uint8_t *jpeg, size_t len, uint8_t *planarRgb, Times *times);

    /* The planar pad below the picture: 114 in every plane. */
    static void padFill(uint8_t *planarRgb);

private:
    struct Impl;
    Impl *_impl;
    bool _created;
    uint32_t _w;
    uint32_t _h;
};

#endif /* AMBA_VIRT_USB_IAV_DECODER_HXX */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
