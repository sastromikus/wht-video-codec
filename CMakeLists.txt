cmake_minimum_required(VERSION 3.20)

project(walsh_video_codec LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(diplom
    main.cpp
    codec.cpp
    encoded_io.cpp
    encoded_video_io.cpp
    image_io.cpp
    metrics.cpp
    test_runner.cpp
    video_io_ffmpeg.cpp
    wht.cpp
)

target_include_directories(diplom PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

find_package(ZLIB REQUIRED)
target_link_libraries(diplom PRIVATE ZLIB::ZLIB)

# FFmpeg is used only for video container/codec input-output. The Walsh codec
# itself is implemented in codec.cpp and works with already decoded RGB frames.
find_path(FFMPEG_INCLUDE_DIR libavformat/avformat.h REQUIRED)
find_library(FFMPEG_AVFORMAT_LIBRARY avformat REQUIRED)
find_library(FFMPEG_AVCODEC_LIBRARY avcodec REQUIRED)
find_library(FFMPEG_AVUTIL_LIBRARY avutil REQUIRED)
find_library(FFMPEG_SWSCALE_LIBRARY swscale REQUIRED)
find_library(FFMPEG_SWRESAMPLE_LIBRARY swresample REQUIRED)

target_include_directories(diplom PRIVATE ${FFMPEG_INCLUDE_DIR})
target_link_libraries(diplom PRIVATE
    ${FFMPEG_AVFORMAT_LIBRARY}
    ${FFMPEG_AVCODEC_LIBRARY}
    ${FFMPEG_AVUTIL_LIBRARY}
    ${FFMPEG_SWSCALE_LIBRARY}
    ${FFMPEG_SWRESAMPLE_LIBRARY}
)

if (MSVC)
    target_compile_options(diplom PRIVATE /W4 /permissive-)
endif()
