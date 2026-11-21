# Opt-in TikTok Explore probe

This experimental CLI links the asynchronous Rust Explore host from BG Tok
PR #6 to the existing BG Media range adapter and FFmpeg decoder. It does not
render frames or play sound; it checks that each of three sources can produce
video and audio and that a seek can produce another frame. No persistent media
cache is enabled. It is separate from the SDL demo and ordinary CI.

With FFmpeg and SDL3 development dependencies installed:

```sh
cd /path/to/BG-Tok-integration
cargo build -p bg-tok-explore-ffi
cd /path/to/BG-Media-integration
cmake -S . -B build -DBGMEDIA_BUILD_VIDEO=ON \
  -DBGMEDIA_BUILD_VIDEO_FFMPEG=ON -DBGMEDIA_BUILD_VIDEO_DEMO=ON \
  -DBGMEDIA_BUILD_DESKTOP=OFF \
  -DBGMEDIA_TOK_EXPLORE_ROOT=/path/to/BG-Tok-integration \
  -DBGMEDIA_TOK_EXPLORE_LIB=/path/to/BG-Tok-integration/target/debug/libbg_tok_explore_ffi.a \
  -DBGMEDIA_ENABLE_LIVE_TIKTOK_TESTS=ON
cmake --build build --target bgmedia_tiktok_probe
ctest --test-dir build -R '^bgmedia.tiktok_live_probe$' --output-on-failure
```

`BG_TOK_LIVE_PROXY`, when explicitly set by the host, is passed to the Rust
HTTP transport; the probe does not print it. The test needs live Explore and
CDN access, fails when unavailable, and must not be enabled in normal CI.
Sources lacking a stable revision or a length corroborated by the CDN are
rejected. The probe does not send playback events and does not claim to
implement personalized recommendations.
