#include "mi_abi.h"

#include <dlfcn.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef enum {
    POC_CODEC_H264,
    POC_CODEC_H265,
} poc_codec_t;

typedef enum {
    POC_RC_CBR,
    POC_RC_VBR,
} poc_rc_mode_t;

typedef struct {
    poc_codec_t codec;
    poc_rc_mode_t rc_mode;
    unsigned width;
    unsigned height;
    unsigned fps;
    unsigned bitrate_kbps;
    double gop_size;
} poc_video_config_t;

static int load_sym(void *handle, const char *name, void **sym)
{
    *sym = dlsym(handle, name);
    if (!*sym) {
        fprintf(stderr, "missing symbol %s: %s\n", name, dlerror());
        return -1;
    }
    return 0;
}

static int load_libs(mi_libs_t *mi)
{
    memset(mi, 0, sizeof(*mi));
    dlopen("libcam_os_wrapper.so", RTLD_LAZY | RTLD_GLOBAL);
    mi->sys = dlopen("libmi_sys.so", RTLD_LAZY | RTLD_GLOBAL);
    mi->venc = dlopen("libmi_venc.so", RTLD_LAZY | RTLD_GLOBAL);
    if (!mi->sys || !mi->venc) {
        fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return -1;
    }

#define LOAD(h, f) do { if (load_sym((h), #f, (void **)&mi->f)) return -1; } while (0)
    LOAD(mi->sys, MI_SYS_Init);
    LOAD(mi->sys, MI_SYS_Exit);
    LOAD(mi->venc, MI_VENC_CreateChn);
    LOAD(mi->venc, MI_VENC_DestroyChn);
    LOAD(mi->venc, MI_VENC_StartRecvPic);
    LOAD(mi->venc, MI_VENC_StopRecvPic);
    LOAD(mi->venc, MI_VENC_Query);
    LOAD(mi->venc, MI_VENC_GetFd);
#undef LOAD
    return 0;
}

static char *trim(char *s)
{
    while (isspace((unsigned char)*s))
        s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1]))
        *--end = '\0';
    return s;
}

static int parse_size(const char *s, unsigned *width, unsigned *height)
{
    char *end;
    unsigned w = (unsigned)strtoul(s, &end, 10);
    if (*end != 'x' && *end != 'X')
        return -1;
    unsigned h = (unsigned)strtoul(end + 1, &end, 10);
    if (*end != '\0' || !w || !h)
        return -1;
    *width = w;
    *height = h;
    return 0;
}

static void load_majestic_video0_config(poc_video_config_t *cfg)
{
    const char *path = getenv("MAJESTIC_CONFIG");
    FILE *fp;
    char line[256];
    int in_video0 = 0;

    if (!path || !*path)
        path = "/etc/majestic.yaml";
    fp = fopen(path, "r");
    if (!fp)
        return;

    while (fgets(line, sizeof(line), fp)) {
        char *s = trim(line);
        char *colon;
        char *value;
        if (*s == '#' || *s == '\0')
            continue;
        if (strcmp(s, "video0:") == 0) {
            in_video0 = 1;
            continue;
        }
        if (in_video0 && s == line && strchr(s, ':'))
            break;
        if (!in_video0)
            continue;

        colon = strchr(s, ':');
        if (!colon)
            continue;
        *colon = '\0';
        value = trim(colon + 1);
        if (strcmp(s, "codec") == 0) {
            if (strcasecmp(value, "h265") == 0)
                cfg->codec = POC_CODEC_H265;
            else if (strcasecmp(value, "h264") == 0)
                cfg->codec = POC_CODEC_H264;
        } else if (strcmp(s, "size") == 0) {
            parse_size(value, &cfg->width, &cfg->height);
        } else if (strcmp(s, "fps") == 0) {
            cfg->fps = (unsigned)strtoul(value, NULL, 0);
        } else if (strcmp(s, "bitrate") == 0) {
            cfg->bitrate_kbps = (unsigned)strtoul(value, NULL, 0);
        } else if (strcmp(s, "rcMode") == 0) {
            cfg->rc_mode = strcasecmp(value, "cbr") == 0 ? POC_RC_CBR : POC_RC_VBR;
        } else if (strcmp(s, "gopSize") == 0) {
            cfg->gop_size = strtod(value, NULL);
        }
    }
    fclose(fp);
}

static unsigned gop_frames(const poc_video_config_t *cfg)
{
    double gop = cfg->fps * cfg->gop_size;
    if (gop < 1.0)
        gop = 1.0;
    return (unsigned)(gop + 0.5);
}

static void fill_venc_attr(MI_VENC_ChnAttr_t *attr, const poc_video_config_t *cfg)
{
    memset(attr, 0, sizeof(*attr));
    MI_VENC_AttrH26x_t *ve = cfg->codec == POC_CODEC_H265 ?
        &attr->stVeAttr.stAttrH265e : &attr->stVeAttr.stAttrH264e;
    ve->u32MaxPicWidth = cfg->width;
    ve->u32MaxPicHeight = cfg->height;
    ve->u32BufSize = cfg->width * cfg->height / 2;
    ve->u32Profile = 0;
    ve->bByFrame = MI_TRUE;
    ve->u32PicWidth = cfg->width;
    ve->u32PicHeight = cfg->height;
    ve->u32BFrameNum = 0;
    ve->u32RefNum = 1;

    attr->stVeAttr.eType = cfg->codec == POC_CODEC_H265 ? MI_VENC_CODEC_H265 : MI_VENC_CODEC_H264;
    if (cfg->rc_mode == POC_RC_CBR) {
        MI_VENC_AttrH26xCbr_t *rc = cfg->codec == POC_CODEC_H265 ?
            &attr->stRcAttr.stAttrH265Cbr : &attr->stRcAttr.stAttrH264Cbr;
        attr->stRcAttr.eRcMode = cfg->codec == POC_CODEC_H265 ? MI_VENC_RATEMODE_H265CBR : MI_VENC_RATEMODE_H264CBR;
        rc->u32Gop = gop_frames(cfg);
        rc->u32StatTime = 1;
        rc->u32SrcFrmRateNum = cfg->fps;
        rc->u32SrcFrmRateDen = 1;
        rc->u32BitRate = cfg->bitrate_kbps * 1024;
        rc->u32FluctuateLevel = 0;
    } else {
        MI_VENC_AttrH26xVbr_t *rc = cfg->codec == POC_CODEC_H265 ?
            &attr->stRcAttr.stAttrH265Vbr : &attr->stRcAttr.stAttrH264Vbr;
        attr->stRcAttr.eRcMode = cfg->codec == POC_CODEC_H265 ? MI_VENC_RATEMODE_H265VBR : MI_VENC_RATEMODE_H264VBR;
        rc->u32Gop = gop_frames(cfg);
        rc->u32StatTime = 1;
        rc->u32SrcFrmRateNum = cfg->fps;
        rc->u32SrcFrmRateDen = 1;
        rc->u32MaxBitRate = cfg->bitrate_kbps * 1024;
        rc->u32MaxQp = 48;
        rc->u32MinQp = 12;
    }
}

int main(int argc, char **argv)
{
    poc_video_config_t cfg = { POC_CODEC_H265, POC_RC_CBR, 1920, 1080, 90, 4096, 1.0 };
    mi_libs_t mi;
    MI_VENC_ChnAttr_t attr;
    MI_VENC_ChnStat_t stat;
    int chn = 0;
    int ret;

    printf("sizeof(MI_VENC_ChnAttr_t)=%zu\n", sizeof(MI_VENC_ChnAttr_t));
    printf("sizeof(MI_VENC_Stream_t)=%zu\n", sizeof(MI_VENC_Stream_t));
    printf("sizeof(MI_SYS_BufInfo_t)=%zu\n", sizeof(MI_SYS_BufInfo_t));

    load_majestic_video0_config(&cfg);
    (void)argc;
    (void)argv;
    printf("video0: codec=%s %ux%u fps=%u bitrate=%ukbps rc=%s gop=%u\n",
           cfg.codec == POC_CODEC_H265 ? "h265" : "h264", cfg.width, cfg.height,
           cfg.fps, cfg.bitrate_kbps, cfg.rc_mode == POC_RC_CBR ? "cbr" : "vbr",
           gop_frames(&cfg));

    if (load_libs(&mi))
        return 1;

    ret = mi.MI_SYS_Init();
    printf("MI_SYS_Init -> %#x\n", ret);

    fill_venc_attr(&attr, &cfg);
    ret = mi.MI_VENC_CreateChn(chn, &attr);
    printf("MI_VENC_CreateChn -> %#x\n", ret);
    if (ret != MI_SUCCESS)
        return 2;

    ret = mi.MI_VENC_StartRecvPic(chn);
    printf("MI_VENC_StartRecvPic -> %#x\n", ret);

    memset(&stat, 0, sizeof(stat));
    ret = mi.MI_VENC_Query(chn, &stat);
    printf("MI_VENC_Query -> %#x packs=%u leftRecv=%u leftEnc=%u\n",
           ret, stat.u32CurPacks, stat.u32LeftRecvPics, stat.u32LeftEncPics);

    printf("MI_VENC_GetFd -> %d\n", mi.MI_VENC_GetFd(chn));

    ret = mi.MI_VENC_StopRecvPic(chn);
    printf("MI_VENC_StopRecvPic -> %#x\n", ret);
    ret = mi.MI_VENC_DestroyChn(chn);
    printf("MI_VENC_DestroyChn -> %#x\n", ret);
    mi.MI_SYS_Exit();
    return 0;
}
