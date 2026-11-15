# BG Wave — Portable P2P Audio Engine & Signaling Contract (v1)

## 1. Architecture & Core Boundaries

In accordance with [docs/ARCHITECTURE.md](ARCHITECTURE.md), `bgmedia_core` remains a pure C++20 library with **zero** dependencies on SDL, miniaudio, JNI, Android, TDLib, NTgCalls, or audio output devices.

BG Wave is split into:

- **Engine Core (`include/bgmedia/wave/`, `src/wave/`)**:
  - [`IPcmOutputTap`](../include/bgmedia/audio/IPcmOutputTap.hpp): real-time-safe audio-thread observer attached to [`MixEngine::setPcmTap()`](../include/bgmedia/mix/MixEngine.hpp) (retired via `renderEpoch_`, zero locks, zero allocations). Hosts on Android/JNI can also call `WaveAudioPipeline::pushHostPcm()` directly from their audio callback.
  - [`WavePcmRingBuffer`](../include/bgmedia/wave/WaveRingBuffer.hpp): bounded, lock-free SPSC ring buffer for interleaved stereo `float32` PCM with NaN/Inf sanitization, `[-1, 1]` clipping control, and atomic overrun/underrun counters.
  - [`WaveStereoResampler`](../include/bgmedia/wave/WaveResampler.hpp): streaming Catmull-Rom cubic stereo resampler used **off** the audio thread only when host sample rate != 48 kHz (bypassed at 48 kHz).
  - [`WaveJitterBuffer`](../include/bgmedia/wave/WaveJitterBuffer.hpp): bounded (64-slot) adaptive jitter buffer with `uint32_t` wrap-around handling, reordering, duplicate suppression, RFC 3550-style EWMA jitter estimation, and `MissingPlc` signaling.
  - [`IWaveAudioCodec`](../include/bgmedia/wave/WaveCodec.hpp) + built-in `Pcm16WaveCodec` reference codec (48 kHz stereo, 20 ms / 960-sample frames, fade-out PLC).
  - [`WaveSignaling`](../include/bgmedia/wave/WaveSignaling.hpp): versioned (`v=1`) compact binary codec, `bg://wave/v1/...` and `https://beygram.beykus.fun/#bgwave-v1....` URI helpers, SDP <-> `WaveSdpSignal` parser/formatter (IPv4, IPv6, mDNS `.local`, multi-interface), and diagnostic redaction.
  - [`IWaveTransport`](../include/bgmedia/wave/WaveTransport.hpp) + `SimulatedWaveTransport` (deterministic in-process loopback/network simulator for spike benchmarks, unit tests, and CI verification).
  - [`IExternalCallTransport`](../include/bgmedia/wave/WaveFallback.hpp): host-implemented external call PCM bridge for phase-2 Telegram Call fallback (no Telegram/TDLib code in BG-Media).
  - [`WaveAudioPipeline`](../include/bgmedia/wave/WaveAudioPipeline.hpp): unified Host/Listener state machine, instant `setPublishingEnabled(bool)` revocation, and redacted diagnostics (bitrate defaults to `WavePipelineConfig::targetBitrateBps`; `WebRtcTransportConfig::onBitrateFeedback` exposes Host-side RTT-based bitrate suggestions that the host app can wire to `IWaveAudioCodec::setBitrate()`, as `WaveAudioPipeline` does not wire it automatically).
- **Optional Codec Adapter (`adapters/wave_opus/`, `BGMEDIA_BUILD_WAVE_OPUS=OFF` by default)**:
  - `OpusWaveCodec`: wraps `libopus` (BSD-3-Clause) at 48 kHz stereo, 20 ms frames, 128–160 kbit/s music profile (default 128 kbit/s, clamped `48–160` kbit/s), in-band FEC, and Opus PLC.
- **Optional WebRTC Transport Adapter (`adapters/wave_webrtc/`, `BGMEDIA_BUILD_WAVE_WEBRTC=OFF` by default)**:
  - `WebRtcWaveTransport`: wraps `libdatachannel` (MPL-2.0) for real ICE/STUN + DTLS-SRTP audio transport.

## 2. Session State Machine

```text
Idle ──► Offering ──► AwaitingAnswer ──► ConnectingP2P ──► LiveP2P ──► Ended
                                                │
                                                ▼
                                            P2PFailed ──(reconnect)──► Offering / Idle
                                                │
                                                ▼ (phase-2 fallback)
                                  OfferingTelegramFallback
                                                │
                                                ▼
                                   ConnectingTelegramCall ──► LiveTelegramCall ──► Ended
                                                │
                                                ▼
                                              Failed
```

## 3. Wire Contract BG Wave v1

Binary layout (`kWaveMaxBinaryPayloadBytes = 2048`, `kWaveMaxCandidates = 32`):

| Offset / Field | Size | Description |
|---|---|---|
| Magic | 2 B | `'B'`, `'W'` |
| `version` | 1 B | `1` (unknown versions rejected cleanly) |
| `type` | 1 B | `1=Offer`, `2=Answer`, `3=Cancel`, `4=End` |
| `sid` | 16 B | 128-bit random session ID (non-zero) |
| `iat` | 8 B | Big-endian `int64` Unix seconds |
| `exp` | 8 B | Big-endian `int64` Unix seconds |
| `caps` | 4 B | Big-endian `uint32` capability flags |
| `nonce` | 8 B | Big-endian `uint64` replay-protection nonce |
| `iceUfrag`, `icePwd`, `dtlsAlg` | len+UTF8 | Bounded `<= 128` bytes each |
| `dtlsFingerprint` | 1+32 B | Mode `1`: raw 32-byte SHA-256 (or mode `0` string) |
| `setupRole`, `candCount` | 1+1 B | `0=actpass, 1=active, 2=passive`; `candCount <= 32` |
| `candidates[]` | var | `AddrKind::Ipv4` (`1`) as 4 B, `AddrKind::Ipv6` (`2`) as 16 B (without zone ID), `AddrKind::Hostname` (`3`) as 1 B len + UTF-8 (for mDNS `.local` and other unparsed hostnames/addresses such as IPv6 with zone ID) |

URIs:
- Internal app URI: `bg://wave/v1/<base64url(payload)>`
- External HTTPS fragment URI: `https://beygram.beykus.fun/#bgwave-v1.<base64url(payload)>`

## 4. Spike Measurements (`bgmedia_wave_spike`)

Measured on Windows x64 Release (`core-only` preset):

| Scenario | Raw SDP (B) | Candidates | Binary (B) | `bg://` URI (chars) | `https://` URI (chars) |
|---|---:|---:|---:|---:|---:|
| IPv4 host + srflx | 392 | 2 | 155 | 220 | 245 |
| Dual-stack IPv4 + IPv6 + mDNS | 558 | 4 | 236 | 328 | 353 |
| Multi-interface (6 candidates) | 644 | 6 | 240 | 333 | 358 |

- **PCM Tap Overhead**: `MixEngine::render()` 512-frame callback (10.67 ms budget at 48 kHz) averaged ~2.5–4.8 µs (< 0.05% of callback budget).
- **1-Hour Continuous Loopback Stream** (180,000 × 20 ms frames, 2% loss, 10 ms jitter, 5% reordering): 180,000 packets sent, 176,533 received, 4,833 PLC frames, `jitterMs=9.98 ms`, `depthMs=40 ms`, zero crashes, bounded memory.

## 5. Licenses & Optional Dependencies

- `bgmedia_core` BG Wave: repository MIT license (no new third-party dependencies).
- Optional `adapters/wave_opus`: `libopus` (BSD-3-Clause), disabled by default (`BGMEDIA_BUILD_WAVE_OPUS=OFF`).
- Optional `adapters/wave_webrtc`: `libdatachannel` v0.22.5 + `libjuice` + `libsrtp` + OpenSSL (MPL-2.0), disabled by default (`BGMEDIA_BUILD_WAVE_WEBRTC=OFF`).

## 6. Current Scope & E0/E1 Interoperability Notes

- **Canonical Starting Point (`WaveSignaling` v1):** The C++20 binary codec (`16`-byte `sid`, `uint64` `nonce`, `uint32` `caps`, `iat`/`exp`, bounded ICE/DTLS fields) is the canonical starting point for BG Media and BeyGram JNI integration. Final v1 wire freeze requires E0 real-SDP peer-connection validation and Telegram `text-url` round-trip size verification.
- **SDP Subset Boundary:** `parseSdpToWaveSignal()` extracts only the compact ICE/DTLS subset (`ice-ufrag`, `ice-pwd`, `fingerprint`, `setup`, `candidate`) and ignores unrelated/optional SDP attributes (`mid`, `ice-options`, `rtcp-mux`, `raddr`/`rport`, `msid`, etc.). Conversely, `formatWaveSignalToSdp()` formats those fields into a fixed v1 audio SDP template (`m=audio 9 UDP/TLS/RTP/SAVPF 111`, `a=mid:audio`, `a=rtcp-mux`, `a=rtpmap:111 opus/48000/2`, `a=fmtp:111 ...`, `a=sendonly`/`a=recvonly`). It does **not** claim general SDP equivalence outside this v1 audio track profile.
- **Canonical Golden Fixtures:** Byte-level valid (`offer`, `answer`, `cancel`, `end`) and negative (`invalid_*`) fixtures plus a sanitized WebRTC SDP fixture are published in `tests/fixtures/wave_v1/` and verified by `bgmedia.wave`.
- **Threading Contract:** `onPcmRendered()` / `pullListenerPcm()` are lock-free and allocation-free on the real-time audio thread; `IWaveTransport::onMediaPacket` is thread-safe and enqueues into a bounded (`256`-packet) `inboundQueue_` drained by `process()`; user callbacks are always dispatched with `controlMutex_` unlocked and guarded by `sessionGen_`.
- **Transport & Codec Status:** `SimulatedWaveTransport` is an in-process deterministic loopback/network simulator (`linkPeers()` + `poll(nowSec)`) for `bgmedia_wave_spike` benchmarks, core pipeline unit tests, and CI verification—**not** a real network transport or proof of cross-device P2P NAT traversal. The real ICE/DTLS-SRTP transport (`adapters/wave_webrtc`) and `libopus` (`adapters/wave_opus`) are implemented as optional adapters.

## 7. Real WebRTC P2P Transport (`adapters/wave_webrtc`) & BeyGram JNI Contract

- **Build Options:**
  - `BGMEDIA_BUILD_WAVE_OPUS=ON` (fetches/uses `libopus` 1.5.2, BSD-3-Clause)
  - `BGMEDIA_BUILD_WAVE_WEBRTC=ON` (fetches/uses `libdatachannel` v0.22.5 + `libjuice` + `libsrtp` + OpenSSL, MPL-2.0)
  - `BGMEDIA_FETCH_WAVE_DEPS=ON` (enabled by default when adapters are turned on)
  - `bgmedia_core` remains pure C++20 with `BGMEDIA_BUILD_WAVE_OPUS=OFF` and `BGMEDIA_BUILD_WAVE_WEBRTC=OFF`.
- **Transport Mechanism (DTLS-SRTP Media Track, NOT DataChannel):**
  - `WebRtcWaveTransport` uses a WebRTC Audio Media Track (`m=audio 9 UDP/TLS/RTP/SAVPF 111`, `opus/48000/2`, `a=mid:audio`, `a=rtcp-mux`) with `rtc::OpusRtpPacketizer` + `rtc::RtcpSrReporter` on the Host and `rtc::RtcpReceivingSession` on the Listener.
  - DTLS-SRTP avoids SCTP head-of-line blocking and carries native 16-bit RTP sequence numbers (extended to 32-bit with wraparound via `RtpSequenceUnwrapper`) and 48 kHz RTP timestamps directly into `WaveJitterBuffer`.
  - **Bitrate Feedback (`onBitrateFeedback`):** On the Host side, `WebRtcWaveTransport::poll()` checks `peer->rtt()` at most once per second while `Connected` and invokes `WebRtcTransportConfig::onBitrateFeedback(suggestedBitrateBps)` (`96000` for RTT > 250 ms, `112000` for RTT > 150 ms, or `kWaveDefaultBitrateBps` = `128000` bps otherwise). `WebRtcWaveTransport` does not currently parse RTCP Receiver Report (RR) loss fractions, and `WaveAudioPipeline` does not automatically wire `onBitrateFeedback` to `IWaveAudioCodec::setBitrate()`—host applications wanting RTT-based adaptation should bind `onBitrateFeedback` to update `OpusWaveCodec::setBitrate()` on the same thread driving `pipeline.process()` (so it is serialized with `encode()`).
- **Security & Endpoint Identity:**
  - All media is encrypted end-to-end with DTLS-SRTP; unencrypted UDP is never used.
  - `disableFingerprintVerification` is kept `false` in `libdatachannel`, and `WebRtcWaveTransport` additionally enforces an exact SHA-256 `remoteFingerprint()` match against the signaling `dtlsFingerprint` when `PeerConnection::State::Connected` is reached (closing and failing on mismatch).
- **Wire Codec v1 Compatibility:**
  - Canonical binary wire codec v1 (`offer.bin`, `answer.bin`, `cancel.bin`, `end.bin`) and `https://beygram.beykus.fun/#bgwave-v1.<base64url(payload)>` URIs are unchanged and remain backwards-compatible.
  - `formatWaveSignalToSdp()` emits `a=mid:audio` and `a=rtcp-mux` (constant for the v1 audio track). `WebRtcWaveTransport` uses `selectCompactIceCandidates()` to rank candidates and cap them to `kWaveMaxCandidates` (32) within the `kWaveMaxBinaryPayloadBytes` (2048 B) / `kWaveMaxUriLength` (4096 chars) wire budget before encoding.
- **NAT / CGNAT Limitations (No TURN):**
  - `libjuice` handles IPv4, IPv6, STUN (`srflx`), and cone/restricted NAT traversal. Because BG Media does not ship or host a TURN relay server, symmetric NAT / strict CGNAT pairs that disallow direct UDP hole-punching will transition to `P2PFailed`, at which point BeyGram should invoke its Telegram Call fallback (`IExternalCallTransport`).
- **JNI Integration Notes for BeyGram (Claude):**
  1. Instantiate `OpusWaveCodec(kWaveDefaultBitrateBps)` and `WebRtcWaveTransport(WebRtcTransportConfig{role, stunServers, ...})`, pass both to `WaveAudioPipeline`.
  2. Call `startOffer(nowUnixSeconds)` on Host and send `encodeWaveHttpsFragmentUri(msg)` via TDLib hidden link; on Listener call `decodeWaveUri(uri)` -> `acceptOffer(offer, nowUnixSeconds)` -> send Answer URI back -> Host calls `acceptAnswer(answer, nowUnixSeconds)` (or pass incoming signals to `handleRemoteSignal(msg, nowUnixSeconds)`). Pass current Unix epoch seconds (`nowUnixSeconds > 0`) in production so `validateWaveSignal()` enforces `exp` expiry and the 60 s `iat` future clock-skew check; passing `nowUnixSeconds <= 0` (e.g. `0`) skips `iat`/`exp` expiry/skew validation in `validateWaveSignal()` (intended for deterministic tests), while outbound `iat` generation (`resolveTimestamp`) falls back to `std::chrono::system_clock::now()`.
  3. Drive `pipeline.process(nowSec)` periodically (~10–20 ms) off the audio thread; feed captured/mixed PCM via `onPcmRendered()` / `pushHostPcm()` and read playback PCM via `pullListenerPcm()`.
  4. On `WaveSessionState::P2PFailed`, trigger Telegram Group/Private Call fallback via `switchToExternalCallFallback(externalCall)` with an `IExternalCallTransport` implementation without re-creating `WaveAudioPipeline`.
  5. On Android, package `libcrypto`/`libssl` (OpenSSL) or configure `FindOrFetchLibDataChannel` for the Android NDK toolchain (`CMAKE_ANDROID_ARCH_ABI`).
