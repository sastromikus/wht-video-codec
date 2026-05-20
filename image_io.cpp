#define _CRT_SECURE_NO_WARNINGS
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

#include "image_io.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <stdexcept>
#include <string>

namespace {
    constexpr int BYTE_VALUES = 256;
    constexpr int RGB_PAIR_VALUES = BYTE_VALUES * BYTE_VALUES;

    std::string getFileExtensionLower(const std::string& path) {
        const std::size_t dotPos = path.find_last_of('.');
        if (dotPos == std::string::npos) {
            return "";
        }

        std::string ext = path.substr(dotPos);
        std::transform(
            ext.begin(),
            ext.end(),
            ext.begin(),
            [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });

        return ext;
    }

    int clampToByteRange(int value) {
        if (value < 0) return 0;
        if (value > 255) return 255;
        return value;
    }

    struct RGBToYCbCrTables {
        std::array<double, RGB_PAIR_VALUES> yFromRG{};
        std::array<double, RGB_PAIR_VALUES> cbFromRG{};
        std::array<double, RGB_PAIR_VALUES> crFromRG{};

        RGBToYCbCrTables() {
            for (int r = 0; r < BYTE_VALUES; ++r) {
                for (int g = 0; g < BYTE_VALUES; ++g) {
                    const std::size_t index =
                        static_cast<std::size_t>((r << 8) | g);

                    yFromRG[index] = 0.299000 * static_cast<double>(r) + 0.587000 * static_cast<double>(g);

                    cbFromRG[index] = 128.0 - 0.168736 * static_cast<double>(r) - 0.331264 * static_cast<double>(g);

                    crFromRG[index] = 128.0 + 0.500000 * static_cast<double>(r) - 0.418688 * static_cast<double>(g);
                }
            }
        }
    };

    struct YCbCrToRGBTables {
        std::array<int, BYTE_VALUES> rOffsetFromCr{};
        std::array<int, BYTE_VALUES> bOffsetFromCb{};
        std::array<int, RGB_PAIR_VALUES> gOffsetFromCbCr{};

        YCbCrToRGBTables() {
            for (int cbByte = 0; cbByte < BYTE_VALUES; ++cbByte) {
                const int cb = cbByte - 128;

                bOffsetFromCb[static_cast<std::size_t>(cbByte)] = static_cast<int>(std::round(1.772000 * static_cast<double>(cb)));
            }

            for (int crByte = 0; crByte < BYTE_VALUES; ++crByte) {
                const int cr = crByte - 128;

                rOffsetFromCr[static_cast<std::size_t>(crByte)] = static_cast<int>(std::round(1.402000 * static_cast<double>(cr)));
            }

            for (int cbByte = 0; cbByte < BYTE_VALUES; ++cbByte) {
                const int cb = cbByte - 128;

                for (int crByte = 0; crByte < BYTE_VALUES; ++crByte) {
                    const int cr = crByte - 128;

                    const std::size_t index = static_cast<std::size_t>((cbByte << 8) | crByte);

                    gOffsetFromCbCr[index] =
                        static_cast<int>(std::round(
                            -0.344136 * static_cast<double>(cb) -
                            0.714136 * static_cast<double>(cr)));
                }
            }
        }
    };

    const RGBToYCbCrTables& getRGBToYCbCrTables() {
        static const RGBToYCbCrTables tables;
        return tables;
    }

    const YCbCrToRGBTables& getYCbCrToRGBTables() {
        static const YCbCrToRGBTables tables;
        return tables;
    }
}

bool loadImage(const std::string& path, Image& img) {
    int width = 0;
    int height = 0;
    int channels = 0;

    unsigned char* rawData = stbi_load(
        path.c_str(),
        &width,
        &height,
        &channels,
        0);

    if (!rawData) {
        return false;
    }

    if (width <= 0 || height <= 0 || channels <= 0) {
        stbi_image_free(rawData);
        return false;
    }

    img.width = width;
    img.height = height;
    img.channels = channels;

    const std::size_t dataSize =
        static_cast<std::size_t>(width) *
        static_cast<std::size_t>(height) *
        static_cast<std::size_t>(channels);

    img.data.assign(rawData, rawData + dataSize);

    stbi_image_free(rawData);

    return true;
}

bool saveImage(const std::string& path, const Image& img) {
    if (img.width <= 0 ||
        img.height <= 0 ||
        img.channels <= 0 ||
        img.data.empty()) {
        return false;
    }

    const std::string ext = getFileExtensionLower(path);

    if (ext == ".png") {
        const int stride = img.width * img.channels;

        return stbi_write_png(
            path.c_str(),
            img.width,
            img.height,
            img.channels,
            img.data.data(),
            stride) != 0;
    }

    if (ext == ".bmp") {
        return stbi_write_bmp(
            path.c_str(),
            img.width,
            img.height,
            img.channels,
            img.data.data()) != 0;
    }

    if (ext == ".jpg" || ext == ".jpeg") {
        const int quality = 95;

        return stbi_write_jpg(
            path.c_str(),
            img.width,
            img.height,
            img.channels,
            img.data.data(),
            quality) != 0;
    }

    if (ext == ".tga") {
        return stbi_write_tga(
            path.c_str(),
            img.width,
            img.height,
            img.channels,
            img.data.data()) != 0;
    }

    const int stride = img.width * img.channels;

    return stbi_write_png(
        path.c_str(),
        img.width,
        img.height,
        img.channels,
        img.data.data(),
        stride) != 0;
}

bool saveJpegWithQuality(const std::string& path, const Image& img, int quality) {
    if (img.width <= 0 ||
        img.height <= 0 ||
        img.channels <= 0 ||
        img.data.empty()) {

        return false;
    }

    if (quality < 1) {
        quality = 1;
    }

    if (quality > 100) {
        quality = 100;
    }

    return stbi_write_jpg(
        path.c_str(),
        img.width,
        img.height,
        img.channels,
        img.data.data(),
        quality) != 0;
}

Image convertToGrayscale(const Image& src) {
    if (src.data.empty()) {
        throw std::runtime_error("convertToGrayscale: empty source image");
    }

    if (src.width <= 0 || src.height <= 0 || src.channels <= 0) {
        throw std::runtime_error("convertToGrayscale: invalid source image");
    }

    if (src.channels == 1) {
        return src;
    }

    Image gray;
    gray.width = src.width;
    gray.height = src.height;
    gray.channels = 1;

    const std::size_t pixelCount =
        static_cast<std::size_t>(gray.width) *
        static_cast<std::size_t>(gray.height);

    gray.data.resize(pixelCount);

    const uint8_t* srcPtr = src.data.data();
    uint8_t* dstPtr = gray.data.data();

    for (std::size_t i = 0; i < pixelCount; ++i) {
        const unsigned char r = srcPtr[0];
        const unsigned char g = (src.channels >= 2) ? srcPtr[1] : r;
        const unsigned char b = (src.channels >= 3) ? srcPtr[2] : r;

        const double grayValue =
            0.299 * static_cast<double>(r) +
            0.587 * static_cast<double>(g) +
            0.114 * static_cast<double>(b);

        dstPtr[i] = static_cast<unsigned char>(grayValue);

        srcPtr += src.channels;
    }

    return gray;
}

Image convertToRGB(const Image& src) {
    if (src.data.empty()) {
        throw std::runtime_error("convertToRGB: empty source image");
    }

    if (src.width <= 0 || src.height <= 0 || src.channels <= 0) {
        throw std::runtime_error("convertToRGB: invalid source image");
    }

    if (src.channels == 3) {
        return src;
    }

    if (src.channels < 3) {
        throw std::runtime_error("convertToRGB: source image must have at least 3 channels");
    }

    Image rgb;
    rgb.width = src.width;
    rgb.height = src.height;
    rgb.channels = 3;

    const std::size_t pixelCount =
        static_cast<std::size_t>(rgb.width) *
        static_cast<std::size_t>(rgb.height);

    rgb.data.resize(pixelCount * 3);

    const uint8_t* srcPtr = src.data.data();
    uint8_t* dstPtr = rgb.data.data();

    for (std::size_t i = 0; i < pixelCount; ++i) {
        dstPtr[0] = srcPtr[0];
        dstPtr[1] = srcPtr[1];
        dstPtr[2] = srcPtr[2];

        srcPtr += src.channels;
        dstPtr += 3;
    }

    return rgb;
}

Image convertRGBToYCbCr(const Image& src) {
    if (src.channels < 3) {
        throw std::runtime_error("convertRGBToYCbCr: source image must have at least 3 channels");
    }

    Image dst;
    dst.width = src.width;
    dst.height = src.height;
    dst.channels = 3;

    const std::size_t pixelCount =
        static_cast<std::size_t>(dst.width) *
        static_cast<std::size_t>(dst.height);

    dst.data.resize(pixelCount * 3);

    const RGBToYCbCrTables& tables = getRGBToYCbCrTables();

    const uint8_t* srcPtr = src.data.data();
    uint8_t* dstPtr = dst.data.data();

    for (std::size_t i = 0; i < pixelCount; ++i) {
        const int r = srcPtr[0];
        const int g = srcPtr[1];
        const int b = srcPtr[2];

        const std::size_t rgIndex =
            static_cast<std::size_t>((r << 8) | g);

        const double y =
            tables.yFromRG[rgIndex] +
            0.114000 * static_cast<double>(b);

        const double cb =
            tables.cbFromRG[rgIndex] +
            0.500000 * static_cast<double>(b);

        const double cr =
            tables.crFromRG[rgIndex] -
            0.081312 * static_cast<double>(b);

        dstPtr[0] = static_cast<uint8_t>(
            clampToByteRange(static_cast<int>(std::round(y))));

        dstPtr[1] = static_cast<uint8_t>(
            clampToByteRange(static_cast<int>(std::round(cb))));

        dstPtr[2] = static_cast<uint8_t>(
            clampToByteRange(static_cast<int>(std::round(cr))));

        srcPtr += src.channels;
        dstPtr += 3;
    }

    return dst;
}

Image convertYCbCrToRGB(const Image& src) {
    if (src.channels < 3) {
        throw std::runtime_error("convertYCbCrToRGB: source image must have at least 3 channels");
    }

    Image dst;
    dst.width = src.width;
    dst.height = src.height;
    dst.channels = 3;

    const std::size_t pixelCount =
        static_cast<std::size_t>(dst.width) *
        static_cast<std::size_t>(dst.height);

    dst.data.resize(pixelCount * 3);

    const YCbCrToRGBTables& tables = getYCbCrToRGBTables();

    const uint8_t* srcPtr = src.data.data();
    uint8_t* dstPtr = dst.data.data();

    for (std::size_t i = 0; i < pixelCount; ++i) {
        const int y = srcPtr[0];
        const int cbByte = srcPtr[1];
        const int crByte = srcPtr[2];

        const std::size_t cbCrIndex = static_cast<std::size_t>((cbByte << 8) | crByte);

        const int r = y + tables.rOffsetFromCr[static_cast<std::size_t>(crByte)];

        const int g = y + tables.gOffsetFromCbCr[cbCrIndex];

        const int b = y + tables.bOffsetFromCb[static_cast<std::size_t>(cbByte)];

        dstPtr[0] = static_cast<uint8_t>(clampToByteRange(r));
        dstPtr[1] = static_cast<uint8_t>(clampToByteRange(g));
        dstPtr[2] = static_cast<uint8_t>(clampToByteRange(b));

        srcPtr += src.channels;
        dstPtr += 3;
    }

    return dst;
}