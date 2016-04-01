#include <dlfcn.h>  // for dlopen/dlclose
#include <stdint.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdbool.h>
#include <unistd.h>

#include <time.h>
#include <sys/time.h>

#include "rockchip/vpu.h"
#include "rockchip/vpu_global.h"
#include "rockchip/vpu_api_private.h"
#include "rockchip/vpu_simple_mem_pool.h"
#include "rockchip.h"

#include "avcodec.h"
#include "internal.h"

typedef struct _RkVpuDecContext {
	RkVpuContext				parent_ctx;
	OMX_RK_VIDEO_CODINGTYPE		video_type;
	VpuCodecContext_t *ctx;
	DecoderOut_t				decOut;
	VideoPacket_t				demoPkt;
	VpuMemFunc					vpu_mem_link;
	VpuMemFunc					vpu_free_linear;
	VpuMemFunc					vpu_mem_invalidate;
	VpuMemFunc                  vpu_mem_getfd;
	VpuContextFunc				vpu_open;
	VpuContextFunc				vpu_close;
	VPUMemLinear_t              front_vpumem;
	RK_S32 (*vpu_mem_dup)(VPUMemLinear_t *dst, VPUMemLinear_t *src);
	int enable_mem_pool;
} RkVpuDecContext;

static int rkdec_prepare(AVCodecContext *avctx)
{
	RkVpuDecContext *rkdec_ctx = avctx->priv_data;

	if (!rk_init_dlopen(avctx)) {
		av_log(avctx, AV_LOG_ERROR, "init_rkdeclib failed");
		return -1;
	}

	rkdec_ctx->vpu_open = (VpuContextFunc)dlsym(rkdec_ctx->parent_ctx.api_handle, "vpu_open_context");
	rkdec_ctx->vpu_close = (VpuContextFunc)dlsym(rkdec_ctx->parent_ctx.api_handle, "vpu_close_context");
	rkdec_ctx->vpu_mem_link = (VpuMemFunc)dlsym(rkdec_ctx->parent_ctx.vpu_handle, "VPUMemLink");
	rkdec_ctx->vpu_free_linear = (VpuMemFunc)dlsym(rkdec_ctx->parent_ctx.vpu_handle, "VPUFreeLinear");
	rkdec_ctx->vpu_mem_invalidate = (VpuMemFunc)dlsym(rkdec_ctx->parent_ctx.vpu_handle, "VPUMemInvalidate");
	rkdec_ctx->vpu_mem_getfd = (VpuMemFunc)dlsym(rkdec_ctx->parent_ctx.vpu_handle, "VPUMemGetFD");
	rkdec_ctx->vpu_mem_dup = (VpuMemFunc)dlsym(rkdec_ctx->parent_ctx.vpu_handle, "VPUMemDuplicate");

	if (rkdec_ctx->vpu_open(&rkdec_ctx->ctx) || rkdec_ctx->ctx == NULL) {
		av_log(avctx, AV_LOG_ERROR, "vpu_open_context failed");
		return -1;
	}

	rkdec_ctx->ctx->codecType = CODEC_DECODER;
	rkdec_ctx->ctx->videoCoding = rkdec_ctx->video_type;
	rkdec_ctx->ctx->width = avctx->width;
	rkdec_ctx->ctx->height = avctx->height;
	rkdec_ctx->ctx->no_thread = 1;
	rkdec_ctx->ctx->enableparsing = 1;

	if (rkdec_ctx->ctx->init(rkdec_ctx->ctx, avctx->extradata, avctx->extradata_size) != 0) {
		av_log(avctx, AV_LOG_ERROR, "ctx init failed");
		return -1;
	}

	if (avctx->opaque != NULL) {
		rkdec_ctx->ctx->control(rkdec_ctx->ctx, VPU_API_SET_VPUMEM_CONTEXT, avctx->opaque);
		rkdec_ctx->enable_mem_pool = 1;
	} else {
		rkdec_ctx->enable_mem_pool = 0;
	}

#if 0
	EncParameter_t param;
	param.width = avctx->width;
	param.height = avctx->height;
	rkdec_ctx->ctx->control(rkdec_ctx->ctx, VPU_API_MALLOC_THREAD, &param);
#endif

	return 0;
}

static int rkdec_init(AVCodecContext *avctx)
{
	RkVpuDecContext *rkdec_ctx = avctx->priv_data;
	if (!rk_hw_support(avctx->codec_id, CODEC_DECODER)) {
		return -1;
	}

	memset(rkdec_ctx, 0, sizeof(RkVpuDecContext));

	rkdec_ctx->video_type = OMX_RK_VIDEO_CodingUnused;
	switch (avctx->codec_id) {
	case AV_CODEC_ID_H264:
		rkdec_ctx->video_type = OMX_RK_VIDEO_CodingAVC;
		break;
	case AV_CODEC_ID_HEVC:
		rkdec_ctx->video_type = OMX_RK_VIDEO_CodingHEVC;
		break;
	case AV_CODEC_ID_MPEG4:
		rkdec_ctx->video_type = OMX_RK_VIDEO_CodingMPEG4;
		break;
	case AV_CODEC_ID_MJPEG:
		rkdec_ctx->video_type = OMX_RK_VIDEO_CodingMJPEG;
		break;
	default:
		av_log(avctx, AV_LOG_ERROR, "codec id %d is not supported", avctx->codec_id);
		break;
	}

	avctx->pix_fmt = AV_PIX_FMT_YUV420P;

	return rkdec_prepare(avctx);
}

static int rkdec_deinit(AVCodecContext *avctx)
{
	RkVpuDecContext *rkdec_ctx = avctx->priv_data;

	rkdec_ctx->vpu_close(&rkdec_ctx->ctx);
	rk_deinit_dlopen(avctx);

	return 0;
}

static int rkdec_decode_frame(AVCodecContext *avctx/*ctx*/, void *data/*AVFrame*/,
							  int *got_frame/*frame count*/, AVPacket *packet/*src*/)
{
	int ret = 0;
	int i;
	RkVpuDecContext *rkdec_ctx = avctx->priv_data;
	VpuCodecContext_t *ctx = rkdec_ctx->ctx;
	VideoPacket_t *pDemoPkt = &rkdec_ctx->demoPkt;
	DecoderOut_t *pDecOut = &rkdec_ctx->decOut;

	pDemoPkt->data = packet->data;
	pDemoPkt->size = packet->size;

	if (packet->pts != AV_NOPTS_VALUE) {
		pDemoPkt->pts = av_q2d(avctx->time_base) * (packet->pts) * 1000000ll;
	} else {
		pDemoPkt->pts = av_q2d(avctx->time_base) * (packet->dts) * 1000000ll;
	}

	memset(pDecOut, 0, sizeof(DecoderOut_t));
	pDecOut->data = (RK_U8 *)(av_malloc)(sizeof(VPU_FRAME));
	if (pDecOut->data == NULL) {
		av_log(avctx, AV_LOG_ERROR, "malloc VPU_FRAME failed");
		goto out;
	}

	memset(pDecOut->data, 0, sizeof(VPU_FRAME));
	pDecOut->size = 0;

	if (ctx->decode_sendstream(ctx, pDemoPkt) != 0) {
		av_log(avctx, AV_LOG_ERROR, "send packet failed");
	}

	if ((ret = ctx->decode_getframe(ctx, pDecOut)) == 0) {
		if (pDecOut->size && pDecOut->data) {
			VPU_FRAME *pframe = (VPU_FRAME *)(pDecOut->data);
			AVFrame *frame = data;

			if (avctx->width == 0 || avctx->height == 0) {
				avctx->width = pframe->DisplayWidth;
				avctx->height = pframe->DisplayHeight;
			}

			if ((ret = ff_get_buffer(avctx, frame, 0)) < 0) {
				av_log(avctx, AV_LOG_ERROR, "Failed to get buffer!!!:%d", ret);
				goto out;
			}
			
			if (rkdec_ctx->enable_mem_pool == 0 && rkdec_ctx->front_vpumem.phy_addr) {
				rkdec_ctx->vpu_free_linear(&rkdec_ctx->front_vpumem);
			}

			//rkdec_ctx->vpu_mem_link(&pframe->vpumem);
			//rkdec_ctx->vpu_mem_invalidate(&pframe->vpumem);
			rkdec_ctx->vpu_mem_dup(&rkdec_ctx->front_vpumem, &pframe->vpumem);
 
			int dma_fd = rkdec_ctx->vpu_mem_getfd(&rkdec_ctx->front_vpumem);
			frame->data[3] = dma_fd;

			if (getenv("RK_FFMPEG_SAVE_PIC") != NULL && atoi(getenv("RK_FFMPEG_SAVE_PIC")) == 1) {
				uint32_t wAlign16 = pframe->FrameWidth;
				uint32_t hAlign16 = pframe->FrameHeight;
				uint32_t frameSize = wAlign16 * hAlign16 * 3 / 2;
				uint8_t *buffer = pframe->vpumem.vir_addr;
				memcpy(buffer, pframe->vpumem.vir_addr, frameSize);
				for (int i = 0;i < hAlign16;i++) {
					memcpy((frame->data[0] + i * frame->linesize[0]), (buffer + i * wAlign16), wAlign16);
				}
				uint8_t *tmpBuffer = buffer + wAlign16 * hAlign16;
				for (int i = 0;i < (hAlign16 >> 1);i++) {
					for (int j = 0;j < (wAlign16 >> 1);j++) {
						*(frame->data[1] + j + i * frame->linesize[1]) = *(tmpBuffer + 2 * j + i * wAlign16);
						*(frame->data[2] + j + i * frame->linesize[2]) = *(tmpBuffer + 2 * j + 1 + i * wAlign16);
					}
				}
			}

			rkdec_ctx->vpu_free_linear(&pframe->vpumem);

			*got_frame = 1;
		}
	} else {
		goto out;
	}

	ret = 0;
out:
	if (pDecOut->data != NULL) {
		av_free(pDecOut->data);
		pDecOut->data = NULL;
	}
	if (ret < 0) {
		av_log(avctx, AV_LOG_ERROR, "Something wrong during decode!!!");
	}

	return (ret < 0) ? ret : packet->size;
}

static void rkdec_decode_flush(AVCodecContext *avctx)
{
	RkVpuDecContext *rkdec_ctx = avctx->priv_data;
	if (rkdec_ctx->front_vpumem.phy_addr) {
		rkdec_ctx->vpu_free_linear(&rkdec_ctx->front_vpumem);
	}
	rkdec_ctx->ctx->flush(rkdec_ctx->ctx);
}

#define DECLARE_RKDEC_VIDEO_DECODER(TYPE, CODEC_ID)                     \
    AVCodec ff_##TYPE##_decoder = {                                         \
        .name           = #TYPE,                                            \
        .long_name      = NULL_IF_CONFIG_SMALL(#TYPE " hw decoder"),        \
        .type           = AVMEDIA_TYPE_VIDEO,                               \
        .id             = CODEC_ID,                                         \
        .priv_data_size = sizeof(RkVpuDecContext),                          \
        .init           = rkdec_init,                                       \
        .close          = rkdec_deinit,                                     \
        .decode         = rkdec_decode_frame,                               \
        .capabilities   = CODEC_CAP_DELAY,                                  \
        .flush          = rkdec_decode_flush,                               \
        .pix_fmts       = (const enum AVPixelFormat[]) {                    \
            AV_PIX_FMT_YUV420P,						\
            AV_PIX_FMT_NONE							\
        },									\
    };                                                                      \

DECLARE_RKDEC_VIDEO_DECODER(h264_rkvpu, AV_CODEC_ID_H264)
DECLARE_RKDEC_VIDEO_DECODER(mpeg4_rkvpu, AV_CODEC_ID_MPEG4)
DECLARE_RKDEC_VIDEO_DECODER(mjpeg_rkvpu, AV_CODEC_ID_MJPEG)
