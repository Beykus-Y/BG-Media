#pragma once

#include "bgmedia/wave/WaveTypes.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace bgmedia::wave {

// Bounded adaptive jitter buffer for 20 ms BG Wave media packets.
// Handles packet reordering, duplicate suppression, late-packet drops,
// RFC 3550-style EWMA jitter estimation, and PLC signaling on missing frames.
class WaveJitterBuffer {
public:
    enum class PopStatus {
        Buffering = 0,
        Packet,
        MissingPlc
    };

    struct Config {
        std::size_t maxSlots = 64;          // bounded capacity (1.28 s @ 20 ms)
        std::size_t minDepthFrames = 2;     // 40 ms minimum target depth
        std::size_t maxDepthFrames = 10;    // 200 ms maximum target depth
        std::uint32_t frameDurationMs = kWaveFrameDurationMs;
        std::uint32_t sampleRate = kWaveSampleRate;
    };

    WaveJitterBuffer();
    explicit WaveJitterBuffer(Config config);

    void reset();

    void push(WaveMediaPacket packet, double arrivalTimeSeconds);
    [[nodiscard]] PopStatus popNext(WaveMediaPacket& outPacket);

    [[nodiscard]] std::size_t queuedCount() const noexcept { return queuedCount_; }
    [[nodiscard]] std::size_t targetDepthFrames() const noexcept {
        return targetDepthFrames_;
    }
    [[nodiscard]] double jitterMs() const noexcept { return jitterMs_; }
    [[nodiscard]] std::uint64_t packetsReceived() const noexcept {
        return packetsReceived_;
    }
    [[nodiscard]] std::uint64_t packetsLost() const noexcept {
        return packetsLost_;
    }
    [[nodiscard]] std::uint64_t packetsReordered() const noexcept {
        return packetsReordered_;
    }
    [[nodiscard]] std::uint64_t packetsDroppedLate() const noexcept {
        return packetsDroppedLate_;
    }
    [[nodiscard]] std::uint64_t packetsDuplicate() const noexcept {
        return packetsDuplicate_;
    }

private:
    struct Slot {
        bool occupied = false;
        WaveMediaPacket packet;
    };

    void updateJitterEstimate(std::uint32_t timestamp48k,
                              double arrivalTimeSeconds);
    void advancePlaySeq() noexcept;

    Config config_;
    std::vector<Slot> slots_;
    bool initialized_ = false;
    bool primed_ = false;
    std::uint32_t nextPlaySeq_ = 0;
    std::uint32_t highestSeq_ = 0;
    std::size_t queuedCount_ = 0;
    std::size_t targetDepthFrames_ = 2;

    bool hasPrevTiming_ = false;
    double prevArrivalSec_ = 0.0;
    std::uint32_t prevTimestamp48k_ = 0;
    double jitterMs_ = 0.0;

    std::uint64_t packetsReceived_ = 0;
    std::uint64_t packetsLost_ = 0;
    std::uint64_t packetsReordered_ = 0;
    std::uint64_t packetsDroppedLate_ = 0;
    std::uint64_t packetsDuplicate_ = 0;
};

} // namespace bgmedia::wave
