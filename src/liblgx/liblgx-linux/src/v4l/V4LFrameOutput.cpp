#include "V4LFrameOutput.h"

#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

namespace {
    // Largest frame the CV710 can deliver: 1080p60 YUY2 = 1920 * 1080 * 2 bytes.
    constexpr size_t MAX_FRAME_BYTES = 1920u * 1080u * 2u;
}

v4l::V4LFrameOutput::V4LFrameOutput(const std::string &deviceName) {
    _v4l2fd = open(deviceName.c_str(), O_RDWR);
}

void v4l::V4LFrameOutput::initialiseVideo(lgx2::VideoScale) {
    if (_v4l2fd == -1) {
        throw std::runtime_error("Failed to open V4L2 Loopback device");
    }

    _frameBuffer = new uint8_t[MAX_FRAME_BYTES];
    _bufferCapacity = MAX_FRAME_BYTES;

    // Start from the native mode; the format is renegotiated in
    // videoFrameAvailable() if the source switches resolution.
    negotiateFormat(1920, 1080);
}

bool v4l::V4LFrameOutput::negotiateFormat(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return false;
    if (width == _width && height == _height) return true;

    struct v4l2_format v {};
    v.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    if (ioctl(_v4l2fd, VIDIOC_G_FMT, &v) == -1) {
        fprintf(stderr, "[V4L] Cannot query loopback device format\n");
        return false;
    }
    v.fmt.pix.width = width;
    v.fmt.pix.height = height;
    v.fmt.pix.pixelformat = V4L2_PIX_FMT_YUYV;
    v.fmt.pix.bytesperline = width * 2;
    v.fmt.pix.sizeimage = width * height * 2;
    v.fmt.pix.field = V4L2_FIELD_NONE;
    if (ioctl(_v4l2fd, VIDIOC_S_FMT, &v) == -1) {
        fprintf(stderr, "[V4L] Cannot set loopback format to %ux%u\n", width, height);
        return false;
    }

    _width = v.fmt.pix.width;
    _height = v.fmt.pix.height;
    _frameBytes = static_cast<size_t>(_width) * _height * 2;
    printf("[V4L] Output format set to %ux%u YUYV (%zu bytes/frame)\n",
           _width, _height, _frameBytes);
    fflush(stdout);
    return true;
}

void v4l::V4LFrameOutput::videoFrameAvailable(uint32_t *image, uint32_t width, uint32_t height) {
    if (!image || !_frameBuffer || width == 0 || height == 0) return;

    if (width != _width || height != _height) {
        if (!negotiateFormat(width, height)) {
            // The consumer may be holding a fixed format; drop rather than
            // corrupt the loopback stream with a wrongly sized frame.
            if (++_droppedMismatch <= 5 || (_droppedMismatch % 120) == 0) {
                printf("[V4L] Dropped %ux%u frame: loopback format still %ux%u\n",
                       width, height, _width, _height);
                fflush(stdout);
            }
            return;
        }
    }

    size_t bytes = static_cast<size_t>(width) * height * 2;
    if (bytes > _bufferCapacity) bytes = _bufferCapacity;
    memcpy(_frameBuffer, image, bytes);
    _newFrame = true;
}

void v4l::V4LFrameOutput::videoFrameAvailable(uint32_t *image) {
    videoFrameAvailable(image, 1920, 1080);
}

void v4l::V4LFrameOutput::display() {
    // CV-17: never re-write the same frame. Previously this ran on every
    // main-loop iteration (~CPU speed) and flooded the loopback device.
    if (!_newFrame || _frameBytes == 0) return;
    _newFrame = false;

    ssize_t bytesWritten = write(_v4l2fd, _frameBuffer, _frameBytes);
    if (bytesWritten < 0 || static_cast<size_t>(bytesWritten) < _frameBytes) {
        printf("[V4L] Only wrote %ld of %zu bytes\n", static_cast<long>(bytesWritten), _frameBytes);
        fflush(stdout);
    }
}

void v4l::V4LFrameOutput::shutdownVideo() {
    if (_frameBuffer) {
        delete[] _frameBuffer;
        _frameBuffer = nullptr;
    }
    if (_v4l2fd != -1) {
        close(_v4l2fd);
        _v4l2fd = -1;
    }
}
