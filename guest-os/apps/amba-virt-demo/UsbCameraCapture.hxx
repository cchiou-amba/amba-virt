/*
 * UsbCameraCapture.hxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#ifndef AMBA_VIRT_USB_CAMERA_CAPTURE_HXX
#define AMBA_VIRT_USB_CAMERA_CAPTURE_HXX

#include <stddef.h>
#include <stdint.h>

/*
 * V4L2 MJPEG MMAP capture for uvcvideo nodes.
 * Default format is 1280x720 at 30 fps. 1920x1080 at 30 fps is selectable.
 * Four buffers. Hot-unplug moves the link to recovering until the node returns.
 */
class UsbCameraCapture {
public:
    static const int kBufferCount = 4;
    static const uint32_t kDefaultWidth = 1280;
    static const uint32_t kDefaultHeight = 720;
    static const uint32_t kDefaultFps = 30;
    static const uint32_t kSelectableWidth = 1920;
    static const uint32_t kSelectableHeight = 1080;

    enum Resolution {
        Resolution720p = 0,
        Resolution1080p = 1
    };

    enum LinkState {
        LinkAbsent = 0,
        LinkOpening = 1,
        LinkStreaming = 2,
        LinkRecovering = 3
    };

    enum LinkEvent {
        EventDeviceFound = 1,
        EventStreamStarted = 2,
        EventUnplug = 3,
        EventGiveUp = 4
    };

    /*
     * Fixed exposure, exposure_time_absolute in 100 us units. Measured on the AVerMedia 07ca:313a (plan_iav_usb_yolo
     * Envelope I): 100 (10 ms) delivers 30 frames per second, 120 to 250 deliver 25 and 300 or more deliver 20, so the
     * fixed mode is held to 100 and the image is brightened with gain instead.
     */
    static const int kFixedExposureMax = 100;
    static const int kFixedExposureDefault = 100;

    struct ExposureState {
        bool fixed;             /* auto_exposure is manual (1), not aperture priority (3) */
        int exposureAbs;        /* exposure_time_absolute as read back, 100 us units; -1 when unreadable */
        int gain;               /* gain as read back; -1 when unreadable */
    };

    struct Format {
        uint32_t width;
        uint32_t height;
        uint32_t fps;
        uint32_t pixelFormat;
        uint32_t memory;
        int bufferCount;
    };

    UsbCameraCapture();
    ~UsbCameraCapture();

    UsbCameraCapture(const UsbCameraCapture &) = delete;
    UsbCameraCapture &operator=(const UsbCameraCapture &) = delete;

    static int defaultFormat(Format *out);
    static int formatForResolution(int resolution, Format *out);
    static bool driverIsUvc(const char *driver);
    static bool nodeIsUvcCapture(const char *driver, const char *name);
    static bool capsAreVideoCapture(uint32_t capabilities, uint32_t deviceCaps);
    static bool errorIsDisconnect(int err);
    static int applyLinkEvent(int state, int event);
    static int findUvcCaptureNode(char *path, size_t pathLen);

    int openUvc(int resolution);
    void closeCamera();
    /* sequence, when given, receives the V4L2 frame sequence number (gaps are frames the camera or driver dropped). */
    int readJpeg(uint8_t *dst, size_t dstCap, size_t *outLen, uint32_t *sequence = nullptr);

    /*
     * Exposure control. setFixedExposure() sets auto_exposure=1 (manual), then exposure_time_absolute (clamped to the
     * control's range and to kFixedExposureMax) and gain (clamped to the control's range) and reads them back into
     * state. It remembers the gain and mode it replaced. restoreAutoExposure() puts them back (auto_exposure=3).
     * closeCamera() restores them too, so a fixed exposure never outlives the capture. 0, or a negative errno.
     */
    int setFixedExposure(int exposureAbs, int gain, ExposureState *state);
    int restoreAutoExposure();
    bool fixedExposureApplied() const { return _exposureFixed; }
    static int clampValue(int value, int minimum, int maximum);
    bool isOpen() const;
    int linkState() const;
    int resolution() const;

private:
    int controlGet(uint32_t id, int *value);
    int controlSet(uint32_t id, int value);
    int controlRange(uint32_t id, int *minimum, int *maximum);
    int startStreaming(const char *path, const Format &fmt);
    void releaseDevice(bool transition);

    int _fd;
    int _linkState;
    int _resolution;
    int _bufferCount;
    bool _streaming;
    bool _exposureFixed;
    int _savedAutoMode;
    int _savedGain;
    uint32_t _width;
    uint32_t _height;
    uint32_t _fps;
    void *_bufferStart[kBufferCount];
    size_t _bufferLength[kBufferCount];
};

#endif /* AMBA_VIRT_USB_CAMERA_CAPTURE_HXX */

/*
 * Local variables:
 * mode: C++
 * c-file-style: "BSD"
 * c-basic-offset: 4
 * tab-width: 4
 * indent-tabs-mode: nil
 * End:
 */
