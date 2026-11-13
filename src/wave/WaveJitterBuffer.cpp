#include "bgmedia/wave/WaveJitterBuffer.hpp"

#include <algorithm>
#include <cmath>

namespace bgmedia::wave {

WaveJitterBuffer::WaveJitterBuffer() : WaveJitterBuffer(Config{}) {}

WaveJitterBuffer::WaveJitterBuffer(Config config)
    : config_(config),
      slots_(std::max<std::size_t>(8, config.maxSlots)),
      targetDepthFrames_(std::clamp(config.minDepthFrames, std::size_t{1},
                                    slots_.size() / 2)) {}

void WaveJitterBuffer::reset() {
    for (auto& slot : slots_) {
        slot.occupied = false;
        slot.packet.payload.clear();
    }
    initialized_ = false;
    primed_ = false;
    nextPlaySeq_ = 0;
    highestSeq_ = 0;
    queuedCount_ = 0;
    targetDepthFrames_ = std::clamp(config_.minDepthFrames, std::size_t{1},
                                    slots_.size() / 2);
    hasPrevTiming_ = false;
    prevArrivalSec_ = 0.0;
    prevTimestamp48k_ = 0;
    jitterMs_ = 0.0;
    packetsReceived_ = 0;
    packetsLost_ = 0;
    packetsReordered_ = 0;
    packetsDroppedLate_ = 0;
    packetsDuplicate_ = 0;
}

void WaveJitterBuffer::updateJitterEstimate(std::uint32_t timestamp48k,
                                            double arrivalTimeSeconds) {
    if (!hasPrevTiming_) {
        hasPrevTiming_ = true;
        prevArrivalSec_ = arrivalTimeSeconds;
        prevTimestamp48k_ = timestamp48k;
        return;
    }
    const double arrivalDeltaMs =
        (arrivalTimeSeconds - prevArrivalSec_) * 1000.0;
    const auto tsDelta = static_cast<std::int32_t>(timestamp48k - prevTimestamp48k_);
    const double mediaDeltaMs =
        (static_cast<double>(tsDelta) / static_cast<double>(config_.sampleRate)) *
        1000.0;
    const double d = std::abs(arrivalDeltaMs - mediaDeltaMs);
    jitterMs_ += (d - jitterMs_) / 16.0;

    prevArrivalSec_ = arrivalTimeSeconds;
    prevTimestamp48k_ = timestamp48k;

    const double frameMs = static_cast<double>(
        std::max<std::uint32_t>(1, config_.frameDurationMs));
    const std::size_t desired = static_cast<std::size_t>(
        std::ceil((jitterMs_ * 2.0) / frameMs));
    targetDepthFrames_ = std::clamp(
        desired, config_.minDepthFrames,
        std::min(config_.maxDepthFrames, slots_.size() / 2));
}

void WaveJitterBuffer::push(WaveMediaPacket packet, double arrivalTimeSeconds) {
    ++packetsReceived_;
    updateJitterEstimate(packet.timestamp48k, arrivalTimeSeconds);

    if (!initialized_) {
        initialized_ = true;
        nextPlaySeq_ = packet.sequence;
        highestSeq_ = packet.sequence;
    }

    const auto offset =
        static_cast<std::int32_t>(packet.sequence - nextPlaySeq_);
    if (offset < 0) {
        ++packetsDroppedLate_;
        return;
    }

    if (static_cast<std::size_t>(offset) >= slots_.size()) {
        // Bounded O(capacity) resync on large sequence jumps (e.g. reconnect
        // or corrupted packet) so a ~1e9 jump never loops O(offset) times.
        if (static_cast<std::size_t>(offset) >= slots_.size() * 2) {
            for (auto& slot : slots_) {
                slot.occupied = false;
                slot.packet.payload.clear();
            }
            queuedCount_ = 0;
            primed_ = false;
            hasPrevTiming_ = false;
            jitterMs_ = 0.0;
            targetDepthFrames_ = std::clamp(config_.minDepthFrames, std::size_t{1}, slots_.size() / 2);
            nextPlaySeq_ = packet.sequence;
        } else {
            const std::size_t advance =
                static_cast<std::size_t>(offset) - (slots_.size() / 2);
            for (std::size_t i = 0; i < advance; ++i) {
                advancePlaySeq();
            }
        }
    }

    const std::size_t idx = packet.sequence % slots_.size();
    if (slots_[idx].occupied && slots_[idx].packet.sequence == packet.sequence) {
        ++packetsDuplicate_;
        return;
    }

    const auto diffFromHighest =
        static_cast<std::int32_t>(packet.sequence - highestSeq_);
    if (diffFromHighest < 0) {
        ++packetsReordered_;
    } else {
        highestSeq_ = packet.sequence;
    }
    if (!slots_[idx].occupied) {
        ++queuedCount_;
    }
    slots_[idx].occupied = true;
    slots_[idx].packet = std::move(packet);

    if (!primed_ && queuedCount_ >= targetDepthFrames_) {
        primed_ = true;
    }
}

void WaveJitterBuffer::advancePlaySeq() noexcept {
    const std::size_t idx = nextPlaySeq_ % slots_.size();
    if (slots_[idx].occupied) {
        slots_[idx].occupied = false;
        slots_[idx].packet.payload.clear();
        if (queuedCount_ > 0) {
            --queuedCount_;
        }
    }
    ++nextPlaySeq_;
}

WaveJitterBuffer::PopStatus WaveJitterBuffer::popNext(
    WaveMediaPacket& outPacket) {
    if (!initialized_ || !primed_) {
        return PopStatus::Buffering;
    }

    const std::size_t idx = nextPlaySeq_ % slots_.size();
    if (slots_[idx].occupied && slots_[idx].packet.sequence == nextPlaySeq_) {
        outPacket = std::move(slots_[idx].packet);
        slots_[idx].occupied = false;
        if (queuedCount_ > 0) {
            --queuedCount_;
        }
        ++nextPlaySeq_;
        return PopStatus::Packet;
    }

    if (queuedCount_ == 0) {
        primed_ = false;
        return PopStatus::Buffering;
    }

    // A later packet is already queued: declare this frame lost so the caller
    // can run Packet Loss Concealment (PLC).
    ++packetsLost_;
    ++nextPlaySeq_;
    return PopStatus::MissingPlc;
}

} // namespace bgmedia::wave
