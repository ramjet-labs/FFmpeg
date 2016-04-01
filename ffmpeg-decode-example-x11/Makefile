CFLAGS=-I/usr/local/include/libdrm -I/usr/local/include/ `pkg-config --cflags x11` `pkg-config --cflags xext` `pkg-config --cflags dri2` -g -O0
LIBS=-L/usr/local/lib `pkg-config --libs x11` `pkg-config --libs xext` `pkg-config --libs dri2` -lkms -ldrm_rockchip -ldrm -lavformat -lavcodec -lswscale -lavutil -ldl -lswresample -lpthread -lvpu -lion -lrk_codec -lvpu_avcdec -lvpu_avcenc -lvpu_mjpegdec -lvpu_mpeg4dec
LDFLAGS="-L/usr/lib -L/usr/local/lib"
C_SRC = $(wildcard *.c)
C_OBJ = $(patsubst %c, %o, $(C_SRC))
.PHONY:all clean
all: ffmpegexample
%o: %c
	$(CC) $(CFLAGS) -c $< 
ffmpegexample:$(C_OBJ)
	$(CC) -o $@ $^ $(LIBS) $(LDFLAGS) 
clean:
	rm *.o -f *.yuv
