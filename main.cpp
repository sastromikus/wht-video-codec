#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <direct.h>
#include <exception>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>
#include <vector>

#include "types.h"
#include "image_io.h"
#include "codec.h"
#include "metrics.h"
#include "encoded_io.h"
#include "encoded_video_io.h"
#include "video_io_ffmpeg.h"
#include "test_runner.h"

namespace {
    enum class WorkMode {
        Grayscale,
        RGB,
        YCbCr
    };

    struct CliOptions {
        std::string inputPath;
        std::string outputImagePath;
        std::string outputStreamPath;

        WorkMode mode = WorkMode::YCbCr;

        int blockSize = 8;
        int quantStep = 16;

        bool outputImageProvided = false;
        bool outputStreamProvided = false;
        bool modeProvided = false;
        bool blockProvided = false;
        bool quantProvided = false;

        int fps = 1;
        bool fpsProvided = false;
    };

    struct CodecTestResult {
        std::string source;
        std::string method;
        std::string mode;
        int blockSize = 0;
        int quantStep = 0;
        int quality = 0;

        std::string encodedPath;
        std::string previewPath;

        std::size_t encodedSizeBytes = 0;
        std::size_t previewSizeBytes = 0;

        double mse = 0.0;
        double psnr = 0.0;

        double encodeMs = 0.0;
        double decodeMs = 0.0;
        double saveEncodedMs = 0.0;
        double savePreviewMs = 0.0;
        double totalMs = 0.0;

        std::string notes;
    };

    void printUsage(const char* exeName) {
        std::cout
            << "Usage:\n"
            << "  " << exeName << " <input_image> [options]\n"
            << "  " << exeName << " <input_stream.walsh> [options]\n"
            << "  " << exeName << " <input_video.mp4|avi|mkv> -fps 1 -w output.wlsv [options]\n"
            << "  " << exeName << " <input_video.wlsv> -o output.mp4\n"
            << "  " << exeName << " -test <input_lossless.png|bmp> [output_dir]\n"
            << "  " << exeName << " -info <input_stream.walsh>\n"
            << "  " << exeName << " <input_stream.walsh> -info\n"
            << "  " << exeName << " -dump <input_stream.walsh> [output_dir]\n"
            << "  " << exeName << " <input_stream.walsh> -dump [output_dir]\n"
            << "  " << exeName << " -diff <image_a> <image_b> <diff_output> [amplify]\n"
            << "  " << exeName << " <image_a> -diff <image_b> <diff_output> [amplify]\n"
            << "  " << exeName << " -test-codecs <input_image> [output_dir]\n\n"

            << "Encode options:\n"
            << "  -mode,  -m        gray|grayscale|rgb|ycbcr|chan|channel|channels\n"
            << "  -block, -b        block size: 2, 4, 8\n"
            << "  -blocks           alias for -block\n"
            << "  -quant, -q        quantization step: 1..255\n"
            << "  -quantize         alias for -quant\n"
            << "  -out,   -o        output image path\n"
            << "  -walsh, -w        output .walsh stream path, or .wlsv for video input\n"
            << "  -fps              video frame rate for input/output .wlsv, default 1\n\n"

            << "Decode options:\n"
            << "  -out, -o          output image path\n\n"

            << "Codec comparison options:\n"
            << "  -test-codecs      compare Walsh modes with JPEG qualities\n\n"

            << "Info options:\n"
            << "  -info             print .walsh container and channel statistics\n\n"

            << "Dump options:\n"
            << "  -dump             decode .walsh and save decoded image plus channel images\n\n"

            << "Diff options:\n"
            << "  -diff             create visual absolute-difference map between two images\n"
            << "                    optional amplify value makes small differences visible\n\n"

            << "Output path rules:\n"
            << "  -out name         if no extension is given, source image extension is used\n"
            << "  -out name.jpg     extension forces image format\n"
            << "  -walsh name       becomes name.walsh\n"
            << "  -walsh name.walsh stays name.walsh\n\n"

            << "Examples:\n"
            << "  " << exeName << " source.png -m ycbcr -b 8 -q 16 -o output.png -w output.walsh\n"
            << "  " << exeName << " source.png -mode gray -quant 32\n"
            << "  " << exeName << " output.walsh -out decoded.png\n"
            << "  " << exeName << " -info output.walsh\n"
            << "  " << exeName << " output.walsh -dump dump_output\n"
            << "  " << exeName << " -diff source.png output.png diff.png 8\n"
            << "  " << exeName << " -test source.png test_results\n"
            << "  " << exeName << " input.mp4 -fps 1 -m ycbcr -b 8 -q 16 -w output.wlsv\n"
            << "  " << exeName << " output.wlsv -o decoded.mp4\n\n";
    }

    std::string toLowerCopy(std::string s) {
        std::transform(
            s.begin(),
            s.end(),
            s.begin(),
            [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });

        return s;
    }

    bool isOption(const std::string& value) {
        return !value.empty() && value[0] == '-';
    }

    std::size_t findLastPathSeparator(const std::string& path) {
        const std::size_t slash = path.find_last_of('/');
        const std::size_t backslash = path.find_last_of('\\');

        if (slash == std::string::npos) {
            return backslash;
        }

        if (backslash == std::string::npos) {
            return slash;
        }

        return std::max(slash, backslash);
    }

    std::string getExtensionLower(const std::string& path) {
        const std::size_t sepPos = findLastPathSeparator(path);
        const std::size_t dotPos = path.find_last_of('.');

        if (dotPos == std::string::npos) {
            return "";
        }

        if (sepPos != std::string::npos && dotPos < sepPos) {
            return "";
        }

        return toLowerCopy(path.substr(dotPos));
    }

    bool hasExtension(const std::string& path) {
        return !getExtensionLower(path).empty();
    }

    bool isWalshFile(const std::string& path) {
        return getExtensionLower(path) == ".walsh";
    }

    bool isVideoInputFile(const std::string& path) {
        const std::string ext = getExtensionLower(path);
        return ext == ".mp4" || ext == ".avi" || ext == ".mkv" || ext == ".mov" || ext == ".webm";
    }

    std::string replaceExtension(const std::string& path, const std::string& newExt) {
        const std::size_t sepPos = findLastPathSeparator(path);
        const std::size_t dotPos = path.find_last_of('.');

        if (dotPos == std::string::npos ||
            (sepPos != std::string::npos && dotPos < sepPos)) {
            return path + newExt;
        }

        return path.substr(0, dotPos) + newExt;
    }

    std::string normalizeWalshVideoPath(const std::string& path) {
        if (path.empty()) {
            return path;
        }

        if (getExtensionLower(path) == ".wlsv") {
            return path;
        }

        if (!hasExtension(path)) {
            return path + ".wlsv";
        }

        return replaceExtension(path, ".wlsv");
    }

    std::string appendBeforeExtension(const std::string& path, const std::string& suffix) {
        const std::size_t sepPos = findLastPathSeparator(path);
        const std::size_t dotPos = path.find_last_of('.');

        if (dotPos == std::string::npos ||
            (sepPos != std::string::npos && dotPos < sepPos)) {
            return path + suffix;
        }

        return path.substr(0, dotPos) + suffix + path.substr(dotPos);
    }

    std::string applySourceImageExtension(
        const std::string& outputPath,
        const std::string& sourcePath)
    {
        if (hasExtension(outputPath)) {
            return outputPath;
        }

        std::string sourceExt = getExtensionLower(sourcePath);

        if (sourceExt.empty() || sourceExt == ".walsh") {
            sourceExt = ".png";
        }

        return outputPath + sourceExt;
    }

    std::string normalizeWalshPath(const std::string& path) {
        if (path.empty()) {
            return path;
        }

        if (getExtensionLower(path) == ".walsh") {
            return path;
        }

        if (!hasExtension(path)) {
            return path + ".walsh";
        }

        return replaceExtension(path, ".walsh");
    }

    std::string getFileNameWithoutExtension(const std::string& path) {
        const std::size_t sepPos = findLastPathSeparator(path);

        const std::size_t nameBegin = (sepPos == std::string::npos) ? 0 : sepPos + 1;

        const std::size_t dotPos = path.find_last_of('.');

        if (dotPos == std::string::npos || dotPos < nameBegin) {
            return path.substr(nameBegin);
        }

        return path.substr(nameBegin, dotPos - nameBegin);
    }

    std::string getParentDirectory(const std::string& path) {
        const std::size_t sepPos = findLastPathSeparator(path);

        if (sepPos == std::string::npos) {
            return "";
        }

        return path.substr(0, sepPos);
    }

    std::string joinPath(const std::string& dir, const std::string& fileName) {
        if (dir.empty()) {
            return fileName;
        }

        const char last = dir.back();

        if (last == '/' || last == '\\') {
            return dir + fileName;
        }

        return dir + "/" + fileName;
    }

    bool ensureDirectoryExists(const std::string& dir) {
        if (dir.empty()) {
            return true;
        }

        const int result = _mkdir(dir.c_str());

        if (result == 0) {
            return true;
        }

        return errno == EEXIST;
    }

    bool tryParseInt(const std::string& text, int& outValue) {
        try {
            std::size_t pos = 0;
            const int value = std::stoi(text, &pos);

            if (pos != text.size()) {
                return false;
            }

            outValue = value;
            return true;
        }
        catch (...) {
            return false;
        }
    }

    int parseRequiredInt(const std::string& text, const std::string& optionName) {
        int value = 0;

        if (!tryParseInt(text, value)) {
            throw std::runtime_error("Invalid integer for " + optionName + ": " + text);
        }

        return value;
    }

    void validateBlockSize(int blockSize) {
        if (blockSize != 2 && blockSize != 4 && blockSize != 8) {
            throw std::runtime_error("Invalid block size. Allowed values: 2, 4, 8.");
        }
    }

    void validateQuantStep(int quantStep) {
        if (quantStep < 1 || quantStep > 255) {
            throw std::runtime_error("Invalid quantization step. Allowed range: 1..255.");
        }
    }

    std::string requireOptionValue(
        int argc,
        char* argv[],
        int& index,
        const std::string& optionName)
    {
        if (index + 1 >= argc) {
            throw std::runtime_error("Missing value for option " + optionName);
        }

        ++index;

        const std::string value = argv[index];

        if (isOption(value)) {
            throw std::runtime_error("Missing value for option " + optionName);
        }

        return value;
    }

    WorkMode parseModeValue(const std::string& value) {
        const std::string lower = toLowerCopy(value);

        if (lower == "gray" || lower == "grayscale") {
            return WorkMode::Grayscale;
        }

        if (lower == "rgb" ||
            lower == "chan" ||
            lower == "channel" ||
            lower == "channels") {
            return WorkMode::RGB;
        }

        if (lower == "ycbcr") {
            return WorkMode::YCbCr;
        }

        throw std::runtime_error(
            "Invalid mode: " + value +
            ". Allowed: gray, grayscale, rgb, ycbcr, chan, channel, channels.");
    }

    const char* modeToString(WorkMode mode) {
        switch (mode) {
            case WorkMode::Grayscale:
                return "grayscale";
            case WorkMode::RGB:
                return "rgb";
            case WorkMode::YCbCr:
                return "ycbcr";
            default:
                return "unknown";
        }
    }

    std::size_t getFileSize(const std::string& path) {
        std::ifstream in(path, std::ios::binary | std::ios::ate);

        if (!in) {
            return 0;
        }

        return static_cast<std::size_t>(in.tellg());
    }

    double toMilliseconds(const std::chrono::steady_clock::duration& d) {
        return std::chrono::duration<double, std::milli>(d).count();
    }

    int clampInt(int value, int minValue, int maxValue) {
        return std::max(minValue, std::min(value, maxValue));
    }

    void printBytesAsKb(std::size_t bytes) {
        const double kb = static_cast<double>(bytes) / 1024.0;
        std::cout << kb << " KB";
    }

    void printEncodeStats(const EncodeStats& stats) {
        std::cout << "\n=== Encode statistics ===\n";

        for (std::size_t c = 0; c < stats.channels.size(); ++c) {
            const auto& s = stats.channels[c];

            double zeroPercent = 0.0;
            if (s.totalCoefficients > 0) {
                zeroPercent =
                    100.0 *
                    static_cast<double>(s.zeroCoefficients) /
                    static_cast<double>(s.totalCoefficients);
            }

            std::cout << "Channel " << c << ":\n";
            std::cout << "  Blocks:              " << s.totalBlocks << "\n";
            std::cout << "  Total coefficients:  " << s.totalCoefficients << "\n";
            std::cout << "  Zero coefficients:   " << s.zeroCoefficients << "\n";
            std::cout << "  Non-zero coeffs:     " << s.nonZeroCoefficients << "\n";
            std::cout << "  Zero percent:        " << zeroPercent << " %\n";
            std::cout << "  Max abs q-value:     " << s.maxAbsQuantizedValue << "\n";
            std::cout << "  Max zero run:        " << s.maxZeroRun << "\n";
        }
    }

    const char* compressionMethodToString(uint32_t method) {
        switch (method) {
            case 0:
                return "none";
            case 1:
                return "zlib";
            default:
                return "unknown";
        }
    }

    std::string walshModeToString(const WalshFileInfo& info) {
        if (info.isGrayscale) {
            return "grayscale";
        }

        if (info.useYCbCr) {
            return "ycbcr";
        }

        return "rgb";
    }

    Image extractChannelAsGrayscale(const Image& src, int channelIndex) {
        if (src.width <= 0 ||
            src.height <= 0 ||
            src.channels <= 0 ||
            src.data.empty()) {
            throw std::runtime_error("extractChannelAsGrayscale: invalid image");
        }

        if (channelIndex < 0 || channelIndex >= src.channels) {
            throw std::runtime_error("extractChannelAsGrayscale: invalid channel index");
        }

        Image channelImage;
        channelImage.width = src.width;
        channelImage.height = src.height;
        channelImage.channels = 1;

        const std::size_t pixelCount =
            static_cast<std::size_t>(src.width) *
            static_cast<std::size_t>(src.height);

        channelImage.data.resize(pixelCount);

        const uint8_t* srcPtr = src.data.data();
        uint8_t* dstPtr = channelImage.data.data();

        for (std::size_t i = 0; i < pixelCount; ++i) {
            dstPtr[i] = srcPtr[channelIndex];
            srcPtr += src.channels;
        }

        return channelImage;
    }

    std::string getChannelName(const EncodedFrame& frame, int channelIndex) {
        if (frame.isGrayscale) {
            return "gray";
        }

        if (frame.useYCbCr) {
            switch (channelIndex) {
                case 0:
                    return "Y";
                case 1:
                    return "Cb";
                case 2:
                    return "Cr";
                default:
                    return "channel_" + std::to_string(channelIndex);
            }
        }

        switch (channelIndex) {
            case 0:
                return "R";
            case 1:
                return "G";
            case 2:
                return "B";
            default:
                return "channel_" + std::to_string(channelIndex);
        }
    }

    Image colorizeYCbCrChannel(const Image& src, int channelIndex) {
        if (src.width <= 0 ||
            src.height <= 0 ||
            src.channels < 3 ||
            src.data.empty()) {
            throw std::runtime_error("colorizeYCbCrChannel: invalid YCbCr image");
        }

        if (channelIndex != 1 && channelIndex != 2) {
            throw std::runtime_error("colorizeYCbCrChannel: only Cb/Cr channels can be colorized");
        }

        Image colorized;
        colorized.width = src.width;
        colorized.height = src.height;
        colorized.channels = 3;

        const std::size_t pixelCount =
            static_cast<std::size_t>(src.width) *
            static_cast<std::size_t>(src.height);

        colorized.data.resize(pixelCount * 3);

        const uint8_t* srcPtr = src.data.data();
        uint8_t* dstPtr = colorized.data.data();

        for (std::size_t i = 0; i < pixelCount; ++i) {
            const int value = srcPtr[channelIndex];

            const int delta = value - 128;
            const int strength = std::min(255, std::abs(delta) * 2);

            int r = 128;
            int g = 128;
            int b = 128;

            if (channelIndex == 1) {
                // Cb: ниже 128 Ч желтоватое, выше 128 Ч синеватое.
                if (delta >= 0) {
                    r = 128 - strength / 2;
                    g = 128 - strength / 2;
                    b = 128 + strength / 2;
                } else {
                    r = 128 + strength / 2;
                    g = 128 + strength / 2;
                    b = 128 - strength / 2;
                }
            } else {
                // Cr: ниже 128 Ч зеленоватое, выше 128 Ч красноватое.
                if (delta >= 0) {
                    r = 128 + strength / 2;
                    g = 128 - strength / 2;
                    b = 128 - strength / 2;
                } else {
                    r = 128 - strength / 2;
                    g = 128 + strength / 2;
                    b = 128 - strength / 2;
                }
            }

            dstPtr[0] = static_cast<uint8_t>(clampInt(r, 0, 255));
            dstPtr[1] = static_cast<uint8_t>(clampInt(g, 0, 255));
            dstPtr[2] = static_cast<uint8_t>(clampInt(b, 0, 255));

            srcPtr += src.channels;
            dstPtr += 3;
        }

        return colorized;
    }

    Image convertGrayToRGB(const Image& src) {
        if (src.width <= 0 ||
            src.height <= 0 ||
            src.channels != 1 ||
            src.data.empty()) {
            throw std::runtime_error("convertGrayToRGB: invalid grayscale image");
        }

        Image rgb;
        rgb.width = src.width;
        rgb.height = src.height;
        rgb.channels = 3;

        const std::size_t pixelCount =
            static_cast<std::size_t>(src.width) *
            static_cast<std::size_t>(src.height);

        rgb.data.resize(pixelCount * 3);

        const uint8_t* srcPtr = src.data.data();
        uint8_t* dstPtr = rgb.data.data();

        for (std::size_t i = 0; i < pixelCount; ++i) {
            const uint8_t value = srcPtr[i];

            dstPtr[0] = value;
            dstPtr[1] = value;
            dstPtr[2] = value;

            dstPtr += 3;
        }

        return rgb;
    }

    Image normalizeImageForDiff(const Image& src) {
        if (src.channels == 1) {
            return convertGrayToRGB(src);
        }

        if (src.channels >= 3) {
            return convertToRGB(src);
        }

        throw std::runtime_error("normalizeImageForDiff: unsupported channel count");
    }

    Image createDiffImage(const Image& a, const Image& b, int amplify, double& outMse, double& outPsnr) {
        if (amplify <= 0) {
            amplify = 1;
        }

        Image left = normalizeImageForDiff(a);
        Image right = normalizeImageForDiff(b);

        if (left.width != right.width ||
            left.height != right.height ||
            left.channels != right.channels ||
            left.data.size() != right.data.size()) {
            throw std::runtime_error("createDiffImage: image sizes do not match");
        }

        outMse = calcMSE(left, right);
        outPsnr = calcPSNR(left, right);

        Image diff;
        diff.width = left.width;
        diff.height = left.height;
        diff.channels = 3;
        diff.data.resize(left.data.size());

        for (std::size_t i = 0; i < left.data.size(); ++i) {
            const int lv = static_cast<int>(left.data[i]);
            const int rv = static_cast<int>(right.data[i]);

            const int delta = std::abs(lv - rv);
            const int visibleDelta = clampInt(delta * amplify, 0, 255);

            diff.data[i] = static_cast<uint8_t>(visibleDelta);
        }

        return diff;
    }

    CliOptions parseCliOptions(int argc, char* argv[]) {
        if (argc < 2) {
            throw std::runtime_error("Missing input path.");
        }

        CliOptions options;
        options.inputPath = argv[1];

        std::vector<std::string> positional;

        for (int i = 2; i < argc; ++i) {
            const std::string arg = argv[i];
            const std::string lower = toLowerCopy(arg);

            if (lower == "-mode" || lower == "-m") {
                const std::string value = requireOptionValue(argc, argv, i, arg);
                options.mode = parseModeValue(value);
                options.modeProvided = true;
                continue;
            }

            if (lower == "-block" || lower == "-b" || lower == "-blocks") {
                const std::string value = requireOptionValue(argc, argv, i, arg);
                options.blockSize = parseRequiredInt(value, arg);
                options.blockProvided = true;
                continue;
            }

            if (lower == "-quant" || lower == "-q" || lower == "-quantize") {
                const std::string value = requireOptionValue(argc, argv, i, arg);
                options.quantStep = parseRequiredInt(value, arg);
                options.quantProvided = true;
                continue;
            }

            if (lower == "-out" || lower == "-o") {
                options.outputImagePath = requireOptionValue(argc, argv, i, arg);
                options.outputImageProvided = true;
                continue;
            }

            if (lower == "-walsh" || lower == "-w") {
                options.outputStreamPath = requireOptionValue(argc, argv, i, arg);
                options.outputStreamProvided = true;
                continue;
            }

            if (lower == "-fps") {
                options.fps = parseRequiredInt(requireOptionValue(argc, argv, i, arg), arg);
                options.fpsProvided = true;
                if (options.fps <= 0) {
                    throw std::runtime_error("FPS must be positive.");
                }
                continue;
            }

            if (lower == "-help" || lower == "--help" || lower == "/?") {
                printUsage(argv[0]);
                std::exit(0);
            }

            positional.push_back(arg);
        }

        validateBlockSize(options.blockSize);
        validateQuantStep(options.quantStep);

        const bool decodeMode = isWalshFile(options.inputPath) || isWalshVideoFile(options.inputPath);

        if (decodeMode) {
            if (!options.outputImageProvided && !positional.empty()) {
                options.outputImagePath = positional[0];
                options.outputImageProvided = true;
            }

            if (isWalshVideoFile(options.inputPath)) {
                if (!options.outputImageProvided) {
                    options.outputImagePath = replaceExtension(options.inputPath, ".mp4");
                } else if (!hasExtension(options.outputImagePath)) {
                    options.outputImagePath += ".mp4";
                }
            } else {
                if (!options.outputImageProvided) {
                    options.outputImagePath = replaceExtension(options.inputPath, ".png");
                } else {
                    options.outputImagePath =
                        applySourceImageExtension(options.outputImagePath, options.inputPath);
                }
            }

            return options;
        }

        std::vector<std::string> numericPositionals;

        for (const std::string& value : positional) {
            int number = 0;

            if (tryParseInt(value, number)) {
                numericPositionals.push_back(value);
                continue;
            }

            if (isWalshFile(value) || isWalshVideoFile(value)) {
                if (!options.outputStreamProvided) {
                    options.outputStreamPath = value;
                    options.outputStreamProvided = true;
                }
            } else {
                if (!options.outputImageProvided) {
                    options.outputImagePath = value;
                    options.outputImageProvided = true;
                }
            }
        }

        if (!options.blockProvided && !numericPositionals.empty()) {
            options.blockSize = parseRequiredInt(numericPositionals[0], "block size");
            validateBlockSize(options.blockSize);
        }

        if (!options.quantProvided && numericPositionals.size() >= 2) {
            options.quantStep = parseRequiredInt(numericPositionals[1], "quantization step");
            validateQuantStep(options.quantStep);
        }

        if (!options.outputImageProvided) {
            options.outputImagePath = appendBeforeExtension(options.inputPath, "_out");
        }

        options.outputImagePath =
            applySourceImageExtension(options.outputImagePath, options.inputPath);

        if (isVideoInputFile(options.inputPath)) {
            if (!options.outputStreamProvided) {
                options.outputStreamPath = replaceExtension(options.inputPath, ".wlsv");
            }

            options.outputStreamPath = normalizeWalshVideoPath(options.outputStreamPath);
        } else {
            if (!options.outputStreamProvided) {
                options.outputStreamPath = replaceExtension(options.inputPath, ".walsh");
            }

            options.outputStreamPath = normalizeWalshPath(options.outputStreamPath);
        }

        return options;
    }

    int runWalshInfo(const std::string& path) {
        WalshFileInfo info{};

        if (!readWalshFileInfo(path, info)) {
            std::cerr << "Error: failed to read .walsh info.\n";
            return 1;
        }

        std::cout << "\n=== Walsh file info ===\n";
        std::cout << "File:                " << info.path << "\n";
        std::cout << "Format:              WLSH v" << info.formatVersion << "\n";
        std::cout << "Compression:         " << compressionMethodToString(info.compressionMethod) << "\n";

        std::cout << "File size:           ";
        printBytesAsKb(info.fileSize);
        std::cout << " (" << info.fileSize << " bytes)\n";

        std::cout << "Raw payload size:    ";
        printBytesAsKb(info.rawPayloadSize);
        std::cout << " (" << info.rawPayloadSize << " bytes)\n";

        std::cout << "Compressed payload:  ";
        printBytesAsKb(info.compressedPayloadSize);
        std::cout << " (" << info.compressedPayloadSize << " bytes)\n";

        if (info.compressedPayloadSize > 0) {
            const double payloadRatio =
                static_cast<double>(info.rawPayloadSize) /
                static_cast<double>(info.compressedPayloadSize);

            std::cout << "Payload ratio:       " << payloadRatio << "x\n";
        }

        std::cout << "\n=== Encoded image ===\n";
        std::cout << "Size:                " << info.width << "x" << info.height << "\n";
        std::cout << "Channels:            " << info.channels << "\n";
        std::cout << "Mode:                " << walshModeToString(info) << "\n";
        std::cout << "Block size:          " << info.blockSize << "\n";
        std::cout << "Quant step:          " << info.quantStep << "\n";

        std::cout << "\n=== Channel statistics ===\n";

        uint64_t totalBlocks = 0;
        uint64_t totalEmptyBlocks = 0;
        uint64_t totalNonZeroTokens = 0;
        std::size_t totalRleInts = 0;

        for (std::size_t i = 0; i < info.channelsInfo.size(); ++i) {
            const WalshChannelInfo& c = info.channelsInfo[i];

            totalBlocks += c.blockCount;
            totalEmptyBlocks += c.emptyBlocks;
            totalNonZeroTokens += c.nonZeroTokens;
            totalRleInts += c.rleIntCount;

            double emptyPercent = 0.0;
            if (c.blockCount > 0) {
                emptyPercent =
                    100.0 *
                    static_cast<double>(c.emptyBlocks) /
                    static_cast<double>(c.blockCount);
            }

            std::cout << "Channel " << i << ":\n";
            std::cout << "  Blocks:            " << c.blockCount << "\n";
            std::cout << "  Empty blocks:      " << c.emptyBlocks << " (" << emptyPercent << " %)\n";
            std::cout << "  RLE ints:          " << c.rleIntCount << "\n";
            std::cout << "  RLE pairs:         " << c.rlePairCount << "\n";
            std::cout << "  Non-zero tokens:   " << c.nonZeroTokens << "\n";
            std::cout << "  Avg tokens/block:  " << c.averageNonZeroTokensPerBlock << "\n";
            std::cout << "  Max zero run:      " << c.maxZeroRun << "\n";
            std::cout << "  Max abs value:     " << c.maxAbsValue << "\n";
        }

        std::cout << "\n=== Summary ===\n";
        std::cout << "Total blocks:        " << totalBlocks << "\n";
        std::cout << "Total empty blocks:  " << totalEmptyBlocks << "\n";
        std::cout << "Total RLE ints:      " << totalRleInts << "\n";
        std::cout << "Total non-zero tokens: " << totalNonZeroTokens << "\n";

        if (totalBlocks > 0) {
            const double avgTokens =
                static_cast<double>(totalNonZeroTokens) /
                static_cast<double>(totalBlocks);

            std::cout << "Avg tokens/block:    " << avgTokens << "\n";
        }

        std::cout << "\nInfo done.\n";

        return 0;
    }

    int runWalshDump(const std::string& walshPath, const std::string& outputDirArg) {
        const auto totalStart = std::chrono::steady_clock::now();

        std::cout << "\n=== Walsh dump ===\n";
        std::cout << "Input stream:       " << walshPath << "\n";

        std::string outputDir = outputDirArg;

        if (outputDir.empty()) {
            outputDir = getParentDirectory(walshPath);
        }

        if (!outputDir.empty()) {
            if (!ensureDirectoryExists(outputDir)) {
                std::cerr << "Error: failed to create output directory: " << outputDir << "\n";
                return 1;
            }
        }

        const std::string baseName = getFileNameWithoutExtension(walshPath);

        std::cout << "Output directory:   " << (outputDir.empty() ? "." : outputDir) << "\n\n";

        const auto loadStart = std::chrono::steady_clock::now();

        EncodedFrame frame{};

        std::cout << "Loading encoded stream...\n";

        if (!loadEncodedFrame(walshPath, frame)) {
            std::cerr << "Error: failed to load encoded stream.\n";
            return 1;
        }

        const auto loadEnd = std::chrono::steady_clock::now();

        std::cout << "Loaded successfully.\n";
        std::cout << "Size:               " << frame.width << "x" << frame.height << "\n";
        std::cout << "Channels:           " << frame.channels << "\n";
        std::cout << "Mode:               " << (frame.isGrayscale ? "grayscale" : (frame.useYCbCr ? "ycbcr" : "rgb")) << "\n";
        std::cout << "Block size:         " << frame.blockSize << "\n";
        std::cout << "Quant step:         " << frame.quantStep << "\n\n";

        const auto decodeStart = std::chrono::steady_clock::now();

        std::cout << "Decoding internal image...\n";

        Image internalImage = decodeFrame(frame);

        const auto decodeEnd = std::chrono::steady_clock::now();

        const auto saveStart = std::chrono::steady_clock::now();

        std::cout << "Saving channel images...\n";

        for (int c = 0; c < internalImage.channels; ++c) {
            Image channelImage = extractChannelAsGrayscale(internalImage, c);

            const std::string channelName = getChannelName(frame, c);

            const std::string channelPath = joinPath(
                outputDir,
                baseName + "_channel_" + std::to_string(c) + "_" + channelName + ".png");

            if (!saveImage(channelPath, channelImage)) {
                std::cerr << "Error: failed to save channel image: " << channelPath << "\n";
                return 1;
            }

            std::cout << "Saved:              " << channelPath << "\n";

            if (frame.useYCbCr && (c == 1 || c == 2)) {
                Image colorizedChannel = colorizeYCbCrChannel(internalImage, c);

                const std::string colorPath = joinPath(
                    outputDir,
                    baseName + "_channel_" + std::to_string(c) + "_" + channelName + "_color.png");

                if (!saveImage(colorPath, colorizedChannel)) {
                    std::cerr << "Error: failed to save colorized channel image: " << colorPath << "\n";
                    return 1;
                }

                std::cout << "Saved:              " << colorPath << "\n";
            }
        }

        Image decodedImage = internalImage;

        if (!frame.isGrayscale && frame.useYCbCr) {
            std::cout << "Converting decoded YCbCr back to RGB...\n";
            decodedImage = convertYCbCrToRGB(internalImage);
        }

        const std::string decodedPath = joinPath(
            outputDir,
            baseName + "_decoded.png");

        if (!saveImage(decodedPath, decodedImage)) {
            std::cerr << "Error: failed to save decoded image: " << decodedPath << "\n";
            return 1;
        }

        std::cout << "Saved:              " << decodedPath << "\n";

        const auto saveEnd = std::chrono::steady_clock::now();
        const auto totalEnd = std::chrono::steady_clock::now();

        std::cout << "\n=== Timing ===\n";
        std::cout << "Load .walsh:        " << toMilliseconds(loadEnd - loadStart) << " ms\n";
        std::cout << "Decode internal:    " << toMilliseconds(decodeEnd - decodeStart) << " ms\n";
        std::cout << "Save dump images:   " << toMilliseconds(saveEnd - saveStart) << " ms\n";
        std::cout << "Total:              " << toMilliseconds(totalEnd - totalStart) << " ms\n";

        std::cout << "\nDump done.\n";

        return 0;
    }

    std::string csvEscape(const std::string& value) {
        bool needQuotes = false;

        for (char c : value) {
            if (c == ',' || c == '"' || c == '\n' || c == '\r') {
                needQuotes = true;
                break;
            }
        }

        if (!needQuotes) {
            return value;
        }

        std::string result;
        result.reserve(value.size() + 2);
        result.push_back('"');

        for (char c : value) {
            if (c == '"') {
                result.push_back('"');
            }

            result.push_back(c);
        }

        result.push_back('"');
        return result;
    }

    void writeCodecResultsCsv(
        const std::string& path,
        const std::vector<CodecTestResult>& results)
    {
        std::ofstream out(path);

        if (!out) {
            throw std::runtime_error("writeCodecResultsCsv: failed to open csv file");
        }

        out
            << "source,"
            << "method,"
            << "mode,"
            << "block,"
            << "quant,"
            << "quality,"
            << "encoded_file,"
            << "preview_file,"
            << "encoded_size_kb,"
            << "preview_size_kb,"
            << "mse,"
            << "psnr,"
            << "encode_ms,"
            << "decode_ms,"
            << "save_encoded_ms,"
            << "save_preview_ms,"
            << "total_ms,"
            << "notes\n";

        for (const CodecTestResult& r : results) {
            out
                << csvEscape(r.source) << ","
                << csvEscape(r.method) << ","
                << csvEscape(r.mode) << ","
                << r.blockSize << ","
                << r.quantStep << ","
                << r.quality << ","
                << csvEscape(r.encodedPath) << ","
                << csvEscape(r.previewPath) << ","
                << (static_cast<double>(r.encodedSizeBytes) / 1024.0) << ","
                << (static_cast<double>(r.previewSizeBytes) / 1024.0) << ","
                << r.mse << ","
                << r.psnr << ","
                << r.encodeMs << ","
                << r.decodeMs << ","
                << r.saveEncodedMs << ","
                << r.savePreviewMs << ","
                << r.totalMs << ","
                << csvEscape(r.notes) << "\n";
        }
    }

    void writeCodecResultsTxt(
        const std::string& path,
        const std::vector<CodecTestResult>& results)
    {
        std::ofstream out(path);

        if (!out) {
            throw std::runtime_error("writeCodecResultsTxt: failed to open txt file");
        }

        out << "=== Codec comparison summary ===\n";

        for (const CodecTestResult& r : results) {
            out << r.source << " | " << r.method << " | mode=" << r.mode;

            if (r.blockSize > 0) {
                out << " | block=" << r.blockSize;
            }

            if (r.quantStep > 0) {
                out << " | q=" << r.quantStep;
            }

            if (r.quality > 0) {
                out << " | quality=" << r.quality;
            }

            out
                << " | encoded=" << (static_cast<double>(r.encodedSizeBytes) / 1024.0) << " KB"
                << " | preview=" << (static_cast<double>(r.previewSizeBytes) / 1024.0) << " KB"
                << " | psnr=" << r.psnr
                << " | mse=" << r.mse
                << " | enc=" << r.encodeMs << " ms"
                << " | dec=" << r.decodeMs << " ms"
                << " | total=" << r.totalMs << " ms";

            if (!r.notes.empty()) {
                out << " | " << r.notes;
            }

            out << "\n";
        }
    }

    CodecTestResult runWalshCodecScenario(
        const Image& sourceImage,
        const std::string& sourceLabel,
        const std::string& outputDir,
        const std::string& baseName,
        WorkMode mode,
        int blockSize,
        int quantStep) 
    {
        CodecTestResult result;
        result.source = sourceLabel;
        result.method = "walsh";
        result.mode = modeToString(mode);
        result.blockSize = blockSize;
        result.quantStep = quantStep;

        const std::string scenarioName =
            baseName +
            "_walsh_" +
            result.mode +
            "_b" +
            std::to_string(blockSize) +
            "_q" +
            std::to_string(quantStep);

        result.encodedPath = joinPath(outputDir, scenarioName + ".walsh");
        result.previewPath = joinPath(outputDir, scenarioName + ".png");

        CodecParams params{};
        params.blockSize = blockSize;
        params.quantStep = quantStep;
        params.printStats = false;

        Image workingImage;
        Image metricReference;

        if (mode == WorkMode::Grayscale) {
            params.isGrayscale = true;
            params.useYCbCr = false;

            workingImage = convertToGrayscale(sourceImage);
            metricReference = workingImage;
        } else {
            Image rgbImage = convertToRGB(sourceImage);

            params.isGrayscale = false;
            params.useYCbCr = (mode == WorkMode::YCbCr);

            if (mode == WorkMode::YCbCr) {
                workingImage = convertRGBToYCbCr(rgbImage);
            } else {
                workingImage = rgbImage;
            }

            metricReference = rgbImage;
        }

        const auto totalStart = std::chrono::steady_clock::now();

        const auto encodeStart = std::chrono::steady_clock::now();

        EncodeStats stats;
        EncodedFrame frame = encodeFrame(workingImage, params, stats);

        const auto encodeEnd = std::chrono::steady_clock::now();

        const auto saveEncodedStart = std::chrono::steady_clock::now();

        if (!saveEncodedFrame(result.encodedPath, frame)) {
            throw std::runtime_error("runWalshCodecScenario: failed to save .walsh");
        }

        const auto saveEncodedEnd = std::chrono::steady_clock::now();

        const auto decodeStart = std::chrono::steady_clock::now();

        Image decodedImage = decodeFrame(frame);

        if (!params.isGrayscale && params.useYCbCr) {
            decodedImage = convertYCbCrToRGB(decodedImage);
        }

        const auto decodeEnd = std::chrono::steady_clock::now();

        const auto savePreviewStart = std::chrono::steady_clock::now();

        if (!saveImage(result.previewPath, decodedImage)) {
            throw std::runtime_error("runWalshCodecScenario: failed to save preview image");
        }

        const auto savePreviewEnd = std::chrono::steady_clock::now();
        const auto totalEnd = std::chrono::steady_clock::now();

        result.mse = calcMSE(metricReference, decodedImage);
        result.psnr = calcPSNR(metricReference, decodedImage);

        result.encodedSizeBytes = getFileSize(result.encodedPath);
        result.previewSizeBytes = getFileSize(result.previewPath);

        result.encodeMs = toMilliseconds(encodeEnd - encodeStart);
        result.saveEncodedMs = toMilliseconds(saveEncodedEnd - saveEncodedStart);
        result.decodeMs = toMilliseconds(decodeEnd - decodeStart);
        result.savePreviewMs = toMilliseconds(savePreviewEnd - savePreviewStart);
        result.totalMs = toMilliseconds(totalEnd - totalStart);

        if (mode == WorkMode::YCbCr) {
            result.notes = "YCbCr uses RGB reference for metrics";
        }

        return result;
    }

    CodecTestResult runJpegCodecScenario(
        const Image& sourceImage,
        const std::string& sourceLabel,
        const std::string& outputDir,
        const std::string& baseName,
        int quality)
    {
        CodecTestResult result;
        result.source = sourceLabel;
        result.method = "jpeg";
        result.mode = "rgb";
        result.quality = quality;

        const std::string scenarioName = baseName + "_jpeg_q" + std::to_string(quality);

        result.encodedPath = joinPath(outputDir, scenarioName + ".jpg");
        result.previewPath = result.encodedPath;

        Image rgbImage = convertToRGB(sourceImage);

        const auto totalStart = std::chrono::steady_clock::now();

        const auto encodeStart = std::chrono::steady_clock::now();

        if (!saveJpegWithQuality(result.encodedPath, rgbImage, quality)) {
            throw std::runtime_error("runJpegCodecScenario: failed to save jpeg");
        }

        const auto encodeEnd = std::chrono::steady_clock::now();

        const auto decodeStart = std::chrono::steady_clock::now();

        Image decodedJpeg;
        if (!loadImage(result.encodedPath, decodedJpeg)) {
            throw std::runtime_error("runJpegCodecScenario: failed to load jpeg");
        }

        Image decodedRgb = normalizeImageForDiff(decodedJpeg);

        const auto decodeEnd = std::chrono::steady_clock::now();
        const auto totalEnd = std::chrono::steady_clock::now();

        result.mse = calcMSE(rgbImage, decodedRgb);
        result.psnr = calcPSNR(rgbImage, decodedRgb);

        result.encodedSizeBytes = getFileSize(result.encodedPath);
        result.previewSizeBytes = result.encodedSizeBytes;

        result.encodeMs = toMilliseconds(encodeEnd - encodeStart);
        result.decodeMs = toMilliseconds(decodeEnd - decodeStart);
        result.totalMs = toMilliseconds(totalEnd - totalStart);

        result.notes = "JPEG quality " + std::to_string(quality);

        return result;
    }

    int runCodecComparisonTest(const std::string& inputPath, const std::string& outputDirArg) {
        const auto totalStart = std::chrono::steady_clock::now();

        std::string outputDir = outputDirArg.empty() ? "codec_tests" : outputDirArg;

        if (!ensureDirectoryExists(outputDir)) {
            std::cerr << "Error: failed to create output directory: "
                << outputDir << "\n";
            return 1;
        }

        std::cout << "\n=== Codec comparison test ===\n";
        std::cout << "Input image:        " << inputPath << "\n";
        std::cout << "Output directory:   " << outputDir << "\n\n";

        Image sourceImage;
        if (!loadImage(inputPath, sourceImage)) {
            std::cerr << "Error: failed to load input image.\n";
            return 1;
        }

        std::cout << "Loaded image:       " << sourceImage.width << "x" << sourceImage.height << ", channels: " << sourceImage.channels << "\n\n";

        const std::string baseName = getFileNameWithoutExtension(inputPath);

        std::vector<CodecTestResult> results;

        const struct WalshScenario {
            WorkMode mode;
            int blockSize;
            int quantStep;
        } walshScenarios[] = {
            { WorkMode::YCbCr, 8, 8 },
            { WorkMode::YCbCr, 8, 16 },
            { WorkMode::YCbCr, 8, 32 },
            { WorkMode::RGB,   8, 16 },
            { WorkMode::Grayscale, 8, 16 },
            { WorkMode::RGB,   4, 16 },
            { WorkMode::YCbCr, 4, 16 },
            { WorkMode::RGB,   2, 8 },
            { WorkMode::YCbCr, 2, 8 }
        };

        for (const WalshScenario& scenario : walshScenarios) {
            std::cout
                << "Running Walsh "
                << modeToString(scenario.mode)
                << " b" << scenario.blockSize
                << " q" << scenario.quantStep
                << "...\n";

            results.push_back(
                runWalshCodecScenario(
                    sourceImage,
                    baseName,
                    outputDir,
                    baseName,
                    scenario.mode,
                    scenario.blockSize,
                    scenario.quantStep));
        }

        const int jpegQualities[] = {
            95, 90, 80, 70, 60
        };

        for (int quality : jpegQualities) {
            std::cout << "Running JPEG quality " << quality << "...\n";

            results.push_back(
                runJpegCodecScenario(
                    sourceImage,
                    baseName,
                    outputDir,
                    baseName,
                    quality));
        }

        const std::string csvPath = joinPath(outputDir, "benchmark_codecs.csv");
        const std::string txtPath = joinPath(outputDir, "benchmark_codecs.txt");

        writeCodecResultsCsv(csvPath, results);
        writeCodecResultsTxt(txtPath, results);

        const auto totalEnd = std::chrono::steady_clock::now();

        std::cout << "\n=== Codec comparison summary ===\n";

        for (const CodecTestResult& r : results) {
            std::cout << r.source << " | " << r.method << " | mode=" << r.mode;

            if (r.blockSize > 0) {
                std::cout << " | block=" << r.blockSize;
            }

            if (r.quantStep > 0) {
                std::cout << " | q=" << r.quantStep;
            }

            if (r.quality > 0) {
                std::cout << " | quality=" << r.quality;
            }

            std::cout
                << " | encoded=" << (static_cast<double>(r.encodedSizeBytes) / 1024.0) << " KB"
                << " | psnr=" << r.psnr
                << " | mse=" << r.mse
                << " | enc=" << r.encodeMs << " ms"
                << " | dec=" << r.decodeMs << " ms"
                << "\n";
        }

        std::cout << "\nSaved CSV:          " << csvPath << "\n";
        std::cout << "Saved TXT:          " << txtPath << "\n";
        std::cout << "Total time:         " << toMilliseconds(totalEnd - totalStart) << " ms\n";

        std::cout << "\nCodec comparison done.\n";

        return 0;
    }

    int runDiff(const std::string& imageAPath, const std::string& imageBPath, const std::string& diffOutputPath, int amplify) {
        const auto totalStart = std::chrono::steady_clock::now();

        std::cout << "\n=== Difference map ===\n";
        std::cout << "Image A:             " << imageAPath << "\n";
        std::cout << "Image B:             " << imageBPath << "\n";
        std::cout << "Output diff:         " << diffOutputPath << "\n";
        std::cout << "Amplify:             " << amplify << "x\n\n";

        Image imageA{};
        Image imageB{};

        const auto loadStart = std::chrono::steady_clock::now();

        if (!loadImage(imageAPath, imageA)) {
            std::cerr << "Error: failed to load image A.\n";
            return 1;
        }

        if (!loadImage(imageBPath, imageB)) {
            std::cerr << "Error: failed to load image B.\n";
            return 1;
        }

        const auto loadEnd = std::chrono::steady_clock::now();

        std::cout << "Image A size:        " << imageA.width << "x" << imageA.height << ", channels: " << imageA.channels << "\n";

        std::cout << "Image B size:        " << imageB.width << "x" << imageB.height << ", channels: " << imageB.channels << "\n";

        double mse = 0.0;
        double psnr = 0.0;

        const auto diffStart = std::chrono::steady_clock::now();

        Image diffImage = createDiffImage(imageA, imageB, amplify, mse, psnr);

        const auto diffEnd = std::chrono::steady_clock::now();

        const auto saveStart = std::chrono::steady_clock::now();

        if (!saveImage(diffOutputPath, diffImage)) {
            std::cerr << "Error: failed to save diff image.\n";
            return 1;
        }

        const auto saveEnd = std::chrono::steady_clock::now();
        const auto totalEnd = std::chrono::steady_clock::now();

        std::cout << "\n=== Metrics ===\n";
        std::cout << "MSE:                 " << mse << "\n";
        std::cout << "PSNR:                " << psnr << " dB\n";

        std::cout << "\n=== Timing ===\n";
        std::cout << "Load images:         " << toMilliseconds(loadEnd - loadStart) << " ms\n";
        std::cout << "Create diff:         " << toMilliseconds(diffEnd - diffStart) << " ms\n";
        std::cout << "Save diff:           " << toMilliseconds(saveEnd - saveStart) << " ms\n";
        std::cout << "Total:               " << toMilliseconds(totalEnd - totalStart) << " ms\n";

        std::cout << "\nDiff saved: " << diffOutputPath << "\n";

        return 0;
    }

    int runEncode(const CliOptions& options) {
        const auto totalStart = std::chrono::steady_clock::now();

        CodecParams params{};
        params.blockSize = options.blockSize;
        params.quantStep = options.quantStep;
        params.printStats = true;

        switch (options.mode) {
            case WorkMode::Grayscale:
                params.isGrayscale = true;
                params.useYCbCr = false;
                break;

            case WorkMode::RGB:
                params.isGrayscale = false;
                params.useYCbCr = false;
                break;

            case WorkMode::YCbCr:
                params.isGrayscale = false;
                params.useYCbCr = true;
                break;
        }

        std::cout << "\n=== Encode parameters ===\n";
        std::cout << "Input image:        " << options.inputPath << "\n";
        std::cout << "Output image:       " << options.outputImagePath << "\n";
        std::cout << "Output stream:      " << options.outputStreamPath << "\n";
        std::cout << "Mode:               " << modeToString(options.mode) << "\n";
        std::cout << "Block size:         " << params.blockSize << "\n";
        std::cout << "Quantization step:  " << params.quantStep << "\n\n";

        const auto loadStart = std::chrono::steady_clock::now();

        std::cout << "Loading image...\n";

        Image srcImage{};
        if (!loadImage(options.inputPath, srcImage)) {
            std::cerr << "Error: failed to load input image.\n";
            return 1;
        }

        const auto loadEnd = std::chrono::steady_clock::now();

        std::cout << "Loaded successfully.\n";
        std::cout << "Width: " << srcImage.width << ", Height: " << srcImage.height << ", Channels: " << srcImage.channels << "\n";

        const auto prepStart = std::chrono::steady_clock::now();

        Image workingImage{};
        Image metricReference{};

        if (options.mode == WorkMode::Grayscale) {
            std::cout << "Converting to grayscale...\n";

            workingImage = convertToGrayscale(srcImage);
            metricReference = workingImage;
        } else {
            if (srcImage.channels < 3) {
                throw std::runtime_error(
                    "Selected color mode requires an image with at least 3 channels.");
            }

            Image rgbImage = convertToRGB(srcImage);

            if (options.mode == WorkMode::YCbCr) {
                std::cout << "Converting RGB to YCbCr...\n";

                workingImage = convertRGBToYCbCr(rgbImage);
            } else {
                std::cout << "Using RGB channels separately...\n";

                workingImage = rgbImage;
            }

            metricReference = rgbImage;
        }

        const auto prepEnd = std::chrono::steady_clock::now();

        const auto encodeStart = std::chrono::steady_clock::now();

        std::cout << "Encoding...\n";

        EncodeStats stats;
        EncodedFrame encoded = encodeFrame(workingImage, params, stats);

        const auto encodeEnd = std::chrono::steady_clock::now();

        const auto saveStreamStart = std::chrono::steady_clock::now();

        std::cout << "Saving encoded stream...\n";

        if (!saveEncodedFrame(options.outputStreamPath, encoded)) {
            std::cerr << "Error: failed to save encoded stream.\n";
            return 1;
        }

        const auto saveStreamEnd = std::chrono::steady_clock::now();

        const auto decodeStart = std::chrono::steady_clock::now();

        std::cout << "Decoding for preview/output image...\n";

        Image decodedImage = decodeFrame(encoded);

        if (!params.isGrayscale && params.useYCbCr) {
            std::cout << "Converting decoded YCbCr back to RGB...\n";
            decodedImage = convertYCbCrToRGB(decodedImage);
        }

        const auto decodeEnd = std::chrono::steady_clock::now();

        const auto saveImageStart = std::chrono::steady_clock::now();

        std::cout << "Saving output image...\n";

        if (!saveImage(options.outputImagePath, decodedImage)) {
            std::cerr << "Error: failed to save output image.\n";
            return 1;
        }

        const auto saveImageEnd = std::chrono::steady_clock::now();

        const double mse = calcMSE(metricReference, decodedImage);
        const double psnr = calcPSNR(metricReference, decodedImage);

        const std::size_t originalSizeBytes = params.isGrayscale ? workingImage.data.size() : metricReference.data.size();

        const std::size_t compressedSizeBytes = getFileSize(options.outputStreamPath);

        double compressionRatio = 0.0;
        if (compressedSizeBytes > 0) {
            compressionRatio =
                static_cast<double>(originalSizeBytes) /
                static_cast<double>(compressedSizeBytes);
        }

        const auto totalEnd = std::chrono::steady_clock::now();

        std::cout << "\n=== Results ===\n";
        std::cout << "Original size:      " << originalSizeBytes << " bytes\n";
        std::cout << "Walsh stream size:  " << compressedSizeBytes << " bytes\n";
        std::cout << "Compression ratio:  " << compressionRatio << "\n";
        std::cout << "MSE:                " << mse << "\n";
        std::cout << "PSNR:               " << psnr << " dB\n";
        std::cout << "Saved image:        " << options.outputImagePath << "\n";
        std::cout << "Saved stream:       " << options.outputStreamPath << "\n";

        std::cout << "\n=== Timing ===\n";
        std::cout << "Load image:         " << toMilliseconds(loadEnd - loadStart) << " ms\n";
        std::cout << "Prepare image:      " << toMilliseconds(prepEnd - prepStart) << " ms\n";
        std::cout << "Encode:             " << toMilliseconds(encodeEnd - encodeStart) << " ms\n";
        std::cout << "Save .walsh:        " << toMilliseconds(saveStreamEnd - saveStreamStart) << " ms\n";
        std::cout << "Decode:             " << toMilliseconds(decodeEnd - decodeStart) << " ms\n";
        std::cout << "Save image:         " << toMilliseconds(saveImageEnd - saveImageStart) << " ms\n";
        std::cout << "Total:              " << toMilliseconds(totalEnd - totalStart) << " ms\n";

        printEncodeStats(stats);

        std::cout << "\nEncode done.\n";

        return 0;
    }

    Image prepareImageForCodecMode(const Image& sourceImage, WorkMode mode, Image* metricReference) {
        if (mode == WorkMode::Grayscale) {
            Image gray = convertToGrayscale(sourceImage);
            if (metricReference) {
                *metricReference = gray;
            }
            return gray;
        }

        if (sourceImage.channels < 3) {
            throw std::runtime_error("Selected color mode requires an image with at least 3 channels.");
        }

        Image rgbImage = convertToRGB(sourceImage);
        if (metricReference) {
            *metricReference = rgbImage;
        }

        if (mode == WorkMode::YCbCr) {
            return convertRGBToYCbCr(rgbImage);
        }

        return rgbImage;
    }

    CodecParams makeCodecParams(const CliOptions& options) {
        CodecParams params{};
        params.blockSize = options.blockSize;
        params.quantStep = options.quantStep;
        params.printStats = false;

        switch (options.mode) {
            case WorkMode::Grayscale:
                params.isGrayscale = true;
                params.useYCbCr = false;
                break;

            case WorkMode::RGB:
                params.isGrayscale = false;
                params.useYCbCr = false;
                break;

            case WorkMode::YCbCr:
                params.isGrayscale = false;
                params.useYCbCr = true;
                break;
        }

        return params;
    }

    // Video mode keeps the codec intra-only: FFmpeg extracts RGB frames,
    // and every selected frame is encoded independently by encodeFrame().
    int runEncodeVideo(const CliOptions& options) {
        const auto totalStart = std::chrono::steady_clock::now();

        CodecParams params = makeCodecParams(options);

        std::string outputPath = options.outputStreamPath;
        if (!options.outputStreamProvided) {
            outputPath = replaceExtension(options.inputPath, ".wlsv");
        } else {
            outputPath = normalizeWalshVideoPath(outputPath);
        }

        std::cout << "\n=== Walsh video encode parameters ===\n";
        std::cout << "Input video:        " << options.inputPath << "\n";
        std::cout << "Output stream:      " << outputPath << "\n";
        std::cout << "FPS:                " << options.fps << "\n";
        std::cout << "Mode:               " << modeToString(options.mode) << "\n";
        std::cout << "Block size:         " << params.blockSize << "\n";
        std::cout << "Quantization step:  " << params.quantStep << "\n\n";

        WalshVideoWriter writer;
        bool writerOpened = false;
        int encodedFrames = 0;
        std::string errorMessage;

        const auto readEncodeStart = std::chrono::steady_clock::now();

        VideoInputInfo inputInfo{};
        const bool ok = readVideoFramesFFmpeg(
            options.inputPath,
            options.fps,
            [&](const Image& rgbFrame, int selectedIndex) -> bool {
                try {
                    Image workingImage = prepareImageForCodecMode(rgbFrame, options.mode, nullptr);

                    if (!writerOpened) {
                        WalshVideoHeader header{};
                        header.width = workingImage.width;
                        header.height = workingImage.height;
                        header.fps = options.fps;
                        header.blockSize = params.blockSize;
                        header.quantStep = params.quantStep;
                        header.isGrayscale = params.isGrayscale;
                        header.useYCbCr = params.useYCbCr;

                        if (!writer.open(outputPath, header)) {
                            errorMessage = "failed to open .wlsv writer";
                            return false;
                        }

                        writerOpened = true;
                    }

                    EncodedFrame encodedFrame = encodeFrame(workingImage, params);

                    if (!writer.writeFrame(encodedFrame)) {
                        errorMessage = "failed to write encoded frame to .wlsv";
                        return false;
                    }

                    ++encodedFrames;

                    if ((selectedIndex + 1) % 10 == 0) {
                        std::cout << "Encoded frames:     " << (selectedIndex + 1) << "\r";
                    }

                    return true;
                }
                catch (const std::exception& ex) {
                    errorMessage = ex.what();
                    return false;
                }
            },
            inputInfo,
            errorMessage);

        if (!ok) {
            std::cerr << "Error: video encode failed: " << errorMessage << "\n";
            return 1;
        }

        if (!writerOpened || encodedFrames <= 0) {
            std::cerr << "Error: no frames were selected from input video.\n";
            return 1;
        }

        if (!writer.close()) {
            std::cerr << "Error: failed to finalize .wlsv file.\n";
            return 1;
        }

        const auto readEncodeEnd = std::chrono::steady_clock::now();
        const auto totalEnd = std::chrono::steady_clock::now();

        std::cout << "\n\n=== Walsh video result ===\n";
        std::cout << "Input size:         " << inputInfo.width << "x" << inputInfo.height << "\n";
        std::cout << "Source FPS:         " << inputInfo.sourceFps << "\n";
        std::cout << "Selected FPS:       " << options.fps << "\n";
        std::cout << "Frames encoded:     " << encodedFrames << "\n";
        std::cout << "Output stream:      " << outputPath << "\n";
        std::cout << "Output size:        ";
        printBytesAsKb(getFileSize(outputPath));
        std::cout << "\n";

        std::cout << "\n=== Timing ===\n";
        std::cout << "Read + encode:      " << toMilliseconds(readEncodeEnd - readEncodeStart) << " ms\n";
        std::cout << "Total:              " << toMilliseconds(totalEnd - totalStart) << " ms\n";

        std::cout << "\nWalsh video encode done.\n";
        return 0;
    }

    // Decode .wlsv by restoring every EncodedFrame and passing RGB frames
    // to FFmpegVideoWriter for final MP4 muxing/encoding.
    int runDecodeVideo(const CliOptions& options) {
        const auto totalStart = std::chrono::steady_clock::now();

        std::string outputPath = options.outputImagePath;
        if (!options.outputImageProvided) {
            outputPath = replaceExtension(options.inputPath, ".mp4");
        }

        std::cout << "\n=== Walsh video decode parameters ===\n";
        std::cout << "Input stream:       " << options.inputPath << "\n";
        std::cout << "Output video:       " << outputPath << "\n\n";

        WalshVideoReader reader;
        if (!reader.open(options.inputPath)) {
            std::cerr << "Error: failed to open .wlsv stream.\n";
            return 1;
        }

        const WalshVideoHeader& header = reader.header();

        FFmpegVideoWriter writer;
        std::string errorMessage;
        if (!writer.open(outputPath, header.width, header.height, header.fps, errorMessage)) {
            std::cerr << "Error: failed to open output video: " << errorMessage << "\n";
            return 1;
        }

        int decodedFrames = 0;
        EncodedFrame encodedFrame{};

        const auto decodeStart = std::chrono::steady_clock::now();

        while (reader.readNextFrame(encodedFrame)) {
            Image decodedImage = decodeFrame(encodedFrame);

            if (!encodedFrame.isGrayscale && encodedFrame.useYCbCr) {
                decodedImage = convertYCbCrToRGB(decodedImage);
            } else if (encodedFrame.isGrayscale) {
                decodedImage = convertGrayToRGB(decodedImage);
            } else {
                decodedImage = convertToRGB(decodedImage);
            }

            if (!writer.writeFrame(decodedImage, errorMessage)) {
                std::cerr << "Error: failed to write MP4 frame: " << errorMessage << "\n";
                return 1;
            }

            ++decodedFrames;

            if (decodedFrames % 10 == 0) {
                std::cout << "Decoded frames:     " << decodedFrames << "\r";
            }
        }

        if (!writer.close(errorMessage)) {
            std::cerr << "Error: failed to finalize MP4: " << errorMessage << "\n";
            return 1;
        }

        const auto decodeEnd = std::chrono::steady_clock::now();
        const auto totalEnd = std::chrono::steady_clock::now();

        std::cout << "\n\n=== Walsh video decode result ===\n";
        std::cout << "Size:               " << header.width << "x" << header.height << "\n";
        std::cout << "FPS:                " << header.fps << "\n";
        std::cout << "Frames decoded:     " << decodedFrames << "\n";
        std::cout << "Output video:       " << outputPath << "\n";
        std::cout << "Output size:        ";
        printBytesAsKb(getFileSize(outputPath));
        std::cout << "\n";

        std::cout << "\n=== Timing ===\n";
        std::cout << "Decode + write:     " << toMilliseconds(decodeEnd - decodeStart) << " ms\n";
        std::cout << "Total:              " << toMilliseconds(totalEnd - totalStart) << " ms\n";

        std::cout << "\nWalsh video decode done.\n";
        return 0;
    }

    int runDecode(const CliOptions& options) {
        const auto totalStart = std::chrono::steady_clock::now();

        std::cout << "\n=== Decode parameters ===\n";
        std::cout << "Input stream:       " << options.inputPath << "\n";
        std::cout << "Output image:       " << options.outputImagePath << "\n\n";

        const auto loadStart = std::chrono::steady_clock::now();

        EncodedFrame frame{};

        std::cout << "Loading encoded stream...\n";

        if (!loadEncodedFrame(options.inputPath, frame)) {
            std::cerr << "Error: failed to load encoded stream.\n";
            return 1;
        }

        const auto loadEnd = std::chrono::steady_clock::now();

        std::cout << "Loaded successfully.\n";
        std::cout << "Width: " << frame.width
            << ", Height: " << frame.height
            << ", Channels: " << frame.channels
            << ", Block size: " << frame.blockSize
            << ", Quant step: " << frame.quantStep
            << ", Grayscale: " << (frame.isGrayscale ? "yes" : "no")
            << ", YCbCr: " << (frame.useYCbCr ? "yes" : "no") << "\n";

        const auto decodeStart = std::chrono::steady_clock::now();

        std::cout << "Decoding image...\n";

        Image decodedImage = decodeFrame(frame);

        if (!frame.isGrayscale && frame.useYCbCr) {
            std::cout << "Converting decoded YCbCr back to RGB...\n";
            decodedImage = convertYCbCrToRGB(decodedImage);
        }

        const auto decodeEnd = std::chrono::steady_clock::now();

        const auto saveStart = std::chrono::steady_clock::now();

        std::cout << "Saving output image...\n";

        if (!saveImage(options.outputImagePath, decodedImage)) {
            std::cerr << "Error: failed to save output image.\n";
            return 1;
        }

        const auto saveEnd = std::chrono::steady_clock::now();

        const auto totalEnd = std::chrono::steady_clock::now();

        std::cout << "\n=== Timing ===\n";
        std::cout << "Load .walsh:        " << toMilliseconds(loadEnd - loadStart) << " ms\n";
        std::cout << "Decode:             " << toMilliseconds(decodeEnd - decodeStart) << " ms\n";
        std::cout << "Save image:         " << toMilliseconds(saveEnd - saveStart) << " ms\n";
        std::cout << "Total:              " << toMilliseconds(totalEnd - totalStart) << " ms\n";

        std::cout << "\nDecode done.\n";
        std::cout << "Saved image: " << options.outputImagePath << "\n";

        return 0;
    }
}

int main(int argc, char* argv[]) {
    try {
        if (argc < 2) {
            printUsage(argv[0]);
            return 1;
        }

        const std::string firstArg = argv[1];
        const std::string firstLower = toLowerCopy(firstArg);

        if (firstLower == "-help" ||
            firstLower == "--help" ||
            firstLower == "/?") {
            printUsage(argv[0]);
            return 0;
        }

        if (firstLower == "-test") {
            if (argc < 3) {
                printUsage(argv[0]);
                return 1;
            }

            const std::string inputPath = argv[2];
            const std::string outputDir = (argc >= 4) ? argv[3] : "test_results";

            return runAutomatedTest(inputPath, outputDir);
        }

        if (firstLower == "-test-codecs" ||
            firstLower == "--test-codecs" ||
            firstLower == "-codec-test" ||
            firstLower == "--codec-test") {
            if (argc < 3) {
                printUsage(argv[0]);
                return 1;
            }

            const std::string inputPath = argv[2];
            const std::string outputDir = (argc >= 4) ? argv[3] : "codec_tests";

            return runCodecComparisonTest(inputPath, outputDir);
        }

        // Special mode:
        //   diplom.exe -info file.walsh
        //   diplom.exe file.walsh -info
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            const std::string lowerArg = toLowerCopy(arg);

            if (lowerArg == "-info" || lowerArg == "--info" || lowerArg == "info") {
                std::string infoPath;

                if (i == 1) {
                    if (i + 1 < argc && !isOption(argv[i + 1])) {
                        infoPath = argv[i + 1];
                    }
                } else {
                    if (argc >= 2 && isWalshFile(argv[1])) {
                        infoPath = argv[1];
                    } else if (i + 1 < argc && !isOption(argv[i + 1])) {
                        infoPath = argv[i + 1];
                    }
                }

                if (infoPath.empty()) {
                    std::cerr << "Error: missing .walsh file for -info.\n";
                    return 1;
                }

                if (!isWalshFile(infoPath)) {
                    std::cerr << "Error: -info expects a .walsh file.\n";
                    return 1;
                }

                return runWalshInfo(infoPath);
            }
        }

        // Special mode:
        //   diplom.exe -dump file.walsh [output_dir]
        //   diplom.exe file.walsh -dump [output_dir]
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            const std::string lowerArg = toLowerCopy(arg);

            if (lowerArg == "-dump" || lowerArg == "--dump" || lowerArg == "dump") {
                std::string walshPath;
                std::string outputDir;

                if (i == 1) {
                    if (i + 1 >= argc || isOption(argv[i + 1])) {
                        std::cerr << "Error: missing .walsh file for -dump.\n";
                        return 1;
                    }

                    walshPath = argv[i + 1];

                    if (i + 2 < argc && !isOption(argv[i + 2])) {
                        outputDir = argv[i + 2];
                    }
                } else {
                    if (argc >= 2 && isWalshFile(argv[1])) {
                        walshPath = argv[1];

                        if (i + 1 < argc && !isOption(argv[i + 1])) {
                            outputDir = argv[i + 1];
                        }
                    } else if (i + 1 < argc && !isOption(argv[i + 1])) {
                        walshPath = argv[i + 1];

                        if (i + 2 < argc && !isOption(argv[i + 2])) {
                            outputDir = argv[i + 2];
                        }
                    } else {
                        std::cerr << "Error: missing .walsh file for -dump.\n";
                        return 1;
                    }
                }

                if (!isWalshFile(walshPath)) {
                    std::cerr << "Error: -dump expects a .walsh file.\n";
                    return 1;
                }

                return runWalshDump(walshPath, outputDir);
            }
        }

        // Special mode:
        //   diplom.exe -diff image_a image_b diff_output [amplify]
        //   diplom.exe image_a -diff image_b diff_output [amplify]
        for (int i = 1; i < argc; ++i) {
            const std::string arg = argv[i];
            const std::string lowerArg = toLowerCopy(arg);

            if (lowerArg == "-diff" || lowerArg == "--diff" || lowerArg == "diff") {
                std::string imageAPath;
                std::string imageBPath;
                std::string diffOutputPath;
                int amplify = 4;

                if (i == 1) {
                    if (argc < 5) {
                        std::cerr << "Error: usage is -diff <image_a> <image_b> <diff_output> [amplify].\n";
                        return 1;
                    }

                    imageAPath = argv[2];
                    imageBPath = argv[3];
                    diffOutputPath = argv[4];

                    if (argc >= 6) {
                        amplify = parseRequiredInt(argv[5], "diff amplify");
                    }
                } else {
                    if (argc <= i + 2) {
                        std::cerr << "Error: usage is <image_a> -diff <image_b> <diff_output> [amplify].\n";
                        return 1;
                    }

                    imageAPath = argv[1];
                    imageBPath = argv[i + 1];
                    diffOutputPath = argv[i + 2];

                    if (i + 3 < argc && !isOption(argv[i + 3])) {
                        amplify = parseRequiredInt(argv[i + 3], "diff amplify");
                    }
                }

                if (amplify <= 0) {
                    std::cerr << "Error: diff amplify must be positive.\n";
                    return 1;
                }

                return runDiff(imageAPath, imageBPath, diffOutputPath, amplify);
            }
        }

        const CliOptions options = parseCliOptions(argc, argv);

        if (isWalshVideoFile(options.inputPath)) {
            return runDecodeVideo(options);
        }

        if (isWalshFile(options.inputPath)) {
            return runDecode(options);
        }

        if (isVideoInputFile(options.inputPath)) {
            return runEncodeVideo(options);
        }

        return runEncode(options);
    }
    catch (const std::invalid_argument&) {
        std::cerr << "Error: invalid numeric argument.\n";
        return 1;
    }
    catch (const std::out_of_range&) {
        std::cerr << "Error: numeric argument is out of range.\n";
        return 1;
    }
    catch (const std::exception& ex) {
        std::cerr << "Unhandled exception: " << ex.what() << "\n";
        return 1;
    }
    catch (...) {
        std::cerr << "Unhandled unknown exception.\n";
        return 1;
    }
}