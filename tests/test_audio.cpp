undefined
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
