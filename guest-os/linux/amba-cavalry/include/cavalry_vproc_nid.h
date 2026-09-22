/*
 * cavalry_vproc_nid.h
 *
 * Copyright (C) 2026, Ambarella International LLC
 */

#ifndef _CAVALRY_VPROC_NID_H_
#define _CAVALRY_VPROC_NID_H_

typedef enum {
    CAVALRY_VPROC_NID = 0x01000000,    /*!< start of enum, do not modify */

    CAVALRY_VPROC_NID_RESIZE,                               /*!< 0x01000001 */
    CAVALRY_VPROC_NID_RESIZE_ITL,                           /*!< 0x01000002 */
    CAVALRY_VPROC_NID_CVT_YUV420_RGB,                       /*!< 0x01000003 */
    CAVALRY_VPROC_NID_SUB_MEAN,                             /*!< 0x01000004 */
    CAVALRY_VPROC_NID_SCALE,                                /*!< 0x01000005 */
    CAVALRY_VPROC_NID_DTCVT,                                /*!< 0x01000006 */
    CAVALRY_VPROC_NID_ROTATE,                               /*!< 0x01000007 */
    CAVALRY_VPROC_NID_IMCVT,                                /*!< 0x01000008 */
    CAVALRY_VPROC_NID_YUV2RGB_RESIZE,                       /*!< 0x01000009 */
    CAVALRY_VPROC_NID_HARRIS,                               /*!< 0x0100000A */
    CAVALRY_VPROC_NID_CVFILTER,                             /*!< 0x0100000B */
    CAVALRY_VPROC_NID_LP_FILTER,                            /*!< 0x0100000C */
    CAVALRY_VPROC_NID_MORPH,                                /*!< 0x0100000D */
    CAVALRY_VPROC_NID_PERSPECT_LWM,                         /*!< 0x0100000E */
    CAVALRY_VPROC_NID_PERSPECT_WRAP,                        /*!< 0x0100000F */
    CAVALRY_VPROC_NID_OPTLK,                                /*!< 0x01000010 */
    CAVALRY_VPROC_NID_CDIST,                                /*!< 0x01000011 */
    CAVALRY_VPROC_NID_IMHIST,                               /*!< 0x01000012 */
    CAVALRY_VPROC_NID_BW,                                   /*!< 0x01000013 */
    CAVALRY_VPROC_NID_EPNR,                                 /*!< 0x01000014 */
    CAVALRY_VPROC_NID_CCLB,                                 /*!< 0x01000015 */
    CAVALRY_VPROC_NID_BAYER2BGR,                            /*!< 0x01000016 */
    CAVALRY_VPROC_NID_MEMCPY,                               /*!< 0x01000017 */
    CAVALRY_VPROC_NID_MEMSET,                               /*!< 0x01000018 */
    CAVALRY_VPROC_NID_DSI_SPLIT,                            /*!< 0x01000019 */
    CAVALRY_VPROC_NID_WARP,                                 /*!< 0x0100001A */
    CAVALRY_VPROC_NID_GEN_WP_FIELD,                         /*!< 0x0100001B */
    CAVALRY_VPROC_NID_STATE,                                /*!< 0x0100001C */
    CAVALRY_VPROC_NID_DSI_FUSION_2SCALES_FFOV,              /*!< 0x0100001D */
    CAVALRY_VPROC_NID_DSI_FUSION_SCALE0_WITH_SHIFTED,       /*!< 0x0100001E */
    CAVALRY_VPROC_NID_DSI_FUSION_FSCALE0_WITH_SCALE2_FFOV,  /*!< 0x0100001F */
    CAVALRY_VPROC_NID_MERGE_UV,                             /*!< 0x01000020 */
    CAVALRY_VPROC_NID_SPLIT_UV,                             /*!< 0x01000021 */
    CAVALRY_VPROC_NID_ALPHA_BLEND,                          /*!< 0x01000022 */
    CAVALRY_VPROC_NID_CVT_RGB_YUV420,                       /*!< 0x01000023 */
    CAVALRY_VPROC_NID_TRANSPOSE,                            /*!< 0x01000024 */
    CAVALRY_VPROC_NID_ABS,                                  /*!< 0x01000025 */
    CAVALRY_VPROC_NID_FLATTEN,                              /*!< 0x01000026 */
    CAVALRY_VPROC_NID_BITWISE,                              /*!< 0x01000027 */
    CAVALRY_VPROC_NID_DSI_TO_POINTCLOUD,                    /*!< 0x01000028 */
    CAVALRY_VPROC_NID_DSI_TO_DEPTH,                         /*!< 0x01000029 */
    CAVALRY_VPROC_NID_DFILTER,                              /*!< 0x0100002A */
    CAVALRY_VPROC_NID_XSOBEL,                               /*!< 0x0100002B */
    CAVALRY_VPROC_NID_FBM,                                  /*!< 0x0100002C */
    CAVALRY_VPROC_NID_RENDER3D,                             /*!< 0x0100002D */
    CAVALRY_VPROC_NID_DSI_FUSION_MULTI_SCALE,               /*!< 0x0100002E */
    CAVALRY_VPROC_NID_DSI_FUSION_UPS0_SFT0_S2,              /*!< 0x0100002F */
    CAVALRY_VPROC_NID_CROP,                                 /*!< 0x01000030 */
    CAVALRY_VPROC_NID_END           /*!< the end of enum, do not modify */
} cavalry_vproc_nid_t;

#endif
