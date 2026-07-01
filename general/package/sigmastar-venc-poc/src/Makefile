CC ?= gcc
CFLAGS ?= -Os -Wall -Wextra
CFLAGS += -std=c11
LDFLAGS += -ldl
LDFLAGS += -Wl,-rpath,/mnt/mmcblk0p1 -Wl,-rpath,/usr/lib

BINARIES = mi_probe nv12_venc_poc

all: $(addprefix output/,$(BINARIES))

output:
	mkdir -p $@

output/mi_probe: mi_probe.c mi_abi.h | output
	$(CC) $(CFLAGS) -o $@ mi_probe.c $(LDFLAGS)

output/nv12_venc_poc: nv12_venc_poc.c mi_abi.h | output
	$(CC) $(CFLAGS) -o $@ nv12_venc_poc.c $(LDFLAGS)

clean:
	rm -rf output

.PHONY: all clean
