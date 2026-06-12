cmake_minimum_required(VERSION 3.20)

if(POLICY CMP0091)
    cmake_policy(SET CMP0091 NEW)
endif()

project(walsh_video_codec LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    set(CMAKE_MSVC_RUNTIME_LIBRARY
        "MultiThreaded$<$<CONFIG:Debug>:Debug>"
    )
endif()

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

target_include_directories(diplom PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
)

find_package(ZLIB REQUIRED)
target_link_libraries(diplom PRIVATE ZLIB::ZLIB)

# FFmpeg is used only for video container/codec input-output.
# The Walsh codec itself is implemented in codec.cpp and works
# with already decoded RGB frames.
find_path(FFMPEG_INCLUDE_DIR
    NAMES libavformat/avformat.h
    REQUIRED
)

find_library(FFMPEG_AVFORMAT_LIBRARY
    NAMES avformat
    REQUIRED
)

find_library(FFMPEG_AVCODEC_LIBRARY
    NAMES avcodec
    REQUIRED
)

find_library(FFMPEG_AVUTIL_LIBRARY
    NAMES avutil
    REQUIRED
)

find_library(FFMPEG_SWSCALE_LIBRARY
    NAMES swscale
    REQUIRED
)

find_library(FFMPEG_SWRESAMPLE_LIBRARY
    NAMES swresample
    REQUIRED
)

target_include_directories(diplom PRIVATE
    ${FFMPEG_INCLUDE_DIR}
)

target_link_libraries(diplom PRIVATE
    ${FFMPEG_AVFORMAT_LIBRARY}
    ${FFMPEG_AVCODEC_LIBRARY}
    ${FFMPEG_AVUTIL_LIBRARY}
    ${FFMPEG_SWSCALE_LIBRARY}
    ${FFMPEG_SWRESAMPLE_LIBRARY}
)

if(WIN32)
    target_link_libraries(diplom PRIVATE
        ws2_32
        secur32
        bcrypt
        mfplat
        mfuuid
        strmiids
        ole32
        oleaut32
        uuid
    )
endif()

if(MSVC)
    target_compile_options(diplom PRIVATE
        /W4
        /permissive-
    )
endif()
