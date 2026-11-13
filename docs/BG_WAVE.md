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
  - [`IWaveTransport`](../include/bgmedia/wave/WaveTransport.hpp) + `SimulatedWaveTransport` (deterministic latency/jitter/loss/reorder/failure simulator).
  - [`IExternalCallTransport`](../include/bgmedia/wave/WaveFallback.hpp): host-implemented external call PCM bridge for phase-2 Telegram Call fallback (no Telegram/TDLib code in BG-Media).
  - [`WaveAudioPipeline`](../include/bgmedia/wave/WaveAudioPipeline.hpp): unified Host/Listener state machine, instant `setPublishingEnabled(bool)` revocation, bitrate adaptation on loss, and redacted diagnostics.
- **Optional Codec Adapter (`adapters/wave_opus/`, `BGMEDIA_BUILD_WAVE_OPUS=OFF` by default)**:
  - `OpusWaveCodec`: wraps `libopus` (BSD-3-Clause) at 48 kHz stereo, 20 ms frames, 128–160 kbit/s music profile, in-band FEC, and Opus PLC.

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
| `candidates[]` | var | IPv4 stored as 4 B, IPv6 as 16 B, mDNS `.local` as len+UTF8 |

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
- Future WebRTC adapter (`libdatachannel`, MPL-2.0): not bundled in `bgmedia_core`; planned as an optional adapter target.
