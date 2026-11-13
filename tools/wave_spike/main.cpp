#include "bgmedia/mix/MixEngine.hpp"
#include "bgmedia/mix/PcmTrack.hpp"
#include "bgmedia/wave/WaveAudioPipeline.hpp"
#include "bgmedia/wave/WaveCodec.hpp"
#include "bgmedia/wave/WaveSignaling.hpp"
#include "bgmedia/wave/WaveTransport.hpp"

#include <chrono>
#include <cmath>
#include <iostream>
#include <memory>
#include <filesystem>
#include <fstream>
#include <string_view>
#include <vector>

using namespace bgmedia::wave;

namespace {

void measureSignalingScenario(const char* label, const std::string& sdp) {
    WaveSdpSignal sig;
    std::string err;
    if (!parseSdpToWaveSignal(sdp, sig, &err)) {
        std::cerr << label << " parse failed: " << err << "\n";
        return;
    }
    WaveSignalMessage msg;
    msg.version = 1;
    msg.type = WaveSignalType::Offer;
    msg.sid = generateWaveSessionId();
    msg.iat = 1763000000;
    msg.exp = 1763000120;
    msg.caps = kWaveCapOpus48kStereo | kWaveCapDtlsSrtp |
               kWaveCapExternalCallFallback;
    msg.nonce = 1;
    msg.signal = sig;

    std::vector<std::uint8_t> bin;
    (void)encodeWaveSignalBinary(msg, bin, &err);
    const std::string waveUri = encodeWaveUri(msg, &err);
    const std::string httpsUri = encodeWaveHttpsFragmentUri(msg);

    std::cout << "[Signaling] " << label
              << " | sdp_bytes=" << sdp.size()
              << " | candidates=" << sig.candidates.size()
              << " | bin_bytes=" << bin.size()
              << " | bg_uri_len=" << waveUri.size()
              << " | https_uri_len=" << httpsUri.size() << "\n";
}

} // namespace

void writeBytes(const std::filesystem::path& path, const std::vector<std::uint8_t>& data) {
    std::ofstream f(path, std::ios::binary);
    f.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
}
void writeText(const std::filesystem::path& path, std::string_view text) {
    std::ofstream f(path, std::ios::binary);
    f.write(text.data(), static_cast<std::streamsize>(text.size()));
}
int dumpFixtures(const std::filesystem::path& dir) {
    std::filesystem::create_directories(dir);
    const std::array<std::uint8_t, 16> sid = {
        0x10, 0x21, 0x32, 0x43, 0x54, 0x65, 0x76, 0x87,
        0x98, 0xA9, 0xBA, 0xCB, 0xDC, 0xED, 0xFE, 0x0F
    };
    auto dumpMsg = [&](const char* name, WaveSignalMessage msg) {
        std::vector<std::uint8_t> bin;
        std::string err;
        if (!encodeWaveSignalBinary(msg, bin, &err)) {
            std::cerr << name << " encode failed: " << err << "\n";
            return;
        }
        writeBytes(dir / (std::string(name) + ".bin"), bin);
        const std::string uris = encodeWaveUri(msg, &err) + "\n" + encodeWaveHttpsFragmentUri(msg, "https://beygram.beykus.fun/", &err) + "\n";
        writeText(dir / (std::string(name) + ".uri.txt"), uris);
    };
    WaveSignalMessage offer;
    offer.version = 1; offer.type = WaveSignalType::Offer; offer.sid = sid;
    offer.iat = 1763000000; offer.exp = 1763000120;
    offer.caps = kWaveCapOpus48kStereo | kWaveCapDtlsSrtp | kWaveCapExternalCallFallback;
    offer.nonce = 1;
    offer.signal.iceUfrag = "bgwHost1";
    offer.signal.icePwd = "hostIceSecretPwd12345678";
    offer.signal.dtlsFingerprintAlg = "sha-256";
    offer.signal.dtlsFingerprint = "00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF";
    offer.signal.setupRole = WaveDtlsSetupRole::Actpass;
    offer.signal.candidates = {
        {"1", 1, WaveIceProtocol::Udp, 2122260223u, "192.0.2.10", 50000, WaveIceCandidateType::Host, WaveIceTcpType::None},
        {"2", 1, WaveIceProtocol::Udp, 2122194687u, "2001:db8::10", 50001, WaveIceCandidateType::Host, WaveIceTcpType::None},
        {"3", 1, WaveIceProtocol::Udp, 1686052607u, "a1b2c3d4-0001.local", 50002, WaveIceCandidateType::Host, WaveIceTcpType::None}
    };
    dumpMsg("offer", offer);
    WaveSignalMessage answer = offer;
    answer.type = WaveSignalType::Answer; answer.nonce = 2;
    answer.signal.iceUfrag = "bgwList1";
    answer.signal.icePwd = "listIceSecretPwd12345678";
    answer.signal.setupRole = WaveDtlsSetupRole::Active;
    answer.signal.candidates = {{"1", 1, WaveIceProtocol::Udp, 2122260223u, "198.51.100.20", 51000, WaveIceCandidateType::Srflx, WaveIceTcpType::None}};
    dumpMsg("answer", answer);
    WaveSignalMessage cancel;
    cancel.version = 1; cancel.type = WaveSignalType::Cancel; cancel.sid = sid;
    cancel.iat = 1763000010; cancel.exp = 1763000070; cancel.caps = 0; cancel.nonce = 3;
    dumpMsg("cancel", cancel);
    WaveSignalMessage endMsg = cancel;
    endMsg.type = WaveSignalType::End; endMsg.iat = 1763000090; endMsg.exp = 1763000150; endMsg.nonce = 4;
    dumpMsg("end", endMsg);
    std::vector<std::uint8_t> offerBin;
    (void)encodeWaveSignalBinary(offer, offerBin, nullptr);
    { auto b = offerBin; b[0] = 'X'; writeBytes(dir / "invalid_bad_magic.bin", b); }
    { auto b = offerBin; b[2] = 99; writeBytes(dir / "invalid_unknown_version.bin", b); }
    { auto b = offerBin; b.resize(12); writeBytes(dir / "invalid_truncated.bin", b); }
    { auto b = offerBin; b.push_back(0xAA); writeBytes(dir / "invalid_trailing_bytes.bin", b); }
    { auto b = offerBin; std::fill(b.begin() + 4, b.begin() + 20, std::uint8_t{0}); writeBytes(dir / "invalid_zero_sid.bin", b); }
    std::cout << "Dumped BG Wave v1 golden fixtures to " << dir.string() << "\n";
    return 0;
}

int main(int argc, char** argv) {
    if (argc == 3 && std::string_view(argv[1]) == "--dump-fixtures") {
        return dumpFixtures(argv[2]);
    }
    std::cout << "=== BG Wave Spike Measurements ===\n";

    // 1. Signaling payload sizes across real ICE scenarios
    measureSignalingScenario(
        "IPv4 host+srflx",
        "v=0\r\no=- 1 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "a=ice-ufrag:ufrag123\r\na=ice-pwd:password1234567890abcdef\r\n"
        "a=fingerprint:sha-256 AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:"
        "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99\r\n"
        "a=setup:actpass\r\n"
        "a=candidate:1 1 udp 2122260223 192.168.1.10 50000 typ host\r\n"
        "a=candidate:2 1 udp 1686052607 198.51.100.42 50001 typ srflx\r\n");

    measureSignalingScenario(
        "Dual-stack IPv4+IPv6+mDNS",
        "v=0\r\no=- 1 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "a=ice-ufrag:ufrag123\r\na=ice-pwd:password1234567890abcdef\r\n"
        "a=fingerprint:sha-256 AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:"
        "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99\r\n"
        "a=setup:actpass\r\n"
        "a=candidate:1 1 udp 2122260223 192.168.1.10 50000 typ host\r\n"
        "a=candidate:2 1 udp 2122194687 2001:db8:85a3::8a2e:370:7334 50002 typ host\r\n"
        "a=candidate:3 1 udp 2122129151 9f8e7d6c-1234-4abc-9def-0123456789ab.local 50003 typ host\r\n"
        "a=candidate:4 1 udp 1686052607 198.51.100.42 50001 typ srflx\r\n");

    measureSignalingScenario(
        "Multi-interface (6 candidates)",
        "v=0\r\no=- 1 2 IN IP4 127.0.0.1\r\ns=-\r\nt=0 0\r\n"
        "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        "a=ice-ufrag:ufrag123\r\na=ice-pwd:password1234567890abcdef\r\n"
        "a=fingerprint:sha-256 AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99:"
        "AA:BB:CC:DD:EE:FF:00:11:22:33:44:55:66:77:88:99\r\n"
        "a=setup:actpass\r\n"
        "a=candidate:1 1 udp 2122260223 192.168.1.10 50000 typ host\r\n"
        "a=candidate:2 1 udp 2122260222 10.0.0.5 50001 typ host\r\n"
        "a=candidate:3 1 udp 2122194687 2001:db8::1 50002 typ host\r\n"
        "a=candidate:4 1 udp 2122129151 host-iface.local 50003 typ host\r\n"
        "a=candidate:5 1 udp 1686052607 198.51.100.42 50004 typ srflx\r\n"
        "a=candidate:6 1 tcp 1518280447 198.51.100.42 9 typ srflx tcptype active\r\n");

    // 2. PCM Tap Overhead
    {
        using namespace bgmedia::mix;
        MixEngine engine(48000);
        auto track = std::make_shared<PcmTrack>();
        track->sampleRate = 48000;
        track->channels = 2;
        track->samples.assign(48000 * 2, 0.25f);
        (void)engine.loadDeck(DeckId::A, track, 1, 120.0);
        engine.play(DeckId::A);

        constexpr std::size_t kFrames = 512;
        constexpr int kIters = 2000;
        std::vector<float> buf(kFrames * 2);

        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < kIters; ++i) {
            engine.render(buf.data(), kFrames);
        }
        const auto t1 = std::chrono::steady_clock::now();

        auto tx = std::make_shared<SimulatedWaveTransport>();
        WavePipelineConfig cfg;
        cfg.role = WaveRole::Host;
        auto pipe = std::make_shared<WaveAudioPipeline>(
            cfg, std::make_shared<Pcm16WaveCodec>(), tx);
        engine.setPcmTap(pipe);

        const auto t2 = std::chrono::steady_clock::now();
        for (int i = 0; i < kIters; ++i) {
            engine.render(buf.data(), kFrames);
        }
        const auto t3 = std::chrono::steady_clock::now();
        engine.setPcmTap(nullptr);
        engine.reclaimRetired();

        const double noTapUs =
            std::chrono::duration<double, std::micro>(t1 - t0).count() / kIters;
        const double withTapUs =
            std::chrono::duration<double, std::micro>(t3 - t2).count() / kIters;
        std::cout << "[PCM Tap] 512-frame render avg: no_tap=" << noTapUs
                  << " us | with_tap=" << withTapUs
                  << " us | overhead=" << (withTapUs - noTapUs) << " us\n";
    }

    // 3. 1-hour simulated continuous stream (3600 s = 180,000 frames @ 20 ms)
    {
        SimulatedNetworkProfile prof;
        prof.baseLatencyMs = 20.0;
        prof.jitterMs = 10.0;
        prof.lossPercent = 2.0;
        prof.reorderPercent = 5.0;

        auto hostTx = std::make_shared<SimulatedWaveTransport>(prof);
        auto listTx = std::make_shared<SimulatedWaveTransport>(prof);
        SimulatedWaveTransport::linkPeers(*hostTx, *listTx);

        WavePipelineConfig hCfg;
        hCfg.role = WaveRole::Host;
        WaveAudioPipeline host(hCfg, std::make_shared<Pcm16WaveCodec>(), hostTx);

        WavePipelineConfig lCfg;
        lCfg.role = WaveRole::Listener;
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

        host.startOffer(1000);
        listener.acceptOffer(offer, 1001);
        host.acceptAnswer(answer, 1002);

        std::vector<float> inPcm(kWaveFrameSamples * 2, 0.2f);
        std::vector<float> outPcm(kWaveFrameSamples * 2, 0.0f);
        double simTime = 0.0;
        for (int f = 0; f < 180000; ++f) {
            host.pushHostPcm(inPcm.data(), kWaveFrameSamples, kWaveSampleRate);
            host.process(simTime);
            listener.process(simTime);
            listener.pullListenerPcm(outPcm.data(), kWaveFrameSamples, kWaveSampleRate);
            simTime += 0.020;
        }
        std::cout << "[1h Stream] " << host.diagnosticsSummary() << "\n"
                  << "            " << listener.diagnosticsSummary() << "\n";
    }

    return 0;
}
