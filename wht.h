#pragma once

#include <vector>

// 1D Walsh-Hadamard transform.
// size must be a power of two.
void forwardWHT1D(std::vector<int>& data);
void inverseWHT1D(std::vector<int>& data);

// 2D Walsh-Hadamard transform for square block stored row-major.
// block.size() must be blockSize * blockSize.
// blockSize must be a power of two.
void forwardWHT2D(std::vector<int>& block, int blockSize);
void inverseWHT2D(std::vector<int>& block, int blockSize);