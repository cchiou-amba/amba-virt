/*
 * UsbWebcamControl.hxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#ifndef AMBA_VIRT_USB_WEBCAM_CONTROL_HXX
#define AMBA_VIRT_USB_WEBCAM_CONTROL_HXX

#include <stddef.h>

/* The request bodies of the USB Webcam tab's POST /usb/backend and POST /usb/exposure. */
class UsbWebcamControl {
public:
    enum Backend {
        BackendHardware = 0,    /* virtualized IAV hardware decode + scale, the default */
        BackendSoftware = 1     /* CPU decode and resize (the baseline) */
    };

    enum ExposureMode {
        ExposureAuto = 0,       /* the camera's own aperture-priority auto exposure, the default */
        ExposureFixed = 1       /* manual exposure for a steady 30 fps, with a gain the user sets */
    };

    struct Exposure {
        int mode;
        int exposureAbs;        /* 100 us units, -1 when the body gave none */
        int gain;               /* -1 when the body gave none */
    };

    /* {"backend":"hw"|"hardware"} or {"backend":"cpu"|"software"}. 0 and *out, or -EINVAL. */
    static int parseBackend(const char *body, size_t len, int *out);

    /* {"mode":"auto"} or {"mode":"fixed","exposure":<n>,"gain":<n>}. 0 and *out, or -EINVAL. */
    static int parseExposure(const char *body, size_t len, Exposure *out);
};

#endif /* AMBA_VIRT_USB_WEBCAM_CONTROL_HXX */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
