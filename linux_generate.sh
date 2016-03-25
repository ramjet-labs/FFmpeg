#!/bin/bash

PREFIX=out
TOOLCHAINS=/home/zhaojun/gcc-linaro-arm-linux-gnueabihf-4.7-2013.04-20130415_linux
CROSS_COMPILE=${TOOLCHAINS}/bin/arm-linux-gnueabihf-

LOCAL_PATH=`pwd`
CFLAGS="-Wall -marm -pipe -fpic \
  -finline-limit=300 -ffast-math \
  -fstrict-aliasing -Werror=strict-aliasing \
  -fmodulo-sched -fmodulo-sched-allow-regmoves \
  -Wno-psabi -Wa,--noexecstack"

EXTRA_CFLAGS=

EXTRA_LDFLAGS=

FFMPEG_FLAGS="--prefix=${PREFIX} \
  --target-os=linux \
  --arch=arm \
  --enable-cross-compile \
  --cross-prefix=$TOOLCHAINS/bin/arm-linux-gnueabihf- \
  --enable-shared \
  --disable-symver \
  --disable-doc \
  --disable-ffplay \
  --disable-ffmpeg \
  --disable-ffprobe \
  --disable-ffserver \
  --disable-avdevice \
  --disable-avfilter \
  --disable-muxers \
  --disable-filters \
  --disable-devices \
  --disable-everything \
  --enable-protocols  \
  --enable-parsers \
  --enable-demuxers \
  --disable-demuxer=sbg \
  --enable-decoders \
  --enable-bsfs \
  --enable-network \
  --enable-swscale  \
  --enable-neon \
  --enable-version3 \
  --disable-asm"

#--enable-debug \
#--disable-stripping \
#--disable-optimizations \

./configure $FFMPEG_FLAGS --extra-cflags="$CFLAGS $EXTRA_CFLAGS" --extra-ldflags="$LDFLAGS $EXTRA_LDFLAGS"

make -j8 && make install
