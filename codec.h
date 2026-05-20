#pragma once

#include "types.h"

EncodedFrame encodeFrame(const Image& img, const CodecParams& params);
EncodedFrame encodeFrame(const Image& img, const CodecParams& params, EncodeStats& stats);
Image decodeFrame(const EncodedFrame& frame);