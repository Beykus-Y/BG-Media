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

#include <algorithm>
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

TEST_CASE(wave.signaling_field_injection_and_sdp_hardening) {
    using namespace bgmedia::wave;

    auto makeValidOffer = []() {
        WaveSignalMessage m;
        m.version = 1;
        m.type = WaveSignalType::Offer;
        m.sid = generateWaveSessionId();
        m.iat = 1763000000;
        m.exp = 1763000120;
        m.nonce = 1;
        m.signal.iceUfrag = "AbCd12";
        m.signal.icePwd = "secretPwd12345678";
        m.signal.dtlsFingerprintAlg = "sha-256";
        m.signal.dtlsFingerprint =
            "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:"
            "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99";
        m.signal.setupRole = WaveDtlsSetupRole::Actpass;
        m.signal.candidates = {
            {"1", 1, WaveIceProtocol::Udp, 2122260223u, "192.0.2.10", 50000,
             WaveIceCandidateType::Host, WaveIceTcpType::None}
        };
        return m;
    };

    std::string err;
    std::vector<std::uint8_t> bin;

    // 1. Reject control characters and whitespace (SDP injection) across fields
    {
        auto m = makeValidOffer();
        m.signal.iceUfrag = "u\r\na=evil:1";
        CHECK(!validateWaveSignal(m, 0, &err));
        CHECK(!encodeWaveSignalBinary(m, bin, &err));
        CHECK(formatWaveSignalToSdp(m.signal, true).empty());
    }
    {
        auto m = makeValidOffer();
        m.signal.iceUfrag = "u space";
        CHECK(!validateWaveSignal(m, 0, &err));
        CHECK(formatWaveSignalToSdp(m.signal, true).empty());
    }
    {
        auto m = makeValidOffer();
        m.signal.icePwd = std::string("pwd\0bad", 7);
        CHECK(!validateWaveSignal(m, 0, &err));
    }
    {
        auto m = makeValidOffer();
        m.signal.icePwd = "pwd\x7f";
        CHECK(!validateWaveSignal(m, 0, &err));
    }
    {
        auto m = makeValidOffer();
        m.signal.dtlsFingerprintAlg = "sha-256\n";
        CHECK(!validateWaveSignal(m, 0, &err));
    }
    {
        auto m = makeValidOffer();
        m.signal.dtlsFingerprint = "AA BB CC";
        CHECK(!validateWaveSignal(m, 0, &err));
        CHECK(formatWaveSignalToSdp(m.signal, true).empty());
    }
    {
        auto m = makeValidOffer();
        m.signal.candidates[0].foundation = "1\r\n";
        CHECK(!validateWaveSignal(m, 0, &err));
        CHECK(formatWaveSignalToSdp(m.signal, true).empty());
    }
    {
        auto m = makeValidOffer();
        m.signal.candidates[0].address = "192.0.2.10 evil";
        CHECK(!validateWaveSignal(m, 0, &err));
        CHECK(formatWaveSignalToSdp(m.signal, true).empty());
    }

    // 2. Offer and Answer require non-empty ICE/DTLS fields; Cancel/End allow empty
    {
        auto m = makeValidOffer();
        m.signal.dtlsFingerprint.clear();
        CHECK(!validateWaveSignal(m, 0, &err));
        m.type = WaveSignalType::Answer;
        CHECK(!validateWaveSignal(m, 0, &err));
        m.type = WaveSignalType::Cancel;
        m.signal.iceUfrag.clear();
        m.signal.icePwd.clear();
        m.signal.candidates.clear();
        CHECK(validateWaveSignal(m, 0, &err));
        m.type = WaveSignalType::End;
        CHECK(validateWaveSignal(m, 0, &err));
    }

    // 3. Non-canonical base64url trailing bits must be rejected
    {
        std::vector<std::uint8_t> decodedBytes;
        CHECK(base64UrlDecode("AA", decodedBytes, &err));
        CHECK(decodedBytes.size() == 1u && decodedBytes[0] == 0u);
        CHECK(!base64UrlDecode("AB", decodedBytes, &err)); // rem==2, low 4 bits non-zero

        CHECK(base64UrlDecode("AAA", decodedBytes, &err));
        CHECK(decodedBytes.size() == 2u);
        CHECK(!base64UrlDecode("AAB", decodedBytes, &err)); // rem==3, low 2 bits non-zero
    }

    // 4. encodeWaveHttpsFragmentUri enforces non-empty https:// baseUrl and URI length bound
    {
        const auto m = makeValidOffer();
        CHECK(encodeWaveHttpsFragmentUri(m, "", &err).empty());
        CHECK(encodeWaveHttpsFragmentUri(m, "http://beygram.beykus.fun/", &err).empty());
        CHECK(encodeWaveHttpsFragmentUri(m, "https://", &err).empty());
        const std::string longBase =
            "https://beygram.beykus.fun/" + std::string(kWaveMaxUriLength, 'a');
        CHECK(encodeWaveHttpsFragmentUri(m, longBase, &err).empty());
    }

    // 5. parseSdpToWaveSignal rejects unknown candidate protocol/type and whitespace,
    //    and prefers sha-256 over sha-512 when multiple fingerprints exist
    {
        WaveSdpSignal parsed;
        const std::string badProtoSdp =
            "v=0\r\na=ice-ufrag:u\r\na=ice-pwd:secretPwd12345678\r\n"
            "a=candidate:1 1 sctp 123 192.0.2.1 5000 typ host\r\n";
        CHECK(!parseSdpToWaveSignal(badProtoSdp, parsed, &err));

        const std::string badTypeSdp =
            "v=0\r\na=ice-ufrag:u\r\na=ice-pwd:secretPwd12345678\r\n"
            "a=candidate:1 1 udp 123 192.0.2.1 5000 typ bogus\r\n";
        CHECK(!parseSdpToWaveSignal(badTypeSdp, parsed, &err));

        const std::string spaceUfragSdp =
            "v=0\r\na=ice-ufrag:u bad\r\na=ice-pwd:secretPwd12345678\r\n";
        CHECK(!parseSdpToWaveSignal(spaceUfragSdp, parsed, &err));

        const std::string fp256 =
            "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:"
            "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99";
        const std::string multiFpSdp =
            "v=0\r\na=ice-ufrag:u\r\na=ice-pwd:secretPwd12345678\r\n"
            "a=fingerprint:sha-256 " + fp256 + "\r\n"
            "a=fingerprint:sha-512 11:22:33:44\r\n";
        CHECK(parseSdpToWaveSignal(multiFpSdp, parsed, &err));
        CHECK(parsed.dtlsFingerprintAlg == "sha-256");
        CHECK(parsed.dtlsFingerprint == fp256);

        const std::string reverseMultiFpSdp =
            "v=0\r\na=ice-ufrag:u\r\na=ice-pwd:secretPwd12345678\r\n"
            "a=fingerprint:sha-512 11:22:33:44\r\n"
            "a=fingerprint:sha-256 " + fp256 + "\r\n";
        CHECK(parseSdpToWaveSignal(reverseMultiFpSdp, parsed, &err));
        CHECK(parsed.dtlsFingerprintAlg == "sha-256");
        CHECK(parsed.dtlsFingerprint == fp256);
    }

    // 6. formatWaveSignalToSdp(signal, false) never emits a=setup:actpass
    {
        const auto m = makeValidOffer();
        const std::string offerSdp = formatWaveSignalToSdp(m.signal, true);
        CHECK(offerSdp.find("a=setup:actpass\r\n") != std::string::npos);

        const std::string answerSdp = formatWaveSignalToSdp(m.signal, false);
        CHECK(answerSdp.find("a=setup:actpass") == std::string::npos);
        CHECK(answerSdp.find("a=setup:active\r\n") != std::string::npos);

        auto passiveSig = m.signal;
        passiveSig.setupRole = WaveDtlsSetupRole::Passive;
        const std::string passiveAnswerSdp = formatWaveSignalToSdp(passiveSig, false);
        CHECK(passiveAnswerSdp.find("a=setup:passive\r\n") != std::string::npos);
    }
}

TEST_CASE(wave.pipeline_accept_offer_state_replay_and_overflow) {
    using namespace bgmedia::wave;

    // 1. int64_t overflow guards in validateWaveSignal and startOffer
    {
        WaveSignalMessage m;
        m.version = 1;
        m.type = WaveSignalType::Cancel;
        m.sid = generateWaveSessionId();
        const std::int64_t maxI64 = std::numeric_limits<std::int64_t>::max();
        m.iat = maxI64 - 10;
        m.exp = maxI64;
        std::string err;
        CHECK(validateWaveSignal(m, maxI64, &err));

        m.iat = 1000;
        m.exp = 1100;
        CHECK(!validateWaveSignal(m, maxI64, &err));
    }

    auto hostTx = std::make_shared<SimulatedWaveTransport>();
    auto listTx = std::make_shared<SimulatedWaveTransport>();
    SimulatedWaveTransport::linkPeers(*hostTx, *listTx);

    WavePipelineConfig hCfg;
    hCfg.role = WaveRole::Host;
    WaveAudioPipeline host(hCfg, std::make_shared<Pcm16WaveCodec>(), hostTx);

    WavePipelineConfig lCfg;
    lCfg.role = WaveRole::Listener;
    WaveAudioPipeline listener(lCfg, std::make_shared<Pcm16WaveCodec>(), listTx);

    CHECK(!host.startOffer(std::numeric_limits<std::int64_t>::max(), 120));
    CHECK(host.state() == WaveSessionState::Idle);

    WaveSignalMessage offer1;
    WavePipelineCallbacks hCb;
    hCb.onOutboundSignal = [&](const WaveSignalMessage& m) {
        if (m.type == WaveSignalType::Offer) {
            offer1 = m;
        }
    };
    host.setCallbacks(std::move(hCb));

    CHECK(host.startOffer(1000, 120));
    CHECK(listener.acceptOffer(offer1, 1001));
    CHECK(listener.state() == WaveSessionState::LiveP2P);

    // Prepare a second distinct valid offer
    WaveSignalMessage offer2 = offer1;
    offer2.sid = generateWaveSessionId();
    offer2.nonce = offer1.nonce + 1;

    // acceptOffer must reject when Listener is already LiveP2P
    CHECK(!listener.acceptOffer(offer2, 1002));
    CHECK(listener.state() == WaveSessionState::LiveP2P);

    // Stop or reset, then replay offer1 within TTL -> must be rejected
    listener.stop(false, 1005);
    CHECK(listener.state() == WaveSessionState::Ended);
    CHECK(!listener.acceptOffer(offer1, 1006));

    listener.reset();
    CHECK(listener.state() == WaveSessionState::Idle);
    CHECK(!listener.acceptOffer(offer1, 1007));
    CHECK(!listener.handleRemoteSignal(offer1, 1008));

    // Fresh offer2 succeeds
    CHECK(listener.acceptOffer(offer2, 1009));
    CHECK(listener.state() == WaveSessionState::LiveP2P);
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

TEST_CASE(wave.jitter_buffer_huge_seq_jump) {
    using namespace bgmedia::wave;
    WaveJitterBuffer jb;
    auto makePkt = [](std::uint32_t seq) {
        WaveMediaPacket p;
        p.sequence = seq;
        p.timestamp48k = seq * 960;
        p.payload = {1, 2, 3, 4};
        return p;
    };
    jb.push(makePkt(10), 0.0);
    // Jump by 1,000,000,000 (~1e9) must complete in O(capacity)
    jb.push(makePkt(1000000010u), 0.020);
    jb.push(makePkt(1000000011u), 0.040);
    WaveMediaPacket out;
    CHECK(jb.popNext(out) == WaveJitterBuffer::PopStatus::Packet);
    CHECK(out.sequence == 1000000010u);
}

TEST_CASE(wave.signaling_security_and_limits) {
    using namespace bgmedia::wave;
    WaveSignalMessage base;
    base.version = 1;
    base.type = WaveSignalType::Offer;
    base.sid = generateWaveSessionId();
    base.iat = 1763000000;
    base.exp = 1763000120;
    base.nonce = 1;
    base.signal.iceUfrag = "u";
    base.signal.icePwd = "secretpwd12345678";
    base.signal.dtlsFingerprint =
        "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:"
        "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99";
    std::string err;

    // Time validation: iat=0, exp=0, exp==iat, TTL>600s, future skew>60s
    { auto m = base; m.iat = 0; CHECK(!validateWaveSignal(m, 1763000010, &err)); }
    { auto m = base; m.exp = 0; CHECK(!validateWaveSignal(m, 1763000010, &err)); }
    { auto m = base; m.exp = m.iat; CHECK(!validateWaveSignal(m, 1763000010, &err)); }
    { auto m = base; m.exp = m.iat + 601; CHECK(!validateWaveSignal(m, 1763000010, &err)); }
    { auto m = base; CHECK(!validateWaveSignal(m, m.iat - 61, &err)); }
    CHECK(validateWaveSignal(base, base.iat, &err));

    // Origin validation: reject arbitrary HTTPS origins and wrong paths
    const std::string validHttps = encodeWaveHttpsFragmentUri(base, "https://beygram.beykus.fun/", &err);
    const std::size_t fragPos = validHttps.find("#bgwave-v1.");
    CHECK(fragPos != std::string::npos);
    const std::string frag = validHttps.substr(fragPos);
    WaveSignalMessage out;
    CHECK(!decodeWaveUri("https://attacker.example/" + frag, out, &err));
    CHECK(!decodeWaveUri("http://beygram.beykus.fun/" + frag, out, &err));
    CHECK(!decodeWaveUri("https://beygram.beykus.fun/evil/" + frag, out, &err));

    // >32 ICE candidates must fail explicitly (no silent truncation)
    std::string bigSdp = "v=0\r\na=ice-ufrag:u\r\na=ice-pwd:secretpwd12345678\r\n";
    for (int i = 0; i < 33; ++i) {
        bigSdp += "a=candidate:" + std::to_string(i + 1) +
                  " 1 udp 2122260223 192.0.2." + std::to_string(i + 1) +
                  " 50000 typ host\r\n";
    }
    WaveSdpSignal parsed;
    CHECK(!parseSdpToWaveSignal(bigSdp, parsed, &err));
}

TEST_CASE(wave.stop_reset_concurrent_stress) {
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
    CHECK(host.startOffer(1000));
    CHECK(listener.acceptOffer(offer, 1001));
    CHECK(host.acceptAnswer(answer, 1002));

    std::atomic<bool> running{true};
    std::thread audioThread([&]() {
        std::vector<float> buf(512 * 2, 0.1f);
        while (running.load(std::memory_order_relaxed)) {
            host.onPcmRendered(buf.data(), 512, 48000);
            (void)listener.pullListenerPcm(buf.data(), 512, 48000);
        }
    });
    std::thread workerThread([&]() {
        double t = 0.0;
        while (running.load(std::memory_order_relaxed)) {
            host.process(t);
            listener.process(t);
            (void)host.stats();
            (void)listener.stats();
            t += 0.01;
        }
    });
    for (int i = 0; i < 10; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        host.stop(true, 1005 + i);
        host.reset();
    }
    running.store(false, std::memory_order_relaxed);
    audioThread.join();
    workerThread.join();
}

TEST_CASE(wave.reentrant_callbacks_no_deadlock) {
    using namespace bgmedia::wave;
    auto hostTx = std::make_shared<SimulatedWaveTransport>();
    auto listTx = std::make_shared<SimulatedWaveTransport>();
    SimulatedWaveTransport::linkPeers(*hostTx, *listTx);

    WavePipelineConfig hCfg; hCfg.role = WaveRole::Host;
    WaveAudioPipeline host(hCfg, std::make_shared<Pcm16WaveCodec>(), hostTx);
    WavePipelineConfig lCfg; lCfg.role = WaveRole::Listener;
    WaveAudioPipeline listener(lCfg, std::make_shared<Pcm16WaveCodec>(), listTx);

    int signalCallbacks = 0;
    // 1. Reentrant stop() at Offering -> no Offer emitted, returns false
    WavePipelineCallbacks hCb1;
    hCb1.onStateChanged = [&](WaveSessionState st, const std::string&) {
        (void)host.sessionId();
        (void)host.stats();
        if (st == WaveSessionState::Offering) {
            host.stop(false, 1005);
        }
    };
    hCb1.onOutboundSignal = [&](const WaveSignalMessage&) {
        ++signalCallbacks;
    };
    host.setCallbacks(std::move(hCb1));
    CHECK(!host.startOffer(1000));
    CHECK(signalCallbacks == 0);
    CHECK(host.state() == WaveSessionState::Ended);

    // 2. Reentrant stop() at AwaitingAnswer -> no orphan Offer emitted!
    host.reset();
    WavePipelineCallbacks hCb2;
    hCb2.onStateChanged = [&](WaveSessionState st, const std::string&) {
        if (st == WaveSessionState::AwaitingAnswer) {
            host.stop(false, 1005);
        }
    };
    hCb2.onOutboundSignal = [&](const WaveSignalMessage&) {
        ++signalCallbacks;
    };
    host.setCallbacks(std::move(hCb2));
    CHECK(!host.startOffer(1000));
    CHECK(signalCallbacks == 0);
    CHECK(host.state() == WaveSessionState::Ended);

    // 3. Valid offer, then listener reentrant stop() at ConnectingP2P -> no Answer emitted
    host.reset();
    WaveSignalMessage validOffer;
    WavePipelineCallbacks hCb3;
    hCb3.onOutboundSignal = [&](const WaveSignalMessage& m) {
        if (m.type == WaveSignalType::Offer) validOffer = m;
    };
    host.setCallbacks(std::move(hCb3));
    CHECK(host.startOffer(1000));

    int listenerAnswers = 0;
    WavePipelineCallbacks lCb;
    lCb.onStateChanged = [&](WaveSessionState st, const std::string&) {
        (void)listener.sessionId();
        if (st == WaveSessionState::ConnectingP2P) {
            listener.stop(false, 1006);
        }
    };
    lCb.onOutboundSignal = [&](const WaveSignalMessage&) {
        ++listenerAnswers;
    };
    listener.setCallbacks(std::move(lCb));
    CHECK(!listener.acceptOffer(validOffer, 1001));
    CHECK(listenerAnswers == 0);
    CHECK(listener.state() == WaveSessionState::Ended);
}

TEST_CASE(wave.deferred_reset_and_reconnect_no_process) {
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

    CHECK(host.startOffer(1000));
    CHECK(listener.acceptOffer(offer, 1001));
    CHECK(host.acceptAnswer(answer, 1002));

    // Stop while audio thread is pumping, then stop audio thread WITHOUT calling process().
    std::atomic<bool> pumping{true};
    std::thread audioThread([&]() {
        std::vector<float> buf(512 * 2, 0.3f);
        while (pumping.load(std::memory_order_relaxed)) {
            host.onPcmRendered(buf.data(), 512, 48000);
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    host.stop(true, 1010);
    listener.stop(false, 1010);
    pumping.store(false, std::memory_order_relaxed);
    audioThread.join();

    // Reconnect with NO intermediate process() call: audio must not stay paused.
    CHECK(host.reconnect(1020));
    CHECK(listener.reconnect(1020));
    CHECK(listener.acceptOffer(offer, 1021));
    CHECK(host.acceptAnswer(answer, 1022));
    CHECK(host.state() == WaveSessionState::LiveP2P);
    std::vector<float> pcm(960 * 2, 0.25f);
    host.onPcmRendered(pcm.data(), 960, 48000);
    host.process(1.0);
    CHECK(host.stats().packetsSent >= 1u);
}

TEST_CASE(wave.reentrant_stop_and_restart_from_poll_callback) {
    using namespace bgmedia::wave;
    auto hostTx = std::make_shared<SimulatedWaveTransport>();
    auto listTx = std::make_shared<SimulatedWaveTransport>();
    SimulatedWaveTransport::linkPeers(*hostTx, *listTx);

    WavePipelineConfig hCfg; hCfg.role = WaveRole::Host;
    WaveAudioPipeline host(hCfg, std::make_shared<Pcm16WaveCodec>(), hostTx);
    WavePipelineConfig lCfg; lCfg.role = WaveRole::Listener;
    WaveAudioPipeline listener(lCfg, std::make_shared<Pcm16WaveCodec>(), listTx);

    WaveSignalMessage offer, answer;
    std::vector<WaveSignalMessage> offers;
    bool didRestart = false;
    bool restartOk = false;

    WavePipelineCallbacks hCb;
    hCb.onOutboundSignal = [&](const WaveSignalMessage& m) {
        if (m.type == WaveSignalType::Offer) {
            offer = m;
            offers.push_back(m);
        }
    };
    hCb.onStateChanged = [&](WaveSessionState st, const std::string&) {
        if (st == WaveSessionState::LiveP2P && !didRestart) {
            didRestart = true;
            // Reentrant stop + immediate reconnect from inside transport->poll()
            // during process() must neither deadlock on processMutex_ nor hang.
            host.stop(false, 1010);
            restartOk = host.reconnect(1020);
        }
    };
    host.setCallbacks(std::move(hCb));

    WavePipelineCallbacks lCb;
    lCb.onOutboundSignal = [&](const WaveSignalMessage& m) {
        if (m.type == WaveSignalType::Answer) answer = m;
    };
    listener.setCallbacks(std::move(lCb));

    CHECK(host.startOffer(1000));
    CHECK(listener.acceptOffer(offer, 1001));
    CHECK(host.acceptAnswer(answer, 1002));

    // Triggers transport->poll() -> onConnected -> onStateChanged(LiveP2P).
    host.process(0.1);

    CHECK(didRestart);
    CHECK(restartOk);
    CHECK(offers.size() == 2u);
    CHECK(host.state() == WaveSessionState::AwaitingAnswer);
}

TEST_CASE(wave.jitter_buffer_retention_interleaved_10ms_process) {
    using namespace bgmedia::wave;

    class ManualListenerTransport final : public IWaveTransport {
    public:
        void setCallbacks(WaveTransportCallbacks callbacks) override {
            cb_ = std::move(callbacks);
        }
        bool startOffer() override { return false; }
        bool acceptOffer(const WaveSdpSignal& /*remoteOffer*/) override {
            state_ = WaveTransportState::Gathering;
            if (cb_.onLocalSignalReady) {
                WaveSdpSignal sig;
                sig.iceUfrag = "listufrag";
                sig.icePwd = "listpassword1234567890";
                sig.dtlsFingerprintAlg = "sha-256";
                sig.dtlsFingerprint =
                    "01:23:45:67:89:AB:CD:EF:01:23:45:67:89:AB:CD:EF:"
                    "01:23:45:67:89:AB:CD:EF:01:23:45:67:89:AB:CD:EF";
                sig.setupRole = WaveDtlsSetupRole::Active;
                cb_.onLocalSignalReady(sig);
            }
            state_ = WaveTransportState::Connected;
            if (cb_.onStateChanged) {
                cb_.onStateChanged(WaveTransportState::Connected, "connected");
            }
            return true;
        }
        bool applyAnswer(const WaveSdpSignal& /*remoteAnswer*/) override {
            return false;
        }
        bool sendMediaPacket(const WaveMediaPacket& /*packet*/) override {
            return false;
        }
        void close() override {
            state_ = WaveTransportState::Closed;
        }
        void poll(double /*currentTimeSeconds*/) override {}
        WaveTransportState state() const noexcept override { return state_; }

        void injectPacket(WaveMediaPacket pkt, double arrivalTimeSeconds) {
            if (cb_.onMediaPacket) {
                cb_.onMediaPacket(std::move(pkt), arrivalTimeSeconds);
            }
        }

    private:
        WaveTransportCallbacks cb_;
        WaveTransportState state_ = WaveTransportState::Idle;
    };

    auto listTx = std::make_shared<ManualListenerTransport>();
    WavePipelineConfig lCfg;
    lCfg.role = WaveRole::Listener;
    lCfg.jitterConfig.minDepthFrames = 2;
    lCfg.jitterConfig.maxDepthFrames = 6;
    auto codec = std::make_shared<Pcm16WaveCodec>();
    WaveAudioPipeline listener(lCfg, codec, listTx);

    WaveSignalMessage offer;
    offer.version = kWaveSignalVersion;
    offer.type = WaveSignalType::Offer;
    offer.sid = generateWaveSessionId();
    offer.iat = 1763000000;
    offer.exp = 1763000120;
    offer.nonce = 1;
    offer.signal.iceUfrag = "hostufrag";
    offer.signal.icePwd = "hostpassword1234567890";
    offer.signal.dtlsFingerprintAlg = "sha-256";
    offer.signal.dtlsFingerprint =
        "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:"
        "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99";
    offer.signal.setupRole = WaveDtlsSetupRole::Actpass;

    CHECK(listener.acceptOffer(offer, 1763000001));
    CHECK(listener.state() == WaveSessionState::LiveP2P);

    auto makeEncodedPkt = [&](std::uint32_t seq) {
        std::vector<float> pcm(kWaveFrameSamples * 2, 0.25f);
        std::vector<std::uint8_t> encoded;
        CHECK(codec->encode(pcm, encoded));
        WaveMediaPacket pkt;
        pkt.sequence = seq;
        pkt.timestamp48k = seq * static_cast<std::uint32_t>(kWaveFrameSamples);
        pkt.frameSamples = static_cast<std::uint16_t>(kWaveFrameSamples);
        pkt.payload = std::move(encoded);
        return pkt;
    };

    std::vector<float> pulled(480 * 2, 0.0f);
    // Interleaved 20ms packet arrival (with seq 2 delayed by 10ms behind seq 3)
    // and 10ms process() ticks: jitter buffer must retain target depth so seq 2
    // arrives in time without triggering MissingPlc / packet loss.
    listTx->injectPacket(makeEncodedPkt(0), 0.000);
    listener.process(0.000);
    listener.process(0.010);

    listTx->injectPacket(makeEncodedPkt(1), 0.020);
    listener.process(0.020);
    (void)listener.pullListenerPcm(pulled.data(), 480, 48000);
    listener.process(0.030);
    (void)listener.pullListenerPcm(pulled.data(), 480, 48000);

    listTx->injectPacket(makeEncodedPkt(3), 0.040); // seq 2 delayed
    listener.process(0.040);
    (void)listener.pullListenerPcm(pulled.data(), 480, 48000);

    listTx->injectPacket(makeEncodedPkt(2), 0.050); // arrives 10ms later
    listener.process(0.050);
    (void)listener.pullListenerPcm(pulled.data(), 480, 48000);

    listTx->injectPacket(makeEncodedPkt(4), 0.060);
    listener.process(0.060);
    (void)listener.pullListenerPcm(pulled.data(), 480, 48000);
    listener.process(0.070);
    (void)listener.pullListenerPcm(pulled.data(), 480, 48000);

    listTx->injectPacket(makeEncodedPkt(5), 0.080);
    listener.process(0.080);

    const WaveStats ls = listener.stats();
    CHECK(ls.packetsReceived == 6u);
    CHECK(ls.packetsLost == 0u);
    CHECK(ls.plcFrames == 0u);
    CHECK(ls.packetsReordered == 1u);
}

TEST_CASE(wave.jitter_buffer_edge_cases) {
    using namespace bgmedia::wave;

    auto makePkt = [](std::uint32_t seq) {
        WaveMediaPacket p;
        p.sequence = seq;
        p.timestamp48k = seq * 960;
        p.payload = {1, 2, 3, 4};
        return p;
    };

    // 1. sampleRate = 0 and invalid min/max depth sanitization
    {
        WaveJitterBuffer::Config cfg;
        cfg.sampleRate = 0;
        cfg.minDepthFrames = 0;
        cfg.maxDepthFrames = 0;
        WaveJitterBuffer jb(cfg);
        CHECK(jb.targetDepthFrames() >= 1u);
        jb.push(makePkt(0), 0.000);
        jb.push(makePkt(1), 0.050);
        jb.push(makePkt(2), 0.120);
        CHECK(std::isfinite(jb.jitterMs()));
        CHECK_NEAR(jb.jitterMs(), 0.0, 1e-9);
    }

    // 2. minDepthFrames > maxDepthFrames sanitized without clamp UB
    {
        WaveJitterBuffer::Config cfg;
        cfg.minDepthFrames = 8;
        cfg.maxDepthFrames = 2;
        WaveJitterBuffer jb(cfg);
        CHECK(jb.targetDepthFrames() == 8u);
        jb.push(makePkt(0), 0.000);
        jb.push(makePkt(1), 0.020);
        CHECK(jb.targetDepthFrames() == 8u);
    }

    // 3. NaN/Inf arrivalTimeSeconds ignored cleanly
    {
        WaveJitterBuffer jb;
        jb.push(makePkt(0), 0.000);
        jb.push(makePkt(1), 0.020);
        jb.push(makePkt(2), std::numeric_limits<double>::quiet_NaN());
        jb.push(makePkt(3), std::numeric_limits<double>::infinity());
        CHECK(std::isfinite(jb.jitterMs()));
        CHECK_NEAR(jb.jitterMs(), 0.0, 1e-9);
    }

    // 4. Late and duplicate packets do not inflate jitterMs
    {
        WaveJitterBuffer::Config cfg;
        cfg.minDepthFrames = 2;
        cfg.maxDepthFrames = 8;
        WaveJitterBuffer jb(cfg);
        jb.push(makePkt(0), 0.000);
        jb.push(makePkt(1), 0.020);
        jb.push(makePkt(2), 0.040);
        CHECK_NEAR(jb.jitterMs(), 0.0, 1e-9);

        WaveMediaPacket out;
        CHECK(jb.popNext(out) == WaveJitterBuffer::PopStatus::Packet);
        CHECK(out.sequence == 0u);

        // Late packet with huge arrival delay must not affect jitterMs
        jb.push(makePkt(0), 5.000);
        CHECK(jb.packetsDroppedLate() == 1u);
        CHECK_NEAR(jb.jitterMs(), 0.0, 1e-9);

        // Duplicate packet with huge arrival delay must not affect jitterMs
        jb.push(makePkt(1), 10.000);
        CHECK(jb.packetsDuplicate() == 1u);
        CHECK_NEAR(jb.jitterMs(), 0.0, 1e-9);
    }

    // 5. advancePlaySeq accounts for skipped occupied and gap slots
    {
        WaveJitterBuffer::Config cfg;
        cfg.maxSlots = 16;
        cfg.minDepthFrames = 2;
        cfg.maxDepthFrames = 4;
        WaveJitterBuffer jb(cfg);
        jb.push(makePkt(0), 0.000);
        jb.push(makePkt(1), 0.020);
        // offset = 20 (>= 16, < 32) -> advance = 20 - 8 = 12 (skips seq 0..11)
        jb.push(makePkt(20), 0.400);
        CHECK(jb.packetsDroppedLate() == 2u); // occupied seq 0, 1
        CHECK(jb.packetsLost() == 10u);       // gap seq 2..11
    }
}

TEST_CASE(wave.resampler_downsampling_prefilter_and_streaming) {
    using namespace bgmedia::wave;

    // 1. Streaming 96kHz -> 48kHz passband (1 kHz) in chunks including small <4 frame chunks
    {
        WaveStereoResampler resampler;
        resampler.configure(96000, 48000);
        CHECK(!resampler.isBypass());

        const std::size_t totalInFrames = 9600; // 100 ms @ 96 kHz
        std::vector<float> in(totalInFrames * 2);
        for (std::size_t i = 0; i < totalInFrames; ++i) {
            const float v =
                0.5f * std::sin(2.0f * 3.14159265f * 1000.0f *
                                static_cast<float>(i) / 96000.0f);
            in[i * 2] = v;
            in[i * 2 + 1] = -v;
        }

        std::vector<float> out;
        std::size_t pos = 0;
        const std::size_t chunkSizes[] = {2, 3, 955, 960, 1920};
        std::size_t cIdx = 0;
        while (pos < totalInFrames) {
            const std::size_t take =
                std::min(chunkSizes[cIdx % 5], totalInFrames - pos);
            resampler.process(
                std::span<const float>(in.data() + pos * 2, take * 2), out);
            pos += take;
            ++cIdx;
        }

        const std::size_t outFrames = out.size() / 2;
        CHECK(outFrames > 4770u && outFrames < 4820u);
        double energy = 0.0;
        for (float s : out) {
            CHECK(std::isfinite(s));
            CHECK(s >= -1.0f && s <= 1.0f);
            energy += static_cast<double>(s) * s;
        }
        CHECK(energy > 500.0);
    }

    // 2. Anti-aliasing 3-tap [0.25, 0.5, 0.25] prefilter attenuates fs_in/2 Nyquist tone
    {
        WaveStereoResampler resampler;
        resampler.configure(96000, 48000);
        const std::size_t frames = 2000;
        std::vector<float> nyquistIn(frames * 2);
        for (std::size_t i = 0; i < frames; ++i) {
            const float v = (i % 2 == 0) ? 0.8f : -0.8f;
            nyquistIn[i * 2] = v;
            nyquistIn[i * 2 + 1] = v;
        }
        std::vector<float> nyquistOut;
        resampler.process(nyquistIn, nyquistOut);
        const std::size_t outFrames = nyquistOut.size() / 2;
        CHECK(outFrames > 900u);
        float maxSteady = 0.0f;
        for (std::size_t i = 16; i < outFrames; ++i) {
            maxSteady = std::max(maxSteady, std::abs(nyquistOut[i * 2]));
            maxSteady = std::max(maxSteady, std::abs(nyquistOut[i * 2 + 1]));
        }
        CHECK(maxSteady < 0.01f);
    }
}

TEST_CASE(wave.pipeline_stack_lifetime_and_final_stats_after_stop) {
    using namespace bgmedia::wave;

    auto hostTx = std::make_shared<SimulatedWaveTransport>();
    auto listTx = std::make_shared<SimulatedWaveTransport>();
    SimulatedWaveTransport::linkPeers(*hostTx, *listTx);

    {
        WavePipelineConfig hCfg;
        hCfg.role = WaveRole::Host;
        WaveAudioPipeline host(hCfg, std::make_shared<Pcm16WaveCodec>(), hostTx);

        WavePipelineConfig lCfg;
        lCfg.role = WaveRole::Listener;
        WaveAudioPipeline listener(lCfg, std::make_shared<Pcm16WaveCodec>(),
                                   listTx);

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

        CHECK(host.startOffer(1000));
        CHECK(listener.acceptOffer(offer, 1001));
        CHECK(host.acceptAnswer(answer, 1002));

        std::vector<float> pcm(kWaveFrameSamples * 2, 0.25f);
        std::vector<float> out(kWaveFrameSamples * 2, 0.0f);
        double t = 0.0;
        for (int step = 0; step < 6; ++step) {
            host.onPcmRendered(pcm.data(), kWaveFrameSamples, kWaveSampleRate);
            host.process(t);
            listener.process(t);
            (void)listener.pullListenerPcm(out.data(), kWaveFrameSamples,
                                           kWaveSampleRate);
            t += 0.020;
        }

        host.stop(true, 1005);
        listener.stop(false, 1005);

        // Final stats must remain readable after stop().
        CHECK(host.stats().packetsSent >= 5u);
        CHECK(host.stats().bytesSent > 0u);
        CHECK(listener.stats().packetsReceived >= 4u);
        CHECK(listener.stats().bytesReceived > 0u);

        host.reset();
        CHECK(host.stats().packetsSent == 0u);
    }

    // Transports outlive stack-allocated pipelines: poll/close must be safe.
    hostTx->poll(1.0);
    listTx->poll(1.0);
    hostTx->close();
    listTx->close();
}
