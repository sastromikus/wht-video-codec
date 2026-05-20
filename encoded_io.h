#pragma once

#include "types.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

struct WalshChannelInfo {
    uint32_t blockCount = 0;

    std::size_t rleIntCount = 0;
    std::size_t rlePairCount = 0;

    uint64_t emptyBlocks = 0;
    uint64_t nonZeroTokens = 0;

    int maxAbsValue = 0;
    int maxZeroRun = 0;

    double averageNonZeroTokensPerBlock = 0.0;
};

struct WalshFileInfo {
    std::string path;

    uint32_t formatVersion = 0;
    uint32_t compressionMethod = 0;

    uint32_t rawPayloadSize = 0;
    uint32_t compressedPayloadSize = 0;

    std::size_t fileSize = 0;

    int width = 0;
    int height = 0;
    int channels = 0;
    int blockSize = 0;
    int quantStep = 0;

    bool isGrayscale = false;
    bool useYCbCr = false;

    std::vector<WalshChannelInfo> channelsInfo;
};

bool saveEncodedFrame(const std::string& path, const EncodedFrame& frame);
bool loadEncodedFrame(const std::string& path, EncodedFrame& frame);

// In-memory variants are used by the .wlsv container: one video file stores
// a sequence of regular Walsh frames without creating temporary .walsh files.
bool saveEncodedFrameToBytes(const EncodedFrame& frame, std::vector<uint8_t>& bytes);
bool loadEncodedFrameFromBytes(const std::vector<uint8_t>& bytes, EncodedFrame& frame);

bool readWalshFileInfo(const std::string& path, WalshFileInfo& info);