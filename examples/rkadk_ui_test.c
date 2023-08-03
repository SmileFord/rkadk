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

#include "rkadk_common.h"
#include "rkadk_media_comm.h"
#include "rkadk_ui.h"
#include "rkadk_disp.h"
#include "rkadk_log.h"
#include "rkadk_param.h"
#include "isp/sample_isp.h"
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern int optind;
extern char *optarg;

static bool is_quit = false;
static RKADK_CHAR optstr[] = "a:I:W:H:p:h";

#define IQ_FILE_PATH "/etc/iqfiles"

static void print_usage(const RKADK_CHAR *name) {
  printf("usage example:\n");
  printf("\t%s [-a /etc/iqfiles] [-I 0]\n", name);
  printf("\t-a: enable aiq with dirpath provided, eg:-a "
         "/oem/etc/iqfiles/, Default /etc/iqfiles,"
         "without this option aiq should run in other application\n");
  printf("\t-I: camera id, Default 0\n");
  printf("\t-p: param ini directory path, Default:/data/rkadk\n");
  printf("\t-W: display width, default 720\n");
  printf("\t-H: display height, default 1280\n");
}

static void sigterm_handler(int sig) {
  fprintf(stderr, "signal %d\n", sig);
  is_quit = true;
}

int main(int argc, char *argv[]) {
  int c, ret;
  RKADK_MW_PTR pUi = NULL;
  RKADK_PARAM_FPS_S stFps;
  RKADK_U32 u32CamId = 0;
  RKADK_U32 u32Size = 0;
  RKADK_U32 u32Width = 720, u32Height = 1280;
  RKADK_VOID *pData;
  RKADK_UI_ATTR_S stUiAttr;
  RKADK_UI_FRAME_INFO stUiFrmInfo;
  RKADK_CHAR *pIqfilesPath = IQ_FILE_PATH;
  const char *iniPath = NULL;
  char path[RKADK_PATH_LEN];
  char sensorPath[RKADK_MAX_SENSOR_CNT][RKADK_PATH_LEN];

  while ((c = getopt(argc, argv, optstr)) != -1) {
    const char *tmp_optarg = optarg;
    switch (c) {
    case 'a':
      if (!optarg && NULL != argv[optind] && '-' != argv[optind][0]) {
        tmp_optarg = argv[optind++];
      }

      if (tmp_optarg)
        pIqfilesPath = (char *)tmp_optarg;
      break;
    case 'I':
      u32CamId = atoi(optarg);
      break;
    case 'p':
      iniPath = optarg;
      RKADK_LOGD("iniPath: %s", iniPath);
      break;
    case 'W':
      u32Width = atoi(optarg);
      break;
    case 'H':
      u32Height = atoi(optarg);
      break;
    case 'h':
    default:
      print_usage(argv[0]);
      optind = 0;
      return 0;
    }
  }
  optind = 0;

  RKADK_LOGD("#camera id: %d", u32CamId);

  memset(&stUiAttr, 0, sizeof(RKADK_UI_ATTR_S));

  stUiAttr.u32VoChn = 1;
  stUiAttr.u32DispWidth = u32Width;
  stUiAttr.u32DispHeight = u32Height;
  stUiAttr.u32DispFrmRt = 30;
  stUiAttr.u32ImgWidth = u32Width;
  stUiAttr.u32ImgHeight = u32Height;
  stUiAttr.enUiVoFormat = VO_FORMAT_RGB888;
  stUiAttr.enVoSpliceMode = SPLICE_MODE_RGA;
#ifdef RV1106_1103
  stUiAttr.enUiVoIntfTye = DISPLAY_TYPE_DEFAULT;
#else
  stUiAttr.enUiVoIntfTye = DISPLAY_TYPE_MIPI;
#endif

  stUiFrmInfo.Format = RKADK_FMT_RGBA8888;
  stUiFrmInfo.u32Width = u32Width;
  stUiFrmInfo.u32Height = u32Height;

  u32Size = stUiFrmInfo.u32Width * stUiFrmInfo.u32Height * 4;

  RKADK_MPI_SYS_Init();

  ret = RK_MPI_MMZ_Alloc(&stUiFrmInfo.pMblk, u32Size,  RK_MMZ_ALLOC_CACHEABLE);
  if (ret) {
    RKADK_LOGE("alloc failed!");
    return -1;
  }

  pData = RK_MPI_MMZ_Handle2VirAddr(stUiFrmInfo.pMblk);
  memset(pData, 0x00, u32Size);
  RK_MPI_SYS_MmzFlushCache(stUiFrmInfo.pMblk, RK_FALSE);

#ifdef RKAIQ
  if (iniPath) {
    memset(path, 0, RKADK_PATH_LEN);
    memset(sensorPath, 0, RKADK_MAX_SENSOR_CNT * RKADK_PATH_LEN);
    sprintf(path, "%s/rkadk_setting.ini", iniPath);
    for (int i = 0; i < RKADK_MAX_SENSOR_CNT; i++)
      sprintf(sensorPath[i], "%s/rkadk_setting_sensor_%d.ini", iniPath, i);

    /*
    lg:
      char *sPath[] = {"/data/rkadk/rkadk_setting_sensor_0.ini",
      "/data/rkadk/rkadk_setting_sensor_1.ini", NULL};
    */
    char *sPath[] = {sensorPath[0], sensorPath[1], NULL};

    RKADK_PARAM_Init(path, sPath);
  } else {
    RKADK_PARAM_Init(NULL, NULL);
  }

  stFps.enStreamType = RKADK_STREAM_TYPE_SENSOR;
  ret = RKADK_PARAM_GetCamParam(u32CamId, RKADK_PARAM_TYPE_FPS, &stFps);
  if (ret) {
    RKADK_LOGE("RKADK_PARAM_GetCamParam fps failed");
    return -1;
  }

  rk_aiq_working_mode_t hdr_mode = RK_AIQ_WORKING_MODE_NORMAL;
  RKADK_BOOL fec_enable = RKADK_FALSE;
  SAMPLE_ISP_Start(u32CamId, hdr_mode, fec_enable, pIqfilesPath, stFps.u32Framerate);
#endif
  ret = RKADK_DISP_Init(u32CamId);
  ret |= RKADK_UI_Create(&stUiAttr, &pUi);
  if (ret) {
    RKADK_LOGE("RKADK_DISP_Init failed(%d)", ret);
#ifdef RKAIQ
    SAMPLE_ISP_Stop(u32CamId);
#endif
    return -1;
  }

  signal(SIGINT, sigterm_handler);
  char cmd[64];
  printf("\n#Usage: input 'quit' to exit programe!\n"
         "peress any other key to quit\n");

  while (!is_quit) {
    fgets(cmd, sizeof(cmd), stdin);
    if (strstr(cmd, "quit") || is_quit) {
      RKADK_LOGD("#Get 'quit' cmd!");
      break;
    } else if (strstr(cmd, "all")){
      memset(pData, 0xff, u32Size);
      RKADK_UI_Update(pUi, &stUiFrmInfo);
    } else if (strstr(cmd, "few")){
      memset(pData, 0x00, u32Size);
      memset(pData, 0xaa, u32Size / 2);
      RKADK_UI_Update(pUi, &stUiFrmInfo);
    }
  }

  RKADK_UI_Destroy(pUi);
  RKADK_DISP_DeInit(u32CamId);
  RK_MPI_MMZ_Free(stUiFrmInfo.pMblk);

#ifdef RKAIQ
  SAMPLE_ISP_Stop(u32CamId);
#endif
  RKADK_MPI_SYS_Exit();
  return 0;
}
