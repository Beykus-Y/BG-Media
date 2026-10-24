#include "framework.hpp"

#include "bgmedia/analysis/Analyzer.hpp"
#include "bgmedia/cache/AnalysisCache.hpp"
#include "support/SyntheticSignals.hpp"

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace {

struct Analyzed {
    bgmedia::PcmBuffer pcm;
    bgmedia::AnalysisConfig config;
    bgmedia::AnalysisResult result;
};

Analyzed analyzeShortTrack() {
    using namespace bgmedia;
    test::TrackOptions options;
    options.bpm = 123.0;
    options.duration = 10.0;
    const auto track = test::makeClickTrack(options);
    std::string error;
    Analyzed analyzed;
    analyzed.pcm = PcmBuffer::fromAudio(track.audio, &error);
    if (!analyzed.pcm.valid()) {
        throw test::Failure("failed to prepare PCM: " + error);
    }
    Analyzer analyzer(analyzed.config);
    auto report = analyzer.analyze(analyzed.pcm);
    if (!report.ok()) {
        throw test::Failure("analysis failed: " + report.message);
    }
    analyzed.result = std::move(report.result);
    return analyzed;
}

void checkEquivalent(const bgmedia::AnalysisResult& a, const bgmedia::AnalysisResult& b) {
    CHECK_NEAR(a.bpm, b.bpm, 1e-9);
    CHECK_NEAR(a.bpmConfidence, b.bpmConfidence, 1e-9);
    CHECK(a.tempoStable == b.tempoStable);
    CHECK(a.octaveAmbiguous == b.octaveAmbiguous);
    CHECK(a.beatsPerBar == b.beatsPerBar);
    CHECK(a.beats.size() == b.beats.size());
    CHECK(a.onsets.size() == b.onsets.size());
    CHECK(a.bassAccents.size() == b.bassAccents.size());
    CHECK(a.silences.size() == b.silences.size());
    CHECK(a.tempoSegments.size() == b.tempoSegments.size());
    CHECK(a.tempoTransitions.size() == b.tempoTransitions.size());
    CHECK(a.waveform.size() == b.waveform.size());
    CHECK(a.spectrogram.frameCount == b.spectrogram.frameCount);
    CHECK(a.spectrogram.magnitudes == b.spectrogram.magnitudes);
    CHECK(a.info.contentHash == b.info.contentHash);
    CHECK(a.info.configHash == b.info.configHash);
    if (!a.beats.empty() && !b.beats.empty()) {
        CHECK_NEAR(a.beats.front().time, b.beats.front().time, 1e-9);
        CHECK_NEAR(a.beats.back().time, b.beats.back().time, 1e-9);
    }
}

} // namespace

TEST_CASE(cache.roundtrip_and_corruption) {
    using namespace bgmedia;
    const Analyzed analyzed = analyzeShortTrack();
    const auto directory =
        std::filesystem::temp_directory_path() / "bgmedia_test_cache_roundtrip";
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
    AnalysisCache cache(directory);

    auto miss = cache.load(analyzed.pcm, analyzed.config);
    CHECK(miss.status == CacheStatus::Miss);

    std::string error;
    REQUIRE(cache.store(analyzed.result, analyzed.pcm, analyzed.config, &error));
    const auto path = directory / AnalysisCache::fileNameFor(
                                       analyzed.pcm.contentHash,
                                       analyzed.config.fingerprint());
    CHECK(std::filesystem::exists(path));

    auto hit = cache.load(analyzed.pcm, analyzed.config);
    REQUIRE(hit.hit());
    checkEquivalent(analyzed.result, hit.result);

    // Corrupt a payload byte; the checksum must turn this into a miss/corrupt,
    // never a partially-valid result.
    {
        std::vector<std::uint8_t> bytes;
        {
            std::ifstream in(path, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(in),
                         std::istreambuf_iterator<char>());
        }
        REQUIRE(bytes.size() > 80);
        bytes.back() ^= 0x5A;
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    auto corrupt = cache.load(analyzed.pcm, analyzed.config);
    CHECK(corrupt.status == CacheStatus::Corrupt);
    CHECK(!corrupt.hit());

    // Restore, then pretend the cache format changed.
    REQUIRE(cache.store(analyzed.result, analyzed.pcm, analyzed.config, &error));
    {
        std::vector<std::uint8_t> bytes;
        {
            std::ifstream in(path, std::ios::binary);
            bytes.assign(std::istreambuf_iterator<char>(in),
                         std::istreambuf_iterator<char>());
        }
        REQUIRE(bytes.size() > 20);
        bytes[8] = 99; // format version, little endian
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }
    auto versionMiss = cache.load(analyzed.pcm, analyzed.config);
    CHECK(versionMiss.status == CacheStatus::Miss);

    std::filesystem::remove_all(directory, ec);
}

TEST_CASE(cache.key_changes_and_validation) {
    using namespace bgmedia;
    const Analyzed analyzed = analyzeShortTrack();
    const auto directory =
        std::filesystem::temp_directory_path() / "bgmedia_test_cache_keys";
    std::error_code ec;
    std::filesystem::remove_all(directory, ec);
    AnalysisCache cache(directory);
    REQUIRE(cache.store(analyzed.result, analyzed.pcm, analyzed.config));

    AnalysisConfig different = analyzed.config;
    different.minBpm = 60.0;
    auto configMiss = cache.load(analyzed.pcm, different);
    CHECK(configMiss.status == CacheStatus::Miss);

    AudioData changedAudio = analyzed.pcm.samples.empty()
                                 ? AudioData{}
                                 : AudioData{analyzed.pcm.samples, 1, analyzed.pcm.sampleRate};
    if (!changedAudio.samples.empty()) {
        changedAudio.samples[0] += 0.01f;
    }
    std::string error;
    const PcmBuffer changedPcm = PcmBuffer::fromAudio(changedAudio, &error);
    auto contentMiss = cache.load(changedPcm, analyzed.config);
    CHECK(contentMiss.status == CacheStatus::Miss);

    // Storing a result against a different PCM must be refused.
    CHECK(!cache.store(analyzed.result, changedPcm, analyzed.config, &error));
    CHECK(!error.empty());

    std::filesystem::remove_all(directory, ec);
}
