#cmake_minimum_required(VERSION 3.9)
cmake_minimum_required(VERSION 3.15)
project(ffplay)

set(CMAKE_CXX_STANDARD 14)

#error: invalid conversion from ‘const AVCodec*’ to ‘AVCodec*’ [-fpermissive]
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} -std=c++11 -fpermissive -g")

#set(CMAKE_CXX_STANDARD 11)
#使用ffplay的代码时需要注释掉
#add_definitions(-std=c++11)

include_directories(.)

add_executable(
        ffplay

        ffplay.cpp
        cmdutils.c
)

#要包含的头文件
include_directories(
        # macOS
        /usr/local/Cellar/ffmpeg/4.4.1_3/include
        /usr/local/Cellar/sdl2/2.0.18/include/SDL2
        # Ubuntu
        # /root/mydev/tools/ffmpeg/ffmpeg_ubuntu/ffmpeg-n4.4-152-gdf288deb9b-linux64-gpl-shared-4.4/include
        # /usr/include/SDL2
)

# macOS
set(FFMPEG_LIB_PATH /usr/local/Cellar/ffmpeg/4.4.1_3/lib)
set(SDL2_LIB_PATH /usr/local/Cellar/sdl2/2.0.18/lib)
# Ubuntu
# set(FFMPEG_LIB_PATH /root/mydev/tools/ffmpeg/ffmpeg_ubuntu/ffmpeg-n4.4-152-gdf288deb9b-linux64-gpl-shared-4.4/lib)
# set(SDL2_LIB_PATH /usr/lib/x86_64-linux-gnu)
target_link_libraries(
        ffplay

        -L${FFMPEG_LIB_PATH} -lavformat
        -L${FFMPEG_LIB_PATH} -lavcodec
        -L${FFMPEG_LIB_PATH} -lavfilter
        -L${FFMPEG_LIB_PATH} -lavutil
        -L${FFMPEG_LIB_PATH} -lswresample
        -L${FFMPEG_LIB_PATH} -lswscale
        -L${FFMPEG_LIB_PATH} -lavdevice
        -L${FFMPEG_LIB_PATH} -lpostproc
        -L${SDL2_LIB_PATH}   -lSDL2
        -L${SDL2_LIB_PATH}   -lSDL2main

        -lpthread
        -ldl
        -lm
)
