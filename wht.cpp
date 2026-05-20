#include "wht.h"

#include <stdexcept>
#include <string>
#include <vector>

namespace {
    bool isPowerOfTwo(int value) {
        return value > 0 && (value & (value - 1)) == 0;
    }

    void check1DSize(std::size_t size) {
        if (size == 0 || (size & (size - 1)) != 0) {
            throw std::runtime_error("WHT: 1D size must be a power of two");
        }
    }

    void hadamardTransform(std::vector<int>& data) {
        check1DSize(data.size());

        const int n = static_cast<int>(data.size());

        for (int len = 1; len < n; len <<= 1) {
            for (int i = 0; i < n; i += (len << 1)) {
                for (int j = 0; j < len; ++j) {
                    const int a = i + j;
                    const int b = a + len;

                    const int u = data[static_cast<std::size_t>(a)];
                    const int v = data[static_cast<std::size_t>(b)];

                    data[static_cast<std::size_t>(a)] = u + v;
                    data[static_cast<std::size_t>(b)] = u - v;
                }
            }
        }
    }

    void check2DInput(const std::vector<int>& block, int blockSize, const char* functionName) {
        if (!isPowerOfTwo(blockSize)) {
            throw std::runtime_error(std::string(functionName) + ": blockSize must be a power of two");
        }

        if (static_cast<int>(block.size()) != blockSize * blockSize) {
            throw std::runtime_error(std::string(functionName) + ": invalid block size");
        }
    }

    inline void hadamard8(int* base, int stride) {
        const int x0 = base[0 * stride];
        const int x1 = base[1 * stride];
        const int x2 = base[2 * stride];
        const int x3 = base[3 * stride];
        const int x4 = base[4 * stride];
        const int x5 = base[5 * stride];
        const int x6 = base[6 * stride];
        const int x7 = base[7 * stride];

        const int a0 = x0 + x1;
        const int a1 = x0 - x1;
        const int a2 = x2 + x3;
        const int a3 = x2 - x3;
        const int a4 = x4 + x5;
        const int a5 = x4 - x5;
        const int a6 = x6 + x7;
        const int a7 = x6 - x7;

        const int b0 = a0 + a2;
        const int b1 = a1 + a3;
        const int b2 = a0 - a2;
        const int b3 = a1 - a3;
        const int b4 = a4 + a6;
        const int b5 = a5 + a7;
        const int b6 = a4 - a6;
        const int b7 = a5 - a7;

        base[0 * stride] = b0 + b4;
        base[1 * stride] = b1 + b5;
        base[2 * stride] = b2 + b6;
        base[3 * stride] = b3 + b7;
        base[4 * stride] = b0 - b4;
        base[5 * stride] = b1 - b5;
        base[6 * stride] = b2 - b6;
        base[7 * stride] = b3 - b7;
    }

    void forwardWHT2D8x8(std::vector<int>& block) {
        int* data = block.data();

        hadamard8(data + 0 * 8, 1);
        hadamard8(data + 1 * 8, 1);
        hadamard8(data + 2 * 8, 1);
        hadamard8(data + 3 * 8, 1);
        hadamard8(data + 4 * 8, 1);
        hadamard8(data + 5 * 8, 1);
        hadamard8(data + 6 * 8, 1);
        hadamard8(data + 7 * 8, 1);

        hadamard8(data + 0, 8);
        hadamard8(data + 1, 8);
        hadamard8(data + 2, 8);
        hadamard8(data + 3, 8);
        hadamard8(data + 4, 8);
        hadamard8(data + 5, 8);
        hadamard8(data + 6, 8);
        hadamard8(data + 7, 8);
    }

    void inverseWHT2D8x8(std::vector<int>& block) {
        int* data = block.data();

        // Сохраняем старый порядок целочисленного деления:
        // после строк делим на 8, потом после столбцов ещё раз на 8.
        hadamard8(data + 0 * 8, 1);
        hadamard8(data + 1 * 8, 1);
        hadamard8(data + 2 * 8, 1);
        hadamard8(data + 3 * 8, 1);
        hadamard8(data + 4 * 8, 1);
        hadamard8(data + 5 * 8, 1);
        hadamard8(data + 6 * 8, 1);
        hadamard8(data + 7 * 8, 1);

        for (int i = 0; i < 64; ++i) {
            data[i] /= 8;
        }

        hadamard8(data + 0, 8);
        hadamard8(data + 1, 8);
        hadamard8(data + 2, 8);
        hadamard8(data + 3, 8);
        hadamard8(data + 4, 8);
        hadamard8(data + 5, 8);
        hadamard8(data + 6, 8);
        hadamard8(data + 7, 8);

        for (int i = 0; i < 64; ++i) {
            data[i] /= 8;
        }
    }

    void hadamardRowsInPlace(std::vector<int>& block, int blockSize) {
        for (int y = 0; y < blockSize; ++y) {
            const int rowOffset = y * blockSize;

            for (int len = 1; len < blockSize; len <<= 1) {
                for (int i = 0; i < blockSize; i += (len << 1)) {
                    for (int j = 0; j < len; ++j) {
                        const int a = rowOffset + i + j;
                        const int b = a + len;

                        const int u = block[static_cast<std::size_t>(a)];
                        const int v = block[static_cast<std::size_t>(b)];

                        block[static_cast<std::size_t>(a)] = u + v;
                        block[static_cast<std::size_t>(b)] = u - v;
                    }
                }
            }
        }
    }

    void hadamardColumnsInPlace(std::vector<int>& block, int blockSize) {
        for (int x = 0; x < blockSize; ++x) {
            for (int len = 1; len < blockSize; len <<= 1) {
                for (int i = 0; i < blockSize; i += (len << 1)) {
                    for (int j = 0; j < len; ++j) {
                        const int y1 = i + j;
                        const int y2 = y1 + len;

                        const int a = y1 * blockSize + x;
                        const int b = y2 * blockSize + x;

                        const int u = block[static_cast<std::size_t>(a)];
                        const int v = block[static_cast<std::size_t>(b)];

                        block[static_cast<std::size_t>(a)] = u + v;
                        block[static_cast<std::size_t>(b)] = u - v;
                    }
                }
            }
        }
    }

    void divideInPlace(std::vector<int>& block, int divisor) {
        for (int& value : block) {
            value /= divisor;
        }
    }
}

void forwardWHT1D(std::vector<int>& data) {
    hadamardTransform(data);
}

void inverseWHT1D(std::vector<int>& data) {
    hadamardTransform(data);

    const int n = static_cast<int>(data.size());
    for (int& value : data) {
        value /= n;
    }
}

void forwardWHT2D(std::vector<int>& block, int blockSize) {
    check2DInput(block, blockSize, "forwardWHT2D");

    if (blockSize == 8) {
        forwardWHT2D8x8(block);
        return;
    }

    hadamardRowsInPlace(block, blockSize);
    hadamardColumnsInPlace(block, blockSize);
}

void inverseWHT2D(std::vector<int>& block, int blockSize) {
    check2DInput(block, blockSize, "inverseWHT2D");

    if (blockSize == 8) {
        inverseWHT2D8x8(block);
        return;
    }

    hadamardRowsInPlace(block, blockSize);
    divideInPlace(block, blockSize);

    hadamardColumnsInPlace(block, blockSize);
    divideInPlace(block, blockSize);
}