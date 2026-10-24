#include "bgmedia/audio/MiniaudioDecoder.hpp"

#include "miniaudio.h"

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <string>
#include <vector>

namespace bgmedia {
namespace {

constexpr std::size_t kMaxDecodedBytes = 1024ull * 1024ull * 1024ull; // 1 GiB
constexpr ma_uint64 kBlockFrames = 8192;

std::string lowerExtension(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext;
}

ma_result openDecoder(const std::filesystem::path& path,
                      const ma_decoder_config& config, ma_decoder* decoder) {
#ifdef _WIN32
    return ma_decoder_init_file_w(path.wstring().c_str(), &config, decoder);
#else
    const std::string utf8 = path.string();
    return ma_decoder_init_file(utf8.c_str(), &config, decoder);
#endif
}

} // namespace

bool MiniaudioDecoder::supports(const std::filesystem::path& path) const {
    const std::string ext = lowerExtension(path);
    return ext == ".wav" || ext == ".mp3" || ext == ".flac";
}

DecodeResult MiniaudioDecoder::decode(const std::filesystem::path& path,
                                      const DecodeProgressCallback& progress) const {
    if (progress) {
        progress(0.0f, "opening audio");
    }
    if (!std::filesystem::exists(path)) {
        return DecodeResult::failure("audio file does not exist: " + path.string());
    }

    ma_decoder_config config = ma_decoder_config_init(ma_format_f32, 0, 0);
    ma_decoder decoder{};
    const ma_result initResult = openDecoder(path, config, &decoder);
    if (initResult != MA_SUCCESS) {
        return DecodeResult::failure(std::string("miniaudio could not decode file: ") +
                                     ma_result_description(initResult));
    }

    const ma_uint32 channels = decoder.outputChannels;
    const ma_uint32 sampleRate = decoder.outputSampleRate;
    if (channels == 0 || sampleRate == 0) {
        ma_decoder_uninit(&decoder);
        return DecodeResult::failure("decoder reported invalid channels or sample rate");
    }

    ma_uint64 lengthFrames = 0;
    const bool haveLength =
        ma_decoder_get_length_in_pcm_frames(&decoder, &lengthFrames) == MA_SUCCESS &&
        lengthFrames > 0;

    std::vector<float> samples;
    if (haveLength) {
        const std::uint64_t estimated =
            static_cast<std::uint64_t>(lengthFrames) * channels;
        const std::uint64_t maxFloats = kMaxDecodedBytes / sizeof(float);
        if (estimated > maxFloats) {
            ma_decoder_uninit(&decoder);
            return DecodeResult::failure("audio file is too large for the in-memory decoder");
        }
        samples.reserve(static_cast<std::size_t>(estimated));
    }

    std::vector<float> block(static_cast<std::size_t>(kBlockFrames) * channels);
    ma_uint64 framesRead = 0;
    while (true) {
        framesRead = 0;
        const ma_result result = ma_decoder_read_pcm_frames(
            &decoder, block.data(), kBlockFrames, &framesRead);
        if (framesRead > 0) {
            const std::size_t oldSize = samples.size();
            if (oldSize + framesRead * channels > kMaxDecodedBytes / sizeof(float)) {
                ma_decoder_uninit(&decoder);
                return DecodeResult::failure("decoded audio exceeds the in-memory limit");
            }
            samples.resize(oldSize + static_cast<std::size_t>(framesRead) * channels);
            std::copy_n(block.data(), static_cast<std::size_t>(framesRead) * channels,
                        samples.data() + oldSize);
            if (progress && haveLength) {
                progress(std::clamp(
                             static_cast<float>(
                                 static_cast<double>(samples.size() / channels) /
                                 static_cast<double>(lengthFrames)),
                             0.0f, 1.0f),
                         "decoding audio");
            }
        }
        if (result == MA_AT_END) {
            break;
        }
        if (result != MA_SUCCESS) {
            ma_decoder_uninit(&decoder);
            return DecodeResult::failure(
                std::string("miniaudio decoding error: ") + ma_result_description(result));
        }
        if (framesRead == 0) {
            break;
        }
    }

    ma_decoder_uninit(&decoder);

    if (samples.empty()) {
        return DecodeResult::failure("decoder produced no audio samples");
    }
    if (progress) {
        progress(1.0f, "audio decoded");
    }

    AudioData audio;
    audio.samples = std::move(samples);
    audio.channels = channels;
    audio.sampleRate = sampleRate;
    return DecodeResult::success(std::move(audio));
}

} // namespace bgmedia
