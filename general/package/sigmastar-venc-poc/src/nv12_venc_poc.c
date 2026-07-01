#define _DEFAULT_SOURCE

#include "mi_abi.h"

#include <dlfcn.h>
#include <errno.h>
#include <ctype.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#define RTP_PAYLOAD_TYPE 97
#define RTP_MAX_PAYLOAD 1200
#define MAX_VENC_PACKS 32

static volatile sig_atomic_t g_stop;

static uint32_t g_drop_slice_prob_threshold;
static uint32_t g_rand_state = 0x12345678U;

static const unsigned char *find_start_code(const unsigned char *p, const unsigned char *end,
                                            size_t *prefix);

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

typedef enum {
    OUTPUT_FILE,
    OUTPUT_RTP,
} output_type_t;

typedef struct {
    output_type_t type;
    FILE *fp;
    int fd;
    struct sockaddr_storage addr;
    socklen_t addrlen;
    uint16_t seq;
    uint32_t ssrc;
} output_t;

typedef struct {
    unsigned char *y;
    unsigned char *uv;
    size_t y_size;
    size_t uv_size;
} noise_pattern_t;

#define FRAME_TIME_QUEUE_SIZE 512

typedef struct {
    MI_U64 times[FRAME_TIME_QUEUE_SIZE];
    unsigned head;
    unsigned tail;
    unsigned count;
} frame_time_queue_t;

static int load_sym(void *handle, const char *name, void **sym)
{
    *sym = dlsym(handle, name);
    if (!*sym) {
        fprintf(stderr, "missing symbol %s: %s\n", name, dlerror());
        return -1;
    }
    return 0;
}

static void on_signal(int sig)
{
    (void)sig;
    g_stop = 1;
}

static void frame_time_push(frame_time_queue_t *q, MI_U64 t)
{
    if (q->count == FRAME_TIME_QUEUE_SIZE) {
        q->head = (q->head + 1) % FRAME_TIME_QUEUE_SIZE;
        q->count--;
    }
    q->times[q->tail] = t;
    q->tail = (q->tail + 1) % FRAME_TIME_QUEUE_SIZE;
    q->count++;
}

static int frame_time_pop(frame_time_queue_t *q, MI_U64 *t)
{
    if (!q->count)
        return -1;
    *t = q->times[q->head];
    q->head = (q->head + 1) % FRAME_TIME_QUEUE_SIZE;
    q->count--;
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
    LOAD(mi->sys, MI_SYS_ChnInputPortGetBuf);
    LOAD(mi->sys, MI_SYS_ChnInputPortPutBuf);
    LOAD(mi->venc, MI_VENC_CreateChn);
    LOAD(mi->venc, MI_VENC_DestroyChn);
    LOAD(mi->venc, MI_VENC_StartRecvPic);
    LOAD(mi->venc, MI_VENC_StopRecvPic);
    LOAD(mi->venc, MI_VENC_Query);
    LOAD(mi->venc, MI_VENC_GetStream);
    LOAD(mi->venc, MI_VENC_ReleaseStream);
    LOAD(mi->venc, MI_VENC_SetH264SliceSplit);
    LOAD(mi->venc, MI_VENC_GetH264SliceSplit);
    LOAD(mi->venc, MI_VENC_SetH265SliceSplit);
    LOAD(mi->venc, MI_VENC_GetH265SliceSplit);
#undef LOAD
    return 0;
}

static MI_U64 now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (MI_U64)tv.tv_sec * 1000000ULL + (MI_U64)tv.tv_usec;
}

static int parse_u32_arg(const char *name, const char *value, unsigned *out)
{
    char *end;
    unsigned long parsed;

    parsed = strtoul(value, &end, 0);
    if (*end != '\0' || parsed > 0xffffffffUL) {
        fprintf(stderr, "invalid %s=%s\n", name, value);
        return -1;
    }
    *out = (unsigned)parsed;
    return 0;
}

static int parse_double_arg(const char *name, const char *value, double *out)
{
    char *end;
    double parsed;

    parsed = strtod(value, &end);
    if (*end != '\0') {
        fprintf(stderr, "invalid %s=%s\n", name, value);
        return -1;
    }
    *out = parsed;
    return 0;
}

static uint32_t prng_u32(void)
{
    g_rand_state ^= g_rand_state << 13;
    g_rand_state ^= g_rand_state >> 17;
    g_rand_state ^= g_rand_state << 5;
    return g_rand_state;
}

static int parse_prob_threshold_arg(const char *name, const char *value, uint32_t *out)
{
    char *end;
    double prob;

    prob = strtod(value, &end);
    if (*end != '\0' || prob < 0.0 || prob > 1.0) {
        fprintf(stderr, "invalid %s=%s\n", name, value);
        return -1;
    }
    if (prob <= 0.0)
        *out = 0;
    else if (prob >= 1.0)
        *out = UINT32_MAX;
    else
        *out = (uint32_t)(prob * (double)UINT32_MAX);
    return 0;
}

static void usage(const char *prog)
{
    fprintf(stderr, "usage: %s [options] <out.stream|udp://host:port|rtp://host:port>\n", prog);
    fprintf(stderr, "video defaults are read from ${MAJESTIC_CONFIG:-/etc/majestic.yaml} video0\n");
    fprintf(stderr, "options:\n");
    fprintf(stderr, "  -g, --gop-size <seconds>       override gopSize, 0 means GOP1\n");
    fprintf(stderr, "  -s, --slice-rows <rows>        enable slice split, rows per slice\n");
    fprintf(stderr, "  -p, --drop-slice-prob <0..1>   per-slice drop probability\n");
    fprintf(stderr, "  -b, --bitrate <kbit/s>         override video bitrate\n");
}

static const char *option_value(int argc, char **argv, int *i, const char *arg)
{
    const char *eq = strchr(arg, '=');

    if (eq)
        return eq + 1;
    if (*i + 1 >= argc) {
        fprintf(stderr, "missing value for %s\n", arg);
        return NULL;
    }
    return argv[++*i];
}

static int parse_options(int argc, char **argv, poc_video_config_t *cfg,
                         unsigned *slice_rows, const char **out_path)
{
    *out_path = NULL;

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        const char *value;

        if (strcmp(arg, "-h") == 0 || strcmp(arg, "--help") == 0)
            return 1;

        if (arg[0] != '-') {
            if (*out_path) {
                fprintf(stderr, "unexpected extra argument: %s\n", arg);
                return -1;
            }
            *out_path = arg;
            continue;
        }

        if (strcmp(arg, "-g") == 0 || strncmp(arg, "--gop-size", 10) == 0) {
            value = option_value(argc, argv, &i, arg);
            if (!value || parse_double_arg("gop-size", value, &cfg->gop_size))
                return -1;
        } else if (strcmp(arg, "-s") == 0 || strncmp(arg, "--slice-rows", 12) == 0) {
            value = option_value(argc, argv, &i, arg);
            if (!value || parse_u32_arg("slice-rows", value, slice_rows))
                return -1;
        } else if (strcmp(arg, "-p") == 0 || strncmp(arg, "--drop-slice-prob", 17) == 0) {
            value = option_value(argc, argv, &i, arg);
            if (!value || parse_prob_threshold_arg("drop-slice-prob", value, &g_drop_slice_prob_threshold))
                return -1;
        } else if (strcmp(arg, "-b") == 0 || strncmp(arg, "--bitrate", 9) == 0) {
            value = option_value(argc, argv, &i, arg);
            if (!value || parse_u32_arg("bitrate", value, &cfg->bitrate_kbps))
                return -1;
        } else {
            fprintf(stderr, "unknown option: %s\n", arg);
            return -1;
        }
    }

    if (!*out_path) {
        fprintf(stderr, "missing output path\n");
        return -1;
    }
    return 0;
}

static int is_h264_vcl_nal(const unsigned char *nal, size_t len)
{
    unsigned type;

    if (!len)
        return 0;
    type = nal[0] & 0x1f;
    return type >= 1 && type <= 5;
}

static int is_h265_vcl_nal(const unsigned char *nal, size_t len)
{
    unsigned type;

    if (len < 2)
        return 0;
    type = (nal[0] >> 1) & 0x3f;
    return type <= 31;
}

static int should_drop_slice(const poc_video_config_t *cfg, const unsigned char *nal,
                             size_t len, unsigned frame_index, unsigned slice_index,
                             int already_dropped)
{
    int is_vcl = cfg->codec == POC_CODEC_H265 ?
        is_h265_vcl_nal(nal, len) : is_h264_vcl_nal(nal, len);

    if (!is_vcl)
        return 0;
    if (g_drop_slice_prob_threshold && prng_u32() <= g_drop_slice_prob_threshold)
        return 1;
    (void)frame_index;
    (void)slice_index;
    (void)already_dropped;
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

static int parse_rtp_url(const char *url, char *host, size_t host_len, char *port, size_t port_len)
{
    const char *p;
    const char *colon;

    if (strncmp(url, "udp://", 6) == 0)
        p = url + 6;
    else if (strncmp(url, "rtp://", 6) == 0)
        p = url + 6;
    else
        return -1;

    colon = strrchr(p, ':');
    if (!colon || colon == p || !colon[1])
        return -1;
    if ((size_t)(colon - p) >= host_len || strlen(colon + 1) >= port_len)
        return -1;
    memcpy(host, p, colon - p);
    host[colon - p] = '\0';
    strcpy(port, colon + 1);
    return 0;
}

static int output_open(output_t *out, const char *path)
{
    char host[128];
    char port[16];

    memset(out, 0, sizeof(*out));
    out->fd = -1;
    out->seq = (uint16_t)(now_us() & 0xffff);
    out->ssrc = 0x53535650;

    if (parse_rtp_url(path, host, sizeof(host), port, sizeof(port)) == 0) {
        struct addrinfo hints;
        struct addrinfo *res = NULL;
        int ret;

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
        out->type = OUTPUT_RTP;
        printf("RTP output: %s:%s payload=%u ssrc=%#x\n", host, port, RTP_PAYLOAD_TYPE, out->ssrc);
        return 0;
    }

    out->fp = fopen(path, "wb");
    if (!out->fp) {
        fprintf(stderr, "open output %s: %s\n", path, strerror(errno));
        return -1;
    }
    out->type = OUTPUT_FILE;
    return 0;
}

static void output_close(output_t *out)
{
    if (out->fp)
        fclose(out->fp);
    if (out->fd >= 0)
        close(out->fd);
}

static int rtp_send(output_t *out, const unsigned char *payload, size_t len,
                    uint32_t timestamp, int marker)
{
    unsigned char pkt[12 + RTP_MAX_PAYLOAD];

    if (len > RTP_MAX_PAYLOAD)
        return -1;
    pkt[0] = 0x80;
    pkt[1] = RTP_PAYLOAD_TYPE | (marker ? 0x80 : 0);
    pkt[2] = out->seq >> 8;
    pkt[3] = out->seq & 0xff;
    pkt[4] = timestamp >> 24;
    pkt[5] = timestamp >> 16;
    pkt[6] = timestamp >> 8;
    pkt[7] = timestamp & 0xff;
    pkt[8] = out->ssrc >> 24;
    pkt[9] = out->ssrc >> 16;
    pkt[10] = out->ssrc >> 8;
    pkt[11] = out->ssrc & 0xff;
    memcpy(pkt + 12, payload, len);
    if (sendto(out->fd, pkt, len + 12, 0, (struct sockaddr *)&out->addr, out->addrlen) < 0) {
        fprintf(stderr, "sendto: %s\n", strerror(errno));
        return -1;
    }
    out->seq++;
    return 0;
}

static int rtp_send_h264_nal(output_t *out, const unsigned char *nal, size_t len,
                             uint32_t timestamp, int marker)
{
    if (len <= RTP_MAX_PAYLOAD)
        return rtp_send(out, nal, len, timestamp, marker);
    if (len < 2)
        return 0;

    unsigned char fu[2 + RTP_MAX_PAYLOAD];
    unsigned char nri = nal[0] & 0x60;
    unsigned char type = nal[0] & 0x1f;
    size_t off = 1;
    int start = 1;

    while (off < len) {
        size_t chunk = len - off;
        int end;
        if (chunk > RTP_MAX_PAYLOAD - 2)
            chunk = RTP_MAX_PAYLOAD - 2;
        end = off + chunk == len;
        fu[0] = nri | 28;
        fu[1] = type | (start ? 0x80 : 0) | (end ? 0x40 : 0);
        memcpy(fu + 2, nal + off, chunk);
        if (rtp_send(out, fu, chunk + 2, timestamp, marker && end))
            return -1;
        off += chunk;
        start = 0;
    }
    return 0;
}

static int rtp_send_h265_nal(output_t *out, const unsigned char *nal, size_t len,
                             uint32_t timestamp, int marker)
{
    if (len <= RTP_MAX_PAYLOAD)
        return rtp_send(out, nal, len, timestamp, marker);
    if (len < 3)
        return 0;

    unsigned char fu[3 + RTP_MAX_PAYLOAD];
    unsigned char type = (nal[0] >> 1) & 0x3f;
    size_t off = 2;
    int start = 1;

    while (off < len) {
        size_t chunk = len - off;
        int end;
        if (chunk > RTP_MAX_PAYLOAD - 3)
            chunk = RTP_MAX_PAYLOAD - 3;
        end = off + chunk == len;
        fu[0] = (49 << 1) | (nal[0] & 0x81);
        fu[1] = nal[1];
        fu[2] = type | (start ? 0x80 : 0) | (end ? 0x40 : 0);
        memcpy(fu + 3, nal + off, chunk);
        if (rtp_send(out, fu, chunk + 3, timestamp, marker && end))
            return -1;
        off += chunk;
        start = 0;
    }
    return 0;
}

static const unsigned char *find_start_code(const unsigned char *p, const unsigned char *end,
                                            size_t *prefix)
{
    while (p + 3 <= end) {
        if (p[0] == 0 && p[1] == 0 && p[2] == 1) {
            *prefix = 3;
            return p;
        }
        if (p + 4 <= end && p[0] == 0 && p[1] == 0 && p[2] == 0 && p[3] == 1) {
            *prefix = 4;
            return p;
        }
        p++;
    }
    return NULL;
}

static int output_write_frame(output_t *out, const poc_video_config_t *cfg,
                              const unsigned char *data, size_t len, unsigned frame_index)
{
    const unsigned char *end = data + len;
    const unsigned char *sc;
    size_t prefix;
    uint32_t timestamp = (uint32_t)((uint64_t)frame_index * 90000 / cfg->fps);
    unsigned slice_index = 0;

    if (out->type == OUTPUT_FILE && !g_drop_slice_prob_threshold) {
        fwrite(data, 1, len, out->fp);
        return ferror(out->fp) ? -1 : 0;
    }

    sc = find_start_code(data, end, &prefix);
    if (!sc) {
        return cfg->codec == POC_CODEC_H265 ?
            rtp_send_h265_nal(out, data, len, timestamp, 1) :
            rtp_send_h264_nal(out, data, len, timestamp, 1);
    }

    while (sc) {
        const unsigned char *nal = sc + prefix;
        const unsigned char *next = find_start_code(nal, end, &prefix);
        const unsigned char *nal_end = next ? next : end;
        while (nal_end > nal && nal_end[-1] == 0)
            nal_end--;
        if (nal_end > nal) {
            int is_vcl = cfg->codec == POC_CODEC_H265 ?
                is_h265_vcl_nal(nal, nal_end - nal) : is_h264_vcl_nal(nal, nal_end - nal);
            if (is_vcl && should_drop_slice(cfg, nal, nal_end - nal, frame_index, slice_index,
                                           0)) {
                if ((frame_index % 30) == 0)
                    printf("drop frame=%u slice=%u len=%zu%s\n", frame_index, slice_index,
                           nal_end - nal, g_drop_slice_prob_threshold ? " prob" : "");
                slice_index++;
                sc = next;
                continue;
            }
            int marker = next == NULL;
            if (out->type == OUTPUT_FILE) {
                static const unsigned char start_code[] = { 0, 0, 0, 1 };
                fwrite(start_code, 1, sizeof(start_code), out->fp);
                fwrite(nal, 1, nal_end - nal, out->fp);
                if (ferror(out->fp))
                    return -1;
            } else {
                int ret = cfg->codec == POC_CODEC_H265 ?
                    rtp_send_h265_nal(out, nal, nal_end - nal, timestamp, marker) :
                    rtp_send_h264_nal(out, nal, nal_end - nal, timestamp, marker);
                if (ret)
                    return ret;
            }
            if (is_vcl)
                slice_index++;
        }
        sc = next;
    }
    return 0;
}

static int append_buf(unsigned char **buf, size_t *len, size_t *cap,
                      const unsigned char *data, size_t data_len)
{
    if (*len + data_len > *cap) {
        size_t new_cap = *cap ? *cap * 2 : 65536;
        unsigned char *new_buf;
        while (new_cap < *len + data_len)
            new_cap *= 2;
        new_buf = realloc(*buf, new_cap);
        if (!new_buf)
            return -1;
        *buf = new_buf;
        *cap = new_cap;
    }
    memcpy(*buf + *len, data, data_len);
    *len += data_len;
    return 0;
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

static int configure_slice_split(mi_libs_t *mi, const poc_video_config_t *cfg, int chn,
                                 unsigned slice_rows)
{
    MI_VENC_ParamH26xSliceSplit_t split;
    MI_S32 ret;

    if (!slice_rows)
        return 0;
    memset(&split, 0, sizeof(split));

    if (cfg->codec == POC_CODEC_H265) {
        ret = mi->MI_VENC_GetH265SliceSplit(chn, &split);
        printf("MI_VENC_GetH265SliceSplit -> %#x enable=%d rows=%u\n",
               ret, split.bSplitEnable, split.u32SliceRowCount);
        if (ret != MI_SUCCESS)
            return ret;
        split.bSplitEnable = MI_TRUE;
        split.u32SliceRowCount = slice_rows;
        ret = mi->MI_VENC_SetH265SliceSplit(chn, &split);
        printf("MI_VENC_SetH265SliceSplit rows=%u -> %#x\n", slice_rows, ret);
        return ret;
    }

    ret = mi->MI_VENC_GetH264SliceSplit(chn, &split);
    printf("MI_VENC_GetH264SliceSplit -> %#x enable=%d rows=%u\n",
           ret, split.bSplitEnable, split.u32SliceRowCount);
    if (ret != MI_SUCCESS)
        return ret;
    split.bSplitEnable = MI_TRUE;
    split.u32SliceRowCount = slice_rows;
    ret = mi->MI_VENC_SetH264SliceSplit(chn, &split);
    printf("MI_VENC_SetH264SliceSplit rows=%u -> %#x\n", slice_rows, ret);
    return ret;
}

static int noise_pattern_init(noise_pattern_t *noise, unsigned width)
{
    uint32_t rnd = 0x12345678U;

    memset(noise, 0, sizeof(*noise));
    noise->y_size = width * 1024U;
    noise->uv_size = width * 512U;
    noise->y = malloc(noise->y_size);
    noise->uv = malloc(noise->uv_size);
    if (!noise->y || !noise->uv)
        return -1;

    for (size_t i = 0; i < noise->y_size; i++) {
        rnd ^= rnd << 13;
        rnd ^= rnd >> 17;
        rnd ^= rnd << 5;
        noise->y[i] = (unsigned char)(48 + ((i / width) & 0x3f) / 3 + (rnd & 0x5f));
    }

    for (size_t i = 0; i < noise->uv_size; i += 2) {
        rnd ^= rnd << 13;
        rnd ^= rnd >> 17;
        rnd ^= rnd << 5;
        unsigned block = ((i / width) + (i / 96)) & 3;
        noise->uv[i] = (unsigned char)(118 + block * 5 + (rnd & 0x03));
        if (i + 1 < noise->uv_size)
            noise->uv[i + 1] = (unsigned char)(134 - block * 4 + ((rnd >> 8) & 0x03));
    }
    return 0;
}

static void noise_pattern_free(noise_pattern_t *noise)
{
    free(noise->y);
    free(noise->uv);
}

static void generate_nv12_frame(MI_SYS_FrameData_t *frame_data, const noise_pattern_t *noise,
                                unsigned width, unsigned height, unsigned frame)
{
    unsigned char *y_plane = frame_data->pVirAddr[0];
    unsigned char *uv_plane = frame_data->pVirAddr[1];
    unsigned y_stride = frame_data->u32Stride[0] ? frame_data->u32Stride[0] : width;
    unsigned uv_stride = frame_data->u32Stride[1] ? frame_data->u32Stride[1] : width;
    unsigned box = width / 10;
    unsigned x0 = width > box ? (frame * 17) % (width - box) : 0;
    unsigned y0 = height > box ? (frame * 11) % (height - box) : 0;
    unsigned y_tex_rows = noise->y_size / width;
    unsigned uv_tex_rows = noise->uv_size / width;
    unsigned y_scroll = y_tex_rows ? (frame * 3) % y_tex_rows : 0;
    unsigned uv_scroll = uv_tex_rows ? (frame / 2) % uv_tex_rows : 0;

    for (unsigned y = 0; y < height; y++) {
        unsigned src_y = y_tex_rows ? (y + y_scroll) % y_tex_rows : 0;
        memcpy(y_plane + y * y_stride, noise->y + (size_t)src_y * width, width);
        if (y_stride > width)
            memset(y_plane + y * y_stride + width, 0, y_stride - width);
    }

    for (unsigned y = y0; y < y0 + box && y < height; y++) {
        for (unsigned x = x0; x < x0 + box && x < width; x++) {
            unsigned checker = (((x - x0) >> 4) ^ ((y - y0) >> 4)) & 1;
            y_plane[y * y_stride + x] = checker ? 220 : 96;
        }
    }

    for (unsigned y = 0; y < height / 2; y++) {
        unsigned src_y = uv_tex_rows ? (y + uv_scroll) % uv_tex_rows : 0;
        memcpy(uv_plane + y * uv_stride, noise->uv + (size_t)src_y * width, width);
        if (uv_stride > width)
            memset(uv_plane + y * uv_stride + width, 0, uv_stride - width);
    }
}

int main(int argc, char **argv)
{
    poc_video_config_t cfg = { POC_CODEC_H265, POC_RC_CBR, 1920, 1080, 90, 4096, 1.0 };
    unsigned slice_rows = 0;
    const char *out_path;
    int optret;

    load_majestic_video0_config(&cfg);
    optret = parse_options(argc, argv, &cfg, &slice_rows, &out_path);
    if (optret) {
        usage(argv[0]);
        return optret < 0 ? 1 : 0;
    }

    unsigned width = cfg.width;
    unsigned height = cfg.height;
    noise_pattern_t noise;
    output_t out;
    unsigned char *encoded = NULL;
    size_t encoded_len = 0;
    size_t encoded_cap = 0;
    mi_libs_t mi;
    MI_VENC_ChnAttr_t attr;
    frame_time_queue_t frame_times = { 0 };
    frame_time_queue_t encode_times = { 0 };
    int chn = 0;
    int ret;
    unsigned frame = 0;
    MI_U64 next_frame_us = now_us();
    MI_U64 last_report_us = next_frame_us;
    MI_U64 frame_interval_us = cfg.fps ? 1000000ULL / cfg.fps : 33333ULL;
    unsigned report_submitted = 0;
    unsigned report_output = 0;
    unsigned long long report_bytes = 0;
    unsigned report_generated = 0;
    unsigned long long report_generate_us = 0;
    unsigned report_generate_min_us = 0;
    unsigned report_generate_max_us = 0;
    unsigned report_frame_time = 0;
    unsigned long long report_frame_time_us = 0;
    unsigned report_frame_time_min_us = 0;
    unsigned report_frame_time_max_us = 0;
    unsigned report_encode_time = 0;
    unsigned long long report_encode_time_us = 0;
    unsigned report_encode_time_min_us = 0;
    unsigned report_encode_time_max_us = 0;

    if (noise_pattern_init(&noise, width)) {
        fprintf(stderr, "alloc failed: %s\n", strerror(errno));
        return 1;
    }
    if (output_open(&out, out_path))
        return 1;
    signal(SIGINT, on_signal);
    signal(SIGTERM, on_signal);
    if (load_libs(&mi))
        return 1;

    printf("abi sizes: chn=%zu stream=%zu bufinfo=%zu framedata=%zu\n",
           sizeof(MI_VENC_ChnAttr_t), sizeof(MI_VENC_Stream_t),
           sizeof(MI_SYS_BufInfo_t), sizeof(MI_SYS_FrameData_t));
    printf("video0: codec=%s %ux%u fps=%u bitrate=%ukbps rc=%s gop=%u\n",
           cfg.codec == POC_CODEC_H265 ? "h265" : "h264", cfg.width, cfg.height,
           cfg.fps, cfg.bitrate_kbps, cfg.rc_mode == POC_RC_CBR ? "cbr" : "vbr",
           gop_frames(&cfg));
    if (slice_rows)
        printf("slice split: %u macroblock rows per slice\n", slice_rows);
    if (g_drop_slice_prob_threshold)
        printf("drop slice probability: threshold=%u (~%.6f per VCL slice)\n",
               g_drop_slice_prob_threshold,
               (double)g_drop_slice_prob_threshold / (double)UINT32_MAX);

    ret = mi.MI_SYS_Init();
    printf("MI_SYS_Init -> %#x\n", ret);

    fill_venc_attr(&attr, &cfg);
    ret = mi.MI_VENC_CreateChn(chn, &attr);
    printf("MI_VENC_CreateChn -> %#x\n", ret);
    if (ret != MI_SUCCESS)
        return 2;

    ret = configure_slice_split(&mi, &cfg, chn, slice_rows);
    if (ret != MI_SUCCESS)
        return 3;

    ret = mi.MI_VENC_StartRecvPic(chn);
    printf("MI_VENC_StartRecvPic -> %#x\n", ret);
    if (ret != MI_SUCCESS)
        return 4;

    while (!g_stop) {
        MI_SYS_ChnPort_t port = { E_MI_MODULE_ID_VENC, 0, (MI_U32)chn, 0 };
        MI_U64 current_us = now_us();

        if (current_us >= next_frame_us) {
            MI_SYS_BufConf_t conf;
            MI_SYS_BufInfo_t info;
            MI_SYS_BUF_HANDLE handle = 0;
            MI_U64 generate_start_us;
            unsigned generate_us;

            next_frame_us += frame_interval_us;

            memset(&conf, 0, sizeof(conf));
            memset(&info, 0, sizeof(info));
            conf.eBufType = E_MI_SYS_BUFDATA_FRAME;
            conf.u32Flags = MI_SYS_MAP_VA;
            conf.u64TargetPts = current_us;
            conf.stFrameCfg.u16Width = width;
            conf.stFrameCfg.u16Height = height;
            conf.stFrameCfg.eFrameScanMode = E_MI_SYS_FRAME_SCAN_MODE_PROGRESSIVE;
            conf.stFrameCfg.eFormat = E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420;
            conf.stFrameCfg.eCompressMode = E_MI_SYS_COMPRESS_MODE_NONE;

            ret = mi.MI_SYS_ChnInputPortGetBuf(&port, &conf, &info, &handle, 0);
            if (ret == MI_SUCCESS) {
                if (!info.stFrameData.pVirAddr[0] || !info.stFrameData.pVirAddr[1]) {
                    fprintf(stderr, "empty frame VA y=%p uv=%p\n",
                            info.stFrameData.pVirAddr[0], info.stFrameData.pVirAddr[1]);
                    mi.MI_SYS_ChnInputPortPutBuf(handle, &info, MI_TRUE);
                    break;
                }
                generate_start_us = now_us();
                generate_nv12_frame(&info.stFrameData, &noise, width, height, frame);
                generate_us = (unsigned)(now_us() - generate_start_us);
                report_generate_us += generate_us;
                if (!report_generated || generate_us < report_generate_min_us)
                    report_generate_min_us = generate_us;
                if (generate_us > report_generate_max_us)
                    report_generate_max_us = generate_us;
                report_generated++;
                info.u64Pts = conf.u64TargetPts;
                ret = mi.MI_SYS_ChnInputPortPutBuf(handle, &info, MI_FALSE);
                if (ret != MI_SUCCESS) {
                    fprintf(stderr, "MI_SYS_ChnInputPortPutBuf frame %u -> %#x\n", frame, ret);
                    break;
                }
                frame_time_push(&frame_times, generate_start_us);
                frame_time_push(&encode_times, now_us());
                frame++;
                report_submitted++;
            }
        }

        for (unsigned drain = 0; drain < 8; drain++) {
            MI_VENC_ChnStat_t stat;
            MI_VENC_Pack_t pack[MAX_VENC_PACKS];
            MI_VENC_Stream_t stream;
            MI_U64 frame_start_us;
            MI_U64 encode_start_us;
            MI_U64 stream_ready_us;
            unsigned frame_time_us;
            unsigned encode_time_us;
            memset(&stat, 0, sizeof(stat));
            ret = mi.MI_VENC_Query(chn, &stat);
            if (ret != MI_SUCCESS || stat.u32CurPacks == 0)
                break;
            memset(pack, 0, sizeof(pack));
            memset(&stream, 0, sizeof(stream));
            stream.pstPack = pack;
            stream.u32PackCount = stat.u32CurPacks > MAX_VENC_PACKS ? MAX_VENC_PACKS : stat.u32CurPacks;
            ret = mi.MI_VENC_GetStream(chn, &stream, 0);
            if (ret != MI_SUCCESS)
                break;
            stream_ready_us = now_us();
            encoded_len = 0;
            for (MI_U32 p = 0; p < stream.u32PackCount; p++) {
                if (pack[p].pu8Addr && pack[p].u32Len > pack[p].u32Offset) {
                    if (append_buf(&encoded, &encoded_len, &encoded_cap,
                                   pack[p].pu8Addr + pack[p].u32Offset,
                                   pack[p].u32Len - pack[p].u32Offset)) {
                        fprintf(stderr, "failed to grow encoded buffer\n");
                        mi.MI_VENC_ReleaseStream(chn, &stream);
                        goto out;
                    }
                }
            }
            mi.MI_VENC_ReleaseStream(chn, &stream);
            if (encoded_len) {
                if (output_write_frame(&out, &cfg, encoded, encoded_len, report_output))
                    goto out;
                report_bytes += encoded_len;
                if (frame_time_pop(&frame_times, &frame_start_us) == 0) {
                    frame_time_us = (unsigned)(now_us() - frame_start_us);
                    report_frame_time_us += frame_time_us;
                    if (!report_frame_time || frame_time_us < report_frame_time_min_us)
                        report_frame_time_min_us = frame_time_us;
                    if (frame_time_us > report_frame_time_max_us)
                        report_frame_time_max_us = frame_time_us;
                    report_frame_time++;
                }
                if (frame_time_pop(&encode_times, &encode_start_us) == 0) {
                    encode_time_us = (unsigned)(stream_ready_us - encode_start_us);
                    report_encode_time_us += encode_time_us;
                    if (!report_encode_time || encode_time_us < report_encode_time_min_us)
                        report_encode_time_min_us = encode_time_us;
                    if (encode_time_us > report_encode_time_max_us)
                        report_encode_time_max_us = encode_time_us;
                    report_encode_time++;
                }
                report_output++;
            }
        }

        if (current_us < next_frame_us) {
            MI_U64 sleep_us = next_frame_us - current_us;
            if (sleep_us > 1000)
                sleep_us = 1000;
            usleep((useconds_t)sleep_us);
        }

        current_us = now_us();
        if (current_us - last_report_us >= 1000000ULL) {
            double secs = (double)(current_us - last_report_us) / 1000000.0;
            double submit_fps = report_submitted / secs;
            double output_fps = report_output / secs;
            double mbps = report_bytes * 8.0 / secs / 1000000.0;
            double avg_generate_ms = report_generated ? (double)report_generate_us / report_generated / 1000.0 : 0.0;
            double avg_frame_ms = report_frame_time ? (double)report_frame_time_us / report_frame_time / 1000.0 : 0.0;
            double avg_encode_ms = report_encode_time ? (double)report_encode_time_us / report_encode_time / 1000.0 : 0.0;
            printf("stats: submit %.1f fps, output %.1f fps, %.2f Mbit/s, generate %.2f ms avg %.2f min %.2f max, encode %.2f ms avg %.2f min %.2f max, frame %.2f ms avg %.2f min %.2f max\n",
                   submit_fps, output_fps, mbps, avg_generate_ms,
                   report_generate_min_us / 1000.0, report_generate_max_us / 1000.0,
                   avg_encode_ms, report_encode_time_min_us / 1000.0, report_encode_time_max_us / 1000.0,
                   avg_frame_ms, report_frame_time_min_us / 1000.0, report_frame_time_max_us / 1000.0);
            report_submitted = 0;
            report_output = 0;
            report_bytes = 0;
            report_generated = 0;
            report_generate_us = 0;
            report_generate_min_us = 0;
            report_generate_max_us = 0;
            report_frame_time = 0;
            report_frame_time_us = 0;
            report_frame_time_min_us = 0;
            report_frame_time_max_us = 0;
            report_encode_time = 0;
            report_encode_time_us = 0;
            report_encode_time_min_us = 0;
            report_encode_time_max_us = 0;
            last_report_us = current_us;
        }
    }

out:
    if (out.fp)
        fflush(out.fp);
    mi.MI_VENC_StopRecvPic(chn);
    mi.MI_VENC_DestroyChn(chn);
    mi.MI_SYS_Exit();
    output_close(&out);
    free(encoded);
    noise_pattern_free(&noise);
    return 0;
}
