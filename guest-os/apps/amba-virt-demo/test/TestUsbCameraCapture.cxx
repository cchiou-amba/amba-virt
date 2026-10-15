/*
 * TestUsbCameraCapture.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include "CppUTest/TestHarness.h"
#include "../UsbCameraCapture.hxx"

#include <errno.h>
#include <linux/videodev2.h>

TEST_GROUP(UsbCameraCaptureContract)
{
    void setup() override {}
    void teardown() override {}
};

TEST(UsbCameraCaptureContract, DefaultAndSelectableResolution)
{
    UsbCameraCapture::Format fmt{};
    LONGS_EQUAL(0, UsbCameraCapture::defaultFormat(&fmt));
    LONGS_EQUAL(1280, fmt.width);
    LONGS_EQUAL(720, fmt.height);
    LONGS_EQUAL(30, fmt.fps);
    LONGS_EQUAL(V4L2_PIX_FMT_MJPEG, fmt.pixelFormat);
    LONGS_EQUAL(V4L2_MEMORY_MMAP, fmt.memory);
    LONGS_EQUAL(4, fmt.bufferCount);

    LONGS_EQUAL(0, UsbCameraCapture::formatForResolution(
                       UsbCameraCapture::Resolution1080p, &fmt));
    LONGS_EQUAL(1920, fmt.width);
    LONGS_EQUAL(1080, fmt.height);
    LONGS_EQUAL(30, fmt.fps);
    LONGS_EQUAL(V4L2_PIX_FMT_MJPEG, fmt.pixelFormat);
    LONGS_EQUAL(V4L2_MEMORY_MMAP, fmt.memory);
    LONGS_EQUAL(4, fmt.bufferCount);

    LONGS_EQUAL(-EINVAL, UsbCameraCapture::formatForResolution(99, &fmt));
    LONGS_EQUAL(-EINVAL, UsbCameraCapture::defaultFormat(nullptr));
}

TEST(UsbCameraCaptureContract, UvcNodeFilterAndDisconnect)
{
    CHECK_TRUE(UsbCameraCapture::driverIsUvc("uvcvideo"));
    CHECK_FALSE(UsbCameraCapture::driverIsUvc("vivid"));
    CHECK_FALSE(UsbCameraCapture::driverIsUvc(nullptr));

    CHECK_TRUE(UsbCameraCapture::nodeIsUvcCapture("uvcvideo", "Live Streamer CAM 313"));
    CHECK_FALSE(UsbCameraCapture::nodeIsUvcCapture(
        "uvcvideo", "Live Streamer CAM 313: USB Video Metadata"));
    CHECK_FALSE(UsbCameraCapture::nodeIsUvcCapture("uvcvideo", "metadata node"));
    CHECK_FALSE(UsbCameraCapture::nodeIsUvcCapture("vivid", "virtual"));

    /* uvcvideo metadata node: same driver and name, META_CAPTURE only. */
    CHECK_TRUE(UsbCameraCapture::capsAreVideoCapture(
        V4L2_CAP_DEVICE_CAPS | V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_META_CAPTURE,
        V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_STREAMING));
    CHECK_FALSE(UsbCameraCapture::capsAreVideoCapture(
        V4L2_CAP_DEVICE_CAPS | V4L2_CAP_VIDEO_CAPTURE | V4L2_CAP_META_CAPTURE,
        V4L2_CAP_META_CAPTURE | V4L2_CAP_STREAMING));
    CHECK_TRUE(UsbCameraCapture::capsAreVideoCapture(V4L2_CAP_VIDEO_CAPTURE, 0));
    CHECK_FALSE(UsbCameraCapture::capsAreVideoCapture(V4L2_CAP_META_CAPTURE, 0));

    CHECK_TRUE(UsbCameraCapture::errorIsDisconnect(ENODEV));
    CHECK_TRUE(UsbCameraCapture::errorIsDisconnect(EIO));
    CHECK_TRUE(UsbCameraCapture::errorIsDisconnect(ENXIO));
    CHECK_FALSE(UsbCameraCapture::errorIsDisconnect(EAGAIN));
}

TEST(UsbCameraCaptureContract, HotUnplugReconnectTransitions)
{
    int state = UsbCameraCapture::LinkAbsent;
    state = UsbCameraCapture::applyLinkEvent(state, UsbCameraCapture::EventDeviceFound);
    LONGS_EQUAL(UsbCameraCapture::LinkOpening, state);
    state = UsbCameraCapture::applyLinkEvent(state, UsbCameraCapture::EventStreamStarted);
    LONGS_EQUAL(UsbCameraCapture::LinkStreaming, state);
    state = UsbCameraCapture::applyLinkEvent(state, UsbCameraCapture::EventUnplug);
    LONGS_EQUAL(UsbCameraCapture::LinkRecovering, state);
    state = UsbCameraCapture::applyLinkEvent(state, UsbCameraCapture::EventDeviceFound);
    LONGS_EQUAL(UsbCameraCapture::LinkOpening, state);
    state = UsbCameraCapture::applyLinkEvent(state, UsbCameraCapture::EventGiveUp);
    LONGS_EQUAL(UsbCameraCapture::LinkRecovering, state);

    UsbCameraCapture camera;
    CHECK_FALSE(camera.isOpen());
    LONGS_EQUAL(UsbCameraCapture::LinkAbsent, camera.linkState());
    uint8_t scratch[8];
    size_t got = 99;
    LONGS_EQUAL(-ENODEV, camera.readJpeg(scratch, sizeof(scratch), &got));
    LONGS_EQUAL(UsbCameraCapture::LinkAbsent, camera.linkState());
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
