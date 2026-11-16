#include "bgmedia/audio/MiniaudioMixLoader.hpp"

#include "bgmedia/analysis/Analyzer.hpp"
#include "bgmedia/audio/AudioData.hpp"
#include "bgmedia/audio/MiniaudioDecoder.hpp"
#include "bgmedia/cache/AnalysisCache.hpp"
#include "bgmedia/mix/MixFeatureCache.hpp"
#include "bgmedia/mix/MixFeatureMap.hpp"
#include "bgmedia/mix/PcmTrack.hpp"

#include <algorithm>
#include <cctype>
#include <memory>
#include <optional>
#include <system_error>

namespace bgmedia {
namespace {

void report(const MixLoadProgress& progress, float fraction,
            std::string_view stage) {
    if (progress) {
        progress(std::clamp(fraction, 0.0f, 1.0f), stage);
    }
}

bool isAudioExtension(const std::filesystem::path& path) {
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return ext == ".wav" || ext == ".mp3" || ext == ".flac";
}

} // namespace

bool prepareMixTrack(const std::filesystem::path& path,
                     const MixTrackLoadOptions& options,
                     mix::PreparedMixTrack& out, std::string& error,
                     const MixLoadProgress& progress) {
    out = {};
    if (!isAudioExtension(path)) {
        error = "Unsupported file type. Choose WAV, MP3 or FLAC.";
        return false;
    }
    if (!std::filesystem::exists(path)) {
        error = "File does not exist.";
        return false;
    }

    report(progress, 0.02f, "decoding audio");
    MiniaudioDecoder decoder(options.maxPcmBytes);
    DecodeResult decoded = decoder.decode(
        path, [&](float fraction, std::string_view stage) {
            report(progress, 0.02f + 0.18f * fraction, stage);
        });
    if (!decoded.ok) {
        error = decoded.error;
        return false;
    }

    const auto rate = options.targetSampleRate == 0 ? decoded.audio.sampleRate : options.targetSampleRate;
    const auto channels = std::min(decoded.audio.channels, 2u);
    const long double estimatedFrames = static_cast<long double>(decoded.audio.frameCount()) * rate / decoded.audio.sampleRate;
    const auto maxFrames = options.maxPcmBytes / sizeof(float) / channels;
    if (estimatedFrames + 0.5L >= static_cast<long double>(maxFrames) + 1.0L || maxFrames == 0) {
        error = "mixer PCM exceeds the in-memory limit";
        return false;
    }
    std::string pcmError;
    PcmBuffer pcm = PcmBuffer::fromAudio(decoded.audio, &pcmError);
    if (!pcm.valid()) {
        error = "Could not prepare PCM: " + pcmError;
        return false;
    }

    report(progress, 0.22f, "checking cache");
    std::optional<AnalysisResult> cached;
    if (options.useCache && !options.cacheDirectory.empty()) {
        AnalysisCache cache(options.cacheDirectory);
        CacheLoadResult loaded = cache.load(pcm, options.analysis);
        if (loaded.hit()) {
            cached = std::move(loaded.result);
        }
    }

    auto analysis = std::make_shared<AnalysisResult>();
    if (cached) {
        *analysis = std::move(*cached);
        report(progress, 0.78f, "loaded analysis from cache");
    } else {
        Analyzer analyzer(options.analysis);
        report(progress, 0.25f, "analysing");
        AnalysisReport analysisReport = analyzer.analyze(
            pcm, [&](float fraction, const std::string& stage) {
                report(progress, 0.25f + 0.52f * fraction, stage);
            });
        if (!analysisReport.ok()) {
            error = analysisReport.status == AnalysisStatus::Cancelled
                        ? "analysis cancelled"
                        : analysisReport.message;
            return false;
        }
        *analysis = std::move(analysisReport.result);
        if (options.useCache && !options.cacheDirectory.empty()) {
            report(progress, 0.79f, "saving analysis cache");
            AnalysisCache cache(options.cacheDirectory);
            std::string cacheError;
            (void)cache.store(*analysis, pcm, options.analysis, &cacheError);
        }
    }

    report(progress, 0.82f, "building feature map");
    std::optional<mix::MixFeatureMap> cachedFeatures;
    if (options.useCache && !options.cacheDirectory.empty()) {
        mix::MixFeatureCache featureCache(options.cacheDirectory);
        mix::FeatureCacheLoadResult loadedFeatures =
            featureCache.load(decoded.audio, pcm, *analysis, options.features);
        if (loadedFeatures.hit()) {
            cachedFeatures = std::move(loadedFeatures.features);
        }
    }
    auto features = std::make_shared<mix::MixFeatureMap>();
    if (cachedFeatures) {
        *features = std::move(*cachedFeatures);
    } else {
        mix::MixFeatureExtractor extractor(options.features, nullptr, [&] {
            report(progress, 0.82f, "building feature map");
        });
        *features = extractor.extract(decoded.audio, pcm, *analysis);
        if (options.useCache && !options.cacheDirectory.empty() &&
            features->valid()) {
            mix::MixFeatureCache featureCache(options.cacheDirectory);
            std::string featureCacheError;
            (void)featureCache.store(*features, options.features,
                                     &featureCacheError);
        }
    }

    // Analysis and feature caches are finished: release the full mono buffer
    // before any mixer output allocation.
    pcm = {};
    report(progress, 0.90f, "preparing mixer audio");
    std::string trackError;
    auto track = std::make_shared<mix::PcmTrack>(mix::PcmTrack::fromAudio(
        std::move(decoded.audio), options.targetSampleRate, &trackError));
    if (!track->valid()) {
        error = "Could not prepare mixer audio: " + trackError;
        return false;
    }

    out.audio = std::move(track);
    out.analysis = std::move(analysis);
    out.features = std::move(features);
    report(progress, 1.0f, "ready");
    return true;
}

} // namespace bgmedia
