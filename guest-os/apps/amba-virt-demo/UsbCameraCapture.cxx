/*
 * UsbCameraCapture.cxx
 *
 * Copyright (C) 2026, Ambarella International LLC.
 */

#include "UsbCameraCapture.hxx"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

UsbCameraCapture::UsbCameraCapture()
    : _fd(-1),
      _linkState(LinkAbsent),
      _resolution(Resolution720p),
      _bufferCount(0),
      _streaming(false),
      _width(0),
      _height(0),
      _fps(0)
{
    for (int i = 0; i < kBufferCount; i++) {
        _bufferStart[i] = nullptr;
        _bufferLength[i] = 0;
    }
}

UsbCameraCapture::~UsbCameraCapture()
{
    closeCamera();
}

int UsbCameraCapture::defaultFormat(Format *out)
{
    return formatForResolution(Resolution720p, out);
}

int UsbCameraCapture::formatForResolution(int resolution, Format *out)
{
    if (!out)
        return -EINVAL;

    if (resolution == Resolution720p) {
        out->width = kDefaultWidth;
        out->height = kDefaultHeight;
    } else if (resolution == Resolution1080p) {
        out->width = kSelectableWidth;
        out->height = kSelectableHeight;
    } else {
        return -EINVAL;
    }

    out->fps = kDefaultFps;
    out->pixelFormat = V4L2_PIX_FMT_MJPEG;
    out->memory = V4L2_MEMORY_MMAP;
    out->bufferCount = kBufferCount;
    return 0;
}

bool UsbCameraCapture::driverIsUvc(const char *driver)
{
    return driver && strcmp(driver, "uvcvideo") == 0;
}

bool UsbCameraCapture::nodeIsUvcCapture(const char *driver, const char *name)
{
    if (!driverIsUvc(driver))
        return false;
    if (name && (strstr(name, "Metadata") || strstr(name, "metadata")))
        return false;
    return true;
}

/*
 * uvcvideo registers a metadata node next to each capture node, under the
 * same driver and often the same name. Only the capability bits tell
 * them apart.
 */
bool UsbCameraCapture::capsAreVideoCapture(uint32_t capabilities, uint32_t deviceCaps)
{
    uint32_t caps = (capabilities & V4L2_CAP_DEVICE_CAPS) ? deviceCaps : capabilities;
    return (caps & V4L2_CAP_VIDEO_CAPTURE) != 0;
}

static bool NodeQueriesAsVideoCapture(const char *devPath)
{
    int fd = open(devPath, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0)
        return false;
    struct v4l2_capability cap{};
    bool ok = ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0 &&
              UsbCameraCapture::capsAreVideoCapture(cap.capabilities, cap.device_caps);
    close(fd);
    return ok;
}

bool UsbCameraCapture::errorIsDisconnect(int err)
{
    return err == ENODEV || err == EIO || err == ENXIO || err == EPIPE;
}

int UsbCameraCapture::applyLinkEvent(int state, int event)
{
    switch (state) {
    case LinkAbsent:
        if (event == EventDeviceFound)
            return LinkOpening;
        return state;
    case LinkOpening:
        if (event == EventStreamStarted)
            return LinkStreaming;
        if (event == EventUnplug || event == EventGiveUp)
            return LinkRecovering;
        return state;
    case LinkStreaming:
        if (event == EventUnplug || event == EventGiveUp)
            return LinkRecovering;
        return state;
    case LinkRecovering:
        if (event == EventDeviceFound)
            return LinkOpening;
        return state;
    default:
        return LinkAbsent;
    }
}

int UsbCameraCapture::findUvcCaptureNode(char *path, size_t pathLen)
{
    if (!path || pathLen < 12)
        return -EINVAL;

    DIR *dir = opendir("/sys/class/video4linux");
    if (!dir)
        return -ENODEV;

    /* Lowest videoN wins, so the choice does not depend on readdir order. */
    int found = -ENODEV;
    long bestIndex = -1;
    struct dirent *ent;
    while ((ent = readdir(dir)) != nullptr) {
        if (strncmp(ent->d_name, "video", 5) != 0)
            continue;

        char driverLink[512];
        snprintf(driverLink, sizeof(driverLink),
                 "/sys/class/video4linux/%s/device/driver", ent->d_name);
        char target[512];
        ssize_t n = readlink(driverLink, target, sizeof(target) - 1);
        if (n < 0)
            continue;
        target[n] = '\0';
        const char *base = strrchr(target, '/');
        base = base ? base + 1 : target;

        char namePath[512];
        snprintf(namePath, sizeof(namePath),
                 "/sys/class/video4linux/%s/name", ent->d_name);
        char name[256];
        name[0] = '\0';
        FILE *nf = fopen(namePath, "r");
        if (nf) {
            if (!fgets(name, sizeof(name), nf))
                name[0] = '\0';
            fclose(nf);
            char *nl = strchr(name, '\n');
            if (nl)
                *nl = '\0';
        }

        if (!nodeIsUvcCapture(base, name))
            continue;

        char *end = nullptr;
        long index = strtol(ent->d_name + 5, &end, 10);
        if (end == ent->d_name + 5 || *end != '\0' || index < 0)
            continue;
        if (bestIndex >= 0 && index >= bestIndex)
            continue;

        char devPath[64];
        int written = snprintf(devPath, sizeof(devPath), "/dev/%s", ent->d_name);
        if (written < 0 || static_cast<size_t>(written) >= sizeof(devPath) ||
            static_cast<size_t>(written) >= pathLen)
            continue;
        if (!NodeQueriesAsVideoCapture(devPath))
            continue;

        memcpy(path, devPath, static_cast<size_t>(written) + 1);
        bestIndex = index;
        found = 0;
    }

    closedir(dir);
    return found;
}

bool UsbCameraCapture::isOpen() const
{
    return _fd >= 0 && _streaming;
}

int UsbCameraCapture::linkState() const
{
    return _linkState;
}

int UsbCameraCapture::resolution() const
{
    return _resolution;
}

void UsbCameraCapture::releaseDevice(bool transition)
{
    if (_fd >= 0 && _streaming) {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        ioctl(_fd, VIDIOC_STREAMOFF, &type);
    }
    _streaming = false;

    for (int i = 0; i < _bufferCount; i++) {
        if (_bufferStart[i] && _bufferStart[i] != MAP_FAILED)
            munmap(_bufferStart[i], _bufferLength[i]);
        _bufferStart[i] = nullptr;
        _bufferLength[i] = 0;
    }

    if (_fd >= 0) {
        struct v4l2_requestbuffers req{};
        req.count = 0;
        req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        req.memory = V4L2_MEMORY_MMAP;
        ioctl(_fd, VIDIOC_REQBUFS, &req);
        close(_fd);
        _fd = -1;
    }
    _bufferCount = 0;

    if (transition && (_linkState == LinkOpening || _linkState == LinkStreaming))
        _linkState = applyLinkEvent(_linkState, EventUnplug);
}

void UsbCameraCapture::closeCamera()
{
    releaseDevice(true);
}

int UsbCameraCapture::startStreaming(const char *path, const Format &fmt)
{
    _fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (_fd < 0)
        return -errno;

    struct v4l2_capability cap{};
    if (ioctl(_fd, VIDIOC_QUERYCAP, &cap) < 0) {
        int err = -errno;
        releaseDevice(true);
        return err;
    }

    char driver[sizeof(cap.driver) + 1];
    memcpy(driver, cap.driver, sizeof(cap.driver));
    driver[sizeof(cap.driver)] = '\0';
    if (!driverIsUvc(driver)) {
        releaseDevice(true);
        return -ENODEV;
    }

    if (!capsAreVideoCapture(cap.capabilities, cap.device_caps)) {
        releaseDevice(true);
        return -ENODEV;
    }

    struct v4l2_format vfmt{};
    vfmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    vfmt.fmt.pix.width = fmt.width;
    vfmt.fmt.pix.height = fmt.height;
    vfmt.fmt.pix.pixelformat = fmt.pixelFormat;
    vfmt.fmt.pix.field = V4L2_FIELD_NONE;
    if (ioctl(_fd, VIDIOC_S_FMT, &vfmt) < 0) {
        int err = -errno;
        releaseDevice(true);
        return err == 0 ? -EIO : err;
    }
    if (vfmt.fmt.pix.pixelformat != V4L2_PIX_FMT_MJPEG ||
        vfmt.fmt.pix.width == 0 || vfmt.fmt.pix.height == 0) {
        releaseDevice(true);
        return -EINVAL;
    }
    _width = vfmt.fmt.pix.width;
    _height = vfmt.fmt.pix.height;
    _fps = fmt.fps;

    struct v4l2_streamparm parm{};
    parm.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    parm.parm.capture.timeperframe.numerator = 1;
    parm.parm.capture.timeperframe.denominator = fmt.fps;
    ioctl(_fd, VIDIOC_S_PARM, &parm);

    struct v4l2_requestbuffers req{};
    req.count = static_cast<uint32_t>(fmt.bufferCount);
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = fmt.memory;
    if (ioctl(_fd, VIDIOC_REQBUFS, &req) < 0) {
        int err = -errno;
        releaseDevice(true);
        return err == 0 ? -EIO : err;
    }
    if (req.count != static_cast<uint32_t>(kBufferCount) || req.memory != V4L2_MEMORY_MMAP) {
        releaseDevice(true);
        return -EINVAL;
    }

    _bufferCount = kBufferCount;
    for (int i = 0; i < _bufferCount; i++) {
        struct v4l2_buffer buf{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = static_cast<uint32_t>(i);
        if (ioctl(_fd, VIDIOC_QUERYBUF, &buf) < 0) {
            int err = -errno;
            releaseDevice(true);
            return err == 0 ? -EIO : err;
        }
        void *mapped = mmap(nullptr, buf.length, PROT_READ | PROT_WRITE, MAP_SHARED,
                            _fd, buf.m.offset);
        if (mapped == MAP_FAILED) {
            int err = -errno;
            releaseDevice(true);
            return err == 0 ? -EIO : err;
        }
        _bufferStart[i] = mapped;
        _bufferLength[i] = buf.length;
        if (ioctl(_fd, VIDIOC_QBUF, &buf) < 0) {
            int err = -errno;
            releaseDevice(true);
            return err == 0 ? -EIO : err;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (ioctl(_fd, VIDIOC_STREAMON, &type) < 0) {
        int err = -errno;
        if (errorIsDisconnect(errno))
            err = -ENODEV;
        releaseDevice(true);
        return err == 0 ? -EIO : err;
    }

    _streaming = true;
    _linkState = applyLinkEvent(_linkState, EventStreamStarted);
    return 0;
}

int UsbCameraCapture::openUvc(int resolution)
{
    Format fmt{};
    int rc = formatForResolution(resolution, &fmt);
    if (rc < 0)
        return rc;

    if (_fd >= 0 || _streaming)
        closeCamera();

    char path[64];
    rc = findUvcCaptureNode(path, sizeof(path));
    if (rc < 0) {
        if (_linkState != LinkRecovering)
            _linkState = LinkAbsent;
        return -ENODEV;
    }

    int base = (_linkState == LinkRecovering) ? LinkRecovering : LinkAbsent;
    _linkState = applyLinkEvent(base, EventDeviceFound);
    _resolution = resolution;

    rc = startStreaming(path, fmt);
    if (rc < 0) {
        if (_linkState == LinkOpening || _linkState == LinkStreaming)
            _linkState = applyLinkEvent(_linkState, EventUnplug);
        else if (_linkState != LinkRecovering)
            _linkState = LinkRecovering;
        return rc;
    }
    return 0;
}

int UsbCameraCapture::readJpeg(uint8_t *dst, size_t dstCap, size_t *outLen)
{
    if (!dst || !outLen || dstCap == 0)
        return -EINVAL;
    if (_fd < 0 || !_streaming)
        return -ENODEV;

    struct pollfd pfd{};
    pfd.fd = _fd;
    pfd.events = POLLIN;
    int pr = poll(&pfd, 1, 200);
    if (pr < 0) {
        if (errno == EINTR)
            return -EINTR;
        if (errorIsDisconnect(errno)) {
            releaseDevice(true);
            return -ENODEV;
        }
        return -errno;
    }
    if (pr == 0)
        return -EAGAIN;
    if (pfd.revents & (POLLERR | POLLHUP | POLLNVAL)) {
        releaseDevice(true);
        return -ENODEV;
    }

    struct v4l2_buffer buf{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    if (ioctl(_fd, VIDIOC_DQBUF, &buf) < 0) {
        if (errno == EAGAIN || errno == EINTR)
            return -errno;
        if (errorIsDisconnect(errno)) {
            releaseDevice(true);
            return -ENODEV;
        }
        return -errno;
    }

    if (buf.index >= static_cast<uint32_t>(_bufferCount) || !_bufferStart[buf.index]) {
        releaseDevice(true);
        return -EIO;
    }

    size_t used = buf.bytesused;
    int copy_rc = 0;
    if (used > dstCap) {
        copy_rc = -EMSGSIZE;
    } else if (used == 0) {
        copy_rc = -EAGAIN;
    } else {
        memcpy(dst, _bufferStart[buf.index], used);
        *outLen = used;
    }

    if (ioctl(_fd, VIDIOC_QBUF, &buf) < 0) {
        if (errorIsDisconnect(errno)) {
            releaseDevice(true);
            return -ENODEV;
        }
        return -errno;
    }
    return copy_rc;
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
