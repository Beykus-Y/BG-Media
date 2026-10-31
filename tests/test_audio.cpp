#include "framework.hpp"

#if defined(BGMEDIA_HAVE_MINIAUDIO) && BGMEDIA_HAVE_MINIAUDIO
#include "bgmedia/audio/MiniaudioDecoder.hpp"
#endif
#include "bgmedia/audio/WavDecoder.hpp"
#include "support/SyntheticSignals.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

std::filesystem::path tempPath(const std::string& name) {
    const auto directory = std::filesystem::temp_directory_path() / "bgmedia_tests";
    std::error_code ec;
    std::filesystem::create_directories(directory, ec);
    return directory / name;
}

void writeCustomWav(const std::filesystem::path& path, std::uint16_t format,
                    std::uint16_t channels, std::uint32_t sampleRate,
                    std::uint16_t bits, const std::vector<std::uint8_t>& data) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    auto u16 = [&](std::uint16_t value) {
        out.put(static_cast<char>(value & 0xFF));
        out.put(static_cast<char>((value >> 8) & 0xFF));
    };
    auto u32 = [&](std::uint32_t value) {
        for (int i = 0; i < 4; ++i) {
            out.put(static_cast<char>((value >> (i * 8)) & 0xFF));
        }
    };
    out.write("RIFF", 4);
    u32(static_cast<std::uint32_t>(36 + data.size()));
    out.write("WAVE", 4);
    out.write("fmt ", 4);
    u32(16);
    u16(format);
    u16(channels);
    u32(sampleRate);
    u32(sampleRate * channels * bits / 8);
    u16(static_cast<std::uint16_t>(channels * bits / 8));
    u16(bits);
    out.write("data", 4);
    u32(static_cast<std::uint32_t>(data.size()));
    out.write(reinterpret_cast<const char*>(data.data()),
              static_cast<std::streamsize>(data.size()));
}

} // namespace

TEST_CASE(audio.wav16_roundtrip) {
    using namespace bgmedia;
    test::TrackOptions options;
    options.bpm = 123.0;
    options.duration = 2.0;
    const auto track = test::makeClickTrack(options);
    const auto path = tempPath("roundtrip.wav");
    std::string error;
    REQUIRE(test::writeWav16(path, track.audio, &error));

    WavDecoder decoder;
    CHECK(decoder.supports(path));
    const DecodeResult result = decoder.decode(path);
    REQUIRE(result.ok);
    CHECK(result.audio.sampleRate == 44100);
    CHECK(result.audio.channels == 1);
    CHECK_NEAR(result.audio.duration(), track.audio.duration(), 0.002);
    REQUIRE(result.audio.samples.size() == track.audio.samples.size());
    for (std::size_t i = 0; i < 500; ++i) {
        CHECK_NEAR(result.audio.samples[i], track.audio.samples[i], 1.0 / 32767.0);
    }
}

TEST_CASE(audio.wav24_and_float32) {
    using namespace bgmedia;
    // One frame of 24-bit PCM: 0.5 * 8388607 = 4194304.
    std::vector<std::uint8_t> pcm24{0x00, 0x00, 0x40};
    writeCustomWav(tempPath("pcm24.wav"), 1, 1, 48000, 24, pcm24);
    WavDecoder wav;
    auto result = wav.decode(tempPath("pcm24.wav"));
    REQUIRE(result.ok);
    REQUIRE(result.audio.samples.size() == 1);
    CHECK_NEAR(result.audio.samples[0], 0.5, 0.0001);

    std::vector<std::uint8_t> floats(4);
    const float value = -0.25f;
    std::memcpy(floats.data(), &value, sizeof(value));
    writeCustomWav(tempPath("float32.wav"), 3, 1, 44100, 32, floats);
    result = wav.decode(tempPath("float32.wav"));
    REQUIRE(result.ok);
    REQUIRE(result.audio.samples.size() == 1);
    CHECK_NEAR(result.audio.samples[0], -0.25, 1e-6);
}

TEST_CASE(audio.wav8_and_pcm32) {
    using namespace bgmedia;
    writeCustomWav(tempPath("pcm8.wav"), 1, 1, 22050, 8,
                   std::vector<std::uint8_t>{0xFF, 0x00, 0x80});
    WavDecoder decoder;
    auto result = decoder.decode(tempPath("pcm8.wav"));
    REQUIRE(result.ok);
    REQUIRE(result.audio.samples.size() == 3);
    CHECK_NEAR(result.audio.samples[0], 127.0 / 128.0, 1e-5);
    CHECK_NEAR(result.audio.samples[1], -1.0, 1e-6);
    CHECK_NEAR(result.audio.samples[2], 0.0, 1e-6);

    const std::uint32_t bits = 0x40000000u; // 0.5 in Q31
    std::vector<std::uint8_t> pcm32(4);
    std::memcpy(pcm32.data(), &bits, sizeof(bits));
    writeCustomWav(tempPath("pcm32.wav"), 1, 1, 48000, 32, pcm32);
    result = decoder.decode(tempPath("pcm32.wav"));
    REQUIRE(result.ok);
    REQUIRE(result.audio.samples.size() == 1);
    CHECK_NEAR(result.audio.samples[0], 0.5, 1e-6);
}

TEST_CASE(audio.wav_rejects_truncated) {
    const auto path = tempPath("truncated.wav");
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write("RIFF", 4);
    out.write("\x10\x00\x00\x00", 4);
    out.write("WAVEfmt ", 8);
    out.close();
    bgmedia::WavDecoder decoder;
    const auto result = decoder.decode(path);
    CHECK(!result.ok);
    CHECK(!result.error.empty());
}

#if defined(BGMEDIA_HAVE_MINIAUDIO) && BGMEDIA_HAVE_MINIAUDIO
TEST_CASE(audio.miniaudio_decodes_wav_mp3_flac) {
    using namespace bgmedia;
    const std::filesystem::path fixtureDir = BGMEDIA_TEST_FIXTURE_DIR;
    MiniaudioDecoder decoder;
    for (const char* name : {"decode_123.wav", "decode_123.mp3", "decode_123.flac"}) {
        const std::filesystem::path path = fixtureDir / name;
        if (!std::filesystem::exists(path)) {
            throw test::Failure(std::string("missing fixture: ") + path.string());
        }
        CHECK(decoder.supports(path));
        const DecodeResult result = decoder.decode(path);
        REQUIRE(result.ok);
        CHECK(result.audio.sampleRate == 44100);
        CHECK(result.audio.channels == 2);
        CHECK_NEAR(result.audio.duration(), 4.0, 0.12);
        CHECK(!result.audio.samples.empty());
        for (std::size_t i = 0; i < std::min<std::size_t>(1000, result.audio.samples.size()); ++i) {
            CHECK(std::isfinite(result.audio.samples[i]));
        }
    }
}
#endif

TEST_CASE(audio.pcm_hash_and_downmix) {
    using namespace bgmedia;
    AudioData audio;
    audio.channels = 2;
    audio.sampleRate = 48000;
    audio.samples = {1.0f, 1.0f, -0.5f, -0.5f, 0.25f, 0.25f};
    std::string error;
    const PcmBuffer pcm = PcmBuffer::fromAudio(audio, &error);
    REQUIRE(pcm.valid());
    REQUIRE(pcm.samples.size() == 3);
    CHECK_NEAR(pcm.samples[1], -0.5, 1e-6);
    CHECK(pcm.contentHash != 0);

    const PcmBuffer same = PcmBuffer::fromAudio(audio, &error);
    CHECK(same.contentHash == pcm.contentHash);

    audio.samples[0] = 0.99f;
    const PcmBuffer changed = PcmBuffer::fromAudio(audio, &error);
    CHECK(changed.contentHash != pcm.contentHash);
}

TEST_CASE(audio.antiphase_stereo_keeps_analysable_pulse) {
    using namespace bgmedia;
    AudioData stereo;
    stereo.sampleRate = 48000;
    stereo.channels = 2;
    stereo.samples = {0.8f, -0.8f, 0.0f, 0.0f,
                      -0.7f, 0.7f, 0.0f, 0.0f};
    const auto mono = stereo.toMono();
    REQUIRE(mono.size() == 4);
    CHECK_NEAR(mono[0], 0.8, 1e-6);
    CHECK_NEAR(mono[2], -0.7, 1e-6);

    // An ordinary stereo mix still averages channels.
    stereo.samples = {0.8f, 0.6f, -0.7f, -0.5f};
    const auto normal = stereo.toMono();
    REQUIRE(normal.size() == 2);
    CHECK_NEAR(normal[0], 0.7, 1e-6);
    CHECK_NEAR(normal[1], -0.6, 1e-6);
}

TEST_CASE(audio.multichannel_downmix_keeps_distinct_channels) {
    using namespace bgmedia;
    AudioData surround;
    surround.sampleRate = 48000;
    surround.channels = 6;
    surround.samples = {1.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,
                        0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 0.0f};
    const std::vector<float> mono = surround.toMono();
    REQUIRE(mono.size() == 2);
    CHECK_NEAR(mono[0], 1.0 / 6.0, 1e-6);
    CHECK_NEAR(mono[1], 1.0 / 6.0, 1e-6);
}
