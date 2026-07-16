#include <stdio.h>
#include <string.h>

int mi_probe_main(int argc, char **argv);
int nv12_venc_main(int argc, char **argv);
int camera_raw_dump_main(int argc, char **argv);

static void usage(const char *prog)
{
    fprintf(stderr,
        "Usage: %s <command> [args...]\n"
        "Commands:\n"
        "  probe       run the MI/VENC lifecycle probe\n"
        "  nv12        encode synthetic NV12 frames with VENC\n"
        "  raw-dump    capture camera frames from VPE as NV12\n",
        prog);
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        usage(argv[0]);
        return 1;
    }

    if (!strcmp(argv[1], "probe"))
        return mi_probe_main(argc - 1, argv + 1);
    if (!strcmp(argv[1], "nv12"))
        return nv12_venc_main(argc - 1, argv + 1);
    if (!strcmp(argv[1], "raw-dump"))
        return camera_raw_dump_main(argc - 1, argv + 1);

    usage(argv[0]);
    return 1;
}
