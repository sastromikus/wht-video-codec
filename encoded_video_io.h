#pragma once

#include "types.h"

#include <cstdint>
#include <string>
#include <vector>

struct WalshVideoHeader {
    uint32_t version = 1;

    int width = 0;
    int height = 0;
    int fps = 1;
    int frameCount = 0;

    int blockSize = 8;
    int quantStep = 16;
    bool isGrayscale = false;
    bool useYCbCr = true;
};

// Simple intra-only Walsh-video container.
// Each frame is stored as a serialized EncodedFrame payload, so the image codec
// can be reused without introducing a second WHT implementation.
class WalshVideoWriter {
public:
    bool open(const std::string& path, const WalshVideoHeader& header);
    bool writeFrame(const EncodedFrame& frame);
    bool close();

    int frameCount() const { return frameCount_; }

private:
    std::string path_;
    WalshVideoHeader header_{};

    // First implementation keeps frame payloads in memory and writes final
    // frameCount in the header on close(). This is simple and sufficient for
    // short 1 FPS diploma tests; large videos should later switch to offsets.
    std::vector<std::vector<uint8_t>> frameBytes_;

    int frameCount_ = 0;
    bool opened_ = false;
};

class WalshVideoReader {
public:
    bool open(const std::string& path);
    bool readNextFrame(EncodedFrame& frame);

    const WalshVideoHeader& header() const { return header_; }
    int currentFrameIndex() const { return currentFrameIndex_; }

private:
    WalshVideoHeader header_{};
    std::vector<std::vector<uint8_t>> frameBytes_;
    int currentFrameIndex_ = 0;
    bool opened_ = false;
};

bool isWalshVideoFile(const std::string& path);
