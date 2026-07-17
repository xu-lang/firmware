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
