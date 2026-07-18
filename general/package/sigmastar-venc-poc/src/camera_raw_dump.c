#define _DEFAULT_SOURCE

#include "mi_abi.h"
#include "time_sync.h"

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <getopt.h>
#include <netdb.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop;

#define MAX_VENC_PACKS 32
#define RTP_PAYLOAD_TYPE 97
#define RTP_MAX_PAYLOAD 1200

typedef struct {
    MI_U32 sensor, width, height, fps, frames, read_port;
    MI_U32 exposure_us;
    MI_U32 bitrate_kbps;
    MI_U32 crop_x, crop_y, crop_w, crop_h;
    MI_U32 led_on_after, led_on_for, led_off_for, mark_x, mark_y, mark_w, mark_h;
    MI_ModuleId_e read_module;
    MI_U32 user_depth, buf_depth;
    MI_S32 timeout_ms;
    int existing, mirror, flip, led_gpio, led_active_low, led_ready, crop, led_mark, led_is_on, verbose, no_set_depth, venc_dump;
    const char *sensor_config;
    char sensor_config_buf[256];
    const char *out_path;
} raw_cfg_t;

static void on_signal(int sig) { (void)sig; g_stop = 1; }

static unsigned long long monotonic_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000ULL + ts.tv_nsec / 1000ULL;
}

static unsigned long long pc_time_us(void)
{
    return monotonic_us() + (unsigned long long)time_sync_offset_us();
}

static int write_file_string(const char *path, const char *value)
{
    FILE *fp = fopen(path, "w");
    if (!fp) return -1;
    int ret = fputs(value, fp) < 0 ? -1 : 0;
    if (fclose(fp) && !ret) ret = -1;
    return ret;
}

static int gpio_export_if_needed(int gpio)
{
    char path[64];
    char value[16];

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", gpio);
    if (!access(path, F_OK)) return 0;

    snprintf(value, sizeof(value), "%d", gpio);
    if (write_file_string("/sys/class/gpio/export", value)) return -1;
    usleep(100000);
    return 0;
}

static int gpio_set_output(int gpio)
{
    char path[64];

    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/direction", gpio);
    return write_file_string(path, "out");
}

static int led_set(const raw_cfg_t *cfg, int on)
{
    char path[64];
    int value;

    if (cfg->led_gpio < 0 || !cfg->led_ready) return 0;

    value = cfg->led_active_low ? !on : on;
    snprintf(path, sizeof(path), "/sys/class/gpio/gpio%d/value", cfg->led_gpio);
    return write_file_string(path, value ? "1" : "0");
}

static int led_switch(raw_cfg_t *cfg, int on, const char *reason)
{
    if (cfg->led_gpio < 0 || !cfg->led_ready || cfg->led_is_on == on) return 0;

    if (led_set(cfg, on)) {
        fprintf(stderr, "warning: GPIO%d LED %s failed: %s\n", cfg->led_gpio, on ? "on" : "off", strerror(errno));
        return -1;
    }

    cfg->led_is_on = on;
    (void)reason;
    return 0;
}

static void led_prepare(raw_cfg_t *cfg)
{
    if (cfg->led_gpio < 0) return;

    if (gpio_export_if_needed(cfg->led_gpio) || gpio_set_output(cfg->led_gpio)) {
        fprintf(stderr, "warning: GPIO%d LED setup failed: %s\n", cfg->led_gpio, strerror(errno));
        return;
    }
    cfg->led_ready = 1;
    if (led_set(cfg, 0))
        fprintf(stderr, "warning: GPIO%d LED off failed: %s\n", cfg->led_gpio, strerror(errno));
    else {
        cfg->led_is_on = 0;
        printf("GPIO%d LED ready, active_%s\n", cfg->led_gpio, cfg->led_active_low ? "low" : "high");
    }
}

static int load_sym(void *handle, const char *name, void **sym)
{
    *sym = dlsym(handle, name);
    if (!*sym) fprintf(stderr, "missing symbol %s: %s\n", name, dlerror());
    return *sym ? 0 : -1;
}

static int load_libs(mi_camera_libs_t *mi)
{
    memset(mi, 0, sizeof(*mi));
    dlopen("libcam_os_wrapper.so", RTLD_LAZY | RTLD_GLOBAL);
    mi->sys = dlopen("libmi_sys.so", RTLD_LAZY | RTLD_GLOBAL);
    if (!mi->sys) fprintf(stderr, "dlopen libmi_sys.so failed: %s\n", dlerror());
    mi->snr = dlopen("libmi_sensor.so", RTLD_LAZY | RTLD_GLOBAL);
    if (!mi->snr) fprintf(stderr, "dlopen libmi_sensor.so failed: %s\n", dlerror());
    mi->vif = dlopen("libmi_vif.so", RTLD_LAZY | RTLD_GLOBAL);
    if (!mi->vif) fprintf(stderr, "dlopen libmi_vif.so failed: %s\n", dlerror());
    mi->ispalgo = dlopen("libispalgo.so", RTLD_LAZY | RTLD_GLOBAL);
    if (!mi->ispalgo) fprintf(stderr, "dlopen libispalgo.so failed: %s\n", dlerror());
    mi->cus3a = dlopen("libcus3a.so", RTLD_LAZY | RTLD_GLOBAL);
    if (!mi->cus3a) fprintf(stderr, "dlopen libcus3a.so failed: %s\n", dlerror());
    mi->isp = dlopen("libmi_isp.so", RTLD_LAZY | RTLD_GLOBAL);
    if (!mi->isp) fprintf(stderr, "dlopen libmi_isp.so failed: %s\n", dlerror());
    mi->vpe = dlopen("libmi_vpe.so", RTLD_LAZY | RTLD_GLOBAL);
    if (!mi->vpe) fprintf(stderr, "dlopen libmi_vpe.so failed: %s\n", dlerror());
    mi->venc = dlopen("libmi_venc.so", RTLD_LAZY | RTLD_GLOBAL);
    if (!mi->venc) fprintf(stderr, "dlopen libmi_venc.so failed: %s\n", dlerror());
    if (!mi->sys || !mi->snr || !mi->vif || !mi->ispalgo || !mi->cus3a || !mi->isp || !mi->vpe || !mi->venc) {
        return -1;
    }
#define LS(h, n) load_sym((h), #n, (void **)&mi->n)
    return LS(mi->sys, MI_SYS_Init) || LS(mi->sys, MI_SYS_Exit) ||
        LS(mi->sys, MI_SYS_BindChnPort2) || LS(mi->sys, MI_SYS_UnBindChnPort) ||
        LS(mi->sys, MI_SYS_SetChnOutputPortDepth) || LS(mi->sys, MI_SYS_GetChnOutputPortDepth) ||
        LS(mi->sys, MI_SYS_ChnOutputPortGetBuf) || LS(mi->sys, MI_SYS_ChnOutputPortPutBuf) ||
        LS(mi->snr, MI_SNR_SetPlaneMode) || LS(mi->snr, MI_SNR_QueryResCount) ||
        LS(mi->snr, MI_SNR_GetRes) || LS(mi->snr, MI_SNR_SetRes) ||
        LS(mi->snr, MI_SNR_SetFps) || LS(mi->snr, MI_SNR_SetOrien) ||
        LS(mi->snr, MI_SNR_GetPadInfo) || LS(mi->snr, MI_SNR_GetPlaneInfo) ||
        LS(mi->snr, MI_SNR_Enable) || LS(mi->snr, MI_SNR_Disable) ||
        LS(mi->vif, MI_VIF_SetDevAttr) || LS(mi->vif, MI_VIF_EnableDev) ||
        LS(mi->vif, MI_VIF_DisableDev) || LS(mi->vif, MI_VIF_SetChnPortAttr) ||
        LS(mi->vif, MI_VIF_EnableChnPort) || LS(mi->vif, MI_VIF_DisableChnPort) ||
        LS(mi->vpe, MI_VPE_CreateChannel) || LS(mi->vpe, MI_VPE_DestroyChannel) ||
        LS(mi->vpe, MI_VPE_SetChannelParam) || LS(mi->vpe, MI_VPE_StartChannel) ||
        LS(mi->vpe, MI_VPE_StopChannel) || LS(mi->vpe, MI_VPE_SetPortMode) ||
        LS(mi->vpe, MI_VPE_EnablePort) || LS(mi->vpe, MI_VPE_DisablePort) ||
        LS(mi->isp, MI_ISP_API_CmdLoadBinFile) ||
        LS(mi->isp, MI_ISP_AE_GetExposureLimit) || LS(mi->isp, MI_ISP_AE_SetExposureLimit) ||
        LS(mi->venc, MI_VENC_CreateChn) || LS(mi->venc, MI_VENC_DestroyChn) ||
        LS(mi->venc, MI_VENC_StartRecvPic) || LS(mi->venc, MI_VENC_StopRecvPic) ||
        LS(mi->venc, MI_VENC_GetChnDevid) || LS(mi->venc, MI_VENC_Query) ||
        LS(mi->venc, MI_VENC_GetStream) || LS(mi->venc, MI_VENC_ReleaseStream);
#undef LS
}

static void print_output_depth(mi_camera_libs_t *mi, const char *label, MI_SYS_ChnPort_t *port)
{
    MI_U32 user_depth = 0, buf_depth = 0;
    MI_S32 ret = mi->MI_SYS_GetChnOutputPortDepth(port, &user_depth, &buf_depth);

    printf("MI_SYS_GetChnOutputPortDepth %s -> %#x user=%u buf=%u\n",
        label, ret, user_depth, buf_depth);
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) *--end = '\0';
    return s;
}

static int parse_resolution(const char *value, MI_U32 *width, MI_U32 *height)
{
    char *end;
    unsigned long w = strtoul(value, &end, 10);

    if (!w || (*end != 'x' && *end != 'X'))
        return -1;

    unsigned long h = strtoul(end + 1, &end, 10);
    if (!h || *end != '\0')
        return -1;

    *width = (MI_U32)w;
    *height = (MI_U32)h;
    return 0;
}

static void load_majestic_raw_config(raw_cfg_t *cfg)
{
    const char *path = getenv("MAJESTIC_CONFIG");
    FILE *fp;
    char line[256];
    int in_video0 = 0;

    if (!path || !*path) path = "/etc/majestic.yaml";
    fp = fopen(path, "r");
    if (!fp) return;

    while (fgets(line, sizeof(line), fp)) {
        char *s = trim(line);
        char *colon;
        char *value;

        if (*s == '#' || *s == '\0') continue;
        if (strcmp(s, "video0:") == 0) {
            in_video0 = 1;
            continue;
        }
        if (in_video0 && s == line && strchr(s, ':'))
            in_video0 = 0;
        colon = strchr(s, ':');
        if (!colon) continue;
        *colon = '\0';
        value = trim(colon + 1);
        if (strcmp(s, "sensorConfig") == 0 || strcmp(s, "sensor_config") == 0) {
            snprintf(cfg->sensor_config_buf, sizeof(cfg->sensor_config_buf), "%s", value);
            cfg->sensor_config = cfg->sensor_config_buf;
            continue;
        }
        if (!in_video0) continue;
        if (strcmp(s, "size") == 0) {
            parse_resolution(value, &cfg->width, &cfg->height);
        } else if (strcmp(s, "fps") == 0) {
            MI_U32 fps = (MI_U32)strtoul(value, NULL, 0);
            if (fps) cfg->fps = fps;
        } else if (strcmp(s, "exposure") == 0) {
            double exposure_ms = strtod(value, NULL);
            if (exposure_ms > 0.0)
                cfg->exposure_us = (MI_U32)(exposure_ms * 1000.0 + 0.5);
        }
    }
    fclose(fp);
}

static int apply_exposure(mi_camera_libs_t *mi, const raw_cfg_t *cfg)
{
    i6_isp_exp exp;
    MI_S32 ret = MI_SUCCESS;

    memset(&exp, 0, sizeof(exp));
    for (unsigned i = 0; i < 100; i++) {
        ret = mi->MI_ISP_AE_GetExposureLimit(0, &exp);
        if (ret == MI_SUCCESS) break;
        usleep(10000);
    }
    printf("MI_ISP_AE_GetExposureLimit -> %#x shutter=%u..%u us\n", ret, exp.minShutterUs, exp.maxShutterUs);
    if (ret != MI_SUCCESS) return ret;

    exp.minShutterUs = cfg->exposure_us;
    exp.maxShutterUs = cfg->exposure_us;
    ret = mi->MI_ISP_AE_SetExposureLimit(0, &exp);
    printf("MI_ISP_AE_SetExposureLimit shutter=%u us -> %#x\n", cfg->exposure_us, ret);
    return ret;
}

static int load_sensor_config(mi_camera_libs_t *mi, const raw_cfg_t *cfg)
{
    if (!cfg->sensor_config || !*cfg->sensor_config)
        return MI_SUCCESS;
    if (access(cfg->sensor_config, F_OK)) {
        fprintf(stderr, "sensor config %s not found: %s\n", cfg->sensor_config, strerror(errno));
        return -1;
    }

    sleep(1);
    MI_S32 ret = mi->MI_ISP_API_CmdLoadBinFile(0, (char *)cfg->sensor_config, 1234);
    printf("MI_ISP_API_CmdLoadBinFile %s -> %#x\n", cfg->sensor_config, ret);
    return ret;
}

static void close_libs(mi_camera_libs_t *mi)
{
    if (mi->venc) dlclose(mi->venc);
    if (mi->vpe) dlclose(mi->vpe);
    if (mi->isp) dlclose(mi->isp);
    if (mi->cus3a) dlclose(mi->cus3a);
    if (mi->ispalgo) dlclose(mi->ispalgo);
    if (mi->vif) dlclose(mi->vif);
    if (mi->snr) dlclose(mi->snr);
    if (mi->sys) dlclose(mi->sys);
}

static const unsigned char digit_font[10][7] = {
    { 0x0e, 0x11, 0x13, 0x15, 0x19, 0x11, 0x0e },
    { 0x04, 0x0c, 0x04, 0x04, 0x04, 0x04, 0x0e },
    { 0x0e, 0x11, 0x01, 0x02, 0x04, 0x08, 0x1f },
    { 0x1f, 0x02, 0x04, 0x02, 0x01, 0x11, 0x0e },
    { 0x02, 0x06, 0x0a, 0x12, 0x1f, 0x02, 0x02 },
    { 0x1f, 0x10, 0x1e, 0x01, 0x01, 0x11, 0x0e },
    { 0x06, 0x08, 0x10, 0x1e, 0x11, 0x11, 0x0e },
    { 0x1f, 0x01, 0x02, 0x04, 0x08, 0x08, 0x08 },
    { 0x0e, 0x11, 0x11, 0x0e, 0x11, 0x11, 0x0e },
    { 0x0e, 0x11, 0x11, 0x0f, 0x01, 0x02, 0x0c },
};

static const unsigned char colon_font[7] = { 0x00, 0x04, 0x04, 0x00, 0x04, 0x04, 0x00 };
static const unsigned char dash_font[7] = { 0x00, 0x00, 0x00, 0x1f, 0x00, 0x00, 0x00 };
static const unsigned char dot_font[7] = { 0x00, 0x00, 0x00, 0x00, 0x00, 0x0c, 0x0c };

static const unsigned char *glyph_for(char ch)
{
    if (ch >= '0' && ch <= '9') return digit_font[ch - '0'];
    if (ch == ':') return colon_font;
    if (ch == '-') return dash_font;
    if (ch == '.') return dot_font;
    return NULL;
}

static void draw_glyph(unsigned char *y, unsigned width, unsigned height, unsigned stride,
                       unsigned x, unsigned y0, char ch, unsigned scale)
{
    const unsigned char *glyph = glyph_for(ch);

    if (!glyph) return;
    for (unsigned row = 0; row < 7; row++) {
        for (unsigned col = 0; col < 5; col++) {
            if (!(glyph[row] & (1U << (4 - col)))) continue;
            for (unsigned sy = 0; sy < scale; sy++) {
                unsigned py = y0 + row * scale + sy;
                if (py >= height) continue;
                for (unsigned sx = 0; sx < scale; sx++) {
                    unsigned px = x + col * scale + sx;
                    if (px < width) y[(size_t)py * stride + px] = 235;
                }
            }
        }
    }
}

static void overlay_timestamp(MI_SYS_FrameData_t *frame, unsigned long long user_us)
{
    unsigned long long ms = user_us / 1000ULL;
    char text[8];
    unsigned scale = 3;
    unsigned x = 8, y = 8;

    if (!frame->pVirAddr[0] || frame->u32Stride[0] < frame->u16Width) return;
    snprintf(text, sizeof(text), "%04llu", ms % 10000ULL);

    for (char *p = text; *p; p++) {
        if (*p != ' ')
            draw_glyph(frame->pVirAddr[0], frame->u16Width, frame->u16Height, frame->u32Stride[0], x, y, *p, scale);
        x += 6 * scale;
    }
}

static void fill_nv12_rect(MI_SYS_FrameData_t *frame, MI_U32 x, MI_U32 y, MI_U32 w, MI_U32 h,
                           unsigned char y_value, unsigned char u_value, unsigned char v_value)
{
    if (!frame->pVirAddr[0] || !frame->pVirAddr[1]) return;
    if (!frame->u32Stride[0] || !frame->u32Stride[1]) return;
    if (x + w > frame->u16Width || y + h > frame->u16Height) return;

    x &= ~1U;
    y &= ~1U;
    w &= ~1U;
    h &= ~1U;
    if (!w || !h) return;

    for (MI_U32 row = 0; row < h; row++)
        memset(frame->pVirAddr[0] + (size_t)(y + row) * frame->u32Stride[0] + x, y_value, w);

    for (MI_U32 row = 0; row < h / 2; row++) {
        unsigned char *uv = frame->pVirAddr[1] + (size_t)(y / 2 + row) * frame->u32Stride[1] + x;
        for (MI_U32 col = 0; col < w; col += 2) {
            uv[col] = u_value;
            uv[col + 1] = v_value;
        }
    }
}

static void mark_led_on_frame(const raw_cfg_t *cfg, MI_SYS_FrameData_t *frame)
{
    /* BT.601 limited-range green, visible in NV12 without RGB conversion. */
    fill_nv12_rect(frame, cfg->mark_x, cfg->mark_y, cfg->mark_w, cfg->mark_h, 145, 54, 34);
}

typedef struct {
    FILE *fp;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    unsigned char **data;
    size_t *size;
    size_t cap;
    unsigned *seq;
    unsigned long long *pts;
    unsigned written;
    unsigned head, tail, queued, slots;
    int stop;
    int error;
} async_writer_t;

typedef struct {
    int fd;
    struct sockaddr_storage addr;
    socklen_t addrlen;
    MI_U16 seq;
    MI_U32 ssrc;
} rtp_out_t;

static size_t frame_payload_size(const MI_SYS_FrameData_t *f)
{
    switch (f->ePixelFormat) {
    case E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420:
    case E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420_NV21:
        return (size_t)f->u16Width * f->u16Height * 3 / 2;
    case E_MI_SYS_PIXEL_FRAME_YUV422_YUYV:
        return (size_t)f->u16Width * f->u16Height * 2;
    default:
        if (f->pVirAddr[0] && f->u32BufSize)
            return f->u32BufSize;
        if (f->pVirAddr[0] && f->u32Stride[0] && f->u16Height)
            return (size_t)f->u32Stride[0] * f->u16Height;
        return 0;
    }
}

static size_t buf_payload_size(const MI_SYS_BufInfo_t *info)
{
    if (info->eBufType == E_MI_SYS_BUFDATA_RAW)
        return info->stRawData.u32ContentSize ? info->stRawData.u32ContentSize : info->stRawData.u32BufSize;
    if (info->eBufType == E_MI_SYS_BUFDATA_FRAME)
        return frame_payload_size(&info->stFrameData);
    return 0;
}

static size_t output_payload_size(const raw_cfg_t *cfg, const MI_SYS_BufInfo_t *info)
{
    if (cfg->crop && info->eBufType == E_MI_SYS_BUFDATA_FRAME &&
        (info->stFrameData.ePixelFormat == E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420 ||
         info->stFrameData.ePixelFormat == E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420_NV21))
        return (size_t)cfg->crop_w * cfg->crop_h * 3 / 2;

    return buf_payload_size(info);
}

static int copy_plane_to_buffer(unsigned char **dst, const MI_SYS_FrameData_t *frame,
                                unsigned plane, unsigned width, unsigned height)
{
    const unsigned char *src = frame->pVirAddr[plane];
    MI_U32 stride = frame->u32Stride[plane];

    if (!src || stride < width)
        return -1;
    for (unsigned y = 0; y < height; y++) {
        memcpy(*dst, src + (size_t)y * stride, width);
        *dst += width;
    }
    return 0;
}

static int copy_frame_to_buffer(unsigned char *dst, const MI_SYS_BufInfo_t *info)
{
    const MI_SYS_FrameData_t *f = &info->stFrameData;

    if (info->eBufType == E_MI_SYS_BUFDATA_RAW) {
        size_t size = buf_payload_size(info);
        if (!info->stRawData.pVirAddr || !size)
            return -1;
        memcpy(dst, info->stRawData.pVirAddr, size);
        return 0;
    }

    switch (f->ePixelFormat) {
    case E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420:
    case E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420_NV21:
        return copy_plane_to_buffer(&dst, f, 0, f->u16Width, f->u16Height) ||
               copy_plane_to_buffer(&dst, f, 1, f->u16Width, f->u16Height / 2);
    case E_MI_SYS_PIXEL_FRAME_YUV422_YUYV:
        return copy_plane_to_buffer(&dst, f, 0, f->u16Width * 2, f->u16Height);
    default:
        if (f->pVirAddr[0] && f->u32BufSize) {
            memcpy(dst, f->pVirAddr[0], f->u32BufSize);
            return 0;
        }
        if (f->pVirAddr[0] && f->u32Stride[0] && f->u16Height) {
            memcpy(dst, f->pVirAddr[0], (size_t)f->u32Stride[0] * f->u16Height);
            return 0;
        }
        fprintf(stderr, "unsupported pixel format %d\n", f->ePixelFormat);
        return -1;
    }
}

static int copy_nv12_crop_to_buffer(unsigned char *dst, const MI_SYS_FrameData_t *f,
                                    MI_U32 x, MI_U32 y, MI_U32 w, MI_U32 h)
{
    if (!f->pVirAddr[0] || !f->pVirAddr[1] || !f->u32Stride[0] || !f->u32Stride[1])
        return -1;
    if (x + w > f->u16Width || y + h > f->u16Height || (x | y | w | h) & 1U)
        return -1;

    for (MI_U32 row = 0; row < h; row++) {
        memcpy(dst, f->pVirAddr[0] + (size_t)(y + row) * f->u32Stride[0] + x, w);
        dst += w;
    }
    for (MI_U32 row = 0; row < h / 2; row++) {
        memcpy(dst, f->pVirAddr[1] + (size_t)(y / 2 + row) * f->u32Stride[1] + x, w);
        dst += w;
    }
    return 0;
}

static int copy_output_to_buffer(unsigned char *dst, const raw_cfg_t *cfg, const MI_SYS_BufInfo_t *info)
{
    if (cfg->crop && info->eBufType == E_MI_SYS_BUFDATA_FRAME &&
        (info->stFrameData.ePixelFormat == E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420 ||
         info->stFrameData.ePixelFormat == E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420_NV21))
        return copy_nv12_crop_to_buffer(dst, &info->stFrameData, cfg->crop_x, cfg->crop_y, cfg->crop_w, cfg->crop_h);

    return copy_frame_to_buffer(dst, info);
}

static void *async_writer_main(void *arg)
{
    async_writer_t *w = arg;

    for (;;) {
        pthread_mutex_lock(&w->lock);
        while (!w->queued && !w->stop)
            pthread_cond_wait(&w->cond, &w->lock);
        if (!w->queued && w->stop) {
            pthread_mutex_unlock(&w->lock);
            break;
        }
        unsigned slot = w->tail;
        size_t size = w->size[slot];
        unsigned seq = w->seq[slot];
        unsigned long long pts = w->pts[slot];
        pthread_mutex_unlock(&w->lock);

        if (fwrite(w->data[slot], 1, size, w->fp) != size) {
            fprintf(stderr, "writer: failed frame seq=%u pts=%llu: %s\n", seq, pts, strerror(errno));
            pthread_mutex_lock(&w->lock);
            w->error = 1;
            w->stop = 1;
            pthread_cond_broadcast(&w->cond);
            pthread_mutex_unlock(&w->lock);
            break;
        }

        pthread_mutex_lock(&w->lock);
        w->written++;
        w->tail = (w->tail + 1) % w->slots;
        w->queued--;
        pthread_cond_broadcast(&w->cond);
        pthread_mutex_unlock(&w->lock);
    }
    return NULL;
}

static int async_writer_init(async_writer_t *w, FILE *fp, size_t cap)
{
    const unsigned slots = 4;

    memset(w, 0, sizeof(*w));
    w->fp = fp;
    w->cap = cap;
    w->slots = slots;
    w->data = calloc(slots, sizeof(*w->data));
    w->size = calloc(slots, sizeof(*w->size));
    w->seq = calloc(slots, sizeof(*w->seq));
    w->pts = calloc(slots, sizeof(*w->pts));
    if (!w->data || !w->size || !w->seq || !w->pts)
        return -1;
    for (unsigned i = 0; i < slots; i++) {
        w->data[i] = malloc(cap);
        if (!w->data[i]) return -1;
    }
    pthread_mutex_init(&w->lock, NULL);
    pthread_cond_init(&w->cond, NULL);
    return 0;
}

static void async_writer_destroy(async_writer_t *w)
{
    pthread_mutex_destroy(&w->lock);
    pthread_cond_destroy(&w->cond);
    if (w->data) {
        for (unsigned i = 0; i < w->slots; i++)
            free(w->data[i]);
    }
    free(w->data);
    free(w->size);
    free(w->seq);
    free(w->pts);
}

static void async_writer_stop(async_writer_t *w, pthread_t thread)
{
    pthread_mutex_lock(&w->lock);
    w->stop = 1;
    pthread_cond_broadcast(&w->cond);
    pthread_mutex_unlock(&w->lock);
    pthread_join(thread, NULL);
}

static int parse_rtp_url(const char *url, char *host, size_t host_len, char *port, size_t port_len)
{
    const char *p = strncmp(url, "rtp://", 6) == 0 ? url + 6 : NULL;
    const char *colon;

    if (!p) return -1;
    colon = strrchr(p, ':');
    if (!colon || colon == p || !colon[1]) return -1;
    if ((size_t)(colon - p) >= host_len || strlen(colon + 1) >= port_len) return -1;
    memcpy(host, p, colon - p);
    host[colon - p] = '\0';
    strcpy(port, colon + 1);
    return 0;
}

static int rtp_open(rtp_out_t *out, const char *url)
{
    char host[128], port[16];
    struct addrinfo hints, *res = NULL;
    int ret;

    memset(out, 0, sizeof(*out));
    out->fd = -1;
    out->seq = (MI_U16)(monotonic_us() & 0xffff);
    out->ssrc = (MI_U32)(monotonic_us() ^ 0x56504f43U);
    if (parse_rtp_url(url, host, sizeof(host), port, sizeof(port))) return -1;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_DGRAM;
    ret = getaddrinfo(host, port, &hints, &res);
    if (ret) {
        fprintf(stderr, "getaddrinfo(%s:%s): %s\n", host, port, gai_strerror(ret));
        return -1;
    }
    out->fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (out->fd < 0) {
        fprintf(stderr, "socket: %s\n", strerror(errno));
        freeaddrinfo(res);
        return -1;
    }
    memcpy(&out->addr, res->ai_addr, res->ai_addrlen);
    out->addrlen = res->ai_addrlen;
    freeaddrinfo(res);
    printf("RTP output: %s:%s payload=%u ssrc=%#x\n", host, port, RTP_PAYLOAD_TYPE, out->ssrc);
    return 0;
}

static void rtp_close(rtp_out_t *out)
{
    if (out->fd >= 0) close(out->fd);
    out->fd = -1;
}

static int rtp_send_packet(rtp_out_t *out, const unsigned char *payload, size_t len, MI_U32 timestamp, int marker)
{
    unsigned char pkt[12 + RTP_MAX_PAYLOAD];

    if (len > RTP_MAX_PAYLOAD) return -1;
    pkt[0] = 0x80;
    pkt[1] = RTP_PAYLOAD_TYPE | (marker ? 0x80 : 0);
    pkt[2] = out->seq >> 8;
    pkt[3] = out->seq & 0xff;
    pkt[4] = timestamp >> 24;
    pkt[5] = timestamp >> 16;
    pkt[6] = timestamp >> 8;
    pkt[7] = timestamp;
    pkt[8] = out->ssrc >> 24;
    pkt[9] = out->ssrc >> 16;
    pkt[10] = out->ssrc >> 8;
    pkt[11] = out->ssrc;
    memcpy(pkt + 12, payload, len);
    if (sendto(out->fd, pkt, len + 12, 0, (struct sockaddr *)&out->addr, out->addrlen) < 0) {
        fprintf(stderr, "sendto: %s\n", strerror(errno));
        return -1;
    }
    out->seq++;
    return 0;
}

static int rtp_send_h265_nal(rtp_out_t *out, const unsigned char *nal, size_t len, MI_U32 timestamp, int marker)
{
    if (len <= RTP_MAX_PAYLOAD) return rtp_send_packet(out, nal, len, timestamp, marker);
    if (len < 3) return 0;

    unsigned char fu[3 + RTP_MAX_PAYLOAD];
    unsigned char type = (nal[0] >> 1) & 0x3f;
    size_t off = 2;
    int start = 1;
    while (off < len) {
        size_t chunk = len - off;
        int end;
        if (chunk > RTP_MAX_PAYLOAD - 3) chunk = RTP_MAX_PAYLOAD - 3;
        end = off + chunk == len;
        fu[0] = (49 << 1) | (nal[0] & 0x81);
        fu[1] = nal[1];
        fu[2] = type | (start ? 0x80 : 0) | (end ? 0x40 : 0);
        memcpy(fu + 3, nal + off, chunk);
        if (rtp_send_packet(out, fu, chunk + 3, timestamp, marker && end)) return -1;
        off += chunk;
        start = 0;
    }
    return 0;
}

static const unsigned char *find_start_code(const unsigned char *p, const unsigned char *end, size_t *prefix)
{
    while (p + 3 <= end) {
        if (p[0] == 0 && p[1] == 0 && p[2] == 1) { *prefix = 3; return p; }
        if (p + 4 <= end && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) { *prefix = 4; return p; }
        p++;
    }
    return NULL;
}

static int rtp_send_h265_frame(rtp_out_t *out, const unsigned char *data, size_t len, MI_U32 timestamp)
{
    const unsigned char *end = data + len;
    size_t prefix;
    const unsigned char *sc = find_start_code(data, end, &prefix);

    if (!sc) return rtp_send_h265_nal(out, data, len, timestamp, 1);
    while (sc) {
        const unsigned char *nal = sc + prefix;
        const unsigned char *next = find_start_code(nal, end, &prefix);
        const unsigned char *nal_end = next ? next : end;
        while (nal_end > nal && nal_end[-1] == 0) nal_end--;
        if (nal_end > nal && rtp_send_h265_nal(out, nal, nal_end - nal, timestamp, next == NULL)) return -1;
        sc = next;
    }
    return 0;
}

static int dump_venc_stream(mi_camera_libs_t *mi, raw_cfg_t *cfg)
{
    char meta_path[512];
    FILE *meta;
    FILE *fp = NULL;
    rtp_out_t rtp;
    int use_rtp = strncmp(cfg->out_path, "rtp://", 6) == 0;
    unsigned frames = 0;
    unsigned empty = 0;
    unsigned long long bytes = 0;
    unsigned long long start_us = monotonic_us();

    memset(&rtp, 0, sizeof(rtp));
    rtp.fd = -1;
    if (use_rtp) {
        if (rtp_open(&rtp, cfg->out_path)) return -1;
        snprintf(meta_path, sizeof(meta_path), "/mnt/mmcblk0p1/venc-dump.tsv");
    } else {
        fp = fopen(cfg->out_path, "wb");
        if (!fp) {
            fprintf(stderr, "open %s failed: %s\n", cfg->out_path, strerror(errno));
            return -1;
        }
        snprintf(meta_path, sizeof(meta_path), "%s.tsv", cfg->out_path);
    }
    meta = fopen(meta_path, "w");
    if (!meta) {
        fprintf(stderr, "open %s failed: %s\n", meta_path, strerror(errno));
        if (fp) fclose(fp);
        rtp_close(&rtp);
        return -1;
    }
    fprintf(meta, "type\tframe\tseq\tpts_us\tpc_time_us\tmono_us\tbytes\tled\n");

    while (!g_stop && (!cfg->frames || frames < cfg->frames)) {
        MI_VENC_ChnStat_t stat;
        MI_VENC_Pack_t packs[MAX_VENC_PACKS];
        MI_VENC_Stream_t stream;
        MI_S32 ret;

        int led_should_be_on = 0;
        if (frames >= cfg->led_on_after && cfg->led_on_for) {
            MI_U32 cycle = cfg->led_on_for + cfg->led_off_for;
            MI_U32 phase = cycle ? (frames - cfg->led_on_after) % cycle : 0;
            led_should_be_on = !cfg->led_off_for || phase < cfg->led_on_for;
        }
        if (cfg->led_is_on != led_should_be_on) {
            led_switch(cfg, led_should_be_on, led_should_be_on ? "mark-cycle-on" : "mark-cycle-off");
            fprintf(meta, "led-%s\t%u\t\t\t%llu\t%llu\t\t%d\n",
                led_should_be_on ? "on" : "off", frames, pc_time_us(), monotonic_us(), cfg->led_is_on);
        }

        memset(&stat, 0, sizeof(stat));
        ret = mi->MI_VENC_Query(0, &stat);
        if (ret) {
            fprintf(stderr, "MI_VENC_Query -> %#x\n", ret);
            usleep(1000);
            continue;
        }
        if (!stat.u32CurPacks) {
            if (++empty > 2000) break;
            usleep(1000);
            continue;
        }
        empty = 0;

        memset(packs, 0, sizeof(packs));
        memset(&stream, 0, sizeof(stream));
        stream.pstPack = packs;
        stream.u32PackCount = stat.u32CurPacks > MAX_VENC_PACKS ? MAX_VENC_PACKS : stat.u32CurPacks;
        ret = mi->MI_VENC_GetStream(0, &stream, 1000);
        if (ret) {
            fprintf(stderr, "MI_VENC_GetStream -> %#x\n", ret);
            usleep(1000);
            continue;
        }

        unsigned frame_bytes = 0;
        MI_U64 frame_pts = 0;
        unsigned char *frame_data = NULL;
        size_t frame_len = 0, frame_cap = 0;
        for (MI_U32 i = 0; i < stream.u32PackCount; i++) {
            MI_VENC_Pack_t *pack = &packs[i];
            if (!pack->pu8Addr || pack->u32Len <= pack->u32Offset)
                continue;
            size_t len = pack->u32Len - pack->u32Offset;
            if (use_rtp) {
                if (frame_len + len > frame_cap) {
                    size_t new_cap = frame_cap ? frame_cap * 2 : 65536;
                    while (new_cap < frame_len + len) new_cap *= 2;
                    unsigned char *new_data = realloc(frame_data, new_cap);
                    if (!new_data) {
                        fprintf(stderr, "alloc venc frame failed\n");
                        free(frame_data);
                        mi->MI_VENC_ReleaseStream(0, &stream);
                        fclose(meta);
                        rtp_close(&rtp);
                        return -1;
                    }
                    frame_data = new_data;
                    frame_cap = new_cap;
                }
                memcpy(frame_data + frame_len, pack->pu8Addr + pack->u32Offset, len);
                frame_len += len;
            } else {
                if (fwrite(pack->pu8Addr + pack->u32Offset, 1, len, fp) != len) {
                    fprintf(stderr, "write venc stream failed: %s\n", strerror(errno));
                    mi->MI_VENC_ReleaseStream(0, &stream);
                    fclose(meta);
                    fclose(fp);
                    return -1;
                }
            }
            if (!frame_pts && pack->u64PTS)
                frame_pts = pack->u64PTS;
            frame_bytes += (unsigned)len;
            bytes += len;
        }
        if (use_rtp && frame_len) {
            MI_U32 rtp_ts = frames * (90000U / (cfg->fps ? cfg->fps : 120));
            if (rtp_send_h265_frame(&rtp, frame_data, frame_len, rtp_ts)) {
                free(frame_data);
                mi->MI_VENC_ReleaseStream(0, &stream);
                fclose(meta);
                rtp_close(&rtp);
                return -1;
            }
        }
        free(frame_data);

        fprintf(meta, "frame\t%u\t%u\t%llu\t%llu\t%llu\t%u\t%d\n",
            frames, stream.u32Seq, (unsigned long long)frame_pts,
            pc_time_us(), monotonic_us(), frame_bytes, cfg->led_is_on);
        mi->MI_VENC_ReleaseStream(0, &stream);
        frames++;
    }
    led_switch(cfg, 0, "capture-end");
    fprintf(meta, "led-off\t%u\t\t\t%llu\t%llu\t\t%d\n",
        frames, pc_time_us(), monotonic_us(), cfg->led_is_on);
    fclose(meta);
    if (fp) fclose(fp);
    rtp_close(&rtp);

    unsigned long long elapsed_us = monotonic_us() - start_us;
    printf("venc-dump frames=%u bytes=%llu meta=%s elapsed=%llu.%03llu s avg=%llu.%03llu KB/s\n",
        frames, bytes, meta_path, elapsed_us / 1000000ULL, (elapsed_us / 1000ULL) % 1000ULL,
        elapsed_us ? (bytes * 1000000ULL / elapsed_us) / 1024ULL : 0,
        elapsed_us ? ((bytes * 1000000ULL / elapsed_us) % 1024ULL) * 1000ULL / 1024ULL : 0);
    return frames ? 0 : -1;
}

static int async_writer_submit_or_drop(async_writer_t *w, const raw_cfg_t *cfg, const MI_SYS_BufInfo_t *info, int *dropped)
{
    const MI_SYS_FrameData_t *f = &info->stFrameData;
    size_t size = output_payload_size(cfg, info);

    *dropped = 0;
    if (!size)
        return -1;

    pthread_mutex_lock(&w->lock);
    if (w->error) {
        pthread_mutex_unlock(&w->lock);
        return -1;
    }
    if (w->queued >= w->slots) {
        *dropped = 1;
        pthread_mutex_unlock(&w->lock);
        return 0;
    }
    if (size > w->cap) {
        fprintf(stderr, "payload %zu exceeds writer buffer %zu; frame is %ux%u stride=%u/%u bufType=%d\n",
            size, w->cap, f->u16Width, f->u16Height, f->u32Stride[0], f->u32Stride[1], info->eBufType);
        pthread_mutex_unlock(&w->lock);
        return -1;
    }
    unsigned slot = w->head;
    if (copy_output_to_buffer(w->data[slot], cfg, info)) {
        pthread_mutex_unlock(&w->lock);
        return -1;
    }
    w->size[slot] = size;
    w->seq[slot] = info->u32SequenceNumber;
    w->pts[slot] = (unsigned long long)info->u64Pts;
    w->head = (w->head + 1) % w->slots;
    w->queued++;
    pthread_cond_signal(&w->cond);
    pthread_mutex_unlock(&w->lock);
    return 0;
}

static void probe_venc(mi_camera_libs_t *mi)
{
    MI_VENC_ChnStat_t stat;
    MI_VENC_Pack_t packs[8];
    MI_VENC_Stream_t stream;

    memset(&stat, 0, sizeof(stat));
    MI_S32 ret = mi->MI_VENC_Query(0, &stat);
    printf("MI_VENC_Query -> %#x packs=%u frames=%u bytes=%u\n", ret,
        stat.u32CurPacks, stat.u32LeftStreamFrames, stat.u32LeftStreamBytes);
    if (ret || !stat.u32CurPacks)
        return;

    memset(packs, 0, sizeof(packs));
    memset(&stream, 0, sizeof(stream));
    stream.pstPack = packs;
    stream.u32PackCount = stat.u32CurPacks > 8 ? 8 : stat.u32CurPacks;
    ret = mi->MI_VENC_GetStream(0, &stream, 1000);
    printf("MI_VENC_GetStream -> %#x packCount=%u seq=%u\n", ret, stream.u32PackCount, stream.u32Seq);
    if (!ret) {
        for (MI_U32 i = 0; i < stream.u32PackCount; i++)
            printf("  pack%u len=%u pts=%llu frameEnd=%d\n", i, stream.pstPack[i].u32Len,
                (unsigned long long)stream.pstPack[i].u64PTS, stream.pstPack[i].bFrameEnd);
        mi->MI_VENC_ReleaseStream(0, &stream);
    }
}

static int create_pipeline(mi_camera_libs_t *mi, raw_cfg_t *cfg, i6_snr_plane *plane)
{
    MI_S32 ret;
    MI_U32 count = 0;
    MI_U8 profile = 0xff;
    MI_U32 profile_area = ~0U;
    i6_snr_res res;
    i6_snr_pad pad;

    if ((ret = mi->MI_SYS_Init())) { fprintf(stderr, "MI_SYS_Init -> %#x\n", ret); return -1; }
    if ((ret = mi->MI_SNR_SetPlaneMode(cfg->sensor, 0))) return fprintf(stderr, "MI_SNR_SetPlaneMode -> %#x\n", ret), -1;
    if ((ret = mi->MI_SNR_QueryResCount(cfg->sensor, &count))) return fprintf(stderr, "MI_SNR_QueryResCount -> %#x\n", ret), -1;
    for (MI_U8 i = 0; i < count; i++) {
        if ((ret = mi->MI_SNR_GetRes(cfg->sensor, i, &res))) return fprintf(stderr, "MI_SNR_GetRes -> %#x\n", ret), -1;
        if (cfg->width <= res.crop.width && cfg->height <= res.crop.height && cfg->fps <= res.maxFps) {
            MI_U32 area = (MI_U32)res.crop.width * res.crop.height;
            if (area < profile_area) {
                profile = i;
                profile_area = area;
            }
        }
    }
    if (profile == 0xff) return fprintf(stderr, "no sensor profile for %ux%u@%u\n", cfg->width, cfg->height, cfg->fps), -1;
    if ((ret = mi->MI_SNR_SetRes(cfg->sensor, profile))) return fprintf(stderr, "MI_SNR_SetRes -> %#x\n", ret), -1;
    if ((ret = mi->MI_SNR_SetFps(cfg->sensor, cfg->fps))) return fprintf(stderr, "MI_SNR_SetFps -> %#x\n", ret), -1;
    if ((ret = mi->MI_SNR_SetOrien(cfg->sensor, cfg->mirror, cfg->flip))) return fprintf(stderr, "MI_SNR_SetOrien -> %#x\n", ret), -1;
    if ((ret = mi->MI_SNR_GetPadInfo(cfg->sensor, &pad))) return fprintf(stderr, "MI_SNR_GetPadInfo -> %#x\n", ret), -1;
    if ((ret = mi->MI_SNR_GetPlaneInfo(cfg->sensor, 0, plane))) return fprintf(stderr, "MI_SNR_GetPlaneInfo -> %#x\n", ret), -1;

    i6_vif_dev vif_dev = {0};
    vif_dev.intf = pad.intf;
    vif_dev.work = vif_dev.intf == I6_INTF_BT656 ? I6_VIF_WORK_1MULTIPLEX :
        (cfg->read_module == E_MI_MODULE_ID_VIF ? I6_VIF_WORK_RGB_FRAME : I6_VIF_WORK_RGB_REALTIME);
    vif_dev.hdr = I6_HDR_OFF;
    if (vif_dev.intf == I6_INTF_MIPI) { vif_dev.edge = I6_EDGE_DOUBLE; vif_dev.input = pad.intfAttr.mipi.input; }
    else if (vif_dev.intf == I6_INTF_BT656) { vif_dev.edge = pad.intfAttr.bt656.edge; vif_dev.sync = pad.intfAttr.bt656.sync; }
    if ((ret = mi->MI_VIF_SetDevAttr(0, &vif_dev))) return fprintf(stderr, "MI_VIF_SetDevAttr -> %#x\n", ret), -1;
    if ((ret = mi->MI_VIF_EnableDev(0))) return fprintf(stderr, "MI_VIF_EnableDev -> %#x\n", ret), -1;

    i6_vif_port vif_port = {0};
    vif_port.capt = plane->capt;
    vif_port.dest.width = plane->capt.width;
    vif_port.dest.height = plane->capt.height;
    vif_port.pixFmt = plane->bayer > I6_BAYER_END ? plane->pixFmt : (i6_pixfmt)(I6_PIXFMT_RGB_BAYER + plane->precision * I6_BAYER_END + plane->bayer);
    vif_port.frate = I6_VIF_FRATE_FULL;
    MI_U32 vif_port_id = cfg->read_module == E_MI_MODULE_ID_VIF ? cfg->read_port : 0;
    if ((ret = mi->MI_VIF_SetChnPortAttr(0, vif_port_id, &vif_port))) return fprintf(stderr, "MI_VIF_SetChnPortAttr port%u -> %#x\n", vif_port_id, ret), -1;
    if (cfg->read_module == E_MI_MODULE_ID_VIF) {
        MI_SYS_ChnPort_t vif_out = { E_MI_MODULE_ID_VIF, 0, 0, cfg->read_port };
        print_output_depth(mi, "VIF before set", &vif_out);
        if (!cfg->no_set_depth) {
            ret = mi->MI_SYS_SetChnOutputPortDepth(&vif_out, cfg->user_depth, cfg->buf_depth);
            printf("MI_SYS_SetChnOutputPortDepth VIF before enable -> %#x\n", ret);
            print_output_depth(mi, "VIF after set", &vif_out);
            if (ret) return -1;
        }
    }
    if ((ret = mi->MI_VIF_EnableChnPort(0, vif_port_id))) return fprintf(stderr, "MI_VIF_EnableChnPort port%u -> %#x\n", vif_port_id, ret), -1;
    if ((ret = mi->MI_SNR_Enable(cfg->sensor))) return fprintf(stderr, "MI_SNR_Enable -> %#x\n", ret), -1;

    if (cfg->read_module == E_MI_MODULE_ID_VIF) {
        printf("VIF-only mode: skipping ISP bin load and AE controls\n");
        return 0;
    }

    i6e_vpe_chn vpe_chn = {0};
    vpe_chn.capt.width = plane->capt.width;
    vpe_chn.capt.height = plane->capt.height;
    vpe_chn.pixFmt = vif_port.pixFmt;
    vpe_chn.hdr = I6_HDR_OFF;
    vpe_chn.sensor = (i6_vpe_sens)(cfg->sensor + 1);
    vpe_chn.mode = I6_VPE_MODE_REALTIME;
    if ((ret = mi->MI_VPE_CreateChannel(0, &vpe_chn))) return fprintf(stderr, "MI_VPE_CreateChannel -> %#x\n", ret), -1;

    i6e_vpe_para vpe_para = {0};
    vpe_para.hdr = I6_HDR_OFF;
    vpe_para.level3DNR = 1;
    if ((ret = mi->MI_VPE_SetChannelParam(0, &vpe_para))) return fprintf(stderr, "MI_VPE_SetChannelParam -> %#x\n", ret), -1;
    if ((ret = mi->MI_VPE_StartChannel(0))) return fprintf(stderr, "MI_VPE_StartChannel -> %#x\n", ret), -1;

    MI_SYS_ChnPort_t src = { E_MI_MODULE_ID_VIF, 0, 0, 0 };
    MI_SYS_ChnPort_t dst = { E_MI_MODULE_ID_VPE, 0, 0, 0 };
    i6_vpe_port out = {0};
    out.output.width = cfg->width;
    out.output.height = cfg->height;
    out.pixFmt = I6_PIXFMT_YUV420SP;
    out.compress = I6_COMPR_NONE;
    if ((ret = mi->MI_VPE_SetPortMode(0, 0, &out))) return fprintf(stderr, "MI_VPE_SetPortMode -> %#x\n", ret), -1;
    print_output_depth(mi, "VPE port0 before set", &dst);
    if (!cfg->no_set_depth) {
        ret = mi->MI_SYS_SetChnOutputPortDepth(&dst, cfg->user_depth, cfg->buf_depth);
        printf("MI_SYS_SetChnOutputPortDepth -> %#x\n", ret);
        print_output_depth(mi, "VPE port0 after set", &dst);
    }
    if ((ret = mi->MI_VPE_EnablePort(0, 0))) return fprintf(stderr, "MI_VPE_EnablePort -> %#x\n", ret), -1;

    i6_vpe_port sink = out;
    if ((ret = mi->MI_VPE_SetPortMode(0, 1, &sink))) return fprintf(stderr, "MI_VPE_SetPortMode sink -> %#x\n", ret), -1;
    if ((ret = mi->MI_VPE_EnablePort(0, 1))) return fprintf(stderr, "MI_VPE_EnablePort sink -> %#x\n", ret), -1;

    MI_VENC_ChnAttr_t venc = {0};
    venc.stVeAttr.eType = MI_VENC_CODEC_H264;
    MI_VENC_AttrH26x_t *ve = &venc.stVeAttr.stAttrH264e;
    MI_VENC_AttrH26xCbr_t *rc = &venc.stRcAttr.stAttrH264Cbr;
    if (cfg->venc_dump) {
        venc.stVeAttr.eType = MI_VENC_CODEC_H265;
        venc.stRcAttr.eRcMode = MI_VENC_RATEMODE_H265CBR;
        ve = &venc.stVeAttr.stAttrH265e;
        rc = &venc.stRcAttr.stAttrH265Cbr;
    } else {
        venc.stRcAttr.eRcMode = MI_VENC_RATEMODE_H264CBR;
    }
    ve->u32MaxPicWidth = cfg->width;
    ve->u32MaxPicHeight = cfg->height;
    ve->u32BufSize = cfg->width * cfg->height / 2;
    ve->u32Profile = 0;
    ve->bByFrame = MI_TRUE;
    ve->u32PicWidth = cfg->width;
    ve->u32PicHeight = cfg->height;
    ve->u32BFrameNum = 0;
    ve->u32RefNum = 1;
    rc->u32Gop = cfg->fps * 2;
    rc->u32StatTime = 1;
    rc->u32SrcFrmRateNum = cfg->fps;
    rc->u32SrcFrmRateDen = 1;
    rc->u32BitRate = cfg->bitrate_kbps * 1024;
    if ((ret = mi->MI_VENC_CreateChn(0, &venc))) return fprintf(stderr, "MI_VENC_CreateChn -> %#x\n", ret), -1;
    if ((ret = mi->MI_VENC_StartRecvPic(0))) return fprintf(stderr, "MI_VENC_StartRecvPic -> %#x\n", ret), -1;

    if ((ret = mi->MI_SYS_BindChnPort2(&src, &dst, cfg->fps, cfg->fps, I6_SYS_LINK_REALTIME, 0)))
        return fprintf(stderr, "MI_SYS_BindChnPort2 VIF->VPE -> %#x\n", ret), -1;

    MI_U32 venc_dev = 0;
    if ((ret = mi->MI_VENC_GetChnDevid(0, &venc_dev))) return fprintf(stderr, "MI_VENC_GetChnDevid -> %#x\n", ret), -1;
    printf("MI_VENC_GetChnDevid -> dev%u\n", venc_dev);
    MI_SYS_ChnPort_t vpe_sink = { E_MI_MODULE_ID_VPE, 0, 0, 1 };
    MI_SYS_ChnPort_t venc_dst = { E_MI_MODULE_ID_VENC, venc_dev, 0, 0 };
    if ((ret = mi->MI_SYS_BindChnPort2(&vpe_sink, &venc_dst, cfg->fps, cfg->fps, I6_SYS_LINK_FRAMEBASE, 0)))
        return fprintf(stderr, "MI_SYS_BindChnPort2 VPE->VENC -> %#x\n", ret), -1;

    if ((ret = load_sensor_config(mi, cfg))) return fprintf(stderr, "load sensor config -> %#x\n", ret), -1;
    if ((ret = apply_exposure(mi, cfg))) return fprintf(stderr, "apply exposure -> %#x\n", ret), -1;

    return 0;
}

static void destroy_pipeline(mi_camera_libs_t *mi, const raw_cfg_t *cfg)
{
    if (cfg->read_module == E_MI_MODULE_ID_VIF) {
        mi->MI_VIF_DisableChnPort(0, cfg->read_port);
        mi->MI_VIF_DisableDev(0);
        mi->MI_SNR_Disable(cfg->sensor);
        mi->MI_SYS_Exit();
        return;
    }

    MI_SYS_ChnPort_t src = { E_MI_MODULE_ID_VIF, 0, 0, 0 };
    MI_SYS_ChnPort_t dst = { E_MI_MODULE_ID_VPE, 0, 0, 0 };
    mi->MI_VPE_DisablePort(0, 0);
    MI_U32 venc_dev = 0;
    mi->MI_VENC_GetChnDevid(0, &venc_dev);
    MI_SYS_ChnPort_t vpe_sink = { E_MI_MODULE_ID_VPE, 0, 0, 1 };
    MI_SYS_ChnPort_t venc_dst = { E_MI_MODULE_ID_VENC, venc_dev, 0, 0 };
    mi->MI_SYS_UnBindChnPort(&vpe_sink, &venc_dst);
    mi->MI_VENC_StopRecvPic(0);
    mi->MI_VENC_DestroyChn(0);
    mi->MI_VPE_DisablePort(0, 1);
    mi->MI_SYS_UnBindChnPort(&src, &dst);
    mi->MI_VPE_StopChannel(0);
    mi->MI_VPE_DestroyChannel(0);
    mi->MI_VIF_DisableChnPort(0, 0);
    mi->MI_VIF_DisableDev(0);
    mi->MI_SNR_Disable(cfg->sensor);
    mi->MI_SYS_Exit();
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  -o <file|rtp://host:port> output file or VENC RTP target\n"
        "  -n <num>    frames to dump, 0 until Ctrl-C (default: 1)\n"
        "  -r, --resolution <WxH> override VPE output resolution\n"
        "  -f <fps>    override sensor/output fps\n"
        "  -x, --exposure <ms> override exposure time in milliseconds\n"
        "  --bitrate <kbps> VENC dump bitrate (default: 8192)\n"
        "  --sensor-config <file> load ISP/sensor config bin after pipeline setup\n"
        "  -s <id>     sensor id (default: 0)\n"
        "  -M <mod>    read module: vpe or vif (default: vpe)\n"
        "  -P <port>   read output port (default: 0)\n"
        "  --led-gpio <pin> turn LED on when capture starts (default: 6, -1 disables)\n"
        "  --led-active-high / --led-active-low set LED polarity (default: active-low)\n"
        "  --timestamp-roi write only 128x64 top-left NV12 crop containing the overlay timestamp\n"
        "  --led-on-after <frames> led-mark-dump warmup frames before blinking (default: 30)\n"
        "  --led-on-for <frames> led-mark-dump LED-on frames per cycle (default: 60)\n"
        "  --led-off-for <frames> led-mark-dump LED-off frames per cycle (default: 60)\n"
        "  --verbose   print per-frame debug logs\n"
        "  --no-set-depth do not call MI_SYS_SetChnOutputPortDepth; only read SDK default\n"
        "  -E          do not create pipeline, read existing VPE chn0 port0\n"
        "  -h          help\n"
        "Defaults are read from ${MAJESTIC_CONFIG:-/etc/majestic.yaml} video0.\n", prog);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    raw_cfg_t cfg = { .sensor = 0, .width = 1920, .height = 1080, .fps = 30, .frames = 1,
        .exposure_us = 1000, .bitrate_kbps = 8192,
        .led_on_after = 30, .led_on_for = 60, .led_off_for = 60, .mark_x = 96, .mark_y = 0, .mark_w = 32, .mark_h = 32,
        .read_module = E_MI_MODULE_ID_VPE, .user_depth = 1, .buf_depth = 3,
        .timeout_ms = 1000, .led_gpio = 6, .led_active_low = 1, .out_path = "/tmp/camera.nv12" };
    if (!strcmp(argv[0], "led-mark-dump")) {
        cfg.led_mark = 1;
        cfg.frames = 1200;
        cfg.crop = 1;
        cfg.crop_x = 0;
        cfg.crop_y = 0;
        cfg.crop_w = 128;
        cfg.crop_h = 64;
    }
    if (!strcmp(argv[0], "venc-dump")) {
        cfg.venc_dump = 1;
        cfg.led_mark = 1;
        cfg.out_path = "/tmp/camera.h265";
    }
    static const struct option long_opts[] = {
        { "resolution", required_argument, NULL, 'r' },
        { "exposure", required_argument, NULL, 'x' },
        { "sensor-config", required_argument, NULL, 1000 },
        { "bitrate", required_argument, NULL, 1010 },
        { "led-gpio", required_argument, NULL, 1001 },
        { "led-active-high", no_argument, NULL, 1002 },
        { "led-active-low", no_argument, NULL, 1003 },
        { "timestamp-roi", no_argument, NULL, 1004 },
        { "led-on-after", required_argument, NULL, 1005 },
        { "led-on-for", required_argument, NULL, 1006 },
        { "led-off-for", required_argument, NULL, 1007 },
        { "verbose", no_argument, NULL, 1008 },
        { "no-set-depth", no_argument, NULL, 1009 },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    load_majestic_raw_config(&cfg);
    int opt;
    while ((opt = getopt_long(argc, argv, "o:n:r:f:x:s:M:P:Eh", long_opts, NULL)) != -1) {
        switch (opt) {
        case 'o': cfg.out_path = optarg; break;
        case 'n': cfg.frames = strtoul(optarg, NULL, 0); break;
        case 'r':
            if (parse_resolution(optarg, &cfg.width, &cfg.height)) {
                fprintf(stderr, "bad resolution %s, expected WxH\n", optarg);
                return 1;
            }
            break;
        case 'f': cfg.fps = strtoul(optarg, NULL, 0); break;
        case 'x': {
            double exposure_ms = strtod(optarg, NULL);
            if (exposure_ms <= 0.0) { fprintf(stderr, "bad exposure %s\n", optarg); return 1; }
            cfg.exposure_us = (MI_U32)(exposure_ms * 1000.0 + 0.5);
            break;
        }
        case 1000: cfg.sensor_config = optarg; break;
        case 1001: cfg.led_gpio = strtol(optarg, NULL, 0); break;
        case 1002: cfg.led_active_low = 0; break;
        case 1003: cfg.led_active_low = 1; break;
        case 1004:
            cfg.crop = 1;
            cfg.crop_x = 0;
            cfg.crop_y = 0;
            cfg.crop_w = 128;
            cfg.crop_h = 64;
            break;
        case 1005: cfg.led_on_after = strtoul(optarg, NULL, 0); break;
        case 1006: cfg.led_on_for = strtoul(optarg, NULL, 0); break;
        case 1007: cfg.led_off_for = strtoul(optarg, NULL, 0); break;
        case 1008: cfg.verbose = 1; break;
        case 1009: cfg.no_set_depth = 1; break;
        case 1010: cfg.bitrate_kbps = strtoul(optarg, NULL, 0); break;
        case 's': cfg.sensor = strtoul(optarg, NULL, 0); break;
        case 'M':
            if (!strcmp(optarg, "vif")) cfg.read_module = E_MI_MODULE_ID_VIF;
            else if (!strcmp(optarg, "vpe")) cfg.read_module = E_MI_MODULE_ID_VPE;
            else { fprintf(stderr, "bad module %s\n", optarg); return 1; }
            break;
        case 'P': cfg.read_port = strtoul(optarg, NULL, 0); break;
        case 'E': cfg.existing = 1; break;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 1;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    led_prepare(&cfg);

    mi_camera_libs_t mi;
    i6_snr_plane plane = {0};
    if (load_libs(&mi)) return 1;
    if (!cfg.existing && create_pipeline(&mi, &cfg, &plane)) return 2;

    if (cfg.venc_dump) {
        int rc = dump_venc_stream(&mi, &cfg);
        if (!cfg.existing) destroy_pipeline(&mi, &cfg);
        close_libs(&mi);
        return rc ? 4 : 0;
    }

    FILE *fp = fopen(cfg.out_path, "wb");
    if (!fp) { fprintf(stderr, "open %s failed: %s\n", cfg.out_path, strerror(errno)); return 3; }

    async_writer_t writer;
    pthread_t writer_thread;
    memset(&writer, 0, sizeof(writer));
    size_t writer_cap = (size_t)cfg.width * cfg.height * 2;
    size_t sensor_cap = (size_t)plane.capt.width * plane.capt.height * 2;
    if (sensor_cap > writer_cap) writer_cap = sensor_cap;
    if (cfg.crop) writer_cap = (size_t)cfg.crop_w * cfg.crop_h * 3 / 2;
    if (async_writer_init(&writer, fp, writer_cap)) {
        fprintf(stderr, "writer alloc failed: %s\n", strerror(errno));
        fclose(fp);
        return 3;
    }
    if (pthread_create(&writer_thread, NULL, async_writer_main, &writer)) {
        fprintf(stderr, "writer thread failed: %s\n", strerror(errno));
        async_writer_destroy(&writer);
        fclose(fp);
        return 3;
    }

    MI_SYS_ChnPort_t port = { cfg.read_module, 0, 0, cfg.read_port };
    MI_S32 ret = 0;
    print_output_depth(&mi, "read port before set", &port);
    if (!cfg.no_set_depth) {
        ret = mi.MI_SYS_SetChnOutputPortDepth(&port, cfg.user_depth, cfg.buf_depth);
        printf("MI_SYS_SetChnOutputPortDepth read port -> %#x\n", ret);
        print_output_depth(&mi, "read port after set", &port);
        if (ret) {
            fprintf(stderr, "output port is not available; aborting before GetBuf\n");
            async_writer_stop(&writer, writer_thread);
            fclose(fp);
            async_writer_destroy(&writer);
            close_libs(&mi);
            return 2;
        }
    }
    printf("dumping %s chn0 port0 %ux%u to %s\n",
        cfg.read_module == E_MI_MODULE_ID_VIF ? "VIF" : "VPE",
        cfg.width, cfg.height, cfg.out_path);
    if (cfg.crop)
        printf("output crop: %u,%u %ux%u NV12\n", cfg.crop_x, cfg.crop_y, cfg.crop_w, cfg.crop_h);
    if (*plane.sensName) printf("sensor=%s capture=%ux%u\n", plane.sensName, plane.capt.width, plane.capt.height);
    if (!cfg.led_mark)
        led_switch(&cfg, 1, "capture-start");

    unsigned captured = 0;
    unsigned dropped = 0;
    unsigned errors = 0;
    unsigned copy_samples = 0;
    unsigned long long copy_total_us = 0;
    unsigned long long copy_min_us = ~0ULL;
    unsigned long long copy_max_us = 0;
    unsigned long long first_getbuf_us = monotonic_us();
    while (!g_stop && (!cfg.frames || captured < cfg.frames)) {
        MI_SYS_BufInfo_t info = {0};
        MI_SYS_BUF_HANDLE handle = 0;
        if (cfg.led_mark) {
            int led_should_be_on = 0;
            if (captured >= cfg.led_on_after && cfg.led_on_for) {
                MI_U32 cycle = cfg.led_on_for + cfg.led_off_for;
                MI_U32 phase = cycle ? (captured - cfg.led_on_after) % cycle : 0;
                led_should_be_on = !cfg.led_off_for || phase < cfg.led_on_for;
            }
            led_switch(&cfg, led_should_be_on, led_should_be_on ? "mark-cycle-on" : "mark-cycle-off");
        }
        ret = mi.MI_SYS_ChnOutputPortGetBuf(&port, &info, &handle, cfg.timeout_ms);
        if (ret) {
            if (cfg.verbose)
                fprintf(stderr, "MI_SYS_ChnOutputPortGetBuf -> %#x\n", ret);
            if (!cfg.existing && !captured && errors == 0)
                probe_venc(&mi);
            if (++errors >= 2000) break;
            usleep(1000);
            continue;
        }
        errors = 0;
        unsigned long long got_frame_us = monotonic_us();
        if (!captured && cfg.verbose) {
            unsigned long long wait_us = got_frame_us - first_getbuf_us;
            printf("first getbuf wait: %llu.%03llu ms\n",
                wait_us / 1000ULL, wait_us % 1000ULL);
        }
        if (cfg.verbose && info.u64Pts && got_frame_us > info.u64Pts) {
            unsigned long long pts_to_user_us = got_frame_us - info.u64Pts;
            printf("pts-to-user latency: %llu.%03llu ms now=%llu pts=%llu\n",
                pts_to_user_us / 1000ULL, pts_to_user_us % 1000ULL,
                got_frame_us, (unsigned long long)info.u64Pts);
        }
        if (cfg.verbose)
            printf("frame %u: type=%d %ux%u fmt=%d stride=%u/%u raw=%u/%u seq=%u pts=%llu\n", captured,
                info.eBufType,
                info.stFrameData.u16Width, info.stFrameData.u16Height, info.stFrameData.ePixelFormat,
                info.stFrameData.u32Stride[0], info.stFrameData.u32Stride[1],
                info.stRawData.u32ContentSize, info.stRawData.u32BufSize, info.u32SequenceNumber,
                (unsigned long long)info.u64Pts);
        if (info.stFrameData.ePixelFormat == E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420 ||
            info.stFrameData.ePixelFormat == E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420_NV21) {
            overlay_timestamp(&info.stFrameData, pc_time_us());
            if (cfg.led_mark && cfg.led_is_on)
                mark_led_on_frame(&cfg, &info.stFrameData);
        }
        int drop = 0;
        unsigned long long copy_start_us = monotonic_us();
        if (async_writer_submit_or_drop(&writer, &cfg, &info, &drop))
            fprintf(stderr, "dump frame failed\n");
        unsigned long long copy_us = monotonic_us() - copy_start_us;
        if (!drop) {
            copy_samples++;
            copy_total_us += copy_us;
            if (copy_us < copy_min_us) copy_min_us = copy_us;
            if (copy_us > copy_max_us) copy_max_us = copy_us;
            if (cfg.verbose)
                printf("copy-to-writer latency: %llu.%03llu ms\n", copy_us / 1000ULL, copy_us % 1000ULL);
        }
        if (drop) {
            dropped++;
            if (cfg.verbose && (dropped % 30) == 1)
                printf("drop frame %u while writer busy\n", captured);
        }
        mi.MI_SYS_ChnOutputPortPutBuf(handle);
        captured++;
    }

    async_writer_stop(&writer, writer_thread);
    led_switch(&cfg, 0, "capture-end");
    fflush(fp);
    fclose(fp);
    if (!cfg.existing) destroy_pipeline(&mi, &cfg);
    close_libs(&mi);
    if (copy_samples)
        printf("copy-to-writer stats: n=%u min=%llu.%03llu avg=%llu.%03llu max=%llu.%03llu ms\n",
            copy_samples,
            copy_min_us / 1000ULL, copy_min_us % 1000ULL,
            (copy_total_us / copy_samples) / 1000ULL, (copy_total_us / copy_samples) % 1000ULL,
            copy_max_us / 1000ULL, copy_max_us % 1000ULL);
    printf("captured %u frame(s), wrote %u, dropped %u\n", captured, writer.written, dropped);
    async_writer_destroy(&writer);
    return captured ? 0 : 4;
}
