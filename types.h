#pragma once

#include <cstdint>
#include <vector>

struct Image {
    int width = 0;
    int height = 0;
    int channels = 0;
    std::vector<uint8_t> data;
};

struct EncodedChannel {
    std::vector<int> rleData;
    std::vector<uint32_t> blockOffsets;
};

struct ChannelStats {
    int totalBlocks = 0;
    int totalCoefficients = 0;
    int zeroCoefficients = 0;
    int nonZeroCoefficients = 0;

    int maxAbsQuantizedValue = 0;
    int maxZeroRun = 0;
};

struct EncodeStats {
    std::vector<ChannelStats> channels;
};

struct EncodedFrame {
    int width = 0;
    int height = 0;
    int channels = 0;
    int blockSize = 0;
    int quantStep = 0;
    bool isGrayscale = true;
    bool useYCbCr = false;

    std::vector<EncodedChannel> channelData;
};

struct CodecParams {
    int blockSize = 8;
    int quantStep = 16;
    bool isGrayscale = true;
    bool printStats = false;
    bool useYCbCr = true;
};