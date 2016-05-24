#include "rockchip.h"

static const char *RK_VPU_API_LIB = "/usr/lib/librk_codec.so";
static const char *RK_VPU_LIB = "/usr/lib/libvpu.so";

bool rk_hw_support(enum AVCodecID codec_id, CODEC_TYPE codec_type) {
	switch (codec_type) {
	case CODEC_DECODER:
		switch (codec_id) {
        	case AV_CODEC_ID_H264:
            	return true;
	        default:
    	        return false;
	    }
		break;
		
	case CODEC_ENCODER:
		switch (codec_id) {
			case AV_CODEC_ID_H264:
            	return true;
	        default:
    	        return false;
		}
		break;
	}
	
    return false;
}

bool rk_init_dlopen(AVCodecContext *avctx) {
	RkVpuContext *rk_ctx = (RkVpuContext *)avctx->priv_data;

    if (rk_ctx->api_handle == NULL) {
        rk_ctx->api_handle = dlopen(RK_VPU_API_LIB, RTLD_NOW | RTLD_LOCAL);
    }

    if (rk_ctx->vpu_handle == NULL) {
        rk_ctx->vpu_handle = dlopen(RK_VPU_LIB, RTLD_NOW | RTLD_LOCAL);
    }

    return rk_ctx->api_handle != NULL && rk_ctx->vpu_handle != NULL;
}

void rk_deinit_dlopen(AVCodecContext *avctx) {
	RkVpuContext *rk_ctx = (RkVpuContext *)avctx->priv_data;

    if (rk_ctx->api_handle != NULL) {
        dlclose(rk_ctx->api_handle);
        rk_ctx->api_handle = NULL;
    }

    if (rk_ctx->vpu_handle != NULL) {
        dlclose(rk_ctx->vpu_handle);
        rk_ctx->vpu_handle = NULL;
    }
}
