#include "framework.hpp"

#include "bgmedia/mix/MixEngine.hpp"
#include "bgmedia/mix/PcmTrack.hpp"
#include "bgmedia/wave/WaveAudioPipeline.hpp"
#include "bgmedia/wave/WaveCodec.hpp"
#include "bgmedia/wave/WaveFallback.hpp"
#include "bgmedia/wave/WaveJitterBuffer.hpp"
#include "bgmedia/wave/WaveResampler.hpp"
#include "bgmedia/wave/WaveRingBuffer.hpp"
#include "bgmedia/wave/WaveSignaling.hpp"
#include "bgmedia/wave/WaveTransport.hpp"

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <limits>
#include <memory>
#include <thread>
#include <vector>

namespace {

std::shared_ptr<bgmedia::mix::PcmTrack> makeToneTrack(double freqHz,
                                                      double seconds,
                                                      std::uint32_t rate) {
    auto track = std::make_shared<bgmedia::mix::PcmTrack>();
    track->sampleRate = rate;
    track->channels = 2;
    const std::size_t frames = static_cast<std::size_t>(seconds * rate);
    track->samples.resize(frames * 2);
    for (std::size_t i = 0; i < frames; ++i) {
        const double t = static_cast<double>(i) / static_cast<double>(rate);
        const float s = 0.4f * static_cast<float>(std::sin(2.0 * 3.14159265358979323846 * freqHz * t));
        track->samples[i * 2] = s;
        track->samples[i * 2 + 1] = s * 0.8f;
    }
    return track;
}

class FakeExternalCallTransport final : public bgmedia::wave::IExternalCallTransport {
public:
    explicit FakeExternalCallTransport(bool failStart = false)
        : failStart_(failStart) {}

    void setCallbacks(bgmedia::wave::ExternalCallCallbacks callbacks) override {
        callbacks_ = std::move(callbacks);
    }

    bool start() override {
        if (failStart_) {
            state_ = bgmedia::wave::ExternalCallState::Failed;
            if (callbacks_.onStateChanged) {
                callbacks_.onStateChanged(state_, "call rejected");
            }
            return false;
        }
        state_ = bgmedia::wave::ExternalCallState::Live;
        if (callbacks_.onStateChanged) {
            callbacks_.onStateChanged(state_, "telegram call live");
        }
        return true;
    }

    void stop() override {
        state_ = bgmedia::wave::ExternalCallState::Ended;
        if (callbacks_.onStateChanged) {
            callbacks_.onStateChanged(state_, "call ended");
        }
    }

    std::size_t pushOutgoingPcm(const float* interleavedStereo,
                                std::size_t frames,
                                std::uint32_t /*sampleRate*/) override {
        if (state_ != bgmedia::wave::ExternalCallState::Live ||
            !interleavedStereo) {
            return 0;
        }
        pcm_.insert(pcm_.end(), interleavedStereo,
                    interleavedStereo + frames * 2);
        return frames;
    }

    std::size_t pullIncomingPcm(float* interleavedStereo,
                                std::size_t frames,
                                std::uint32_t /*sampleRate*/) override {
        if (state_ != bgmedia::wave::ExternalCallState::Live ||
            !interleavedStereo) {
            return 0;
        }
        const std::size_t avail = pcm_.size() / 2;
        const std::size_t take = std::min(frames, avail);
        if (take > 0) {
            std::memcpy(interleavedStereo, pcm_.data(),
                        take * 2 * sizeof(float));
            pcm_.erase(pcm_.begin(),
                       pcm_.begin() + static_cast<std::ptrdiff_t>(take * 2));
        }
        return take;
    }

    bgmedia::wave::ExternalCallState state() const noexcept override {
        return state_;
    }

private:
    bool failStart_ = false;
    bgmedia::wave::ExternalCallState state_ =
        bgmedia::wave::ExternalCallState::Idle;
    bgmedia::wave::ExternalCallCallbacks callbacks_;
    std::vector<float> pcm_;
};

} // namespace

TEST_CASE(wave.ring_buffer_bounds_and_sanitize) {
    using namespace bgmedia::wave;
    WavePcmRingBuffer ring(1024);
    CHECK((ring.capacityFrames()) == (1024u));

    std::vector<float> badIn = {
        std::numeric_limits<float>::quiet_NaN(), 2.5f,
        -3.0f, std::numeric_limits<float>::infinity()
    };
    CHECK((ring.push(badIn.data(), 2, 48000, true)) == (2u));
    CHECK((ring.clippedSamples()) == (4u));

    float out[4]{};
    CHECK((ring.pop(out, 2)) == (2u));
    CHECK_NEAR(out[0], 0.0f, 1e-6f);
    CHECK_NEAR(out[1], 1.0f, 1e-6f);
    CHECK_NEAR(out[2], -1.0f, 1e-6f);
    CHECK_NEAR(out[3], 0.0f, 1e-6f);

    // Underrun fills silence
    CHECK((ring.pop(out, 2)) == (0u));
    CHECK((ring.underruns()) == (1u));
    CHECK_NEAR(out[0], 0.0f, 1e-6f);

    // Overrun drops excess without blocking
    std::vector<float> big(1200 * 2, 0.25f);
    const std::size_t written = ring.push(big.data(), 1200, 48000, true);
    CHECK((written) == (1024u));
    CHECK((ring.overruns()) == (1u));
}

TEST_CASE(wave.resampler_44100_48000) {
    using namespace bgmedia::wave;
    WaveStereoResampler resampler;
    resampler.configure(44100, 48000);
    CHECK(!resampler.isBypass());

    const std::size_t inFrames = 4410; // 100 ms @ 44.1 kHz
    std::vector<float> in(inFrames * 2);
    for (std::size_t i = 0; i < inFrames; ++i) {
        const float v = 0.5f * std::sin(2.0f * 3.14159265f * 440.0f *
                                       static_cast<float>(i) / 44100.0f);
        in[i * 2] = v;
        in[i * 2 + 1] = -v;
    }
    std::vector<float> out;
    resampler.process(in, out);
    const std::size_t outFrames = out.size() / 2;
    // Expect ~4800 frames (+/- small cubic lookahead window)
    CHECK(outFrames > 4700 && outFrames < 4850);

    double energy = 0.0;
    for (float s : out) {
        energy += static_cast<double>(s) * s;
    }
    CHECK(energy > 100.0);
}

TEST_CASE(wave.jitter_buffer_reorder_and_loss) {
    using namespace bgmedia::wave;
    WaveJitterBuffer::Config cfg;
    cfg.minDepthFrames = 2;
    cfg.maxDepthFrames = 8;
    WaveJitterBuffer jb(cfg);

    auto makePkt = [](std::uint32_t seq) {
        WaveMediaPacket p;
        p.sequence = seq;
        p.timestamp48k = seq * 960;
        p.payload = {static_cast<std::uint8_t>(seq & 0xFFu)};
        return p;
    };

    // Start near uint32_t wraparound, deliver out-of-order and drop seq+2
    const std::uint32_t base = 0xFFFFFFFEu;
    jb.push(makePkt(base + 0), 0.000);
    jb.push(makePkt(base + 3), 0.021);
    jb.push(makePkt(base + 1), 0.040); // reordered!
    jb.push(makePkt(base + 1), 0.041); // duplicate!

    CHECK((jb.packetsReordered()) == (1u));
    CHECK((jb.packetsDuplicate()) == (1u));

    WaveMediaPacket out;
    CHECK((static_cast<int>(jb.popNext(out))) == (static_cast<int>(WaveJitterBuffer::PopStatus::Packet)));
    CHECK((out.sequence) == (base + 0));

    CHECK((static_cast<int>(jb.popNext(out))) == (static_cast<int>(WaveJitterBuffer::PopStatus::Packet)));
    CHECK((out.sequence) == (base + 1));

    // base + 2 was lost; base + 3 is queued -> MissingPlc
    CHECK((static_cast<int>(jb.popNext(out))) == (static_cast<int>(WaveJitterBuffer::PopStatus::MissingPlc)));
    CHECK((jb.packetsLost()) == (1u));

    CHECK((static_cast<int>(jb.popNext(out))) == (static_cast<int>(WaveJitterBuffer::PopStatus::Packet)));
    CHECK((out.sequence) == (base + 3));
}

TEST_CASE(wave.signaling_v1_codec_and_uris) {
    using namespace bgmedia::wave;

    const std::string sampleSdp =
        "v=0\r\n"
        "o=- 12345 2 IN IP4 127.0.0.1\r\n"
        "s=-\r\n"
        "t=0 0\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "a=ice-ufrag:AbCdEf12\r\n"
        "a=ice-pwd:superSecretIcePassword123456\r\n"
        "a=fingerprint:sha-256 "
        "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:"
        "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99\r\n"
        "a=setup:actpass\r\n"
        "a=candidate:1 1 udp 2122260223 192.168.1.42 54321 typ host\r\n"
        "a=candidate:2 1 udp 2122194687 2001:db8::1 54322 typ host\r\n"
        "a=candidate:3 1 udp 1686052607 a1b2c3d4-e5f6.local 54323 typ host\r\n"
        "a=candidate:4 1 tcp 1518280447 203.0.113.99 9 typ srflx tcptype active\r\n";

    WaveSdpSignal parsed;
    std::string err;
    CHECK(parseSdpToWaveSignal(sampleSdp, parsed, &err));
    CHECK((parsed.iceUfrag) == ("AbCdEf12"));
    CHECK((parsed.candidates.size()) == (4u));
    CHECK((parsed.candidates[0].address) == ("192.168.1.42"));
    CHECK((parsed.candidates[1].address) == ("2001:db8::1"));
    CHECK((parsed.candidates[2].address) == ("a1b2c3d4-e5f6.local"));
    CHECK((parsed.candidates[3].protocol) == (WaveIceProtocol::Tcp));
    CHECK((parsed.candidates[3].tcpType) == (WaveIceTcpType::Active));

    WaveSignalMessage msg;
    msg.version = 1;
    msg.type = WaveSignalType::Offer;
    msg.sid = generateWaveSessionId();
    msg.iat = 1763000000;
    msg.exp = 1763000120;
    msg.caps = kWaveCapOpus48kStereo | kWaveCapDtlsSrtp;
    msg.nonce = 42;
    msg.signal = parsed;

    const std::string internalUri = encodeWaveUri(msg, &err);
    CHECK(!internalUri.empty());
    CHECK(internalUri.starts_with("bg://wave/v1/"));

    const std::string httpsUri =
        encodeWaveHttpsFragmentUri(msg, "https://beygram.beykus.fun/", &err);
    CHECK(!httpsUri.empty());
    CHECK(httpsUri.starts_with("https://beygram.beykus.fun/#bgwave-v1."));

    WaveSignalMessage decoded;
    CHECK(decodeWaveUri(httpsUri, decoded, &err));
    CHECK(decoded.sid == msg.sid);
    CHECK((decoded.nonce) == (42u));
    CHECK((decoded.signal.candidates.size()) == (4u));
    CHECK((decoded.signal.candidates[1].address) == ("2001:db8::1"));
    CHECK((decoded.signal.candidates[2].address) == ("a1b2c3d4-e5f6.local"));

    // Redaction must not leak IPs or secrets
    const std::string diag = redactWaveSignalForDiagnostics(decoded);
    CHECK(diag.find("192.168.1.42") == std::string::npos);
    CHECK(diag.find("superSecretIcePassword") == std::string::npos);
    CHECK(diag.find("<redacted>") != std::string::npos);

    // Unknown version and corrupt inputs must fail cleanly without crashing
    std::vector<std::uint8_t> raw;
    CHECK(encodeWaveSignalBinary(msg, raw, &err));
    raw[2] = 99; // unknown version
    WaveSignalMessage bad;
    CHECK(!decodeWaveSignalBinary(raw, bad, &err));

    raw[2] = 1;
    raw.resize(10); // truncated
    CHECK(!decodeWaveSignalBinary(raw, bad, &err));

    CHECK(!decodeWaveUri("https://beygram.beykus.fun/#bgwave-v2.AAAA", bad, &err));
    CHECK(!validateWaveSignal(msg, 1763000200, &err)); // expired
}

TEST_CASE(wave.pipeline_state_machine_and_fallback) {
    using namespace bgmedia::wave;

    SimulatedNetworkProfile failProf;
    failProf.failOnConnect = true;
    auto hostTx = std::make_shared<SimulatedWaveTransport>(failProf);
    auto listenerTx = std::make_shared<SimulatedWaveTransport>(failProf);
    SimulatedWaveTransport::linkPeers(*hostTx, *listenerTx);

    WavePipelineConfig hCfg;
    hCfg.role = WaveRole::Host;
    WaveAudioPipeline host(hCfg, std::make_shared<Pcm16WaveCodec>(), hostTx);

    WavePipelineConfig lCfg;
    lCfg.role = WaveRole::Listener;
    WaveAudioPipeline listener(lCfg, std::make_shared<Pcm16WaveCodec>(),
                               listenerTx);

    WaveSignalMessage capturedOffer;
    bool gotOffer = false;
    WavePipelineCallbacks hCb;
    hCb.onOutboundSignal = [&](const WaveSignalMessage& m) {
        if (m.type == WaveSignalType::Offer) {
            capturedOffer = m;
            gotOffer = true;
        }
    };
    host.setCallbacks(std::move(hCb));

    CHECK(host.startOffer(1000, 120));
    CHECK(gotOffer);
    CHECK((host.state()) == (WaveSessionState::AwaitingAnswer));

    // Listener fails to connect P2P -> enters P2PFailed
    CHECK(!listener.acceptOffer(capturedOffer, 1001));
    CHECK((listener.state()) == (WaveSessionState::P2PFailed));

    // Host forces failure if reconnect fails, or test fallback on listener:
    auto extCall = std::make_shared<FakeExternalCallTransport>(false);
    CHECK(listener.switchToExternalCallFallback(extCall));
    CHECK((listener.state()) == (WaveSessionState::LiveTelegramCall));
    CHECK((listener.route()) == (WaveTransportRoute::ExternalCall));

    // Instant publishing revocation
    host.setPublishingEnabled(false);
    CHECK(!host.publishingEnabled());
    float dummy[1920]{};
    host.pushHostPcm(dummy, 960, 48000);
    CHECK((host.stats().packetsSent) == (0u));
}

TEST_CASE(wave.mix_engine_loopback_end_to_end) {
    using namespace bgmedia::mix;
    using namespace bgmedia::wave;

    SimulatedNetworkProfile netProf;
    netProf.baseLatencyMs = 15.0;
    netProf.jitterMs = 8.0;
    netProf.lossPercent = 5.0;
    netProf.reorderPercent = 10.0;

    auto hostTx = std::make_shared<SimulatedWaveTransport>(netProf);
    auto listenerTx = std::make_shared<SimulatedWaveTransport>(netProf);
    SimulatedWaveTransport::linkPeers(*hostTx, *listenerTx);

    WavePipelineConfig hCfg;
    hCfg.role = WaveRole::Host;
    auto host = std::make_shared<WaveAudioPipeline>(
        hCfg, std::make_shared<Pcm16WaveCodec>(), hostTx);

    WavePipelineConfig lCfg;
    lCfg.role = WaveRole::Listener;
    auto listener = std::make_shared<WaveAudioPipeline>(
        lCfg, std::make_shared<Pcm16WaveCodec>(), listenerTx);

    WaveSignalMessage offerMsg;
    WaveSignalMessage answerMsg;
    WavePipelineCallbacks hCb;
    hCb.onOutboundSignal = [&](const WaveSignalMessage& m) {
        if (m.type == WaveSignalType::Offer) offerMsg = m;
    };
    host->setCallbacks(std::move(hCb));

    WavePipelineCallbacks lCb;
    lCb.onOutboundSignal = [&](const WaveSignalMessage& m) {
        if (m.type == WaveSignalType::Answer) answerMsg = m;
    };
    listener->setCallbacks(std::move(lCb));

    CHECK(host->startOffer(1000));
    CHECK(listener->acceptOffer(offerMsg, 1001));
    CHECK(host->acceptAnswer(answerMsg, 1002));
    CHECK((host->state()) == (WaveSessionState::LiveP2P));
    CHECK((listener->state()) == (WaveSessionState::LiveP2P));

    MixEngine engine(48000);
    engine.setPcmTap(host);
    CHECK(engine.loadDeck(DeckId::A, makeToneTrack(220.0, 4.0, 48000), 1, 120.0));
    CHECK(engine.loadDeck(DeckId::B, makeToneTrack(440.0, 4.0, 48000), 2, 120.0));
    engine.play(DeckId::A);

    MixPlan plan;
    plan.valid = true;
    plan.type = TransitionType::Crossfade;
    plan.requestedType = TransitionType::Crossfade;
    plan.from = DeckId::A;
    plan.to = DeckId::B;
    plan.transitionStartA = 0.4;
    plan.transitionEndA = 1.2;
    plan.duration = 0.8;
    plan.startB = 0.0;
    plan.endB = 0.8;
    CHECK(engine.startTransition(plan));

    // Pump 1.6 seconds in 512-frame callbacks (~10.67 ms each)
    constexpr std::size_t kBlock = 512;
    std::vector<float> hostRender(kBlock * 2);
    std::vector<float> listenerOut(kBlock * 2);
    double listenerEnergy = 0.0;
    double simTime = 0.0;
    for (int step = 0; step < 150; ++step) {
        engine.render(hostRender.data(), kBlock);
        host->process(simTime);
        listener->process(simTime);
        listener->pullListenerPcm(listenerOut.data(), kBlock, 48000);
        for (float s : listenerOut) {
            listenerEnergy += static_cast<double>(s) * s;
        }
        simTime += static_cast<double>(kBlock) / 48000.0;
    }

    const WaveStats hs = host->stats();
    const WaveStats ls = listener->stats();
    CHECK(hs.packetsSent > 50);
    CHECK(ls.packetsReceived > 40);
    CHECK(listenerEnergy > 50.0);

    engine.setPcmTap(nullptr);
    engine.reclaimRetired();
    host->stop(true, 1005);
    CHECK((host->state()) == (WaveSessionState::Ended));
}

TEST_CASE(wave.golden_fixtures_v1) {
    using namespace bgmedia::wave;
    const std::filesystem::path dir =
        std::filesystem::path(BGMEDIA_TEST_FIXTURE_DIR) / "wave_v1";
    auto readBin = [&](const char* name) {
        std::ifstream f(dir / name, std::ios::binary);
        CHECK(f.good());
        return std::vector<std::uint8_t>((std::istreambuf_iterator<char>(f)),
                                         std::istreambuf_iterator<char>());
    };
    auto readTxt = [&](const char* name) {
        std::ifstream f(dir / name, std::ios::binary);
        CHECK(f.good());
        std::ostringstream oss;
        oss << f.rdbuf();
        return oss.str();
    };

    const char* validNames[] = {"offer", "answer", "cancel", "end"};
    for (const char* name : validNames) {
        const std::string binName = std::string(name) + ".bin";
        const std::string uriName = std::string(name) + ".uri.txt";
        const std::vector<std::uint8_t> expectedBin = readBin(binName.c_str());
        WaveSignalMessage msg;
        std::string err;
        CHECK(decodeWaveSignalBinary(expectedBin, msg, &err));
        std::vector<std::uint8_t> reencoded;
        CHECK(encodeWaveSignalBinary(msg, reencoded, &err));
        CHECK(reencoded == expectedBin);

        std::istringstream uris(readTxt(uriName.c_str()));
        std::string bgUri, httpsUri;
        CHECK(static_cast<bool>(std::getline(uris, bgUri)));
        CHECK(static_cast<bool>(std::getline(uris, httpsUri)));
        if (!bgUri.empty() && bgUri.back() == '\r') bgUri.pop_back();
        if (!httpsUri.empty() && httpsUri.back() == '\r') httpsUri.pop_back();
        CHECK(encodeWaveUri(msg, &err) == bgUri);
        CHECK(encodeWaveHttpsFragmentUri(msg, "https://beygram.beykus.fun/", &err) == httpsUri);
        WaveSignalMessage fromBg, fromHttps;
        CHECK(decodeWaveUri(bgUri, fromBg, &err));
        CHECK(decodeWaveUri(httpsUri, fromHttps, &err));
        CHECK(fromBg.sid == msg.sid);
        CHECK(fromHttps.nonce == msg.nonce);
    }

    const char* invalidNames[] = {
        "invalid_bad_magic.bin",
        "invalid_unknown_version.bin",
        "invalid_truncated.bin",
        "invalid_trailing_bytes.bin",
        "invalid_zero_sid.bin"
    };
    for (const char* badName : invalidNames) {
        const std::vector<std::uint8_t> badBytes = readBin(badName);
        WaveSignalMessage badMsg;
        std::string err;
        CHECK(!decodeWaveSignalBinary(badBytes, badMsg, &err));
    }

    const std::string sdp = readTxt("sanitized_webrtc_offer.sdp");
    WaveSdpSignal sdpSig;
    std::string err;
    CHECK(parseSdpToWaveSignal(sdp, sdpSig, &err));
    CHECK(sdpSig.candidates.size() == 4u);
    CHECK(sdpSig.candidates[3].type == WaveIceCandidateType::Srflx);
}

TEST_CASE(wave.ttl_and_replay_guard) {
    using namespace bgmedia::wave;
    auto hostTx = std::make_shared<SimulatedWaveTransport>();
    auto listTx = std::make_shared<SimulatedWaveTransport>();
    SimulatedWaveTransport::linkPeers(*hostTx, *listTx);

    WavePipelineConfig hCfg; hCfg.role = WaveRole::Host;
    WaveAudioPipeline host(hCfg, std::make_shared<Pcm16WaveCodec>(), hostTx);
    WavePipelineConfig lCfg; lCfg.role = WaveRole::Listener;
    WaveAudioPipeline listener(lCfg, std::make_shared<Pcm16WaveCodec>(), listTx);

    WaveSignalMessage offer, answer;
    WavePipelineCallbacks hCb;
    hCb.onOutboundSignal = [&](const WaveSignalMessage& m) {
        if (m.type == WaveSignalType::Offer) offer = m;
    };
    host.setCallbacks(std::move(hCb));
    WavePipelineCallbacks lCb;
    lCb.onOutboundSignal = [&](const WaveSignalMessage& m) {
        if (m.type == WaveSignalType::Answer) answer = m;
    };
    listener.setCallbacks(std::move(lCb));

    CHECK(host.startOffer(1000, 60));
    // Expired offer must be rejected
    CHECK(!listener.acceptOffer(offer, 1061));
    // Valid offer within TTL succeeds
    CHECK(listener.acceptOffer(offer, 1010));
    CHECK(host.acceptAnswer(answer, 1011));

    // Replay of End with stale nonce <= lastRemoteNonce (answer.nonce == 1) must be rejected
    WaveSignalMessage staleEnd;
    staleEnd.version = 1;
    staleEnd.type = WaveSignalType::End;
    staleEnd.sid = offer.sid;
    staleEnd.iat = 1012;
    staleEnd.exp = 1072;
    staleEnd.nonce = answer.nonce;
    CHECK(!host.handleRemoteSignal(staleEnd, 1012));
    CHECK(host.state() == WaveSessionState::LiveP2P);

    // Fresh nonce succeeds
    staleEnd.nonce = answer.nonce + 1;
    CHECK(host.handleRemoteSignal(staleEnd, 1012));
    CHECK(host.state() == WaveSessionState::Ended);
}
