#include "bgmedia/cache/AnalysisCache.hpp"

#include "bgmedia/version.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <limits>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace bgmedia {
namespace {

constexpr std::array<char, 8> kMagic{'B', 'G', 'M', 'E', 'D', 'I', 'A', '1'};
constexpr std::size_t kMaxFileBytes = 512ull * 1024ull * 1024ull;
constexpr std::uint32_t kMaxElements = 20'000'000;

constexpr std::uint64_t kFnvOffset = 1469598103934665603ULL;
constexpr std::uint64_t kFnvPrime = 1099511628211ULL;

std::uint64_t hashBytes(const std::uint8_t* data, std::size_t size) noexcept {
    std::uint64_t h = kFnvOffset;
    for (std::size_t i = 0; i < size; ++i) {
        h ^= data[i];
        h *= kFnvPrime;
    }
    return h;
}

class Writer {
public:
    explicit Writer(std::vector<std::uint8_t>& out) : out_(out) {}

    void u8(std::uint8_t value) { out_.push_back(value); }

    void u16(std::uint16_t value) {
        out_.push_back(static_cast<std::uint8_t>(value & 0xFF));
        out_.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    }

    void u32(std::uint32_t value) {
        for (int i = 0; i < 4; ++i) {
            out_.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
        }
    }

    void u64(std::uint64_t value) {
        for (int i = 0; i < 8; ++i) {
            out_.push_back(static_cast<std::uint8_t>((value >> (i * 8)) & 0xFF));
        }
    }

    void i32(std::int32_t value) { u32(static_cast<std::uint32_t>(value)); }

    void f32(float value) {
        std::uint32_t bits = 0;
        static_assert(sizeof(bits) == sizeof(value));
        std::memcpy(&bits, &value, sizeof(bits));
        u32(bits);
    }

    void f64(double value) {
        std::uint64_t bits = 0;
        static_assert(sizeof(bits) == sizeof(value));
        std::memcpy(&bits, &value, sizeof(bits));
        u64(bits);
    }

    void bytes(const std::uint8_t* data, std::size_t size) {
        out_.insert(out_.end(), data, data + size);
    }

private:
    std::vector<std::uint8_t>& out_;
};

class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t size) : data_(data), size_(size) {}

    [[nodiscard]] bool ok() const noexcept { return ok_; }
    [[nodiscard]] std::size_t remaining() const noexcept {
        return ok_ ? size_ - position_ : 0;
    }
    [[nodiscard]] std::size_t position() const noexcept { return position_; }

    std::uint8_t u8() {
        if (!require(1)) {
            return 0;
        }
        return data_[position_++];
    }

    std::uint16_t u16() {
        if (!require(2)) {
            return 0;
        }
        const std::uint16_t v = static_cast<std::uint16_t>(data_[position_]) |
                                (static_cast<std::uint16_t>(data_[position_ + 1]) << 8);
        position_ += 2;
        return v;
    }

    std::uint32_t u32() {
        if (!require(4)) {
            return 0;
        }
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            v |= static_cast<std::uint32_t>(data_[position_ + static_cast<std::size_t>(i)])
                 << (i * 8);
        }
        position_ += 4;
        return v;
    }

    std::uint64_t u64() {
        if (!require(8)) {
            return 0;
        }
        std::uint64_t v = 0;
        for (int i = 0; i < 8; ++i) {
            v |= static_cast<std::uint64_t>(data_[position_ + static_cast<std::size_t>(i)])
                 << (i * 8);
        }
        position_ += 8;
        return v;
    }

    std::int32_t i32() { return static_cast<std::int32_t>(u32()); }

    float f32() {
        const std::uint32_t bits = u32();
        float value = 0.0f;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    double f64() {
        const std::uint64_t bits = u64();
        double value = 0.0;
        std::memcpy(&value, &bits, sizeof(value));
        return value;
    }

    [[nodiscard]] std::uint32_t count(std::uint32_t minimumElementBytes) {
        const std::uint32_t value = u32();
        if (!ok_ || value > kMaxElements ||
            static_cast<std::uint64_t>(value) * minimumElementBytes > remaining()) {
            ok_ = false;
            return 0;
        }
        return value;
    }

    [[nodiscard]] bool bytes(std::uint8_t* out, std::size_t size) {
        if (!require(size)) {
            return false;
        }
        std::memcpy(out, data_ + position_, size);
        position_ += size;
        return true;
    }

    void fail() noexcept { ok_ = false; }

private:
    [[nodiscard]] bool require(std::size_t size) {
        if (!ok_ || size > size_ - position_) {
            ok_ = false;
            return false;
        }
        return true;
    }

    const std::uint8_t* data_ = nullptr;
    std::size_t size_ = 0;
    std::size_t position_ = 0;
    bool ok_ = true;
};

bool finite(double value) { return std::isfinite(value); }
bool finite(float value) { return std::isfinite(value); }

bool validAnalysis(const AnalysisResult& result) {
    if (!finite(result.bpm) || !finite(result.bpmConfidence) ||
        !finite(result.bpmLevelConfidence) || !finite(result.beatPhaseConfidence) ||
        !finite(result.downbeatConfidence) || !finite(result.info.duration) ||
        result.info.duration < 0.0) {
        return false;
    }
    if (result.info.sampleRate == 0 || result.info.sourceChannels == 0 ||
        result.bpmConfidence < 0.0f || result.bpmConfidence > 1.0f ||
        result.bpmLevelConfidence < 0.0f || result.bpmLevelConfidence > 1.0f ||
        result.beatPhaseConfidence < 0.0f || result.beatPhaseConfidence > 1.0f ||
        result.downbeatConfidence < 0.0f || result.downbeatConfidence > 1.0f ||
        result.meterConfidence < 0.0f || result.meterConfidence > 1.0f ||
        result.beatsPerBar < 0 || result.beatsPerBar > 8) {
        return false;
    }
    double previousBeatTime = -1e9;
    for (const Beat& beat : result.beats) {
        if (!finite(beat.time) || !finite(beat.strength) || !finite(beat.confidence) ||
            beat.time < -0.25 || beat.time > result.info.duration + 0.25 ||
            beat.time < previousBeatTime - 1e-6 ||
            beat.beatInBar < 0 || beat.barIndex < -1) {
            return false;
        }
        if (result.beatsPerBar > 0 && beat.beatInBar >= result.beatsPerBar) {
            return false;
        }
        previousBeatTime = beat.time;
    }
    for (const Onset& onset : result.onsets) {
        if (!finite(onset.time) || onset.time < -0.25 ||
            onset.time > result.info.duration + 0.25) {
            return false;
        }
    }
    for (const auto& segment : result.tempoSegments) {
        if (!finite(segment.bpm) || !finite(segment.start) || !finite(segment.end) ||
            segment.bpm < 0.0 || segment.start < -0.1 || segment.end > result.info.duration + 0.5) {
            return false;
        }
    }
    if (result.spectrogram.frameCount > 0 &&
        result.spectrogram.magnitudes.size() !=
            static_cast<std::size_t>(result.spectrogram.bands) * result.spectrogram.frameCount) {
        return false;
    }
    return true;
}

void writeResult(Writer& w, const AnalysisResult& r) {
    w.f64(r.bpm);
    w.f32(r.bpmConfidence);
    w.f32(r.bpmLevelConfidence);
    w.f32(r.beatPhaseConfidence);
    w.f32(r.downbeatConfidence);
    w.u8(r.tempoStable ? 1 : 0);
    w.u8(r.octaveAmbiguous ? 1 : 0);
    w.i32(r.beatsPerBar);
    w.f32(r.meterConfidence);

    w.u32(static_cast<std::uint32_t>(r.bpmCandidates.size()));
    for (double value : r.bpmCandidates) {
        w.f64(value);
    }
    w.u32(static_cast<std::uint32_t>(r.bpmCandidateScores.size()));
    for (float value : r.bpmCandidateScores) {
        w.f32(value);
    }

    w.u32(static_cast<std::uint32_t>(r.tempoSegments.size()));
    for (const auto& s : r.tempoSegments) {
        w.f64(s.start);
        w.f64(s.end);
        w.f64(s.bpm);
        w.f32(s.confidence);
    }
    w.u32(static_cast<std::uint32_t>(r.tempoTransitions.size()));
    for (const auto& t : r.tempoTransitions) {
        w.f64(t.start);
        w.f64(t.end);
        w.f64(t.fromBpm);
        w.f64(t.toBpm);
        w.f32(t.confidence);
    }

    w.u32(static_cast<std::uint32_t>(r.beats.size()));
    for (const auto& b : r.beats) {
        w.f64(b.time);
        w.f32(b.strength);
        w.f32(b.confidence);
        w.i32(b.beatInBar);
        w.i32(b.barIndex);
        w.u8(b.downbeat ? 1 : 0);
        w.u8(b.extrapolated ? 1 : 0);
    }

    w.u32(static_cast<std::uint32_t>(r.onsets.size()));
    for (const auto& o : r.onsets) {
        w.f64(o.time);
        w.f32(o.strength);
        w.f32(o.lowBandRatio);
    }

    w.u32(static_cast<std::uint32_t>(r.bassAccents.size()));
    for (const auto& a : r.bassAccents) {
        w.f64(a.time);
        w.f32(a.strength);
        w.f32(a.lowBandRatio);
        w.f32(a.confidence);
        w.i32(a.nearestBeat);
        w.f64(a.offsetFromBeat);
    }

    w.u32(static_cast<std::uint32_t>(r.silences.size()));
    for (const auto& s : r.silences) {
        w.f64(s.start);
        w.f64(s.end);
        w.f32(s.confidence);
    }

    w.u32(static_cast<std::uint32_t>(r.sections.size()));
    for (const auto& s : r.sections) {
        w.f64(s.start);
        w.f64(s.end);
        w.i32(static_cast<std::int32_t>(s.kind));
        w.f32(s.energy);
        w.f32(s.confidence);
    }

    w.u32(static_cast<std::uint32_t>(r.waveform.size()));
    for (const auto& p : r.waveform) {
        w.f32(p.minValue);
        w.f32(p.maxValue);
    }
    w.f32(r.waveformPeaksPerSecond);

    w.i32(r.spectrogram.bands);
    w.f64(r.spectrogram.framesPerSecond);
    w.u32(r.spectrogram.frameCount);
    w.u32(static_cast<std::uint32_t>(r.spectrogram.magnitudes.size()));
    if (!r.spectrogram.magnitudes.empty()) {
        w.bytes(r.spectrogram.magnitudes.data(), r.spectrogram.magnitudes.size());
    }

    w.f64(r.analysisSeconds);
    w.f64(r.activeRatio);
}

AnalysisResult readResult(Reader& r) {
    AnalysisResult result;
    result.bpm = r.f64();
    result.bpmConfidence = r.f32();
    result.bpmLevelConfidence = r.f32();
    result.beatPhaseConfidence = r.f32();
    result.downbeatConfidence = r.f32();
    result.tempoStable = r.u8() != 0;
    result.octaveAmbiguous = r.u8() != 0;
    result.beatsPerBar = r.i32();
    result.meterConfidence = r.f32();

    std::uint32_t count = r.count(8);
    result.bpmCandidates.resize(count);
    for (double& value : result.bpmCandidates) {
        value = r.f64();
    }
    count = r.count(4);
    result.bpmCandidateScores.resize(count);
    for (float& value : result.bpmCandidateScores) {
        value = r.f32();
    }

    count = r.count(28);
    result.tempoSegments.resize(count);
    for (auto& s : result.tempoSegments) {
        s.start = r.f64();
        s.end = r.f64();
        s.bpm = r.f64();
        s.confidence = r.f32();
    }
    count = r.count(36);
    result.tempoTransitions.resize(count);
    for (auto& t : result.tempoTransitions) {
        t.start = r.f64();
        t.end = r.f64();
        t.fromBpm = r.f64();
        t.toBpm = r.f64();
        t.confidence = r.f32();
    }

    count = r.count(26);
    result.beats.resize(count);
    for (auto& b : result.beats) {
        b.time = r.f64();
        b.strength = r.f32();
        b.confidence = r.f32();
        b.beatInBar = r.i32();
        b.barIndex = r.i32();
        b.downbeat = r.u8() != 0;
        b.extrapolated = r.u8() != 0;
    }

    count = r.count(16);
    result.onsets.resize(count);
    for (auto& o : result.onsets) {
        o.time = r.f64();
        o.strength = r.f32();
        o.lowBandRatio = r.f32();
    }

    count = r.count(32);
    result.bassAccents.resize(count);
    for (auto& a : result.bassAccents) {
        a.time = r.f64();
        a.strength = r.f32();
        a.lowBandRatio = r.f32();
        a.confidence = r.f32();
        a.nearestBeat = r.i32();
        a.offsetFromBeat = r.f64();
    }

    count = r.count(20);
    result.silences.resize(count);
    for (auto& s : result.silences) {
        s.start = r.f64();
        s.end = r.f64();
        s.confidence = r.f32();
    }

    count = r.count(28);
    result.sections.resize(count);
    for (auto& s : result.sections) {
        s.start = r.f64();
        s.end = r.f64();
        s.kind = static_cast<SectionKind>(r.i32());
        s.energy = r.f32();
        s.confidence = r.f32();
    }

    count = r.count(8);
    result.waveform.resize(count);
    for (auto& p : result.waveform) {
        p.minValue = r.f32();
        p.maxValue = r.f32();
    }
    result.waveformPeaksPerSecond = r.f32();

    result.spectrogram.bands = r.i32();
    result.spectrogram.framesPerSecond = r.f64();
    result.spectrogram.frameCount = r.u32();
    const std::uint32_t magnitudeBytes = r.count(1);
    result.spectrogram.magnitudes.resize(magnitudeBytes);
    if (magnitudeBytes > 0 &&
        !r.bytes(result.spectrogram.magnitudes.data(), magnitudeBytes)) {
        r.fail();
    }
    result.analysisSeconds = r.f64();
    result.activeRatio = r.f64();
    return result;
}

} // namespace

AnalysisCache::AnalysisCache(std::filesystem::path directory)
    : directory_(std::move(directory)) {}

std::string AnalysisCache::fileNameFor(std::uint64_t contentHash,
                                       std::uint64_t configHash) {
    std::ostringstream out;
    out << std::hex << std::setfill('0')
        << std::setw(16) << contentHash << '-'
        << std::setw(16) << configHash << ".bgm";
    return out.str();
}

CacheLoadResult AnalysisCache::load(const PcmBuffer& pcm,
                                    const AnalysisConfig& config) const {
    CacheLoadResult result;
    if (!pcm.valid()) {
        result.status = CacheStatus::Error;
        result.message = "cannot load cache for an invalid PCM buffer";
        return result;
    }

    const std::filesystem::path file =
        directory_ / fileNameFor(pcm.contentHash, config.fingerprint());
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        result.status = CacheStatus::Miss;
        result.message = "no cache entry";
        return result;
    }
    const auto size = std::filesystem::file_size(file, ec);
    if (ec || size < 8 || size > kMaxFileBytes) {
        result.status = CacheStatus::Corrupt;
        result.message = "cache file has an invalid size";
        return result;
    }

    std::ifstream in(file, std::ios::binary);
    if (!in) {
        result.status = CacheStatus::Error;
        result.message = "cannot open cache file";
        return result;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!in) {
        result.status = CacheStatus::Corrupt;
        result.message = "failed to read cache file";
        return result;
    }

    Reader header(bytes.data(), bytes.size());
    std::array<char, 8> magic{};
    (void)header.bytes(reinterpret_cast<std::uint8_t*>(magic.data()), magic.size());
    if (!header.ok() || magic != kMagic) {
        result.status = CacheStatus::Corrupt;
        result.message = "cache magic does not match";
        return result;
    }

    const std::uint32_t formatVersion = header.u32();
    const std::uint32_t schemaVersion = header.u32();
    const std::uint32_t analyzerVersion = header.u32();
    (void)header.u32(); // reserved
    const std::uint64_t configHash = header.u64();
    const std::uint64_t contentHash = header.u64();
    const std::uint32_t sampleRate = header.u32();
    const std::uint32_t sourceChannels = header.u32();
    const double duration = header.f64();
    const std::uint32_t payloadBytes = header.u32();
    const std::uint64_t payloadHash = header.u64();
    if (!header.ok()) {
        result.status = CacheStatus::Corrupt;
        result.message = "cache header is truncated";
        return result;
    }
    if (formatVersion != kCacheFormatVersion || schemaVersion != kAnalysisSchemaVersion ||
        analyzerVersion != kAnalyzerVersion) {
        result.status = CacheStatus::Miss;
        result.message = "cache version mismatch";
        return result;
    }
    if (configHash != config.fingerprint() || contentHash != pcm.contentHash) {
        result.status = CacheStatus::Miss;
        result.message = "cache key mismatch";
        return result;
    }
    if (sampleRate != pcm.sampleRate || sourceChannels != pcm.sourceChannels ||
        std::abs(duration - pcm.duration()) > 0.002) {
        result.status = CacheStatus::Miss;
        result.message = "cached source metadata mismatch";
        return result;
    }

    const std::size_t payloadOffset = header.position();
    if (payloadBytes > bytes.size() - payloadOffset) {
        result.status = CacheStatus::Corrupt;
        result.message = "cache payload is truncated";
        return result;
    }
    if (hashBytes(bytes.data() + payloadOffset, payloadBytes) != payloadHash) {
        result.status = CacheStatus::Corrupt;
        result.message = "cache payload checksum mismatch";
        return result;
    }

    Reader payload(bytes.data() + payloadOffset, payloadBytes);
    AnalysisResult loaded = readResult(payload);
    if (!payload.ok() || payload.remaining() != 0) {
        result.status = CacheStatus::Corrupt;
        result.message = "cache payload is malformed";
        return result;
    }

    loaded.info.schemaVersion = schemaVersion;
    loaded.info.analyzerVersion = analyzerVersion;
    loaded.info.contentHash = contentHash;
    loaded.info.configHash = configHash;
    loaded.info.sampleRate = sampleRate;
    loaded.info.sourceChannels = sourceChannels;
    loaded.info.duration = duration;

    if (!validAnalysis(loaded)) {
        result.status = CacheStatus::Corrupt;
        result.message = "cache payload failed validation";
        return result;
    }

    result.status = CacheStatus::Hit;
    result.result = std::move(loaded);
    result.message = "cache hit";
    return result;
}

bool AnalysisCache::store(const AnalysisResult& result, const PcmBuffer& pcm,
                          const AnalysisConfig& config, std::string* error) const {
    if (!pcm.valid()) {
        if (error) {
            *error = "cannot cache an invalid PCM buffer";
        }
        return false;
    }
    if (result.info.contentHash != pcm.contentHash ||
        result.info.sampleRate != pcm.sampleRate ||
        std::abs(result.info.duration - pcm.duration()) > 0.002) {
        if (error) {
            *error = "analysis result does not belong to this PCM buffer";
        }
        return false;
    }
    if (!validAnalysis(result)) {
        if (error) {
            *error = "refusing to cache an invalid analysis result";
        }
        return false;
    }

    std::error_code ec;
    std::filesystem::create_directories(directory_, ec);
    if (ec) {
        if (error) {
            *error = "cannot create cache directory: " + ec.message();
        }
        return false;
    }

    std::vector<std::uint8_t> payload;
    payload.reserve(64 * 1024);
    Writer payloadWriter(payload);
    writeResult(payloadWriter, result);
    const std::uint64_t payloadHash = hashBytes(payload.data(), payload.size());

    std::vector<std::uint8_t> bytes;
    bytes.reserve(payload.size() + 96);
    Writer writer(bytes);
    writer.bytes(reinterpret_cast<const std::uint8_t*>(kMagic.data()), kMagic.size());
    writer.u32(kCacheFormatVersion);
    writer.u32(kAnalysisSchemaVersion);
    writer.u32(kAnalyzerVersion);
    writer.u32(0);
    writer.u64(config.fingerprint());
    writer.u64(pcm.contentHash);
    writer.u32(pcm.sampleRate);
    writer.u32(pcm.sourceChannels);
    writer.f64(pcm.duration());
    writer.u32(static_cast<std::uint32_t>(payload.size()));
    writer.u64(payloadHash);
    writer.bytes(payload.data(), payload.size());

    const std::filesystem::path target =
        directory_ / fileNameFor(pcm.contentHash, config.fingerprint());
    static std::atomic<std::uint64_t> counter{0};
    std::filesystem::path temporary = target;
    temporary += ".tmp-" + std::to_string(counter.fetch_add(1, std::memory_order_relaxed));

    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error) {
                *error = "cannot create temporary cache file";
            }
            return false;
        }
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        out.flush();
        if (!out) {
            out.close();
            (void)std::filesystem::remove(temporary, ec);
            if (error) {
                *error = "failed to write cache file";
            }
            return false;
        }
    }

    std::filesystem::rename(temporary, target, ec);
    if (ec) {
        // Windows does not replace an existing target with rename. Remove and
        // retry; the cache is content-addressed, so replacing is safe.
        std::error_code removeError;
        (void)std::filesystem::remove(target, removeError);
        ec.clear();
        std::filesystem::rename(temporary, target, ec);
    }
    if (ec) {
        std::error_code removeError;
        (void)std::filesystem::remove(temporary, removeError);
        if (error) {
            *error = "cannot commit cache file: " + ec.message();
        }
        return false;
    }
    return true;
}

} // namespace bgmedia
