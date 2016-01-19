#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
#include <stdio.h>

static void SaveFrame(AVFrame *pFrame, int width, int height, int iFrame)
{
	FILE *pFile;
	char szFilename[32];
	int y;  
	sprintf(szFilename, "frame%d.ppm", iFrame);
	pFile = fopen(szFilename, "wb");
	if (!pFile)
		return;
	fprintf(pFile, "P6\n%d %d\n255\n", width, height);
	for (y = 0; y < height; y++)
		fwrite(pFrame->data[0] + y * pFrame->linesize[0], 1, width * 3, pFile);
	fclose(pFile);
}

int main(int argc, const char *argv[])
{
	AVFormatContext *pFormatCtx = NULL;
	int i, videoStream;
	AVCodecContext *pCodecCtx;
	AVCodec *pCodec;
	AVFrame *pFrame;
	AVFrame *pFrameRGB;
	AVPacket packet;
	AVCodecContext *pEncCodecCtx;
	AVCodec *pEncCodec;
	AVPacket encPacket;
	FILE *pEncOutFile;
	int frameFinished;
	int gotPacket;
	int numBytes;
	uint8_t *buffer;

	av_register_all();
	/* decoder init */
	if (avformat_open_input(&pFormatCtx, argv[1], NULL, NULL) != 0) {
		return -1;
	}
	if (avformat_find_stream_info(pFormatCtx, NULL) < 0) {
		return -1;
	}
	av_dump_format(pFormatCtx, -1, argv[1], 0);
	videoStream = -1;
	for (i = 0; i < pFormatCtx->nb_streams; i++)
		if (pFormatCtx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
			videoStream = i;
			break;
		}
	if (videoStream == -1) {
		return -1;
	}
	pCodecCtx = pFormatCtx->streams[videoStream]->codec;
	pCodec = avcodec_find_decoder(pCodecCtx->codec_id);
	if (pCodec == NULL) {
		return -1;
	}
	if (avcodec_open2(pCodecCtx, pCodec, NULL) < 0) {
		return -1;
	}

	/* encoder init */
	pEncCodec = avcodec_find_encoder(pCodecCtx->codec_id);
	if (pEncCodec == NULL) {
		printf("could not find enc codec %d\n", pCodecCtx->codec_id);
		return -1;
	}

	pEncCodecCtx = avcodec_alloc_context3(pEncCodec);

	pEncCodecCtx->width = pCodecCtx->width;
	pEncCodecCtx->height = pCodecCtx->height;
	pEncCodecCtx->pix_fmt = pCodecCtx->pix_fmt;
	/* framerate, vpu will use the half of this value as framerate */
	pEncCodecCtx->time_base = (AVRational){ 1, 50 };
	pEncCodecCtx->bit_rate = 1800000;
	pEncCodecCtx->level = pCodecCtx->level;

	if (avcodec_open2(pEncCodecCtx, pEncCodec, NULL) < 0) {
		printf("enc codec open failed\n");
		return -1;
	}

	/* output encoder file */
	pEncOutFile = fopen("output.h264", "wb");
	av_init_packet(&encPacket);

	pFrame = avcodec_alloc_frame();
	if (pFrame == NULL) {
		return -1;
	}
	pFrameRGB = avcodec_alloc_frame();
	if (pFrameRGB == NULL) {
		return -1;
	}
	numBytes = avpicture_get_size(PIX_FMT_RGB24, pCodecCtx->width, pCodecCtx->height);
	buffer = av_malloc(numBytes);
	avpicture_fill((AVPicture *)pFrameRGB, buffer, PIX_FMT_RGB24, pCodecCtx->width, pCodecCtx->height);
	i = 0;
	while (av_read_frame(pFormatCtx, &packet) >= 0) {
		if (packet.stream_index == videoStream) {
			/* first we get a decoded frame */
			avcodec_decode_video2(pCodecCtx, pFrame, &frameFinished, &packet);
			if (frameFinished) {
				/* convert the decoded data from yuv420p to rgb24 */
				struct SwsContext *img_convert_ctx = NULL;
				img_convert_ctx = sws_getCachedContext(img_convert_ctx, pCodecCtx->width, pCodecCtx->height, pCodecCtx->pix_fmt, pCodecCtx->width, pCodecCtx->height, PIX_FMT_RGB24, SWS_BICUBIC, NULL, NULL, NULL);
				if (!img_convert_ctx) {
					fprintf(stderr, "Cannot initialize sws conversion context\n");
					exit(1);
				}
				sws_scale(img_convert_ctx, (const uint8_t *const *)pFrame->data, pFrame->linesize, 0, pCodecCtx->height, pFrameRGB->data, pFrameRGB->linesize);
				if (i < 50) {
					SaveFrame(pFrameRGB, pCodecCtx->width, pCodecCtx->height, i);
				}

				AVFrame *tmpFrame = avcodec_alloc_frame();
				tmpFrame->format = pFrame->format;
				int h_align, w_align;
				h_align = pFrame->height;
				w_align = pFrame->width;
				tmpFrame->width = w_align;
				tmpFrame->height = h_align;

				/* the rk h264 encoder only can receive the continus memory in tmpFrame->data[0] */
				tmpFrame->data[0] = av_malloc(w_align * h_align * 3 / 2);
				tmpFrame->data[1] = tmpFrame->data[0] + h_align * pFrame->linesize[0];
				tmpFrame->data[2] = tmpFrame->data[1] + (h_align >> 1) * pFrame->linesize[1];
				memcpy(tmpFrame->data[0], pFrame->data[0], h_align * pFrame->linesize[0]);
				memcpy(tmpFrame->data[1], pFrame->data[1], (h_align >> 1) * pFrame->linesize[1]);
				memcpy(tmpFrame->data[2], pFrame->data[2], (h_align >> 1) * pFrame->linesize[2]);

				/* encode a frame */
				avcodec_encode_video2(pEncCodecCtx, &encPacket, tmpFrame, &gotPacket);
				if (gotPacket && i < 100) {
					if (i == 0) {
						fwrite(pEncCodecCtx->extradata, 1, pEncCodecCtx->extradata_size, pEncOutFile);
						fflush(pEncOutFile);
					}
					fwrite(encPacket.data, 1, encPacket.size, pEncOutFile);
					fflush(pEncOutFile);
				}
				av_free(tmpFrame->data[0]);
				av_free(tmpFrame);
				i++;
			}
			av_free_packet(&encPacket);

		}
		av_free_packet(&packet);
	}
	fclose(pEncOutFile);
	av_free(buffer);
	av_free(pFrameRGB);
	av_free(pFrame);
	avcodec_close(pCodecCtx);
	avformat_close_input(&pFormatCtx);
	return 0;
}
