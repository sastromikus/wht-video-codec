#include "test_runner.h"

#include <iostream>
#include <fstream>
#include <vector>
#include <string>
#include <chrono>
#include <algorithm>
#include <direct.h>
#include <cerrno>
#include <windows.h>
#include <intrin.h>
#include <sstream>
#include <cstring>

#include "types.h"
#include "image_io.h"
#include "codec.h"
#include "metrics.h"
#include "encoded_io.h"

namespace {
    struct TestScenario {
        std::string name;
        bool isGrayscale = true;
        bool useYCbCr = false;
        int blockSize = 8;
        int quantStep = 16;
    };

    struct BenchmarkConfig {
        std::string buildConfig;
        std::string platform;
        std::string cppStandard;
        std::string os;

        std::string cpuName;
        unsigned int logicalCores = 0;
        unsigned long long ramMB = 0;
    };

    struct TestResult {
        std::string inputName;
        std::string scenarioName;
        std::string mode;

        int width = 0;
        int height = 0;
        int channels = 0;

        int blockSize = 8;
        int quantStep = 16;

        std::size_t inputFileBytes = 0;
        std::size_t originalSizeBytes = 0;
        std::size_t walshSizeBytes = 0;
        std::size_t outputImageBytes = 0;

        double compressionRatio = 0.0;
        double mse = 0.0;
        double psnr = 0.0;

        double loadMs = 0.0;
        double prepareMs = 0.0;
        double encodeMs = 0.0;
        double saveWalshMs = 0.0;
        double decodeMs = 0.0;
        double saveImageMs = 0.0;
        double totalMs = 0.0;

        double zeroPercentCh0 = 0.0;
        double zeroPercentCh1 = 0.0;
        double zeroPercentCh2 = 0.0;
        double avgZeroPercent = 0.0;
    };

    static std::string getCpuName() {
        int cpuInfo[4] = { -1, -1, -1, -1 };
        char brand[49] = {};

        __cpuid(cpuInfo, 0x80000000);
        const unsigned int maxExtId = static_cast<unsigned int>(cpuInfo[0]);

        if (maxExtId >= 0x80000004) {
            __cpuid(cpuInfo, 0x80000002);
            std::memcpy(brand + 0, cpuInfo, sizeof(cpuInfo));

            __cpuid(cpuInfo, 0x80000003);
            std::memcpy(brand + 16, cpuInfo, sizeof(cpuInfo));

            __cpuid(cpuInfo, 0x80000004);
            std::memcpy(brand + 32, cpuInfo, sizeof(cpuInfo));
        }

        std::string name = brand;
        while (!name.empty() && (name.back() == ' ' || name.back() == '\0')) {
            name.pop_back();
        }

        return name.empty() ? "Unknown CPU" : name;
    }

    static unsigned int getLogicalCoreCount() {
        SYSTEM_INFO si{};
        GetSystemInfo(&si);
        return si.dwNumberOfProcessors;
    }

    static unsigned long long getInstalledRamMB() {
        MEMORYSTATUSEX mem{};
        mem.dwLength = sizeof(mem);

        if (!GlobalMemoryStatusEx(&mem)) {
            return 0;
        }

        return static_cast<unsigned long long>(mem.ullTotalPhys / (1024ull * 1024ull));
    }

    std::string toLowerCopy(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        return s;
    }

    std::string getExtensionLower(const std::string& path) {
        const std::size_t dotPos = path.find_last_of('.');
        if (dotPos == std::string::npos) {
            return "";
        }
        return toLowerCopy(path.substr(dotPos));
    }

    bool isLosslessInput(const std::string& path) {
        const std::string ext = getExtensionLower(path);
        return ext == ".png" || ext == ".bmp";
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

    std::string buildModeName(bool isGrayscale, bool useYCbCr) {
        if (isGrayscale) {
            return "grayscale";
        }
        return useYCbCr ? "ycbcr" : "rgb";
    }

    std::string getBaseName(const std::string& path) {
        std::size_t slashPos = path.find_last_of("\\/");
        std::string fileName = (slashPos == std::string::npos) ? path : path.substr(slashPos + 1);

        std::size_t dotPos = fileName.find_last_of('.');
        if (dotPos == std::string::npos) {
            return fileName;
        }

        return fileName.substr(0, dotPos);
    }

    std::string joinPath(const std::string& dir, const std::string& fileName) {
        if (dir.empty()) {
            return fileName;
        }

        const char last = dir.back();
        if (last == '\\' || last == '/') {
            return dir + fileName;
        }

        return dir + "\\" + fileName;
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

    bool pathIsDirectory(const std::string& path) {
        DWORD attrs = GetFileAttributesA(path.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            return false;
        }
        return (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    }

    std::vector<std::string> collectInputFiles(const std::string& inputPathOrDir) {
        std::vector<std::string> files;

        if (isLosslessInput(inputPathOrDir)) {
            files.push_back(inputPathOrDir);
            return files;
        }

        if (!pathIsDirectory(inputPathOrDir)) {
            return files;
        }

        const std::string pattern = joinPath(inputPathOrDir, "*.*");

        WIN32_FIND_DATAA findData{};
        HANDLE hFind = FindFirstFileA(pattern.c_str(), &findData);
        if (hFind == INVALID_HANDLE_VALUE) {
            return files;
        }

        do {
            if (findData.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
                continue;
            }

            const std::string name = findData.cFileName;
            const std::string fullPath = joinPath(inputPathOrDir, name);

            if (isLosslessInput(fullPath)) {
                files.push_back(fullPath);
            }
        }
        while (FindNextFileA(hFind, &findData));

        FindClose(hFind);

        std::sort(files.begin(), files.end());
        return files;
    }

    BenchmarkConfig getBenchmarkConfig() {
        BenchmarkConfig cfg{};

    #ifdef _DEBUG
        cfg.buildConfig = "Debug";
    #else
        cfg.buildConfig = "Release";
    #endif

    #ifdef _WIN64
        cfg.platform = "x64";
    #elif defined(_WIN32)
        cfg.platform = "x86";
    #else
        cfg.platform = "unknown";
    #endif

    #if __cplusplus >= 202302L
        cfg.cppStandard = "C++23_or_newer";
    #elif __cplusplus >= 202002L
        cfg.cppStandard = "C++20";
    #elif __cplusplus >= 201703L
        cfg.cppStandard = "C++17";
    #elif __cplusplus >= 201402L
        cfg.cppStandard = "C++14";
    #else
        cfg.cppStandard = "pre-C++14";
    #endif

        cfg.os = "Windows";
        cfg.cpuName = getCpuName();
        cfg.logicalCores = getLogicalCoreCount();
        cfg.ramMB = getInstalledRamMB();

        return cfg;
    }

    bool saveCsvReport(const std::string& csvPath, const BenchmarkConfig& cfg, const std::vector<TestResult>& results) {
        std::ofstream out(csvPath);
        if (!out) {
            return false;
        }

    out << "build_config,platform,cpp_standard,os,cpu_name,logical_cores,ram_mb,"
           "input_name,scenario_name,mode,width,height,channels,"
           "block_size,quant_step,"
           "input_file_bytes,original_size_bytes,walsh_size_bytes,output_image_bytes,"
           "compression_ratio,mse,psnr,"
           "load_ms,prepare_ms,encode_ms,save_walsh_ms,decode_ms,save_image_ms,total_ms,"
           "zero_percent_ch0,zero_percent_ch1,zero_percent_ch2,avg_zero_percent\n";

        for (const auto& r : results) {
            out << cfg.buildConfig << ','
                << cfg.platform << ','
                << cfg.cppStandard << ','
                << cfg.os << ','
                << '"' << cfg.cpuName << '"' << ','
                << cfg.logicalCores << ','
                << cfg.ramMB << ','
                << r.inputName << ','
                << r.scenarioName << ','
                << r.mode << ','
                << r.width << ','
                << r.height << ','
                << r.channels << ','
                << r.blockSize << ','
                << r.quantStep << ','
                << r.inputFileBytes << ','
                << r.originalSizeBytes << ','
                << r.walshSizeBytes << ','
                << r.outputImageBytes << ','
                << r.compressionRatio << ','
                << r.mse << ','
                << r.psnr << ','
                << r.loadMs << ','
                << r.prepareMs << ','
                << r.encodeMs << ','
                << r.saveWalshMs << ','
                << r.decodeMs << ','
                << r.saveImageMs << ','
                << r.totalMs << ','
                << r.zeroPercentCh0 << ','
                << r.zeroPercentCh1 << ','
                << r.zeroPercentCh2 << ','
                << r.avgZeroPercent << '\n';
        }

        return true;
    }

    bool saveTextConfigReport(const std::string& path, const BenchmarkConfig& cfg) {
        std::ofstream out(path);
        if (!out) {
            return false;
        }

        out << "Benchmark configuration\n";
        out << "Build config:  " << cfg.buildConfig << "\n";
        out << "Platform:      " << cfg.platform << "\n";
        out << "C++ standard:  " << cfg.cppStandard << "\n";
        out << "OS:            " << cfg.os << "\n";
        out << "CPU:           " << cfg.cpuName << "\n";
        out << "Logical cores: " << cfg.logicalCores << "\n";
        out << "RAM (MB):      " << cfg.ramMB << "\n";
        return true;
    }

    void printResultsTable(const BenchmarkConfig& cfg, const std::vector<TestResult>& results) {
        std::cout << "\n=== Benchmark configuration ===\n";
        std::cout << "Build config: " << cfg.buildConfig << "\n";
        std::cout << "Platform:     " << cfg.platform << "\n";
        std::cout << "C++ standard: " << cfg.cppStandard << "\n";
        std::cout << "OS:           " << cfg.os << "\n";
        std::cout << "CPU:          " << cfg.cpuName << "\n";
        std::cout << "Logical cores:" << cfg.logicalCores << "\n";
        std::cout << "RAM (MB):     " << cfg.ramMB << "\n";

        std::cout << "\n=== Benchmark summary ===\n";
        for (const auto& r : results) {
            std::cout
                << r.inputName
                << " | " << r.scenarioName
                << " | mode=" << r.mode
                << " | q=" << r.quantStep
                << " | walsh=" << r.walshSizeBytes / 1024 << " KB"
                << " | psnr=" << r.psnr
                << " | mse=" << r.mse
                << " | enc=" << r.encodeMs << " ms"
                << " | dec=" << r.decodeMs << " ms"
                << '\n';
        }
    }

    double calcZeroPercent(const ChannelStats& s) {
        if (s.totalCoefficients <= 0) {
            return 0.0;
        }

        return 100.0 * static_cast<double>(s.zeroCoefficients) / static_cast<double>(s.totalCoefficients);
    }

    bool runOneScenario(const std::string& inputPath, const std::string& outDir, const TestScenario& scenario, TestResult& result) {
        const auto totalStart = std::chrono::steady_clock::now();

        const auto loadStart = std::chrono::steady_clock::now();
        Image srcImage{};
        if (!loadImage(inputPath, srcImage)) {
            return false;
        }
        const auto loadEnd = std::chrono::steady_clock::now();

        CodecParams params{};
        params.blockSize = scenario.blockSize;
        params.quantStep = scenario.quantStep;
        params.isGrayscale = scenario.isGrayscale;
        params.useYCbCr = scenario.useYCbCr;
        params.printStats = false;

        const auto prepStart = std::chrono::steady_clock::now();

        Image workingImage{};
        Image metricReference{};

        if (scenario.isGrayscale) {
            workingImage = convertToGrayscale(srcImage);
            metricReference = workingImage;
            params.useYCbCr = false;
        } else {
            if (srcImage.channels < 3) {
                return false;
            }

            Image rgbImage = convertToRGB(srcImage);

            if (scenario.useYCbCr) {
                workingImage = convertRGBToYCbCr(rgbImage);
            } else {
                workingImage = rgbImage;
            }

            metricReference = rgbImage;
        }

        const auto prepEnd = std::chrono::steady_clock::now();

        const auto encodeStart = std::chrono::steady_clock::now();
        EncodeStats stats;
        EncodedFrame encoded = encodeFrame(workingImage, params, stats);
        const auto encodeEnd = std::chrono::steady_clock::now();

        const std::string baseName = getBaseName(inputPath) + "_" + scenario.name;
        const std::string walshPath = joinPath(outDir, baseName + ".walsh");
        const std::string imagePath = joinPath(outDir, baseName + ".png");

        const auto saveWalshStart = std::chrono::steady_clock::now();
        if (!saveEncodedFrame(walshPath, encoded)) {
            return false;
        }
        const auto saveWalshEnd = std::chrono::steady_clock::now();

        const auto decodeStart = std::chrono::steady_clock::now();
        Image decodedImage = decodeFrame(encoded);
        if (!scenario.isGrayscale && scenario.useYCbCr) {
            decodedImage = convertYCbCrToRGB(decodedImage);
        }
        const auto decodeEnd = std::chrono::steady_clock::now();

        const auto saveImageStart = std::chrono::steady_clock::now();
        if (!saveImage(imagePath, decodedImage)) {
            return false;
        }
        const auto saveImageEnd = std::chrono::steady_clock::now();

        const auto totalEnd = std::chrono::steady_clock::now();

        result.inputName = getBaseName(inputPath);
        result.scenarioName = scenario.name;
        result.mode = buildModeName(scenario.isGrayscale, scenario.useYCbCr);

        result.width = srcImage.width;
        result.height = srcImage.height;
        result.channels = srcImage.channels;

        result.blockSize = scenario.blockSize;
        result.quantStep = scenario.quantStep;

        result.inputFileBytes = getFileSize(inputPath);
        result.originalSizeBytes = scenario.isGrayscale ? workingImage.data.size() : srcImage.data.size();
        result.walshSizeBytes = getFileSize(walshPath);
        result.outputImageBytes = getFileSize(imagePath);

        result.compressionRatio = result.walshSizeBytes > 0 ? static_cast<double>(result.originalSizeBytes) / static_cast<double>(result.walshSizeBytes) : 0.0;

        result.mse = calcMSE(metricReference, decodedImage);
        result.psnr = calcPSNR(metricReference, decodedImage);

        result.loadMs = toMilliseconds(loadEnd - loadStart);
        result.prepareMs = toMilliseconds(prepEnd - prepStart);
        result.encodeMs = toMilliseconds(encodeEnd - encodeStart);
        result.saveWalshMs = toMilliseconds(saveWalshEnd - saveWalshStart);
        result.decodeMs = toMilliseconds(decodeEnd - decodeStart);
        result.saveImageMs = toMilliseconds(saveImageEnd - saveImageStart);
        result.totalMs = toMilliseconds(totalEnd - totalStart);

        if (!stats.channels.empty()) {
            result.zeroPercentCh0 = calcZeroPercent(stats.channels[0]);
        }
        if (stats.channels.size() > 1) {
            result.zeroPercentCh1 = calcZeroPercent(stats.channels[1]);
        }
        if (stats.channels.size() > 2) {
            result.zeroPercentCh2 = calcZeroPercent(stats.channels[2]);
        }

        double sum = 0.0;
        int count = 0;
        for (std::size_t i = 0; i < stats.channels.size(); ++i) {
            sum += calcZeroPercent(stats.channels[i]);
            ++count;
        }
        result.avgZeroPercent = (count > 0) ? (sum / static_cast<double>(count)) : 0.0;

        return true;
    }
}

int runAutomatedTest(const std::string& inputPathOrDir, const std::string& outputDir) {
    const std::string outDir = outputDir.empty() ? "test_results" : outputDir;

    if (!ensureDirectoryExists(outDir)) {
        std::cerr << "Error: failed to create output directory: " << outDir << "\n";
        return 1;
    }

    std::vector<std::string> inputFiles = collectInputFiles(inputPathOrDir);

    if (inputFiles.empty()) {
        std::cerr << "Error: no valid .png/.bmp input files found.\n";
        std::cerr << "You can pass either one file or a folder with .png/.bmp files.\n";
        return 1;
    }

    const std::vector<TestScenario> scenarios = {
        { "gray_q8",     true,  false, 8,  8  },
        { "gray_q16",    true,  false, 8,  16 },
        { "gray_q32",    true,  false, 8,  32 },
        { "gray_b2_q8",  false, false, 2,  8  },
        { "rgb_q8",      false, false, 8,  8  },
        { "rgb_q16",     false, false, 8,  16 },
        { "rgb_q32",     false, false, 8,  32 },
        { "rgb_b2_q8",   false, false, 2,  8  },
        { "ycbcr_q8",    false, true,  8,  8  },
        { "ycbcr_q16",   false, true,  8,  16 },
        { "ycbcr_q32",   false, true,  8,  32 },
        { "ycbcr_b2_q8", false, true,  2,  8  },
    };

    BenchmarkConfig cfg = getBenchmarkConfig();
    std::vector<TestResult> results;

    for (std::size_t f = 0; f < inputFiles.size(); ++f) {
        for (std::size_t s = 0; s < scenarios.size(); ++s) {
            std::cout << "\n--- Running: " << getBaseName(inputFiles[f]) << " / " << scenarios[s].name << " ---\n";

            TestResult result{};
            if (!runOneScenario(inputFiles[f], outDir, scenarios[s], result)) {
                std::cerr << "Error: benchmark scenario failed.\n";
                return 1;
            }

            results.push_back(result);
        }
    }

    const std::string csvPath = joinPath(outDir, "benchmark_report.csv");
    if (!saveCsvReport(csvPath, cfg, results)) {
        std::cerr << "Error: failed to save benchmark_report.csv\n";
        return 1;
    }

    const std::string txtPath = joinPath(outDir, "benchmark_config.txt");
    if (!saveTextConfigReport(txtPath, cfg)) {
        std::cerr << "Error: failed to save benchmark_config.txt\n";
        return 1;
    }

    printResultsTable(cfg, results);

    std::cout << "\nSaved benchmark results to: " << csvPath << "\n";
    std::cout << "Saved benchmark config to:  " << txtPath << "\n";
    std::cout << "Saved output files to:      " << outDir << "\n";

    return 0;
}