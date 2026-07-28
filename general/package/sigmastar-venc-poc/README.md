# Sigmastar VENC PoC and ABI Notes

`sigmastar-venc-poc` contains experimental userspace tests for the SigmaStar
SSC338Q/infinity6e VENC path. The code does not include official SigmaStar SDK
headers. Instead, `src/mi_abi.h` reconstructs the minimum ABI from public
SigmaStar documentation, exported symbols in `libmi_sys.so`/`libmi_venc.so`,
OpenIPC/Divinus adapter code, and board-level runtime probing.

These declarations are for experiments only. Keep the official SDK headers as
the source of truth if they become available.

## Commands

`sigmastar_venc_poc probe` checks the minimum VENC channel lifecycle:

```text
MI_SYS_Init
MI_VENC_CreateChn
MI_VENC_StartRecvPic
MI_VENC_GetFd
MI_VENC_StopRecvPic
MI_VENC_DestroyChn
MI_SYS_Exit
```

`sigmastar_venc_poc nv12` generates synthetic NV12 frames, injects them
into a VENC input port, drains H.264/H.265 elementary stream output, and
optionally packetizes the stream as RTP over UDP.

`sigmastar_venc_poc raw-dump` creates a minimal camera pipeline based on the
OpenIPC Divinus i6 path:

```text
SNR -> VIF -> VPE port0 -> MI_SYS_ChnOutputPortGetBuf
            -> VPE port1 -> dummy VENC sink
```

It dumps VPE NV12 frames to a raw file. The dummy VENC channel keeps the VPE
pipeline active while the raw-dump mode reads from VPE port0.

Example:

```sh
sigmastar_venc_poc nv12 -g 0 -s 4 -b 4096 -r 12,8,8,12 udp://192.168.1.3:5600
```

Capture one 1920x1080 NV12 frame from the camera:

```sh
sigmastar_venc_poc raw-dump -M vpe --resolution 1920x1080 -f 20 -n 1 -o /tmp/camera-1080p.nv12
```

Capture one full-resolution IMX415 frame:

```sh
sigmastar_venc_poc raw-dump -M vpe --resolution 3840x2160 -f 20 -n 1 -o /tmp/camera-4k.nv12
```

Load the same ISP/sensor config bin used by Divinus after creating the camera pipeline:

```sh
sigmastar_venc_poc raw-dump -M vpe --resolution 1280x720 -f 30 --sensor-config /etc/sensors/imx415.bin -n 1 -o /tmp/camera-720p.nv12
```

## Runtime Loading

The PoC uses `dlopen()` and `dlsym()` instead of compile-time linking. It loads:

```text
libcam_os_wrapper.so
libmi_sys.so
libmi_venc.so
libmi_sensor.so
libmi_vif.so
libmi_vpe.so
libmi_isp.so
libcus3a.so
libispalgo.so
```

The binary is built with RPATH entries for:

```text
/mnt/mmcblk0p1
/usr/lib
```

This lets the target board use `/mnt/mmcblk0p1/libmi_venc.so` without setting
`LD_LIBRARY_PATH`.

The raw-dump mode needs the camera stack libraries as well as `libmi_venc.so`.
On minimal images where these libraries are not installed under `/usr/lib`, copy
them to `/mnt/mmcblk0p1` or run with `LD_LIBRARY_PATH=/mnt/mmcblk0p1:/usr/lib`.

## Basic Types

The ABI uses simple fixed-width aliases:

```c
typedef int32_t MI_S32;
typedef uint8_t MI_U8;
typedef uint16_t MI_U16;
typedef uint32_t MI_U32;
typedef uint64_t MI_U64;
typedef uint64_t MI_PHY;
typedef int MI_BOOL;
typedef int MI_SYS_BUF_HANDLE;
```

`MI_PHY` is represented as `uint64_t`. This matches the tested userspace ABI on
SSC338Q for the structures used by the PoC.

## Module and Port ABI

`MI_SYS_ChnPort_t` identifies a module/dev/channel/port tuple:

```c
typedef struct {
    MI_ModuleId_e eModId;
    MI_U32 u32DevId;
    MI_U32 u32ChnId;
    MI_U32 u32PortId;
} MI_SYS_ChnPort_t;
```

The PoC injects frames into:

```c
{ E_MI_MODULE_ID_VENC, 0, venc_channel, 0 }
```

Only the enum values needed by the PoC are reconstructed. Do not assume the enum
is complete for every SigmaStar SDK generation.

## SYS Frame Injection ABI

The PoC uses `MI_SYS_ChnInputPortGetBuf()` and
`MI_SYS_ChnInputPortPutBuf()` to inject APP-generated NV12 frames into VENC.

Loaded symbols:

```text
MI_SYS_Init
MI_SYS_Exit
MI_SYS_ChnInputPortGetBuf
MI_SYS_ChnInputPortPutBuf
```

Important structures:

```text
MI_SYS_BufConf_t
MI_SYS_BufInfo_t
MI_SYS_FrameData_t
MI_SYS_FrameIspInfo_t
MI_SYS_WindowRect_t
```

The tested buffer request uses:

```c
conf.eBufType = E_MI_SYS_BUFDATA_FRAME;
conf.u32Flags = MI_SYS_MAP_VA;
conf.stFrameCfg.eFormat = E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420;
conf.stFrameCfg.eFrameScanMode = E_MI_SYS_FRAME_SCAN_MODE_PROGRESSIVE;
conf.stFrameCfg.eCompressMode = E_MI_SYS_COMPRESS_MODE_NONE;
```

`MI_SYS_FrameIspInfo_t` is intentionally opaque:

```c
typedef struct {
    MI_U8 opaque[256];
} MI_SYS_FrameIspInfo_t;
```

A smaller local definition caused stack corruption during early tests. The
opaque size is a defensive compatibility choice, not a verified official layout.

Runtime-tested sizes currently printed by the `nv12` command on SSC338Q:

```text
chn=76 stream=64 bufinfo=384 framedata=352
```

Treat these as sanity checks for this board/software combination only.

## VENC Channel ABI

Loaded lifecycle and stream symbols:

```text
MI_VENC_CreateChn
MI_VENC_DestroyChn
MI_VENC_StartRecvPic
MI_VENC_StopRecvPic
MI_VENC_Query
MI_VENC_GetStream
MI_VENC_ReleaseStream
MI_VENC_GetFd
```

The PoC reconstructs enough of these structures to create an H.264/H.265
channel and read encoded packs:

```text
MI_VENC_AttrH26x_t
MI_VENC_Attr_t
MI_VENC_AttrH26xCbr_t
MI_VENC_AttrH26xVbr_t
MI_VENC_RcAttr_t
MI_VENC_ChnAttr_t
MI_VENC_NaluType_t
MI_VENC_PackInfo_t
MI_VENC_Pack_t
MI_VENC_StreamInfoH265_t
MI_VENC_Stream_t
MI_VENC_ChnStat_t
```

Verified channel creation settings include:

```text
H.265 1920x1080 90fps CBR 4096 kbit/s GOP1
H.265 1920x1080 90fps CBR 4096 kbit/s GOP1 with slice split rows=4
```

The PoC sets `u32BitRate`/`u32MaxBitRate` as `kbit/s * 1024`, matching the
observed SigmaStar ABI behavior used in existing tests.

## Slice Split ABI

Loaded symbols:

```text
MI_VENC_SetH264SliceSplit
MI_VENC_GetH264SliceSplit
MI_VENC_SetH265SliceSplit
MI_VENC_GetH265SliceSplit
```

Structure:

```c
typedef struct {
    MI_BOOL bSplitEnable;
    MI_U32 u32SliceRowCount;
} MI_VENC_ParamH26xSliceSplit_t;
```

Validated on SSC338Q:

```text
MI_VENC_GetH265SliceSplit -> 0 enable=0 rows=0
MI_VENC_SetH265SliceSplit rows=4 -> 0
MI_VENC_SetH265SliceSplit rows=8 -> 0
```

For H.265, `u32SliceRowCount` is in coding tree block/macroblock-style rows.
With 1080p input, `rows=4` roughly means horizontal slices about 64 pixels high.

No tile-row/tile-column VENC API has been found in the current libraries.

## ROI QP ABI

Loaded symbols:

```text
MI_VENC_SetRoiCfg
MI_VENC_GetRoiCfg
```

Exported by the current `libmi_venc.so`:

```text
MI_VENC_SetRoiCfg
MI_VENC_GetRoiCfg
MI_VENC_SetRoiBgFrameRate
MI_VENC_GetRoiBgFrameRate
```

Only `Set/GetRoiCfg` are declared by the PoC today.

Structure used by the C ABI:

```c
typedef struct {
    MI_U32 u32Left;
    MI_U32 u32Top;
    MI_U32 u32Width;
    MI_U32 u32Height;
} MI_VENC_Rect_t;

typedef struct {
    MI_U32 u32Index;
    MI_BOOL bEnable;
    MI_BOOL bAbsQp;
    MI_S32 s32Qp;
    MI_VENC_Rect_t stRect;
} MI_VENC_RoiCfg_t;
```

The PoC uses relative QP mode:

```c
bAbsQp = MI_FALSE;
s32Qp = relative_qp_offset;
```

The `nv12` command accepts an adaptive-link style argument:

```sh
sigmastar_venc_poc nv12 -r 12,8,8,12 output.h265
```

Positive relative QP values increase QP in the ROI and usually reduce
quality/bitrate there. Negative values decrease QP and usually improve
quality/increase bitrate. The PoC accepts `[-32,31]`.

Current SSC338Q validation only proved that ROI index 0 with a left-side region
is accepted:

```text
MI_VENC_SetRoiCfg index=0 qp=12 rect=224x1024x0x32 -> 0
```

Attempts to set additional right-side/high-X regions returned `0xa0022003`.
Because of that, the PoC currently accepts the four-value `-r` syntax but only
applies the first value to ROI index 0.

## ROI Rect Ordering

Keep these two layers separate.

Majestic/adaptive-link style text configuration may represent ROI rectangles as:

```text
<left>x<top>x<height>x<width>
```

The SigmaStar `MI_VENC_RoiCfg_t` C ABI used by this PoC follows the vendor VENC
API structure order:

```text
left, top, width, height
```

Changing the C struct to `left, top, height, width` was tested on SSC338Q and
made previously accepted ROI settings fail. The text format and the C struct
layout are separate layers.

## RTP and NAL Handling

The PoC parses Annex-B start codes from the returned elementary stream and can
write either a raw stream file or RTP over UDP.

RTP constants:

```text
payload type: 97
clock: 90000
max payload: 1200 bytes
```

H.264 VCL NAL detection uses NAL types `1..5`. H.265 VCL NAL detection uses NAL
types `0..31`.

## Camera VENC SD Capture Test

`sigmastar_venc_poc venc-dump` creates the camera pipeline, binds VPE port1 to
VENC, drains the H.265 elementary stream with `MI_VENC_GetStream()`, and writes
the encoded stream to SD card. It also writes a sidecar TSV with one row per
encoded frame plus LED transition rows.

Pipeline:

```text
SNR -> VIF -> VPE port0
            -> VPE port1 -> VENC -> MI_VENC_GetStream -> .h265
```

The LED polarity default remains active-low for general OpenIPC boards. The
tested board's red GPIO6 LED is active-high, so add `--led-active-high` for this
board.

Build and upload the current PoC binary:

```sh
cd /home/xulang/github/firmware/general/package/sigmastar-venc-poc/src
make clean
make CC=/home/xulang/github/firmware/output/host/opt/ext-toolchain/bin/arm-openipc-linux-gnueabihf-gcc.br_real
sshpass -p 12345 scp -O output/sigmastar_venc_poc root@192.168.1.10:/mnt/mmcblk0p1/sigmastar_venc_poc
```

Run a 10-second 720p120 H.265 capture to SD:

```sh
sshpass -p 12345 ssh root@192.168.1.10 \
  'cd /mnt/mmcblk0p1 && \
   chmod +x ./sigmastar_venc_poc && \
   rm -f camera-test.h265 camera-test.h265.tsv venc-dump.log && \
   LD_LIBRARY_PATH=/mnt/mmcblk0p1:/usr/lib \
   timeout -s INT 10s ./sigmastar_venc_poc \
     --sync 192.168.1.3:5602 \
     venc-dump \
     -r 1280x720 -f 120 \
     --sensor-config /etc/sensors/imx415_fpv.bin \
     -x 1 \
     -n 0 \
     -o /mnt/mmcblk0p1/camera-test.h265 \
     --bitrate 8192 \
     --led-active-high \
     > /mnt/mmcblk0p1/venc-dump.log 2>&1; \
   sed -n "/venc-dump/p;/GPIO6 LED ready/p" /mnt/mmcblk0p1/venc-dump.log; \
   wc -c /mnt/mmcblk0p1/camera-test.h265 /mnt/mmcblk0p1/camera-test.h265.tsv'
```

Representative result on SSC338Q/IMX415:

```text
GPIO6 LED ready, active_high
venc-dump frames=1044 bytes=9212265 meta=/mnt/mmcblk0p1/camera-test.h265.tsv elapsed=8.710 s avg=1032.799 KB/s
```

This is about `1 MB/s`, so SD writing is not the bottleneck for encoded H.265
720p120 at `8192 kbit/s`.

Download the stream and metadata:

```sh
sshpass -p 12345 scp -O root@192.168.1.10:/mnt/mmcblk0p1/camera-test.h265 /home/xulang/github/firmware/camera-test.h265
sshpass -p 12345 scp -O root@192.168.1.10:/mnt/mmcblk0p1/camera-test.h265.tsv /home/xulang/github/firmware/camera-test.h265.tsv
```

TSV columns:

```text
type    frame   seq   pts_us   pc_time_us   mono_us   bytes   led
```

Rows with `type=frame` correspond to encoded frames in stream order. Rows with
`type=led-on` or `type=led-off` record GPIO transition times. The `led` column is
the software LED state at the time that encoded frame was drained.

Validated alignment for the captured stream:

```sh
awk -F '\t' 'BEGIN{count=0; bad=0} $1=="frame"{if($2!=count || $3!=count) bad++; count++} END{print "tsv_frames",count,"bad",bad}' camera-test.h265.tsv
ffprobe -v error -count_frames -select_streams v:0 -show_entries stream=nb_read_frames,r_frame_rate -of default=noprint_wrappers=1 camera-test.h265
ffprobe -v error -select_streams v:0 -show_entries frame=pict_type -of csv=p=0 camera-test.h265 | sort | uniq -c
```

Expected checks:

```text
tsv_frames 1044 bad 0
nb_read_frames=1044
I/P frames only, no B frames
```

## VENC Capture Video Post-Processing

Generate a review MP4 from the H.265 stream, drawing a green block on frames
where TSV `led=1`. Use `-bf 0` so VLC frame stepping is not affected by B-frame
reordering:

```sh
cd /home/xulang/github/firmware
expr=$(awk -F '\t' 'BEGIN{inrun=0; first=1} \
  $1=="frame" { \
    f=$2; led=$8+0; \
    if (led && !inrun) {start=f; inrun=1} \
    else if (!led && inrun) { \
      end=f-1; \
      if (!first) printf "+"; \
      printf "between(n\\,%d\\,%d)", start, end; \
      first=0; inrun=0 \
    } \
  } \
  END{ \
    if (inrun) { \
      if (!first) printf "+"; \
      printf "between(n\\,%d\\,%d)", start, f \
    } \
  }' camera-test.h265.tsv)

ffmpeg -y \
  -r 120 -i camera-test.h265 \
  -vf "drawbox=x=0:y=0:w=96:h=64:color=lime@0.85:t=fill:enable='$expr'" \
  -c:v libx264 -bf 0 -g 120 -pix_fmt yuv420p -r 120 \
  camera-test.mp4
```

Verify the MP4 is 120fps and has no B frames:

```sh
ffprobe -v error -select_streams v:0 \
  -show_entries stream=has_b_frames,avg_frame_rate,r_frame_rate,nb_frames,duration \
  -of default=noprint_wrappers=1 \
  camera-test.mp4
```

Expected:

```text
has_b_frames=0
r_frame_rate=120/1
avg_frame_rate=120/1
nb_frames=<same as TSV frame count>
```

For frame-index debugging, also burn ffmpeg's decoded frame number into the
video:

```sh
ffmpeg -y \
  -r 120 -i camera-test.h265 \
  -vf "drawbox=x=0:y=0:w=96:h=64:color=lime@0.85:t=fill:enable='$expr',drawtext=text='%{n}':x=12:y=80:fontsize=48:fontcolor=yellow:box=1:boxcolor=black@0.6" \
  -c:v libx264 -bf 0 -g 120 -pix_fmt yuv420p -r 120 \
  camera-test-indexed.mp4
```

Interpretation of this LED test:

```text
green block on frame N = software toggled/held LED state while draining encoded frame N
visible LED change on frame N+k = capture/encode closed-loop latency of about k / 120 seconds
```

This is a closed-loop measurement from encoded-frame drain time to the LED change
appearing in later encoded frames. It is not the same as absolute optical-event
to encoded-output latency, but in an ideal no-reordering pipeline it should be no
less than the optical pipeline latency plus the sampling phase/LED response.

## Verified Target Notes

Validated target board:

```text
SoC: SSC338Q / infinity6e
Kernel: 4.9.84 OpenIPC build
Library path: /mnt/mmcblk0p1/libmi_venc.so
```

Representative successful command:

```sh
/mnt/mmcblk0p1/sigmastar_venc_poc nv12 -g 0 -s 4 -b 4096 -r 12,8,8,12 /mnt/mmcblk0p1/roi_test.h265
```

Representative output:

```text
MI_SYS_Init -> 0
MI_VENC_CreateChn -> 0
MI_VENC_SetH265SliceSplit rows=4 -> 0
MI_VENC_SetRoiCfg index=0 qp=12 rect=224x1024x0x32 -> 0
MI_VENC_StartRecvPic -> 0
stats: submit 90.0 fps, output 89.0 fps, 5.50 Mbit/s
```

## Known Risks

The ABI is incomplete and partly opaque. In particular:

```text
MI_SYS_FrameIspInfo_t is opaque padding.
MI_VENC_StreamInfoH264_t and JPEG stream info are represented as byte padding.
Only CBR/VBR channel attributes used by the PoC are modeled.
ROI background frame-rate APIs are exported but not declared here.
RcParam, Dblk, Trans, IntraRefresh, Crop, Ref, and CustomMap APIs are not modeled.
```

Any new API should be added with a target-board probe and should print return
codes before being treated as usable.


## 测试延迟

测试路径

```
 .----------led---------------------.
|                                   ^
v                                   |
camera->vif->vpe->venc->rtp->ffmpeg->yuv
```

测试方法：将摄像头对准板子led，pc端发送UDP信号给板子，周期控制其led亮灭，同时，pc端记录发送UDP信号的时间戳，作为led点亮的时刻（误差为udp传输时间，<1ms），点亮led后，后续解码出来的yuv帧，都打上“led亮”的标记，标记从这帧开始（误差<1帧）led点亮，最后将所有yuv帧编码成MP4，然后单帧跟踪“led亮”的帧和实际图像led变亮帧之间的差值。

设备端：

```bash
./sigmastar_venc_poc --server 192.168.1.3 --tsync 5602 v \
enc-dump -r 1280x720 -f 120 --sensor-config /etc/sensors/imx415_fpv.bin -x 1 -n \
0 --led-active-high --rtp 5600 --bitrate 8192
```

pc端运行aviateus，选择Local->record raw rtp->frame decoded->start

测试 720@120，曝光 1ms，实测间隔2-3帧，约17-25ms，加上1帧的误差，约25-33ms。这里不包括后面图像送显和显示屏响应的延迟。