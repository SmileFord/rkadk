/*
 * Copyright (c) 2021 Rockchip, Inc. All Rights Reserved.
 *
 *  Licensed under the Apache License, Version 2.0 (the "License");
 *  you may not use this file except in compliance with the License.
 *  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include "rkadk_photo.h"
#include "rkadk_log.h"
#include "rkadk_media_comm.h"
#include "rkadk_param.h"
#include "rkadk_thumb_comm.h"
#include "rkadk_signal.h"
#include <assert.h>
#include <malloc.h>
#include <pthread.h>
#include <rga/RgaApi.h>
#include <rga/rga.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#include <utime.h>

#define JPG_THM_FIND_NUM_MAX 50
#define JPG_EXIF_FLAG_LEN 6
#define JPG_DIRECTORY_ENTRY_LEN 12
#define JPG_DE_TYPE_COUNT 12
#define JPG_MP_FLAG_LEN 4
#define JPG_MP_ENTRY_LEN 16
#define JPG_THUMB_TAG_LEN 4

#define VDEC_CHN_THM 0
#define VDEC_CHN_GET_DATA 1

#define JPG_MMAP_ONE_FILE_PATH "/tmp/.mmap_1.jpeg"
#define JPG_MMAP_TWO_FILE_PATH "/tmp/.mmap_2.jpeg"

typedef enum {
  RKADK_JPG_LITTLE_ENDIAN, // II
  RKADK_JPG_BIG_ENDIAN,    // MM
  RKADK_JPG_BYTE_ORDER_BUTT
} RKADK_JPG_BYTE_ORDER_E;

typedef struct {
  RKADK_U16 u16Type;
  RKADK_U16 u16TypeByte;
} RKADK_JPG_DE_TYPE_S;

typedef struct {
  bool binit;
  RKADK_U32 u32CamId;
  RKADK_PHOTO_DATA_RECV_FN_PTR pDataRecvFn;
  pthread_t tid;
  bool bGetJpeg;
  pthread_t thumb_tid;
  bool bGetThumbJpeg;
  RKADK_U32 u32PhotoCnt;
  RKADK_U8 *pJpegData;
  RKADK_U32 u32JpegLen;
  RKADK_U32 u32MmapLen;
  void *pSignal;
} RKADK_PHOTO_HANDLE_S;

static RKADK_PHOTO_HANDLE_S g_stPhotoHandle[RKADK_MAX_SENSOR_CNT] = {0};

static RKADK_JPG_DE_TYPE_S g_stJpgDEType[JPG_DE_TYPE_COUNT] = {
    {1, 1}, {2, 1}, {3, 3}, {4, 4},  {5, 8},  {6, 1},
    {7, 1}, {8, 2}, {9, 3}, {10, 8}, {11, 4}, {12, 8}};

static RKADK_U8 *RKADK_PHOTO_Mmap(RKADK_CHAR *FileName, RKADK_U32 u32PhotoLen) {
  char data = 0xff;
  FILE *fd = NULL;
  RKADK_U8 *pu8Photo = NULL;

  fd = fopen(FileName, "w+");
  if (!fd) {
    RKADK_LOGE("open %s failed", FileName);
    return NULL;
  }

  if (fseek(fd, u32PhotoLen, SEEK_SET)) {
    fclose(fd);
    RKADK_LOGE("seek %s failed", FileName);
    return NULL;
  }
  fwrite(&data, 1, 1, fd);

  pu8Photo = (RKADK_U8 *)mmap(NULL, u32PhotoLen, PROT_READ | PROT_WRITE, MAP_SHARED, fileno(fd), 0);
  if (pu8Photo == MAP_FAILED) {
    fclose(fd);
    RKADK_LOGE("Mmap %s, errno: %d",FileName, errno);
    return NULL;
  }
  fclose(fd);

  return pu8Photo;
}

static void *RKADK_PHOTO_GetJpeg(void *params) {
  int ret;
  VENC_STREAM_S stFrame;
  VENC_PACK_S stPack;
  RKADK_PHOTO_RECV_DATA_S stData;

  RKADK_PHOTO_HANDLE_S *pHandle = (RKADK_PHOTO_HANDLE_S *)params;
  if (!pHandle) {
    RKADK_LOGE("Get jpeg thread invalid param");
    return NULL;
  }

  if (!pHandle->pDataRecvFn) {
    RKADK_LOGE("u32CamId[%d] don't register callback", pHandle->u32CamId);
    return NULL;
  }

  RKADK_PARAM_PHOTO_CFG_S *pstPhotoCfg =
      RKADK_PARAM_GetPhotoCfg(pHandle->u32CamId);
  if (!pstPhotoCfg) {
    RKADK_LOGE("RKADK_PARAM_GetPhotoCfg failed");
    return NULL;
  }

  stFrame.pstPack = &stPack;
  // drop first frame
  ret = RK_MPI_VENC_GetStream(pstPhotoCfg->venc_chn, &stFrame, 1000);
  if (ret == RK_SUCCESS)
    RK_MPI_VENC_ReleaseStream(pstPhotoCfg->venc_chn, &stFrame);
  else
    RKADK_LOGE("RK_MPI_VENC_GetStream timeout %x\n", ret);

  while (pHandle->bGetJpeg) {
    ret = RK_MPI_VENC_GetStream(pstPhotoCfg->venc_chn, &stFrame, 1000);
    if (ret == RK_SUCCESS) {
      if (pHandle->pSignal) {
        memcpy(pHandle->pJpegData,
              (RKADK_U8 *)RK_MPI_MB_Handle2VirAddr(stFrame.pstPack->pMbBlk),
              stFrame.pstPack->u32Len);
        pHandle->u32JpegLen = stFrame.pstPack->u32Len;
        RKADK_SIGNAL_Give(pHandle->pSignal);
        RKADK_LOGI("Photo success, seq = %d, len = %d", stFrame.u32Seq, pHandle->u32JpegLen);
      } else {
        if (pHandle->u32PhotoCnt) {
          memset(&stData, 0, sizeof(RKADK_PHOTO_RECV_DATA_S));
          stData.pu8DataBuf =
              (RKADK_U8 *)RK_MPI_MB_Handle2VirAddr(stFrame.pstPack->pMbBlk);
          stData.u32DataLen = stFrame.pstPack->u32Len;
          stData.u32CamId = pHandle->u32CamId;
          pHandle->pDataRecvFn(stData.pu8DataBuf, stData.u32DataLen,
                              stData.u32CamId);
          pHandle->u32PhotoCnt -= 1;
        }
      }

      ret = RK_MPI_VENC_ReleaseStream(pstPhotoCfg->venc_chn, &stFrame);
      if (ret != RK_SUCCESS)
        RKADK_LOGE("RK_MPI_VENC_ReleaseStream failed[%x]", ret);
    }
  }

  RKADK_LOGD("Exit get jpeg thread");
  return NULL;
}

static void *RKADK_PHOTO_GetThumbJpeg(void *params) {
  int ret;
  VENC_STREAM_S stThumbFrame;
  VENC_PACK_S stPack;
  RKADK_PHOTO_RECV_DATA_S stData;
  RKADK_U32 u32PhotoLen;

  RKADK_PHOTO_HANDLE_S *pHandle = (RKADK_PHOTO_HANDLE_S *)params;
  if (!pHandle) {
    RKADK_LOGE("Get jpeg thread invalid param");
    return NULL;
  }

  if (!pHandle->pDataRecvFn) {
    RKADK_LOGE("u32CamId[%d] don't register callback", pHandle->u32CamId);
    return NULL;
  }

  RKADK_PARAM_THUMB_CFG_S *ptsThumbCfg = RKADK_PARAM_GetThumbCfg();
  if (!ptsThumbCfg) {
    RKADK_LOGE("RKADK_PARAM_GetThumbCfg failed");
    return NULL;
  }

  RKADK_PARAM_SENSOR_CFG_S *pstSensorCfg =
    RKADK_PARAM_GetSensorCfg(pHandle->u32CamId);
  if (!pstSensorCfg) {
    RKADK_LOGE("RKADK_PARAM_GetSensorCfg failed");
    return NULL;
  }

  stThumbFrame.pstPack = &stPack;

  u32PhotoLen = pstSensorCfg->max_width *
                pstSensorCfg->max_height * 3 / 2;

  RKADK_U8 *pu8Photo = RKADK_PHOTO_Mmap(JPG_MMAP_TWO_FILE_PATH, u32PhotoLen);
  if (!pu8Photo)
    return NULL;

    // drop first frame
  ret = RK_MPI_VENC_GetStream(ptsThumbCfg->venc_chn, &stThumbFrame, 1000);
  if (ret == RK_SUCCESS)
    RK_MPI_VENC_ReleaseStream(ptsThumbCfg->venc_chn, &stThumbFrame);
  else
    RKADK_LOGE("RK_MPI_VENC_GetStream timeout %x\n", ret);

  while (pHandle->bGetThumbJpeg) {
    ret = RK_MPI_VENC_GetStream(ptsThumbCfg->venc_chn, &stThumbFrame, 50);
    if (ret == RK_SUCCESS) {
      ret = RKADK_SIGNAL_Wait(pHandle->pSignal, 2000);/* Lapse video record one frame per second */
      if (ret == 0) {
        if (pHandle->u32PhotoCnt) {
          memset(&stData, 0, sizeof(RKADK_PHOTO_RECV_DATA_S));
          stData.u32DataLen = ThumbnailPhotoData(pHandle->pJpegData, pHandle->u32JpegLen, stThumbFrame, pu8Photo);
          stData.pu8DataBuf = pu8Photo;
          stData.u32CamId = pHandle->u32CamId;
          pHandle->pDataRecvFn(stData.pu8DataBuf, stData.u32DataLen,
                                stData.u32CamId);
          pHandle->u32PhotoCnt -= 1;
        }
      } else {
        RKADK_LOGE("Lose jpeg data, take photo failed");
      }

      ret = RK_MPI_VENC_ReleaseStream(ptsThumbCfg->venc_chn, &stThumbFrame);
      if (ret != RK_SUCCESS)
        RKADK_LOGE("RK_MPI_VENC_ReleaseStream failed[%x]", ret);
    }
  }

  if (pu8Photo)
    munmap(pu8Photo, u32PhotoLen);

  RKADK_LOGD("Exit get thumbnail jpeg thread");
  return NULL;
}

static void RKADK_PHOTO_SetVencAttr(RKADK_PHOTO_THUMB_ATTR_S stThumbAttr,
                                    RKADK_PARAM_PHOTO_CFG_S *pstPhotoCfg,
                                    RKADK_PARAM_SENSOR_CFG_S *pstSensorCfg,
                                    VENC_CHN_ATTR_S *pstVencAttr) {
  VENC_ATTR_JPEG_S *pstAttrJpege = &(pstVencAttr->stVencAttr.stAttrJpege);

  memset(pstVencAttr, 0, sizeof(VENC_CHN_ATTR_S));
  pstVencAttr->stVencAttr.enType = RK_VIDEO_ID_JPEG;
  pstVencAttr->stVencAttr.enPixelFormat =
      pstPhotoCfg->vi_attr.stChnAttr.enPixelFormat;
  pstVencAttr->stVencAttr.u32MaxPicWidth = pstSensorCfg->max_width;
  pstVencAttr->stVencAttr.u32MaxPicHeight = pstSensorCfg->max_height;
  pstVencAttr->stVencAttr.u32PicWidth = pstPhotoCfg->image_width;
  pstVencAttr->stVencAttr.u32PicHeight = pstPhotoCfg->image_height;
  pstVencAttr->stVencAttr.u32VirWidth = pstPhotoCfg->image_width;
  pstVencAttr->stVencAttr.u32VirHeight = pstPhotoCfg->image_height;
  pstVencAttr->stVencAttr.u32StreamBufCnt = 1;
  pstVencAttr->stVencAttr.u32BufSize =
      pstSensorCfg->max_width * pstSensorCfg->max_height / 2;

  pstAttrJpege->bSupportDCF = (RK_BOOL)stThumbAttr.bSupportDCF;
  pstAttrJpege->stMPFCfg.u8LargeThumbNailNum =
      stThumbAttr.stMPFAttr.sCfg.u8LargeThumbNum;
  if (pstAttrJpege->stMPFCfg.u8LargeThumbNailNum >
      RKADK_MPF_LARGE_THUMB_NUM_MAX)
    pstAttrJpege->stMPFCfg.u8LargeThumbNailNum = RKADK_MPF_LARGE_THUMB_NUM_MAX;

  switch (stThumbAttr.stMPFAttr.eMode) {
  case RKADK_PHOTO_MPF_SINGLE:
    pstAttrJpege->enReceiveMode = VENC_PIC_RECEIVE_SINGLE;
    pstAttrJpege->stMPFCfg.astLargeThumbNailSize[0].u32Width =
        UPALIGNTO(stThumbAttr.stMPFAttr.sCfg.astLargeThumbSize[0].u32Width, 4);
    pstAttrJpege->stMPFCfg.astLargeThumbNailSize[0].u32Height =
        UPALIGNTO(stThumbAttr.stMPFAttr.sCfg.astLargeThumbSize[0].u32Height, 2);
    break;
  case RKADK_PHOTO_MPF_MULTI:
    pstAttrJpege->enReceiveMode = VENC_PIC_RECEIVE_MULTI;
    for (int i = 0; i < pstAttrJpege->stMPFCfg.u8LargeThumbNailNum; i++) {
      pstAttrJpege->stMPFCfg.astLargeThumbNailSize[i].u32Width = UPALIGNTO(
          stThumbAttr.stMPFAttr.sCfg.astLargeThumbSize[i].u32Width, 4);
      pstAttrJpege->stMPFCfg.astLargeThumbNailSize[i].u32Height = UPALIGNTO(
          stThumbAttr.stMPFAttr.sCfg.astLargeThumbSize[i].u32Height, 2);
    }
    break;
  default:
    pstAttrJpege->enReceiveMode = VENC_PIC_RECEIVE_BUTT;
    break;
  }
}

static void RKADK_PHOTO_CreateVencCombo(RKADK_S32 s32ChnId,
                                        VENC_CHN_ATTR_S *pstVencChnAttr,
                                        RKADK_PARAM_PHOTO_CFG_S *pstPhotoCfg) {
  VENC_RECV_PIC_PARAM_S stRecvParam;
  VENC_CHN_BUF_WRAP_S stVencChnBufWrap;
  VENC_CHN_REF_BUF_SHARE_S stVencChnRefBufShare;
  VENC_COMBO_ATTR_S stComboAttr;
  VENC_JPEG_PARAM_S stJpegParam;
  memset(&stRecvParam, 0, sizeof(VENC_RECV_PIC_PARAM_S));
  memset(&stVencChnBufWrap, 0, sizeof(VENC_CHN_BUF_WRAP_S));
  memset(&stVencChnRefBufShare, 0, sizeof(VENC_CHN_REF_BUF_SHARE_S));
  memset(&stComboAttr, 0, sizeof(VENC_COMBO_ATTR_S));
  memset(&stJpegParam, 0, sizeof(stJpegParam));

  RK_MPI_VENC_CreateChn(s32ChnId, pstVencChnAttr);

  stVencChnBufWrap.bEnable = RK_TRUE;
  RK_MPI_VENC_SetChnBufWrapAttr(s32ChnId, &stVencChnBufWrap);

  stVencChnRefBufShare.bEnable = RK_TRUE;
  RK_MPI_VENC_SetChnRefBufShareAttr(s32ChnId, &stVencChnRefBufShare);

  stRecvParam.s32RecvPicNum = 1;
  RK_MPI_VENC_StartRecvFrame(s32ChnId, &stRecvParam);

  stComboAttr.bEnable = RK_TRUE;
  stComboAttr.s32ChnId = pstPhotoCfg->combo_venc_chn;
  RK_MPI_VENC_SetComboAttr(s32ChnId, &stComboAttr);

  stJpegParam.u32Qfactor = pstPhotoCfg->qfactor;
  RK_MPI_VENC_SetJpegParam(s32ChnId, &stJpegParam);

  RK_MPI_VENC_StopRecvFrame(s32ChnId);
}

static void RKADK_PHOTO_SetChn(RKADK_PARAM_PHOTO_CFG_S *pstPhotoCfg,
                               RKADK_U32 u32CamId, MPP_CHN_S *pstViChn,
                               MPP_CHN_S *pstVencChn, MPP_CHN_S *pstRgaChn) {
  pstViChn->enModId = RK_ID_VI;
  pstViChn->s32DevId = u32CamId;
  pstViChn->s32ChnId = pstPhotoCfg->vi_attr.u32ViChn;

  pstRgaChn->enModId = RK_ID_VPSS;
  pstRgaChn->s32DevId = u32CamId;
  pstRgaChn->s32ChnId = pstPhotoCfg->rga_chn;

  pstVencChn->enModId = RK_ID_VENC;
  pstVencChn->s32DevId = u32CamId;
  pstVencChn->s32ChnId = pstPhotoCfg->venc_chn;
}

static bool RKADK_PHOTO_IsUseRga(RKADK_PARAM_PHOTO_CFG_S *pstPhotoCfg) {
  RKADK_U32 u32ViWidth = pstPhotoCfg->vi_attr.stChnAttr.stSize.u32Width;
  RKADK_U32 u32ViHeight = pstPhotoCfg->vi_attr.stChnAttr.stSize.u32Height;

  if (pstPhotoCfg->image_width == u32ViWidth &&
      pstPhotoCfg->image_height == u32ViHeight) {
    return false;
  } else {
    RKADK_LOGD("In[%d, %d], Out[%d, %d]", u32ViWidth, u32ViHeight,
               pstPhotoCfg->image_width, pstPhotoCfg->image_height);
    return true;
  }
}

static void RKADK_PHOTO_ResetAttr(RKADK_PARAM_SENSOR_CFG_S *pstSensorCfg,
                                  RKADK_PARAM_PHOTO_CFG_S *pstPhotoCfg,
                                  VENC_CHN_ATTR_S *pstPhoAttr,
                                  VI_CHN_ATTR_S *pstChnAttr) {
  pstPhoAttr->stVencAttr.u32PicWidth = pstPhotoCfg->image_width;
  pstPhoAttr->stVencAttr.u32PicHeight = pstPhotoCfg->image_height;
  pstPhoAttr->stVencAttr.u32VirWidth = pstPhotoCfg->image_width;
  pstPhoAttr->stVencAttr.u32VirHeight = pstPhotoCfg->image_height;

  pstChnAttr->stIspOpt.stMaxSize.u32Width = pstSensorCfg->max_width;
  pstChnAttr->stIspOpt.stMaxSize.u32Height = pstSensorCfg->max_height;
  pstChnAttr->stSize.u32Width = pstPhotoCfg->image_width;
  pstChnAttr->stSize.u32Height = pstPhotoCfg->image_height;
}

RKADK_S32 RKADK_PHOTO_Init(RKADK_PHOTO_ATTR_S *pstPhotoAttr) {
  int ret;
  bool bSysInit = false;
  char name[256];
  MPP_CHN_S stViChn, stVencChn, stRgaChn;
  VENC_CHN_ATTR_S stVencAttr;
  RKADK_PARAM_THUMB_CFG_S *ptsThumbCfg = NULL;
  RKADK_PARAM_SENSOR_CFG_S *pstSensorCfg = NULL;
  bool bUseRga = false;
  VPSS_GRP_ATTR_S stGrpAttr;
  VPSS_CHN_ATTR_S stChnAttr;
  RKADK_S32 s32VpssGrp = 0;

  RKADK_CHECK_POINTER(pstPhotoAttr, RKADK_FAILURE);
  RKADK_CHECK_CAMERAID(pstPhotoAttr->u32CamID, RKADK_FAILURE);

  RKADK_LOGI("Photo[%d] Init...", pstPhotoAttr->u32CamID);

  RKADK_PHOTO_HANDLE_S *pHandle = &g_stPhotoHandle[pstPhotoAttr->u32CamID];
  if (pHandle->binit) {
    RKADK_LOGE("Photo: camera[%d] has been init", pstPhotoAttr->u32CamID);
    return 0;
  }

  memset(pHandle, 0, sizeof(RKADK_PHOTO_HANDLE_S));

  pHandle->u32CamId = pstPhotoAttr->u32CamID;
  pHandle->pDataRecvFn = pstPhotoAttr->pfnPhotoDataProc;

  RKADK_PARAM_Init(NULL, NULL);
  RKADK_PARAM_PHOTO_CFG_S *pstPhotoCfg =
      RKADK_PARAM_GetPhotoCfg(pstPhotoAttr->u32CamID);
  if (!pstPhotoCfg) {
    RKADK_LOGE("RKADK_PARAM_GetPhotoCfg failed");
    return -1;
  }

  ptsThumbCfg = RKADK_PARAM_GetThumbCfg();
  if (!ptsThumbCfg) {
    RKADK_LOGE("RKADK_PARAM_GetThumbCfg failed");
    return -1;
  }

  pstSensorCfg = RKADK_PARAM_GetSensorCfg(pstPhotoAttr->u32CamID);
  if (!pstSensorCfg) {
    RKADK_LOGE("RKADK_PARAM_GetSensorCfg failed");
    return -1;
  }

  bSysInit = RKADK_MPI_SYS_CHECK();
  if (!bSysInit) {
    RKADK_LOGE("System is not initialized");
    return -1;
  }

  RKADK_PHOTO_SetChn(pstPhotoCfg, pstPhotoAttr->u32CamID, &stViChn, &stVencChn,
                     &stRgaChn);
  RKADK_PHOTO_SetVencAttr(pstPhotoAttr->stThumbAttr, pstPhotoCfg,
                          pstSensorCfg, &stVencAttr);

  // Create VI
  ret = RKADK_MPI_VI_Init(pstPhotoAttr->u32CamID, stViChn.s32ChnId,
                          &(pstPhotoCfg->vi_attr.stChnAttr));
  if (ret) {
    RKADK_LOGE("RKADK_MPI_VI_Init failed, ret = %d", ret);
    return ret;
  }

  bUseRga = RKADK_PHOTO_IsUseRga(pstPhotoCfg);
  // Create RGA
  if (bUseRga) {
    memset(&stGrpAttr, 0, sizeof(VPSS_GRP_ATTR_S));
    memset(&stChnAttr, 0, sizeof(VPSS_CHN_ATTR_S));

    stGrpAttr.u32MaxW = 4096;
    stGrpAttr.u32MaxH = 4096;
    stGrpAttr.enPixelFormat = pstPhotoCfg->vi_attr.stChnAttr.enPixelFormat;
    stGrpAttr.enCompressMode = COMPRESS_MODE_NONE;
    stGrpAttr.stFrameRate.s32SrcFrameRate = -1;
    stGrpAttr.stFrameRate.s32DstFrameRate = -1;
    stChnAttr.enChnMode = VPSS_CHN_MODE_USER;
    stChnAttr.enCompressMode = COMPRESS_MODE_NONE;
    stChnAttr.enDynamicRange = DYNAMIC_RANGE_SDR8;
    stChnAttr.enPixelFormat = pstPhotoCfg->vi_attr.stChnAttr.enPixelFormat;
    stChnAttr.stFrameRate.s32SrcFrameRate = -1;
    stChnAttr.stFrameRate.s32DstFrameRate = -1;
    stChnAttr.u32Width = pstPhotoCfg->image_width;
    stChnAttr.u32Height = pstPhotoCfg->image_height;
    stChnAttr.u32Depth = 0;

    ret = RKADK_MPI_VPSS_Init(s32VpssGrp, pstPhotoCfg->rga_chn,
                              &stGrpAttr, &stChnAttr);
    if (ret) {
      RKADK_LOGE("RKADK_MPI_VPSS_Init vpssfalied[%d]",ret);
      RKADK_MPI_VI_DeInit(pstPhotoAttr->u32CamID, pstPhotoCfg->vi_attr.u32ViChn);
      RKADK_MPI_VPSS_DeInit(s32VpssGrp, pstPhotoCfg->rga_chn);
      return ret;
    }
  }

  // Create VENC
  if (pstPhotoCfg->enable_combo) {
    RKADK_LOGE("Select combo mode");
    RKADK_PHOTO_CreateVencCombo(stVencChn.s32ChnId, &stVencAttr,
                                pstPhotoCfg);
  } else {
    ret = RK_MPI_VENC_CreateChn(stVencChn.s32ChnId, &stVencAttr);
    if (ret) {
      RKADK_LOGE("Create Venc failed[%x]", ret);
      goto failed;
    }

    VENC_CHN_REF_BUF_SHARE_S stVencChnRefBufShare;
    memset(&stVencChnRefBufShare, 0, sizeof(VENC_CHN_REF_BUF_SHARE_S));
    stVencChnRefBufShare.bEnable = RK_TRUE;
    RK_MPI_VENC_SetChnRefBufShareAttr(stVencChn.s32ChnId, &stVencChnRefBufShare);

    VENC_JPEG_PARAM_S stJpegParam;
    memset(&stJpegParam, 0, sizeof(VENC_JPEG_PARAM_S));
    stJpegParam.u32Qfactor = pstPhotoCfg->qfactor;
    RK_MPI_VENC_SetJpegParam(stVencChn.s32ChnId, &stJpegParam);

    // must, for no streams callback running failed
    VENC_RECV_PIC_PARAM_S stRecvParam;
    stRecvParam.s32RecvPicNum = 1;
    ret = RK_MPI_VENC_StartRecvFrame(stVencChn.s32ChnId, &stRecvParam);
    if (ret) {
      RKADK_LOGE("RK_MPI_VENC_StartRecvFrame failed[%x]", ret);
      goto failed;
    }
    RK_MPI_VENC_StopRecvFrame(stVencChn.s32ChnId);
  }

  pHandle->bGetJpeg = true;
  ret = pthread_create(&pHandle->tid, NULL, RKADK_PHOTO_GetJpeg, pHandle);
  if (ret) {
    RKADK_LOGE("Create get jpg(%d) thread failed [%d]", pstPhotoAttr->u32CamID,
               ret);
    goto failed;
  }
  snprintf(name, sizeof(name), "PhotoGetJpeg_%d", stVencChn.s32ChnId);
  pthread_setname_np(pHandle->tid, name);

  ret = ThumbnailInit(pstPhotoAttr->u32CamID, ptsThumbCfg->thumb_width,
                      ptsThumbCfg->thumb_height, ptsThumbCfg->venc_chn,
                      ptsThumbCfg->vi_chn);
  if (ret) {
    RKADK_LOGI("Thumbnail venc [%d] Init fail [%d]",
                ptsThumbCfg->venc_chn, ret);
    goto failed;
  }
  ThumbnailChnBind(stVencChn.s32ChnId, ptsThumbCfg->venc_chn);

  pHandle->u32MmapLen = pstPhotoCfg->image_width *
                        pstPhotoCfg->image_height * 3 / 2;
  pHandle->pSignal = RKADK_SIGNAL_Create(0, 1);
  pHandle->bGetThumbJpeg = true;
  pHandle->pJpegData = RKADK_PHOTO_Mmap(JPG_MMAP_ONE_FILE_PATH, pHandle->u32MmapLen);
  if (!pHandle->pJpegData) {
    ThumbnailDeInit(pstPhotoAttr->u32CamID,
                    ptsThumbCfg->venc_chn,
                    ptsThumbCfg->vi_chn);
    goto failed;
  }

  ret = pthread_create(&pHandle->thumb_tid, NULL, RKADK_PHOTO_GetThumbJpeg, pHandle);
  if (ret) {
    RKADK_LOGE("Create get thumbnail jpg(%d) thread failed [%d]", pstPhotoAttr->u32CamID,
              ret);
    ThumbnailDeInit(pstPhotoAttr->u32CamID,
                    ptsThumbCfg->venc_chn,
                    ptsThumbCfg->vi_chn);
    goto failed;
  }
  memset(name, 0, sizeof(name));
  snprintf(name, sizeof(name), "PhotoThumbJpeg_%d", ptsThumbCfg->venc_chn);
  pthread_setname_np(pHandle->thumb_tid, name);

  if (bUseRga) {
    // RGA Bind VENC
    ret = RKADK_MPI_SYS_Bind(&stRgaChn, &stVencChn);
    if (ret) {
      RKADK_LOGE("Bind RGA[%d] to VENC[%d] failed[%x]", stRgaChn.s32ChnId,
                 stVencChn.s32ChnId, ret);
      goto failed;
    }

    // VI Bind RGA
    ret = RKADK_MPI_SYS_Bind(&stViChn, &stRgaChn);
    if (ret) {
      RKADK_LOGE("Bind VI[%d] to RGA[%d] failed[%x]", stViChn.s32ChnId,
                 stRgaChn.s32ChnId, ret);
      RKADK_MPI_SYS_UnBind(&stRgaChn, &stVencChn);
      goto failed;
    }
  } else {
    // VI Bind VENC
    if (!pstPhotoCfg->enable_combo) {
      ret = RK_MPI_SYS_Bind(&stViChn, &stVencChn);
      if (ret) {
        RKADK_LOGE("Bind VI[%d] to VENC[%d] failed[%x]", stViChn.s32ChnId,
                    stVencChn.s32ChnId, ret);
        goto failed;
      }
    }
  }

  pHandle->binit = true;
  RKADK_LOGI("Photo[%d] Init End...", pstPhotoAttr->u32CamID);
  return 0;

failed:
  RKADK_LOGE("failed");
  RK_MPI_VENC_DestroyChn(stVencChn.s32ChnId);

  pHandle->bGetJpeg = false;
  if (pHandle->tid) {
    ret = pthread_join(pHandle->tid, NULL);
    if (ret)
      RKADK_LOGE("Exit get jpeg thread failed!");
    pHandle->tid = 0;
  }

  pHandle->bGetThumbJpeg = false;
  if (pHandle->pSignal) {
    RKADK_SIGNAL_Give(pHandle->pSignal);
    RKADK_SIGNAL_Destroy(pHandle->pSignal);
  }

  if (pHandle->thumb_tid) {
    ret = pthread_join(pHandle->thumb_tid, NULL);
    if (ret)
      RKADK_LOGE("Exit get thumbnail jpeg thread failed!");
    pHandle->thumb_tid = 0;
  }

  if (pHandle->pJpegData) {
    munmap(pHandle->pJpegData, pHandle->u32MmapLen);
    pHandle->pJpegData = NULL;
  }

  if (bUseRga)
    RKADK_MPI_VPSS_DeInit(s32VpssGrp, pstPhotoCfg->rga_chn);

  RKADK_MPI_VI_DeInit(pstPhotoAttr->u32CamID, stViChn.s32ChnId);
  return ret;
}

RKADK_S32 RKADK_PHOTO_DeInit(RKADK_U32 u32CamId) {
  int ret;
  MPP_CHN_S stViChn, stVencChn, stRgaChn;

  bool bUseRga = false;
  RKADK_S32 s32VpssGrp = 0;

  RKADK_PHOTO_HANDLE_S *pHandle = &g_stPhotoHandle[u32CamId];
  if (!pHandle->binit) {
    RKADK_LOGE("Photo: camera[%d] has been deinit", pHandle->u32CamId);
    return 0;
  }

  RKADK_CHECK_CAMERAID(pHandle->u32CamId, RKADK_FAILURE);
  RKADK_LOGI("Photo[%d] DeInit...", pHandle->u32CamId);

  RKADK_PARAM_PHOTO_CFG_S *pstPhotoCfg =
      RKADK_PARAM_GetPhotoCfg(pHandle->u32CamId);
  if (!pstPhotoCfg) {
    RKADK_LOGE("RKADK_PARAM_GetPhotoCfg failed");
    return -1;
  }

  RKADK_PARAM_THUMB_CFG_S *ptsThumbCfg =
            RKADK_PARAM_GetThumbCfg();
  if (!ptsThumbCfg) {
    RKADK_LOGE("RKADK_PARAM_GetThumbCfg failed");
    return -1;
  }

  RKADK_PHOTO_SetChn(pstPhotoCfg, pHandle->u32CamId, &stViChn, &stVencChn,
                     &stRgaChn);

  if (pHandle->pSignal) {
    RKADK_SIGNAL_Give(pHandle->pSignal);
    RKADK_SIGNAL_Destroy(pHandle->pSignal);
  }

  pHandle->bGetThumbJpeg = false;
  if (pHandle->thumb_tid) {
    ret = pthread_join(pHandle->thumb_tid, NULL);
    if (ret)
      RKADK_LOGE("Exit get thumbnail jpeg thread failed!");
    pHandle->thumb_tid = 0;
  }

  ThumbnailDeInit(pHandle->u32CamId,
                  ptsThumbCfg->venc_chn,
                  ptsThumbCfg->vi_chn);
  if (pHandle->pJpegData) {
    munmap(pHandle->pJpegData, pHandle->u32MmapLen);
    pHandle->pJpegData = NULL;
  }

  pHandle->bGetJpeg = false;

#if 1
  // The current version cannot be forced to exit
  ret = RK_MPI_VENC_StopRecvFrame(stVencChn.s32ChnId);
  if (ret) {
    RKADK_LOGE("StopRecvFrame VENC[%d] failed[%d]", stVencChn.s32ChnId, ret);
    return ret;
  }
#else
  VENC_RECV_PIC_PARAM_S stRecvParam;
  stRecvParam.s32RecvPicNum = 1;
  ret = RK_MPI_VENC_StartRecvFrame(stVencChn.s32ChnId, &stRecvParam);
  if (ret) {
    RKADK_LOGE("RK_MPI_VENC_StartRecvFrame failed[%x]", ret);
    return ret;
  }
#endif

  if (pHandle->tid) {
    ret = pthread_join(pHandle->tid, NULL);
    if (ret)
      RKADK_LOGE("Exit get jpeg thread failed!");
    pHandle->tid = 0;
  }

  bUseRga = RKADK_PHOTO_IsUseRga(pstPhotoCfg);
  if (bUseRga) {
    // RGA UnBind VENC
    ret = RKADK_MPI_SYS_UnBind(&stRgaChn, &stVencChn);
    if (ret) {
      RKADK_LOGE("UnBind RGA[%d] to VENC[%d] failed[%d]", stRgaChn.s32ChnId,
                 stVencChn.s32ChnId, ret);
      return ret;
    }

    // VI UnBind RGA
    ret = RKADK_MPI_SYS_UnBind(&stViChn, &stRgaChn);
    if (ret) {
      RKADK_LOGE("UnBind VI[%d] to RGA[%d] failed[%d]", stViChn.s32ChnId,
                 stRgaChn.s32ChnId, ret);
      return ret;
    }
  } else {
    if (!pstPhotoCfg->enable_combo) {
      // VI UnBind VENC
      ret = RK_MPI_SYS_UnBind(&stViChn, &stVencChn);
      if (ret) {
        RKADK_LOGE("UnBind VI[%d] to VENC[%d] failed[%d]", stViChn.s32ChnId,
                  stVencChn.s32ChnId, ret);
        return ret;
      }
    }
  }

  // Destory VENC
  ret = RK_MPI_VENC_DestroyChn(stVencChn.s32ChnId);
  if (ret) {
    RKADK_LOGE("Destory VENC[%d] failed[%d]", stVencChn.s32ChnId, ret);
    return ret;
  }

  // Destory RGA
  if (bUseRga) {
    ret = RKADK_MPI_VPSS_DeInit(s32VpssGrp, pstPhotoCfg->rga_chn);
    if (ret) {
      RKADK_LOGE("DeInit RGA[%d] failed[%d]", pstPhotoCfg->rga_chn, ret);
      return ret;
    }
  }

  // Destory VI
  ret = RKADK_MPI_VI_DeInit(pHandle->u32CamId, stViChn.s32ChnId);
  if (ret) {
    RKADK_LOGE("RKADK_MPI_VI_DeInit failed[%d]", ret);
    return ret;
  }

  RKADK_LOGI("Photo[%d] DeInit End...", pHandle->u32CamId);

  pHandle->pDataRecvFn = NULL;
  pHandle->binit = false;
  memset(pHandle, 0, sizeof(RKADK_PHOTO_HANDLE_S));

  return 0;
}

RKADK_S32 RKADK_PHOTO_TakePhoto(RKADK_PHOTO_ATTR_S *pstPhotoAttr) {
  VENC_RECV_PIC_PARAM_S stRecvParam;

  RKADK_PHOTO_HANDLE_S *pHandle = &g_stPhotoHandle[pstPhotoAttr->u32CamID];

  RKADK_CHECK_CAMERAID(pHandle->u32CamId, RKADK_FAILURE);

  RKADK_PARAM_PHOTO_CFG_S *pstPhotoCfg =
      RKADK_PARAM_GetPhotoCfg(pHandle->u32CamId);
  if (!pstPhotoCfg) {
    RKADK_LOGE("RKADK_PARAM_GetPhotoCfg failed");
    return -1;
  }

  if (pstPhotoAttr->enPhotoType == RKADK_PHOTO_TYPE_LAPSE) {
    // TODO
    RKADK_LOGI("nonsupport photo type = %d", pstPhotoAttr->enPhotoType);
    return -1;
  }

  memset(&stRecvParam, 0, sizeof(VENC_RECV_PIC_PARAM_S));
  if (pstPhotoAttr->enPhotoType == RKADK_PHOTO_TYPE_SINGLE)
    stRecvParam.s32RecvPicNum = 1;
  else
    stRecvParam.s32RecvPicNum = pstPhotoAttr->unPhotoTypeAttr.stMultipleAttr.s32Count;

  pHandle->u32PhotoCnt = stRecvParam.s32RecvPicNum;
  RKADK_LOGI("Take photo number = %d", pHandle->u32PhotoCnt);
  return RK_MPI_VENC_StartRecvFrame(pstPhotoCfg->venc_chn, &stRecvParam);
}

RKADK_S32 RKADK_PHOTO_Reset(RKADK_U32 u32CamId) {
  int ret;
  bool bPhoto;
  RKADK_PARAM_PHOTO_CFG_S *pstPhotoCfg = NULL;
  RKADK_PARAM_SENSOR_CFG_S *pstSensorCfg = NULL;
  MPP_CHN_S stSrcChn, stPhoVenChn;
  VENC_CHN_ATTR_S stPhoAttr;
  VI_CHN_ATTR_S stChnAttr;

  RKADK_CHECK_CAMERAID(u32CamId, RKADK_FAILURE);
  RKADK_LOGI("Photo[%d] Reset start...", u32CamId);

  pstPhotoCfg = RKADK_PARAM_GetPhotoCfg(u32CamId);
  if (!pstPhotoCfg) {
    RKADK_LOGE("RKADK_PARAM_GetPhotoCfg failed");
    return -1;
  }

  pstSensorCfg = RKADK_PARAM_GetSensorCfg(u32CamId);
  if (!pstSensorCfg) {
    RKADK_LOGE("RKADK_PARAM_GetSensorCfg failed");
    return -1;
  }

  if (pstPhotoCfg->enable_combo) {
    RKADK_LOGE("Photo combo venc [%d], not support reset",
                pstPhotoCfg->combo_venc_chn);
    return -1;
  }

  memset(&stSrcChn, 0, sizeof(MPP_CHN_S));
  memset(&stPhoVenChn, 0, sizeof(MPP_CHN_S));

  stSrcChn.enModId = RK_ID_VI;
  stSrcChn.s32DevId = u32CamId;
  stSrcChn.s32ChnId = pstPhotoCfg->vi_attr.u32ViChn;
  stPhoVenChn.enModId = RK_ID_VENC;
  stPhoVenChn.s32DevId = u32CamId;
  stPhoVenChn.s32ChnId = pstPhotoCfg->venc_chn;

  memset(&stPhoAttr, 0, sizeof(VENC_CHN_ATTR_S));
  memset(&stChnAttr, 0, sizeof(stChnAttr));

  ret = RK_MPI_VENC_GetChnAttr(stPhoVenChn.s32ChnId, &stPhoAttr);
  if (ret != RK_SUCCESS) {
    RKADK_LOGE("Photo get venc[%d] attr failed %x",
                stPhoVenChn.s32ChnId, ret);
    return -1;
  }

  bPhoto = RKADK_MEDIA_CompareResolution(&stPhoAttr,
                                  pstPhotoCfg->image_width,
                                  pstPhotoCfg->image_height);
  if (!bPhoto) {
    RKADK_LOGE("Photo dose not reset venc attr");
    return -1;
  }

  RKADK_PHOTO_ResetAttr(pstSensorCfg, pstPhotoCfg,
                        &stPhoAttr, &stChnAttr);

  ret = RK_MPI_SYS_UnBind(&stSrcChn, &stPhoVenChn);
  if (ret != RK_SUCCESS) {
    RKADK_LOGE("Photo VI UnBind VENC [%d %d]fail %x", stSrcChn.s32ChnId,
                stPhoVenChn.s32ChnId, ret);
    return -1;
  }

  ret = RK_MPI_VENC_SetChnAttr(stPhoVenChn.s32ChnId, &stPhoAttr);
  if (ret != RK_SUCCESS) {
    RKADK_LOGE("Photo set venc[%d] attr failed %x",
                stPhoVenChn.s32ChnId, ret);
    return -1;
  }

  ret = RK_MPI_VI_SetChnAttr(u32CamId, stSrcChn.s32ChnId,
                            &stChnAttr);
  if (ret != RK_SUCCESS) {
    RKADK_LOGE("RK_MPI_VI_SetChnAttr(%d) failed %x",
                stSrcChn.s32ChnId, ret);
    return -1;
  }

  ret = RK_MPI_SYS_Bind(&stSrcChn, &stPhoVenChn);
  if (ret != RK_SUCCESS) {
    RKADK_LOGE("Photo VI Bind VENC [%d %d] fail %x",stSrcChn.s32ChnId,
                stPhoVenChn.s32ChnId, ret);
    return -1;
  }

  RKADK_LOGI("Photo[%d] Reset end...", u32CamId);
  return 0;
}

static RKADK_S32 RKADK_GetThmInJpg(RKADK_CHAR *pszFileName,
                                   RKADK_THUMB_ATTR_S *pstThumbAttr) {
  FILE *fd = NULL;
  RKADK_U8 *pFile;
  int len, cur, exif_end;

  RKADK_PARAM_THUMB_CFG_S *ptsThumbCfg = RKADK_PARAM_GetThumbCfg();
  if (!ptsThumbCfg) {
    RKADK_LOGE("RKADK_PARAM_GetThumbCfg failed");
    return -1;
  }

  if (!pstThumbAttr->u32Width || !pstThumbAttr->u32Height) {
    pstThumbAttr->u32Width = UPALIGNTO(ptsThumbCfg->thumb_width, 4);
    pstThumbAttr->u32Height = UPALIGNTO(ptsThumbCfg->thumb_height, 2);
  }

  if (!pstThumbAttr->u32VirWidth || !pstThumbAttr->u32VirHeight) {
    pstThumbAttr->u32VirWidth = pstThumbAttr->u32Width;
    pstThumbAttr->u32VirHeight = pstThumbAttr->u32Height;
  }

  fd = fopen(pszFileName, "r");
  if (!fd) {
    RKADK_LOGE("open %s failed", pszFileName);
    return -1;
  }

  if (fseek(fd, 0, SEEK_END) || (len = ftell(fd)) == -1 ||
      fseek(fd, 0, SEEK_SET)) {
    fclose(fd);
    RKADK_LOGE("seek %s failed", pszFileName);
    return -1;
  }

  pFile = (RKADK_U8 *)mmap(NULL, len, PROT_READ, MAP_SHARED, fileno(fd), 0);
  if (pFile == MAP_FAILED) {
    fclose(fd);
    RKADK_LOGE("mmap %s failed, errno: %d", pszFileName, errno);
    return -1;
  }
  fclose(fd);

  cur = 0;

  while (cur + 4 + 4 < len) {
    if (pFile[cur] != 0xFF) {
      RKADK_LOGE("Bad Jpg file, 0xFF expected at offset 0x%08x", cur);
      munmap(pFile, len);
      return RKADK_FAILURE;
    }

    if (pFile[cur + 1] == 0xD8 || pFile[cur + 1] == 0xD9) {
      cur += 2;
    } else if (pFile[cur + 1] == 0xE1) {
      int _tmp;
      _tmp = cur + 1 + 2 + 1;
      if (pFile[_tmp + 0] == 'E' && pFile[_tmp + 1] == 'x' &&
          pFile[_tmp + 2] == 'i' && pFile[_tmp + 3] == 'f') {
        /* Found the 0xFFE1 (EXIF) */
        cur += 2;
        break;
      }
    }

    cur += 2;
    cur += bswap_16(*(RKADK_U16 *) (pFile + cur));
  }

  /* Instead of parsing Exif, searching 0xFFD8 */
  RKADK_U16 u16ExifLen = bswap_16(*(RKADK_U16 *) (pFile + cur));
  int i;

  if (u16ExifLen + cur >= len) {
    RKADK_LOGE("Bad Jpg file, Exif len exceed at offset 0x%08x", cur);
    munmap(pFile, len);
    return RKADK_FAILURE;
  }

  exif_end = cur + u16ExifLen;

  for (i = 2; i < u16ExifLen; i++) {
    if (pFile[cur + i] != 0xFF || pFile[cur + i + 1] != 0xD8)
      continue;

    cur += i;
    /* Found the thumbnail */
    break;
  }

  /* pFile[cur, exif_end) is the thumbnail */
  if (!pstThumbAttr->pu8Buf) {
    pstThumbAttr->pu8Buf = (RKADK_U8 *) malloc(exif_end - cur);
    if (!pstThumbAttr->pu8Buf) {
      munmap(pFile, len);
      return RKADK_FAILURE;
    }
    pstThumbAttr->u32BufSize = exif_end - cur;
  }

  memcpy(pstThumbAttr->pu8Buf, pFile + cur, pstThumbAttr->u32BufSize);

  munmap(pFile, len);

  return RKADK_SUCCESS;
}

RKADK_S32 RKADK_PHOTO_GetThmInJpg(RKADK_CHAR *pszFileName,
                                  RKADK_JPG_THUMB_TYPE_E eThmType,
                                  RKADK_U8 *pu8Buf, RKADK_U32 *pu32Size) {
  int ret;
  RKADK_THUMB_ATTR_S stThumbAttr;

  RKADK_CHECK_POINTER(pszFileName, RKADK_FAILURE);
  RKADK_CHECK_POINTER(pu8Buf, RKADK_FAILURE);

  stThumbAttr.u32Width = 0;
  stThumbAttr.u32Height = 0;
  stThumbAttr.u32VirWidth = 0;
  stThumbAttr.u32VirHeight = 0;
  stThumbAttr.enType = RKADK_THUMB_TYPE_JPEG;
  stThumbAttr.pu8Buf = pu8Buf;
  stThumbAttr.u32BufSize = *pu32Size;

  ret = RKADK_GetThmInJpg(pszFileName, &stThumbAttr);
  *pu32Size = stThumbAttr.u32BufSize;

  return ret;
}

RKADK_S32 RKADK_PHOTO_ThumbBufFree(RKADK_THUMB_ATTR_S *pstThumbAttr) {
  return RKADK_MEDIA_FrameFree((RKADK_FRAME_ATTR_S *)pstThumbAttr);
}

RKADK_S32 RKADK_PHOTO_GetThmInJpgEx(RKADK_CHAR *pszFileName,
                                    RKADK_JPG_THUMB_TYPE_E eThmType,
                                    RKADK_THUMB_ATTR_S *pstThumbAttr) {
  int ret;

  RKADK_CHECK_POINTER(pszFileName, RKADK_FAILURE);
  RKADK_CHECK_POINTER(pstThumbAttr, RKADK_FAILURE);

  ret = RKADK_GetThmInJpg(pszFileName, pstThumbAttr);
  if (ret)
    RKADK_PHOTO_ThumbBufFree(pstThumbAttr);

  return ret;
}
