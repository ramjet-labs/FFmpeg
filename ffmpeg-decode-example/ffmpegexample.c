/*
 * Copyright 2016 Rockchip Electronics S.LSI Co. LTD
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
*/

#include <libavformat/avformat.h>
#include <libavcodec/avcodec.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
#include <stdio.h>
#include <linux/fb.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <drm_fourcc.h>
#include "dev.h"
#include "bo.h"
#include "modeset.h"
#include <sys/time.h>
#include <time.h>
#include <pthread.h>
#include "xf86drm.h"
#include "xf86drmMode.h"
#include "list.h"

#define DMA_FD_QUEUE_LENGTH 20

struct dma_fd_list {
	struct list_head node;
	int width;
	int height;
	int dma_fd;
};

struct cmd_context {
	int single_thread;
	char *input_file;

	/* ffmpeg parameter */
	AVFormatContext *format_ctx;
	AVCodecContext *codec_ctx;
	AVCodec *codec;
	AVFrame *frame;
	int video_stream_index;

	/* fps parameter */
	int fps;
	double cl;
	double last_cl;
	int cycle_num;

	/* display parameter */
	int displayed;
	struct sp_dev *dev;
	struct sp_plane **plane;
	struct sp_crtc *test_crtc;
	struct sp_plane *test_plane;

	/* multi thread parameter */
	pthread_t pid;
	struct list_head head;
	int list_max_num;
	int cur_num;
	pthread_mutex_t list_mutex;
	pthread_cond_t list_empty_cond;
	pthread_cond_t list_full_cond;
	int quit_flag;

	/* save frame */
	int record_frame;
};

void get_fps(struct cmd_context *cmd_ctx) {

	struct timeval tv;
	gettimeofday(&tv, NULL);
	cmd_ctx->cl = tv.tv_sec + (double)tv.tv_usec / 1000000;
	if (!cmd_ctx->last_cl)
		cmd_ctx->last_cl = cmd_ctx->cl;
	if (cmd_ctx->cl - cmd_ctx->last_cl > 1.0f) {
		cmd_ctx->last_cl = cmd_ctx->cl;
		printf(">>>>>fps=%d\n", cmd_ctx->fps);
		cmd_ctx->fps = 1;
	} else {
		cmd_ctx->fps++;
	}
}

static void save_frame(AVFrame *pFrame, int width, int height, int iFrame)  {
	FILE *pFile;
	char szFilename[32];
	int y;
	sprintf(szFilename, "frame%d.yuv", iFrame);
	pFile = fopen(szFilename, "wb");
	if (!pFile)
		return;

	fwrite(pFrame->data[0], 1, pFrame->linesize[0] * height, pFile);
	fwrite(pFrame->data[1], 1, pFrame->linesize[1] * height / 2, pFile);
	fwrite(pFrame->data[2], 1, pFrame->linesize[2] * height / 2, pFile);
	fclose(pFile);
}

void init_ffmpeg_context(struct cmd_context *cmd_ctx) {
	int i;

	av_register_all();
	if (avformat_open_input(&cmd_ctx->format_ctx, cmd_ctx->input_file, NULL, NULL) != 0) {
		printf("avformat_open_input failed\n");
		exit(-1);
	}

	if (avformat_find_stream_info(cmd_ctx->format_ctx, NULL) < 0) {
		printf("avformat_find_stream_info failed\n");
		exit(-1);
	}

	av_dump_format(cmd_ctx->format_ctx, -1, cmd_ctx->input_file, 0);

	cmd_ctx->video_stream_index = -1;
	for (i = 0; i < cmd_ctx->format_ctx->nb_streams; i++) {
		if (cmd_ctx->format_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
			cmd_ctx->video_stream_index = i;
			break;
		}
	}
	if (cmd_ctx->video_stream_index == -1) {
		printf("can not find video stream info\n");
		exit(-1);
	}
	cmd_ctx->codec_ctx = cmd_ctx->format_ctx->streams[cmd_ctx->video_stream_index]->codec;

	cmd_ctx->codec = avcodec_find_decoder(cmd_ctx->codec_ctx->codec_id);
	if (cmd_ctx->codec == NULL) {
		printf("can not find codec %d\n", cmd_ctx->codec_ctx->codec_id);
		exit(-1);
	}

	if (avcodec_open2(cmd_ctx->codec_ctx, cmd_ctx->codec, NULL) < 0) {
		printf("avcodec_open2 failed\n");
		exit(-1);
	}

	cmd_ctx->frame = av_frame_alloc();
	if (!cmd_ctx->frame) {
		printf("av_frame_alloc failed\n");
		exit(-1);
	}
}

void deinit_ffmpeg_context(struct cmd_context *cmd_ctx) {
	av_frame_free(&cmd_ctx->frame);
	avcodec_close(cmd_ctx->codec_ctx);
	avformat_close_input(&cmd_ctx->format_ctx);
}

int decode_one_frame(struct cmd_context *cmd_ctx) {
	AVPacket packet;
	int get_frame = 0;

	if (av_read_frame(cmd_ctx->format_ctx, &packet) >= 0) {
		//printf("read some packet index %d video_stream_index %d\n", packet.stream_index, cmd_ctx->video_stream_index);
		if (packet.stream_index == cmd_ctx->video_stream_index) {
			avcodec_decode_video2(cmd_ctx->codec_ctx, cmd_ctx->frame, &get_frame, &packet);
			get_fps(cmd_ctx);
		}
		//printf("after\n");
		av_free_packet(&packet);
	} else {
		get_frame = -1;
	}

	return get_frame;
}

void* decode_thread(void *arg) {
	struct cmd_context *cmd_ctx = (struct cmd_context *)arg;
	int get_frame = 0;

	do {
		pthread_mutex_lock(&cmd_ctx->list_mutex);
		if (get_frame) {
			while (cmd_ctx->cur_num == cmd_ctx->list_max_num) {
				pthread_cond_wait(&cmd_ctx->list_full_cond, &cmd_ctx->list_mutex);
			}
			struct dma_fd_list *dma_node = malloc(sizeof(struct dma_fd_list));
			INIT_LIST_HEAD(&dma_node->node);
			dma_node->width = (cmd_ctx->codec_ctx->width + 15) & (~15);
			dma_node->height = (cmd_ctx->codec_ctx->height + 15) & (~15);
			dma_node->dma_fd = dup(cmd_ctx->frame->data[3]);
			list_add_tail(&dma_node->node, &cmd_ctx->head);
			cmd_ctx->cur_num++;
			pthread_cond_broadcast(&cmd_ctx->list_empty_cond);
		}
		pthread_mutex_unlock(&cmd_ctx->list_mutex);
	} while ((get_frame = decode_one_frame(cmd_ctx)) != -1);

	pthread_mutex_lock(&cmd_ctx->list_mutex);
	cmd_ctx->quit_flag = 1;
	pthread_cond_broadcast(&cmd_ctx->list_empty_cond);
	pthread_mutex_unlock(&cmd_ctx->list_mutex);
}

void init_cmd_context(struct cmd_context *cmd_ctx) {
	/* default single thread */
	cmd_ctx->single_thread = 1;
	cmd_ctx->input_file = NULL;
	/* 0 means infinite */
	cmd_ctx->cycle_num = 1;

	/* ffmpeg parameter */
	cmd_ctx->format_ctx = NULL;
	cmd_ctx->codec_ctx = NULL;
	cmd_ctx->codec = NULL;
	cmd_ctx->frame = NULL;
	cmd_ctx->video_stream_index = -1;

	/* fps parameter */
	cmd_ctx->fps = 0;
	cmd_ctx->cl = 0;
	cmd_ctx->last_cl = 0;

	/* display parameter */
	cmd_ctx->dev = NULL;
	cmd_ctx->plane = NULL;
	cmd_ctx->test_crtc = NULL;
	cmd_ctx->test_plane = NULL;
	/* default no display */
	cmd_ctx->displayed = 0;

	/* default no save frame */
	cmd_ctx->record_frame = 0;

	/* multi thread parameter */
	INIT_LIST_HEAD(&cmd_ctx->head);
	cmd_ctx->list_max_num = 1;
	cmd_ctx->cur_num = 0;
	pthread_mutex_init(&cmd_ctx->list_mutex, NULL);
	pthread_cond_init(&cmd_ctx->list_empty_cond, NULL);
	pthread_cond_init(&cmd_ctx->list_full_cond, NULL);
	cmd_ctx->quit_flag = 0;

}

void deinit_cmd_context(struct cmd_context *cmd_ctx) {
	pthread_mutex_destroy(&cmd_ctx->list_mutex);
	pthread_cond_destroy(&cmd_ctx->list_empty_cond);
	pthread_cond_destroy(&cmd_ctx->list_full_cond);
}

void init_drm_context(struct cmd_context *cmd_ctx) {
	int ret, i;
	cmd_ctx->dev = create_sp_dev();
	if (!cmd_ctx->dev) {
		printf("create_sp_dev failed\n");
		exit(-1);
	}

	ret = initialize_screens(cmd_ctx->dev);
	if (ret) {
		printf("initialize_screens failed\n");
		exit(-1);
	}
	cmd_ctx->plane = calloc(cmd_ctx->dev->num_planes, sizeof(*cmd_ctx->plane));
	if (!cmd_ctx->plane) {
		printf("calloc plane array failed\n");
		exit(-1);;
	}

	cmd_ctx->test_crtc = &cmd_ctx->dev->crtcs[0];
	for (i = 0; i < cmd_ctx->test_crtc->num_planes; i++) {
		cmd_ctx->plane[i] = get_sp_plane(cmd_ctx->dev, cmd_ctx->test_crtc);
		if (is_supported_format(cmd_ctx->plane[i], DRM_FORMAT_NV12))
			cmd_ctx->test_plane = cmd_ctx->plane[i];
	}
	if (!cmd_ctx->test_plane) {
		printf("test_plane is NULL\n");
		exit(-1);
	}
}

int display_one_frame(struct cmd_context *cmd_ctx, struct dma_fd_list *node) {
	int ret;
	struct drm_mode_create_dumb cd;
	struct sp_bo *bo;
	uint32_t handles[4], pitches[4], offsets[4];

	bo = calloc(1, sizeof(*bo));
	if (!bo) {
		printf("calloc sp_bo failed\n");
		exit(-1);
	}

	ret = drmPrimeFDToHandle(cmd_ctx->dev->fd, node->dma_fd, &bo->handle);
	bo->dev = cmd_ctx->dev;
	bo->width = node->width;
	bo->height = node->height;
	bo->depth = 16;
	bo->bpp = 32;
	bo->format = DRM_FORMAT_NV12;
	bo->flags = 0;

	handles[0] = bo->handle;
	pitches[0] = node->width;
	offsets[0] = 0;
	handles[1] = bo->handle;
	pitches[1] = node->width;
	offsets[1] = node->width * node->height;
	
	ret = drmModeAddFB2(bo->dev->fd, bo->width, bo->height,
			    bo->format, handles, pitches, offsets,
			    &bo->fb_id, bo->flags);
	if (ret) {
		printf("failed to create fb ret=%d\n", ret);
		exit(-1);
	}

	ret = drmModeSetPlane(cmd_ctx->dev->fd, cmd_ctx->test_plane->plane->plane_id,
			      cmd_ctx->test_crtc->crtc->crtc_id, bo->fb_id, 0, 0, 0,
			      cmd_ctx->test_crtc->crtc->mode.hdisplay,
			      cmd_ctx->test_crtc->crtc->mode.vdisplay,
			      0, 0, bo->width << 16, bo->height << 16);
	if (ret) {
		printf("failed to set plane to crtc ret=%d\n", ret);
		exit(-1);

	}

	if (cmd_ctx->test_plane->bo) {
		if (cmd_ctx->test_plane->bo->fb_id) {
			ret = drmModeRmFB(cmd_ctx->dev->fd, cmd_ctx->test_plane->bo->fb_id);
			if (ret)
				printf("Failed to rmfb ret=%d!\n", ret);
		}
		if (cmd_ctx->test_plane->bo->handle) {
			struct drm_gem_close req = {
				.handle = cmd_ctx->test_plane->bo->handle,
			};

			drmIoctl(bo->dev->fd, DRM_IOCTL_GEM_CLOSE, &req);
		}
		free(cmd_ctx->test_plane->bo);
	}

	cmd_ctx->test_plane->bo = bo;

	return ret;

}

void parse_options(int argc, const char *argv[], struct cmd_context *cmd_ctx) {
	int opt;

	while ((opt = getopt(argc, argv, "t:i:m:c:r:d:")) != -1) {
		switch (opt) {
		case 't':
			cmd_ctx->single_thread = atoi(optarg) == 1 ? 0 : 1;
			break;
		case 'i':
			cmd_ctx->input_file = optarg;
			break;
		case 'm':
			cmd_ctx->list_max_num = atoi(optarg);
			break;
		case 'c':
			cmd_ctx->cycle_num = atoi(optarg);
			break;
		case 'r':
			cmd_ctx->record_frame = atoi(optarg);
			break;
		case 'd':
			cmd_ctx->displayed = atoi(optarg);
			break;
		}
	}

	printf("demo parameters setting:\n"); 
	printf("	input filename: %s\n", cmd_ctx->input_file); 
	printf("	single thread: %d\n", cmd_ctx->single_thread); 
	printf("	max list num: %d\n", cmd_ctx->list_max_num);
	printf("	cycle num: %d\n", cmd_ctx->cycle_num);
	printf("	display: %d\n", cmd_ctx->displayed);
	printf("	save output frame: %d\n", cmd_ctx->record_frame);
}

int main(int argc, const char *argv[])  {
	int ret;
	int count = 0;
	struct cmd_context cmd_ctx;

	init_cmd_context(&cmd_ctx);
	parse_options(argc, argv, &cmd_ctx);

	if (cmd_ctx.displayed) {
		init_drm_context(&cmd_ctx);
	}

	init_ffmpeg_context(&cmd_ctx);

	cmd_ctx.frame = avcodec_alloc_frame();
	if (cmd_ctx.frame == NULL) {
		printf("avcodec_alloc_frame failed\n");
		exit(-1);
	}

	struct timeval start_time, end_time;
	int cycle_num = cmd_ctx.cycle_num;
	do {
		if (cmd_ctx.single_thread == 0) {
			pthread_create(&cmd_ctx.pid, NULL, decode_thread, &cmd_ctx);
			while (cmd_ctx.quit_flag == 0) {
				pthread_mutex_lock(&cmd_ctx.list_mutex);
				while (cmd_ctx.cur_num == 0) {
					pthread_cond_wait(&cmd_ctx.list_empty_cond, &cmd_ctx.list_mutex);
					if (cmd_ctx.quit_flag == 1) {
						break;
					}
				}
				if (cmd_ctx.cur_num == 0) {
					pthread_mutex_unlock(&cmd_ctx.list_mutex);
					break;
				}
				struct dma_fd_list *dma_node = list_first_entry(&cmd_ctx.head, struct dma_fd_list, node);
				list_del(&dma_node->node);
				if (cmd_ctx.displayed)
					display_one_frame(&cmd_ctx, dma_node);
				close(dma_node->dma_fd);
				free(dma_node);
				cmd_ctx.cur_num--;
				pthread_cond_broadcast(&cmd_ctx.list_full_cond);
				pthread_mutex_unlock(&cmd_ctx.list_mutex);
			}

			while (!list_empty(&cmd_ctx.head)) {
				struct dma_fd_list *dma_node = list_first_entry(&cmd_ctx.head, struct dma_fd_list, node);
				list_del(&dma_node->node);
				display_one_frame(&cmd_ctx, dma_node);
				close(dma_node->dma_fd);
				free(dma_node);
			}

			pthread_join(cmd_ctx.pid, NULL);
		} else {
			int get_frame;
			gettimeofday(&start_time, NULL);
			while ((get_frame = decode_one_frame(&cmd_ctx)) != -1) {
				if (get_frame == 1) {
					struct dma_fd_list dma_node;
					dma_node.width = (cmd_ctx.codec_ctx->width + 15) & (~15);
					dma_node.height = (cmd_ctx.codec_ctx->height + 15) & (~15);
					dma_node.dma_fd = cmd_ctx.frame->data[3];

					if (count < cmd_ctx.record_frame) {
						save_frame(cmd_ctx.frame, dma_node.width, dma_node.height, count);
						count++;
					}

					if (cmd_ctx.displayed)
						display_one_frame(&cmd_ctx, &dma_node);
				}
			}
		}

		cycle_num--;
		if (cmd_ctx.cycle_num == 0) {
			cycle_num = 1;
		}
		if (cycle_num > 0) {
			if (avformat_seek_file(cmd_ctx.format_ctx, -1, INT64_MIN, 0, INT64_MAX, 0) < 0) {
				printf("seek failed\n");
				return -1;
			}
			avcodec_flush_buffers(cmd_ctx.codec_ctx);
			usleep(50000);
			cmd_ctx.quit_flag = 0;
			cmd_ctx.cur_num = 0;
		}
		printf("new cycle_num %d\n", cycle_num);
	} while (cycle_num);

	deinit_ffmpeg_context(&cmd_ctx);
	deinit_cmd_context(&cmd_ctx);
	return 0;
}
