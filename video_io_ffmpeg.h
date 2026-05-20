#pragma once

#include "types.h"

#include <functional>
#include <string>

struct VideoInputInfo {
    int width = 0;
    int height = 0;
    double sourceFps = 0.0;
    int selectedFrames = 0;
};

// Called for every selected RGB frame. Return false to stop reading with an error.
using VideoFrameCallback = std::function<bool(const Image& frame, int selectedIndex)>;

// Reads a video file through FFmpeg libraries and selects frames with the requested FPS.
// FFmpeg is used only as container/codec I/O; Walsh processing stays in codec.cpp.
bool readVideoFramesFFmpeg(
    const std::string& path,
    int targetFps,
    const VideoFrameCallback& onFrame,
    VideoInputInfo& info,
    std::string& errorMessage);

class FFmpegVideoWriter {
public:
    FFmpegVideoWriter() = default;
    ~FFmpegVideoWriter();

    FFmpegVideoWriter(const FFmpegVideoWriter&) = delete;
    FFmpegVideoWriter& operator=(const FFmpegVideoWriter&) = delete;

    bool open(
        const std::string& path,
        int width,
        int height,
        int fps,
        std::string& errorMessage);

    bool writeFrame(const Image& rgbImage, std::string& errorMessage);
    bool close(std::string& errorMessage);

    int writtenFrames() const { return writtenFrames_; }

private:
    struct Impl;

    void release();

    Impl* impl_ = nullptr;
    int width_ = 0;
    int height_ = 0;
    int fps_ = 1;
    int writtenFrames_ = 0;
};
