#include "framework.hpp"

#include "bgmedia/wave/WaveAudioPipeline.hpp"
#include "bgmedia/wave/WaveCodec.hpp"
#include "bgmedia/wave/WaveJitterBuffer.hpp"
#include "bgmedia/wave/WaveSignaling.hpp"

#ifdef BGMEDIA_HAVE_WAVE_OPUS
#include "bgmedia/wave/OpusWaveCodec.hpp"
#endif

#ifdef BGMEDIA_HAVE_WAVE_WEBRTC
#include "bgmedia/wave/WebRtcWaveTransport.hpp"
#endif

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

std::shared_ptr<bgmedia::wave::IWaveAudioCodec> makeRealOrFallbackCodec(
    std::uint32_t bitrateBps = bgmedia::wave::kWaveDefaultBitrateBps) {
#ifdef BGMEDIA_HAVE_WAVE_OPUS
    auto opus = std::make_shared<bgmedia::wave::OpusWaveCodec>(bitrateBps);
    if (opus->isReady()) {
        return opus;
    }
#else
    (void)bitrateBps;
#endif
    return std::make_shared<bgmedia::wave::Pcm16WaveCodec>();
}

double computeRms(const std::vector<float>& pcm) {
    if (pcm.empty()) return 0.0;
    double sumSq = 0.0;
    for (float s : pcm) {
        sumSq += static_cast<double>(s) * s;
    }
    return std::sqrt(sumSq / static_cast<double>(pcm.size()));
}

void generateStereoSine(std::vector<float>& out, std::size_t frames,
                        double& phase) {
    out.resize(frames * 2);
    for (std::size_t i = 0; i < frames; ++i) {
        out[i * 2] = static_cast<float>(0.45 * std::sin(phase));
        out[i * 2 + 1] = static_cast<float>(0.45 * std::sin(phase * 1.5));
        phase += 2.0 * 3.14159265358979323846 * 440.0 / 48000.0;
    }
}

#if defined(BGMEDIA_HAVE_WAVE_WEBRTC) && defined(_WIN32) && defined(BGMEDIA_WAVE_P2P_PEER_EXE)
struct WinPeerProcess {
    HANDLE process = nullptr;
    HANDLE thread = nullptr;
    HANDLE stdinWrite = nullptr;
    HANDLE stdoutRead = nullptr;

    ~WinPeerProcess() { cleanup(); }

    void cleanup() {
        if (stdinWrite) { CloseHandle(stdinWrite); stdinWrite = nullptr; }
        if (stdoutRead) { CloseHandle(stdoutRead); stdoutRead = nullptr; }
        if (thread) { CloseHandle(thread); thread = nullptr; }
        if (process) { CloseHandle(process); process = nullptr; }
    }

    bool start(const std::string& args) {
        SECURITY_ATTRIBUTES sa{};
        sa.nLength = sizeof(sa);
        sa.bInheritHandle = TRUE;

        HANDLE stdinRead = nullptr;
        HANDLE stdoutWrite = nullptr;
        if (!CreatePipe(&stdinRead, &stdinWrite, &sa, 0) ||
            !CreatePipe(&stdoutRead, &stdoutWrite, &sa, 0)) {
            return false;
        }
        SetHandleInformation(stdinWrite, HANDLE_FLAG_INHERIT, 0);
        SetHandleInformation(stdoutRead, HANDLE_FLAG_INHERIT, 0);

        STARTUPINFOA si{};
        si.cb = sizeof(si);
        si.dwFlags = STARTF_USESTDHANDLES;
        si.hStdInput = stdinRead;
        si.hStdOutput = stdoutWrite;
        si.hStdError = GetStdHandle(STD_ERROR_HANDLE);

        PROCESS_INFORMATION pi{};
        std::string cmd = std::string("\"") + BGMEDIA_WAVE_P2P_PEER_EXE + "\" " + args;
        std::vector<char> cmdBuf(cmd.begin(), cmd.end());
        cmdBuf.push_back('\0');

        const BOOL ok = CreateProcessA(
            nullptr, cmdBuf.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi);
        CloseHandle(stdinRead);
        CloseHandle(stdoutWrite);
        if (!ok) {
            cleanup();
            return false;
        }
        process = pi.hProcess;
        thread = pi.hThread;
        return true;
    }

    bool readLine(std::string& outLine) {
        outLine.clear();
        char ch = 0;
        DWORD n = 0;
        while (ReadFile(stdoutRead, &ch, 1, &n, nullptr) && n == 1) {
            if (ch == '\r') continue;
            if (ch == '\n') return true;
            outLine.push_back(ch);
        }
        return !outLine.empty();
    }

    bool writeLine(const std::string& line) {
        std::string withNl = line + "\n";
        DWORD written = 0;
        return WriteFile(stdinWrite, withNl.data(),
                         static_cast<DWORD>(withNl.size()), &written, nullptr) &&
               written == withNl.size();
    }

    int waitExit(DWORD timeoutMs = 20000) {
        if (!process) return -1;
        if (WaitForSingleObject(process, timeoutMs) != WAIT_OBJECT_0) {
            TerminateProcess(process, 99);
            return -2;
        }
        DWORD code = 99;
        GetExitCodeProcess(process, &code);
        return static_cast<int>(code);
    }
};
#endif

} // namespace

#ifdef BGMEDIA_HAVE_WAVE_OPUS
TEST_CASE(wave_real.opus_stereo_48k_and_plc) {
    using namespace bgmedia::wave;

    for (std::uint32_t bps : {128000u, 160000u}) {
        OpusWaveCodec codec(bps);
        CHECK(codec.isReady());
        CHECK(codec.bitrate() == bps);

        double phase = 0.0;
        std::vector<float> pcm;
        std::vector<std::uint8_t> encoded;
        std::vector<float> decoded(kWaveFrameSamples * 2, 0.0f);

        // Warm up encoder/decoder over 10 frames (200 ms)
        for (int f = 0; f < 10; ++f) {
            generateStereoSine(pcm, kWaveFrameSamples, phase);
            CHECK(codec.encode(pcm, encoded));
            CHECK(!encoded.empty());
            CHECK(encoded.size() < 1000u); // compressed vs 3840 bytes PCM16
            CHECK(codec.decode(encoded, decoded));
        }

        CHECK(computeRms(decoded) > 0.15);

        // Verify PLC produces non-zero audio without crashing
        std::vector<float> plc(kWaveFrameSamples * 2, 0.0f);
        CHECK(codec.decodePlc(plc));
        CHECK(computeRms(plc) > 0.01);
    }
}

TEST_CASE(wave_real.opus_error_and_bitrate_bounds) {
    using namespace bgmedia::wave;

    OpusWaveCodec lowCodec(1000u);
    CHECK(lowCodec.isReady());
    CHECK(lowCodec.bitrate() == kWaveMinBitrateBps);

    OpusWaveCodec highCodec(1000000u);
    CHECK(highCodec.isReady());
    CHECK(highCodec.bitrate() == kWaveMaxBitrateBps);

    lowCodec.setBitrate(0u);
    CHECK(lowCodec.bitrate() == kWaveMinBitrateBps);
    lowCodec.setBitrate(999999u);
    CHECK(lowCodec.bitrate() == kWaveMaxBitrateBps);
    lowCodec.setBitrate(144000u);
    CHECK(lowCodec.bitrate() == 144000u);
    lowCodec.setPacketLossPercent(-10);
    lowCodec.setPacketLossPercent(200);

    std::vector<std::uint8_t> encoded;
    std::vector<float> badPcm(16, 0.0f);
    CHECK(!lowCodec.encode(badPcm, encoded));

    std::vector<float> decoded(kWaveFrameSamples * 2, 0.0f);
    std::vector<float> shortOut(32, 0.0f);
    CHECK(!lowCodec.decode({}, decoded));
    std::vector<std::uint8_t> oversized(5000, 0xAAu);
    CHECK(!lowCodec.decode(oversized, decoded));
    CHECK(!lowCodec.decodePlc(shortOut));
    CHECK(!lowCodec.decodeFec({}, decoded));
    CHECK(!lowCodec.decodeFec(oversized, decoded));

    double phase = 0.0;
    std::vector<float> pcm;
    generateStereoSine(pcm, kWaveFrameSamples, phase);
    CHECK(lowCodec.encode(pcm, encoded));
    CHECK(!lowCodec.decode(encoded, shortOut));
    CHECK(!lowCodec.decodeFec(encoded, shortOut));
    CHECK(lowCodec.decode(encoded, decoded));

    lowCodec.reset();
    CHECK(lowCodec.encode(pcm, encoded));
    CHECK(lowCodec.decode(encoded, decoded));
}
#endif

#ifdef BGMEDIA_HAVE_WAVE_WEBRTC
TEST_CASE(wave_real.webrtc_inproc_p2p_opus_dtls_srtp) {
    using namespace bgmedia::wave;

    WebRtcTransportConfig hTxCfg;
    hTxCfg.role = WaveRole::Host;
    hTxCfg.stunServers.clear(); // deterministic localhost UDP
    hTxCfg.gatheringTimeoutMs = 4000;
    hTxCfg.connectTimeoutMs = 10000;

    WebRtcTransportConfig lTxCfg;
    lTxCfg.role = WaveRole::Listener;
    lTxCfg.stunServers.clear();
    lTxCfg.gatheringTimeoutMs = 4000;
    lTxCfg.connectTimeoutMs = 10000;

    auto hostTx = std::make_shared<WebRtcWaveTransport>(hTxCfg);
    auto listTx = std::make_shared<WebRtcWaveTransport>(lTxCfg);

    WavePipelineConfig hCfg;
    hCfg.role = WaveRole::Host;
    WavePipelineConfig lCfg;
    lCfg.role = WaveRole::Listener;

    WaveAudioPipeline host(hCfg, makeRealOrFallbackCodec(128000), hostTx);
    WaveAudioPipeline listener(lCfg, makeRealOrFallbackCodec(128000), listTx);

    std::string offerUri;
    std::string answerUri;
    std::atomic<bool> hostLive{false};
    std::atomic<bool> listLive{false};

    WavePipelineCallbacks hCb;
    hCb.onOutboundSignal = [&](const WaveSignalMessage& m) {
        if (m.type == WaveSignalType::Offer) {
            offerUri = encodeWaveHttpsFragmentUri(m);
        }
    };
    hCb.onStateChanged = [&](WaveSessionState st, const std::string&) {
        if (st == WaveSessionState::LiveP2P) hostLive.store(true);
    };
    host.setCallbacks(std::move(hCb));

    WavePipelineCallbacks lCb;
    lCb.onOutboundSignal = [&](const WaveSignalMessage& m) {
        if (m.type == WaveSignalType::Answer) {
            answerUri = encodeWaveHttpsFragmentUri(m);
        }
    };
    lCb.onStateChanged = [&](WaveSessionState st, const std::string&) {
        if (st == WaveSessionState::LiveP2P) listLive.store(true);
    };
    listener.setCallbacks(std::move(lCb));

    const std::int64_t nowUnix = 1700000000;
    CHECK(host.startOffer(nowUnix, 120));
    CHECK(offerUri.rfind("https://beygram.beykus.fun/#bgwave-v1.", 0) == 0);

    WaveSignalMessage decodedOffer;
    CHECK(decodeWaveUri(offerUri, decodedOffer));
    CHECK(listener.acceptOffer(decodedOffer, nowUnix + 1));
    CHECK(answerUri.rfind("https://beygram.beykus.fun/#bgwave-v1.", 0) == 0);

    WaveSignalMessage decodedAnswer;
    CHECK(decodeWaveUri(answerUri, decodedAnswer));
    CHECK(host.acceptAnswer(decodedAnswer, nowUnix + 2));

    const auto t0 = std::chrono::steady_clock::now();
    while ((!hostLive.load() || !listLive.load()) &&
           std::chrono::steady_clock::now() - t0 < std::chrono::seconds(10)) {
        const double t = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
        host.process(t);
        listener.process(t);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    CHECK(hostLive.load());
    CHECK(listLive.load());
    CHECK(hostTx->remoteFingerprintVerified());
    CHECK(listTx->remoteFingerprintVerified());
    CHECK(!hostTx->selectedCandidatePairSummary().empty());

    // Stream 2.5 seconds of continuous 48kHz stereo audio over real DTLS-SRTP
    double phase = 0.0;
    std::vector<float> inPcm;
    std::vector<float> outPcm(480 * 2, 0.0f);
    std::size_t nonSilentOut = 0;
    double sumSq = 0.0;
    std::size_t totalOutSamples = 0;

    for (int tick = 0; tick < 250; ++tick) {
        generateStereoSine(inPcm, 480, phase);
        host.onPcmRendered(inPcm.data(), 480, 48000);
        const double t = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
        host.process(t);
        listener.process(t);
        const std::size_t got =
            listener.pullListenerPcm(outPcm.data(), 480, 48000);
        if (got > 0) {
            for (std::size_t i = 0; i < got * 2; ++i) {
                sumSq += static_cast<double>(outPcm[i]) * outPcm[i];
                if (std::abs(outPcm[i]) > 1e-3f) {
                    ++nonSilentOut;
                }
            }
            totalOutSamples += got * 2;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    const auto hStats = host.stats();
    const auto lStats = listener.stats();
    CHECK(hStats.packetsSent >= 80u);
    CHECK(lStats.packetsReceived >= 60u);
    CHECK(lStats.bytesReceived > 0u);
    CHECK(nonSilentOut > 1000u);
    CHECK(totalOutSamples > 0u);
    CHECK(std::sqrt(sumSq / static_cast<double>(totalOutSamples)) > 0.05);
    CHECK(lStats.jitterBufferDepthMs <= 120u);

    // End broadcast mid-stream, then reconnect both peers
    host.stop(true, nowUnix + 10);
    listener.stop(false, nowUnix + 10);
    CHECK(host.state() == WaveSessionState::Ended);
    CHECK(listener.state() == WaveSessionState::Ended);

    offerUri.clear();
    answerUri.clear();
    hostLive.store(false);
    listLive.store(false);

    CHECK(host.reconnect(nowUnix + 20));
    CHECK(listener.reconnect(nowUnix + 20));
    CHECK(decodeWaveUri(offerUri, decodedOffer));
    CHECK(listener.acceptOffer(decodedOffer, nowUnix + 21));
    CHECK(decodeWaveUri(answerUri, decodedAnswer));
    CHECK(host.acceptAnswer(decodedAnswer, nowUnix + 22));

    const auto t1 = std::chrono::steady_clock::now();
    while ((!hostLive.load() || !listLive.load()) &&
           std::chrono::steady_clock::now() - t1 < std::chrono::seconds(10)) {
        const double t = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
        host.process(t);
        listener.process(t);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    CHECK(hostLive.load());
    CHECK(listLive.load());
    CHECK(hostTx->remoteFingerprintVerified());
    CHECK(listTx->remoteFingerprintVerified());
}

TEST_CASE(wave_real.webrtc_tampered_fingerprint_rejected) {
    using namespace bgmedia::wave;

    WebRtcTransportConfig hTxCfg;
    hTxCfg.role = WaveRole::Host;
    hTxCfg.stunServers.clear();
    hTxCfg.gatheringTimeoutMs = 4000;
    hTxCfg.connectTimeoutMs = 4000;

    WebRtcTransportConfig lTxCfg;
    lTxCfg.role = WaveRole::Listener;
    lTxCfg.stunServers.clear();
    lTxCfg.gatheringTimeoutMs = 4000;
    lTxCfg.connectTimeoutMs = 4000;

    auto hostTx = std::make_shared<WebRtcWaveTransport>(hTxCfg);
    auto listTx = std::make_shared<WebRtcWaveTransport>(lTxCfg);

    WavePipelineConfig hCfg; hCfg.role = WaveRole::Host;
    WavePipelineConfig lCfg; lCfg.role = WaveRole::Listener;
    WaveAudioPipeline host(hCfg, makeRealOrFallbackCodec(), hostTx);
    WaveAudioPipeline listener(lCfg, makeRealOrFallbackCodec(), listTx);

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

    CHECK(host.startOffer(1700000000));
    CHECK(listener.acceptOffer(offer, 1700000001));

    // Tamper the first hex character of the remote DTLS SHA-256 fingerprint.
    CHECK(!answer.signal.dtlsFingerprint.empty());
    answer.signal.dtlsFingerprint[0] =
        (answer.signal.dtlsFingerprint[0] == 'A' ||
         answer.signal.dtlsFingerprint[0] == 'a')
            ? 'B'
            : 'A';

    CHECK(host.acceptAnswer(answer, 1700000002));

    const auto t0 = std::chrono::steady_clock::now();
    while (host.state() == WaveSessionState::ConnectingP2P &&
           std::chrono::steady_clock::now() - t0 < std::chrono::seconds(6)) {
        const double t = std::chrono::duration<double>(
                             std::chrono::steady_clock::now() - t0)
                             .count();
        host.process(t);
        listener.process(t);
        std::this_thread::sleep_for(std::chrono::milliseconds(15));
    }

    CHECK(!hostTx->remoteFingerprintVerified());
    CHECK(hostTx->state() == WaveTransportState::Failed);
    CHECK(host.state() == WaveSessionState::P2PFailed);
    CHECK(host.state() != WaveSessionState::LiveP2P);
}

TEST_CASE(wave_real.webrtc_concurrent_stop_reset_callbacks) {
    using namespace bgmedia::wave;

    WebRtcTransportConfig hTxCfg;
    hTxCfg.role = WaveRole::Host;
    hTxCfg.stunServers.clear();
    hTxCfg.gatheringTimeoutMs = 3000;
    WebRtcTransportConfig lTxCfg;
    lTxCfg.role = WaveRole::Listener;
    lTxCfg.stunServers.clear();
    lTxCfg.gatheringTimeoutMs = 3000;

    auto hostTx = std::make_shared<WebRtcWaveTransport>(hTxCfg);
    auto listTx = std::make_shared<WebRtcWaveTransport>(lTxCfg);

    WavePipelineConfig hCfg; hCfg.role = WaveRole::Host;
    WavePipelineConfig lCfg; lCfg.role = WaveRole::Listener;
    WaveAudioPipeline host(hCfg, makeRealOrFallbackCodec(), hostTx);
    WaveAudioPipeline listener(lCfg, makeRealOrFallbackCodec(), listTx);

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

    CHECK(host.startOffer(1700000000));
    CHECK(listener.acceptOffer(offer, 1700000001));
    CHECK(host.acceptAnswer(answer, 1700000002));

    std::atomic<bool> stopThreads{false};
    std::thread killer([&]() {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        host.stop(true, 1700000010);
        listener.reset();
        stopThreads.store(true);
    });

    while (!stopThreads.load()) {
        host.process(0.1);
        listener.process(0.1);
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    killer.join();
    CHECK(host.state() == WaveSessionState::Ended);
    CHECK(listener.state() == WaveSessionState::Idle);
}

#if defined(_WIN32) && defined(BGMEDIA_WAVE_P2P_PEER_EXE)
TEST_CASE(wave_real.webrtc_two_processes_localhost) {
    WinPeerProcess hostProc;
    WinPeerProcess listProc;
    CHECK(hostProc.start("--role host --duration-ms 1200"));
    CHECK(listProc.start("--role listener --duration-ms 1200"));

    std::string offerLine;
    CHECK(hostProc.readLine(offerLine));
    CHECK(offerLine.rfind("OFFER https://beygram.beykus.fun/#bgwave-v1.", 0) == 0);
    CHECK(listProc.writeLine(offerLine));

    std::string answerLine;
    CHECK(listProc.readLine(answerLine));
    CHECK(answerLine.rfind("ANSWER https://beygram.beykus.fun/#bgwave-v1.", 0) == 0);
    CHECK(hostProc.writeLine(answerLine));

    std::string hostStats;
    std::string listStats;
    CHECK(hostProc.readLine(hostStats));
    CHECK(listProc.readLine(listStats));

    CHECK(hostProc.waitExit(20000) == 0);
    CHECK(listProc.waitExit(20000) == 0);

    CHECK(hostStats.find("fingerprintVerified=1") != std::string::npos);
    CHECK(listStats.find("fingerprintVerified=1") != std::string::npos);
}
#endif

TEST_CASE(wave_real.rtp_sequence_rollover_reordered) {
    using namespace bgmedia::wave;

    // Case A: 65534, 65535, 0, 65535(delayed), 1, 2
    {
        RtpSequenceUnwrapper u;
        const std::vector<std::uint16_t> inSeqs = {65534, 65535, 0, 65535, 1, 2};
        const std::vector<std::uint32_t> expected = {65534, 65535, 65536, 65535, 65537, 65538};
        WaveJitterBuffer jb;
        for (std::size_t i = 0; i < inSeqs.size(); ++i) {
            const std::uint32_t seq32 = u.unwrap(inSeqs[i]);
            CHECK(seq32 == expected[i]);
            WaveMediaPacket pkt;
            pkt.sequence = seq32;
            pkt.timestamp48k = seq32 * static_cast<std::uint32_t>(kWaveFrameSamples);
            pkt.payload = {static_cast<std::uint8_t>(i + 1)};
            jb.push(std::move(pkt), static_cast<double>(i) * 0.02);
        }
        // Verify continuity through 65534..65538 with no stream corruption.
        for (std::uint32_t want = 65534; want <= 65538; ++want) {
            WaveMediaPacket popped;
            CHECK(jb.popNext(popped) == WaveJitterBuffer::PopStatus::Packet);
            CHECK(popped.sequence == want);
        }
    }

    // Case B: 65535, 0, 1, 65535(delayed), 2
    {
        RtpSequenceUnwrapper u;
        const std::vector<std::uint16_t> inSeqs = {65535, 0, 1, 65535, 2};
        const std::vector<std::uint32_t> expected = {65535, 65536, 65537, 65535, 65538};
        WaveJitterBuffer jb;
        for (std::size_t i = 0; i < inSeqs.size(); ++i) {
            const std::uint32_t seq32 = u.unwrap(inSeqs[i]);
            CHECK(seq32 == expected[i]);
            WaveMediaPacket pkt;
            pkt.sequence = seq32;
            pkt.timestamp48k = seq32 * static_cast<std::uint32_t>(kWaveFrameSamples);
            pkt.payload = {static_cast<std::uint8_t>(i + 1)};
            jb.push(std::move(pkt), static_cast<double>(i) * 0.02);
        }
        for (std::uint32_t want = 65535; want <= 65538; ++want) {
            WaveMediaPacket popped;
            CHECK(jb.popNext(popped) == WaveJitterBuffer::PopStatus::Packet);
            CHECK(popped.sequence == want);
        }
    }
}

TEST_CASE(wave_real.rtp_sequence_initial_backward_wraparound) {
    using namespace bgmedia::wave;

    {
        RtpSequenceUnwrapper u;
        CHECK(u.unwrap(0) == 0u);
        CHECK(u.unwrap(65535) == 0xFFFFFFFFu);
        CHECK(u.cycles == 0u);
        CHECK(u.maxSeq16 == 0u);
        CHECK(u.unwrap(65534) == 0xFFFFFFFEu);
        CHECK(u.cycles == 0u);
        CHECK(u.unwrap(1) == 1u);
        CHECK(u.cycles == 0u);
        CHECK(u.maxSeq16 == 1u);
        CHECK(u.unwrap(2) == 2u);
    }

    {
        RtpSequenceUnwrapper u;
        WaveJitterBuffer jb;
        const std::vector<std::uint16_t> inSeqs = {0, 65535, 1, 2, 3};
        for (std::size_t i = 0; i < inSeqs.size(); ++i) {
            WaveMediaPacket pkt;
            pkt.sequence = u.unwrap(inSeqs[i]);
            pkt.timestamp48k = pkt.sequence * static_cast<std::uint32_t>(kWaveFrameSamples);
            pkt.payload = {static_cast<std::uint8_t>(i + 1)};
            jb.push(std::move(pkt), static_cast<double>(i) * 0.02);
        }
        CHECK(jb.packetsDroppedLate() == 1u);
        for (std::uint32_t want = 0; want <= 3; ++want) {
            WaveMediaPacket popped;
            CHECK(jb.popNext(popped) == WaveJitterBuffer::PopStatus::Packet);
            CHECK(popped.sequence == want);
        }
    }
}

TEST_CASE(wave_real.rtp_header_parser_bounds_and_padding) {
    using namespace bgmedia::wave;

    auto makeBaseRtp = [](std::uint8_t flags0,
                          std::uint8_t flags1,
                          std::uint16_t seq,
                          std::uint32_t ts,
                          std::uint32_t ssrc) {
        std::vector<std::uint8_t> pkt(12, 0);
        pkt[0] = flags0;
        pkt[1] = flags1;
        pkt[2] = static_cast<std::uint8_t>((seq >> 8) & 0xFFu);
        pkt[3] = static_cast<std::uint8_t>(seq & 0xFFu);
        pkt[4] = static_cast<std::uint8_t>((ts >> 24) & 0xFFu);
        pkt[5] = static_cast<std::uint8_t>((ts >> 16) & 0xFFu);
        pkt[6] = static_cast<std::uint8_t>((ts >> 8) & 0xFFu);
        pkt[7] = static_cast<std::uint8_t>(ts & 0xFFu);
        pkt[8] = static_cast<std::uint8_t>((ssrc >> 24) & 0xFFu);
        pkt[9] = static_cast<std::uint8_t>((ssrc >> 16) & 0xFFu);
        pkt[10] = static_cast<std::uint8_t>((ssrc >> 8) & 0xFFu);
        pkt[11] = static_cast<std::uint8_t>(ssrc & 0xFFu);
        return pkt;
    };

    ParsedRtpPacket parsed;

    // Truncated packet (< 12 bytes) and wrong version.
    CHECK(!parseRtpPacket({}, parsed));
    std::vector<std::uint8_t> tooShort(11, 0x80u);
    CHECK(!parseRtpPacket(tooShort, parsed));
    auto wrongVer = makeBaseRtp(0x40u, 111u, 1u, 960u, 0x11223344u);
    CHECK(!parseRtpPacket(wrongVer, parsed));

    // Valid minimal v2 packet with marker + payload.
    auto valid = makeBaseRtp(0x80u, static_cast<std::uint8_t>(0x80u | 111u),
                             42u, 40320u, 0xAABBCCDDu);
    valid.push_back(0x01u);
    valid.push_back(0x02u);
    valid.push_back(0x03u);
    CHECK(parseRtpPacket(valid, parsed));
    CHECK(parsed.payloadType == 111u);
    CHECK(parsed.marker);
    CHECK(parsed.sequence == 42u);
    CHECK(parsed.timestamp == 40320u);
    CHECK(parsed.ssrc == 0xAABBCCDDu);
    CHECK(parsed.payloadOffset == 12u);
    CHECK(parsed.payloadSize == 3u);

    // CSRC bounds check (CC = 2 -> needs 20 bytes).
    auto badCsrc = makeBaseRtp(0x82u, 111u, 1u, 0u, 1u);
    badCsrc.resize(16, 0u);
    CHECK(!parseRtpPacket(badCsrc, parsed));

    // Extension bit set with truncated extension header (< baseHdr + 4).
    auto truncExt = makeBaseRtp(0x90u, 111u, 1u, 0u, 1u);
    truncExt.push_back(0xBEu);
    truncExt.push_back(0xDEu);
    CHECK(!parseRtpPacket(truncExt, parsed));

    // Extension length exceeds packet bounds.
    auto oobExt = makeBaseRtp(0x90u, 111u, 1u, 0u, 1u);
    oobExt.push_back(0xBEu);
    oobExt.push_back(0xDEu);
    oobExt.push_back(0x00u);
    oobExt.push_back(0x0Au); // 10 * 4 = 40 bytes extension body
    oobExt.resize(20, 0u);
    CHECK(!parseRtpPacket(oobExt, parsed));

    // Valid extension (1 word = 4 bytes after 4-byte ext header = 8 bytes).
    auto validExt = makeBaseRtp(0x90u, 111u, 7u, 960u, 0x12345678u);
    validExt.insert(validExt.end(), {0xBEu, 0xDEu, 0x00u, 0x01u, 0xAAu, 0xBBu, 0xCCu, 0xDDu});
    validExt.insert(validExt.end(), {0x11u, 0x22u, 0x33u, 0x44u, 0x55u});
    CHECK(parseRtpPacket(validExt, parsed));
    CHECK(parsed.payloadOffset == 20u);
    CHECK(parsed.payloadSize == 5u);

    // Malformed padding: no padding byte, zero padLen, and padLen > remaining.
    auto emptyPad = makeBaseRtp(0xA0u, 111u, 1u, 0u, 1u);
    CHECK(!parseRtpPacket(emptyPad, parsed));

    auto zeroPad = makeBaseRtp(0xA0u, 111u, 1u, 0u, 1u);
    zeroPad.insert(zeroPad.end(), {0x10u, 0x00u});
    CHECK(!parseRtpPacket(zeroPad, parsed));

    auto overPad = makeBaseRtp(0xA0u, 111u, 1u, 0u, 1u);
    overPad.insert(overPad.end(), {0x10u, 0x05u});
    CHECK(!parseRtpPacket(overPad, parsed));

    // Valid CSRC(1) + Extension(1 word) + Padding(2 bytes) + 4 payload bytes.
    auto combined = makeBaseRtp(0xB1u, 111u, 99u, 1920u, 0xCAFEBABEu);
    combined.insert(combined.end(), {0x00u, 0x00u, 0x00u, 0x01u}); // 1 CSRC
    combined.insert(combined.end(), {0xBEu, 0xDEu, 0x00u, 0x01u, 0xAAu, 0xBBu, 0xCCu, 0xDDu}); // Ext
    combined.insert(combined.end(), {0x01u, 0x02u, 0x03u, 0x04u}); // Payload
    combined.insert(combined.end(), {0x00u, 0x02u}); // 2 bytes padding
    CHECK(parseRtpPacket(combined, parsed));
    CHECK(parsed.payloadOffset == 24u);
    CHECK(parsed.payloadSize == 4u);
}

TEST_CASE(wave_real.ice_candidate_selection_and_budget) {
    using namespace bgmedia::wave;

    WaveSdpSignal base;
    base.iceUfrag = "ufrag1234";
    base.icePwd = "password1234567890123456";
    base.dtlsFingerprintAlg = "sha-256";
    base.dtlsFingerprint =
        "00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF:"
        "00:11:22:33:44:55:66:77:88:99:AA:BB:CC:DD:EE:FF";
    base.setupRole = WaveDtlsSetupRole::Actpass;

    std::vector<WaveIceCandidate> cands;
    // 36 host candidates first (would crowd out srflx if blindly taking first 32)
    for (int i = 0; i < 36; ++i) {
        WaveIceCandidate c;
        c.foundation = std::to_string(i + 1);
        c.component = 1;
        c.protocol = WaveIceProtocol::Udp;
        c.priority = static_cast<std::uint32_t>(2122260223u - i);
        c.address = "192.168." + std::to_string(i / 10 + 1) + "." + std::to_string((i % 10) + 10);
        c.port = static_cast<std::uint16_t>(50000 + i);
        c.type = WaveIceCandidateType::Host;
        cands.push_back(std::move(c));
    }
    // Late srflx IPv4 and IPv6 candidates
    WaveIceCandidate srflx4;
    srflx4.foundation = "40";
    srflx4.protocol = WaveIceProtocol::Udp;
    srflx4.priority = 1686052607u;
    srflx4.address = "203.0.113.45";
    srflx4.port = 61001;
    srflx4.type = WaveIceCandidateType::Srflx;
    cands.push_back(srflx4);

    WaveIceCandidate srflx6;
    srflx6.foundation = "41";
    srflx6.protocol = WaveIceProtocol::Udp;
    srflx6.priority = 1686052606u;
    srflx6.address = "2001:db8::45";
    srflx6.port = 61002;
    srflx6.type = WaveIceCandidateType::Srflx;
    cands.push_back(srflx6);

    WaveSdpSignal selected;
    CHECK(selectCompactIceCandidates(base, cands, selected));
    CHECK(selected.candidates.size() == kWaveMaxCandidates);

    bool hasSrflx4 = false;
    bool hasSrflx6 = false;
    for (const auto& c : selected.candidates) {
        if (c.type == WaveIceCandidateType::Srflx && c.address == "203.0.113.45") {
            hasSrflx4 = true;
        }
        if (c.type == WaveIceCandidateType::Srflx && c.address == "2001:db8::45") {
            hasSrflx6 = true;
        }
    }
    CHECK(hasSrflx4);
    CHECK(hasSrflx6);

    WaveSignalMessage msg;
    msg.sid.fill(0x42);
    msg.iat = 1700000000;
    msg.exp = 1700000120;
    msg.nonce = 7;
    msg.signal = selected;
    std::vector<std::uint8_t> bin;
    CHECK(encodeWaveSignalBinary(msg, bin));
    CHECK(bin.size() <= kWaveMaxBinaryPayloadBytes);
    const std::string uri = encodeWaveHttpsFragmentUri(msg);
    CHECK(!uri.empty());
    CHECK(uri.size() <= kWaveMaxUriLength);

    // Tight byte budget trims candidate count to fit instead of failing later.
    WaveSdpSignal tight;
    CHECK(selectCompactIceCandidates(base, cands, tight, kWaveMaxCandidates, 220));
    CHECK(!tight.candidates.empty());
    CHECK(tight.candidates.size() < kWaveMaxCandidates);
    msg.signal = tight;
    bin.clear();
    CHECK(encodeWaveSignalBinary(msg, bin));
    CHECK(bin.size() <= 220u);

    // Impossibly small byte budget fails explicitly.
    WaveSdpSignal impossible;
    CHECK(!selectCompactIceCandidates(base, cands, impossible, kWaveMaxCandidates, 60));
}
#endif
