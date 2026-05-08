#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

static int xioctl(int fd, unsigned long req, void *arg)
{
    int ret;
    do
    {
        ret = ioctl(fd, req, arg);
    } while (ret == -1 && errno == EINTR);
    return ret;
}

static void print_fourcc(__u32 fmt)
{
    char f[5];
    f[0] = fmt & 0xFF;
    f[1] = (fmt >> 8) & 0xFF;
    f[2] = (fmt >> 16) & 0xFF;
    f[3] = (fmt >> 24) & 0xFF;
    f[4] = '\0';
    printf("%s", f);
}

static void probe_formats(int fd, enum v4l2_buf_type type, const char *label)
{
    struct v4l2_fmtdesc fmtdesc;
    unsigned int i = 0;

    memset(&fmtdesc, 0, sizeof(fmtdesc));
    fmtdesc.type = type;

    printf("\nFormats for %s:\n", label);
    for (i = 0;; ++i)
    {
        fmtdesc.index = i;
        if (xioctl(fd, VIDIOC_ENUM_FMT, &fmtdesc) < 0)
        {
            if (errno == EINVAL)
                break;
            perror("VIDIOC_ENUM_FMT");
            break;
        }
        printf("  [%u] ", fmtdesc.index);
        print_fourcc(fmtdesc.pixelformat);
        printf(" %s\n", fmtdesc.description);
    }
}

static void probe_capture_caps(int fd)
{
    struct v4l2_requestbuffers req;

    memset(&req, 0, sizeof(req));
    req.count = 2;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;

    req.memory = V4L2_MEMORY_MMAP;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) == 0)
        printf("V4L2_MEMORY_MMAP supported for CAPTURE\n");

    req.memory = V4L2_MEMORY_USERPTR;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) == 0)
        printf("V4L2_MEMORY_USERPTR supported for CAPTURE\n");

    req.memory = V4L2_MEMORY_DMABUF;
    if (xioctl(fd, VIDIOC_REQBUFS, &req) == 0)
        printf("V4L2_MEMORY_DMABUF supported for CAPTURE\n");
}

int main(int argc, char **argv)
{
    const char *dev = "/dev/video1";
    int fd;
    struct v4l2_capability cap;

    if (argc > 1)
        dev = argv[1];

    fd = open(dev, O_RDWR);
    if (fd < 0)
    {
        perror("open");
        return 1;
    }

    memset(&cap, 0, sizeof(cap));
    if (xioctl(fd, VIDIOC_QUERYCAP, &cap) < 0)
    {
        perror("VIDIOC_QUERYCAP");
        close(fd);
        return 1;
    }

    printf("Driver: %s\nCard:   %s\nBus:    %s\n", cap.driver, cap.card, cap.bus_info);
    printf("Caps:   0x%08X\n", cap.capabilities);
    printf("DevCaps:0x%08X\n", cap.device_caps);

    probe_formats(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE, "V4L2_BUF_TYPE_VIDEO_CAPTURE");
    probe_formats(fd, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, "V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE");
    probe_capture_caps(fd);

    close(fd);
    return 0;
}
