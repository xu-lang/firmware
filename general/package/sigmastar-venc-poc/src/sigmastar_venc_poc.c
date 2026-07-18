#include "time_sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int mi_probe_main(int argc, char **argv);
int nv12_venc_main(int argc, char **argv);
int camera_raw_dump_main(int argc, char **argv);

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s <command> [args...]\n"
        "Global options after <command> are accepted by all commands:\n"
        "  -t, --sync <host:port>  sync clock before running command\n"
        "Commands:\n"
        "  probe       run the MI/VENC lifecycle probe\n"
        "  nv12        encode synthetic NV12 frames with VENC\n"
        "  raw-dump       capture camera frames from VPE as NV12\n"
        "  led-mark-dump  capture ROI frames and mark frames while GPIO LED is on\n",
        prog);
}

int main(int argc, char **argv)
{
    const char *sync_addr = NULL;
    char **filtered;
    int filtered_argc = 0;

    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    filtered = malloc((size_t)argc * sizeof(*filtered));
    if (!filtered) {
        fprintf(stderr, "alloc argv failed\n");
        return 1;
    }
    filtered[filtered_argc++] = argv[0];

    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (!strcmp(arg, "-t") || !strcmp(arg, "--sync")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s requires host:port\n", arg);
                free(filtered);
                return 1;
            }
            sync_addr = argv[++i];
            continue;
        }
        if (!strncmp(arg, "--sync=", 7)) {
            sync_addr = arg + 7;
            continue;
        }
        filtered[filtered_argc++] = argv[i];
    }

    if (filtered_argc < 2) {
        usage(argv[0]);
        free(filtered);
        return 1;
    }

    if (sync_addr && time_sync_addr(sync_addr))
        fprintf(stderr, "warning: clock sync failed, continuing with local time\n");

    int ret;
    if (!strcmp(filtered[1], "probe"))
        ret = mi_probe_main(filtered_argc - 1, filtered + 1);
    else if (!strcmp(filtered[1], "nv12"))
        ret = nv12_venc_main(filtered_argc - 1, filtered + 1);
    else if (!strcmp(filtered[1], "raw-dump") || !strcmp(filtered[1], "led-mark-dump"))
        ret = camera_raw_dump_main(filtered_argc - 1, filtered + 1);
    else {
        usage(argv[0]);
        ret = 1;
    }

    free(filtered);
    return ret;
}
