#pragma once

#include <string>
#include "types.h"

bool loadImage(const std::string& path, Image& img);
bool saveImage(const std::string& path, const Image& img);
bool saveJpegWithQuality(const std::string& path, const Image& img, int quality);

Image convertToGrayscale(const Image& src);
Image convertRGBToYCbCr(const Image& src);
Image convertYCbCrToRGB(const Image& src);
Image convertToRGB(const Image& src);