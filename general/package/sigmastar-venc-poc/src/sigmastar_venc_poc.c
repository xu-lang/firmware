#define _DEFAULT_SOURCE

#include "time_sync.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define SERVER_HOST_ENV "SIGMASTAR_SERVER_HOST"
#define LED_CONTROL_PORT_ENV "SIGMASTAR_LED_CONTROL_PORT"
#define SYNC_ADDR_ENV "SIGMASTAR_SYNC_ADDR"

int mi_probe_main(int argc, char **argv);
int nv12_venc_main(int argc, char **argv);
int camera_raw_dump_main(int argc, char **argv);

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s <command> [args...]\n"
        "Global options after <command> are accepted by all commands:\n"
        "  --server <host>  PC/server IP for tsync and RTP\n"
        "  -o <file>       output file, mutually exclusive with --server\n"
        "  --tsync <port>   sync clock with --server:<port>; LED uses tsync local source port\n"
        "  -t, --sync <host:port>  sync clock before running command\n"
        "Commands:\n"
        "  probe       run the MI/VENC lifecycle probe\n"
        "  nv12        encode synthetic NV12 frames with VENC\n"
        "  raw-dump       capture camera frames from VPE as NV12\n"
        "  led-mark-dump  capture ROI frames and mark frames while GPIO LED is on\n"
        "  venc-dump      capture camera frames through VENC as H.265\n",
        prog);
}

static int valid_port(const char *value)
{
    char *end;
    unsigned long port;

    if (!value || !value[0]) return 0;
    port = strtoul(value, &end, 10);
    return *end == '\0' && port > 0 && port <= 65535;
}

int main(int argc, char **argv)
{
    const char *sync_addr = NULL;
    const char *server_host = NULL;
    const char *tsync_port = NULL;
    const char *output_path = NULL;
    char sync_buf[160];
    char **filtered;
    int filtered_argc = 0;

    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    filtered = malloc(((size_t)argc + 2) * sizeof(*filtered));
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
        if (!strcmp(arg, "--server")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s requires host\n", arg);
                free(filtered);
                return 1;
            }
            server_host = argv[++i];
            continue;
        }
        if (!strncmp(arg, "--server=", 9)) {
            server_host = arg + 9;
            continue;
        }
        if (!strcmp(arg, "--tsync")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s requires port\n", arg);
                free(filtered);
                return 1;
            }
            tsync_port = argv[++i];
            continue;
        }
        if (!strncmp(arg, "--tsync=", 8)) {
            tsync_port = arg + 8;
            continue;
        }
        if (!strcmp(arg, "-o")) {
            if (i + 1 >= argc) {
                fprintf(stderr, "%s requires file\n", arg);
                free(filtered);
                return 1;
            }
            output_path = argv[++i];
            continue;
        }
        filtered[filtered_argc++] = argv[i];
    }

    if (filtered_argc < 2) {
        usage(argv[0]);
        free(filtered);
        return 1;
    }

    if (server_host && output_path) {
        fprintf(stderr, "--server cannot be used with -o\n");
        free(filtered);
        return 1;
    }

    if (server_host && server_host[0])
        setenv(SERVER_HOST_ENV, server_host, 1);

    if (tsync_port) {
        if (!server_host || !server_host[0]) {
            fprintf(stderr, "--tsync requires --server\n");
            free(filtered);
            return 1;
        }
        if (!valid_port(tsync_port)) {
            fprintf(stderr, "bad --tsync port %s\n", tsync_port);
            free(filtered);
            return 1;
        }
    }

    if (!sync_addr && tsync_port) {
        snprintf(sync_buf, sizeof(sync_buf), "%s:%s", server_host, tsync_port);
        sync_addr = sync_buf;
    }

    if (sync_addr) {
        if (time_sync_addr(sync_addr))
            fprintf(stderr, "warning: clock sync failed, continuing with local time\n");
        if (tsync_port) {
            char local_port[16];
            int port = time_sync_local_port();
            if (port <= 0) {
                fprintf(stderr, "--tsync did not create a local UDP port for LED control\n");
                free(filtered);
                return 1;
            }
            snprintf(local_port, sizeof(local_port), "%d", port);
            setenv(LED_CONTROL_PORT_ENV, local_port, 1);
            setenv(SYNC_ADDR_ENV, sync_addr, 1);
        }
    }

    int ret;
    if (!strcmp(filtered[1], "probe")) {
        if (output_path) {
            fprintf(stderr, "-o is not supported by probe\n");
            free(filtered);
            return 1;
        }
        ret = mi_probe_main(filtered_argc - 1, filtered + 1);
    }
    else if (!strcmp(filtered[1], "nv12")) {
        if (output_path) {
            fprintf(stderr, "-o is not supported by nv12\n");
            free(filtered);
            return 1;
        }
        ret = nv12_venc_main(filtered_argc - 1, filtered + 1);
    }
    else if (!strcmp(filtered[1], "raw-dump") || !strcmp(filtered[1], "led-mark-dump") || !strcmp(filtered[1], "venc-dump")) {
        if (output_path) {
            filtered[filtered_argc++] = "-o";
            filtered[filtered_argc++] = (char *)output_path;
        }
        ret = camera_raw_dump_main(filtered_argc - 1, filtered + 1);
    }
    else {
        usage(argv[0]);
        ret = 1;
    }

    free(filtered);
    return ret;
}
