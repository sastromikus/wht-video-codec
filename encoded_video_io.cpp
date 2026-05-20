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
