# Third-party components

| Component | Version / source | License | Used by |
|---|---|---|---|
| SDL3 | 3.4.18, fetched by CMake from libsdl.org | zlib | desktop application and SDL_GPU adapter |
| miniaudio | 0.11.25 | public domain / MIT-0 | MP3/FLAC/WAV decoding; desktop playback |
| KissFFT | 131.2.0 | BSD-3-Clause | FFT inside `bgmedia_core` |
| stb_truetype | nothings/stb snapshot, vendored header | MIT / public domain | UI font rasterizer |
| stb_image | nothings/stb snapshot, vendored header | MIT / public domain | embedded VRM texture decoding |
| cgltf | 1.15, vendored single header | MIT | glTF 2.0 / GLB parsing in `bgmedia_vrm` |
| Inter | 4.1 (Google/rsms Inter release) | SIL Open Font License 1.1 | desktop UI font |

License texts are in `third_party/licenses/`,
`third_party/cgltf/LICENSE` and `assets/fonts/Inter-LICENSE.txt`.

The engine core (`bgmedia_core`) links only KissFFT. cgltf/stb_image are private
to `bgmedia_vrm`; SDL3/miniaudio/stb_truetype remain in adapters/apps and do not
leak into the public analysis or choreography API.
