This demo depends on drm-rockchip:
https://github.com/yakir-Yang/libdrm-rockchip.git

Input below cmd to ensure you have right to access these two devices:

sudo chmod 660 /dev/vpu_service
sudo chmod 660 /dev/dri/card0

-t: 0 single thread; 1 enable multithread
-d: 0 no display; 1 display
-c: 0 forever running; n cycle n times
