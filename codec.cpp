#include "codec.h"
#include "wht.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace {
    int clampInt(int value, int minValue, int maxValue) {
        return std::max(minValue, std::min(value, maxValue));
    }

    bool isPowerOfTwo(int value) {
        return value > 0 && (value & (value - 1)) == 0;
    }

    int quantizeValue(int value, int quantStep) {
        const int absValue = value < 0 ? -value : value;
        const int rounded = (absValue * 2 + quantStep) / (2 * quantStep);

        return value < 0 ? -rounded : rounded;
    }

    int dequantizeValue(int value, int quantStep) {
        return value * quantStep;
    }

    static constexpr int ZIGZAG_8X8[64] = {
        0,
        1, 8,
        16, 9, 2,
        3, 10, 17, 24,
        32, 25, 18, 11, 4,
        5, 12, 19, 26, 33, 40,
        48, 41, 34, 27, 20, 13, 6,
        7, 14, 21, 28, 35, 42, 49, 56,
        57, 50, 43, 36, 29, 22, 15,
        23, 30, 37, 44, 51, 58,
        59, 52, 45, 38, 31,
        39, 46, 53, 60,
        61, 54, 47,
        55, 62,
        63
    };

    void zigZagReorder8x8Into(const std::vector<int>& block, std::vector<int>& result) {
        if (block.size() != 64) {
            throw std::runtime_error("zigZagReorder8x8Into: block size must be 64");
        }

        result.resize(64);

        for (int i = 0; i < 64; ++i) {
            result[static_cast<std::size_t>(i)] =
                block[static_cast<std::size_t>(ZIGZAG_8X8[i])];
        }
    }

    void inverseZigZagReorder8x8Into(const std::vector<int>& data, std::vector<int>& result) {
        if (data.size() != 64) {
            throw std::runtime_error("inverseZigZagReorder8x8Into: block size must be 64");
        }

        result.resize(64);

        for (int i = 0; i < 64; ++i) {
            result[static_cast<std::size_t>(ZIGZAG_8X8[i])] =
                data[static_cast<std::size_t>(i)];
        }
    }

    void applyFrequencyPruning8x8(std::vector<int>& orderedBlock, int quantStep) {
        if (orderedBlock.size() != 64) {
            throw std::runtime_error("applyFrequencyPruning8x8: block size must be 64");
        }

        if (quantStep >= 16) {
            for (int i = 16; i < 64; ++i) {
                if (std::abs(orderedBlock[static_cast<std::size_t>(i)]) <= 1) {
                    orderedBlock[static_cast<std::size_t>(i)] = 0;
                }
            }

            for (int i = 32; i < 64; ++i) {
                if (std::abs(orderedBlock[static_cast<std::size_t>(i)]) <= 2) {
                    orderedBlock[static_cast<std::size_t>(i)] = 0;
                }
            }
        }

        if (quantStep >= 32) {
            for (int i = 24; i < 64; ++i) {
                if (std::abs(orderedBlock[static_cast<std::size_t>(i)]) <= 2) {
                    orderedBlock[static_cast<std::size_t>(i)] = 0;
                }
            }

            for (int i = 40; i < 64; ++i) {
                if (std::abs(orderedBlock[static_cast<std::size_t>(i)]) <= 3) {
                    orderedBlock[static_cast<std::size_t>(i)] = 0;
                }
            }
        }
    }

    void encodeRLEInto(const std::vector<int>& block, std::vector<int>& result, ChannelStats* stats) {
        result.clear();
        result.reserve(block.size() * 2 + 2);

        if (stats != nullptr) {
            stats->totalBlocks += 1;
            stats->totalCoefficients += static_cast<int>(block.size());
        }

        int zeroCount = 0;

        for (int value : block) {
            if (value == 0) {
                ++zeroCount;

                if (stats != nullptr) {
                    stats->zeroCoefficients += 1;
                }

                continue;
            }

            if (stats != nullptr) {
                stats->nonZeroCoefficients += 1;

                const int absValue = std::abs(value);
                if (absValue > stats->maxAbsQuantizedValue) {
                    stats->maxAbsQuantizedValue = absValue;
                }

                if (zeroCount > stats->maxZeroRun) {
                    stats->maxZeroRun = zeroCount;
                }
            }

            result.push_back(zeroCount);
            result.push_back(value);
            zeroCount = 0;
        }

        if (stats != nullptr && zeroCount > stats->maxZeroRun) {
            stats->maxZeroRun = zeroCount;
        }

        result.push_back(zeroCount);
        result.push_back(0);
    }

    int getPrunedValue8x8Q16(int value, int zigzagIndex) {
        int threshold = 0;

        if (zigzagIndex >= 32) {
            threshold = 2;
        } else if (zigzagIndex >= 16) {
            threshold = 1;
        }

        if (threshold > 0 && std::abs(value) <= threshold) {
            return 0;
        }

        return value;
    }

    int getPrunedValue8x8Q32(int value, int zigzagIndex) {
        int threshold = 0;

        if (zigzagIndex >= 40) {
            threshold = 3;
        } else if (zigzagIndex >= 24) {
            threshold = 2;
        } else if (zigzagIndex >= 16) {
            threshold = 1;
        }

        if (threshold > 0 && std::abs(value) <= threshold) {
            return 0;
        }

        return value;
    }

    void reserveEncodedChannel(EncodedChannel& channel, int blockCount, int blockSize, int quantStep) {
        channel.rleData.clear();
        channel.blockOffsets.clear();

        channel.blockOffsets.reserve(static_cast<std::size_t>(blockCount) + 1);

        std::size_t estimatedIntsPerBlock = 12;

        if (blockSize != 8) {
            estimatedIntsPerBlock = 18;
        } else if (quantStep < 16) {
            estimatedIntsPerBlock = 28;
        } else if (quantStep < 32) {
            estimatedIntsPerBlock = 16;
        } else {
            estimatedIntsPerBlock = 10;
        }

        channel.rleData.reserve(
            static_cast<std::size_t>(blockCount) * estimatedIntsPerBlock);
    }

    void beginFlatBlock(EncodedChannel& channel) {
        if (channel.rleData.size() > std::numeric_limits<uint32_t>::max()) {
            throw std::runtime_error("beginFlatBlock: RLE stream is too large");
        }

        channel.blockOffsets.push_back(
            static_cast<uint32_t>(channel.rleData.size()));
    }

    void finishFlatChannel(EncodedChannel& channel) {
        if (channel.rleData.size() > std::numeric_limits<uint32_t>::max()) {
            throw std::runtime_error("finishFlatChannel: RLE stream is too large");
        }

        channel.blockOffsets.push_back(
            static_cast<uint32_t>(channel.rleData.size()));
    }

    void appendRLEPair(EncodedChannel& channel, int zeroCount, int value) {
        channel.rleData.push_back(zeroCount);
        channel.rleData.push_back(value);
    }

    void appendRLEIntoFlat(const std::vector<int>& block, EncodedChannel& channel, ChannelStats* stats) {
        if (stats != nullptr) {
            stats->totalBlocks += 1;
            stats->totalCoefficients += static_cast<int>(block.size());
        }

        int zeroCount = 0;

        for (int value : block) {
            if (value == 0) {
                ++zeroCount;

                if (stats != nullptr) {
                    stats->zeroCoefficients += 1;
                }

                continue;
            }

            if (stats != nullptr) {
                stats->nonZeroCoefficients += 1;

                const int absValue = std::abs(value);
                if (absValue > stats->maxAbsQuantizedValue) {
                    stats->maxAbsQuantizedValue = absValue;
                }

                if (zeroCount > stats->maxZeroRun) {
                    stats->maxZeroRun = zeroCount;
                }
            }

            appendRLEPair(channel, zeroCount, value);
            zeroCount = 0;
        }

        if (stats != nullptr && zeroCount > stats->maxZeroRun) {
            stats->maxZeroRun = zeroCount;
        }

        appendRLEPair(channel, zeroCount, 0);
    }

    void encodeRLE8x8FromBlockFlat(const std::vector<int>& block, EncodedChannel& channel, int quantStep, int& prevDC, ChannelStats* stats) {
        if (block.size() != 64) {
            throw std::runtime_error("encodeRLE8x8FromBlockFlat: block size must be 64");
        }

        if (stats != nullptr) {
            stats->totalBlocks += 1;
            stats->totalCoefficients += 64;
        }

        const auto writeValue = [&](int value, int& zeroCount) {
            if (value == 0) {
                ++zeroCount;

                if (stats != nullptr) {
                    stats->zeroCoefficients += 1;
                }

                return;
            }

            if (stats != nullptr) {
                stats->nonZeroCoefficients += 1;

                const int absValue = std::abs(value);
                if (absValue > stats->maxAbsQuantizedValue) {
                    stats->maxAbsQuantizedValue = absValue;
                }

                if (zeroCount > stats->maxZeroRun) {
                    stats->maxZeroRun = zeroCount;
                }
            }

            appendRLEPair(channel, zeroCount, value);
            zeroCount = 0;
        };

        const int currentDC = block[0];
        const int dcDelta = currentDC - prevDC;
        prevDC = currentDC;

        int zeroCount = 0;

        writeValue(dcDelta, zeroCount);

        if (quantStep < 16) {
            for (int i = 1; i < 64; ++i) {
                const int value = block[static_cast<std::size_t>(ZIGZAG_8X8[i])];
                writeValue(value, zeroCount);
            }
        } else if (quantStep < 32) {
            for (int i = 1; i < 64; ++i) {
                int value = block[static_cast<std::size_t>(ZIGZAG_8X8[i])];
                value = getPrunedValue8x8Q16(value, i);
                writeValue(value, zeroCount);
            }
        } else {
            for (int i = 1; i < 64; ++i) {
                int value = block[static_cast<std::size_t>(ZIGZAG_8X8[i])];
                value = getPrunedValue8x8Q32(value, i);
                writeValue(value, zeroCount);
            }
        }

        if (stats != nullptr && zeroCount > stats->maxZeroRun) {
            stats->maxZeroRun = zeroCount;
        }

        appendRLEPair(channel, zeroCount, 0);
    }

    void decodeRLERangeInto(const std::vector<int>& data, std::size_t begin, std::size_t end, int expectedSize, std::vector<int>& result) {
        if (expectedSize <= 0) {
            throw std::runtime_error("decodeRLERangeInto: invalid expected size");
        }

        if (begin > end || end > data.size() || ((end - begin) % 2 != 0)) {
            throw std::runtime_error("decodeRLERangeInto: invalid RLE range");
        }

        result.assign(static_cast<std::size_t>(expectedSize), 0);

        int position = 0;

        for (std::size_t i = begin; i + 1 < end; i += 2) {
            const int zeroCount = data[i];
            const int value = data[i + 1];

            if (zeroCount < 0) {
                throw std::runtime_error("decodeRLERangeInto: negative zero count");
            }

            if (position < expectedSize) {
                const int remaining = expectedSize - position;
                position += std::min(zeroCount, remaining);
            }

            if (position < expectedSize) {
                result[static_cast<std::size_t>(position)] = value;
                ++position;
            }
        }
    }

    void decodeRLE8x8RangeToBlock(const std::vector<int>& data, std::size_t begin, std::size_t end, std::vector<int>& block, int& prevDC) {
        if (begin > end || end > data.size() || ((end - begin) % 2 != 0)) {
            throw std::runtime_error("decodeRLE8x8RangeToBlock: invalid RLE range");
        }

        block.assign(64, 0);

        int position = 0;

        for (std::size_t i = begin; i + 1 < end; i += 2) {
            const int zeroCount = data[i];
            const int value = data[i + 1];

            if (zeroCount < 0) {
                throw std::runtime_error("decodeRLE8x8RangeToBlock: negative zero count");
            }

            if (position < 64) {
                const int remaining = 64 - position;
                position += std::min(zeroCount, remaining);
            }

            if (position < 64) {
                const int blockIndex = ZIGZAG_8X8[position];
                block[static_cast<std::size_t>(blockIndex)] = value;
                ++position;
            }
        }

        block[0] += prevDC;
        prevDC = block[0];
    }

    void encodeRLE8x8FromBlock(const std::vector<int>& block, std::vector<int>& result, int quantStep, int& prevDC, ChannelStats* stats) {
        if (block.size() != 64) {
            throw std::runtime_error("encodeRLE8x8FromBlock: block size must be 64");
        }

        result.clear();
        result.reserve(66);

        if (stats != nullptr) {
            stats->totalBlocks += 1;
            stats->totalCoefficients += 64;
        }

        const auto writeValue = [&](int value, int& zeroCount) {
            if (value == 0) {
                ++zeroCount;

                if (stats != nullptr) {
                    stats->zeroCoefficients += 1;
                }

                return;
            }

            if (stats != nullptr) {
                stats->nonZeroCoefficients += 1;

                const int absValue = std::abs(value);
                if (absValue > stats->maxAbsQuantizedValue) {
                    stats->maxAbsQuantizedValue = absValue;
                }

                if (zeroCount > stats->maxZeroRun) {
                    stats->maxZeroRun = zeroCount;
                }
            }

            result.push_back(zeroCount);
            result.push_back(value);
            zeroCount = 0;
            };

        const int currentDC = block[0];
        const int dcDelta = currentDC - prevDC;
        prevDC = currentDC;

        int zeroCount = 0;

        writeValue(dcDelta, zeroCount);

        if (quantStep < 16) {
            for (int i = 1; i < 64; ++i) {
                const int value = block[static_cast<std::size_t>(ZIGZAG_8X8[i])];
                writeValue(value, zeroCount);
            }
        } else if (quantStep < 32) {
            for (int i = 1; i < 64; ++i) {
                int value = block[static_cast<std::size_t>(ZIGZAG_8X8[i])];
                value = getPrunedValue8x8Q16(value, i);
                writeValue(value, zeroCount);
            }
        } else {
            for (int i = 1; i < 64; ++i) {
                int value = block[static_cast<std::size_t>(ZIGZAG_8X8[i])];
                value = getPrunedValue8x8Q32(value, i);
                writeValue(value, zeroCount);
            }
        }

        if (stats != nullptr && zeroCount > stats->maxZeroRun) {
            stats->maxZeroRun = zeroCount;
        }

        result.push_back(zeroCount);
        result.push_back(0);
    }

    void decodeRLE8x8ToBlock(const std::vector<int>& data, std::vector<int>& block, int& prevDC) {
        if (data.size() % 2 != 0) {
            throw std::runtime_error("decodeRLE8x8ToBlock: invalid RLE data");
        }

        block.assign(64, 0);

        int position = 0;

        for (std::size_t i = 0; i < data.size(); i += 2) {
            const int zeroCount = data[i];
            const int value = data[i + 1];

            if (zeroCount < 0) {
                throw std::runtime_error("decodeRLE8x8ToBlock: negative zero count");
            }

            if (position < 64) {
                const int remaining = 64 - position;
                position += std::min(zeroCount, remaining);
            }

            if (position < 64) {
                const int blockIndex = ZIGZAG_8X8[position];
                block[static_cast<std::size_t>(blockIndex)] = value;
                ++position;
            }
        }

        block[0] += prevDC;
        prevDC = block[0];
    }

    void decodeRLEInto(const std::vector<int>& data, int expectedSize, std::vector<int>& result) {
        if (expectedSize <= 0) {
            throw std::runtime_error("decodeRLEInto: invalid expected size");
        }

        if (data.size() % 2 != 0) {
            throw std::runtime_error("decodeRLEInto: invalid RLE data");
        }

        result.assign(static_cast<std::size_t>(expectedSize), 0);

        int position = 0;

        for (std::size_t i = 0; i < data.size(); i += 2) {
            const int zeroCount = data[i];
            const int value = data[i + 1];

            if (zeroCount < 0) {
                throw std::runtime_error("decodeRLEInto: negative zero count");
            }

            if (position < expectedSize) {
                const int remaining = expectedSize - position;
                position += std::min(zeroCount, remaining);
            }

            if (position < expectedSize) {
                result[static_cast<std::size_t>(position)] = value;
                ++position;
            }
        }
    }

    EncodedChannel encodeSingleChannel(const Image& img, int channelIndex, int blockSize, int quantStep, ChannelStats* outStats) {
        EncodedChannel encodedChannel;

        const int blockArea = blockSize * blockSize;
        const int blocksX = (img.width + blockSize - 1) / blockSize;
        const int blocksY = (img.height + blockSize - 1) / blockSize;

        const int blockCount = blocksX * blocksY;

        reserveEncodedChannel(encodedChannel, blockCount, blockSize, quantStep);

        std::vector<int> block(static_cast<std::size_t>(blockArea), 0);
        std::vector<int> orderedBlock(static_cast<std::size_t>(blockArea), 0);

        int prevDC = 0;

        for (int by = 0; by < img.height; by += blockSize) {
            for (int bx = 0; bx < img.width; bx += blockSize) {
                const bool fullBlock = bx + blockSize <= img.width && by + blockSize <= img.height;

                if (fullBlock) {
                    for (int y = 0; y < blockSize; ++y) {
                        const int srcY = by + y;
                        const int rowOffset = y * blockSize;

                        const std::size_t srcRowBase =
                            (static_cast<std::size_t>(srcY) *
                                static_cast<std::size_t>(img.width) +
                                static_cast<std::size_t>(bx)) *
                            static_cast<std::size_t>(img.channels);

                        for (int x = 0; x < blockSize; ++x) {
                            const std::size_t pixelIndex =
                                srcRowBase +
                                static_cast<std::size_t>(x) *
                                static_cast<std::size_t>(img.channels);

                            block[static_cast<std::size_t>(rowOffset + x)] =
                                static_cast<int>(img.data[pixelIndex + channelIndex]) - 128;
                        }
                    }
                } else {
                    std::fill(block.begin(), block.end(), 0);

                    for (int y = 0; y < blockSize; ++y) {
                        const int srcY = by + y;

                        if (srcY >= img.height) {
                            break;
                        }

                        for (int x = 0; x < blockSize; ++x) {
                            const int srcX = bx + x;

                            if (srcX >= img.width) {
                                break;
                            }

                            const int blockIndex = y * blockSize + x;

                            const std::size_t pixelIndex =
                                (static_cast<std::size_t>(srcY) *
                                    static_cast<std::size_t>(img.width) +
                                    static_cast<std::size_t>(srcX)) *
                                static_cast<std::size_t>(img.channels);

                            block[static_cast<std::size_t>(blockIndex)] =
                                static_cast<int>(img.data[pixelIndex + channelIndex]) - 128;
                        }
                    }
                }

                forwardWHT2D(block, blockSize);

                for (int& value : block) {
                    value = quantizeValue(value, quantStep);

                    if (quantStep >= 16 && std::abs(value) == 1) {
                        value = 0;
                    }
                }

                beginFlatBlock(encodedChannel);

                if (blockSize == 8) {
                    encodeRLE8x8FromBlockFlat(block, encodedChannel, quantStep, prevDC, outStats);
                } else {
                    orderedBlock.resize(static_cast<std::size_t>(blockArea));
                    std::copy(block.begin(), block.end(), orderedBlock.begin());

                    const int currentDC = orderedBlock[0];
                    orderedBlock[0] = currentDC - prevDC;
                    prevDC = currentDC;

                    appendRLEIntoFlat(orderedBlock, encodedChannel, outStats);
                }
            }
        }

        finishFlatChannel(encodedChannel);

        return encodedChannel;
    }

    void decodeSingleChannel(const EncodedChannel& encodedChannel, Image& img, int channelIndex, int blockSize, int quantStep) {
        const int blockArea = blockSize * blockSize;
        std::size_t blockCounter = 0;

        std::vector<int> block(static_cast<std::size_t>(blockArea), 0);

        int prevDC = 0;

        for (int by = 0; by < img.height; by += blockSize) {
            for (int bx = 0; bx < img.width; bx += blockSize) {
                if (encodedChannel.blockOffsets.size() < 2) {
                    throw std::runtime_error("decodeSingleChannel: empty encoded channel");
                }

                const std::size_t totalBlocks = encodedChannel.blockOffsets.size() - 1;

                if (blockCounter >= totalBlocks) {
                    throw std::runtime_error("decodeSingleChannel: not enough encoded blocks");
                }

                const std::size_t rleBegin =
                    encodedChannel.blockOffsets[blockCounter];

                const std::size_t rleEnd =
                    encodedChannel.blockOffsets[blockCounter + 1];

                ++blockCounter;

                if (rleBegin > rleEnd || rleEnd > encodedChannel.rleData.size()) {
                    throw std::runtime_error("decodeSingleChannel: invalid RLE offset");
                }

                if (blockSize == 8) {
                    decodeRLE8x8RangeToBlock(
                        encodedChannel.rleData,
                        rleBegin,
                        rleEnd,
                        block,
                        prevDC);
                } else {
                    decodeRLERangeInto(
                        encodedChannel.rleData,
                        rleBegin,
                        rleEnd,
                        blockArea,
                        block);

                    block[0] = block[0] + prevDC;
                    prevDC = block[0];
                }

                for (int& value : block) {
                    value = dequantizeValue(value, quantStep);
                }

                inverseWHT2D(block, blockSize);

                const bool fullBlock = bx + blockSize <= img.width && by + blockSize <= img.height;

                if (fullBlock) {
                    for (int y = 0; y < blockSize; ++y) {
                        const int dstY = by + y;
                        const int rowOffset = y * blockSize;

                        const std::size_t dstRowBase =
                            (static_cast<std::size_t>(dstY) *
                                static_cast<std::size_t>(img.width) +
                                static_cast<std::size_t>(bx)) *
                            static_cast<std::size_t>(img.channels);

                        for (int x = 0; x < blockSize; ++x) {
                            const int blockIndex = rowOffset + x;
                            const int restored = block[static_cast<std::size_t>(blockIndex)] + 128;

                            const std::size_t pixelIndex = dstRowBase + static_cast<std::size_t>(x) * static_cast<std::size_t>(img.channels);

                            img.data[pixelIndex + channelIndex] = static_cast<uint8_t>(clampInt(restored, 0, 255));
                        }
                    }
                } else {
                    for (int y = 0; y < blockSize; ++y) {
                        const int dstY = by + y;

                        if (dstY >= img.height) {
                            break;
                        }

                        for (int x = 0; x < blockSize; ++x) {
                            const int dstX = bx + x;

                            if (dstX >= img.width) {
                                break;
                            }

                            const int blockIndex = y * blockSize + x;
                            const int restored = block[static_cast<std::size_t>(blockIndex)] + 128;

                            const std::size_t pixelIndex =
                                (static_cast<std::size_t>(dstY) *
                                    static_cast<std::size_t>(img.width) +
                                    static_cast<std::size_t>(dstX)) *
                                static_cast<std::size_t>(img.channels);

                            img.data[pixelIndex + channelIndex] =
                                static_cast<uint8_t>(clampInt(restored, 0, 255));
                        }
                    }
                }
            }
        }
    }

    int getChannelsToEncode(const Image& img, const CodecParams& params) {
        if (params.isGrayscale) {
            if (img.channels != 1) {
                throw std::runtime_error("encodeFrame: grayscale mode expects 1-channel image");
            }

            return 1;
        }

        if (img.channels < 3) {
            throw std::runtime_error("encodeFrame: color mode expects at least 3 channels");
        }

        return 3;
    }

    void validateEncodeInput(const Image& img, const CodecParams& params) {
        if (img.width <= 0 || img.height <= 0 || img.channels <= 0 || img.data.empty()) {
            throw std::runtime_error("encodeFrame: invalid image");
        }

        if (params.blockSize <= 0 || params.quantStep <= 0) {
            throw std::runtime_error("encodeFrame: invalid codec parameters");
        }

        if (!isPowerOfTwo(params.blockSize)) {
            throw std::runtime_error("encodeFrame: blockSize must be a power of two");
        }
    }

    int getChannelQuantStep(const CodecParams& params, int channelIndex) {
        if (!params.isGrayscale && params.useYCbCr && channelIndex > 0) {
            return params.quantStep * 2;
        }

        return params.quantStep;
    }

    int getFrameChannelQuantStep(const EncodedFrame& frame, int channelIndex) {
        if (!frame.isGrayscale && frame.useYCbCr && channelIndex > 0) {
            return frame.quantStep * 2;
        }

        return frame.quantStep;
    }
}

EncodedFrame encodeFrame(const Image& img, const CodecParams& params, EncodeStats& stats) {
    validateEncodeInput(img, params);

    const int channelsToEncode = getChannelsToEncode(img, params);

    EncodedFrame frame;
    frame.width = img.width;
    frame.height = img.height;
    frame.channels = channelsToEncode;
    frame.blockSize = params.blockSize;
    frame.quantStep = params.quantStep;
    frame.isGrayscale = params.isGrayscale;
    frame.useYCbCr = params.useYCbCr;
    frame.channelData.resize(static_cast<std::size_t>(channelsToEncode));

    stats.channels.clear();
    stats.channels.resize(static_cast<std::size_t>(channelsToEncode));

    for (int c = 0; c < channelsToEncode; ++c) {
        const int channelQuantStep = getChannelQuantStep(params, c);

        frame.channelData[static_cast<std::size_t>(c)] =
            encodeSingleChannel(
                img,
                c,
                params.blockSize,
                channelQuantStep,
                &stats.channels[static_cast<std::size_t>(c)]);
    }

    return frame;
}

EncodedFrame encodeFrame(const Image& img, const CodecParams& params) {
    validateEncodeInput(img, params);

    const int channelsToEncode = getChannelsToEncode(img, params);

    EncodedFrame frame;
    frame.width = img.width;
    frame.height = img.height;
    frame.channels = channelsToEncode;
    frame.blockSize = params.blockSize;
    frame.quantStep = params.quantStep;
    frame.isGrayscale = params.isGrayscale;
    frame.useYCbCr = params.useYCbCr;
    frame.channelData.resize(static_cast<std::size_t>(channelsToEncode));

    for (int c = 0; c < channelsToEncode; ++c) {
        const int channelQuantStep = getChannelQuantStep(params, c);

        frame.channelData[static_cast<std::size_t>(c)] =
            encodeSingleChannel(
                img,
                c,
                params.blockSize,
                channelQuantStep,
                nullptr);
    }

    return frame;
}

Image decodeFrame(const EncodedFrame& frame) {
    if (frame.width <= 0 ||
        frame.height <= 0 ||
        frame.blockSize <= 0 ||
        frame.quantStep <= 0) {
        throw std::runtime_error("decodeFrame: invalid encoded frame");
    }

    if (!isPowerOfTwo(frame.blockSize)) {
        throw std::runtime_error("decodeFrame: blockSize must be a power of two");
    }

    if (frame.channels <= 0) {
        throw std::runtime_error("decodeFrame: invalid channel count");
    }

    Image img;
    img.width = frame.width;
    img.height = frame.height;
    img.channels = frame.channels;
    img.data.resize(
        static_cast<std::size_t>(img.width) *
        static_cast<std::size_t>(img.height) *
        static_cast<std::size_t>(img.channels),
        0);

    if (static_cast<int>(frame.channelData.size()) != frame.channels) {
        throw std::runtime_error("decodeFrame: channel data size mismatch");
    }

    for (int c = 0; c < frame.channels; ++c) {
        const int channelQuantStep = getFrameChannelQuantStep(frame, c);

        decodeSingleChannel(
            frame.channelData[static_cast<std::size_t>(c)],
            img,
            c,
            frame.blockSize,
            channelQuantStep);
    }

    return img;
}