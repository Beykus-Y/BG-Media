#include "bgmedia/mix/MixFeatureCache.hpp"

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
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

namespace bgmedia::mix {
namespace {

constexpr std::array<char, 8> kMagic{'B', 'G', 'M', 'I', 'X', 'F', 'M', '1'};
constexpr std::size_t kMaxFileBytes = 512ull * 1024ull * 1024ull;
constexpr std::uint32_t kMaxElements = 10'000'000;
constexpr std::uint32_t kMaxStringBytes = 4096;

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

    void i8(std::int8_t value) {
        out_.push_back(static_cast<std::uint8_t>(value));
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

    void str(const std::string& s) {
        const std::uint32_t len = static_cast<std::uint32_t>(
            std::min<std::size_t>(s.size(), kMaxStringBytes));
        u32(len);
        if (len > 0) {
            bytes(reinterpret_cast<const std::uint8_t*>(s.data()), len);
        }
    }

    void bytes(const std::uint8_t* data, std::size_t size) {
        out_.insert(out_.end(), data, data + size);
    }

private:
    std::vector<std::uint8_t>& out_;
};

class Reader {
public:
    Reader(const std::uint8_t* data, std::size_t size)
        : data_(data), size_(size) {}

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

    std::int8_t i8() { return static_cast<std::int8_t>(u8()); }

    std::uint32_t u32() {
        if (!require(4)) {
            return 0;
        }
        std::uint32_t v = 0;
        for (int i = 0; i < 4; ++i) {
            v |= static_cast<std::uint32_t>(
                     data_[position_ + static_cast<std::size_t>(i)])
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
            v |= static_cast<std::uint64_t>(
                     data_[position_ + static_cast<std::size_t>(i)])
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

    std::string str() {
        const std::uint32_t len = u32();
        if (!ok_ || len > kMaxStringBytes || !require(len)) {
            ok_ = false;
            return {};
        }
        std::string out(reinterpret_cast<const char*>(data_ + position_), len);
        position_ += len;
        return out;
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

void writeCandidate(Writer& w, const CuePointCandidate& c) {
    w.f64(c.time);
    w.i32(c.barIndex);
    w.i32(c.beatIndex);
    w.u8(static_cast<std::uint8_t>(c.role));
    w.u8(static_cast<std::uint8_t>(c.kind));
    w.i32(static_cast<std::int32_t>(c.sectionKind));
    w.f32(c.boundaryStrength);
    w.f32(c.rhythmConfidence);
    w.f32(c.phraseConfidence);
    w.f32(c.energyBefore);
    w.f32(c.energyAfter);
    w.f32(c.bassDominance);
    w.f64(c.availableWindowSeconds);
    w.f32(c.vocalBefore);
    w.f32(c.vocalAfter);
    w.f32(c.vocalConfidence);
    w.f32(c.harmonicStability);
    w.f32(c.priorScore);
    w.str(c.reason);
}

CuePointCandidate readCandidate(Reader& r) {
    CuePointCandidate c;
    c.time = r.f64();
    c.barIndex = r.i32();
    c.beatIndex = r.i32();
    c.role = static_cast<CueRole>(r.u8());
    c.kind = static_cast<CueKind>(r.u8());
    c.sectionKind = static_cast<SectionKind>(r.i32());
    c.boundaryStrength = r.f32();
    c.rhythmConfidence = r.f32();
    c.phraseConfidence = r.f32();
    c.energyBefore = r.f32();
    c.energyAfter = r.f32();
    c.bassDominance = r.f32();
    c.availableWindowSeconds = r.f64();
    c.vocalBefore = r.f32();
    c.vocalAfter = r.f32();
    c.vocalConfidence = r.f32();
    c.harmonicStability = r.f32();
    c.priorScore = r.f32();
    c.reason = r.str();
    return c;
}

void writePayload(Writer& w, const MixFeatureMap& m) {
    w.f64(m.info.frameStepSeconds);
    w.u8(m.info.fromPcm ? 1 : 0);

    w.f32(m.loudness.integratedLufs);
    w.f32(m.loudness.shortTermMaxLufs);
    w.f32(m.loudness.momentaryMaxLufs);
    w.f32(m.loudness.loudnessRangeLu);
    w.f32(m.loudness.meanRmsDbfs);
    w.f32(m.loudness.samplePeakDbfs);
    w.u8(m.loudness.gatedValid ? 1 : 0);

    w.f64(m.audible.audibleStart);
    w.f64(m.audible.fullBodyStart);
    w.f64(m.audible.tailStart);
    w.f64(m.audible.audibleEnd);
    w.f32(m.audible.noiseFloorDbfs);
    w.f32(m.audible.referenceDbfs);
    w.f32(m.audible.enterThresholdDbfs);
    w.f32(m.audible.exitThresholdDbfs);
    w.u8(m.audible.hasFadeIn ? 1 : 0);
    w.u8(m.audible.hasFadeOut ? 1 : 0);
    w.u8(m.audible.hasReverbTail ? 1 : 0);
    w.f32(m.audible.confidence);

    w.f64(m.globalBpm);
    w.f32(m.bpmConfidence);
    w.f32(m.beatPhaseConfidence);
    w.f32(m.downbeatConfidence);
    w.i32(m.beatsPerBar);
    w.f32(m.meterConfidence);
    w.u8(m.tempoStable ? 1 : 0);

    w.u32(static_cast<std::uint32_t>(m.tempoSegments.size()));
    for (const TempoSegment& s : m.tempoSegments) {
        w.f64(s.start);
        w.f64(s.end);
        w.f64(s.bpm);
        w.f32(s.confidence);
    }

    w.u32(static_cast<std::uint32_t>(m.tempoTransitions.size()));
    for (const TempoTransition& t : m.tempoTransitions) {
        w.f64(t.start);
        w.f64(t.end);
        w.f64(t.fromBpm);
        w.f64(t.toBpm);
        w.f32(t.confidence);
    }

    w.u32(static_cast<std::uint32_t>(m.sections.size()));
    for (const Section& s : m.sections) {
        w.f64(s.start);
        w.f64(s.end);
        w.i32(static_cast<std::int32_t>(s.kind));
        w.f32(s.energy);
        w.f32(s.confidence);
    }

    w.u32(static_cast<std::uint32_t>(m.frames.size()));
    for (const MixFeatureFrame& f : m.frames) {
        w.f64(f.time);
        w.f32(f.rms);
        w.f32(f.momentaryLufs);
        w.f32(f.shortTermLufs);
        w.f32(f.energy);
        w.f32(f.lowEnergy);
        w.f32(f.midEnergy);
        w.f32(f.highEnergy);
        w.f32(f.lowShare);
        w.f32(f.midShare);
        w.f32(f.highShare);
        w.f32(f.bassDominance);
        w.f32(f.onsetDensity);
        w.f32(f.spectralNovelty);
        w.f32(f.boundaryStrength);
        w.f32(f.vocalScore);
        w.f32(f.vocalConfidence);
        w.f32(f.harmonicSalience);
        w.i8(f.dominantPitchClass);
        w.u8(f.active ? 1 : 0);
    }

    w.u32(static_cast<std::uint32_t>(m.bars.size()));
    for (const BarFeatureSummary& b : m.bars) {
        w.i32(b.barIndex);
        w.f64(b.start);
        w.f64(b.end);
        w.i32(b.beatCount);
        w.f64(b.downbeatTime);
        w.f32(b.downbeatConfidence);
        w.f32(b.meanEnergy);
        w.f32(b.momentaryLufs);
        w.f32(b.lowShare);
        w.f32(b.midShare);
        w.f32(b.highShare);
        w.f32(b.bassDominance);
        w.f32(b.onsetDensity);
        w.f32(b.spectralNovelty);
        w.f32(b.boundaryStrength);
        w.f32(b.vocalScore);
        w.f32(b.vocalConfidence);
        w.i32(static_cast<std::int32_t>(b.sectionKind));
    }

    w.u32(static_cast<std::uint32_t>(m.phrases.size()));
    for (const PhraseSpan& p : m.phrases) {
        w.f64(p.start);
        w.f64(p.end);
        w.i32(p.barStart);
        w.i32(p.barCount);
        w.i32(p.beatCount);
        w.i32(static_cast<std::int32_t>(p.sectionKind));
        w.f32(p.meanEnergy);
        w.f32(p.energySlope);
        w.f32(p.bassDominance);
        w.f32(p.boundaryStrengthStart);
        w.f32(p.boundaryStrengthEnd);
        w.f32(p.confidence);
        w.u8(p.regularMeter ? 1 : 0);
    }

    w.u32(static_cast<std::uint32_t>(m.mixInCandidates.size()));
    for (const CuePointCandidate& c : m.mixInCandidates) {
        writeCandidate(w, c);
    }

    w.u32(static_cast<std::uint32_t>(m.mixOutCandidates.size()));
    for (const CuePointCandidate& c : m.mixOutCandidates) {
        writeCandidate(w, c);
    }

    w.u8(m.vocal.available ? 1 : 0);
    w.u8(static_cast<std::uint8_t>(m.vocal.detectorKind));
    w.u32(m.vocal.modelVersion);
    w.u64(m.vocal.modelChecksum);
    w.f32(m.vocal.meanConfidence);
    w.f32(m.vocal.activeRatio);
    w.u32(static_cast<std::uint32_t>(m.vocal.regions.size()));
    for (const VocalRegion& reg : m.vocal.regions) {
        w.f64(reg.start);
        w.f64(reg.end);
        w.f32(reg.score);
        w.f32(reg.confidence);
        w.u8(reg.leadLikely ? 1 : 0);
    }

    w.u8(m.harmony.available ? 1 : 0);
    w.i32(m.harmony.keyIndex);
    w.u8(m.harmony.isMinor ? 1 : 0);
    w.str(m.harmony.camelotCode);
    w.f32(m.harmony.keyConfidence);
    w.f32(m.harmony.tonalness);
    for (float c : m.harmony.globalChroma) {
        w.f32(c);
    }
    w.u32(static_cast<std::uint32_t>(m.harmony.segments.size()));
    for (const HarmonicSegment& seg : m.harmony.segments) {
        w.f64(seg.start);
        w.f64(seg.end);
        w.i32(seg.keyIndex);
        w.u8(seg.isMinor ? 1 : 0);
        w.f32(seg.confidence);
        w.f32(seg.tonalness);
        for (float c : seg.chroma) {
            w.f32(c);
        }
    }
}

MixFeatureMap readPayload(Reader& r) {
    MixFeatureMap m;
    m.info.frameStepSeconds = r.f64();
    m.info.fromPcm = r.u8() != 0;

    m.loudness.integratedLufs = r.f32();
    m.loudness.shortTermMaxLufs = r.f32();
    m.loudness.momentaryMaxLufs = r.f32();
    m.loudness.loudnessRangeLu = r.f32();
    m.loudness.meanRmsDbfs = r.f32();
    m.loudness.samplePeakDbfs = r.f32();
    m.loudness.gatedValid = r.u8() != 0;

    m.audible.audibleStart = r.f64();
    m.audible.fullBodyStart = r.f64();
    m.audible.tailStart = r.f64();
    m.audible.audibleEnd = r.f64();
    m.audible.noiseFloorDbfs = r.f32();
    m.audible.referenceDbfs = r.f32();
    m.audible.enterThresholdDbfs = r.f32();
    m.audible.exitThresholdDbfs = r.f32();
    m.audible.hasFadeIn = r.u8() != 0;
    m.audible.hasFadeOut = r.u8() != 0;
    m.audible.hasReverbTail = r.u8() != 0;
    m.audible.confidence = r.f32();

    m.globalBpm = r.f64();
    m.bpmConfidence = r.f32();
    m.beatPhaseConfidence = r.f32();
    m.downbeatConfidence = r.f32();
    m.beatsPerBar = r.i32();
    m.meterConfidence = r.f32();
    m.tempoStable = r.u8() != 0;

    std::uint32_t count = r.count(28);
    m.tempoSegments.resize(count);
    for (TempoSegment& s : m.tempoSegments) {
        s.start = r.f64();
        s.end = r.f64();
        s.bpm = r.f64();
        s.confidence = r.f32();
    }

    count = r.count(36);
    m.tempoTransitions.resize(count);
    for (TempoTransition& t : m.tempoTransitions) {
        t.start = r.f64();
        t.end = r.f64();
        t.fromBpm = r.f64();
        t.toBpm = r.f64();
        t.confidence = r.f32();
    }

    count = r.count(28);
    m.sections.resize(count);
    for (Section& s : m.sections) {
        s.start = r.f64();
        s.end = r.f64();
        s.kind = static_cast<SectionKind>(r.i32());
        s.energy = r.f32();
        s.confidence = r.f32();
    }

    count = r.count(78);
    m.frames.resize(count);
    for (MixFeatureFrame& f : m.frames) {
        f.time = r.f64();
        f.rms = r.f32();
        f.momentaryLufs = r.f32();
        f.shortTermLufs = r.f32();
        f.energy = r.f32();
        f.lowEnergy = r.f32();
        f.midEnergy = r.f32();
        f.highEnergy = r.f32();
        f.lowShare = r.f32();
        f.midShare = r.f32();
        f.highShare = r.f32();
        f.bassDominance = r.f32();
        f.onsetDensity = r.f32();
        f.spectralNovelty = r.f32();
        f.boundaryStrength = r.f32();
        f.vocalScore = r.f32();
        f.vocalConfidence = r.f32();
        f.harmonicSalience = r.f32();
        f.dominantPitchClass = r.i8();
        f.active = r.u8() != 0;
    }

    count = r.count(76);
    m.bars.resize(count);
    for (BarFeatureSummary& b : m.bars) {
        b.barIndex = r.i32();
        b.start = r.f64();
        b.end = r.f64();
        b.beatCount = r.i32();
        b.downbeatTime = r.f64();
        b.downbeatConfidence = r.f32();
        b.meanEnergy = r.f32();
        b.momentaryLufs = r.f32();
        b.lowShare = r.f32();
        b.midShare = r.f32();
        b.highShare = r.f32();
        b.bassDominance = r.f32();
        b.onsetDensity = r.f32();
        b.spectralNovelty = r.f32();
        b.boundaryStrength = r.f32();
        b.vocalScore = r.f32();
        b.vocalConfidence = r.f32();
        b.sectionKind = static_cast<SectionKind>(r.i32());
    }

    count = r.count(57);
    m.phrases.resize(count);
    for (PhraseSpan& p : m.phrases) {
        p.start = r.f64();
        p.end = r.f64();
        p.barStart = r.i32();
        p.barCount = r.i32();
        p.beatCount = r.i32();
        p.sectionKind = static_cast<SectionKind>(r.i32());
        p.meanEnergy = r.f32();
        p.energySlope = r.f32();
        p.bassDominance = r.f32();
        p.boundaryStrengthStart = r.f32();
        p.boundaryStrengthEnd = r.f32();
        p.confidence = r.f32();
        p.regularMeter = r.u8() != 0;
    }

    count = r.count(74);
    m.mixInCandidates.resize(count);
    for (CuePointCandidate& c : m.mixInCandidates) {
        c = readCandidate(r);
    }

    count = r.count(74);
    m.mixOutCandidates.resize(count);
    for (CuePointCandidate& c : m.mixOutCandidates) {
        c = readCandidate(r);
    }

    m.vocal.available = r.u8() != 0;
    m.vocal.detectorKind = static_cast<VocalDetectorKind>(r.u8());
    m.vocal.modelVersion = r.u32();
    m.vocal.modelChecksum = r.u64();
    m.vocal.meanConfidence = r.f32();
    m.vocal.activeRatio = r.f32();
    count = r.count(25);
    m.vocal.regions.resize(count);
    for (VocalRegion& reg : m.vocal.regions) {
        reg.start = r.f64();
        reg.end = r.f64();
        reg.score = r.f32();
        reg.confidence = r.f32();
        reg.leadLikely = r.u8() != 0;
    }

    m.harmony.available = r.u8() != 0;
    m.harmony.keyIndex = r.i32();
    m.harmony.isMinor = r.u8() != 0;
    m.harmony.camelotCode = r.str();
    m.harmony.keyConfidence = r.f32();
    m.harmony.tonalness = r.f32();
    for (float& c : m.harmony.globalChroma) {
        c = r.f32();
    }
    count = r.count(77);
    m.harmony.segments.resize(count);
    for (HarmonicSegment& seg : m.harmony.segments) {
        seg.start = r.f64();
        seg.end = r.f64();
        seg.keyIndex = r.i32();
        seg.isMinor = r.u8() != 0;
        seg.confidence = r.f32();
        seg.tonalness = r.f32();
        for (float& c : seg.chroma) {
            c = r.f32();
        }
    }

    return m;
}

} // namespace

MixFeatureCache::MixFeatureCache(std::filesystem::path directory)
    : directory_(std::move(directory)) {}

std::string MixFeatureCache::fileNameFor(std::uint64_t contentHash,
                                         std::uint64_t analysisConfigHash,
                                         std::uint64_t featureConfigHash) {
    std::ostringstream out;
    out << std::hex << std::setfill('0')
        << std::setw(16) << contentHash << '-'
        << std::setw(16) << analysisConfigHash << '-'
        << std::setw(16) << featureConfigHash << ".bgf";
    return out.str();
}

FeatureCacheLoadResult MixFeatureCache::load(
    const PcmBuffer& pcm,
    const AnalysisResult& analysis,
    const MixFeatureConfig& config) const {
    if (!pcm.valid()) {
        FeatureCacheLoadResult res;
        res.status = FeatureCacheStatus::Error;
        res.message = "cannot load feature cache for an invalid PCM buffer";
        return res;
    }
    const std::uint64_t contentHash =
        pcm.contentHash != 0 ? pcm.contentHash : analysis.info.contentHash;
    return load(contentHash, analysis.info.configHash, config,
                pcm.sampleRate, pcm.sourceChannels, pcm.duration());
}

FeatureCacheLoadResult MixFeatureCache::load(
    std::uint64_t contentHash,
    std::uint64_t analysisConfigHash,
    const MixFeatureConfig& config,
    std::uint32_t expectedSampleRate,
    std::uint32_t expectedSourceChannels,
    double expectedDuration) const {
    FeatureCacheLoadResult result;
    const std::uint64_t featureHash = config.fingerprint();
    const std::filesystem::path file =
        directory_ / fileNameFor(contentHash, analysisConfigHash, featureHash);
    std::error_code ec;
    if (!std::filesystem::exists(file, ec)) {
        result.status = FeatureCacheStatus::Miss;
        result.message = "no feature cache entry";
        return result;
    }
    const auto size = std::filesystem::file_size(file, ec);
    if (ec || size < 16 || size > kMaxFileBytes) {
        result.status = FeatureCacheStatus::Corrupt;
        result.message = "feature cache file has an invalid size";
        return result;
    }

    std::ifstream in(file, std::ios::binary);
    if (!in) {
        result.status = FeatureCacheStatus::Error;
        result.message = "cannot open feature cache file";
        return result;
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(bytes.data()),
            static_cast<std::streamsize>(bytes.size()));
    if (!in) {
        result.status = FeatureCacheStatus::Corrupt;
        result.message = "failed to read feature cache file";
        return result;
    }

    Reader header(bytes.data(), bytes.size());
    std::array<char, 8> magic{};
    (void)header.bytes(reinterpret_cast<std::uint8_t*>(magic.data()), magic.size());
    if (!header.ok() || magic != kMagic) {
        result.status = FeatureCacheStatus::Corrupt;
        result.message = "feature cache magic does not match";
        return result;
    }

    const std::uint32_t formatVersion = header.u32();
    const std::uint32_t schemaVersion = header.u32();
    const std::uint32_t extractorVersion = header.u32();
    const std::uint32_t analyzerVersion = header.u32();
    const std::uint64_t storedContentHash = header.u64();
    const std::uint64_t storedAnalysisHash = header.u64();
    const std::uint64_t storedFeatureHash = header.u64();
    const std::uint32_t vocalDetectorKind = header.u32();
    const std::uint32_t vocalModelVersion = header.u32();
    const std::uint64_t vocalModelChecksum = header.u64();
    const std::uint32_t harmonicAnalyzeVersion = header.u32();
    const std::uint32_t sampleRate = header.u32();
    const std::uint32_t sourceChannels = header.u32();
    const double duration = header.f64();
    const std::uint32_t payloadBytes = header.u32();
    const std::uint64_t payloadHash = header.u64();

    if (!header.ok()) {
        result.status = FeatureCacheStatus::Corrupt;
        result.message = "feature cache header is truncated";
        return result;
    }
    if (formatVersion != kMixFeatureCacheFormatVersion ||
        schemaVersion != kMixFeatureSchemaVersion ||
        extractorVersion != kMixFeatureExtractorVersion ||
        analyzerVersion != kAnalyzerVersion) {
        result.status = FeatureCacheStatus::Miss;
        result.message = "feature cache version mismatch";
        return result;
    }
    if (storedContentHash != contentHash ||
        storedAnalysisHash != analysisConfigHash ||
        storedFeatureHash != featureHash ||
        vocalDetectorKind != static_cast<std::uint32_t>(config.vocalDetector) ||
        vocalModelVersion != config.vocalModelVersion ||
        vocalModelChecksum != config.vocalModelChecksum ||
        harmonicAnalyzeVersion != config.harmonicAnalyzeVersion) {
        result.status = FeatureCacheStatus::Miss;
        result.message = "feature cache key or model version mismatch";
        return result;
    }
    if (sampleRate != expectedSampleRate ||
        sourceChannels != expectedSourceChannels ||
        std::abs(duration - expectedDuration) > 0.002) {
        result.status = FeatureCacheStatus::Miss;
        result.message = "cached source metadata mismatch";
        return result;
    }

    const std::size_t payloadOffset = header.position();
    if (payloadBytes > bytes.size() - payloadOffset) {
        result.status = FeatureCacheStatus::Corrupt;
        result.message = "feature cache payload is truncated";
        return result;
    }
    if (hashBytes(bytes.data() + payloadOffset, payloadBytes) != payloadHash) {
        result.status = FeatureCacheStatus::Corrupt;
        result.message = "feature cache payload checksum mismatch";
        return result;
    }

    Reader payload(bytes.data() + payloadOffset, payloadBytes);
    MixFeatureMap loaded = readPayload(payload);
    if (!payload.ok() || payload.remaining() != 0) {
        result.status = FeatureCacheStatus::Corrupt;
        result.message = "feature cache payload is malformed";
        return result;
    }

    loaded.info.schemaVersion = schemaVersion;
    loaded.info.extractorVersion = extractorVersion;
    loaded.info.analyzerVersion = analyzerVersion;
    loaded.info.contentHash = storedContentHash;
    loaded.info.analysisConfigHash = storedAnalysisHash;
    loaded.info.featureConfigHash = storedFeatureHash;
    loaded.info.vocalDetectorKind = static_cast<VocalDetectorKind>(vocalDetectorKind);
    loaded.info.vocalModelVersion = vocalModelVersion;
    loaded.info.vocalModelChecksum = vocalModelChecksum;
    loaded.info.harmonicAnalyzeVersion = harmonicAnalyzeVersion;
    loaded.info.sampleRate = sampleRate;
    loaded.info.sourceChannels = sourceChannels;
    loaded.info.duration = duration;

    if (!loaded.valid()) {
        result.status = FeatureCacheStatus::Corrupt;
        result.message = "feature cache payload failed validation";
        return result;
    }

    result.status = FeatureCacheStatus::Hit;
    result.features = std::move(loaded);
    result.message = "feature cache hit";
    return result;
}

bool MixFeatureCache::store(const MixFeatureMap& features,
                            const MixFeatureConfig& config,
                            std::string* error) const {
    if (!features.valid()) {
        if (error) {
            *error = "refusing to cache an invalid MixFeatureMap";
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
    writePayload(payloadWriter, features);
    const std::uint64_t payloadHash = hashBytes(payload.data(), payload.size());

    const std::uint64_t featureHash = config.fingerprint();
    std::vector<std::uint8_t> bytes;
    bytes.reserve(payload.size() + 128);
    Writer writer(bytes);
    writer.bytes(reinterpret_cast<const std::uint8_t*>(kMagic.data()), kMagic.size());
    writer.u32(kMixFeatureCacheFormatVersion);
    writer.u32(kMixFeatureSchemaVersion);
    writer.u32(kMixFeatureExtractorVersion);
    writer.u32(features.info.analyzerVersion);
    writer.u64(features.info.contentHash);
    writer.u64(features.info.analysisConfigHash);
    writer.u64(featureHash);
    writer.u32(static_cast<std::uint32_t>(config.vocalDetector));
    writer.u32(config.vocalModelVersion);
    writer.u64(config.vocalModelChecksum);
    writer.u32(config.harmonicAnalyzeVersion);
    writer.u32(features.info.sampleRate);
    writer.u32(features.info.sourceChannels);
    writer.f64(features.info.duration);
    writer.u32(static_cast<std::uint32_t>(payload.size()));
    writer.u64(payloadHash);
    writer.bytes(payload.data(), payload.size());

    const std::filesystem::path target =
        directory_ / fileNameFor(features.info.contentHash,
                                 features.info.analysisConfigHash,
                                 featureHash);
    static std::atomic<std::uint64_t> counter{0};
    std::filesystem::path temporary = target;
    temporary += ".tmp-" + std::to_string(counter.fetch_add(1, std::memory_order_relaxed));

    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) {
            if (error) {
                *error = "cannot create temporary feature cache file";
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
                *error = "failed to write feature cache file";
            }
            return false;
        }
    }

    std::filesystem::rename(temporary, target, ec);
    if (ec) {
        std::error_code removeError;
        (void)std::filesystem::remove(target, removeError);
        ec.clear();
        std::filesystem::rename(temporary, target, ec);
    }
    if (ec) {
        std::error_code removeError;
        (void)std::filesystem::remove(temporary, removeError);
        if (error) {
            *error = "cannot commit feature cache file: " + ec.message();
        }
        return false;
    }
    return true;
}

} // namespace bgmedia::mix
