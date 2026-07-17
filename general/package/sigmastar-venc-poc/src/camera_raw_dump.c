#define _DEFAULT_SOURCE

#include "mi_abi.h"

#include <ctype.h>
#include <dlfcn.h>
#include <errno.h>
#include <getopt.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static volatile sig_atomic_t g_stop;

typedef struct {
    MI_U32 sensor, width, height, fps, frames;
    MI_U32 exposure_us;
    MI_ModuleId_e read_module;
    MI_U32 user_depth, buf_depth;
    MI_S32 timeout_ms;
    int existing, mirror, flip;
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
        LS(mi->sys, MI_SYS_SetChnOutputPortDepth) ||
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

typedef struct {
    FILE *fp;
    pthread_mutex_t lock;
    pthread_cond_t cond;
    unsigned char *data;
    size_t size;
    size_t cap;
    unsigned seq;
    unsigned long long pts;
    unsigned written;
    int has_frame;
    int stop;
    int error;
} async_writer_t;

static size_t frame_payload_size(const MI_SYS_FrameData_t *f)
{
    switch (f->ePixelFormat) {
    case E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420:
    case E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420_NV21:
        return (size_t)f->u16Width * f->u16Height * 3 / 2;
    case E_MI_SYS_PIXEL_FRAME_YUV422_YUYV:
        return (size_t)f->u16Width * f->u16Height * 2;
    default:
        return 0;
    }
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

    switch (f->ePixelFormat) {
    case E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420:
    case E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420_NV21:
        return copy_plane_to_buffer(&dst, f, 0, f->u16Width, f->u16Height) ||
               copy_plane_to_buffer(&dst, f, 1, f->u16Width, f->u16Height / 2);
    case E_MI_SYS_PIXEL_FRAME_YUV422_YUYV:
        return copy_plane_to_buffer(&dst, f, 0, f->u16Width * 2, f->u16Height);
    default:
        fprintf(stderr, "unsupported pixel format %d\n", f->ePixelFormat);
        return -1;
    }
}

static void *async_writer_main(void *arg)
{
    async_writer_t *w = arg;

    for (;;) {
        pthread_mutex_lock(&w->lock);
        while (!w->has_frame && !w->stop)
            pthread_cond_wait(&w->cond, &w->lock);
        if (!w->has_frame && w->stop) {
            pthread_mutex_unlock(&w->lock);
            break;
        }
        size_t size = w->size;
        unsigned seq = w->seq;
        unsigned long long pts = w->pts;
        pthread_mutex_unlock(&w->lock);

        if (fwrite(w->data, 1, size, w->fp) != size) {
            fprintf(stderr, "writer: failed frame seq=%u pts=%llu: %s\n", seq, pts, strerror(errno));
            pthread_mutex_lock(&w->lock);
            w->error = 1;
            w->stop = 1;
            w->has_frame = 0;
            pthread_cond_broadcast(&w->cond);
            pthread_mutex_unlock(&w->lock);
            break;
        }

        pthread_mutex_lock(&w->lock);
        w->written++;
        w->has_frame = 0;
        pthread_cond_broadcast(&w->cond);
        pthread_mutex_unlock(&w->lock);
    }
    return NULL;
}

static int async_writer_init(async_writer_t *w, FILE *fp, size_t cap)
{
    memset(w, 0, sizeof(*w));
    w->fp = fp;
    w->cap = cap;
    w->data = malloc(cap);
    if (!w->data)
        return -1;
    pthread_mutex_init(&w->lock, NULL);
    pthread_cond_init(&w->cond, NULL);
    return 0;
}

static void async_writer_destroy(async_writer_t *w)
{
    pthread_mutex_destroy(&w->lock);
    pthread_cond_destroy(&w->cond);
    free(w->data);
}

static void async_writer_stop(async_writer_t *w, pthread_t thread)
{
    pthread_mutex_lock(&w->lock);
    w->stop = 1;
    pthread_cond_broadcast(&w->cond);
    pthread_mutex_unlock(&w->lock);
    pthread_join(thread, NULL);
}

static int async_writer_submit_or_drop(async_writer_t *w, const MI_SYS_BufInfo_t *info, int *dropped)
{
    const MI_SYS_FrameData_t *f = &info->stFrameData;
    size_t size = frame_payload_size(f);

    *dropped = 0;
    if (!size)
        return -1;

    pthread_mutex_lock(&w->lock);
    if (w->error) {
        pthread_mutex_unlock(&w->lock);
        return -1;
    }
    if (w->has_frame) {
        *dropped = 1;
        pthread_mutex_unlock(&w->lock);
        return 0;
    }
    if (size > w->cap) {
        fprintf(stderr, "frame payload %zu exceeds writer buffer %zu; actual frame is %ux%u stride=%u/%u\n",
            size, w->cap, f->u16Width, f->u16Height, f->u32Stride[0], f->u32Stride[1]);
        pthread_mutex_unlock(&w->lock);
        return -1;
    }
    if (copy_frame_to_buffer(w->data, info)) {
        pthread_mutex_unlock(&w->lock);
        return -1;
    }
    w->size = size;
    w->seq = info->u32SequenceNumber;
    w->pts = (unsigned long long)info->u64Pts;
    w->has_frame = 1;
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
    i6_snr_res res;
    i6_snr_pad pad;

    if ((ret = mi->MI_SYS_Init())) { fprintf(stderr, "MI_SYS_Init -> %#x\n", ret); return -1; }
    if ((ret = mi->MI_SNR_SetPlaneMode(cfg->sensor, 0))) return fprintf(stderr, "MI_SNR_SetPlaneMode -> %#x\n", ret), -1;
    if ((ret = mi->MI_SNR_QueryResCount(cfg->sensor, &count))) return fprintf(stderr, "MI_SNR_QueryResCount -> %#x\n", ret), -1;
    for (MI_U8 i = 0; i < count; i++) {
        if ((ret = mi->MI_SNR_GetRes(cfg->sensor, i, &res))) return fprintf(stderr, "MI_SNR_GetRes -> %#x\n", ret), -1;
        if (cfg->width <= res.crop.width && cfg->height <= res.crop.height && cfg->fps <= res.maxFps) { profile = i; break; }
    }
    if (profile == 0xff) return fprintf(stderr, "no sensor profile for %ux%u@%u\n", cfg->width, cfg->height, cfg->fps), -1;
    if ((ret = mi->MI_SNR_SetRes(cfg->sensor, profile))) return fprintf(stderr, "MI_SNR_SetRes -> %#x\n", ret), -1;
    if ((ret = mi->MI_SNR_SetFps(cfg->sensor, cfg->fps))) return fprintf(stderr, "MI_SNR_SetFps -> %#x\n", ret), -1;
    if ((ret = mi->MI_SNR_SetOrien(cfg->sensor, cfg->mirror, cfg->flip))) return fprintf(stderr, "MI_SNR_SetOrien -> %#x\n", ret), -1;
    if ((ret = mi->MI_SNR_GetPadInfo(cfg->sensor, &pad))) return fprintf(stderr, "MI_SNR_GetPadInfo -> %#x\n", ret), -1;
    if ((ret = mi->MI_SNR_GetPlaneInfo(cfg->sensor, 0, plane))) return fprintf(stderr, "MI_SNR_GetPlaneInfo -> %#x\n", ret), -1;
    if ((ret = mi->MI_SNR_Enable(cfg->sensor))) return fprintf(stderr, "MI_SNR_Enable -> %#x\n", ret), -1;

    i6_vif_dev vif_dev = {0};
    vif_dev.intf = pad.intf;
    vif_dev.work = vif_dev.intf == I6_INTF_BT656 ? I6_VIF_WORK_1MULTIPLEX : I6_VIF_WORK_RGB_REALTIME;
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
    if ((ret = mi->MI_VIF_SetChnPortAttr(0, 0, &vif_port))) return fprintf(stderr, "MI_VIF_SetChnPortAttr -> %#x\n", ret), -1;
    if ((ret = mi->MI_VIF_EnableChnPort(0, 0))) return fprintf(stderr, "MI_VIF_EnableChnPort -> %#x\n", ret), -1;

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
    ret = mi->MI_SYS_SetChnOutputPortDepth(&dst, cfg->user_depth, cfg->buf_depth);
    printf("MI_SYS_SetChnOutputPortDepth -> %#x\n", ret);
    if ((ret = mi->MI_VPE_EnablePort(0, 0))) return fprintf(stderr, "MI_VPE_EnablePort -> %#x\n", ret), -1;

    i6_vpe_port sink = out;
    if ((ret = mi->MI_VPE_SetPortMode(0, 1, &sink))) return fprintf(stderr, "MI_VPE_SetPortMode sink -> %#x\n", ret), -1;
    if ((ret = mi->MI_VPE_EnablePort(0, 1))) return fprintf(stderr, "MI_VPE_EnablePort sink -> %#x\n", ret), -1;

    MI_VENC_ChnAttr_t venc = {0};
    venc.stVeAttr.eType = MI_VENC_CODEC_H264;
    venc.stVeAttr.stAttrH264e.u32MaxPicWidth = cfg->width;
    venc.stVeAttr.stAttrH264e.u32MaxPicHeight = cfg->height;
    venc.stVeAttr.stAttrH264e.u32BufSize = cfg->width * cfg->height / 2;
    venc.stVeAttr.stAttrH264e.u32Profile = 0;
    venc.stVeAttr.stAttrH264e.bByFrame = MI_TRUE;
    venc.stVeAttr.stAttrH264e.u32PicWidth = cfg->width;
    venc.stVeAttr.stAttrH264e.u32PicHeight = cfg->height;
    venc.stVeAttr.stAttrH264e.u32BFrameNum = 0;
    venc.stVeAttr.stAttrH264e.u32RefNum = 1;
    venc.stRcAttr.eRcMode = MI_VENC_RATEMODE_H264CBR;
    venc.stRcAttr.stAttrH264Cbr.u32Gop = cfg->fps * 2;
    venc.stRcAttr.stAttrH264Cbr.u32StatTime = 1;
    venc.stRcAttr.stAttrH264Cbr.u32SrcFrmRateNum = cfg->fps;
    venc.stRcAttr.stAttrH264Cbr.u32SrcFrmRateDen = 1;
    venc.stRcAttr.stAttrH264Cbr.u32BitRate = 2048 * 1024;
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

static void destroy_pipeline(mi_camera_libs_t *mi, MI_U32 sensor)
{
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
    mi->MI_SNR_Disable(sensor);
    mi->MI_SYS_Exit();
}

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s [options]\n"
        "  -o <file>   output raw file (default: /tmp/camera.nv12)\n"
        "  -n <num>    frames to dump, 0 until Ctrl-C (default: 1)\n"
        "  -r, --resolution <WxH> override VPE output resolution\n"
        "  -f <fps>    override sensor/output fps\n"
        "  --sensor-config <file> load ISP/sensor config bin after pipeline setup\n"
        "  -s <id>     sensor id (default: 0)\n"
        "  -M <mod>    read module: vpe or vif (default: vpe)\n"
        "  -E          do not create pipeline, read existing VPE chn0 port0\n"
        "  -h          help\n"
        "Defaults are read from ${MAJESTIC_CONFIG:-/etc/majestic.yaml} video0.\n", prog);
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    raw_cfg_t cfg = { .sensor = 0, .width = 1920, .height = 1080, .fps = 30, .frames = 1,
        .exposure_us = 1000,
        .read_module = E_MI_MODULE_ID_VPE, .user_depth = 2, .buf_depth = 4,
        .timeout_ms = 1000, .out_path = "/tmp/camera.nv12" };
    static const struct option long_opts[] = {
        { "resolution", required_argument, NULL, 'r' },
        { "sensor-config", required_argument, NULL, 1000 },
        { "help", no_argument, NULL, 'h' },
        { NULL, 0, NULL, 0 }
    };

    load_majestic_raw_config(&cfg);
    int opt;
    while ((opt = getopt_long(argc, argv, "o:n:r:f:s:M:Eh", long_opts, NULL)) != -1) {
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
        case 1000: cfg.sensor_config = optarg; break;
        case 's': cfg.sensor = strtoul(optarg, NULL, 0); break;
        case 'M':
            if (!strcmp(optarg, "vif")) cfg.read_module = E_MI_MODULE_ID_VIF;
            else if (!strcmp(optarg, "vpe")) cfg.read_module = E_MI_MODULE_ID_VPE;
            else { fprintf(stderr, "bad module %s\n", optarg); return 1; }
            break;
        case 'E': cfg.existing = 1; break;
        case 'h': usage(argv[0]); return 0;
        default: usage(argv[0]); return 1;
        }
    }

    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);

    mi_camera_libs_t mi;
    i6_snr_plane plane = {0};
    if (load_libs(&mi)) return 1;
    if (!cfg.existing && create_pipeline(&mi, &cfg, &plane)) return 2;

    FILE *fp = fopen(cfg.out_path, "wb");
    if (!fp) { fprintf(stderr, "open %s failed: %s\n", cfg.out_path, strerror(errno)); return 3; }

    async_writer_t writer;
    pthread_t writer_thread;
    memset(&writer, 0, sizeof(writer));
    size_t writer_cap = (size_t)cfg.width * cfg.height * 2;
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

    MI_SYS_ChnPort_t port = { cfg.read_module, 0, 0, 0 };
    MI_S32 ret = 0;
    if (cfg.existing) {
        ret = mi.MI_SYS_SetChnOutputPortDepth(&port, cfg.user_depth, cfg.buf_depth);
        printf("MI_SYS_SetChnOutputPortDepth -> %#x\n", ret);
        if (ret) {
            fprintf(stderr, "existing output port is not available; aborting before GetBuf\n");
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
    if (*plane.sensName) printf("sensor=%s capture=%ux%u\n", plane.sensName, plane.capt.width, plane.capt.height);

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
        ret = mi.MI_SYS_ChnOutputPortGetBuf(&port, &info, &handle, cfg.timeout_ms);
        if (ret) {
            fprintf(stderr, "MI_SYS_ChnOutputPortGetBuf -> %#x\n", ret);
            if (!cfg.existing && !captured && errors == 0)
                probe_venc(&mi);
            if (++errors >= 2000) break;
            usleep(1000);
            continue;
        }
        errors = 0;
        unsigned long long got_frame_us = monotonic_us();
        if (!captured) {
            unsigned long long wait_us = got_frame_us - first_getbuf_us;
            printf("first getbuf wait: %llu.%03llu ms\n",
                wait_us / 1000ULL, wait_us % 1000ULL);
        }
        if (info.u64Pts && got_frame_us > info.u64Pts) {
            unsigned long long pts_to_user_us = got_frame_us - info.u64Pts;
            printf("pts-to-user latency: %llu.%03llu ms now=%llu pts=%llu\n",
                pts_to_user_us / 1000ULL, pts_to_user_us % 1000ULL,
                got_frame_us, (unsigned long long)info.u64Pts);
        }
        printf("frame %u: %ux%u fmt=%d stride=%u/%u seq=%u pts=%llu\n", captured,
            info.stFrameData.u16Width, info.stFrameData.u16Height, info.stFrameData.ePixelFormat,
            info.stFrameData.u32Stride[0], info.stFrameData.u32Stride[1], info.u32SequenceNumber,
            (unsigned long long)info.u64Pts);
        if (info.stFrameData.ePixelFormat == E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420 ||
            info.stFrameData.ePixelFormat == E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420_NV21)
            overlay_timestamp(&info.stFrameData, got_frame_us);
        int drop = 0;
        unsigned long long copy_start_us = monotonic_us();
        if (async_writer_submit_or_drop(&writer, &info, &drop))
            fprintf(stderr, "dump frame failed\n");
        unsigned long long copy_us = monotonic_us() - copy_start_us;
        if (!drop) {
            copy_samples++;
            copy_total_us += copy_us;
            if (copy_us < copy_min_us) copy_min_us = copy_us;
            if (copy_us > copy_max_us) copy_max_us = copy_us;
            printf("copy-to-writer latency: %llu.%03llu ms\n", copy_us / 1000ULL, copy_us % 1000ULL);
        }
        if (drop) {
            dropped++;
            if ((dropped % 30) == 1)
                printf("drop frame %u while writer busy\n", captured);
        }
        mi.MI_SYS_ChnOutputPortPutBuf(handle);
        captured++;
    }

    async_writer_stop(&writer, writer_thread);
    fflush(fp);
    fclose(fp);
    if (!cfg.existing) destroy_pipeline(&mi, cfg.sensor);
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
