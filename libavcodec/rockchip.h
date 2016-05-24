#ifndef __FFMPEG_ROCKCHIP_H__
#define __FFMPEG_ROCKCHIP_H__

#include <dlfcn.h>
#include <stdint.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdbool.h>
#include <unistd.h>

#include "rockchip/vpu.h"
#include "rockchip/vpu_global.h"
#include "rockchip/vpu_api_private.h"

#include "avcodec.h"
#include "internal.h"

typedef struct _RkVpuContext {
    void*						api_handle;
    void*						vpu_handle;
} RkVpuContext;

bool rk_hw_support(enum AVCodecID codec_id, CODEC_TYPE codec_type);
bool rk_init_dlopen(AVCodecContext* avctx);
void rk_deinit_dlopen(AVCodecContext *avctx);

typedef uint32_t (*VpuMemFunc)(VPUMemLinear_t *p);
typedef uint32_t (*VpuContextFunc)(VpuCodecContext_t **ctx);

#endif
