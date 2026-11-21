# BG Video and the SDL3 demo

`BGMedia::Video` owns the feed and the decoder interface. Enable the optional
`BGMedia::VideoFFmpeg` adapter to open regular, self-contained MP4 files. The demo
uses libav directly for demuxing and decoding. The SDL window renders decoded
frames, and its audio stream plays 48 kHz stereo PCM.

On Debian 13, install CMake, Ninja, a C++20 toolchain, pkg-config, SDL3 and the
FFmpeg development packages. The `ffmpeg` and `ffprobe` commands generate and
inspect the test fixtures. On Linux the SDL file picker needs a desktop portal
or Zenity.

```bash
sudo apt-get install build-essential cmake ninja-build pkg-config libsdl3-dev \
  libavformat-dev libavcodec-dev libavutil-dev libswscale-dev libswresample-dev \
  ffmpeg zenity
cmake -S . -B build/video-demo -G Ninja -DCMAKE_BUILD_TYPE=Release \
  -DBGMEDIA_BUILD_VIDEO=ON -DBGMEDIA_BUILD_VIDEO_FFMPEG=ON \
  -DBGMEDIA_BUILD_VIDEO_DEMO=ON -DBGMEDIA_BUILD_DESKTOP=OFF \
  -DBGMEDIA_BUILD_MINIAUDIO=OFF -DBGMEDIA_BUILD_TOOLS=OFF
cmake --build build/video-demo
ctest --test-dir build/video-demo --output-on-failure
build/video-demo/bin/bgmedia_video_demo
```

Use **O** to select multiple MP4 files, drop a group of files onto the window,
or pass paths on the command line:

```bash
build/video-demo/bin/bgmedia_video_demo first.mp4 second.mp4 third.mp4
```

Use **N/P** to switch, **Space** to pause or resume, **S** to stop and reset,
and **Left/Right** to seek by five seconds. Click the timeline to seek to a
position. The footer shows presentation and source FPS, playback position, next
clip preparation status, tracked buffer use and process RSS on Linux.

## Embed playback

Link `BGMedia::Video` in the source tree (or `BGMedia::bgmedia_video` from the
installed `BGMediaTargets.cmake`) and include `bgmedia/video/Playback.hpp`. Construct
`Playback` with a provider for background preparation, a factory for opening a
cold decoder, and a host implementation of `PlaybackOutput`. The output must
outlive the player. It consumes or copies video frames and audio spans during
the callback. Its queue depth is expressed in stereo PCM sample frames at 48
kHz. A platform using an audio callback should use its own synchronized queue
and expose that backlog to the playback thread.

Submit the ordered media with `setQueue` and call `update` regularly from the
same thread that handles commands. `snapshot` reports the selected item,
playback and preparation states, position, and buffer use. Hosts can pass
monotonic timestamps to `update` to drive the scheduler from their own loop.
The library owns seeking, pause/resume, queue transitions and A/V scheduling.
SDL supplies only audio output, frame upload, and the demo UI.

## Playback and ownership

`Feed::activate(index)` selects a clip and transfers its prepared decoder if
available. Preparation opens the next source, discovers the tracks, and
decodes through the first video frame. The foreground uses the same decoder
and prefetched frames. A cold open uses the same adapter without the prefetched
frames. The demo supports `--no-preload` to compare these paths. A missed or
failed preparation can be retried as a cold open. The user can navigate past
an unreadable file.

The decoder returns presentation timestamps in seconds relative to the container
start. Libav reorders frames and Playback schedules them against the playback clock.
The clock uses submitted PCM time minus the output's reported backlog, with monotonic
time when the stream has no queued PCM. This estimates presentation at the SDL
device boundary. Latency in the OS mixer and physical output device remains
outside that measurement. Seek flushes the decoder and SDL audio stream, decodes
from a keyframe and discards or trims frames before the requested position.

The demo reserves up to 40 MiB for converted next-clip frames, up to 64 MiB in
the active video queue, one reusable RGBA buffer and one texture. Audio targets
200 ms of queued PCM plus up to one decoded frame. These quantities describe
buffers managed by BG Video and the SDL player. Codec reference frames, container
probing, SDL internals, graphics drivers and shared libraries contribute to RSS.
The adapter currently accepts a maximum of 3840×2160 pixels per frame and bounds
stream count and stream probing. Limits on app buffers are independent of the
size of the input file.

`Snapshot` contains metadata by value and holds no prepared frames or decoder.
A new selection, queue, budget, or close cancels the previous generation and
discards any stale result. The feed runs at most one preparation call at a time.
A provider must use cooperative cancellation and bounded I/O. The destructor
waits up to 100 ms for the worker after requesting cancellation and normally
joins it. If a provider ignores that request, its one outstanding call and the
provider's shared state can outlive the feed until the call returns. Stop
callbacks must be nonblocking and must not call back into the feed. FFmpeg
checks cancellation and a two-second deadline during open, probing and
preparation. Those checks cannot interrupt an OS call already blocked in a
filesystem driver. Playback and cold open currently run on the SDL thread.

## BG Tok integration boundary

BG Tok PR #3 provides ordered recommendations, account and content IDs, and
`MediaDelivery::open` for cancellable byte ranges. A future host adapter should
keep the account scope in `MediaId.account` and map each content ID to its
stable item identity. Expiring source handles belong to the provider; refreshing
a handle with the same source ID must preserve the item's position in the feed.

A remote decoder needs a seekable byte source over BG Tok ranges. The bridge
should offer time-bounded open/read/seek/close, distinguish EOF, cancellation,
expiry and I/O failure, and propagate the preparation generation's cancellation
to the Rust operation. Read into caller-owned buffers with explicit byte
limits. BG Video owns scheduling, buffered frames and any future shared disk
cache. BG Tok owns source resolution, refresh and source-specific reporting.
The current `Media.localPath` is the local path for this demo. It is not a
remote byte source or a stable C ABI.

The verified backend is software decoding on Linux. Hardware decoding needs a
device and measurements for frame transfer or native surface presentation.
Windows and mobile playback need separate validation.

## Linux validation

Debian 13 x86-64, FFmpeg 7.1.5, SDL 3.2.10, Xvfb/X11 with SDL's software
renderer and a PulseAudio virtual sink. The Zenity picker loaded three clips
into the demo. The PulseAudio monitor recorded the expected 330, 660 and 990 Hz
tones in order. CTest additionally exercised pause/resume, stop, seek, 48 rapid
switches, malformed and empty inputs, cancellation, and the shutdown deadline
with an uncooperative test provider.

The original Linux proof of concept was measured before extracting the playback
library. The measurement starts at item selection and ends after SDL submits the first
decoded frame. It excludes display scanout and device latency. These are
two-second local H.264/AAC clips at 25 FPS. The 180p numbers use six transitions
per mode. The 1080p numbers use two transitions per mode.

| Resolution | Median with preload | Median without preload | Peak RSS with / without | Peak tracked buffers with / without |
| --- | ---: | ---: | ---: | ---: |
| 320×180 | 8.28 ms | 11.11 ms | 112.8 / 106.0 MiB | 1.85 / 1.63 MiB |
| 1920×1080 | 10.25 ms | 25.52 ms | 202.8 / 175.3 MiB | 55.46 / 47.55 MiB |

Peak RSS covers the whole process. The test compares the demo's presentation
clock with decoded PTS. It does not measure the physical screen-to-speaker
delay. To repeat the fixture run after CTest creates the clips:

```bash
demo=build/video-demo/bin/bgmedia_video_demo
fixtures=build/video-demo/tests/video-fixtures
/usr/bin/time -v "$demo" --verify "$fixtures"/{1,2,3}.mp4
/usr/bin/time -v "$demo" --verify --no-preload "$fixtures"/{1,2,3}.mp4
```
