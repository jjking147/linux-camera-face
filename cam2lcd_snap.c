#include <errno.h>
#include <fcntl.h>
#include <jpeglib.h>
#include <linux/videodev2.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MAX_BUFFERS 8

struct buffer
{
    void *start;
    size_t length;
};

struct video_ctx
{
    int fd;
    uint32_t width;
    uint32_t height;
    uint32_t pixfmt;
    uint32_t bytesperline;
    struct buffer bufs[MAX_BUFFERS];
    uint32_t nbufs;
};

static volatile sig_atomic_t g_stop;

static void on_signal(int signo)
{
    (void)signo;
    g_stop = 1;
}

static int xioctl(int fd, unsigned long req, void *arg)
{
    int ret;
    do
    {
        ret = ioctl(fd, req, arg);
    } while (ret == -1 && errno == EINTR);
    return ret;
}

// 确保输出目录存在，若不存在则创建
static int ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) == 0) // 检查路径是否存在
    {
        if (S_ISDIR(st.st_mode)) // 存在且是目录，成功
        {
            return 0;
        } // 存在但是不是目录，返回-1
        fprintf(stderr, "%s exists and is not a directory\n", path);
        return -1;
    }
    if (errno != ENOENT)
    {
        perror("stat output dir");
        return -1;
    } // 不存在且errno是ENOENT，创建目录
    if (mkdir(path, 0755) < 0)
    {
        perror("mkdir output dir");
        return -1;
    }
    return 0;
}
// 初始化V4L2视频采集设备，配置原始格式并映射DMA缓冲区
static int setup_video(struct video_ctx *v, const char *dev, uint32_t req_w, uint32_t req_h)
{
    struct v4l2_capability cap;
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;
    uint32_t i;
    uint32_t try_formats[] = {
        V4L2_PIX_FMT_RGB565,
        V4L2_PIX_FMT_YUYV,
        V4L2_PIX_FMT_UYVY,
    };

    // 打开设备
    v->fd = open(dev, O_RDWR);
    if (v->fd < 0)
    {
        perror("open video");
        return -1;
    }

    memset(&cap, 0, sizeof(cap));                 // 清空cap的值，方便后续给cap填入值
    if (xioctl(v->fd, VIDIOC_QUERYCAP, &cap) < 0) // 查询设备能力，确保支持视频采集+流式传输
    {
        perror("VIDIOC_QUERYCAP");
        return -1;
    }
    // 如果设备不支持VIDEO_CAPTURE | V4L2_CAP_STREAMING，直接退出
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
        !(cap.capabilities & V4L2_CAP_STREAMING))
    {
        fprintf(stderr, "device does not support CAPTURE + STREAMING\n");
        return -1;
    }

    // 先清空fmt结构体
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    fmt.fmt.pix.width = req_w;
    fmt.fmt.pix.height = req_h;
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    for (i = 0; i < ARRAY_SIZE(try_formats); ++i)
    {
        fmt.fmt.pix.pixelformat = try_formats[i];
        if (xioctl(v->fd, VIDIOC_S_FMT, &fmt) == 0)
            break;
    }

    if (i == ARRAY_SIZE(try_formats))
    {
        perror("VIDIOC_S_FMT (raw)");
        return -1;
    }
    // 驱动返回实际生效的格式，用v保存实际生效的格式
    v->width = fmt.fmt.pix.width;
    v->height = fmt.fmt.pix.height;
    v->pixfmt = fmt.fmt.pix.pixelformat;
    v->bytesperline = fmt.fmt.pix.bytesperline;
    printf("视频流的格式为:0x%08X\r\n", fmt.fmt.pix.pixelformat);
    printf("Requested: %u x %u\n", req_w, req_h);
    printf("Actual   : %u x %u, bytesperline=%u, sizeimage=%u\n",
           fmt.fmt.pix.width,
           fmt.fmt.pix.height,
           fmt.fmt.pix.bytesperline,
           fmt.fmt.pix.sizeimage);

    // 申请4个缓冲区
    memset(&req, 0, sizeof(req));
    req.count = 4;                          // 申请4个缓冲区
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE; // 指定缓冲区类型为视频采集
    req.memory = V4L2_MEMORY_MMAP;          // 指定内存模型为内存映射

    if (xioctl(v->fd, VIDIOC_REQBUFS, &req) < 0) // 开始分配！
    {
        perror("VIDIOC_REQBUFS");
        return -1;
    }

    if (req.count == 0 || req.count > MAX_BUFFERS)
    {
        fprintf(stderr, "bad buffer count: %u\n", req.count);
        return -1;
    }

    v->nbufs = req.count;

    for (i = 0; i < v->nbufs; ++i)
    {
        struct v4l2_buffer buf;
        // 把buf清空
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        // 获得刚刚申请的物理地址和长度
        if (xioctl(v->fd, VIDIOC_QUERYBUF, &buf) < 0)
        {
            perror("VIDIOC_QUERYBUF");
            return -1;
        }
        // 获取成功之后，再进行虚拟映射
        v->bufs[i].length = buf.length;
        v->bufs[i].start = mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                MAP_SHARED, v->fd, buf.m.offset);
        if (v->bufs[i].start == MAP_FAILED)
        {
            perror("mmap video buf");
            return -1;
        }

        if (xioctl(v->fd, VIDIOC_QBUF, &buf) < 0)
        {
            perror("VIDIOC_QBUF");
            return -1;
        }
    }
    // 打印一下摄像头输出的分辨率以及格式
    printf("video: %ux%u, pixfmt=%c%c%c%c\n",
           v->width, v->height,
           v->pixfmt & 0xFF,
           (v->pixfmt >> 8) & 0xFF,
           (v->pixfmt >> 16) & 0xFF,
           (v->pixfmt >> 24) & 0xFF);

    return 0;
}

static void cleanup_video(struct video_ctx *v)
{
    uint32_t i;
    // 停止推流
    if (v->fd >= 0)
    {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(v->fd, VIDIOC_STREAMOFF, &type);
    }
    // 遍历所有缓冲区，对有效的地址取消映射
    for (i = 0; i < v->nbufs; ++i)
    {
        if (v->bufs[i].start && v->bufs[i].start != MAP_FAILED)
            munmap(v->bufs[i].start, v->bufs[i].length);
    }
    // 关闭设备
    if (v->fd >= 0)
        close(v->fd);
}

static void rgb565_to_rgb888(const uint8_t *src, uint8_t *dst, uint32_t pixels)
{
    uint32_t i;
    for (i = 0; i < pixels; ++i)
    {
        uint16_t p = (uint16_t)(src[2 * i] | (src[2 * i + 1] << 8));
        uint8_t r = (uint8_t)(((p >> 11) & 0x1F) * 255 / 31);
        uint8_t g = (uint8_t)(((p >> 5) & 0x3F) * 255 / 63);
        uint8_t b = (uint8_t)((p & 0x1F) * 255 / 31);
        dst[3 * i] = r;
        dst[3 * i + 1] = g;
        dst[3 * i + 2] = b;
    }
}

static inline uint8_t clamp_u8(int v)
{
    if (v < 0)
        return 0;
    if (v > 255)
        return 255;
    return (uint8_t)v;
}

static void yuv_to_rgb(uint8_t y, uint8_t u, uint8_t v, uint8_t *r, uint8_t *g, uint8_t *b)
{
    int c = (int)y - 16;
    int d = (int)u - 128;
    int e = (int)v - 128;
    int rr = (298 * c + 409 * e + 128) >> 8;
    int gg = (298 * c - 100 * d - 208 * e + 128) >> 8;
    int bb = (298 * c + 516 * d + 128) >> 8;
    *r = clamp_u8(rr);
    *g = clamp_u8(gg);
    *b = clamp_u8(bb);
}

static void yuyv_to_rgb888(const uint8_t *src, uint8_t *dst, uint32_t width, uint32_t height, uint32_t bytesperline)
{
    uint32_t y;
    for (y = 0; y < height; ++y)
    {
        const uint8_t *line = src + y * bytesperline;
        uint32_t x;
        for (x = 0; x + 1 < width; x += 2)
        {
            uint8_t y0 = line[0];
            uint8_t u = line[1];
            uint8_t y1 = line[2];
            uint8_t v = line[3];
            uint8_t r, g, b;
            yuv_to_rgb(y0, u, v, &r, &g, &b);
            dst[0] = r;
            dst[1] = g;
            dst[2] = b;
            yuv_to_rgb(y1, u, v, &r, &g, &b);
            dst[3] = r;
            dst[4] = g;
            dst[5] = b;
            line += 4;
            dst += 6;
        }
    }
}

static void uyvy_to_rgb888(const uint8_t *src, uint8_t *dst, uint32_t width, uint32_t height, uint32_t bytesperline)
{
    uint32_t y;
    for (y = 0; y < height; ++y)
    {
        const uint8_t *line = src + y * bytesperline;
        uint32_t x;
        for (x = 0; x + 1 < width; x += 2)
        {
            uint8_t u = line[0];
            uint8_t y0 = line[1];
            uint8_t v = line[2];
            uint8_t y1 = line[3];
            uint8_t r, g, b;
            yuv_to_rgb(y0, u, v, &r, &g, &b);
            dst[0] = r;
            dst[1] = g;
            dst[2] = b;
            yuv_to_rgb(y1, u, v, &r, &g, &b);
            dst[3] = r;
            dst[4] = g;
            dst[5] = b;
            line += 4;
            dst += 6;
        }
    }
}

static int write_jpeg_file(const struct video_ctx *v, const char *out_dir, int index,
                           const uint8_t *data, size_t size, uint8_t *rgb_buf, size_t rgb_size)
{
    char path[512];
    time_t now = time(NULL);
    struct tm tm_now;
    FILE *fp;
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;
    JSAMPROW row_pointer[1];

    if (!localtime_r(&now, &tm_now))
    {
        perror("localtime_r");
        return -1;
    }

    snprintf(path, sizeof(path),
             "%s/img_%04d%02d%02d_%02d%02d%02d_%04d.jpg",
             out_dir,
             tm_now.tm_year + 1900,
             tm_now.tm_mon + 1,
             tm_now.tm_mday,
             tm_now.tm_hour,
             tm_now.tm_min,
             tm_now.tm_sec,
             index);

    fp = fopen(path, "wb");
    if (!fp)
    {
        perror("open output jpg");
        return -1;
    }

    if (v->pixfmt == V4L2_PIX_FMT_JPEG)
    {
        if (fwrite(data, 1, size, fp) != size)
        {
            perror("write jpg");
            fclose(fp);
            return -1;
        }
        fclose(fp);
        printf("saved: %s (%zu bytes)\n", path, size);
        return 0;
    }

    if (rgb_size < (size_t)v->width * v->height * 3)
    {
        fprintf(stderr, "rgb buffer too small\n");
        fclose(fp);
        return -1;
    }

    if (v->pixfmt == V4L2_PIX_FMT_RGB565)
    {
        rgb565_to_rgb888(data, rgb_buf, v->width * v->height);
    }
    else if (v->pixfmt == V4L2_PIX_FMT_YUYV)
    {
        yuyv_to_rgb888(data, rgb_buf, v->width, v->height, v->bytesperline);
    }
    else if (v->pixfmt == V4L2_PIX_FMT_UYVY)
    {
        uyvy_to_rgb888(data, rgb_buf, v->width, v->height, v->bytesperline);
    }
    else
    {
        fprintf(stderr, "unsupported pixfmt for jpeg encode: %c%c%c%c\n",
                v->pixfmt & 0xFF,
                (v->pixfmt >> 8) & 0xFF,
                (v->pixfmt >> 16) & 0xFF,
                (v->pixfmt >> 24) & 0xFF);
        fclose(fp);
        return -1;
    }

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);
    jpeg_stdio_dest(&cinfo, fp);

    cinfo.image_width = v->width;
    cinfo.image_height = v->height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_RGB;

    jpeg_set_defaults(&cinfo);
    jpeg_set_quality(&cinfo, 85, TRUE);
    jpeg_start_compress(&cinfo, TRUE);

    while (cinfo.next_scanline < cinfo.image_height)
    {
        row_pointer[0] = &rgb_buf[cinfo.next_scanline * v->width * 3];
        jpeg_write_scanlines(&cinfo, row_pointer, 1);
    }

    jpeg_finish_compress(&cinfo);
    jpeg_destroy_compress(&cinfo);
    fclose(fp);

    printf("saved: %s (encoded JPEG)\n", path);
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr,
            "Usage: %s [video_dev] [out_dir] [width] [height] [interval_s] [count]\n"
            "  video_dev   default: /dev/video1\n"
            "  out_dir     default: ./out\n"
            "  width       default: 1024\n"
            "  height      default: 600\n"
            "  interval_s  default: 5\n"
            "  count       default: 0 (infinite)\n",
            prog);
}

int main(int argc, char **argv)
{
    const char *video_dev = "/dev/video1";
    const char *out_dir = "./out";
    uint32_t req_w = 1024;
    uint32_t req_h = 600;
    int interval_s = 5;
    int count = 0;

    int captured = 0;
    struct video_ctx v;
    enum v4l2_buf_type type;
    uint8_t *rgb_buf = NULL;
    size_t rgb_size = 0;

    memset(&v, 0, sizeof(v));
    v.fd = -1;

    if (argc > 1)
        video_dev = argv[1];
    if (argc > 2)
        out_dir = argv[2];
    if (argc > 3)
        req_w = (uint32_t)atoi(argv[3]);
    if (argc > 4)
        req_h = (uint32_t)atoi(argv[4]);
    if (argc > 5)
        interval_s = atoi(argv[5]);
    if (argc > 6)
        count = atoi(argv[6]);

    if (interval_s <= 0)
    {
        fprintf(stderr, "interval_s must be > 0\n");
        usage(argv[0]);
        return 1;
    }

    if (ensure_dir(out_dir) < 0)
        return 1;

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (setup_video(&v, video_dev, req_w, req_h) < 0)
        goto out;

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(v.fd, VIDIOC_STREAMON, &type) < 0)
    {
        perror("VIDIOC_STREAMON");
        goto out;
    }

    rgb_size = (size_t)v.width * v.height * 3;
    rgb_buf = (uint8_t *)malloc(rgb_size);
    if (!rgb_buf)
    {
        perror("malloc rgb_buf");
        goto out;
    }

    while (!g_stop && (count == 0 || captured < count))
    {
        fd_set fds;
        struct timeval tv;
        int ret;
        struct v4l2_buffer buf;

        FD_ZERO(&fds);
        FD_SET(v.fd, &fds);
        tv.tv_sec = 2;
        tv.tv_usec = 0;

        ret = select(v.fd + 1, &fds, NULL, NULL, &tv);
        if (ret < 0)
        {
            if (errno == EINTR)
                continue;
            perror("select");
            break;
        }
        if (ret == 0)
        {
            fprintf(stderr, "select timeout\n");
            continue;
        }

        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;

        if (xioctl(v.fd, VIDIOC_DQBUF, &buf) < 0)
        {
            perror("VIDIOC_DQBUF");
            break;
        }

        if (buf.index >= v.nbufs)
        {
            fprintf(stderr, "bad buf index: %u\n", buf.index);
            break;
        }

        if (write_jpeg_file(&v, out_dir, captured + 1,
                            (const uint8_t *)v.bufs[buf.index].start,
                            buf.bytesused, rgb_buf, rgb_size) < 0)
        {
            xioctl(v.fd, VIDIOC_QBUF, &buf);
            break;
        }

        captured++;

        if (xioctl(v.fd, VIDIOC_QBUF, &buf) < 0)
        {
            perror("VIDIOC_QBUF(requeue)");
            break;
        }

        if (!g_stop && (count == 0 || captured < count))
            sleep((unsigned int)interval_s);
    }

out:
    if (rgb_buf)
        free(rgb_buf);
    cleanup_video(&v);
    return 0;
}
