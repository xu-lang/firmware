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
 * enough to create a VENC channel and inject NV12 frames through
 * MI_SYS_ChnInputPortGetBuf()/MI_SYS_ChnInputPortPutBuf(), but some structure
 * layouts contain opaque padding and are still open to dispute. Re-check these
 * definitions against the vendor SDK before using them as a stable ABI.
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
} mi_libs_t;
