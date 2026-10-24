# Test fixtures

The files here are **original synthetic test signals**, not third-party music.

`decode_123.wav`, `decode_123.mp3` and `decode_123.flac` are the same 4-second,
44.1 kHz, stereo excerpt of the synthetic 123 BPM click track. They exist to
exercise miniaudio's WAV, MP3 and FLAC paths in `audio.miniaudio_decodes_wav_mp3_flac`.

The full analysis corpus is generated at test/runtime by
`tools/support/SyntheticSignals.cpp` and is not committed as large WAV files.
Use `bgmedia_fixturegen` to write the same material for manual listening:

```bash
build/windows/bin/bgmedia_fixturegen.exe fixtures
```

To regenerate the three encoded decoder fixtures from the generator output
(requires ffmpeg):

```powershell
ffmpeg -y -i fixtures\beat_123.wav -t 4 -ac 2 -c:a pcm_s16le tests\fixtures\decode_123.wav
ffmpeg -y -i fixtures\beat_123.wav -t 4 -ac 2 -c:a libmp3lame -b:a 128k tests\fixtures\decode_123.mp3
ffmpeg -y -i fixtures\beat_123.wav -t 4 -ac 2 -c:a flac tests\fixtures\decode_123.flac
```

The synthetic signals and fixtures are dedicated to the public domain under
CC0 1.0. The test code itself is covered by the repository MIT license.
