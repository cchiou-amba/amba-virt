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
    int readJpeg(uint8_t *dst, size_t dstCap, size_t *outLen);
    bool isOpen() const;
    int linkState() const;
    int resolution() const;

private:
    int startStreaming(const char *path, const Format &fmt);
    void releaseDevice(bool transition);

    int _fd;
    int _linkState;
    int _resolution;
    int _bufferCount;
    bool _streaming;
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
