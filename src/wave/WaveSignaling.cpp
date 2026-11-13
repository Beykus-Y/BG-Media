#include "bgmedia/wave/WaveSignaling.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <charconv>
#include <cstdio>
#include <cstring>
#include <random>
#include <sstream>

namespace bgmedia::wave {
namespace {

constexpr std::uint8_t kMagic0 = 'B';
constexpr std::uint8_t kMagic1 = 'W';
constexpr std::size_t kMaxStringFieldBytes = 128;

enum class AddrKind : std::uint8_t {
    Ipv4 = 1,
    Ipv6 = 2,
    Hostname = 3
};

void setError(std::string* error, std::string_view msg) {
    if (error) {
        *error = std::string(msg);
    }
}

void writeU16(std::vector<std::uint8_t>& out, std::uint16_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
}

void writeU32(std::vector<std::uint8_t>& out, std::uint32_t v) {
    out.push_back(static_cast<std::uint8_t>((v >> 24) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 16) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>((v >> 8) & 0xFFu));
    out.push_back(static_cast<std::uint8_t>(v & 0xFFu));
}

void writeU64(std::vector<std::uint8_t>& out, std::uint64_t v) {
    for (int shift = 56; shift >= 0; shift -= 8) {
        out.push_back(static_cast<std::uint8_t>((v >> shift) & 0xFFu));
    }
}

bool readU8(std::span<const std::uint8_t> in, std::size_t& pos,
            std::uint8_t& v) {
    if (pos >= in.size()) {
        return false;
    }
    v = in[pos++];
    return true;
}

bool readU16(std::span<const std::uint8_t> in, std::size_t& pos,
             std::uint16_t& v) {
    if (pos + 2 > in.size()) {
        return false;
    }
    v = (static_cast<std::uint16_t>(in[pos]) << 8) |
        static_cast<std::uint16_t>(in[pos + 1]);
    pos += 2;
    return true;
}

bool readU32(std::span<const std::uint8_t> in, std::size_t& pos,
             std::uint32_t& v) {
    if (pos + 4 > in.size()) {
        return false;
    }
    v = (static_cast<std::uint32_t>(in[pos]) << 24) |
        (static_cast<std::uint32_t>(in[pos + 1]) << 16) |
        (static_cast<std::uint32_t>(in[pos + 2]) << 8) |
        static_cast<std::uint32_t>(in[pos + 3]);
    pos += 4;
    return true;
}

bool readU64(std::span<const std::uint8_t> in, std::size_t& pos,
             std::uint64_t& v) {
    if (pos + 8 > in.size()) {
        return false;
    }
    v = 0;
    for (int i = 0; i < 8; ++i) {
        v = (v << 8) | static_cast<std::uint64_t>(in[pos + i]);
    }
    pos += 8;
    return true;
}

bool writeShortStr(std::vector<std::uint8_t>& out, std::string_view s) {
    if (s.size() > kMaxStringFieldBytes) {
        return false;
    }
    out.push_back(static_cast<std::uint8_t>(s.size()));
    out.insert(out.end(), s.begin(), s.end());
    return true;
}

bool readShortStr(std::span<const std::uint8_t> in, std::size_t& pos,
                  std::string& out) {
    std::uint8_t len = 0;
    if (!readU8(in, pos, len)) {
        return false;
    }
    if (len > kMaxStringFieldBytes || pos + len > in.size()) {
        return false;
    }
    out.assign(reinterpret_cast<const char*>(in.data() + pos), len);
    pos += len;
    return true;
}

bool parseIpv4(std::string_view text, std::array<std::uint8_t, 4>& out) {
    std::size_t start = 0;
    for (int part = 0; part < 4; ++part) {
        const std::size_t dot = text.find('.', start);
        if ((part < 3 && dot == std::string_view::npos) ||
            (part == 3 && dot != std::string_view::npos)) {
            return false;
        }
        const std::string_view token =
            part < 3 ? text.substr(start, dot - start) : text.substr(start);
        if (token.empty() || token.size() > 3) {
            return false;
        }
        if (token.size() > 1 && token[0] == '0') {
            return false;
        }
        unsigned int val = 0;
        const auto [ptr, ec] =
            std::from_chars(token.data(), token.data() + token.size(), val);
        if (ec != std::errc{} || ptr != token.data() + token.size() ||
            val > 255) {
            return false;
        }
        out[static_cast<std::size_t>(part)] = static_cast<std::uint8_t>(val);
        start = dot + 1;
    }
    return true;
}

std::string formatIpv4(const std::array<std::uint8_t, 4>& ip) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip[0], ip[1], ip[2], ip[3]);
    return std::string(buf);
}

bool parseIpv6(std::string_view text, std::array<std::uint8_t, 16>& out) {
    if (text.empty() || text.find(':') == std::string_view::npos) {
        return false;
    }
    // Strip optional zone id (e.g. %eth0) from link-local addresses only if
    // not present; if zone id is present, keep as Hostname so it isn't lost.
    if (text.find('%') != std::string_view::npos ||
        text.find('.') != std::string_view::npos) {
        return false;
    }

    const std::size_t dbl = text.find("::");
    if (dbl != std::string_view::npos &&
        text.find("::", dbl + 2) != std::string_view::npos) {
        return false;
    }

    auto parseGroups = [](std::string_view part,
                          std::vector<std::uint16_t>& groups) -> bool {
        if (part.empty()) {
            return true;
        }
        std::size_t pos = 0;
        while (pos <= part.size()) {
            const std::size_t colon = part.find(':', pos);
            const std::string_view tok = colon == std::string_view::npos
                                             ? part.substr(pos)
                                             : part.substr(pos, colon - pos);
            if (tok.empty() || tok.size() > 4) {
                return false;
            }
            unsigned int val = 0;
            const auto [ptr, ec] = std::from_chars(
                tok.data(), tok.data() + tok.size(), val, 16);
            if (ec != std::errc{} || ptr != tok.data() + tok.size() ||
                val > 0xFFFFu) {
                return false;
            }
            groups.push_back(static_cast<std::uint16_t>(val));
            if (colon == std::string_view::npos) {
                break;
            }
            pos = colon + 1;
        }
        return true;
    };

    std::vector<std::uint16_t> head;
    std::vector<std::uint16_t> tail;
    if (dbl == std::string_view::npos) {
        if (!parseGroups(text, head) || head.size() != 8) {
            return false;
        }
    } else {
        if (!parseGroups(text.substr(0, dbl), head) ||
            !parseGroups(text.substr(dbl + 2), tail)) {
            return false;
        }
        if (head.size() + tail.size() >= 8) {
            return false;
        }
    }

    std::array<std::uint16_t, 8> words{};
    for (std::size_t i = 0; i < head.size(); ++i) {
        words[i] = head[i];
    }
    const std::size_t tailStart = 8 - tail.size();
    for (std::size_t i = 0; i < tail.size(); ++i) {
        words[tailStart + i] = tail[i];
    }
    for (std::size_t i = 0; i < 8; ++i) {
        out[i * 2] = static_cast<std::uint8_t>((words[i] >> 8) & 0xFFu);
        out[i * 2 + 1] = static_cast<std::uint8_t>(words[i] & 0xFFu);
    }
    return true;
}

std::string formatIpv6(const std::array<std::uint8_t, 16>& ip) {
    std::array<std::uint16_t, 8> words{};
    for (std::size_t i = 0; i < 8; ++i) {
        words[i] = (static_cast<std::uint16_t>(ip[i * 2]) << 8) |
                   static_cast<std::uint16_t>(ip[i * 2 + 1]);
    }
    std::size_t bestStart = 8;
    std::size_t bestLen = 0;
    for (std::size_t i = 0; i < 8;) {
        if (words[i] != 0) {
            ++i;
            continue;
        }
        std::size_t j = i;
        while (j < 8 && words[j] == 0) {
            ++j;
        }
        if (j - i > bestLen && j - i > 1) {
            bestStart = i;
            bestLen = j - i;
        }
        i = j;
    }

    std::ostringstream oss;
    oss << std::hex;
    for (std::size_t i = 0; i < 8;) {
        if (i == bestStart) {
            oss << "::";
            i += bestLen;
            continue;
        }
        if (i > 0 && !(bestStart < i && i == bestStart + bestLen)) {
            oss << ':';
        }
        oss << words[i];
        ++i;
    }
    return oss.str();
}

bool parseSha256FingerprintHex(std::string_view fp,
                               std::array<std::uint8_t, 32>& out) {
    std::size_t byteIdx = 0;
    std::size_t pos = 0;
    while (pos < fp.size() && byteIdx < 32) {
        if (fp[pos] == ':' || fp[pos] == ' ') {
            ++pos;
            continue;
        }
        if (pos + 2 > fp.size()) {
            return false;
        }
        unsigned int val = 0;
        const auto [ptr, ec] =
            std::from_chars(fp.data() + pos, fp.data() + pos + 2, val, 16);
        if (ec != std::errc{} || ptr != fp.data() + pos + 2 || val > 255) {
            return false;
        }
        out[byteIdx++] = static_cast<std::uint8_t>(val);
        pos += 2;
    }
    return byteIdx == 32 && pos == fp.size();
}

std::string formatSha256FingerprintHex(const std::array<std::uint8_t, 32>& fp) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(32 * 3 - 1);
    for (std::size_t i = 0; i < 32; ++i) {
        if (i > 0) {
            out.push_back(':');
        }
        out.push_back(kHex[(fp[i] >> 4) & 0x0Fu]);
        out.push_back(kHex[fp[i] & 0x0Fu]);
    }
    return out;
}

std::string_view trimCr(std::string_view line) {
    while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) {
        line.remove_suffix(1);
    }
    return line;
}

} // namespace

std::array<std::uint8_t, 16> generateWaveSessionId() {
    std::array<std::uint8_t, 16> sid{};
    std::random_device rd;
    std::mt19937_64 rng((static_cast<std::uint64_t>(rd()) << 32) ^
                        static_cast<std::uint64_t>(rd()));
    const std::uint64_t a = rng();
    const std::uint64_t b = rng();
    std::memcpy(sid.data(), &a, 8);
    std::memcpy(sid.data() + 8, &b, 8);
    return sid;
}

std::string formatSessionIdHex(const std::array<std::uint8_t, 16>& sid) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(32);
    for (std::uint8_t b : sid) {
        out.push_back(kHex[(b >> 4) & 0x0Fu]);
        out.push_back(kHex[b & 0x0Fu]);
    }
    return out;
}

bool validateWaveSignal(const WaveSignalMessage& msg,
                        std::int64_t nowUnixSeconds, std::string* error) {
    if (msg.version != kWaveSignalVersion) {
        setError(error, "Unsupported BG Wave signal version");
        return false;
    }
    switch (msg.type) {
    case WaveSignalType::Offer:
    case WaveSignalType::Answer:
    case WaveSignalType::Cancel:
    case WaveSignalType::End:
        break;
    default:
        setError(error, "Unknown BG Wave signal type");
        return false;
    }
    bool allZeroSid = true;
    for (std::uint8_t b : msg.sid) {
        if (b != 0) {
            allZeroSid = false;
            break;
        }
    }
    if (allZeroSid) {
        setError(error, "Session ID must be non-zero");
        return false;
    }
    if (msg.exp > 0 && msg.iat > 0 && msg.exp < msg.iat) {
        setError(error, "Signal expiry precedes issued-at timestamp");
        return false;
    }
    if (nowUnixSeconds > 0 && msg.exp > 0 && nowUnixSeconds > msg.exp) {
        setError(error, "Signal has expired");
        return false;
    }
    if (msg.signal.iceUfrag.size() > kMaxStringFieldBytes ||
        msg.signal.icePwd.size() > kMaxStringFieldBytes ||
        msg.signal.dtlsFingerprintAlg.size() > kMaxStringFieldBytes ||
        msg.signal.dtlsFingerprint.size() > kMaxStringFieldBytes) {
        setError(error, "Signal string field exceeds maximum length");
        return false;
    }
    if (msg.signal.candidates.size() > kWaveMaxCandidates) {
        setError(error, "Too many ICE candidates");
        return false;
    }
    for (const auto& c : msg.signal.candidates) {
        if (c.foundation.empty() ||
            c.foundation.size() > kMaxStringFieldBytes ||
            c.address.empty() || c.address.size() > kMaxStringFieldBytes ||
            c.port == 0 || c.component == 0) {
            setError(error, "Invalid ICE candidate");
            return false;
        }
    }
    return true;
}

bool encodeWaveSignalBinary(const WaveSignalMessage& msg,
                            std::vector<std::uint8_t>& outBytes,
                            std::string* error) {
    if (!validateWaveSignal(msg, 0, error)) {
        return false;
    }
    outBytes.clear();
    outBytes.reserve(256);
    outBytes.push_back(kMagic0);
    outBytes.push_back(kMagic1);
    outBytes.push_back(msg.version);
    outBytes.push_back(static_cast<std::uint8_t>(msg.type));
    outBytes.insert(outBytes.end(), msg.sid.begin(), msg.sid.end());
    writeU64(outBytes, static_cast<std::uint64_t>(msg.iat));
    writeU64(outBytes, static_cast<std::uint64_t>(msg.exp));
    writeU32(outBytes, msg.caps);
    writeU64(outBytes, msg.nonce);

    if (!writeShortStr(outBytes, msg.signal.iceUfrag) ||
        !writeShortStr(outBytes, msg.signal.icePwd) ||
        !writeShortStr(outBytes, msg.signal.dtlsFingerprintAlg)) {
        setError(error, "Signal string exceeds binary field bound");
        return false;
    }

    std::array<std::uint8_t, 32> rawFp{};
    if (msg.signal.dtlsFingerprintAlg == "sha-256" &&
        parseSha256FingerprintHex(msg.signal.dtlsFingerprint, rawFp)) {
        outBytes.push_back(1); // raw 32-byte sha-256
        outBytes.insert(outBytes.end(), rawFp.begin(), rawFp.end());
    } else {
        outBytes.push_back(0); // string fallback
        if (!writeShortStr(outBytes, msg.signal.dtlsFingerprint)) {
            setError(error, "Fingerprint exceeds binary field bound");
            return false;
        }
    }

    outBytes.push_back(static_cast<std::uint8_t>(msg.signal.setupRole));
    outBytes.push_back(static_cast<std::uint8_t>(msg.signal.candidates.size()));

    for (const auto& c : msg.signal.candidates) {
        if (!writeShortStr(outBytes, c.foundation)) {
            setError(error, "Candidate foundation exceeds bound");
            return false;
        }
        outBytes.push_back(c.component);
        const std::uint8_t packedFlags =
            static_cast<std::uint8_t>(static_cast<std::uint8_t>(c.protocol) & 0x01u) |
            static_cast<std::uint8_t>((static_cast<std::uint8_t>(c.type) & 0x03u) << 1) |
            static_cast<std::uint8_t>((static_cast<std::uint8_t>(c.tcpType) & 0x03u) << 3);
        outBytes.push_back(packedFlags);
        writeU32(outBytes, c.priority);
        writeU16(outBytes, c.port);

        std::array<std::uint8_t, 4> ip4{};
        std::array<std::uint8_t, 16> ip6{};
        if (parseIpv4(c.address, ip4)) {
            outBytes.push_back(static_cast<std::uint8_t>(AddrKind::Ipv4));
            outBytes.insert(outBytes.end(), ip4.begin(), ip4.end());
        } else if (parseIpv6(c.address, ip6)) {
            outBytes.push_back(static_cast<std::uint8_t>(AddrKind::Ipv6));
            outBytes.insert(outBytes.end(), ip6.begin(), ip6.end());
        } else {
            outBytes.push_back(static_cast<std::uint8_t>(AddrKind::Hostname));
            if (!writeShortStr(outBytes, c.address)) {
                setError(error, "Candidate hostname exceeds bound");
                return false;
            }
        }
    }

    if (outBytes.size() > kWaveMaxBinaryPayloadBytes) {
        outBytes.clear();
        setError(error, "Encoded BG Wave payload exceeds maximum size");
        return false;
    }
    return true;
}

bool decodeWaveSignalBinary(std::span<const std::uint8_t> bytes,
                            WaveSignalMessage& outMsg, std::string* error) {
    if (bytes.size() < 4 + 16 + 8 + 8 + 4 + 8 ||
        bytes.size() > kWaveMaxBinaryPayloadBytes) {
        setError(error, "BG Wave payload size out of bounds");
        return false;
    }
    if (bytes[0] != kMagic0 || bytes[1] != kMagic1) {
        setError(error, "Invalid BG Wave payload magic");
        return false;
    }
    if (bytes[2] != kWaveSignalVersion) {
        setError(error, "Unsupported BG Wave payload version");
        return false;
    }

    WaveSignalMessage msg;
    msg.version = bytes[2];
    msg.type = static_cast<WaveSignalType>(bytes[3]);
    std::size_t pos = 4;
    std::memcpy(msg.sid.data(), bytes.data() + pos, 16);
    pos += 16;

    std::uint64_t uIat = 0;
    std::uint64_t uExp = 0;
    if (!readU64(bytes, pos, uIat) || !readU64(bytes, pos, uExp) ||
        !readU32(bytes, pos, msg.caps) || !readU64(bytes, pos, msg.nonce)) {
        setError(error, "Truncated BG Wave header");
        return false;
    }
    msg.iat = static_cast<std::int64_t>(uIat);
    msg.exp = static_cast<std::int64_t>(uExp);

    if (!readShortStr(bytes, pos, msg.signal.iceUfrag) ||
        !readShortStr(bytes, pos, msg.signal.icePwd) ||
        !readShortStr(bytes, pos, msg.signal.dtlsFingerprintAlg)) {
        setError(error, "Truncated BG Wave ICE/DTLS fields");
        return false;
    }

    std::uint8_t fpMode = 0;
    if (!readU8(bytes, pos, fpMode)) {
        setError(error, "Truncated fingerprint mode");
        return false;
    }
    if (fpMode == 1) {
        if (pos + 32 > bytes.size()) {
            setError(error, "Truncated SHA-256 fingerprint");
            return false;
        }
        std::array<std::uint8_t, 32> rawFp{};
        std::memcpy(rawFp.data(), bytes.data() + pos, 32);
        pos += 32;
        msg.signal.dtlsFingerprint = formatSha256FingerprintHex(rawFp);
    } else if (fpMode == 0) {
        if (!readShortStr(bytes, pos, msg.signal.dtlsFingerprint)) {
            setError(error, "Truncated string fingerprint");
            return false;
        }
    } else {
        setError(error, "Invalid fingerprint encoding mode");
        return false;
    }

    std::uint8_t setupByte = 0;
    std::uint8_t candCount = 0;
    if (!readU8(bytes, pos, setupByte) || !readU8(bytes, pos, candCount)) {
        setError(error, "Truncated setup/candidate count");
        return false;
    }
    if (setupByte > static_cast<std::uint8_t>(WaveDtlsSetupRole::Passive) ||
        candCount > kWaveMaxCandidates) {
        setError(error, "Invalid setup role or candidate count");
        return false;
    }
    msg.signal.setupRole = static_cast<WaveDtlsSetupRole>(setupByte);
    msg.signal.candidates.reserve(candCount);

    for (std::uint8_t i = 0; i < candCount; ++i) {
        WaveIceCandidate c;
        std::uint8_t packedFlags = 0;
        std::uint8_t addrKind = 0;
        if (!readShortStr(bytes, pos, c.foundation) ||
            !readU8(bytes, pos, c.component) ||
            !readU8(bytes, pos, packedFlags) ||
            !readU32(bytes, pos, c.priority) ||
            !readU16(bytes, pos, c.port) ||
            !readU8(bytes, pos, addrKind)) {
            setError(error, "Truncated ICE candidate");
            return false;
        }
        c.protocol = static_cast<WaveIceProtocol>(packedFlags & 0x01u);
        c.type = static_cast<WaveIceCandidateType>((packedFlags >> 1) & 0x03u);
        c.tcpType = static_cast<WaveIceTcpType>((packedFlags >> 3) & 0x03u);

        if (addrKind == static_cast<std::uint8_t>(AddrKind::Ipv4)) {
            if (pos + 4 > bytes.size()) {
                setError(error, "Truncated IPv4 address");
                return false;
            }
            std::array<std::uint8_t, 4> ip4{};
            std::memcpy(ip4.data(), bytes.data() + pos, 4);
            pos += 4;
            c.address = formatIpv4(ip4);
        } else if (addrKind == static_cast<std::uint8_t>(AddrKind::Ipv6)) {
            if (pos + 16 > bytes.size()) {
                setError(error, "Truncated IPv6 address");
                return false;
            }
            std::array<std::uint8_t, 16> ip6{};
            std::memcpy(ip6.data(), bytes.data() + pos, 16);
            pos += 16;
            c.address = formatIpv6(ip6);
        } else if (addrKind == static_cast<std::uint8_t>(AddrKind::Hostname)) {
            if (!readShortStr(bytes, pos, c.address)) {
                setError(error, "Truncated candidate hostname");
                return false;
            }
        } else {
            setError(error, "Unknown candidate address kind");
            return false;
        }
        msg.signal.candidates.push_back(std::move(c));
    }

    if (pos != bytes.size()) {
        setError(error, "Trailing bytes in BG Wave payload");
        return false;
    }

    if (!validateWaveSignal(msg, 0, error)) {
        return false;
    }
    outMsg = std::move(msg);
    return true;
}

std::string base64UrlEncode(std::span<const std::uint8_t> data) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve(((data.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < data.size(); i += 3) {
        const std::uint32_t b0 = data[i];
        const std::uint32_t b1 = (i + 1 < data.size()) ? data[i + 1] : 0;
        const std::uint32_t b2 = (i + 2 < data.size()) ? data[i + 2] : 0;
        const std::uint32_t triple = (b0 << 16) | (b1 << 8) | b2;
        out.push_back(kAlphabet[(triple >> 18) & 0x3Fu]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3Fu]);
        if (i + 1 < data.size()) {
            out.push_back(kAlphabet[(triple >> 6) & 0x3Fu]);
        }
        if (i + 2 < data.size()) {
            out.push_back(kAlphabet[triple & 0x3Fu]);
        }
    }
    return out;
}

bool base64UrlDecode(std::string_view encoded,
                     std::vector<std::uint8_t>& outBytes, std::string* error) {
    if (encoded.empty() || encoded.size() > kWaveMaxUriLength) {
        setError(error, "Base64url input empty or oversized");
        return false;
    }
    if (encoded.size() % 4 == 1) {
        setError(error, "Invalid base64url length");
        return false;
    }

    auto decodeChar = [](unsigned char ch) -> int {
        if (ch >= 'A' && ch <= 'Z') return ch - 'A';
        if (ch >= 'a' && ch <= 'z') return ch - 'a' + 26;
        if (ch >= '0' && ch <= '9') return ch - '0' + 52;
        if (ch == '-') return 62;
        if (ch == '_') return 63;
        return -1;
    };

    outBytes.clear();
    outBytes.reserve((encoded.size() * 3) / 4);

    std::size_t i = 0;
    while (i + 4 <= encoded.size()) {
        const int v0 = decodeChar(static_cast<unsigned char>(encoded[i]));
        const int v1 = decodeChar(static_cast<unsigned char>(encoded[i + 1]));
        const int v2 = decodeChar(static_cast<unsigned char>(encoded[i + 2]));
        const int v3 = decodeChar(static_cast<unsigned char>(encoded[i + 3]));
        if (v0 < 0 || v1 < 0 || v2 < 0 || v3 < 0) {
            setError(error, "Invalid base64url character");
            return false;
        }
        const std::uint32_t triple =
            (static_cast<std::uint32_t>(v0) << 18) |
            (static_cast<std::uint32_t>(v1) << 12) |
            (static_cast<std::uint32_t>(v2) << 6) |
            static_cast<std::uint32_t>(v3);
        outBytes.push_back(static_cast<std::uint8_t>((triple >> 16) & 0xFFu));
        outBytes.push_back(static_cast<std::uint8_t>((triple >> 8) & 0xFFu));
        outBytes.push_back(static_cast<std::uint8_t>(triple & 0xFFu));
        i += 4;
    }

    const std::size_t rem = encoded.size() - i;
    if (rem == 2) {
        const int v0 = decodeChar(static_cast<unsigned char>(encoded[i]));
        const int v1 = decodeChar(static_cast<unsigned char>(encoded[i + 1]));
        if (v0 < 0 || v1 < 0) {
            setError(error, "Invalid base64url tail");
            return false;
        }
        outBytes.push_back(
            static_cast<std::uint8_t>((v0 << 2) | (v1 >> 4)));
    } else if (rem == 3) {
        const int v0 = decodeChar(static_cast<unsigned char>(encoded[i]));
        const int v1 = decodeChar(static_cast<unsigned char>(encoded[i + 1]));
        const int v2 = decodeChar(static_cast<unsigned char>(encoded[i + 2]));
        if (v0 < 0 || v1 < 0 || v2 < 0) {
            setError(error, "Invalid base64url tail");
            return false;
        }
        outBytes.push_back(
            static_cast<std::uint8_t>((v0 << 2) | (v1 >> 4)));
        outBytes.push_back(
            static_cast<std::uint8_t>(((v1 & 0x0Fu) << 4) | (v2 >> 2)));
    }
    return true;
}

std::string encodeWaveUri(const WaveSignalMessage& msg, std::string* error) {
    std::vector<std::uint8_t> bytes;
    if (!encodeWaveSignalBinary(msg, bytes, error)) {
        return {};
    }
    return "bg://wave/v1/" + base64UrlEncode(bytes);
}

std::string encodeWaveHttpsFragmentUri(const WaveSignalMessage& msg,
                                       std::string_view baseUrl,
                                       std::string* error) {
    std::vector<std::uint8_t> bytes;
    if (!encodeWaveSignalBinary(msg, bytes, error)) {
        return {};
    }
    std::string out(baseUrl);
    if (out.empty()) {
        out = "https://beygram.beykus.fun/";
    }
    if (out.find('#') == std::string::npos) {
        out += "#bgwave-v1.";
    } else {
        out += "bgwave-v1.";
    }
    out += base64UrlEncode(bytes);
    return out;
}

bool decodeWaveUri(std::string_view uri, WaveSignalMessage& outMsg,
                   std::string* error) {
    if (uri.empty() || uri.size() > kWaveMaxUriLength) {
        setError(error, "BG Wave URI empty or oversized");
        return false;
    }

    constexpr std::string_view kInternalPrefix = "bg://wave/v1/";
    constexpr std::string_view kFragmentMarker = "#bgwave-v1.";
    std::string_view b64;
    if (uri.starts_with(kInternalPrefix)) {
        b64 = uri.substr(kInternalPrefix.size());
    } else {
        const std::size_t pos = uri.find(kFragmentMarker);
        if (pos == std::string_view::npos) {
            setError(error, "Unrecognized BG Wave URI scheme or version");
            return false;
        }
        b64 = uri.substr(pos + kFragmentMarker.size());
    }

    std::vector<std::uint8_t> bytes;
    if (!base64UrlDecode(b64, bytes, error)) {
        return false;
    }
    return decodeWaveSignalBinary(bytes, outMsg, error);
}

bool parseSdpToWaveSignal(std::string_view sdp, WaveSdpSignal& outSignal,
                          std::string* error) {
    if (sdp.empty() || sdp.size() > kWaveMaxSdpBytes) {
        setError(error, "SDP empty or exceeds maximum size");
        return false;
    }
    WaveSdpSignal sig;
    std::size_t pos = 0;
    while (pos < sdp.size()) {
        const std::size_t nl = sdp.find('\n', pos);
        const std::string_view rawLine =
            nl == std::string_view::npos ? sdp.substr(pos)
                                         : sdp.substr(pos, nl - pos);
        pos = (nl == std::string_view::npos) ? sdp.size() : (nl + 1);
        const std::string_view line = trimCr(rawLine);

        if (line.starts_with("a=ice-ufrag:")) {
            sig.iceUfrag = std::string(line.substr(12));
        } else if (line.starts_with("a=ice-pwd:")) {
            sig.icePwd = std::string(line.substr(10));
        } else if (line.starts_with("a=fingerprint:")) {
            const std::string_view rest = line.substr(14);
            const std::size_t sp = rest.find(' ');
            if (sp != std::string_view::npos) {
                sig.dtlsFingerprintAlg = std::string(rest.substr(0, sp));
                sig.dtlsFingerprint = std::string(rest.substr(sp + 1));
            }
        } else if (line.starts_with("a=setup:")) {
            const std::string_view role = line.substr(8);
            if (role == "actpass") {
                sig.setupRole = WaveDtlsSetupRole::Actpass;
            } else if (role == "active") {
                sig.setupRole = WaveDtlsSetupRole::Active;
            } else if (role == "passive") {
                sig.setupRole = WaveDtlsSetupRole::Passive;
            }
        } else if (line.starts_with("a=candidate:")) {
            if (sig.candidates.size() >= kWaveMaxCandidates) {
                continue; // bounded candidate count
            }
            std::istringstream iss(std::string(line.substr(12)));
            WaveIceCandidate c;
            int comp = 0;
            std::string proto;
            std::uint32_t prio = 0;
            std::string addr;
            int port = 0;
            std::string typKw;
            std::string typVal;
            if (!(iss >> c.foundation >> comp >> proto >> prio >> addr >>
                  port >> typKw >> typVal)) {
                continue;
            }
            if (comp <= 0 || comp > 255 || port <= 0 || port > 65535 ||
                typKw != "typ") {
                continue;
            }
            c.component = static_cast<std::uint8_t>(comp);
            c.priority = prio;
            c.address = std::move(addr);
            c.port = static_cast<std::uint16_t>(port);

            if (proto == "tcp" || proto == "TCP") {
                c.protocol = WaveIceProtocol::Tcp;
            } else {
                c.protocol = WaveIceProtocol::Udp;
            }

            if (typVal == "host") {
                c.type = WaveIceCandidateType::Host;
            } else if (typVal == "srflx") {
                c.type = WaveIceCandidateType::Srflx;
            } else if (typVal == "prflx") {
                c.type = WaveIceCandidateType::Prflx;
            } else if (typVal == "relay") {
                c.type = WaveIceCandidateType::Relay;
            }

            std::string tok;
            while (iss >> tok) {
                if (tok == "tcptype" && (iss >> tok)) {
                    if (tok == "active") c.tcpType = WaveIceTcpType::Active;
                    else if (tok == "passive") c.tcpType = WaveIceTcpType::Passive;
                    else if (tok == "so") c.tcpType = WaveIceTcpType::So;
                }
            }
            sig.candidates.push_back(std::move(c));
        }
    }

    if (sig.iceUfrag.empty() || sig.icePwd.empty()) {
        setError(error, "SDP missing required ICE ufrag/pwd");
        return false;
    }
    outSignal = std::move(sig);
    return true;
}

std::string formatWaveSignalToSdp(const WaveSdpSignal& signal, bool isOffer) {
    std::ostringstream oss;
    oss << "v=0\r\n"
        << "o=- 0 2 IN IP4 127.0.0.1\r\n"
        << "s=-\r\n"
        << "t=0 0\r\n"
        << "m=audio 9 UDP/TLS/RTP/SAVPF 111\r\n"
        << "c=IN IP4 0.0.0.0\r\n"
        << "a=rtpmap:111 opus/48000/2\r\n"
        << "a=fmtp:111 minptime=10;useinbandfec=1;stereo=1;sprop-stereo=1\r\n"
        << (isOffer ? "a=sendonly\r\n" : "a=recvonly\r\n")
        << "a=ice-ufrag:" << signal.iceUfrag << "\r\n"
        << "a=ice-pwd:" << signal.icePwd << "\r\n";

    if (!signal.dtlsFingerprint.empty()) {
        oss << "a=fingerprint:" << signal.dtlsFingerprintAlg << " "
            << signal.dtlsFingerprint << "\r\n";
    }

    const char* setupStr = "actpass";
    if (signal.setupRole == WaveDtlsSetupRole::Active) setupStr = "active";
    else if (signal.setupRole == WaveDtlsSetupRole::Passive) setupStr = "passive";
    oss << "a=setup:" << setupStr << "\r\n";

    for (const auto& c : signal.candidates) {
        const char* protoStr =
            c.protocol == WaveIceProtocol::Tcp ? "tcp" : "udp";
        const char* typeStr = "host";
        switch (c.type) {
        case WaveIceCandidateType::Host: typeStr = "host"; break;
        case WaveIceCandidateType::Srflx: typeStr = "srflx"; break;
        case WaveIceCandidateType::Prflx: typeStr = "prflx"; break;
        case WaveIceCandidateType::Relay: typeStr = "relay"; break;
        }
        oss << "a=candidate:" << c.foundation << " "
            << static_cast<int>(c.component) << " " << protoStr << " "
            << c.priority << " " << c.address << " " << c.port << " typ "
            << typeStr;
        if (c.protocol == WaveIceProtocol::Tcp &&
            c.tcpType != WaveIceTcpType::None) {
            const char* tcpStr = "active";
            if (c.tcpType == WaveIceTcpType::Passive) tcpStr = "passive";
            else if (c.tcpType == WaveIceTcpType::So) tcpStr = "so";
            oss << " tcptype " << tcpStr;
        }
        oss << "\r\n";
    }
    oss << "a=end-of-candidates\r\n";
    return oss.str();
}

std::string redactWaveSignalForDiagnostics(const WaveSignalMessage& msg) {
    std::ostringstream oss;
    oss << "WaveSignal{v=" << static_cast<int>(msg.version)
        << ", type=" << static_cast<int>(msg.type)
        << ", sid=" << formatSessionIdHex(msg.sid).substr(0, 8) << "..."
        << ", caps=0x" << std::hex << msg.caps << std::dec
        << ", ufrag=" << (msg.signal.iceUfrag.empty() ? "<empty>" : "<redacted>")
        << ", pwd=" << (msg.signal.icePwd.empty() ? "<empty>" : "<redacted>")
        << ", fp=" << (msg.signal.dtlsFingerprint.empty() ? "<empty>" : "<redacted>")
        << ", candidates=" << msg.signal.candidates.size() << "}";
    return oss.str();
}

} // namespace bgmedia::wave
