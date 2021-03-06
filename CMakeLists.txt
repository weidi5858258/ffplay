#cmake_minimum_required(VERSION 3.9)
cmake_minimum_required(VERSION 3.15)
project(ffplay)

set(CMAKE_CXX_STANDARD 14)

#set(CMAKE_CXX_STANDARD 11)
# 使用ffplay的代码时需要注释掉
add_definitions(-std=c++11)

include_directories(.)

add_executable(
        ffplay

        url.h
        avio.h
        libm.h
        config.h
        network.h
        va_copy.h
        cmdutils.h
        cmdutils.c
        intfloat.h
        rational.h
        attributes.h
        os_support.h
        mathematics.h

        # 自己新增的文件
        ffplay.cpp # ubuntu中测试
        FFplayer.h
#        FFplayer.cpp # 移植到android去
)
# 要包含的头文件
include_directories(
        /usr/local/include
#        /usr/local/include/lame
#        /usr/local/Cellar/sdl2/2.0.10/include/SDL2
#        /Users/alexander/mydev/tools/audio_video/ffmpeg_macos/include
        /usr/include/SDL2
        /root/mydev/tools/ffmpeg/ffmpeg_ubuntu/include
)
# 要包含的ffmpeg lib库
set(FFMPEG_LIB_PATH
#        /Users/alexander/mydev/tools/audio_video/ffmpeg_macos/lib
        /root/mydev/tools/ffmpeg/ffmpeg_ubuntu/lib
)
# 要包含的sdl2 lib库
set(SDL2_LIB_PATH
#        /usr/local/Cellar/sdl2/2.0.10/lib
        /usr/lib/x86_64-linux-gnu
)

target_link_libraries(
        ffplay

        -L${FFMPEG_LIB_PATH} -lavformat
        -L${FFMPEG_LIB_PATH} -lavcodec
        -L${FFMPEG_LIB_PATH} -lavfilter
        -L${FFMPEG_LIB_PATH} -lavutil
#        -L${FFMPEG_LIB_PATH} -lavresample
        -L${FFMPEG_LIB_PATH} -lswresample
        -L${FFMPEG_LIB_PATH} -lswscale
        -L${FFMPEG_LIB_PATH} -lavdevice
        -L${FFMPEG_LIB_PATH} -lpostproc
        -L${SDL2_LIB_PATH}   -lSDL2
        -L${SDL2_LIB_PATH}   -lSDL2main
        -lpthread
        -ldl
        -lm

#        -L/usr/local/Cellar/x264/r2917_1/lib -lx264
#        -L/usr/local/Cellar/x265/3.3/lib -lx265
        -L/usr/local/Cellar/x264/r2917_1/lib -lx264
        -L/usr/local/Cellar/x265/3.3/lib -lx265
)
