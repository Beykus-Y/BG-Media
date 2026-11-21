#include "bgmedia/video/FFmpeg.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/error.h>
#include <libswresample/swresample.h>
#include <libswscale/swscale.h>
}

#include <algorithm>
#include <chrono>
#include <cmath>
#include <deque>
#include <limits>
#include <stdexcept>

namespace bgmedia::video {
namespace {

using Clock = std::chrono::steady_clock;
constexpr int maxPixels = 3840 * 2160;

void check(int result, const char* operation) {
    if (result >= 0) return;
    char message[AV_ERROR_MAX_STRING_SIZE]{};
    av_strerror(result, message, sizeof(message));
    throw std::runtime_error(std::string(operation) + ": " + message);
}

template<class T, void (*Free)(T**)>
struct FreePointer {
    void operator()(T* ptr) const { Free(&ptr); }
};

struct FreeIO {
    void operator()(AVIOContext* context) const {
        av_freep(&context->buffer);
        avio_context_free(&context);
    }
};

struct Track {
    std::unique_ptr<AVCodecContext, FreePointer<AVCodecContext, avcodec_free_context>> codec;
    AVStream* stream = nullptr;
    bool drained = false;
    double nextPts = 0;
};

class FFmpegDecoder final : public Decoder {
public:
    FFmpegDecoder(const Media& media, std::stop_token token);
    VideoInfo info() const override { return info_; }
    Decoded step() override;
    void seek(double seconds) override;
    std::size_t bufferedBytes() const noexcept override { return bufferedBytes_ + recycledVideo_.capacity(); }
    void recycle(VideoFrame&& frame) override {
        // Keep at most one allocation from this decoder for the next conversion.
        if (frame.rgba.capacity() <= static_cast<std::size_t>(maxPixels) * 4)
            recycledVideo_ = std::move(frame.rgba);
    }
    void prime(std::size_t budget);
    void finishOpening() { token_ = {}; deadline_ = Clock::time_point::max(); }

private:
    static int interrupt(void* opaque) {
        auto& self = *static_cast<FFmpegDecoder*>(opaque);
        return self.token_.stop_requested() || Clock::now() >= self.deadline_;
    }
    static int readAt(void*, std::uint8_t*, int) noexcept;
    static std::int64_t seekBytes(void*, std::int64_t, int) noexcept;
    Track openTrack(AVMediaType type, bool required);
    Decoded read();
    Decoded convert(Track&);
    AudioFrame convertAudio(double pts);
    VideoFrame convertVideo(double pts);
    static std::size_t bytes(const Decoded&);

    std::stop_token token_;
    Clock::time_point deadline_;
    std::shared_ptr<ByteSource> source_;
    std::shared_ptr<void> sourcePin_;
    std::uint64_t offset_ = 0;
    std::unique_ptr<AVIOContext, FreeIO> io_;
    std::unique_ptr<AVFormatContext, FreePointer<AVFormatContext, avformat_close_input>> format_;
    Track video_, audio_;
    std::unique_ptr<AVPacket, FreePointer<AVPacket, av_packet_free>> packet_{av_packet_alloc()};
    std::unique_ptr<AVFrame, FreePointer<AVFrame, av_frame_free>> frame_{av_frame_alloc()};
    std::unique_ptr<SwsContext, decltype(&sws_freeContext)> scaler_{nullptr, sws_freeContext};
    std::unique_ptr<SwrContext, FreePointer<SwrContext, swr_free>> resampler_;
    Track* receiving_ = nullptr;
    bool inputEnded_ = false;
    VideoInfo info_;
    double origin_ = 0;
    std::deque<Decoded> buffered_;
    std::size_t bufferedBytes_ = 0;
    std::vector<std::uint8_t> recycledVideo_;
    std::size_t conversionBudget_ = std::numeric_limits<std::size_t>::max();
};

int FFmpegDecoder::readAt(void* opaque, std::uint8_t* buffer, int count) noexcept {
    auto& self = *static_cast<FFmpegDecoder*>(opaque);
    if (interrupt(&self)) return AVERROR_EXIT;
    if (count <= 0) return AVERROR(EINVAL);
    const auto size = std::min<std::uint64_t>({static_cast<std::uint64_t>(count), self.source_->size() - self.offset_, 1024 * 1024});
    if (!size) return AVERROR_EOF;
    try {
        self.source_->readAt(self.offset_, {buffer, static_cast<std::size_t>(size)},
            {self.token_, std::min(self.deadline_, Clock::now() + std::chrono::seconds(2))});
    } catch (const ReadError& error) {
        if (error.status() == BG_VIDEO_RANGE_CANCELLED) return AVERROR_EXIT;
        if (error.status() == BG_VIDEO_RANGE_TIMEOUT) return AVERROR(ETIMEDOUT);
        return AVERROR(EIO);
    } catch (...) { return AVERROR(EIO); }
    self.offset_ += size;
    return static_cast<int>(size);
}

std::int64_t FFmpegDecoder::seekBytes(void* opaque, std::int64_t offset, int whence) noexcept {
    auto& self = *static_cast<FFmpegDecoder*>(opaque);
    const auto size = static_cast<std::int64_t>(self.source_->size());
    if (whence == AVSEEK_SIZE) return size;
    if (interrupt(&self)) return AVERROR_EXIT;
    std::int64_t base = 0;
    switch (whence & ~AVSEEK_FORCE) {
    case SEEK_SET: break;
    case SEEK_CUR: base = static_cast<std::int64_t>(self.offset_); break;
    case SEEK_END: base = size; break;
    default: return AVERROR(EINVAL);
    }
    if (offset < -base || offset > size - base) return AVERROR(EINVAL);
    self.offset_ = static_cast<std::uint64_t>(base + offset);
    return base + offset;
}

FFmpegDecoder::FFmpegDecoder(const Media& media, std::stop_token token)
    : token_(token), deadline_(Clock::now() + std::chrono::seconds(2)), source_(media.bytes) {
    std::error_code ec;
    if (interrupt(this)) throw std::runtime_error("opening MP4 interrupted");
    if (!packet_ || !frame_) throw std::bad_alloc();
    std::u8string path;
    if (source_) {
        sourcePin_ = source_->pin();
        if (source_->size() == 0 || source_->size() > std::numeric_limits<std::int64_t>::max())
            throw std::runtime_error("invalid MP4 source length");
        constexpr int bufferBytes = 64 * 1024;
        auto* buffer = static_cast<std::uint8_t*>(av_malloc(bufferBytes));
        if (!buffer) throw std::bad_alloc();
        io_.reset(avio_alloc_context(buffer, bufferBytes, 0, this, readAt, nullptr, seekBytes));
        if (!io_) {
            av_free(buffer);
            throw std::bad_alloc();
        }
    } else {
        if (!std::filesystem::is_regular_file(media.localPath, ec))
            throw std::runtime_error("expected a regular local MP4 file");
        path = std::filesystem::absolute(media.localPath, ec).u8string();
        if (ec) throw std::runtime_error("cannot resolve local media path");
    }
    auto* context = avformat_alloc_context();
    if (!context) throw std::bad_alloc();
    context->interrupt_callback = {interrupt, this};
    context->probesize = 2 * 1024 * 1024;
    context->max_analyze_duration = 2 * AV_TIME_BASE;
    context->max_streams = 32;
    if (io_) {
        context->pb = io_.get();
        context->flags |= AVFMT_FLAG_CUSTOM_IO;
    }
    AVDictionary* options = nullptr;
    av_dict_set(&options, "protocol_whitelist", "file", 0);
    av_dict_set(&options, "enable_drefs", "0", 0);
    const int result = avformat_open_input(&context, io_ ? nullptr : reinterpret_cast<const char*>(path.c_str()),
                                           av_find_input_format("mov"), &options);
    av_dict_free(&options);
    format_.reset(context);
    check(result, "open MP4");
    check(avformat_find_stream_info(format_.get(), nullptr), "read stream info");
    if (interrupt(this)) throw std::runtime_error("opening MP4 interrupted");
    video_ = openTrack(AVMEDIA_TYPE_VIDEO, true);
    audio_ = openTrack(AVMEDIA_TYPE_AUDIO, false);
    if (format_->start_time != AV_NOPTS_VALUE)
        origin_ = static_cast<double>(format_->start_time) / AV_TIME_BASE;
    if (std::abs(origin_) > 1e9) throw std::runtime_error("unsupported container timeline");
    info_.width = video_.codec->width;
    info_.height = video_.codec->height;
    info_.fps = av_q2d(av_guess_frame_rate(format_.get(), video_.stream, nullptr));
    if (!(info_.fps > 0)) info_.fps = 0;
    if (format_->duration != AV_NOPTS_VALUE)
        info_.duration = static_cast<double>(format_->duration) / AV_TIME_BASE;
    if (audio_.codec) {
        info_.sourceSampleRate = audio_.codec->sample_rate;
        info_.sourceChannels = audio_.codec->ch_layout.nb_channels;
    }
}

Track FFmpegDecoder::openTrack(AVMediaType type, bool required) {
    const AVCodec* decoder = nullptr;
    const int index = av_find_best_stream(format_.get(), type, -1, -1, &decoder, 0);
    if (!required && index == AVERROR_STREAM_NOT_FOUND) return {};
    check(index, "find media stream");
    Track track;
    track.stream = format_->streams[index];
    track.codec.reset(avcodec_alloc_context3(decoder));
    if (!track.codec) throw std::bad_alloc();
    check(avcodec_parameters_to_context(track.codec.get(), track.stream->codecpar), "copy codec parameters");
    track.codec->thread_count = 1;
    track.codec->max_pixels = maxPixels + 3840 * 32; // allow codec alignment
    track.codec->pkt_timebase = track.stream->time_base;
    check(avcodec_open2(track.codec.get(), decoder, nullptr), "open codec");
    return track;
}

VideoFrame FFmpegDecoder::convertVideo(double pts) {
    const int width = frame_->width, height = frame_->height;
    if (width <= 0 || height <= 0 || width > 3840 || height > 3840 ||
        static_cast<std::int64_t>(width) * height > maxPixels)
        throw std::runtime_error("unsupported video dimensions");
    scaler_.reset(sws_getCachedContext(scaler_.release(), width, height,
        static_cast<AVPixelFormat>(frame_->format), width, height, AV_PIX_FMT_RGBA,
        SWS_BILINEAR, nullptr, nullptr, nullptr));
    if (!scaler_) throw std::runtime_error("cannot convert video pixel format");
    const auto size = static_cast<std::size_t>(width) * height * 4;
    if (size > conversionBudget_) throw std::runtime_error("video frame exceeds preparation budget");
    VideoFrame out{pts, width, height, std::move(recycledVideo_)};
    if (out.rgba.capacity() < size) out.rgba = std::vector<std::uint8_t>(size);
    else out.rgba.resize(size);
    std::uint8_t* planes[] = {out.rgba.data(), nullptr, nullptr, nullptr};
    int strides[] = {width * 4, 0, 0, 0};
    check(sws_scale(scaler_.get(), frame_->data, frame_->linesize, 0, height, planes, strides),
          "convert video frame");
    return out;
}

AudioFrame FFmpegDecoder::convertAudio(double pts) {
    if (!resampler_) {
        SwrContext* context = nullptr;
        AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;
        const int result = swr_alloc_set_opts2(&context, &stereo, AV_SAMPLE_FMT_FLT,
            AudioFrame::sampleRate, &frame_->ch_layout, static_cast<AVSampleFormat>(frame_->format),
            frame_->sample_rate, 0, nullptr);
        resampler_.reset(context);
        check(result, "configure audio converter");
        check(swr_init(resampler_.get()), "initialize audio converter");
    }
    const int capacity = swr_get_out_samples(resampler_.get(), frame_->nb_samples);
    check(capacity, "size converted audio");
    if (capacity > AudioFrame::sampleRate) throw std::runtime_error("oversized audio frame");
    const double delay = static_cast<double>(swr_get_delay(resampler_.get(), AudioFrame::sampleRate))
                         / AudioFrame::sampleRate;
    const auto samples = static_cast<std::size_t>(capacity) * AudioFrame::channels;
    if (samples * sizeof(float) > conversionBudget_)
        throw std::runtime_error("audio frame exceeds preparation budget");
    AudioFrame out{pts - delay, {}};
    out.samples.resize(samples);
    auto* data = reinterpret_cast<std::uint8_t*>(out.samples.data());
    const int size = swr_convert(resampler_.get(), &data, capacity,
        const_cast<const std::uint8_t**>(frame_->extended_data), frame_->nb_samples);
    check(size, "convert audio frame");
    out.samples.resize(static_cast<std::size_t>(size) * AudioFrame::channels);
    return out;
}

Decoded FFmpegDecoder::convert(Track& track) {
    const double timeBase = av_q2d(track.stream->time_base);
    const double pts = frame_->best_effort_timestamp == AV_NOPTS_VALUE ? track.nextPts
        : static_cast<double>(frame_->best_effort_timestamp) * timeBase - origin_;
    if (&track == &video_) {
        track.nextPts = pts + (info_.fps > 0 ? 1.0 / info_.fps : 0);
        return convertVideo(pts);
    }
    track.nextPts = pts + static_cast<double>(frame_->nb_samples) / frame_->sample_rate;
    return convertAudio(pts);
}

Decoded FFmpegDecoder::read() {
    if (interrupt(this)) throw std::runtime_error("video operation interrupted");
    if (receiving_) {
        const int result = avcodec_receive_frame(receiving_->codec.get(), frame_.get());
        if (result >= 0) {
            auto out = convert(*receiving_);
            av_frame_unref(frame_.get());
            return out;
        }
        if (result != AVERROR(EAGAIN) && result != AVERROR_EOF) check(result, "decode frame");
        if (result == AVERROR_EOF) receiving_->drained = true;
        receiving_ = nullptr;
    }
    if (inputEnded_) {
        for (auto* track : {&video_, &audio_}) {
            if (track->codec && !track->drained) {
                check(avcodec_send_packet(track->codec.get(), nullptr), "drain codec");
                receiving_ = track;
                return {};
            }
        }
        if (resampler_) {
            AudioFrame out{audio_.nextPts - static_cast<double>(
                swr_get_delay(resampler_.get(), AudioFrame::sampleRate)) / AudioFrame::sampleRate, {}};
            out.samples.resize(1024 * AudioFrame::channels);
            auto* data = reinterpret_cast<std::uint8_t*>(out.samples.data());
            const int size = swr_convert(resampler_.get(), &data, 1024, nullptr, 0);
            check(size, "drain audio converter");
            if (size > 0) {
                out.samples.resize(static_cast<std::size_t>(size) * AudioFrame::channels);
                return out;
            }
            resampler_.reset();
        }
        return EndOfStream{};
    }
    const int result = av_read_frame(format_.get(), packet_.get());
    if (result == AVERROR_EOF) {
        inputEnded_ = true;
        return {};
    }
    check(result, "read media packet");
    for (auto* track : {&video_, &audio_}) {
        if (track->stream && packet_->stream_index == track->stream->index) {
            if ((packet_->flags & AV_PKT_FLAG_CORRUPT) != 0) {
                av_packet_unref(packet_.get());
                throw std::runtime_error("corrupt media packet");
            }
            const int sent = avcodec_send_packet(track->codec.get(), packet_.get());
            av_packet_unref(packet_.get());
            check(sent, "submit media packet");
            receiving_ = track;
            return {};
        }
    }
    av_packet_unref(packet_.get());
    return {};
}

std::size_t FFmpegDecoder::bytes(const Decoded& frame) {
    if (auto* video = std::get_if<VideoFrame>(&frame)) return video->rgba.capacity();
    if (auto* audio = std::get_if<AudioFrame>(&frame)) return audio->samples.capacity() * sizeof(float);
    return 0;
}

void FFmpegDecoder::prime(std::size_t budget) {
    for (;;) {
        conversionBudget_ = budget - bufferedBytes_;
        auto frame = read();
        if (std::holds_alternative<EndOfStream>(frame))
            throw std::runtime_error("MP4 has no decodable video frames");
        if (std::holds_alternative<std::monostate>(frame)) continue;
        const auto size = bytes(frame);
        if (size > budget - bufferedBytes_)
            throw std::runtime_error("first frames exceed preparation budget");
        const bool haveVideo = std::holds_alternative<VideoFrame>(frame);
        bufferedBytes_ += size;
        buffered_.push_back(std::move(frame));
        if (haveVideo) break;
    }
    conversionBudget_ = std::numeric_limits<std::size_t>::max();
}

Decoded FFmpegDecoder::step() {
    if (buffered_.empty()) return read();
    auto& frame = buffered_.front();
    bufferedBytes_ -= bytes(frame);
    if (auto* video = std::get_if<VideoFrame>(&frame)) {
        VideoFrame out = std::move(*video);
        buffered_.pop_front();
        return out;
    }
    AudioFrame out = std::move(std::get<AudioFrame>(frame));
    buffered_.pop_front();
    return out;
}

void FFmpegDecoder::seek(double seconds) {
    if (!std::isfinite(seconds) || seconds < 0 || seconds > 1e9)
        throw std::invalid_argument("invalid seek position");
    deadline_ = Clock::now() + std::chrono::seconds(2);
    const int result = av_seek_frame(format_.get(), -1,
        static_cast<std::int64_t>((origin_ + seconds) * AV_TIME_BASE), AVSEEK_FLAG_BACKWARD);
    finishOpening();
    check(result, "seek MP4");
    for (auto* track : {&video_, &audio_}) {
        if (track->codec) avcodec_flush_buffers(track->codec.get());
        track->drained = false;
        track->nextPts = seconds;
    }
    resampler_.reset();
    buffered_.clear();
    bufferedBytes_ = 0;
    receiving_ = nullptr;
    inputEnded_ = false;
}

} // namespace

std::unique_ptr<Decoder> openFFmpeg(const Media& media, std::stop_token token) {
    auto decoder = std::make_unique<FFmpegDecoder>(media, token);
    decoder->finishOpening();
    return decoder;
}

Prepared FFmpegProvider::prepare(const Media& media, std::size_t budget, std::stop_token token) {
    auto decoder = std::make_unique<FFmpegDecoder>(media, token);
    decoder->prime(budget);
    decoder->finishOpening();
    return {media.id, {}, std::move(decoder)};
}

}
