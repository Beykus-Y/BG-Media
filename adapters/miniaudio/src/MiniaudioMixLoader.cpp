#include "bgmedia/audio/MiniaudioMixLoader.hpp"

#include "bgmedia/analysis/Analyzer.hpp"
#include "bgmedia/audio/AudioData.hpp"
#include "bgmedia/audio/MiniaudioDecoder.hpp"
#include "bgmedia/cache/AnalysisCache.hpp"
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
    MiniaudioDecoder decoder;
    const DecodeResult decoded = decoder.decode(
        path, [&](float fraction, std::string_view stage) {
            report(progress, 0.02f + 0.18f * fraction, stage);
        });
    if (!decoded.ok) {
        error = decoded.error;
        return false;
    }

    std::string pcmError;
    const PcmBuffer pcm = PcmBuffer::fromAudio(decoded.audio, &pcmError);
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
        report(progress, 0.84f, "loaded from cache");
    } else {
        Analyzer analyzer(options.analysis);
        report(progress, 0.25f, "analysing");
        AnalysisReport analysisReport = analyzer.analyze(
            pcm, [&](float fraction, const std::string& stage) {
                report(progress, 0.25f + 0.58f * fraction, stage);
            });
        if (!analysisReport.ok()) {
            error = analysisReport.status == AnalysisStatus::Cancelled
                        ? "analysis cancelled"
                        : analysisReport.message;
            return false;
        }
        *analysis = std::move(analysisReport.result);
        if (options.useCache && !options.cacheDirectory.empty()) {
            report(progress, 0.86f, "saving cache");
            AnalysisCache cache(options.cacheDirectory);
            std::string cacheError;
            (void)cache.store(*analysis, pcm, options.analysis, &cacheError);
        }
    }

    report(progress, 0.90f, "preparing mixer audio");
    std::string trackError;
    auto track = std::make_shared<mix::PcmTrack>(mix::PcmTrack::fromAudio(
        decoded.audio, options.targetSampleRate, &trackError));
    if (!track->valid()) {
        error = "Could not prepare mixer audio: " + trackError;
        return false;
    }

    out.audio = std::move(track);
    out.analysis = std::move(analysis);
    report(progress, 1.0f, "ready");
    return true;
}

} // namespace bgmedia
