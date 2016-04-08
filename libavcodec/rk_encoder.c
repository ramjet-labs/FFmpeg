#include <dlfcn.h>  // for dlopen/dlclose
#include <stdint.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <stdbool.h>
#include <unistd.h>

#include "rockchip/vpu.h"
#include "rockchip/vpu_global.h"
#include "rockchip/vpu_api_private.h"
#include "rockchip.h"

#include "avcodec.h"
#include "internal.h"
#include "mpegvideo.h"

#define DIV3_TAG 861292868

//#define FAKE_DECODE

typedef struct _RkVpuEncContext {
	RkVpuContext				parent_ctx;
    OMX_RK_VIDEO_CODINGTYPE		video_type;
    VpuCodecContext_t*			ctx;
    EncoderOut_t				encOut;
    EncInputStream_t			demoPkt;
    VpuMemFunc					vpu_mem_link;
    VpuMemFunc					vpu_free_linear;
    VpuMemFunc					vpu_mem_invalidate;
    VpuContextFunc				vpu_open;
    VpuContextFunc				vpu_close;
} RkVpuEncContext;

static int rkenc_prepare(AVCodecContext* avctx)
{
    RkVpuEncContext* rkenc_ctx = avctx->priv_data;

    if (!rk_init_dlopen(avctx)) {
        av_log(avctx, AV_LOG_ERROR, "init_rkenclib failed");
        return -1;
    }

    rkenc_ctx->vpu_open = (VpuContextFunc) dlsym(rkenc_ctx->parent_ctx.api_handle, "vpu_open_context");
    rkenc_ctx->vpu_close = (VpuContextFunc) dlsym(rkenc_ctx->parent_ctx.api_handle, "vpu_close_context");
    rkenc_ctx->vpu_mem_link = (VpuMemFunc) dlsym(rkenc_ctx->parent_ctx.vpu_handle, "VPUMemLink");
    rkenc_ctx->vpu_free_linear = (VpuMemFunc) dlsym(rkenc_ctx->parent_ctx.vpu_handle, "VPUFreeLinear");
    rkenc_ctx->vpu_mem_invalidate = (VpuMemFunc) dlsym(rkenc_ctx->parent_ctx.vpu_handle, "VPUMemInvalidate");

    if (rkenc_ctx->vpu_open(&rkenc_ctx->ctx) || rkenc_ctx->ctx == NULL) {
        av_log(avctx, AV_LOG_ERROR, "vpu_open_context failed");
        return -1;
    }

    rkenc_ctx->ctx->codecType = CODEC_ENCODER;
    rkenc_ctx->ctx->videoCoding = rkenc_ctx->video_type;
    rkenc_ctx->ctx->width = avctx->width;
    rkenc_ctx->ctx->height = avctx->height;
    rkenc_ctx->ctx->no_thread = 1;
    rkenc_ctx->ctx->enableparsing = 1;
    
    rkenc_ctx->ctx->private_data = av_malloc(sizeof(EncParameter_t));
    memset(rkenc_ctx->ctx->private_data, 0, sizeof(EncParameter_t));

	EncParameter_t *enc_param;
	enc_param = (EncParameter_t*)rkenc_ctx->ctx->private_data;
    enc_param->width = avctx->width;
    enc_param->height = avctx->height;
    enc_param->bitRate = avctx->bit_rate;

    enc_param->framerate = avctx->time_base.den / avctx->time_base.num;
    if (enc_param->framerate < 1) {
        enc_param->framerate = 1;
    } else if (enc_param->framerate > ((1 << 16) - 1)) {
        enc_param->framerate = (1 << 16) - 1;
    }

    enc_param->enableCabac   = 0;
    enc_param->cabacInitIdc  = 0;
    enc_param->intraPicRate  = avctx->gop_size;
    if (avctx->level > 0)
        enc_param->levelIdc = avctx->level;

    av_log(avctx, AV_LOG_DEBUG, "enc width %d height %d bitrate %d framerate %d", enc_param->width, enc_param->height,
        enc_param->bitRate, enc_param->framerate);

    if (rkenc_ctx->ctx->init(rkenc_ctx->ctx, avctx->extradata, avctx->extradata_size) != 0) {
        av_log(avctx, AV_LOG_ERROR, "ctx init failed");
        return -1;
    }

    do {
	    enc_param->rc_mode = 1; /* for dynamic bit rate control */
        rkenc_ctx->ctx->control(rkenc_ctx->ctx, VPU_API_ENC_SETCFG, enc_param);
        rkenc_ctx->ctx->control(rkenc_ctx->ctx, VPU_API_ENC_GETCFG, enc_param);
    } while (enc_param->rc_mode != 1);

    if (rkenc_ctx->ctx->extradata_size > 0) {
        if (avctx->extradata != NULL) {
            av_free(avctx->extradata);
        }
        avctx->extradata = av_malloc(rkenc_ctx->ctx->extradata_size);
        if (avctx->extradata == NULL) {
            av_log(avctx, AV_LOG_ERROR, "extradata malloc failed");
            av_free(rkenc_ctx->ctx->private_data);
            return -1;
        }
        avctx->extradata_size = rkenc_ctx->ctx->extradata_size;
        memcpy(avctx->extradata, rkenc_ctx->ctx->extradata, avctx->extradata_size);
    }

    return 0;
}

static int rkenc_init(AVCodecContext *avctx) {
    RkVpuEncContext* rkenc_ctx = avctx->priv_data;
    if (!rk_hw_support(avctx->codec_id, CODEC_ENCODER)) {
        return -1;
    }
    memset(rkenc_ctx, 0, sizeof(RkVpuEncContext));

    rkenc_ctx->video_type = OMX_RK_VIDEO_CodingUnused;
    switch(avctx->codec_id){
        case AV_CODEC_ID_H264:
            rkenc_ctx->video_type = OMX_RK_VIDEO_CodingAVC;
            break;
        default:
            av_log(avctx, AV_LOG_ERROR, "codec id %d is not supported", avctx->codec_id);
            return -1;
    }

    avctx->pix_fmt = AV_PIX_FMT_YUV420P;

    return rkenc_prepare(avctx);
}

static int rkenc_deinit(AVCodecContext *avctx) {
    RkVpuEncContext* rkenc_ctx = avctx->priv_data;

    if (rkenc_ctx->ctx->private_data != NULL) {
        av_free(rkenc_ctx->ctx->private_data);
        rkenc_ctx->ctx->private_data = NULL;
    }

    rkenc_ctx->vpu_close(&rkenc_ctx->ctx);
    rk_deinit_dlopen(avctx);

    return 0;
}

static int rkenc_ffmpeg2vpu_color_space(AVCodecContext *avctx, const AVFrame *frame, int *size)
{
    int w_align;
    int h_align;
    //w_align = (frame->width + 15) & (~15);
    //h_align = (frame->height + 15) & (~15);
    w_align = frame->width;
    h_align = frame->height;

	switch (frame->format) {
		case AV_PIX_FMT_YUV420P:
			*size = w_align * h_align * 3 / 2;
			return VPU_H264ENC_YUV420_PLANAR;  /* YYYY... UUUU... VVVV */
		case AV_PIX_FMT_NV12:
			*size = w_align * h_align * 3 / 2;
			return VPU_H264ENC_YUV420_SEMIPLANAR;  /* YYYY... UVUVUV...    */
		case AV_PIX_FMT_YVYU422:
			*size = w_align * h_align * 2;
			return VPU_H264ENC_YUV422_INTERLEAVED_YUYV;    /* YUYVYUYV...          */
		case AV_PIX_FMT_UYVY422:
			*size = w_align * h_align * 2;
			return VPU_H264ENC_YUV422_INTERLEAVED_UYVY;    /* UYVYUYVY...          */
		case AV_PIX_FMT_RGB565LE:
			*size = w_align * h_align * 2;
			return VPU_H264ENC_RGB565; /* 16-bit RGB           */
		case AV_PIX_FMT_BGR565LE:
			*size = w_align * h_align * 2;
			return VPU_H264ENC_BGR565; /* 16-bit RGB           */
		case AV_PIX_FMT_RGB555LE:
			*size = w_align * h_align * 2;
			return VPU_H264ENC_RGB555; /* 15-bit RGB           */
		case AV_PIX_FMT_BGR555LE:
			*size = w_align * h_align * 2;
			return VPU_H264ENC_BGR555; /* 15-bit RGB           */
		case AV_PIX_FMT_RGB444LE:
			*size = w_align * h_align * 2;
			return VPU_H264ENC_RGB444; /* 12-bit RGB           */
		case AV_PIX_FMT_BGR444LE:
			*size = w_align * h_align * 2;
			return VPU_H264ENC_BGR444; /* 12-bit RGB           */
		case AV_PIX_FMT_RGB24:
			*size = w_align * h_align * 3;
			return VPU_H264ENC_RGB888;    /* 24-bit RGB           */
		case AV_PIX_FMT_BGR24:
			*size = w_align * h_align * 3;
			return VPU_H264ENC_BGR888;    /* 24-bit RGB           */
		default:
			av_log(avctx, AV_LOG_ERROR, "unsupport compress format %d", frame->format);
			break;
	}

	*size = 0;
	return -1;
}

static int rkenc_encode_frame(AVCodecContext *avctx, AVPacket *packet, const AVFrame *frame, int *got_packet)
{
    int ret = 0;
    int w_align;
    int h_align;

    RkVpuEncContext* rkenc_ctx = avctx->priv_data;
    VpuCodecContext_t* ctx = rkenc_ctx->ctx;
    EncInputStream_t* pDemoPkt = &rkenc_ctx->demoPkt;
    EncoderOut_t* pEncOut = &rkenc_ctx->encOut;
	H264EncPictureType rk_input_format;

	memset(pDemoPkt, 0, sizeof(EncInputStream_t));
    pDemoPkt->buf = frame->data[0];
	
	rk_input_format = rkenc_ffmpeg2vpu_color_space(avctx, frame, &pDemoPkt->size);
	if (rk_input_format == -1) {
		*got_packet = 0;
		return -1;
	}

	/* setup input format */
	ctx->control(ctx, VPU_API_ENC_SETFORMAT, &rk_input_format);

    if (frame->pts != AV_NOPTS_VALUE) {
        pDemoPkt->timeUs = av_q2d(avctx->time_base)*(frame->pts)*1000000ll;
    }

    w_align = frame->width;
    h_align = frame->height;
#if 0
    pEncOut->data = av_malloc(w_align * h_align);
    if (pEncOut->data == NULL) {
        av_log(avctx, AV_LOG_ERROR, "faild to malloc for enc out data");
        ret = -1;
        *got_packet = 0;
        goto vpu_encoder_failed;
    }

    pEncOut->size = w_align * h_align;
	
    memset(pEncOut->data, 0, pEncOut->size);
#endif

    if (ctx->encoder_sendframe(ctx, pDemoPkt) != 0) {
        av_log(avctx, AV_LOG_ERROR, "send packet failed");
        ret = -1;
	*got_packet = 0;
        goto vpu_encoder_failed;
    }
    pEncOut->data = NULL;

	pEncOut->size = 0;
    if ((ret = ctx->encoder_getstream(ctx, pEncOut)) == 0) {
        if (pEncOut->size && pEncOut->data) {
			if ((ret = ff_alloc_packet2(avctx, packet, pEncOut->size + 4)) < 0) {
				av_log(avctx, AV_LOG_ERROR, "failed to alloc for packet");
				*got_packet = 0;
                goto vpu_encoder_failed;
			}

            packet->data[0] = 0;
            packet->data[1] = 0;
            packet->data[2] = 0;
            packet->data[3] = 1;
			memcpy(packet->data + 4, pEncOut->data, pEncOut->size);
			*got_packet = 1;
		}
	} else {
		*got_packet = 0;
	}

vpu_encoder_failed:
    if (pEncOut->data != NULL) {
        av_free(pEncOut->data);
        pEncOut->data = NULL;
    }
	return ret;
}

#define DECLARE_RKENC_VIDEO_ENCODER(TYPE, CODEC_ID)                     \
    AVCodec ff_##TYPE##_encoder = {                                         \
        .name           = #TYPE,                                            \
        .long_name      = NULL_IF_CONFIG_SMALL(#TYPE " hw encoder"),        \
        .type           = AVMEDIA_TYPE_VIDEO,                               \
        .id             = CODEC_ID,                                         \
        .priv_data_size = sizeof(RkVpuEncContext),                          \
        .init           = rkenc_init,                                       \
        .close          = rkenc_deinit,                                     \
        .encode2         = rkenc_encode_frame,                               \
        .capabilities   = CODEC_CAP_DELAY,                                  \
        .pix_fmts       = (const enum AVPixelFormat[]) {                    \
            AV_PIX_FMT_YUV420P,						\
            AV_PIX_FMT_NONE							\
        },									\
    };                                                                      \

DECLARE_RKENC_VIDEO_ENCODER(h264_rkvpu, AV_CODEC_ID_H264)
