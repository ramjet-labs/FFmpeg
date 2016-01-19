CROSS_COMPILE = /home/zj/gcc-linaro-arm-linux-gnueabihf-4.7-2013.04-20130415_linux/bin/arm-linux-gnueabihf-

SYSROOT = /home/zj/gcc-linaro-arm-linux-gnueabihf-4.7-2013.04-20130415_linux/arm-linux-gnueabihf/libc
CC = $(CROSS_COMPILE)gcc --sysroot=$(SYSROOT)
CPP = $(CROSS_COMPILE)g++ --sysroot=$(SYSROOT)
AR = $(CROSS_COMPILE)ar
MAKE = make

ffmpegexample:ffmpegexample.c
	$(CC) ffmpegexample.c -g -o ffmpegexample -I/home/zj/project/rk_ffmpeg_2.6.2/out/include -L/home/zj/project/rk_ffmpeg_2.6.2/out/lib -lavformat -lavcodec -lswscale -lavutil -ldl -lswresample
clean:
	@rm ffmpegexample *.ppm
