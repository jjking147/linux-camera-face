#include <errno.h>
#include <fcntl.h>
#include <linux/fb.h>
#include <linux/videodev2.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/select.h>
#include <sys/time.h>
#include <unistd.h>

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))
#define MAX_BUFFERS 12

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

struct fb_ctx
{
    int fd;
    uint8_t *base;
    size_t length;
    uint32_t width;
    uint32_t height;
    uint32_t bpp;
    uint32_t line_length;
};

static volatile sig_atomic_t g_stop;

static void on_signal(int signo)
{
    (void)signo;
    g_stop = 1;
}

static int xioctl(int fd, unsigned long req, void *arg) // 防止信号打断ioctl这个流程
{
    int ret;
    do
    {
        ret = ioctl(fd, req, arg);
    } while (ret == -1 && errno == EINTR);
    return ret;
}

static uint64_t now_ms(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (uint64_t)tv.tv_sec * 1000ULL + (uint64_t)tv.tv_usec / 1000ULL;
}

static uint8_t clamp_u8(int v) // 限制在0~255之中，钳制函数
{
    if (v < 0)
        return 0;
    if (v > 255)
        return 255;
    return (uint8_t)v;
}

static uint16_t yuv_to_rgb565(uint8_t y, uint8_t u, uint8_t v) // RGB分别是5,6,5位
{
    int c = (int)y - 16;
    int d = (int)u - 128;
    int e = (int)v - 128;

    int r = (298 * c + 409 * e + 128) >> 8;
    int g = (298 * c - 100 * d - 208 * e + 128) >> 8;
    int b = (298 * c + 516 * d + 128) >> 8;

    uint8_t rr = clamp_u8(r);
    uint8_t gg = clamp_u8(g);
    uint8_t bb = clamp_u8(b);

    return (uint16_t)(((rr & 0xF8) << 8) | ((gg & 0xFC) << 3) | (bb >> 3));
}

// 但是这里涉及频繁计算了，后续可以通过查表或者批量赋值进行优化
static void put_pixel_fb(struct fb_ctx *fb, uint32_t x, uint32_t y, uint16_t rgb565)
{
    // line_length是每行字节数，bpp/8是每像素字节数，
    // 也就是说这里是在找到要放入数据的那个像素的起始位置
    uint8_t *dst = fb->base + y * fb->line_length + x * (fb->bpp / 8);

    // 16位的话是RGB565,可以直接进行写入
    if (fb->bpp == 16)
    {
        *(uint16_t *)dst = rgb565;
        return;
    }

    // 如果是32位的数据格式的话，是ARGB8888
    if (fb->bpp == 32)
    {
        uint32_t r = (rgb565 >> 11) & 0x1F; // 高5位为R
        uint32_t g = (rgb565 >> 5) & 0x3F;  // 中间六位为G
        uint32_t b = rgb565 & 0x1F;         // 最后5位为B

        // 都转化成8位
        uint32_t r8 = (r * 255) / 31;
        uint32_t g8 = (g * 255) / 63;
        uint32_t b8 = (b * 255) / 31;

        *(uint32_t *)dst = (0xFFu << 24) | (r8 << 16) | (g8 << 8) | b8;
    }
}

static uint16_t get_src_rgb565(const struct video_ctx *v, const uint8_t *src,
                               uint32_t sx, uint32_t sy)
{
    const uint8_t *line = src + sy * v->bytesperline;

    // 每个RGB565像素占2字节，所以第sx个像素的起始位置就是line+sx*2
    if (v->pixfmt == V4L2_PIX_FMT_RGB565)
    {
        const uint8_t *p = line + sx * 2;
        return (uint16_t)(p[0] | (p[1] << 8));
    }

    // YUYV格式一个像素对占4个字节，就和他的名字一样，Y0 U0 Y1 U1,可以分配给两个水平相邻的像素
    if (v->pixfmt == V4L2_PIX_FMT_YUYV)
    {
        uint32_t pair_x = sx & ~1u;           // 下拉到偶数，0,1都是0；2,3都是2
        const uint8_t *p = line + pair_x * 2; // sx是像素坐标，pair_x只是下拉到偶数，因此相当于还是一个像素占2个字节
        uint8_t yv = (sx & 1u) ? p[2] : p[0]; // 查看他是奇数还是偶数,偶数取Y0,奇数取Y1
        uint8_t u = p[1];
        uint8_t vv = p[3];
        return yuv_to_rgb565(yv, u, vv); // 得到YUYV再转换成RGB就行了
    }

    // 怎么还有UYVY
    if (v->pixfmt == V4L2_PIX_FMT_UYVY)
    {
        uint32_t pair_x = sx & ~1u;
        const uint8_t *p = line + pair_x * 2;
        uint8_t yv = (sx & 1u) ? p[3] : p[1];
        uint8_t u = p[0];
        uint8_t vv = p[2];
        return yuv_to_rgb565(yv, u, vv);
    }
    return 0;
}

// 把视频源转移到帧缓冲区
static void blit_frame(struct video_ctx *v, struct fb_ctx *fb, const uint8_t *src)
{
    uint32_t x;
    uint32_t y;

    if (v->pixfmt == V4L2_PIX_FMT_RGB565 && fb->bpp == 16 &&
        v->width == fb->width && v->height == fb->height &&
        v->bytesperline == fb->line_length)
    {
        memcpy(fb->base, src, (size_t)fb->line_length * fb->height);
        return;
    }
    // 按照图像格式缩放，遍历所有的x和y,然后加个边界保护
    for (y = 0; y < fb->height; ++y)
    {
        uint32_t sy = (uint32_t)(((uint64_t)y * v->height) / fb->height);
        if (sy >= v->height)
            sy = v->height - 1;

        for (x = 0; x < fb->width; ++x)
        {
            uint32_t sx = (uint32_t)(((uint64_t)x * v->width) / fb->width);
            if (sx >= v->width)
                sx = v->width - 1;

            uint16_t rgb565 = get_src_rgb565(v, src, sx, sy);
            put_pixel_fb(fb, x, y, rgb565);
        }
    }
}

static int setup_video(struct video_ctx *v, const char *dev, uint32_t req_w, uint32_t req_h)
{
    struct v4l2_capability cap;
    struct v4l2_format fmt;
    struct v4l2_requestbuffers req;
    uint32_t i;

    // 尝试这三个格式
    uint32_t try_formats[] = {
        V4L2_PIX_FMT_RGB565,
        V4L2_PIX_FMT_YUYV,
        V4L2_PIX_FMT_UYVY,
    };

    v->fd = open(dev, O_RDWR);
    if (v->fd < 0)
    {
        perror("open video");
        return -1;
    }

    memset(&cap, 0, sizeof(cap));
    // 大概是实现了设备信息的获取
    if (xioctl(v->fd, VIDIOC_QUERYCAP, &cap) < 0) // 执行驱动中的ioctl对应的VIDIOC_QUERYCAP，确保不会被信号打断
    {
        perror("VIDIOC_QUERYCAP");
        return -1;
    }
    // 如果设备不支持视频流捕获和上传就无法实现
    if (!(cap.capabilities & V4L2_CAP_VIDEO_CAPTURE) ||
        !(cap.capabilities & V4L2_CAP_STREAMING))
    {
        fprintf(stderr, "device does not support CAPTURE + STREAMING\n");
        return -1;
    }

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
    // 格式全都不匹配
    if (i == ARRAY_SIZE(try_formats))
    {
        fprintf(stderr, "failed to set RGB565/YUYV/UYVY on %s\n", dev);
        return -1;
    }

    v->width = fmt.fmt.pix.width;
    v->height = fmt.fmt.pix.height;
    v->pixfmt = fmt.fmt.pix.pixelformat;
    v->bytesperline = fmt.fmt.pix.bytesperline;

    memset(&req, 0, sizeof(req));
    req.count = 4;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;

    if (xioctl(v->fd, VIDIOC_REQBUFS, &req) < 0)
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

        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;

        if (xioctl(v->fd, VIDIOC_QUERYBUF, &buf) < 0)
        {
            perror("VIDIOC_QUERYBUF");
            return -1;
        }

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

    printf("video: %ux%u, pixfmt=%c%c%c%c, bytesperline=%u\n",
           v->width, v->height,
           v->pixfmt & 0xFF,
           (v->pixfmt >> 8) & 0xFF,
           (v->pixfmt >> 16) & 0xFF,
           (v->pixfmt >> 24) & 0xFF,
           v->bytesperline);

    return 0;
}

static int setup_fb(struct fb_ctx *fb, const char *dev)
{
    struct fb_fix_screeninfo finfo;
    struct fb_var_screeninfo vinfo;

    fb->fd = open(dev, O_RDWR);
    if (fb->fd < 0)
    {
        perror("open fb");
        return -1;
    }

    if (xioctl(fb->fd, FBIOGET_FSCREENINFO, &finfo) < 0)
    {
        perror("FBIOGET_FSCREENINFO");
        return -1;
    }

    if (xioctl(fb->fd, FBIOGET_VSCREENINFO, &vinfo) < 0)
    {
        perror("FBIOGET_VSCREENINFO");
        return -1;
    }

    fb->width = vinfo.xres;
    fb->height = vinfo.yres;
    fb->bpp = vinfo.bits_per_pixel;
    fb->line_length = finfo.line_length;
    fb->length = finfo.smem_len;

    if (fb->bpp != 16 && fb->bpp != 32)
    {
        fprintf(stderr, "unsupported fb bpp: %u (need 16 or 32)\n", fb->bpp);
        return -1;
    }

    if (xioctl(fb->fd, FBIOGET_VSCREENINFO, &vinfo) < 0)
    {
        perror("改动无效\r\n");
    }

    vinfo.bits_per_pixel = 16;
    vinfo.red.offset = 11;
    vinfo.red.length = 5;
    vinfo.green.offset = 5;
    vinfo.green.length = 6;
    vinfo.blue.offset = 0;
    vinfo.blue.length = 5;
    vinfo.transp.offset = 0;
    vinfo.transp.length = 0;
    vinfo.activate = FB_ACTIVATE_NOW;

    if (xioctl(fb->fd, FBIOPUT_VSCREENINFO, &vinfo) < 0)
    {
        perror("FBIOPUT_VSCREENINFO");
        return -1;
    }

    if (xioctl(fb->fd, FBIOGET_VSCREENINFO, &vinfo) < 0)
    {
        perror("FBIOGET_VSCREENINFO");
        return -1;
    }
    if (xioctl(fb->fd, FBIOGET_FSCREENINFO, &finfo) < 0)
    {
        perror("FBIOGET_FSCREENINFO");
        return -1;
    }

    fb->width = vinfo.xres;
    fb->height = vinfo.yres;
    fb->bpp = vinfo.bits_per_pixel;
    fb->line_length = finfo.line_length;
    fb->length = finfo.smem_len;
    printf("fb final bpp=%u\n", vinfo.bits_per_pixel);
    printf("fb: %ux%u, bpp=%u, line_length=%u\n",
           fb->width, fb->height, fb->bpp, fb->line_length);
    fb->base = mmap(NULL, fb->length, PROT_READ | PROT_WRITE, MAP_SHARED, fb->fd, 0);
    if (fb->base == MAP_FAILED)
    {
        perror("mmap fb");
        return -1;
    }
    return 0;
}

static void cleanup_video(struct video_ctx *v)
{
    uint32_t i;

    if (v->fd >= 0)
    {
        enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        xioctl(v->fd, VIDIOC_STREAMOFF, &type);
    }

    for (i = 0; i < v->nbufs; ++i)
    {
        if (v->bufs[i].start && v->bufs[i].start != MAP_FAILED)
            munmap(v->bufs[i].start, v->bufs[i].length);
    }

    if (v->fd >= 0)
        close(v->fd);
}

static void cleanup_fb(struct fb_ctx *fb)
{
    if (fb->base && fb->base != MAP_FAILED)
        munmap(fb->base, fb->length);
    if (fb->fd >= 0)
        close(fb->fd);
}

int main(int argc, char **argv)
{
    const char *video_dev = "/dev/video1";
    const char *fb_dev = "/dev/fb0";
    uint32_t req_w = 1024;
    uint32_t req_h = 600;

    uint64_t fps_t0;
    uint32_t fps_frames;

    struct video_ctx v;
    struct fb_ctx fb;
    enum v4l2_buf_type type;

    memset(&v, 0, sizeof(v));
    memset(&fb, 0, sizeof(fb));
    v.fd = -1;
    fb.fd = -1;

    if (argc > 1)
        video_dev = argv[1];
    if (argc > 2)
        fb_dev = argv[2];
    if (argc > 3)
        req_w = (uint32_t)atoi(argv[3]);
    if (argc > 4)
        req_h = (uint32_t)atoi(argv[4]);

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    if (setup_video(&v, video_dev, req_w, req_h) < 0)
        goto out;

    if (setup_fb(&fb, fb_dev) < 0)
        goto out;

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    if (xioctl(v.fd, VIDIOC_STREAMON, &type) < 0)
    {
        perror("VIDIOC_STREAMON");
        goto out;
    }

    fps_t0 = now_ms();
    fps_frames = 0;
    uint64_t now = 0;
    uint64_t dt = 0;
    while (!g_stop)
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

        blit_frame(&v, &fb, (const uint8_t *)v.bufs[buf.index].start);

        now = now_ms();
        dt = now - fps_t0;
        fps_frames++;
        if (dt >= 1000)
        {
            double fps = (double)fps_frames * 1000.0 / (double)dt;
            printf("frames=%u dt=%llu\n", fps_frames, (unsigned long long)dt);
            printf("fps:%.2f\r\n", fps);
            fflush(stdout);
            fps_t0 = now;
            fps_frames = 0;
        }
        if (xioctl(v.fd, VIDIOC_QBUF, &buf) < 0)
        {
            perror("VIDIOC_QBUF(requeue)");
            break;
        }
    }

out:
    cleanup_fb(&fb);
    cleanup_video(&v);
    return 0;
}
