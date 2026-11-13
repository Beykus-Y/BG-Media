#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace bgmedia::wave {

inline constexpr std::uint8_t kWaveSignalVersion = 1;
inline constexpr std::size_t kWaveMaxCandidates = 32;
inline constexpr std::size_t kWaveMaxBinaryPayloadBytes = 2048;
inline constexpr std::size_t kWaveMaxUriLength = 4096;
inline constexpr std::size_t kWaveMaxSdpBytes = 16384;

enum class WaveSignalType : std::uint8_t {
    Offer = 1,
    Answer = 2,
    Cancel = 3,
    End = 4
};

enum WaveCapabilityFlags : std::uint32_t {
    kWaveCapNone = 0,
    kWaveCapOpus48kStereo = 1u << 0,
    kWaveCapDtlsSrtp = 1u << 1,
    kWaveCapExternalCallFallback = 1u << 2,
    kWaveCapInbandFec = 1u << 3
};

enum class WaveIceProtocol : std::uint8_t {
    Udp = 0,
    Tcp = 1
};

enum class WaveIceCandidateType : std::uint8_t {
    Host = 0,
    Srflx = 1,
    Prflx = 2,
    Relay = 3
};

enum class WaveIceTcpType : std::uint8_t {
    None = 0,
    Active = 1,
    Passive = 2,
    So = 3
};

enum class WaveDtlsSetupRole : std::uint8_t {
    Actpass = 0,
    Active = 1,
    Passive = 2
};

struct WaveIceCandidate {
    std::string foundation = "1";
    std::uint8_t component = 1;
    WaveIceProtocol protocol = WaveIceProtocol::Udp;
    std::uint32_t priority = 0;
    std::string address; // IPv4, IPv6, or mDNS .local hostname
    std::uint16_t port = 0;
    WaveIceCandidateType type = WaveIceCandidateType::Host;
    WaveIceTcpType tcpType = WaveIceTcpType::None;
};

struct WaveSdpSignal {
    std::string iceUfrag;
    std::string icePwd;
    std::string dtlsFingerprintAlg = "sha-256";
    // Colon-separated hex SHA-256 fingerprint (or raw 32 bytes in wire format)
    std::string dtlsFingerprint;
    WaveDtlsSetupRole setupRole = WaveDtlsSetupRole::Actpass;
    std::vector<WaveIceCandidate> candidates;
};

struct WaveSignalMessage {
    std::uint8_t version = kWaveSignalVersion;
    WaveSignalType type = WaveSignalType::Offer;
    std::array<std::uint8_t, 16> sid{}; // 128-bit session ID
    std::int64_t iat = 0;               // Unix seconds
    std::int64_t exp = 0;               // Unix seconds
    std::uint32_t caps = kWaveCapOpus48kStereo | kWaveCapDtlsSrtp;
    std::uint64_t nonce = 0;
    WaveSdpSignal signal;
};

// Generates a cryptographically seeded 128-bit random session ID.
[[nodiscard]] std::array<std::uint8_t, 16> generateWaveSessionId();
[[nodiscard]] std::string formatSessionIdHex(
    const std::array<std::uint8_t, 16>& sid);

// Validates message fields, bounds, and optional timestamp expiry.
// Pass `nowUnixSeconds <= 0` to skip clock expiry check.
[[nodiscard]] bool validateWaveSignal(const WaveSignalMessage& msg,
                                      std::int64_t nowUnixSeconds = 0,
                                      std::string* error = nullptr);

// Canonical versioned binary codec (BG Wave v1).
[[nodiscard]] bool encodeWaveSignalBinary(
    const WaveSignalMessage& msg,
    std::vector<std::uint8_t>& outBytes,
    std::string* error = nullptr);

[[nodiscard]] bool decodeWaveSignalBinary(
    std::span<const std::uint8_t> bytes,
    WaveSignalMessage& outMsg,
    std::string* error = nullptr);

// URL-safe base64 without padding (RFC 4648 Section 5).
[[nodiscard]] std::string base64UrlEncode(std::span<const std::uint8_t> data);
[[nodiscard]] bool base64UrlDecode(std::string_view encoded,
                                    std::vector<std::uint8_t>& outBytes,
                                    std::string* error = nullptr);

// Internal app URI: bg://wave/v1/<base64url(payload)>
[[nodiscard]] std::string encodeWaveUri(const WaveSignalMessage& msg,
                                        std::string* error = nullptr);

// External HTTPS fragment URI:
// https://beygram.beykus.fun/#bgwave-v1.<base64url(payload)>
[[nodiscard]] std::string encodeWaveHttpsFragmentUri(
    const WaveSignalMessage& msg,
    std::string_view baseUrl = "https://beygram.beykus.fun/",
    std::string* error = nullptr);

// Decodes either `bg://wave/v1/<b64>` or `https://.../#bgwave-v1.<b64>`.
[[nodiscard]] bool decodeWaveUri(std::string_view uri,
                                 WaveSignalMessage& outMsg,
                                 std::string* error = nullptr);

// Parses WebRTC SDP (IPv4, IPv6, mDNS .local, multi-interface) into bounded
// WaveSdpSignal and formats back to interoperable WebRTC SDP.
[[nodiscard]] bool parseSdpToWaveSignal(std::string_view sdp,
                                        WaveSdpSignal& outSignal,
                                        std::string* error = nullptr);

[[nodiscard]] std::string formatWaveSignalToSdp(const WaveSdpSignal& signal,
                                                bool isOffer);

// Produces a redacted diagnostic summary (masks IP addresses, ufrag/pwd, and
// DTLS fingerprint) safe for logs and bug reports.
[[nodiscard]] std::string redactWaveSignalForDiagnostics(
    const WaveSignalMessage& msg);

} // namespace bgmedia::wave
