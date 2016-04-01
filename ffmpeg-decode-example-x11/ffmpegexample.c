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
#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <sys/shm.h>
#include <X11/extensions/XShm.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>
#include <xf86drm.h>
#include <X11/Xlib.h>
#include <X11/Xmd.h>
#include <X11/extensions/dri2proto.h>
#include <X11/extensions/dri2.h>
#include <X11/Xlibint.h>
#include <X11/extensions/Xext.h>
#include <X11/extensions/extutil.h>
#include <stdint.h>
#include <xf86drmMode.h>
#include <drm_fourcc.h>
#include <rockchip_drm.h>
#include <rockchip_drmif.h>
#include <rockchip_rga.h>
#include <sys/mman.h>
#include "list.h"
#include "vpu_simple_mem_pool.h"

#define DMA_FD_QUEUE_LENGTH 20

#define DEMO_DEBUG(fmt, ...) \
	do { \
		if (getenv("RK_DEMO_DEBUG") != NULL && atoi(getenv("RK_DEMO_DEBUG")) == 1) { \
			printf("DEBUG:%s:%d " fmt "\n", __func__, __LINE__, ##__VA_ARGS__); \
		} \
	} while (0)

#define DEMO_ERROR(fmt, ...) \
	do { \
		printf("ERROR:%s:%d " fmt "\n", __func__, __LINE__, ##__VA_ARGS__); \
	} while (0)

#define DEMO_INFO(fmt, ...) \
	do { \
		printf("INFO:%s:%d " fmt "\n", __func__, __LINE__, ##__VA_ARGS__); \
	} while (0)


struct dma_fd_list {
	struct list_head node;
	int width;
	int height;
	int dma_fd;
};

struct rk_dma_buffer {
	int fd;
	int handle;
	uint8_t *vir_addr;
	int size;
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
	Display *display;
	Window win;
	struct rk_dma_buffer x11_dma_buffers[2];
	DRI2Buffer *dri2_bufs;
	int nbufs;
	int screen_width;
	int screen_height;
	int choose_screen;
	int displayed;

	/* rga display */
	struct rga_context *rga_ctx;

	/* /dev/dri/card0 fd with xserver authenticated */
	int authenticated_fd;

	/* libvpu mem pool parameter */
	struct vpu_simple_mem_pool *vpu_mem_pool;

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

void get_fps(struct cmd_context *cmd_ctx)
{

	struct timeval tv;
	gettimeofday(&tv, NULL);
	cmd_ctx->cl = tv.tv_sec + (double)tv.tv_usec / 1000000;
	if (!cmd_ctx->last_cl)
		cmd_ctx->last_cl = cmd_ctx->cl;
	if (cmd_ctx->cl - cmd_ctx->last_cl > 1.0f) {
		cmd_ctx->last_cl = cmd_ctx->cl;
		DEMO_INFO(">>>>>fps=%d\n", cmd_ctx->fps);
		cmd_ctx->fps = 1;
	} else {
		cmd_ctx->fps++;
	}
}

static void save_frame(AVFrame *pFrame, int width, int height, int iFrame)
{
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

void init_ffmpeg_context(struct cmd_context *cmd_ctx, void *mem_pool)
{
	int i;

	av_register_all();
	if (avformat_open_input(&cmd_ctx->format_ctx, cmd_ctx->input_file, NULL, NULL) != 0) {
		DEMO_DEBUG("avformat_open_input failed\n");
		exit(-1);
	}
	DEMO_DEBUG("0");

	if (avformat_find_stream_info(cmd_ctx->format_ctx, NULL) < 0) {
		DEMO_DEBUG("avformat_find_stream_info failed\n");
		exit(-1);
	}
	DEMO_DEBUG("1");

	av_dump_format(cmd_ctx->format_ctx, -1, cmd_ctx->input_file, 0);

	cmd_ctx->video_stream_index = -1;
	for (i = 0; i < cmd_ctx->format_ctx->nb_streams; i++) {
		if (cmd_ctx->format_ctx->streams[i]->codec->codec_type == AVMEDIA_TYPE_VIDEO) {
			cmd_ctx->video_stream_index = i;
			break;
		}
	}
	if (cmd_ctx->video_stream_index == -1) {
		DEMO_DEBUG("can not find video stream info\n");
		exit(-1);
	}
	cmd_ctx->codec_ctx = cmd_ctx->format_ctx->streams[cmd_ctx->video_stream_index]->codec;

	DEMO_DEBUG("2");
	cmd_ctx->codec = avcodec_find_decoder(cmd_ctx->codec_ctx->codec_id);
	if (cmd_ctx->codec == NULL) {
		DEMO_DEBUG("can not find codec %d\n", cmd_ctx->codec_ctx->codec_id);
		exit(-1);
	}

	cmd_ctx->codec_ctx->opaque = mem_pool;
	DEMO_DEBUG("codec ctx opaque %p", cmd_ctx->codec_ctx->opaque);
	if (avcodec_open2(cmd_ctx->codec_ctx, cmd_ctx->codec, NULL) < 0) {
		DEMO_DEBUG("avcodec_open2 failed\n");
		exit(-1);
	}

	cmd_ctx->frame = av_frame_alloc();
	if (!cmd_ctx->frame) {
		DEMO_DEBUG("av_frame_alloc failed\n");
		exit(-1);
	}
}

void deinit_ffmpeg_context(struct cmd_context *cmd_ctx)
{
	av_frame_free(&cmd_ctx->frame);
	avcodec_close(cmd_ctx->codec_ctx);
	avformat_close_input(&cmd_ctx->format_ctx);
}

int decode_one_frame(struct cmd_context *cmd_ctx)
{
	AVPacket packet;
	int get_frame = 0;

	if (av_read_frame(cmd_ctx->format_ctx, &packet) >= 0) {

		if (packet.stream_index == cmd_ctx->video_stream_index) {
			avcodec_decode_video2(cmd_ctx->codec_ctx, cmd_ctx->frame, &get_frame, &packet);
			get_fps(cmd_ctx);
		}

		av_free_packet(&packet);
	} else {
		get_frame = -1;
	}

	return get_frame;
}

void* decode_thread(void *arg)
{
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
			dma_node->dma_fd = cmd_ctx->frame->data[3];
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

void init_cmd_context(struct cmd_context *cmd_ctx)
{
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
	cmd_ctx->display = NULL;
	cmd_ctx->win = 0;
	memset(&cmd_ctx->x11_dma_buffers[0], 0, sizeof(cmd_ctx->x11_dma_buffers[0]));
	memset(&cmd_ctx->x11_dma_buffers[1], 0, sizeof(cmd_ctx->x11_dma_buffers[0]));
	cmd_ctx->dri2_bufs = NULL;
	cmd_ctx->nbufs = 0;
	cmd_ctx->screen_width = 0;
	cmd_ctx->screen_height = 0;
	cmd_ctx->choose_screen = 0;

	/* rga display */
	cmd_ctx->rga_ctx = NULL;

	/* /dev/dri/card0 fd with xserver authenticated */
	cmd_ctx->authenticated_fd = -1;

	/* libvpu mem pool parameter */
	cmd_ctx->vpu_mem_pool = NULL;
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

void deinit_cmd_context(struct cmd_context *cmd_ctx)
{
	pthread_mutex_destroy(&cmd_ctx->list_mutex);
	pthread_cond_destroy(&cmd_ctx->list_empty_cond);
	pthread_cond_destroy(&cmd_ctx->list_full_cond);
}

void init_x11_context(struct cmd_context *cmd_ctx)
{
	cmd_ctx->display = XOpenDisplay(getenv("DISPLAY"));

	/* Initial Window */
	cmd_ctx->win = XCreateSimpleWindow(cmd_ctx->display,
					   RootWindow(cmd_ctx->display, DefaultScreen(cmd_ctx->display)),
					   0, 0,
					   DisplayWidth(cmd_ctx->display, 0),
					   DisplayHeight(cmd_ctx->display, 0),
					   0,
					   BlackPixel(cmd_ctx->display, DefaultScreen(cmd_ctx->display)),
					   BlackPixel(cmd_ctx->display, DefaultScreen(cmd_ctx->display)));

	XMapWindow(cmd_ctx->display, cmd_ctx->win);
	XSync(cmd_ctx->display, False);

	XResizeWindow(cmd_ctx->display, cmd_ctx->win, 800, 600);
	XSync(cmd_ctx->display, False);
}

static int dri2_connect(Display *dpy, int fd, int driverType, char **driver)
{
	int eventBase, errorBase, major, minor;
	char *device;
	drm_magic_t magic;
	Window root;

	if (!DRI2InitDisplay(dpy, NULL)) {
		DEMO_ERROR("DRI2InitDisplay failed");
		return -1;
	}

	if (!DRI2QueryExtension(dpy, &eventBase, &errorBase)) {
		DEMO_ERROR("DRI2QueryExtension failed");
		return -1;
	}

	DEMO_DEBUG("DRI2QueryExtension: eventBase=%d, errorBase=%d", eventBase, errorBase);

	if (!DRI2QueryVersion(dpy, &major, &minor)) {
		DEMO_ERROR("DRI2QueryVersion failed");
		return -1;
	}

	DEMO_DEBUG("DRI2QueryVersion: major=%d, minor=%d", major, minor);

	root = RootWindow(dpy, DefaultScreen(dpy));

	if (!DRI2Connect(dpy, root, driverType, driver, &device)) {
		DEMO_ERROR("DRI2Connect failed");
		return -1;
	}

	DEMO_DEBUG("DRI2Connect: driver=%s, device=%s", *driver, device);

	if (drmGetMagic(fd, &magic)) {
		DEMO_ERROR("drmGetMagic failed");
		return -1;
	}

	if (!DRI2Authenticate(dpy, root, magic)) {
		DEMO_ERROR("DRI2Authenticate failed");
		return -1;
	}

	return fd;
}

int init_authenticated_fd(struct cmd_context *cmd_ctx)
{
	char *driver;

	cmd_ctx->authenticated_fd = open("/dev/dri/card0", O_RDWR);

	if (cmd_ctx->authenticated_fd < 0) {
		DEMO_ERROR("failed to open");
		return -1;
	}

	dri2_connect(cmd_ctx->display, cmd_ctx->authenticated_fd, DRI2DriverDRI, &driver);

	cmd_ctx->rga_ctx = rga_init(cmd_ctx->authenticated_fd);
	if (!cmd_ctx->rga_ctx) {
		DEMO_ERROR("rga init failed");
		return -1;
	}

	DEMO_DEBUG("driver name:%s", driver);
}

int get_x11_dma_buffer(struct cmd_context *cmd_ctx)
{
	int i;
	unsigned attachments[] = {
		0,
		1,
	};

	/* create X11 display buffer. in fact, they are dma_buffers */
	DRI2CreateDrawable(cmd_ctx->display, cmd_ctx->win);
	cmd_ctx->dri2_bufs = DRI2GetBuffers(cmd_ctx->display, cmd_ctx->win,
					    &cmd_ctx->screen_width,
					    &cmd_ctx->screen_height,
					    attachments, 2, &cmd_ctx->nbufs);

	DEMO_DEBUG("display width %d height %d nbufs %d", cmd_ctx->screen_width,
		   cmd_ctx->screen_height, cmd_ctx->nbufs);

	/* get x11 display buffer handle */
	for (i = 0; i < cmd_ctx->nbufs; i++) {
		int err;
		struct drm_gem_open req;

		DEMO_DEBUG("dri2_bufs[%d] name %u attachment %u flags %u cpp %u pitch %u",
			   i, cmd_ctx->dri2_bufs[i].names[0],
			   cmd_ctx->dri2_bufs[i].attachment,
			   cmd_ctx->dri2_bufs[i].flags,
			   cmd_ctx->dri2_bufs[i].cpp,
			   cmd_ctx->dri2_bufs[i].pitch[0]);

		req.name = cmd_ctx->dri2_bufs[i].names[0];
		int ret = drmIoctl(cmd_ctx->authenticated_fd, DRM_IOCTL_GEM_OPEN, &req);
		cmd_ctx->x11_dma_buffers[i].handle = req.handle;
		cmd_ctx->x11_dma_buffers[i].size = req.size;
		DEMO_DEBUG("ret %d dri2_bufs[%d] handle is %d size %lu", ret, i, req.handle, req.size);

		drmPrimeHandleToFD(cmd_ctx->authenticated_fd,
				   cmd_ctx->x11_dma_buffers[i].handle, 0, &cmd_ctx->x11_dma_buffers[i].fd);

		struct drm_mode_map_dumb dmmd;

		memset(&dmmd, 0, sizeof(dmmd));
		dmmd.handle = req.handle;
		err = drmIoctl(cmd_ctx->authenticated_fd, DRM_IOCTL_MODE_MAP_DUMB, &dmmd);
		if (err) {
			DEMO_ERROR("drm mode map failed");

			return err;
		}

		cmd_ctx->x11_dma_buffers[i].vir_addr = mmap(0, cmd_ctx->x11_dma_buffers[i].size,
							    PROT_READ | PROT_WRITE,
							    MAP_SHARED, cmd_ctx->authenticated_fd,
							    dmmd.offset);

		DEMO_DEBUG("x11_dma_buffers[%d].vir_addr %p", i, cmd_ctx->x11_dma_buffers[i].vir_addr);

		if (cmd_ctx->x11_dma_buffers[i].vir_addr == MAP_FAILED) {
			DEMO_ERROR("drm map failed");

			return err;
		}

	}
}

static int rga_convert_copy(struct rga_context *rga_ctx,
			    int src_fd,
			    int src_width,
			    int src_height,
			    uint32_t src_stride,
			    uint32_t src_fmt,
			    int dst_fd,
			    int dst_width,
			    int dst_height,
			    uint32_t dst_stride,
			    uint32_t dst_fmt,
			    enum e_rga_buf_type type)
{
	struct rga_image src_img = { 0 }, dst_img = { 0 };
	unsigned int img_w, img_h;

	dst_img.bo[0] = dst_fd;
	src_img.bo[0] = src_fd;

	/*
	 * Source Framebuffer OPS
	 */
	src_img.width = src_width;
	src_img.height = src_height;
	src_img.stride = src_stride;
	src_img.buf_type = type;
	src_img.color_mode = src_fmt;

	/*
	 * Dest Framebuffer OPS
	 */
	dst_img.width = dst_width;
	dst_img.height = dst_height;
	dst_img.stride = dst_stride;
	dst_img.buf_type = type;
	dst_img.color_mode = dst_fmt;

	DEMO_DEBUG("src fd %d stride %d dst stirde %d fd %d", src_fd, src_img.stride, dst_img.stride, dst_fd);

	/*
	 * RGA API Related:
	 *
	 * This code would SCALING the source framebuffer, and place
	 * the output to dest framebuffer, and the window size is:
	 *
	 * Source Window		Dest Window
	 * Start  -  End		Start  -  End
	 * (0, 0) -  (src_w, src_h)	(0.0)  -  (dst_w, dst_h)
	 */
	rga_multiple_transform(rga_ctx, &src_img, &dst_img,
			       0, 0, src_img.width, src_img.height,
			       0, 0, dst_img.width, dst_img.height,
			       0, 0, 0);
	rga_exec(rga_ctx);

	return 0;
}

int display_one_frame(struct cmd_context *cmd_ctx, struct dma_fd_list *node)
{
	int ret;
	unsigned long count;

	ret = rga_convert_copy(cmd_ctx->rga_ctx, node->dma_fd, node->width, node->height, node->width,
			       DRM_FORMAT_NV12,
			       cmd_ctx->x11_dma_buffers[cmd_ctx->choose_screen % cmd_ctx->nbufs].fd,
			       cmd_ctx->screen_width,
			       cmd_ctx->screen_height,
			       cmd_ctx->screen_width * 4,
			       DRM_FORMAT_ARGB8888,
			       RGA_IMGBUF_GEM);

	cmd_ctx->choose_screen++;
	DRI2SwapBuffers(cmd_ctx->display, cmd_ctx->win, 0, 0, 0, &count);

	DEMO_DEBUG("DRI2SwapBuffers: count = %lu", count);

	return ret;
}

static int drm_alloc(int drm_fd, size_t size, int *dma_fd)
{
	int err = 0;
	int map_fd, handle;

	if (size < 4096)
		size = 4096;

	struct drm_mode_create_dumb dmcb;
	memset(&dmcb, 0, sizeof(dmcb));
	dmcb.bpp = 4096;
	dmcb.width = ((size + 4095) & (~4095)) >> 12;
	dmcb.height = 8;
	dmcb.size = dmcb.width * dmcb.bpp;

	err = drmIoctl(drm_fd, DRM_IOCTL_MODE_CREATE_DUMB, &dmcb);
	handle = dmcb.handle;
	size = dmcb.size;

	if (err) {
		DEMO_DEBUG("drm alloc failed\n");

		return err;
	}

	err = drmPrimeHandleToFD(drm_fd, handle, 0, &map_fd);
	if (err) {
		DEMO_DEBUG("prime handle to dma fd failed\n");

		struct drm_mode_destroy_dumb dmdb;
		dmdb.handle = handle;
		drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dmdb);

		return err;
	}

#if 0
	struct drm_mode_map_dumb dmmd;
	memset(&dmmd, 0, sizeof(dmmd));
	dmmd.handle = handle;
	err = drmIoctl(drm_fd, DRM_IOCTL_MODE_MAP_DUMB, &dmmd);
	if (err) {
		VPU_DEMO_LOG("drm mode map failed\n");
		struct drm_mode_destroy_dumb dmdb;
		dmdb.handle = handle;
		drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dmdb);

		return err;
	}

	dmabuf->vir_addr = mmap(0, size, PROT_READ | PROT_WRITE, MAP_SHARED, drm_fd, dmmd.offset);

	if (dmabuf->vir_addr == MAP_FAILED) {
		DMABUF_ERR("drm map failed\n");
		struct drm_mode_destroy_dumb dmdb;
		dmdb.handle = handle;
		drmIoctl(drm_fd, DRM_IOCTL_MODE_DESTROY_DUMB, &dmdb);
		free(dmabuf);
		return err;
	}
#endif

	*dma_fd = map_fd;

	return size;
}

int open_vpu_mem_pool(struct cmd_context *cmd_ctx)
{
	int i;

	cmd_ctx->vpu_mem_pool = open_vpu_simple_mem_pool(cmd_ctx->authenticated_fd);
	if (cmd_ctx->vpu_mem_pool == NULL) {
		DEMO_ERROR("open vpu mem pool failed");

		return -1;
	}

	for (i = 0; i < 23; i++) {
		int fd;
		int size = drm_alloc(cmd_ctx->authenticated_fd,
				     1920 * 1080 * 4,
				     &fd);

		cmd_ctx->vpu_mem_pool->commit_hdl(cmd_ctx->vpu_mem_pool, fd, size);
		close(fd);
	}

	return 0;
}

void parse_options(int argc, const char *argv[], struct cmd_context *cmd_ctx)
{
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

int main(int argc, const char *argv[])
{
	int ret;
	int count = 0;
	struct cmd_context cmd_ctx;

	init_cmd_context(&cmd_ctx);
	parse_options(argc, argv, &cmd_ctx);

	if (cmd_ctx.displayed) {
		init_x11_context(&cmd_ctx);
		init_authenticated_fd(&cmd_ctx);
		get_x11_dma_buffer(&cmd_ctx);
		open_vpu_mem_pool(&cmd_ctx);

	} else {
		cmd_ctx.vpu_mem_pool = NULL;
	}

	init_ffmpeg_context(&cmd_ctx, cmd_ctx.vpu_mem_pool);

	cmd_ctx.frame = avcodec_alloc_frame();
	if (cmd_ctx.frame == NULL) {
		DEMO_DEBUG("avcodec_alloc_frame failed\n");
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
				if (cmd_ctx.displayed) {
					display_one_frame(&cmd_ctx, dma_node);
					cmd_ctx.vpu_mem_pool->put_used(cmd_ctx.vpu_mem_pool, dma_node->dma_fd);
				}

				free(dma_node);
				cmd_ctx.cur_num--;
				pthread_cond_broadcast(&cmd_ctx.list_full_cond);
				pthread_mutex_unlock(&cmd_ctx.list_mutex);
			}

			while (!list_empty(&cmd_ctx.head)) {
				struct dma_fd_list *dma_node = list_first_entry(&cmd_ctx.head, struct dma_fd_list, node);
				list_del(&dma_node->node);
				if (cmd_ctx.displayed) {
					display_one_frame(&cmd_ctx, dma_node);
					cmd_ctx.vpu_mem_pool->put_used(cmd_ctx.vpu_mem_pool, dma_node->dma_fd);
				} else {
					close(dma_node->dma_fd);
				}
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

					DEMO_DEBUG("width %d height %d fd %d", dma_node.width, dma_node.height, dma_node.dma_fd);
					if (count < cmd_ctx.record_frame) {
						save_frame(cmd_ctx.frame, dma_node.width, dma_node.height, count);
						count++;
					}

					if (cmd_ctx.displayed) {
						display_one_frame(&cmd_ctx, &dma_node);
						cmd_ctx.vpu_mem_pool->put_used(cmd_ctx.vpu_mem_pool, dma_node.dma_fd);
					}
				}
				//getchar();
			}
		}

		cycle_num--;
		if (cmd_ctx.cycle_num == 0) {
			cycle_num = 1;
		}
		if (cycle_num > 0) {
			if (avformat_seek_file(cmd_ctx.format_ctx, -1, INT64_MIN, 0, INT64_MAX, 0) < 0) {
				DEMO_DEBUG("seek failed\n");
				return -1;
			}
			avcodec_flush_buffers(cmd_ctx.codec_ctx);
			usleep(50000);
			cmd_ctx.quit_flag = 0;
			cmd_ctx.cur_num = 0;
		}
		DEMO_DEBUG("new cycle_num %d\n", cycle_num);
	} while (cycle_num);

	/* todo:need add deinit function here */

	deinit_ffmpeg_context(&cmd_ctx);
	deinit_cmd_context(&cmd_ctx);
	return 0;
}
