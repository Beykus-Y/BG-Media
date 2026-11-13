#include "bgmedia/wave/WaveAudioPipeline.hpp"
#include "bgmedia/wave/WaveCodec.hpp"
#include "bgmedia/wave/WaveSignaling.hpp"
#include "bgmedia/wave/WebRtcWaveTransport.hpp"

#ifdef BGMEDIA_HAVE_WAVE_OPUS
#include "bgmedia/wave/OpusWaveCodec.hpp"
#endif

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <ctime>
#include <iostream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

std::shared_ptr<bgmedia::wave::IWaveAudioCodec> makeCodec() {
#ifdef BGMEDIA_HAVE_WAVE_OPUS
    auto opus = std::make_shared<bgmedia::wave::OpusWaveCodec>(
        bgmedia::wave::kWaveDefaultBitrateBps);
    if (opus->isReady()) {
        return opus;
    }
#endif
    return std::make_shared<bgmedia::wave::Pcm16WaveCodec>();
}

double nowSec(std::chrono::steady_clock::time_point t0) {
    return std::chrono::duration<double>(std::chrono::steady_clock::now() - t0)
        .count();
}

} // namespace

int main(int argc, char** argv) {
    using namespace bgmedia::wave;

    std::string roleStr = "host";
    int durationMs = 1500;
    std::vector<std::string> stunServers;
    bool enableIpv6 = true;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--role" && i + 1 < argc) {
            roleStr = argv[++i];
        } else if (arg == "--duration-ms" && i + 1 < argc) {
            durationMs = std::stoi(argv[++i]);
        } else if (arg == "--stun" && i + 1 < argc) {
            stunServers.emplace_back(argv[++i]);
        } else if (arg == "--no-ipv6") {
            enableIpv6 = false;
        }
    }

    const WaveRole role =
        (roleStr == "listener") ? WaveRole::Listener : WaveRole::Host;

    WebRtcTransportConfig txCfg;
    txCfg.role = role;
    txCfg.stunServers = stunServers;
    txCfg.enableIpv6 = enableIpv6;
    txCfg.gatheringTimeoutMs = 5000;
    txCfg.connectTimeoutMs = 10000;

    auto transport = std::make_shared<WebRtcWaveTransport>(txCfg);
    WavePipelineConfig pCfg;
    pCfg.role = role;
    WaveAudioPipeline pipeline(pCfg, makeCodec(), transport);

    std::atomic<bool> live{false};
    std::atomic<bool> failed{false};

    WavePipelineCallbacks cb;
    cb.onOutboundSignal = [&](const WaveSignalMessage& msg) {
        std::string err;
        const std::string uri =
            encodeWaveHttpsFragmentUri(msg, "https://beygram.beykus.fun/", &err);
        if (uri.empty()) {
            std::cerr << "encode error: " << err << "\n";
            failed.store(true);
            return;
        }
        if (msg.type == WaveSignalType::Offer) {
            std::cout << "OFFER " << uri << std::endl;
        } else if (msg.type == WaveSignalType::Answer) {
            std::cout << "ANSWER " << uri << std::endl;
        }
    };
    cb.onStateChanged = [&](WaveSessionState st, const std::string&) {
        if (st == WaveSessionState::LiveP2P) {
            live.store(true);
        } else if (st == WaveSessionState::P2PFailed ||
                   st == WaveSessionState::Failed) {
            failed.store(true);
        }
    };
    pipeline.setCallbacks(std::move(cb));

    const auto t0 = std::chrono::steady_clock::now();
    const std::int64_t unixNow = static_cast<std::int64_t>(std::time(nullptr));

    if (role == WaveRole::Host) {
        if (!pipeline.startOffer(unixNow, 120)) {
            std::cerr << "startOffer failed\n";
            return 1;
        }
        std::string line;
        if (!std::getline(std::cin, line) || line.rfind("ANSWER ", 0) != 0) {
            std::cerr << "expected ANSWER line\n";
            return 1;
        }
        WaveSignalMessage answer;
        std::string err;
        if (!decodeWaveUri(line.substr(7), answer, &err)) {
            std::cerr << "decode answer failed: " << err << "\n";
            return 1;
        }
        if (!pipeline.acceptAnswer(answer, unixNow + 1)) {
            std::cerr << "acceptAnswer failed\n";
            return 1;
        }

        const auto waitDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!live.load() && !failed.load() &&
               std::chrono::steady_clock::now() < waitDeadline) {
            pipeline.process(nowSec(t0));
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!live.load()) {
            std::cerr << "host failed to reach LiveP2P\n";
            return 2;
        }

        const int ticks = std::max(1, durationMs / 10);
        std::vector<float> pcm(480 * 2);
        double phase = 0.0;
        for (int t = 0; t < ticks; ++t) {
            for (std::size_t i = 0; i < 480; ++i) {
                const float sL = static_cast<float>(0.4 * std::sin(phase));
                const float sR =
                    static_cast<float>(0.4 * std::sin(phase * 1.5));
                pcm[i * 2] = sL;
                pcm[i * 2 + 1] = sR;
                phase += 2.0 * 3.14159265358979323846 * 440.0 / 48000.0;
            }
            pipeline.onPcmRendered(pcm.data(), 480, 48000);
            pipeline.process(nowSec(t0));
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const auto st = pipeline.stats();
        const bool fpVerified = transport->remoteFingerprintVerified();
        const std::string candPair = transport->selectedCandidatePairSummary();
        pipeline.stop(true, unixNow + 10);
        std::cout << "STATS packetsSent=" << st.packetsSent
                  << " bytesSent=" << st.bytesSent
                  << " fingerprintVerified=" << (fpVerified ? 1 : 0)
                  << " candidatePair=" << candPair
                  << std::endl;
        return (st.packetsSent > 0 && fpVerified) ? 0 : 3;
    } else {
        std::string line;
        if (!std::getline(std::cin, line) || line.rfind("OFFER ", 0) != 0) {
            std::cerr << "expected OFFER line\n";
            return 1;
        }
        WaveSignalMessage offer;
        std::string err;
        if (!decodeWaveUri(line.substr(6), offer, &err)) {
            std::cerr << "decode offer failed: " << err << "\n";
            return 1;
        }
        if (!pipeline.acceptOffer(offer, unixNow)) {
            std::cerr << "acceptOffer failed\n";
            return 1;
        }

        const auto waitDeadline =
            std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!live.load() && !failed.load() &&
               std::chrono::steady_clock::now() < waitDeadline) {
            pipeline.process(nowSec(t0));
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        if (!live.load()) {
            std::cerr << "listener failed to reach LiveP2P\n";
            return 2;
        }

        const int ticks = std::max(1, (durationMs + 400) / 10);
        std::vector<float> out(480 * 2);
        std::size_t nonSilentFrames = 0;
        double sumSq = 0.0;
        std::size_t sampleCount = 0;
        for (int t = 0; t < ticks; ++t) {
            pipeline.process(nowSec(t0));
            const std::size_t got =
                pipeline.pullListenerPcm(out.data(), 480, 48000);
            if (got > 0) {
                for (std::size_t i = 0; i < got * 2; ++i) {
                    sumSq += static_cast<double>(out[i]) * out[i];
                    if (std::abs(out[i]) > 1e-3f) {
                        ++nonSilentFrames;
                    }
                }
                sampleCount += got * 2;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const auto st = pipeline.stats();
        const bool fpVerified = transport->remoteFingerprintVerified();
        const std::string candPair = transport->selectedCandidatePairSummary();
        pipeline.stop(false, unixNow + 10);
        const double rms = sampleCount > 0 ? std::sqrt(sumSq / sampleCount) : 0.0;
        std::cout << "STATS packetsReceived=" << st.packetsReceived
                  << " nonSilentSamples=" << nonSilentFrames
                  << " rms=" << rms
                  << " fingerprintVerified=" << (fpVerified ? 1 : 0)
                  << " candidatePair=" << candPair
                  << std::endl;
        return (st.packetsReceived > 0 && nonSilentFrames > 0 && fpVerified)
                   ? 0
                   : 3;
    }
}
