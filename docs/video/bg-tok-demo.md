# BG Tok fixture integration (Linux)

This optional demo consumes local MP4 inputs through the BG Tok **test** file
provider, its MediaDelivery and the range ABI. It does not contact TikTok.
It requires the BG Tok provider-core PR at or after commit `ba9dd63`, Cargo,
CMake, FFmpeg development libraries, SQLite development libraries, and SDL3.

```sh
cargo build --manifest-path /path/to/BG-Tok/Cargo.toml -p bg-tok-fixture-ffi
cmake -S /path/to/BG-Media -B build -DBGMEDIA_BUILD_VIDEO=ON \
  -DBGMEDIA_BUILD_VIDEO_FFMPEG=ON -DBGMEDIA_BUILD_VIDEO_CACHE=ON \
  -DBGMEDIA_BUILD_VIDEO_DEMO=ON -DBGMEDIA_BUILD_DESKTOP=OFF \
  -DBGMEDIA_TOK_FIXTURE_ROOT=/path/to/BG-Tok \
  -DBGMEDIA_TOK_FIXTURE_LIB=/path/to/BG-Tok/target/debug/libbg_tok_fixture_ffi.a
cmake --build build --target bgmedia_video_demo
build/bin/bgmedia_video_demo --tok-cache /private/cache/directory one.mp4 two.mp4 three.mp4
```

The cache directory must be private to the user and not shared with another
cache process. Inputs must not be modified during use. Space pauses, N/P
navigate, arrows seek, and A switches between two test accounts after
cancelling the previous session and clearing its cache. Fixture source identities use Linux file metadata,
not cryptographic hashes; this is appropriate only for immutable test inputs.
The Rust fixture library is separate from the production Rust core.

With tests enabled, `ctest --test-dir build -R bgmedia.video_tok_integration
--output-on-failure` generates three small MP4s and checks the SDL dummy
video/audio flow, pause, seek, preloading and rapid navigation. Dummy audio
checks submitted samples, not physical speaker output. It starts with a clean
test-owned cache, so it also verifies calls to the Rust provider.
