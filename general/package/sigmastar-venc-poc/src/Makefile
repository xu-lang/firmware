CC ?= gcc
CFLAGS ?= -Os -Wall -Wextra
CFLAGS += -std=c11
LDFLAGS += -ldl
LDFLAGS += -Wl,-rpath,/mnt/mmcblk0p1 -Wl,-rpath,/usr/lib

BINARIES = sigmastar_venc_poc

all: $(addprefix output/,$(BINARIES))

output:
	mkdir -p $@

output/mi_probe.o: mi_probe.c mi_abi.h | output
	$(CC) $(CFLAGS) -Dmain=mi_probe_main -c -o $@ mi_probe.c

output/nv12_venc.o: nv12_venc.c mi_abi.h | output
	$(CC) $(CFLAGS) -Dmain=nv12_venc_main -c -o $@ nv12_venc.c

output/camera_raw_dump.o: camera_raw_dump.c mi_abi.h | output
	$(CC) $(CFLAGS) -Dmain=camera_raw_dump_main -c -o $@ camera_raw_dump.c

output/sigmastar_venc_poc: sigmastar_venc_poc.c output/mi_probe.o output/nv12_venc.o output/camera_raw_dump.o | output
	$(CC) $(CFLAGS) -o $@ sigmastar_venc_poc.c output/mi_probe.o output/nv12_venc.o output/camera_raw_dump.o $(LDFLAGS)

clean:
	rm -rf output

.PHONY: all clean
