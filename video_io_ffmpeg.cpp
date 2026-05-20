#include "video_io_ffmpeg.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/dict.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>
}

namespace {
    std::string ffmpegErrorToString(int errorCode) {
        char buffer[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(errorCode, buffer, sizeof(buffer));
        return std::string(buffer);
    }

    void setError(std::string& errorMessage, const std::string& text) {
        errorMessage = text;
    }

    double rationalToDouble(AVRational value) {
        if (value.num == 0 || value.den == 0) {
            return 0.0;
        }

        return static_cast<double>(value.num) / static_cast<double>(value.den);
    }

    Image makeRGBImage(int width, int height) {
        Image img;
        img.width = width;
        img.height = height;
        img.channels = 3;
        img.data.resize(
            static_cast<std::size_t>(width) *
            static_cast<std::size_t>(height) * 3);
        return img;
    }

    AVFrame* allocateFrame() {
        AVFrame* frame = av_frame_alloc();
        return frame;
    }

    struct AVFrameDeleter {
        void operator()(AVFrame* frame) const {
            if (frame) {
                av_frame_free(&frame);
            }
        }
    };

    struct AVPacketDeleter {
        void operator()(AVPacket* packet) const {
            if (packet) {
                av_packet_free(&packet);
            }
        }
    };

    struct SwsContextDeleter {
        void operator()(SwsContext* ctx) const {
            if (ctx) {
                sws_freeContext(ctx);
            }
        }
    };

    using FramePtr = std::unique_ptr<AVFrame, AVFrameDeleter>;
    using PacketPtr = std::unique_ptr<AVPacket, AVPacketDeleter>;
    using SwsPtr = std::unique_ptr<SwsContext, SwsContextDeleter>;

    // Select frames by timestamps instead of raw frame index. This keeps 1 FPS
    // extraction stable even when the source FPS is fractional or variable.
    bool shouldPickFrame(
        double timestampSeconds,
        double& nextOutputTimestamp,
        double stepSeconds)
    {
        constexpr double EPS = 1e-6;

        if (timestampSeconds + EPS < nextOutputTimestamp) {
            return false;
        }

        while (nextOutputTimestamp <= timestampSeconds + EPS) {
            nextOutputTimestamp += stepSeconds;
        }

        return true;
    }

    // Convert one decoded FFmpeg frame to the project RGB Image type.
    bool copyDecodedFrameToRGB(
        AVFrame* decodedFrame,
        SwsContext* swsContext,
        Image& outImage)
    {
        outImage = makeRGBImage(decodedFrame->width, decodedFrame->height);

        uint8_t* dstData[4] = { outImage.data.data(), nullptr, nullptr, nullptr };
        int dstLinesize[4] = { decodedFrame->width * 3, 0, 0, 0 };

        const int convertedRows = sws_scale(
            swsContext,
            decodedFrame->data,
            decodedFrame->linesize,
            0,
            decodedFrame->height,
            dstData,
            dstLinesize);

        return convertedRows == decodedFrame->height;
    }

    bool isEven(int value) {
        return (value % 2) == 0;
    }

    int makeEven(int value) {
        return isEven(value) ? value : value - 1;
    }
}

// Decode video through FFmpeg and pass selected frames to the Walsh pipeline.
bool readVideoFramesFFmpeg(
    const std::string& path,
    int targetFps,
    const VideoFrameCallback& onFrame,
    VideoInputInfo& info,
    std::string& errorMessage)
{
    if (targetFps <= 0) {
        setError(errorMessage, "target FPS must be positive");
        return false;
    }

    AVFormatContext* rawFormatContext = nullptr;
    int result = avformat_open_input(&rawFormatContext, path.c_str(), nullptr, nullptr);
    if (result < 0) {
        setError(errorMessage, "avformat_open_input failed: " + ffmpegErrorToString(result));
        return false;
    }

    std::unique_ptr<AVFormatContext, void(*)(AVFormatContext*)> formatContext(
        rawFormatContext,
        [](AVFormatContext* ctx) {
            if (ctx) {
                avformat_close_input(&ctx);
            }
        });

    result = avformat_find_stream_info(formatContext.get(), nullptr);
    if (result < 0) {
        setError(errorMessage, "avformat_find_stream_info failed: " + ffmpegErrorToString(result));
        return false;
    }

    const int streamIndex = av_find_best_stream(
        formatContext.get(),
        AVMEDIA_TYPE_VIDEO,
        -1,
        -1,
        nullptr,
        0);

    if (streamIndex < 0) {
        setError(errorMessage, "no video stream found");
        return false;
    }

    AVStream* stream = formatContext->streams[streamIndex];
    const AVCodecParameters* codecParams = stream->codecpar;
    const AVCodec* decoder = avcodec_find_decoder(codecParams->codec_id);

    if (!decoder) {
        setError(errorMessage, "unsupported video codec");
        return false;
    }

    AVCodecContext* rawCodecContext = avcodec_alloc_context3(decoder);
    if (!rawCodecContext) {
        setError(errorMessage, "avcodec_alloc_context3 failed");
        return false;
    }

    std::unique_ptr<AVCodecContext, void(*)(AVCodecContext*)> codecContext(
        rawCodecContext,
        [](AVCodecContext* ctx) {
            if (ctx) {
                avcodec_free_context(&ctx);
            }
        });

    result = avcodec_parameters_to_context(codecContext.get(), codecParams);
    if (result < 0) {
        setError(errorMessage, "avcodec_parameters_to_context failed: " + ffmpegErrorToString(result));
        return false;
    }

    result = avcodec_open2(codecContext.get(), decoder, nullptr);
    if (result < 0) {
        setError(errorMessage, "avcodec_open2 failed: " + ffmpegErrorToString(result));
        return false;
    }

    SwsPtr swsContext(sws_getContext(
        codecContext->width,
        codecContext->height,
        codecContext->pix_fmt,
        codecContext->width,
        codecContext->height,
        AV_PIX_FMT_RGB24,
        SWS_BILINEAR,
        nullptr,
        nullptr,
        nullptr));

    if (!swsContext) {
        setError(errorMessage, "sws_getContext failed");
        return false;
    }

    PacketPtr packet(av_packet_alloc());
    FramePtr decodedFrame(allocateFrame());

    if (!packet || !decodedFrame) {
        setError(errorMessage, "failed to allocate FFmpeg packet/frame");
        return false;
    }

    double sourceFps = rationalToDouble(stream->avg_frame_rate);
    if (sourceFps <= 0.0) {
        sourceFps = rationalToDouble(stream->r_frame_rate);
    }

    info.width = codecContext->width;
    info.height = codecContext->height;
    info.sourceFps = sourceFps;
    info.selectedFrames = 0;

    const double stepSeconds = 1.0 / static_cast<double>(targetFps);
    double nextOutputTimestamp = 0.0;
    int decodedFrameIndex = 0;

    auto handleDecodedFrame = [&]() -> bool {
        double timestampSeconds = 0.0;

        if (decodedFrame->best_effort_timestamp != AV_NOPTS_VALUE) {
            timestampSeconds =
                static_cast<double>(decodedFrame->best_effort_timestamp) *
                av_q2d(stream->time_base);
        } else if (sourceFps > 0.0) {
            timestampSeconds = static_cast<double>(decodedFrameIndex) / sourceFps;
        } else {
            timestampSeconds = static_cast<double>(decodedFrameIndex);
        }

        ++decodedFrameIndex;

        if (!shouldPickFrame(timestampSeconds, nextOutputTimestamp, stepSeconds)) {
            return true;
        }

        Image rgbImage;
        if (!copyDecodedFrameToRGB(decodedFrame.get(), swsContext.get(), rgbImage)) {
            setError(errorMessage, "sws_scale failed while reading video");
            return false;
        }

        const int selectedIndex = info.selectedFrames;
        ++info.selectedFrames;

        return onFrame(rgbImage, selectedIndex);
    };

    while ((result = av_read_frame(formatContext.get(), packet.get())) >= 0) {
        if (packet->stream_index == streamIndex) {
            result = avcodec_send_packet(codecContext.get(), packet.get());
            av_packet_unref(packet.get());

            if (result < 0) {
                setError(errorMessage, "avcodec_send_packet failed: " + ffmpegErrorToString(result));
                return false;
            }

            while (true) {
                result = avcodec_receive_frame(codecContext.get(), decodedFrame.get());

                if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
                    break;
                }

                if (result < 0) {
                    setError(errorMessage, "avcodec_receive_frame failed: " + ffmpegErrorToString(result));
                    return false;
                }

                if (!handleDecodedFrame()) {
                    if (errorMessage.empty()) {
                        setError(errorMessage, "video frame callback failed");
                    }
                    return false;
                }

                av_frame_unref(decodedFrame.get());
            }
        }
        else {
            av_packet_unref(packet.get());
        }
    }

    result = avcodec_send_packet(codecContext.get(), nullptr);
    if (result >= 0) {
        while (true) {
            result = avcodec_receive_frame(codecContext.get(), decodedFrame.get());

            if (result == AVERROR_EOF || result == AVERROR(EAGAIN)) {
                break;
            }

            if (result < 0) {
                setError(errorMessage, "avcodec_receive_frame while flushing failed: " + ffmpegErrorToString(result));
                return false;
            }

            if (!handleDecodedFrame()) {
                if (errorMessage.empty()) {
                    setError(errorMessage, "video frame callback failed");
                }
                return false;
            }

            av_frame_unref(decodedFrame.get());
        }
    }

    return true;
}

struct FFmpegVideoWriter::Impl {
    AVFormatContext* formatContext = nullptr;
    AVCodecContext* codecContext = nullptr;
    AVStream* stream = nullptr;
    SwsContext* swsContext = nullptr;
    AVFrame* yuvFrame = nullptr;
    AVPacket* packet = nullptr;
};

FFmpegVideoWriter::~FFmpegVideoWriter() {
    release();
}

void FFmpegVideoWriter::release() {
    if (!impl_) {
        return;
    }

    if (impl_->packet) {
        av_packet_free(&impl_->packet);
    }

    if (impl_->yuvFrame) {
        av_frame_free(&impl_->yuvFrame);
    }

    if (impl_->swsContext) {
        sws_freeContext(impl_->swsContext);
        impl_->swsContext = nullptr;
    }

    if (impl_->codecContext) {
        avcodec_free_context(&impl_->codecContext);
    }

    if (impl_->formatContext) {
        if (!(impl_->formatContext->oformat->flags & AVFMT_NOFILE) &&
            impl_->formatContext->pb) {
            avio_closep(&impl_->formatContext->pb);
        }

        avformat_free_context(impl_->formatContext);
    }

    delete impl_;
    impl_ = nullptr;
}

bool FFmpegVideoWriter::open(
    const std::string& path,
    int width,
    int height,
    int fps,
    std::string& errorMessage)
{
    if (width <= 0 || height <= 0 || fps <= 0) {
        setError(errorMessage, "invalid video output parameters");
        return false;
    }

    width = makeEven(width);
    height = makeEven(height);

    if (width <= 0 || height <= 0) {
        setError(errorMessage, "invalid video output size after even-size adjustment");
        return false;
    }

    release();

    impl_ = new Impl();
    width_ = width;
    height_ = height;
    fps_ = fps;
    writtenFrames_ = 0;

    int result = avformat_alloc_output_context2(&impl_->formatContext, nullptr, nullptr, path.c_str());
    if (result < 0 || !impl_->formatContext) {
        setError(errorMessage, "avformat_alloc_output_context2 failed: " + ffmpegErrorToString(result));
        release();
        return false;
    }

    const AVCodec* encoder = avcodec_find_encoder(AV_CODEC_ID_H264);
    if (!encoder) {
        encoder = avcodec_find_encoder(AV_CODEC_ID_MPEG4);
    }

    if (!encoder) {
        setError(errorMessage, "no suitable MP4 encoder found (H.264/MPEG4)");
        release();
        return false;
    }

    impl_->stream = avformat_new_stream(impl_->formatContext, nullptr);
    if (!impl_->stream) {
        setError(errorMessage, "avformat_new_stream failed");
        release();
        return false;
    }

    impl_->codecContext = avcodec_alloc_context3(encoder);
    if (!impl_->codecContext) {
        setError(errorMessage, "avcodec_alloc_context3 failed for encoder");
        release();
        return false;
    }

    impl_->codecContext->codec_id = encoder->id;
    impl_->codecContext->codec_type = AVMEDIA_TYPE_VIDEO;
    impl_->codecContext->width = width_;
    impl_->codecContext->height = height_;
    impl_->codecContext->time_base = AVRational{ 1, fps_ };
    impl_->codecContext->framerate = AVRational{ fps_, 1 };
    impl_->codecContext->pix_fmt = AV_PIX_FMT_YUV420P;
    impl_->codecContext->gop_size = std::max(1, fps_ * 2);
    impl_->codecContext->max_b_frames = 0;
    impl_->codecContext->bit_rate = 2'000'000;

    if (impl_->formatContext->oformat->flags & AVFMT_GLOBALHEADER) {
        impl_->codecContext->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;
    }

    AVDictionary* options = nullptr;
    if (encoder->id == AV_CODEC_ID_H264) {
        av_dict_set(&options, "preset", "medium", 0);
        av_dict_set(&options, "crf", "18", 0);
    }

    result = avcodec_open2(impl_->codecContext, encoder, &options);
    av_dict_free(&options);

    if (result < 0) {
        setError(errorMessage, "avcodec_open2 encoder failed: " + ffmpegErrorToString(result));
        release();
        return false;
    }

    result = avcodec_parameters_from_context(impl_->stream->codecpar, impl_->codecContext);
    if (result < 0) {
        setError(errorMessage, "avcodec_parameters_from_context failed: " + ffmpegErrorToString(result));
        release();
        return false;
    }

    impl_->stream->time_base = impl_->codecContext->time_base;

    if (!(impl_->formatContext->oformat->flags & AVFMT_NOFILE)) {
        result = avio_open(&impl_->formatContext->pb, path.c_str(), AVIO_FLAG_WRITE);
        if (result < 0) {
            setError(errorMessage, "avio_open failed: " + ffmpegErrorToString(result));
            release();
            return false;
        }
    }

    result = avformat_write_header(impl_->formatContext, nullptr);
    if (result < 0) {
        setError(errorMessage, "avformat_write_header failed: " + ffmpegErrorToString(result));
        release();
        return false;
    }

    impl_->swsContext = sws_getContext(
        width_,
        height_,
        AV_PIX_FMT_RGB24,
        width_,
        height_,
        AV_PIX_FMT_YUV420P,
        SWS_BILINEAR,
        nullptr,
        nullptr,
        nullptr);

    if (!impl_->swsContext) {
        setError(errorMessage, "sws_getContext failed for writer");
        release();
        return false;
    }

    impl_->yuvFrame = av_frame_alloc();
    if (!impl_->yuvFrame) {
        setError(errorMessage, "av_frame_alloc failed for writer");
        release();
        return false;
    }

    impl_->yuvFrame->format = impl_->codecContext->pix_fmt;
    impl_->yuvFrame->width = width_;
    impl_->yuvFrame->height = height_;

    result = av_frame_get_buffer(impl_->yuvFrame, 32);
    if (result < 0) {
        setError(errorMessage, "av_frame_get_buffer failed: " + ffmpegErrorToString(result));
        release();
        return false;
    }

    impl_->packet = av_packet_alloc();
    if (!impl_->packet) {
        setError(errorMessage, "av_packet_alloc failed for writer");
        release();
        return false;
    }

    return true;
}

// Encode one RGB frame to the output video stream.
bool FFmpegVideoWriter::writeFrame(const Image& rgbImage, std::string& errorMessage) {
    if (!impl_ || !impl_->codecContext || !impl_->swsContext || !impl_->yuvFrame) {
        setError(errorMessage, "writer is not opened");
        return false;
    }

    // Width/height may be one pixel smaller than the decoded Walsh frame because
    // YUV420P/H.264 require even dimensions. In that case FFmpeg crops the
    // right/bottom edge while converting RGB to YUV.
    if (rgbImage.width < width_ || rgbImage.height < height_ || rgbImage.channels != 3) {
        setError(errorMessage, "input frame must be RGB and at least writer size");
        return false;
    }

    int result = av_frame_make_writable(impl_->yuvFrame);
    if (result < 0) {
        setError(errorMessage, "av_frame_make_writable failed: " + ffmpegErrorToString(result));
        return false;
    }

    const uint8_t* srcData[4] = { rgbImage.data.data(), nullptr, nullptr, nullptr };
    int srcLinesize[4] = { rgbImage.width * 3, 0, 0, 0 };

    const int convertedRows = sws_scale(
        impl_->swsContext,
        srcData,
        srcLinesize,
        0,
        height_,
        impl_->yuvFrame->data,
        impl_->yuvFrame->linesize);

    if (convertedRows != height_) {
        setError(errorMessage, "sws_scale failed while writing video");
        return false;
    }

    impl_->yuvFrame->pts = writtenFrames_;

    result = avcodec_send_frame(impl_->codecContext, impl_->yuvFrame);
    if (result < 0) {
        setError(errorMessage, "avcodec_send_frame failed: " + ffmpegErrorToString(result));
        return false;
    }

    while (true) {
        result = avcodec_receive_packet(impl_->codecContext, impl_->packet);

        if (result == AVERROR(EAGAIN) || result == AVERROR_EOF) {
            break;
        }

        if (result < 0) {
            setError(errorMessage, "avcodec_receive_packet failed: " + ffmpegErrorToString(result));
            return false;
        }

        av_packet_rescale_ts(
            impl_->packet,
            impl_->codecContext->time_base,
            impl_->stream->time_base);

        impl_->packet->stream_index = impl_->stream->index;

        result = av_interleaved_write_frame(impl_->formatContext, impl_->packet);
        av_packet_unref(impl_->packet);

        if (result < 0) {
            setError(errorMessage, "av_interleaved_write_frame failed: " + ffmpegErrorToString(result));
            return false;
        }
    }

    ++writtenFrames_;
    return true;
}

bool FFmpegVideoWriter::close(std::string& errorMessage) {
    if (!impl_) {
        return true;
    }

    bool ok = true;
    int result = avcodec_send_frame(impl_->codecContext, nullptr);

    if (result < 0) {
        setError(errorMessage, "avcodec_send_frame flush failed: " + ffmpegErrorToString(result));
        ok = false;
    }

    while (ok) {
        result = avcodec_receive_packet(impl_->codecContext, impl_->packet);

        if (result == AVERROR_EOF || result == AVERROR(EAGAIN)) {
            break;
        }

        if (result < 0) {
            setError(errorMessage, "avcodec_receive_packet while flushing failed: " + ffmpegErrorToString(result));
            ok = false;
            break;
        }

        av_packet_rescale_ts(
            impl_->packet,
            impl_->codecContext->time_base,
            impl_->stream->time_base);

        impl_->packet->stream_index = impl_->stream->index;

        result = av_interleaved_write_frame(impl_->formatContext, impl_->packet);
        av_packet_unref(impl_->packet);

        if (result < 0) {
            setError(errorMessage, "av_interleaved_write_frame while flushing failed: " + ffmpegErrorToString(result));
            ok = false;
            break;
        }
    }

    if (ok) {
        result = av_write_trailer(impl_->formatContext);
        if (result < 0) {
            setError(errorMessage, "av_write_trailer failed: " + ffmpegErrorToString(result));
            ok = false;
        }
    }

    release();
    return ok;
}
