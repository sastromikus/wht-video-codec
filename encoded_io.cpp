#include "encoded_io.h"

#include <cstdint>
#include <fstream>
#include <limits>
#include <sstream>
#include <vector>

#include <zlib.h>

namespace {
    constexpr uint32_t WALSH_MAGIC = 0x48534C57; // 'WLSH'
    constexpr uint32_t WALSH_FORMAT_V6 = 6;

    constexpr uint32_t PAYLOAD_COMPRESSION_NONE = 0;
    constexpr uint32_t PAYLOAD_COMPRESSION_ZLIB = 1;

    constexpr int8_t EXTENDED_MARKER = -128;

    constexpr uint8_t EMPTY_RUN_MARKER = 255;
    constexpr uint16_t MAX_EMPTY_RUN = std::numeric_limits<uint16_t>::max();

    constexpr uint8_t COMPACT_TOKEN_LIMIT = 0x80;
    constexpr uint8_t LONG_ZERO_MARKER = 0xFE;
    constexpr uint8_t NORMAL_TOKEN_ZERO_MASK = 0x7F;

    template<typename T>
    bool writeValue(std::ostream& out, const T& value) {
        out.write(reinterpret_cast<const char*>(&value), sizeof(T));
        return out.good();
    }

    template<typename T>
    bool readValue(std::istream& in, T& value) {
        in.read(reinterpret_cast<char*>(&value), sizeof(T));
        return in.good();
    }

    bool fitsInInt16(int value) {
        return value >= static_cast<int>(std::numeric_limits<int16_t>::min()) &&
            value <= static_cast<int>(std::numeric_limits<int16_t>::max());
    }

    bool fitsInSmallInt8(int value) {
        return value >= -127 && value <= 127;
    }

    bool writeRleValue(std::ostream& out, int value) {
        if (!fitsInInt16(value)) {
            return false;
        }

        if (fitsInSmallInt8(value)) {
            const int8_t small = static_cast<int8_t>(value);
            return writeValue(out, small);
        }

        if (!writeValue(out, EXTENDED_MARKER)) {
            return false;
        }

        const int16_t value16 = static_cast<int16_t>(value);

        return writeValue(out, value16);
    }

    bool readRleValue(std::istream& in, int& value) {
        int8_t tagOrValue = 0;
        if (!readValue(in, tagOrValue)) {
            return false;
        }

        if (tagOrValue == EXTENDED_MARKER) {
            int16_t value16 = 0;
            if (!readValue(in, value16)) {
                return false;
            }

            value = static_cast<int>(value16);
            return true;
        }

        value = static_cast<int>(tagOrValue);
        return true;
    }

    bool fitsInCompactToken(int zeroCount, int value) {
        return zeroCount >= 0 &&
            zeroCount <= 7 &&
            value >= -8 &&
            value <= 7 &&
            value != 0;
    }

    uint8_t encodeSignedNibble(int value) {
        return static_cast<uint8_t>(value) & 0x0F;
    }

    int decodeSignedNibble(uint8_t nibble) {
        nibble &= 0x0F;

        if (nibble >= 8) {
            return static_cast<int>(nibble) - 16;
        }

        return static_cast<int>(nibble);
    }

    bool writeTokenV6(std::ostream& out, int zeroCount, int value) {
        if (!fitsInInt16(value)) {
            return false;
        }

        if (fitsInCompactToken(zeroCount, value)) {
            const uint8_t packed = static_cast<uint8_t>((zeroCount << 4) | encodeSignedNibble(value));

            return writeValue(out, packed);
        }

        if (zeroCount >= 0 && zeroCount <= 125) {
            const uint8_t tag = static_cast<uint8_t>(COMPACT_TOKEN_LIMIT | static_cast<uint8_t>(zeroCount));

            if (!writeValue(out, tag)) {
                return false;
            }

            return writeRleValue(out, value);
        }

        if (zeroCount > std::numeric_limits<uint16_t>::max()) {
            return false;
        }

        if (!writeValue(out, LONG_ZERO_MARKER)) {
            return false;
        }

        const uint16_t zero16 = static_cast<uint16_t>(zeroCount);
        if (!writeValue(out, zero16)) {
            return false;
        }

        return writeRleValue(out, value);
    }

    bool readTokenV6(std::istream& in, int& zeroCount, int& value) {
        uint8_t tag = 0;
        if (!readValue(in, tag)) {
            return false;
        }

        if (tag < COMPACT_TOKEN_LIMIT) {
            zeroCount = static_cast<int>(tag >> 4);
            value = decodeSignedNibble(tag & 0x0F);

            return value != 0;
        }

        if (tag == EMPTY_RUN_MARKER) {
            return false;
        }

        if (tag == LONG_ZERO_MARKER) {
            uint16_t zero16 = 0;
            if (!readValue(in, zero16)) {
                return false;
            }

            zeroCount = static_cast<int>(zero16);
            return readRleValue(in, value);
        }

        zeroCount = static_cast<int>(tag & NORMAL_TOKEN_ZERO_MASK);

        return readRleValue(in, value);
    }

    bool isEmptyBlockRange(const std::vector<int>& rleData, std::size_t begin, std::size_t end) {
        return end == begin + 2 &&
            end <= rleData.size() &&
            rleData[begin + 1] == 0;
    }

    bool saveEmptyRun(std::ostream& out, uint16_t count) {
        if (count == 0) {
            return false;
        }

        if (!writeValue(out, EMPTY_RUN_MARKER)) {
            return false;
        }

        if (!writeValue(out, count)) {
            return false;
        }

        return true;
    }

    bool saveBlockRangeV6(std::ostream& out, const std::vector<int>& rleData, std::size_t begin, std::size_t end) {
        if (begin > end || end > rleData.size() || ((end - begin) % 2 != 0)) {
            return false;
        }

        const std::size_t pairCount = (end - begin) / 2;

        if (pairCount == 0) {
            return false;
        }

        const std::size_t dataPairs = pairCount - 1;

        if (dataPairs >= EMPTY_RUN_MARKER) {
            return false;
        }

        const uint8_t tokenCount = static_cast<uint8_t>(dataPairs);
        if (!writeValue(out, tokenCount)) {
            return false;
        }

        for (std::size_t pairIndex = 0; pairIndex < dataPairs; ++pairIndex) {
            const std::size_t i = begin + pairIndex * 2;

            const int zeroCount = rleData[i];
            const int value = rleData[i + 1];

            if (value == 0) {
                return false;
            }

            if (!writeTokenV6(out, zeroCount, value)) {
                return false;
            }
        }

        return true;
    }

    bool saveChannelV6(std::ostream& out, const EncodedChannel& channel) {
        if (channel.blockOffsets.empty()) {
            return false;
        }

        const std::size_t blockCount = channel.blockOffsets.size() - 1;

        if (blockCount > std::numeric_limits<uint32_t>::max()) {
            return false;
        }

        const uint32_t blockCount32 = static_cast<uint32_t>(blockCount);
        if (!writeValue(out, blockCount32)) {
            return false;
        }

        std::size_t blockIndex = 0;

        while (blockIndex < blockCount) {
            const std::size_t begin = channel.blockOffsets[blockIndex];
            const std::size_t end = channel.blockOffsets[blockIndex + 1];

            if (begin > end || end > channel.rleData.size()) {
                return false;
            }

            if (isEmptyBlockRange(channel.rleData, begin, end)) {
                uint16_t runLength = 0;

                while (blockIndex + runLength < blockCount &&
                    runLength < MAX_EMPTY_RUN) {
                    const std::size_t runBegin = channel.blockOffsets[blockIndex + runLength];

                    const std::size_t runEnd = channel.blockOffsets[blockIndex + runLength + 1];

                    if (!isEmptyBlockRange(channel.rleData, runBegin, runEnd)) {
                        break;
                    }

                    ++runLength;
                }

                if (!saveEmptyRun(out, runLength)) {
                    return false;
                }

                blockIndex += runLength;
                continue;
            }

            if (!saveBlockRangeV6(out, channel.rleData, begin, end)) {
                return false;
            }

            ++blockIndex;
        }

        return true;
    }

    void appendLoadedEmptyBlock(EncodedChannel& channel, int blockSize) {
        if (channel.rleData.size() > std::numeric_limits<uint32_t>::max()) {
            throw std::runtime_error("appendLoadedEmptyBlock: stream too large");
        }

        channel.blockOffsets.push_back(static_cast<uint32_t>(channel.rleData.size()));

        const int blockArea = blockSize * blockSize;
        channel.rleData.push_back(blockArea);
        channel.rleData.push_back(0);
    }

    bool loadBlockBodyV6(std::istream& in, EncodedChannel& channel, int blockSize, uint8_t tokenCount) {
        if (blockSize <= 0) {
            return false;
        }

        if (channel.rleData.size() > std::numeric_limits<uint32_t>::max()) {
            return false;
        }

        channel.blockOffsets.push_back(static_cast<uint32_t>(channel.rleData.size()));

        const int blockArea = blockSize * blockSize;
        int writtenCoefficients = 0;

        for (uint8_t i = 0; i < tokenCount; ++i) {
            int zeroCount = 0;
            int value = 0;

            if (!readTokenV6(in, zeroCount, value)) {
                return false;
            }

            if (value == 0) {
                return false;
            }

            writtenCoefficients += zeroCount + 1;

            if (writtenCoefficients > blockArea) {
                return false;
            }

            channel.rleData.push_back(zeroCount);
            channel.rleData.push_back(value);
        }

        const int trailingZeroCount = blockArea - writtenCoefficients;
        if (trailingZeroCount < 0) {
            return false;
        }

        channel.rleData.push_back(trailingZeroCount);
        channel.rleData.push_back(0);

        return true;
    }

    bool loadChannelV6(std::istream& in, EncodedChannel& channel, uint32_t blockCount, int blockSize) {
        channel.rleData.clear();
        channel.blockOffsets.clear();

        channel.blockOffsets.reserve(
            static_cast<std::size_t>(blockCount) + 1);

        while (channel.blockOffsets.size() < blockCount) {
            uint8_t tag = 0;
            if (!readValue(in, tag)) {
                return false;
            }

            if (tag == EMPTY_RUN_MARKER) {
                uint16_t runLength = 0;
                if (!readValue(in, runLength)) {
                    return false;
                }

                if (runLength == 0) {
                    return false;
                }

                if (channel.blockOffsets.size() + runLength > blockCount) {
                    return false;
                }

                for (uint16_t i = 0; i < runLength; ++i) {
                    appendLoadedEmptyBlock(channel, blockSize);
                }

                continue;
            }

            if (!loadBlockBodyV6(in, channel, blockSize, tag)) {
                return false;
            }
        }

        if (channel.rleData.size() > std::numeric_limits<uint32_t>::max()) {
            return false;
        }

        channel.blockOffsets.push_back(static_cast<uint32_t>(channel.rleData.size()));

        return true;
    }

    bool compressPayloadZlib(
        const std::vector<uint8_t>& input,
        std::vector<uint8_t>& output)
    {
        if (input.empty()) {
            output.clear();

            return true;
        }

        const uLongf compressedBound = compressBound(static_cast<uLong>(input.size()));

        output.resize(static_cast<std::size_t>(compressedBound));

        uLongf compressedSize = compressedBound;

        const int result = compress2(
            output.data(),
            &compressedSize,
            input.data(),
            static_cast<uLong>(input.size()),
            Z_BEST_COMPRESSION);

        if (result != Z_OK) {
            return false;
        }

        output.resize(static_cast<std::size_t>(compressedSize));

        return true;
    }

    bool decompressPayloadZlib(const std::vector<uint8_t>& input, std::vector<uint8_t>& output, uint32_t originalSize) {
        output.resize(originalSize);

        if (originalSize == 0) {
            return input.empty();
        }

        uLongf decompressedSize = static_cast<uLongf>(originalSize);

        const int result = uncompress(output.data(), &decompressedSize, input.data(), static_cast<uLong>(input.size()));

        if (result != Z_OK) {
            return false;
        }

        return decompressedSize == static_cast<uLongf>(originalSize);
    }

    std::vector<uint8_t> stringToBytes(const std::string& value) {
        return std::vector<uint8_t>(value.begin(), value.end());
    }

    bool saveFramePayloadV6(std::ostream& out, const EncodedFrame& frame) {
        if (!writeValue(out, frame.width)) return false;
        if (!writeValue(out, frame.height)) return false;
        if (!writeValue(out, frame.channels)) return false;
        if (!writeValue(out, frame.blockSize)) return false;
        if (!writeValue(out, frame.quantStep)) return false;

        const uint8_t grayscaleFlag = frame.isGrayscale ? 1 : 0;
        if (!writeValue(out, grayscaleFlag)) return false;

        const uint8_t ycbcrFlag = frame.useYCbCr ? 1 : 0;
        if (!writeValue(out, ycbcrFlag)) return false;

        const uint32_t channelCount = static_cast<uint32_t>(frame.channelData.size());

        if (!writeValue(out, channelCount)) return false;

        for (const auto& channel : frame.channelData) {
            if (!saveChannelV6(out, channel)) {
                return false;
            }
        }

        return out.good();
    }

    bool loadFramePayloadV6(std::istream& in, EncodedFrame& frame) {
        if (!readValue(in, frame.width)) return false;
        if (!readValue(in, frame.height)) return false;
        if (!readValue(in, frame.channels)) return false;
        if (!readValue(in, frame.blockSize)) return false;
        if (!readValue(in, frame.quantStep)) return false;

        uint8_t grayscaleFlag = 0;
        if (!readValue(in, grayscaleFlag)) return false;
        frame.isGrayscale = (grayscaleFlag != 0);

        uint8_t ycbcrFlag = 0;
        if (!readValue(in, ycbcrFlag)) return false;
        frame.useYCbCr = (ycbcrFlag != 0);

        uint32_t channelCount = 0;
        if (!readValue(in, channelCount)) {
            return false;
        }

        frame.channelData.clear();
        frame.channelData.resize(channelCount);

        for (uint32_t c = 0; c < channelCount; ++c) {
            uint32_t blockCount = 0;
            if (!readValue(in, blockCount)) {
                return false;
            }

            if (!loadChannelV6(in, frame.channelData[c], blockCount, frame.blockSize)) {
                return false;
            }
        }

        return true;
    }
}

namespace {
    bool writeEncodedFrameStream(std::ostream& out, const EncodedFrame& frame) {
        std::ostringstream payloadStream(std::ios::binary);

        if (!saveFramePayloadV6(payloadStream, frame)) {
            return false;
        }

        const std::string payloadString = payloadStream.str();
        const std::vector<uint8_t> payload = stringToBytes(payloadString);

        std::vector<uint8_t> compressedPayload;
        if (!compressPayloadZlib(payload, compressedPayload)) {
            return false;
        }

        if (!writeValue(out, WALSH_MAGIC)) return false;
        if (!writeValue(out, WALSH_FORMAT_V6)) return false;

        const uint32_t compressionMethod = PAYLOAD_COMPRESSION_ZLIB;
        if (!writeValue(out, compressionMethod)) return false;

        if (payload.size() > std::numeric_limits<uint32_t>::max()) {
            return false;
        }

        if (compressedPayload.size() > std::numeric_limits<uint32_t>::max()) {
            return false;
        }

        const uint32_t originalPayloadSize = static_cast<uint32_t>(payload.size());

        const uint32_t compressedPayloadSize = static_cast<uint32_t>(compressedPayload.size());

        if (!writeValue(out, originalPayloadSize)) return false;
        if (!writeValue(out, compressedPayloadSize)) return false;

        if (!compressedPayload.empty()) {
            out.write(
                reinterpret_cast<const char*>(compressedPayload.data()),
                static_cast<std::streamsize>(compressedPayload.size()));
        }

        return out.good();
    }

    bool readEncodedFrameStream(std::istream& in, EncodedFrame& frame) {
        uint32_t magic = 0;
        if (!readValue(in, magic)) {
            return false;
        }

        if (magic != WALSH_MAGIC) {
            return false;
        }

        uint32_t formatVersion = 0;
        if (!readValue(in, formatVersion)) {
            return false;
        }

        if (formatVersion != WALSH_FORMAT_V6) {
            return false;
        }

        uint32_t compressionMethod = PAYLOAD_COMPRESSION_NONE;
        if (!readValue(in, compressionMethod)) {
            return false;
        }

        if (compressionMethod != PAYLOAD_COMPRESSION_ZLIB) {
            return false;
        }

        uint32_t originalPayloadSize = 0;
        uint32_t compressedPayloadSize = 0;

        if (!readValue(in, originalPayloadSize)) {
            return false;
        }

        if (!readValue(in, compressedPayloadSize)) {
            return false;
        }

        std::vector<uint8_t> compressedPayload(compressedPayloadSize);

        if (compressedPayloadSize > 0) {
            in.read(
                reinterpret_cast<char*>(compressedPayload.data()),
                static_cast<std::streamsize>(compressedPayload.size()));

            if (!in.good()) {
                return false;
            }
        }

        std::vector<uint8_t> payload;
        if (!decompressPayloadZlib(compressedPayload, payload, originalPayloadSize)) {
            return false;
        }

        const std::string payloadString(payload.begin(), payload.end());
        std::istringstream payloadStream(payloadString, std::ios::binary);

        return loadFramePayloadV6(payloadStream, frame);
    }
}

bool saveEncodedFrame(const std::string& path, const EncodedFrame& frame) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return false;
    }

    return writeEncodedFrameStream(out, frame);
}

bool loadEncodedFrame(const std::string& path, EncodedFrame& frame) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }

    return readEncodedFrameStream(in, frame);
}

// Serialize a single .walsh frame to memory. WLSV uses this to store
// multiple independent Walsh frames inside one video container.
bool saveEncodedFrameToBytes(const EncodedFrame& frame, std::vector<uint8_t>& bytes) {
    std::ostringstream out(std::ios::binary | std::ios::out);

    if (!writeEncodedFrameStream(out, frame)) {
        return false;
    }

    const std::string data = out.str();
    bytes.assign(data.begin(), data.end());
    return true;
}

bool loadEncodedFrameFromBytes(const std::vector<uint8_t>& bytes, EncodedFrame& frame) {
    const std::string data(bytes.begin(), bytes.end());
    std::istringstream in(data, std::ios::binary | std::ios::in);

    return readEncodedFrameStream(in, frame);
}

namespace {
    std::size_t getFileSizeForInfo(const std::string& path) {
        std::ifstream in(path, std::ios::binary | std::ios::ate);

        if (!in) {
            return 0;
        }

        return static_cast<std::size_t>(in.tellg());
    }

    WalshChannelInfo analyzeEncodedChannel(const EncodedChannel& channel) {
        WalshChannelInfo info{};

        if (channel.blockOffsets.size() < 2) {
            return info;
        }

        const std::size_t blockCount = channel.blockOffsets.size() - 1;

        info.blockCount = static_cast<uint32_t>(blockCount);
        info.rleIntCount = channel.rleData.size();
        info.rlePairCount = channel.rleData.size() / 2;

        for (std::size_t blockIndex = 0; blockIndex < blockCount; ++blockIndex) {
            const std::size_t begin = channel.blockOffsets[blockIndex];
            const std::size_t end = channel.blockOffsets[blockIndex + 1];

            if (begin > end || end > channel.rleData.size()) {
                continue;
            }

            const std::size_t blockIntCount = end - begin;

            if (blockIntCount == 2 && channel.rleData[begin + 1] == 0) {
                ++info.emptyBlocks;
            }

            if (blockIntCount % 2 != 0) {
                continue;
            }

            const std::size_t pairCount = blockIntCount / 2;

            if (pairCount > 0) {
                // ѕоследн€€ пара Ч trailingZeroRun / 0.
                info.nonZeroTokens += static_cast<uint64_t>(pairCount - 1);
            }

            for (std::size_t i = begin; i + 1 < end; i += 2) {
                const int zeroRun = channel.rleData[i];
                const int value = channel.rleData[i + 1];

                if (zeroRun > info.maxZeroRun) {
                    info.maxZeroRun = zeroRun;
                }

                if (value != 0) {
                    const int absValue = value < 0 ? -value : value;

                    if (absValue > info.maxAbsValue) {
                        info.maxAbsValue = absValue;
                    }
                }
            }
        }

        if (info.blockCount > 0) {
            info.averageNonZeroTokensPerBlock =
                static_cast<double>(info.nonZeroTokens) /
                static_cast<double>(info.blockCount);
        }

        return info;
    }
}

bool readWalshFileInfo(const std::string& path, WalshFileInfo& info) {
    info = WalshFileInfo{};
    info.path = path;
    info.fileSize = getFileSizeForInfo(path);

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }

    uint32_t magic = 0;
    if (!readValue(in, magic)) {
        return false;
    }

    if (magic != WALSH_MAGIC) {
        return false;
    }

    if (!readValue(in, info.formatVersion)) {
        return false;
    }

    if (info.formatVersion != WALSH_FORMAT_V6) {
        return false;
    }

    if (!readValue(in, info.compressionMethod)) {
        return false;
    }

    if (!readValue(in, info.rawPayloadSize)) {
        return false;
    }

    if (!readValue(in, info.compressedPayloadSize)) {
        return false;
    }

    EncodedFrame frame{};
    if (!loadEncodedFrame(path, frame)) {
        return false;
    }

    info.width = frame.width;
    info.height = frame.height;
    info.channels = frame.channels;
    info.blockSize = frame.blockSize;
    info.quantStep = frame.quantStep;
    info.isGrayscale = frame.isGrayscale;
    info.useYCbCr = frame.useYCbCr;

    info.channelsInfo.clear();
    info.channelsInfo.reserve(frame.channelData.size());

    for (const EncodedChannel& channel : frame.channelData) {
        info.channelsInfo.push_back(analyzeEncodedChannel(channel));
    }

    return true;
}