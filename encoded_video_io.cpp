#include "encoded_video_io.h"

#include "encoded_io.h"

#include <algorithm>
#include <cctype>
#include <fstream>
#include <limits>
#include <stdexcept>

namespace {
    constexpr uint32_t WLSV_MAGIC = 0x56534C57; // 'WLSV'
    constexpr uint32_t WLSV_FORMAT_V1 = 1;

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

    std::string toLowerCopy(std::string value) {
        std::transform(
            value.begin(),
            value.end(),
            value.begin(),
            [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });

        return value;
    }

    std::string getExtensionLower(const std::string& path) {
        const std::size_t slash = path.find_last_of("/\\");
        const std::size_t dot = path.find_last_of('.');

        if (dot == std::string::npos) {
            return "";
        }

        if (slash != std::string::npos && dot < slash) {
            return "";
        }

        return toLowerCopy(path.substr(dot));
    }

    bool validateHeader(const WalshVideoHeader& header) {
        if (header.width <= 0 || header.height <= 0) {
            return false;
        }

        if (header.fps <= 0) {
            return false;
        }

        if (header.blockSize != 2 && header.blockSize != 4 && header.blockSize != 8) {
            return false;
        }

        if (header.quantStep < 1 || header.quantStep > 255) {
            return false;
        }

        return true;
    }

    bool writeHeader(std::ostream& out, const WalshVideoHeader& header) {
        if (!writeValue(out, WLSV_MAGIC)) return false;
        if (!writeValue(out, WLSV_FORMAT_V1)) return false;

        if (!writeValue(out, header.width)) return false;
        if (!writeValue(out, header.height)) return false;
        if (!writeValue(out, header.fps)) return false;
        if (!writeValue(out, header.frameCount)) return false;

        if (!writeValue(out, header.blockSize)) return false;
        if (!writeValue(out, header.quantStep)) return false;

        const uint8_t grayscaleFlag = header.isGrayscale ? 1 : 0;
        const uint8_t ycbcrFlag = header.useYCbCr ? 1 : 0;

        if (!writeValue(out, grayscaleFlag)) return false;
        if (!writeValue(out, ycbcrFlag)) return false;

        return out.good();
    }

    bool readHeader(std::istream& in, WalshVideoHeader& header) {
        uint32_t magic = 0;
        if (!readValue(in, magic)) return false;
        if (magic != WLSV_MAGIC) return false;

        uint32_t version = 0;
        if (!readValue(in, version)) return false;
        if (version != WLSV_FORMAT_V1) return false;

        header.version = version;

        if (!readValue(in, header.width)) return false;
        if (!readValue(in, header.height)) return false;
        if (!readValue(in, header.fps)) return false;
        if (!readValue(in, header.frameCount)) return false;

        if (!readValue(in, header.blockSize)) return false;
        if (!readValue(in, header.quantStep)) return false;

        uint8_t grayscaleFlag = 0;
        uint8_t ycbcrFlag = 0;

        if (!readValue(in, grayscaleFlag)) return false;
        if (!readValue(in, ycbcrFlag)) return false;

        header.isGrayscale = grayscaleFlag != 0;
        header.useYCbCr = ycbcrFlag != 0;

        return validateHeader(header) && header.frameCount >= 0;
    }


    std::size_t getFileSizeForVideoInfo(const std::string& path) {
        std::ifstream in(path, std::ios::binary | std::ios::ate);

        if (!in) {
            return 0;
        }

        return static_cast<std::size_t>(in.tellg());
    }

    WalshChannelInfo analyzeVideoEncodedChannel(const EncodedChannel& channel) {
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
                // Last pair is trailingZeroRun / 0, so it is not a non-zero token.
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

    void addChannelInfo(WalshChannelInfo& dst, const WalshChannelInfo& src) {
        dst.blockCount += src.blockCount;
        dst.rleIntCount += src.rleIntCount;
        dst.rlePairCount += src.rlePairCount;
        dst.emptyBlocks += src.emptyBlocks;
        dst.nonZeroTokens += src.nonZeroTokens;

        if (src.maxAbsValue > dst.maxAbsValue) {
            dst.maxAbsValue = src.maxAbsValue;
        }

        if (src.maxZeroRun > dst.maxZeroRun) {
            dst.maxZeroRun = src.maxZeroRun;
        }

        if (dst.blockCount > 0) {
            dst.averageNonZeroTokensPerBlock =
                static_cast<double>(dst.nonZeroTokens) /
                static_cast<double>(dst.blockCount);
        }
    }

    bool frameMatchesVideoHeader(const EncodedFrame& frame, const WalshVideoHeader& header) {
        return frame.width == header.width &&
            frame.height == header.height &&
            frame.blockSize == header.blockSize &&
            frame.quantStep == header.quantStep &&
            frame.isGrayscale == header.isGrayscale &&
            frame.useYCbCr == header.useYCbCr;
    }
}

bool isWalshVideoFile(const std::string& path) {
    return getExtensionLower(path) == ".wlsv";
}

bool WalshVideoWriter::open(const std::string& path, const WalshVideoHeader& header) {
    if (!validateHeader(header)) {
        return false;
    }

    path_ = path;
    header_ = header;
    header_.frameCount = 0;
    frameBytes_.clear();
    frameCount_ = 0;
    opened_ = true;

    return true;
}

bool WalshVideoWriter::writeFrame(const EncodedFrame& frame) {
    if (!opened_) {
        return false;
    }

    if (!frameMatchesVideoHeader(frame, header_)) {
        return false;
    }

    std::vector<uint8_t> bytes;
    if (!saveEncodedFrameToBytes(frame, bytes)) {
        return false;
    }

    frameBytes_.push_back(std::move(bytes));
    ++frameCount_;

    return true;
}

bool WalshVideoWriter::close() {
    if (!opened_) {
        return false;
    }

    header_.frameCount = frameCount_;

    std::ofstream out(path_, std::ios::binary);
    if (!out) {
        return false;
    }

    if (!writeHeader(out, header_)) {
        return false;
    }

    for (const auto& bytes : frameBytes_) {
        if (bytes.size() > std::numeric_limits<uint32_t>::max()) {
            return false;
        }

        const uint32_t size = static_cast<uint32_t>(bytes.size());
        if (!writeValue(out, size)) {
            return false;
        }

        if (size > 0) {
            out.write(
                reinterpret_cast<const char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));

            if (!out.good()) {
                return false;
            }
        }
    }

    opened_ = false;
    return true;
}

bool WalshVideoReader::open(const std::string& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }

    WalshVideoHeader loadedHeader{};
    if (!readHeader(in, loadedHeader)) {
        return false;
    }

    std::vector<std::vector<uint8_t>> loadedFrames;
    loadedFrames.reserve(static_cast<std::size_t>(loadedHeader.frameCount));

    for (int i = 0; i < loadedHeader.frameCount; ++i) {
        uint32_t size = 0;
        if (!readValue(in, size)) {
            return false;
        }

        std::vector<uint8_t> bytes(size);
        if (size > 0) {
            in.read(
                reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));

            if (!in.good()) {
                return false;
            }
        }

        loadedFrames.push_back(std::move(bytes));
    }

    header_ = loadedHeader;
    frameBytes_ = std::move(loadedFrames);
    currentFrameIndex_ = 0;
    opened_ = true;

    return true;
}

bool WalshVideoReader::readNextFrame(EncodedFrame& frame) {
    if (!opened_) {
        return false;
    }

    if (currentFrameIndex_ >= static_cast<int>(frameBytes_.size())) {
        return false;
    }

    if (!loadEncodedFrameFromBytes(frameBytes_[currentFrameIndex_], frame)) {
        return false;
    }

    ++currentFrameIndex_;
    return true;
}


bool readWalshVideoFileInfo(const std::string& path, WalshVideoFileInfo& info) {
    info = WalshVideoFileInfo{};
    info.path = path;
    info.fileSize = getFileSizeForVideoInfo(path);

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return false;
    }

    WalshVideoHeader loadedHeader{};
    if (!readHeader(in, loadedHeader)) {
        return false;
    }

    info.header = loadedHeader;
    info.framesInfo.reserve(static_cast<std::size_t>(loadedHeader.frameCount));

    for (int frameIndex = 0; frameIndex < loadedHeader.frameCount; ++frameIndex) {
        uint32_t size = 0;
        if (!readValue(in, size)) {
            return false;
        }

        std::vector<uint8_t> bytes(size);
        if (size > 0) {
            in.read(
                reinterpret_cast<char*>(bytes.data()),
                static_cast<std::streamsize>(bytes.size()));

            if (!in.good()) {
                return false;
            }
        }

        WalshVideoFrameInfo frameInfo{};
        frameInfo.payloadSize = size;

        EncodedFrame frame{};
        if (!loadEncodedFrameFromBytes(bytes, frame)) {
            return false;
        }

        if (!frameMatchesVideoHeader(frame, loadedHeader)) {
            return false;
        }

        frameInfo.channels = frame.channels;
        frameInfo.channelsInfo.reserve(frame.channelData.size());

        if (info.totalChannelsInfo.empty()) {
            info.totalChannelsInfo.resize(frame.channelData.size());
        } else if (info.totalChannelsInfo.size() != frame.channelData.size()) {
            return false;
        }

        for (std::size_t channelIndex = 0; channelIndex < frame.channelData.size(); ++channelIndex) {
            WalshChannelInfo channelInfo = analyzeVideoEncodedChannel(frame.channelData[channelIndex]);
            frameInfo.channelsInfo.push_back(channelInfo);
            addChannelInfo(info.totalChannelsInfo[channelIndex], channelInfo);
        }

        if (info.framesInfo.empty()) {
            info.minFramePayloadSize = size;
            info.maxFramePayloadSize = size;
        } else {
            if (size < info.minFramePayloadSize) {
                info.minFramePayloadSize = size;
            }

            if (size > info.maxFramePayloadSize) {
                info.maxFramePayloadSize = size;
            }
        }

        info.totalFramePayloadSize += size;
        info.framesInfo.push_back(std::move(frameInfo));
    }

    if (!info.framesInfo.empty()) {
        info.averageFramePayloadSize =
            static_cast<double>(info.totalFramePayloadSize) /
            static_cast<double>(info.framesInfo.size());
    }

    return true;
}
