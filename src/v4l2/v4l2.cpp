/**
 * Copyright (C) 2020, Val Doroshchuk <valbok@gmail.com>
 */

#include "Capture/v4l2.h"
#include "jpeg_utils.h"

#include <string.h>
#include <fcntl.h>              /* low-level i/o */
#include <unistd.h>
#include <errno.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>

#define CLEAR(x) memset(&(x), 0, sizeof(x))

struct Buffer {
    void *start;
    size_t length;
};

static void print_errno(const char *s)
{
    fprintf(stderr, "%s error %d, %s\n", s, errno, strerror(errno));
}

static int xioctl(int fh, int request, void *arg)
{
    int r;
    do {
        r = ioctl(fh, request, arg);
    } while (-1 == r && EINTR == errno);

    return r;
}

static int open_device(const std::string &dev)
{
    struct stat st;
    if (stat(dev.c_str(), &st) == -1) {
        fprintf(stderr, "Cannot identify '%s': %d, %s\n", dev.c_str(),
            errno, strerror(errno));
        return -1;
    }

    if (!S_ISCHR(st.st_mode)) {
        fprintf(stderr, "%s is no device\n", dev.c_str());
        return -1;
    }

    int fd = open(dev.c_str(), O_RDWR /* required */ | O_NONBLOCK, 0);
    if (fd == -1) {
        fprintf(stderr, "Cannot open '%s': %d, %s\n", dev.c_str(),
            errno, strerror(errno));
        return -1;
    }

    return fd;
}

static void close_device(int &fd)
{
    if (close(fd) == -1)
        print_errno("close");

    fd = -1;
}

static bool init_device(int fd, const std::string &dev, unsigned w, unsigned h, unsigned pixel_format, v4l2_format *fmt)
{
    struct v4l2_capability cap;
    struct v4l2_cropcap cropcap;
    struct v4l2_crop crop;

    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) == -1) {
        if (EINVAL == errno)
            fprintf(stderr, "%s is no V4L2 device\n", dev.c_str());
        else
            print_errno("VIDIOC_QUERYCAP");
        return false;
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE)) {
        fprintf(stderr, "%s is no video capture device\n", dev.c_str());
        return false;
    }

    if (!(cap.capabilities & V4L2_CAP_STREAMING)) {
        fprintf(stderr, "%s does not support streaming i/o\n", dev.c_str());
        return false;
    }

    /* Select video input, video standard and tune here. */
    CLEAR(cropcap);

    cropcap.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_CROPCAP, &cropcap) == 0) {
        crop.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        crop.c = cropcap.defrect; /* reset to default */

        if (xioctl(fd, VIDIOC_S_CROP, &crop) == -1) {
            switch (errno) {
            case EINVAL:
                /* Cropping not supported. */
                break;
            default:
                /* Errors ignored. */
                break;
            }
        }
    } else {
        /* Errors ignored. */
    }

    CLEAR(*fmt);

    fmt->type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt->fmt.pix.pixelformat = pixel_format ? pixel_format : V4L2_PIX_FMT_MJPEG;
    fmt->fmt.pix.field = V4L2_FIELD_ANY;
    if (w || h) {
        fmt->fmt.pix.width = w;
        fmt->fmt.pix.height = h;
    }

    if (xioctl(fd, VIDIOC_S_FMT, fmt) == -1) {
        print_errno("VIDIOC_S_FMT");
        return false;
    }

    /* Buggy driver paranoia. */
    unsigned min = fmt->fmt.pix.width * 2;
    if (fmt->fmt.pix.bytesperline < min)
        fmt->fmt.pix.bytesperline = min;
    min = fmt->fmt.pix.bytesperline * fmt->fmt.pix.height;
    if (fmt->fmt.pix.sizeimage < min)
        fmt->fmt.pix.sizeimage = min;

    return true;
}

static void uninit_device(void **buffers, unsigned &n_buffers)
{
    auto buf = (Buffer *)*buffers;
    if (!buf)
        return;

    for (unsigned i = 0; i < n_buffers; ++i) {
        if (munmap(buf[i].start, buf[i].length) == -1)
            print_errno("munmap");
    }

    free(buf);
    *buffers = nullptr;
    n_buffers = 0;
}

static void *init_mmap(int fd, const std::string &dev, unsigned buffers_count)
{
    struct v4l2_requestbuffers req;
    CLEAR(req);
    req.count = buffers_count;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(fd, VIDIOC_REQBUFS, &req) == -1) {
        if (EINVAL == errno)
            fprintf(stderr, "%s does not support memory mapping\n", dev.c_str());
        else
            print_errno("VIDIOC_REQBUFS");
        return nullptr;
    }

    if (req.count < 2) {
        fprintf(stderr, "Insufficient buffer memory on %s\n", dev.c_str());
        return nullptr;
    }

    auto buffers = (Buffer *)calloc(req.count, sizeof(Buffer));
    if (!buffers) {
        fprintf(stderr, "Out of memory\n");
        return nullptr;
    }

    for (unsigned i = 0; i < buffers_count; ++i) {
        struct v4l2_buffer buf;
        CLEAR(buf);

        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (xioctl(fd, VIDIOC_QUERYBUF, &buf) == -1) {
            print_errno("VIDIOC_QUERYBUF");
            uninit_device((void **)&buffers, i);
            return nullptr;
        }

        buffers[i].length = buf.length;
        buffers[i].start =
            mmap(NULL /* start anywhere */,
                 buf.length,
                 PROT_READ | PROT_WRITE /* required */,
                 MAP_SHARED /* recommended */,
                 fd, buf.m.offset);

        if (buffers[i].start == MAP_FAILED) {
            print_errno("mmap");
            uninit_device((void **)&buffers, i);
            return nullptr;
        }
    }

    return buffers;
}

static bool start_capturing(int fd, unsigned n_buffers)
{
    for (unsigned i = 0; i < n_buffers; ++i) {
        struct v4l2_buffer buf;

        CLEAR(buf);
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (xioctl(fd, VIDIOC_QBUF, &buf) == -1) {
            print_errno("VIDIOC_QBUF");
            return false;
        }
    }

    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(fd, VIDIOC_STREAMON, &type) == -1) {
        print_errno("VIDIOC_STREAMON");
        return false;
    }

    return true;
}

static void stop_capturing(int fd)
{
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (fd != -1 && xioctl(fd, VIDIOC_STREAMOFF, &type) == -1)
        print_errno("VIDIOC_STREAMOFF");
}

static struct v4l2_buffer read_frame(int fd)
{
    struct v4l2_buffer buf;
    CLEAR(buf);
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;

    if (xioctl(fd, VIDIOC_DQBUF, &buf) == -1) {
        CLEAR(buf);
        switch (errno) {
        case EAGAIN:
            return buf;
        case EIO:
            /* Could ignore EIO, see spec. */
            /* fall through */
        default:
            print_errno("VIDIOC_DQBUF");
            return buf;
        }
    }

    auto r = buf;

    if (xioctl(fd, VIDIOC_QBUF, &buf) == -1) {
        CLEAR(buf);
        print_errno("VIDIOC_QBUF");
        return buf;
    }

    return r;
}

namespace Capture {

struct v4l2_frame_private
{
    size_t width = 0;
    size_t height = 0;
    unsigned pixel_format = 0;
    unsigned char *data = nullptr;
    size_t size = 0;
    struct timeval timestamp = { 0, 0 };
    bool detached = false;

    ~v4l2_frame_private();
    void release();
    v4l2_frame_private &operator=(const v4l2_frame_private &other);
    void detach();
};

v4l2_frame_private::~v4l2_frame_private()
{
    release();
}

void v4l2_frame_private::release()
{
    if (detached)
        delete [] data;
    data = nullptr;
    detached = false;
}

v4l2_frame_private &v4l2_frame_private::operator=(const v4l2_frame_private &other)
{
    release();
    width = other.width;
    height = other.height;
    pixel_format = other.pixel_format;
    data = other.data;
    size = other.size;
    timestamp = other.timestamp;

    detach();
    return *this;
}

void v4l2_frame_private::detach()
{
    if (!size)
        return;

    auto dst = new unsigned char[size];
    auto d = (unsigned char *)data;
    std::copy(d, d + size, dst);
    data = dst;
    detached = true;
}

v4l2_frame::v4l2_frame()
    : m(new v4l2_frame_private)
{
}

v4l2_frame::~v4l2_frame()
{
    delete m;
}

v4l2_frame::v4l2_frame(v4l2_frame &&other)
    : v4l2_frame()
{
    auto tmp = m;
    m = other.m;
    other.m = tmp;
}

v4l2_frame::v4l2_frame(const v4l2_frame &other)
    : v4l2_frame()
{
    operator=(other);
}

v4l2_frame &v4l2_frame::operator=(const v4l2_frame &other)
{
    *m = *other.m;
    return *this;
}

v4l2_frame::operator bool() const
{
    return m->size;
}

size_t v4l2_frame::width() const
{
    return m->width;
}

size_t v4l2_frame::height() const
{
    return m->height;
}

unsigned v4l2_frame::pixel_format() const
{
    return m->pixel_format;
}

const void *v4l2_frame::data() const
{
    return m->data;
}

size_t v4l2_frame::size() const
{
    return m->size;
}

struct timeval v4l2_frame::timestamp() const
{
    return m->timestamp;
}

v4l2_frame v4l2_frame::convert(unsigned f) const
{
    v4l2_frame frame;
    if (f != V4L2_PIX_FMT_MJPEG)
        return frame;

    switch (m->pixel_format) {
    case V4L2_PIX_FMT_YUYV:
    case V4L2_PIX_FMT_UYVY:
    case V4L2_PIX_FMT_RGB565: {
        unsigned char *output = nullptr;
        frame.m->width = m->width;
        frame.m->height = m->height;
        frame.m->pixel_format = V4L2_PIX_FMT_MJPEG;
        frame.m->timestamp = m->timestamp;
        frame.m->size = jpeg_data(m->pixel_format, m->data, m->width, m->height, output);
        frame.m->data = output;
        frame.m->detach();
        free(output);
        break;
    }
    default:
        break;
    }

    return frame;
}

struct v4l2_private
{
    bool active = false;
    std::string device;
    int fd = -1;
    unsigned requested_pixel_format = 0;
    v4l2_pix_format fmt;
    void *buffers = nullptr;
    unsigned buffers_count = 5;
};

v4l2::v4l2(const std::string &device)
    : m(new v4l2_private)
{
    m->device = device;
}

v4l2::~v4l2()
{
    stop();
    delete m;
}

size_t v4l2::image_size() const
{
    return m->fmt.sizeimage;
}

size_t v4l2::native_width() const
{
    return m->fmt.width;
}

size_t v4l2::native_height() const
{
    return m->fmt.height;
}

size_t v4l2::bytes_perline() const
{
    return m->fmt.bytesperline;
}

unsigned v4l2::pixel_format() const
{
    return m->fmt.pixelformat;
}

bool v4l2::start(size_t width_hint, size_t height_hint, unsigned pixel_format, size_t buffers_count)
{
    if (m->active)
        return false;

    m->fd = open_device(m->device);
    if (m->fd < 0)
        return false;

    v4l2_format fmt;
    if (!init_device(m->fd, m->device, width_hint, height_hint, pixel_format, &fmt)) {
        close_device(m->fd);
        return false;
    }

    m->requested_pixel_format = pixel_format;
    m->fmt = fmt.fmt.pix;
    m->buffers_count = buffers_count;
    m->buffers = init_mmap(m->fd, m->device, m->buffers_count);
    if (!m->buffers) {
        close_device(m->fd);
        return false;
    }

    if (!start_capturing(m->fd, m->buffers_count)) {
        uninit_device(&m->buffers, m->buffers_count);
        close_device(m->fd);
        return false;
    }

    m->active = true;
    return true;
}

void v4l2::stop()
{
    if (!m->active)
        return;

    stop_capturing(m->fd);
    uninit_device(&m->buffers, m->buffers_count);
    close_device(m->fd);
    m->active = false;
}

bool v4l2::is_active() const
{
    return m->active;
}

v4l2_frame v4l2::read_frame() const
{
    v4l2_frame frame;
    while (m->active) {
        fd_set fds;
        struct timeval tv;
        int r;

        FD_ZERO(&fds);
        FD_SET(m->fd, &fds);

        /* Timeout. */
        tv.tv_sec = 2;
        tv.tv_usec = 0;

        r = select(m->fd + 1, &fds, NULL, NULL, &tv);
        if (r == -1) {
            if (errno == EINTR)
                continue;
            break;
        }

        if (r == 0) {
            fprintf(stderr, "select timeout\n");
            break;
        }

        auto buf = ::read_frame(m->fd);
        if (buf.bytesused) {
            frame.m->width = m->fmt.width;
            frame.m->height = m->fmt.height;
            frame.m->pixel_format = m->fmt.pixelformat;
            frame.m->timestamp = buf.timestamp;
            frame.m->size = buf.bytesused;
            frame.m->data = (unsigned char *)((Buffer *)m->buffers)[buf.index].start;

            return frame;
        }

        if (errno == ENODEV)
            break;

        /* EAGAIN - continue select loop. */
    }

    return frame;
}

} // Capture
