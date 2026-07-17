#pragma once

/*
 * Experimental Sigmastar MI ABI declarations.
 *
 * This header is reverse engineered from public Sigmastar MI documentation,
 * exported symbols in libmi_sys.so/libmi_venc.so, Divinus/OpenIPC adapter
 * code, and runtime probing on SSC338Q/infinity6e hardware. It is not copied
 * from an official Sigmastar SDK header.
 *
 * The function prototypes and enum values used by the PoC have been tested
 * enough to create a VENC channel, inject NV12 frames through
 * MI_SYS_ChnInputPortGetBuf()/MI_SYS_ChnInputPortPutBuf(), and capture camera
 * frames through the SNR/VIF/VPE pipeline. Some structure layouts contain
 * opaque padding and are still open to dispute. Re-check these definitions
 * against the vendor SDK before using them as a stable ABI.
 */

#include <stddef.h>
#include <stdint.h>

typedef int32_t MI_S32;
typedef uint8_t MI_U8;
typedef uint16_t MI_U16;
typedef uint32_t MI_U32;
typedef uint64_t MI_U64;
typedef uint64_t MI_PHY;
typedef int MI_BOOL;
typedef int MI_SYS_BUF_HANDLE;

#define MI_SUCCESS 0
#define MI_TRUE 1
#define MI_FALSE 0
#define MI_SYS_MAP_VA 1

typedef enum {
    E_MI_MODULE_ID_IVE = 0,
    E_MI_MODULE_ID_VDF,
    E_MI_MODULE_ID_VENC,
    E_MI_MODULE_ID_RGN,
    E_MI_MODULE_ID_AI,
    E_MI_MODULE_ID_AO,
    E_MI_MODULE_ID_VIF,
    E_MI_MODULE_ID_VPE,
    E_MI_MODULE_ID_VDEC,
    E_MI_MODULE_ID_SYS,
    E_MI_MODULE_ID_FB,
    E_MI_MODULE_ID_HDMI,
    E_MI_MODULE_ID_DIVP,
    E_MI_MODULE_ID_GFX,
    E_MI_MODULE_ID_VDISP,
    E_MI_MODULE_ID_DISP,
} MI_ModuleId_e;

typedef struct {
    MI_ModuleId_e eModId;
    MI_U32 u32DevId;
    MI_U32 u32ChnId;
    MI_U32 u32PortId;
} MI_SYS_ChnPort_t;

typedef enum {
    E_MI_SYS_PIXEL_FRAME_YUV422_YUYV = 0,
    E_MI_SYS_PIXEL_FRAME_ARGB8888,
    E_MI_SYS_PIXEL_FRAME_ABGR8888,
    E_MI_SYS_PIXEL_FRAME_BGRA8888,
    E_MI_SYS_PIXEL_FRAME_RGB565,
    E_MI_SYS_PIXEL_FRAME_ARGB1555,
    E_MI_SYS_PIXEL_FRAME_ARGB4444,
    E_MI_SYS_PIXEL_FRAME_I2,
    E_MI_SYS_PIXEL_FRAME_I4,
    E_MI_SYS_PIXEL_FRAME_I8,
    E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_422,
    E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420,
    E_MI_SYS_PIXEL_FRAME_YUV_SEMIPLANAR_420_NV21,
} MI_SYS_PixelFormat_e;

typedef enum {
    E_MI_SYS_COMPRESS_MODE_NONE = 0,
} MI_SYS_CompressMode_e;

typedef enum {
    E_MI_SYS_FRAME_TILE_MODE_NONE = 0,
} MI_SYS_FrameTileMode_e;

typedef enum {
    E_MI_SYS_FRAME_SCAN_MODE_PROGRESSIVE = 0,
    E_MI_SYS_FRAME_SCAN_MODE_INTERLACE,
} MI_SYS_FrameScanMode_e;

typedef enum {
    E_MI_SYS_FIELDTYPE_NONE = 0,
} MI_SYS_FieldType_e;

typedef enum {
    E_MI_SYS_BUFDATA_RAW = 0,
    E_MI_SYS_BUFDATA_FRAME,
    E_MI_SYS_BUFDATA_META,
    E_MI_SYS_BUFDATA_MULTIPLANE,
} MI_SYS_BufDataType_e;

typedef enum {
    NORMAL_FRAME_DATA = 2,
} MI_SYS_FrameData_PhySignalType;

typedef struct {
    MI_U16 u16X;
    MI_U16 u16Y;
    MI_U16 u16Width;
    MI_U16 u16Height;
} MI_SYS_WindowRect_t;

typedef struct {
    void *pVirAddr;
    MI_PHY phyAddr;
    MI_U32 u32Size;
    MI_U32 u32ExtraData;
    MI_ModuleId_e eDataFromModule;
} MI_SYS_MetaData_t;

typedef struct {
    void *pVirAddr;
    MI_PHY phyAddr;
    MI_U32 u32BufSize;
    MI_U32 u32ContentSize;
    MI_BOOL bEndOfFrame;
    MI_U64 u64SeqNum;
} MI_SYS_RawData_t;

typedef struct {
    MI_U8 opaque[256];
} MI_SYS_FrameIspInfo_t;

typedef struct {
    MI_SYS_FrameTileMode_e eTileMode;
    MI_SYS_PixelFormat_e ePixelFormat;
    MI_SYS_CompressMode_e eCompressMode;
    MI_SYS_FrameScanMode_e eFrameScanMode;
    MI_SYS_FieldType_e eFieldType;
    MI_SYS_FrameData_PhySignalType ePhylayoutType;
    MI_U16 u16Width;
    MI_U16 u16Height;
    void *pVirAddr[3];
    MI_PHY phyAddr[3];
    MI_U32 u32Stride[3];
    MI_U32 u32BufSize;
    MI_U16 u16RingBufStartLine;
    MI_U16 u16RingBufRealTotalHeight;
    MI_SYS_FrameIspInfo_t stFrameIspInfo;
    MI_SYS_WindowRect_t stContentCropWindow;
} MI_SYS_FrameData_t;

typedef struct {
    MI_U64 u64Pts;
    MI_U64 u64SidebandMsg;
    MI_SYS_BufDataType_e eBufType;
    MI_U32 bEndOfStream : 1;
    MI_U32 bUsrBuf : 1;
    MI_U32 bDrop : 1;
    MI_U32 u32IrFlag : 2;
    MI_U32 u32Reserved : 27;
    MI_U32 u32SequenceNumber;
    union {
        MI_SYS_FrameData_t stFrameData;
        MI_SYS_RawData_t stRawData;
        MI_SYS_MetaData_t stMetaData;
    };
} MI_SYS_BufInfo_t;

typedef struct {
    MI_U16 u16Width;
    MI_U16 u16Height;
    MI_SYS_FrameScanMode_e eFrameScanMode;
    MI_SYS_PixelFormat_e eFormat;
    MI_SYS_CompressMode_e eCompressMode;
} MI_SYS_BufFrameConfig_t;

typedef struct {
    MI_U32 u32Size;
} MI_SYS_BufRawConfig_t;

typedef struct {
    MI_U32 u32Size;
} MI_SYS_MetaDataConfig_t;

typedef struct {
    MI_SYS_BufDataType_e eBufType;
    MI_U32 u32Flags;
    MI_U64 u64TargetPts;
    union {
        MI_SYS_BufFrameConfig_t stFrameCfg;
        MI_SYS_BufRawConfig_t stRawCfg;
        MI_SYS_MetaDataConfig_t stMetaCfg;
    };
} MI_SYS_BufConf_t;

typedef enum { I6_COMPR_NONE = 0 } i6_compr;
typedef enum { I6_HDR_OFF = 0 } i6_hdr;
typedef enum { I6_INPUT_VUVU = 0, I6_INPUT_UVUV, I6_INPUT_UYVY = 0, I6_INPUT_VYUY, I6_INPUT_YUYV, I6_INPUT_YVYU } i6_input;
typedef enum { I6_INTF_BT656 = 0, I6_INTF_DIGITAL_CAMERA, I6_INTF_BT1120_STANDARD, I6_INTF_BT1120_INTERLEAVED, I6_INTF_MIPI } i6_intf;
typedef enum { I6_EDGE_SINGLE_UP = 0, I6_EDGE_SINGLE_DOWN, I6_EDGE_DOUBLE } i6_edge;
typedef enum { I6_BAYER_END = 12 } i6_bayer_const;
typedef enum { I6_PIXFMT_YUV422_YUYV = 0, I6_PIXFMT_YUV420SP = 11, I6_PIXFMT_RGB_BAYER = 20 } i6_pixfmt;
typedef enum { I6_VIF_WORK_1MULTIPLEX = 0, I6_VIF_WORK_RGB_REALTIME = 3, I6_VIF_WORK_RGB_FRAME = 4 } i6_vif_work;
typedef enum { I6_VIF_FRATE_FULL = 0 } i6_vif_frate;
typedef enum { I6_VPE_SENS_ID0 = 1 } i6_vpe_sens;
typedef enum { I6_VPE_MODE_CAM = 0x6, I6_VPE_MODE_REALTIME = 0x18 } i6_vpe_mode;
typedef enum { I6_SYS_LINK_FRAMEBASE = 0x1, I6_SYS_LINK_REALTIME = 0x4 } i6_sys_link;

typedef struct { MI_U16 width, height; } i6_dim;
typedef struct { MI_U16 x, y, width, height; } i6_rect;
typedef struct { int vsyncInv, hsyncInv, pixclkInv; MI_U32 vsyncDelay, hsyncDelay, pixclkDelay; } i6_sync;

typedef struct { MI_U32 laneCnt, rgbFmtOn; i6_input input; MI_U32 hsyncMode, sampDelay, hwHdr, virtChn, packType[2]; } i6_snr_mipi;
typedef struct { MI_U32 multplxNum; i6_sync sync; i6_edge edge; int bitswap; } i6_snr_bt656;
typedef struct { i6_sync sync; } i6_snr_par;
typedef union { i6_snr_par parallel; i6_snr_mipi mipi; i6_snr_bt656 bt656; } i6_snr_intfattr;
typedef struct { MI_U32 planeCnt; i6_intf intf; i6_hdr hdr; i6_snr_intfattr intfAttr; char earlyInit; } i6_snr_pad;
typedef struct { MI_U32 planeId; char sensName[32]; i6_rect capt; int bayer; int precision; int hdrSrc; MI_U32 shutter, sensGain, compGain; i6_pixfmt pixFmt; } i6_snr_plane;
typedef struct { i6_rect crop; i6_dim output; MI_U32 maxFps, minFps; char desc[32]; } __attribute__((packed, aligned(4))) i6_snr_res;

typedef struct { i6_intf intf; i6_vif_work work; i6_hdr hdr; i6_edge edge; i6_input input; char bitswap; i6_sync sync; } i6_vif_dev;
typedef struct { i6_rect capt; i6_dim dest; int field; int interlaceOn; i6_pixfmt pixFmt; i6_vif_frate frate; MI_U32 frameLineCnt; } i6_vif_port;

typedef struct { MI_U32 rev, size; unsigned char data[64]; } i6_vpe_iqver;
typedef struct { int mode; char bypassOn, proj3x3On; int proj3x3[9]; MI_U16 userSliceNum; MI_U32 focalLengthX, focalLengthY; void *configAddr; MI_U32 configSize; int mapType; union { struct { void *xMapAddr, *yMapAddr; MI_U32 xMapSize, yMapSize; } dispInfo; struct { void *calibPolyBinAddr; MI_U32 calibPolyBinSize; } calibInfo; }; char lensAdjOn; } i6e_vpe_ildc;
typedef struct { char bypassOn, proj3x3On; int proj3x3[9]; MI_U32 focalLengthX, focalLengthY; void *configAddr; MI_U32 configSize; union { struct { void *xMapAddr, *yMapAddr; MI_U32 xMapSize, yMapSize; } dispInfo; struct { void *calibPolyBinAddr; MI_U32 calibPolyBinSize; } calibInfo; }; } i6e_vpe_ldc;
typedef struct { i6_dim capt; i6_pixfmt pixFmt; i6_hdr hdr; i6_vpe_sens sensor; char noiseRedOn, edgeOn, edgeSmoothOn, contrastOn, invertOn, rotateOn; i6_vpe_mode mode; i6_vpe_iqver iqparam; i6e_vpe_ildc lensInit; char lensAdjOn; MI_U32 chnPort; } i6e_vpe_chn;
typedef struct { char reserved[16]; i6e_vpe_ldc lensAdj; i6_hdr hdr; int level3DNR; char mirror, flip, reserved2, lensAdjOn; } i6e_vpe_para;
typedef struct { i6_dim output; char mirror, flip; i6_pixfmt pixFmt; i6_compr compress; } i6_vpe_port;

typedef struct {
    MI_U32 minShutterUs;
    MI_U32 maxShutterUs;
    MI_U32 minApertX10;
    MI_U32 maxApertX10;
    MI_U32 minSensorGain;
    MI_U32 minIspGain;
    MI_U32 maxSensorGain;
    MI_U32 maxIspGain;
} i6_isp_exp;

typedef enum {
    MI_VENC_CODEC_H264 = 2,
    MI_VENC_CODEC_H265 = 3,
} MI_VENC_ModType_e;

typedef enum {
    MI_VENC_RATEMODE_H264CBR = 1,
    MI_VENC_RATEMODE_H264VBR = 2,
    MI_VENC_RATEMODE_H265CBR = 8,
    MI_VENC_RATEMODE_H265VBR = 9,
} MI_VENC_RcMode_e;

typedef struct {
    MI_U32 u32MaxPicWidth;
    MI_U32 u32MaxPicHeight;
    MI_U32 u32BufSize;
    MI_U32 u32Profile;
    MI_BOOL bByFrame;
    MI_U32 u32PicWidth;
    MI_U32 u32PicHeight;
    MI_U32 u32BFrameNum;
    MI_U32 u32RefNum;
} MI_VENC_AttrH26x_t;

typedef struct {
    MI_VENC_ModType_e eType;
    union {
        MI_VENC_AttrH26x_t stAttrH264e;
        MI_U8 stAttrJpege[36];
        MI_VENC_AttrH26x_t stAttrH265e;
    };
} MI_VENC_Attr_t;

typedef struct {
    MI_U32 u32Gop;
    MI_U32 u32StatTime;
    MI_U32 u32SrcFrmRateNum;
    MI_U32 u32SrcFrmRateDen;
    MI_U32 u32BitRate;
    MI_U32 u32FluctuateLevel;
} MI_VENC_AttrH26xCbr_t;

typedef struct {
    MI_U32 u32Gop;
    MI_U32 u32StatTime;
    MI_U32 u32SrcFrmRateNum;
    MI_U32 u32SrcFrmRateDen;
    MI_U32 u32MaxBitRate;
    MI_U32 u32MaxQp;
    MI_U32 u32MinQp;
} MI_VENC_AttrH26xVbr_t;

typedef struct {
    MI_VENC_RcMode_e eRcMode;
    union {
        MI_VENC_AttrH26xCbr_t stAttrH264Cbr;
        MI_VENC_AttrH26xVbr_t stAttrH264Vbr;
        MI_U8 padding1[28];
        MI_VENC_AttrH26xCbr_t stAttrH265Cbr;
        MI_VENC_AttrH26xVbr_t stAttrH265Vbr;
    };
    void *pstRcNuQualityCfg;
} MI_VENC_RcAttr_t;

typedef struct {
    MI_VENC_Attr_t stVeAttr;
    MI_VENC_RcAttr_t stRcAttr;
} MI_VENC_ChnAttr_t;

typedef union {
    MI_U32 u32Type;
} MI_VENC_NaluType_t;

typedef struct {
    MI_VENC_NaluType_t ePackType;
    MI_U32 u32PackOffset;
    MI_U32 u32PackLength;
    MI_U32 u32SliceId;
} MI_VENC_PackInfo_t;

typedef struct {
    MI_U64 phyAddr;
    MI_U8 *pu8Addr;
    MI_U32 u32Len;
    MI_U64 u64PTS;
    MI_BOOL bFrameEnd;
    MI_VENC_NaluType_t eDataType;
    MI_U32 u32Offset;
    MI_U32 u32PackNum;
    MI_VENC_PackInfo_t stPackInfo[8];
} MI_VENC_Pack_t;

typedef struct {
    MI_U32 u32PicBytesNum;
    MI_U32 u32ICu64x64;
    MI_U32 u32ICu32x32;
    MI_U32 u32ICu16x16;
    MI_U32 u32ICu8x8;
    MI_U32 u32PCu32x32;
    MI_U32 u32PCu16x16;
    MI_U32 u32PCu8x8;
    MI_U32 u32PCu4x4;
    MI_U32 u32RefType;
    MI_U32 u32UpdateAttrCnt;
    MI_U32 u32StartQp;
} MI_VENC_StreamInfoH265_t;

typedef struct {
    MI_VENC_Pack_t *pstPack;
    MI_U32 u32PackCount;
    MI_U32 u32Seq;
    MI_S32 hSeq;
    union {
        MI_U8 stH264Info[48];
        MI_U8 stJpegInfo[12];
        MI_VENC_StreamInfoH265_t stH265Info;
    };
} MI_VENC_Stream_t;

typedef struct {
    MI_U32 u32LeftPics;
    MI_U32 u32LeftStreamBytes;
    MI_U32 u32LeftStreamFrames;
    MI_U32 u32LeftStreamMillisec;
    MI_U32 u32CurPacks;
    MI_U32 u32LeftRecvPics;
    MI_U32 u32LeftEncPics;
    MI_U32 u32FrameRateNum;
    MI_U32 u32FrameRateDen;
    MI_U32 u32BitRate;
} MI_VENC_ChnStat_t;

typedef struct {
    MI_BOOL bSplitEnable;
    MI_U32 u32SliceRowCount;
} MI_VENC_ParamH26xSliceSplit_t;

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

typedef struct {
    void *sys;
    void *venc;
    MI_S32 (*MI_SYS_Init)(void);
    MI_S32 (*MI_SYS_Exit)(void);
    MI_S32 (*MI_SYS_ChnInputPortGetBuf)(MI_SYS_ChnPort_t *, MI_SYS_BufConf_t *, MI_SYS_BufInfo_t *, MI_SYS_BUF_HANDLE *, MI_S32);
    MI_S32 (*MI_SYS_ChnInputPortPutBuf)(MI_SYS_BUF_HANDLE, MI_SYS_BufInfo_t *, MI_BOOL);
    MI_S32 (*MI_VENC_CreateChn)(MI_S32, MI_VENC_ChnAttr_t *);
    MI_S32 (*MI_VENC_DestroyChn)(MI_S32);
    MI_S32 (*MI_VENC_StartRecvPic)(MI_S32);
    MI_S32 (*MI_VENC_StopRecvPic)(MI_S32);
    MI_S32 (*MI_VENC_Query)(MI_S32, MI_VENC_ChnStat_t *);
    MI_S32 (*MI_VENC_GetStream)(MI_S32, MI_VENC_Stream_t *, MI_U32);
    MI_S32 (*MI_VENC_ReleaseStream)(MI_S32, MI_VENC_Stream_t *);
    MI_S32 (*MI_VENC_GetFd)(MI_S32);
    MI_S32 (*MI_VENC_SetH264SliceSplit)(MI_S32, MI_VENC_ParamH26xSliceSplit_t *);
    MI_S32 (*MI_VENC_GetH264SliceSplit)(MI_S32, MI_VENC_ParamH26xSliceSplit_t *);
    MI_S32 (*MI_VENC_SetH265SliceSplit)(MI_S32, MI_VENC_ParamH26xSliceSplit_t *);
    MI_S32 (*MI_VENC_GetH265SliceSplit)(MI_S32, MI_VENC_ParamH26xSliceSplit_t *);
    MI_S32 (*MI_VENC_SetRoiCfg)(MI_S32, MI_VENC_RoiCfg_t *);
    MI_S32 (*MI_VENC_GetRoiCfg)(MI_S32, MI_U32, MI_VENC_RoiCfg_t *);
} mi_libs_t;

typedef struct {
    void *sys, *snr, *vif, *ispalgo, *cus3a, *isp, *vpe, *venc;
    MI_S32 (*MI_SYS_Init)(void);
    MI_S32 (*MI_SYS_Exit)(void);
    MI_S32 (*MI_SYS_BindChnPort2)(MI_SYS_ChnPort_t *, MI_SYS_ChnPort_t *, MI_U32, MI_U32, i6_sys_link, MI_U32);
    MI_S32 (*MI_SYS_UnBindChnPort)(MI_SYS_ChnPort_t *, MI_SYS_ChnPort_t *);
    MI_S32 (*MI_SYS_SetChnOutputPortDepth)(MI_SYS_ChnPort_t *, MI_U32, MI_U32);
    MI_S32 (*MI_SYS_ChnOutputPortGetBuf)(MI_SYS_ChnPort_t *, MI_SYS_BufInfo_t *, MI_SYS_BUF_HANDLE *, MI_S32);
    MI_S32 (*MI_SYS_ChnOutputPortPutBuf)(MI_SYS_BUF_HANDLE);
    MI_S32 (*MI_SNR_SetPlaneMode)(MI_U32, MI_U8);
    MI_S32 (*MI_SNR_QueryResCount)(MI_U32, MI_U32 *);
    MI_S32 (*MI_SNR_GetRes)(MI_U32, MI_U8, i6_snr_res *);
    MI_S32 (*MI_SNR_SetRes)(MI_U32, MI_U8);
    MI_S32 (*MI_SNR_SetFps)(MI_U32, MI_U32);
    MI_S32 (*MI_SNR_SetOrien)(MI_U32, MI_U8, MI_U8);
    MI_S32 (*MI_SNR_GetPadInfo)(MI_U32, i6_snr_pad *);
    MI_S32 (*MI_SNR_GetPlaneInfo)(MI_U32, MI_U32, i6_snr_plane *);
    MI_S32 (*MI_SNR_Enable)(MI_U32);
    MI_S32 (*MI_SNR_Disable)(MI_U32);
    MI_S32 (*MI_VIF_SetDevAttr)(MI_S32, i6_vif_dev *);
    MI_S32 (*MI_VIF_EnableDev)(MI_S32);
    MI_S32 (*MI_VIF_DisableDev)(MI_S32);
    MI_S32 (*MI_VIF_SetChnPortAttr)(MI_S32, MI_S32, i6_vif_port *);
    MI_S32 (*MI_VIF_EnableChnPort)(MI_S32, MI_S32);
    MI_S32 (*MI_VIF_DisableChnPort)(MI_S32, MI_S32);
    MI_S32 (*MI_VPE_CreateChannel)(MI_S32, void *);
    MI_S32 (*MI_VPE_DestroyChannel)(MI_S32);
    MI_S32 (*MI_VPE_SetChannelParam)(MI_S32, void *);
    MI_S32 (*MI_VPE_StartChannel)(MI_S32);
    MI_S32 (*MI_VPE_StopChannel)(MI_S32);
    MI_S32 (*MI_VPE_SetPortMode)(MI_S32, MI_S32, i6_vpe_port *);
    MI_S32 (*MI_VPE_EnablePort)(MI_S32, MI_S32);
    MI_S32 (*MI_VPE_DisablePort)(MI_S32, MI_S32);
    MI_S32 (*MI_ISP_API_CmdLoadBinFile)(MI_S32, char *, MI_U32);
    MI_S32 (*MI_ISP_AE_GetExposureLimit)(MI_S32, i6_isp_exp *);
    MI_S32 (*MI_ISP_AE_SetExposureLimit)(MI_S32, i6_isp_exp *);
    MI_S32 (*MI_VENC_CreateChn)(MI_S32, MI_VENC_ChnAttr_t *);
    MI_S32 (*MI_VENC_DestroyChn)(MI_S32);
    MI_S32 (*MI_VENC_StartRecvPic)(MI_S32);
    MI_S32 (*MI_VENC_StopRecvPic)(MI_S32);
    MI_S32 (*MI_VENC_GetChnDevid)(MI_S32, MI_U32 *);
    MI_S32 (*MI_VENC_Query)(MI_S32, MI_VENC_ChnStat_t *);
    MI_S32 (*MI_VENC_GetStream)(MI_S32, MI_VENC_Stream_t *, MI_U32);
    MI_S32 (*MI_VENC_ReleaseStream)(MI_S32, MI_VENC_Stream_t *);
} mi_camera_libs_t;
