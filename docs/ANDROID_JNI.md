# BG Media на Android: план JNI-интеграции

> Это проектный документ, а не уже собранный Android-модуль. C ABI и JNI-код
> ниже — контракт следующего этапа; в текущем репозитории есть только
> платформенно-независимое ядро и desktop-адаптер.

Документ описывает интеграцию **без изменения ядра**. BG Media не зависит от
Telegram, Android SDK и конкретного аудиовыхода; Android получает тонкий
JNI-адаптер и реализации `IAudioDecoder`/`IPlaybackController`.

## 1. Что переиспользуется

| Слой | Android-решение |
|---|---|
| BG Beat (анализ) | `bgmedia_core` без изменений, NDK C++20 |
| BG Sync | `PlaybackClock` + JNI-адаптер AAudio |
| BG Motion | `ChoreographyBuilder`/`Sampler`, тот же код |
| BG Audio decode | miniaudio (NDK) или MediaCodec→PCM |
| BG Audio output | AAudio (API 26+) или Oboe; host adapter |
| Кеш | `context.cacheDir`/`filesDir`, те же `.bgm`-файлы |
| BG Video (позже) | MediaCodec Surface + общий media clock |

## 2. Сборка NDK

Требования: Android NDK r27+ (C++20, `std::filesystem`), CMake 3.24+.

```bash
cmake -S . -B build-android-arm64 \
  -DCMAKE_TOOLCHAIN_FILE="$ANDROID_NDK/build/cmake/android.toolchain.cmake" \
  -DANDROID_ABI=arm64-v8a \
  -DANDROID_PLATFORM=android-26 \
  -DANDROID_STL=c++_shared \
  -DCMAKE_BUILD_TYPE=Release \
  -DBGMEDIA_BUILD_DESKTOP=OFF \
  -DBGMEDIA_BUILD_TESTS=OFF \
  -DBGMEDIA_BUILD_TOOLS=OFF \
  -DBGMEDIA_BUILD_MINIAUDIO=ON \
  -DBGMEDIA_WARNINGS_AS_ERRORS=OFF
cmake --build build-android-arm64 --parallel
```

Целевые ABI: `arm64-v8a` (основной), `x86_64` (эмулятор). Не включать SDL в
Android-сборку: core и декодер от него не зависят.

Минимальный CMake для JNI-моста:

```cmake
add_library(bgmedia_jni SHARED
    android/bgmedia_jni.cpp
)
target_link_libraries(bgmedia_jni PRIVATE
    bgmedia_core
    bgmedia_miniaudio
    android
    log
)
target_compile_features(bgmedia_jni PRIVATE cxx_std_20)
target_compile_options(bgmedia_jni PRIVATE -fexceptions -frtti)
```

## 3. C ABI для JNI

JNI-код не должен протаскивать C++-исключения через границу JNI. Добавляется
небольшая C-обёртка (её же сможет использовать iOS через Objective-C++):

```c
// include/bgmedia/capi.h
typedef struct bgm_result bgm_result;

typedef void (*bgm_progress_fn)(void* user, float fraction, const char* stage);

bgm_result* bgm_analyze_file(const char* utf8_path,
                             const char* utf8_cache_dir,
                             double min_bpm, double max_bpm,
                             bgm_progress_fn progress, void* user,
                             int* out_status);
const char* bgm_result_json(const bgm_result* result);
void bgm_result_free(bgm_result* result);
```

Реализация создаёт `MiniaudioDecoder`, `PcmBuffer`, `AnalysisCache`,
`Analyzer`, сериализует `AnalysisResult` в JSON (или компактный формат) и
возвращает строку. Для больших карт событий в будущем лучше flatbuffer, но JSON
проще для первого Android-этапа.

## 4. JNI-мост

```cpp
#include <jni.h>
#include "bgmedia/capi.h"

extern "C" JNIEXPORT jstring JNICALL
Java_com_beygram_bgmedia_BgMediaNative_analyzeFile(
    JNIEnv* env, jclass, jstring jpath, jstring jcache,
    jdouble minBpm, jdouble maxBpm) {
    const char* path = env->GetStringUTFChars(jpath, nullptr);
    const char* cache = env->GetStringUTFChars(jcache, nullptr);
    if (!path || !cache) {
        if (path) env->ReleaseStringUTFChars(jpath, path);
        if (cache) env->ReleaseStringUTFChars(jcache, cache);
        return nullptr;
    }

    int status = 0;
    bgm_result* result = bgm_analyze_file(path, cache, minBpm, maxBpm,
                                           nullptr, nullptr, &status);
    env->ReleaseStringUTFChars(jpath, path);
    env->ReleaseStringUTFChars(jcache, cache);
    if (!result) {
        return nullptr;
    }
    jstring json = env->NewStringUTF(bgm_result_json(result));
    bgm_result_free(result);
    return json;
}
```

Kotlin:

```kotlin
object BgMediaNative {
    init { System.loadLibrary("bgmedia_jni") }

    @JvmStatic external fun analyzeFile(
        path: String,
        cacheDir: String,
        minBpm: Double = 55.0,
        maxBpm: Double = 200.0
    ): String?
}

suspend fun analyze(context: Context, file: File): Analysis = withContext(Dispatchers.Default) {
    val json = BgMediaNative.analyzeFile(
        file.absolutePath,
        File(context.cacheDir, "bg-media").apply { mkdirs() }.absolutePath
    ) ?: error("BG Media analysis failed")
    AnalysisJson.decode(json)
}
```

Правила JNI:

- не вызывать Android UI из native-потока;
- при progress-callback из другого потока нужен
  `JavaVM::AttachCurrentThread`/`DetachCurrentThread`; проще начать с
  синхронного вызова на `Dispatchers.Default` и прогрессом на стороне Kotlin;
- всегда освобождать `GetStringUTFChars`;
- не передавать `std::string`/C++-объекты через JNI;
- `@Keep` и ProGuard-правила для класса с `external`-методами;
- для длинных треков добавить cancel-token (`std::atomic_bool`) и C ABI
  `bgm_cancel(handle)`, иначе пользователь не сможет прервать анализ.

## 5. Декодирование на Android

Два допустимых варианта:

1. **miniaudio** (быстрый старт): тот же `MiniaudioDecoder`, файл открывается
   по пути из app-private storage. MP3/FLAC/WAV поддержаны; не требует
   MediaCodec.
2. **MediaExtractor/MediaCodec** (продакшн): декодировать в PCM на Kotlin/NDK и
   передавать в `PcmBuffer`. Это даёт нативный Android-декодер и лучше
   контролирует DRM/стриминг. `IAudioDecoder` остаётся тем же.

Для содержимого APK использовать `AAssetManager` и передавать байты в
`ma_decoder_init_memory`; путь к asset не всегда является обычным файлом.

Кеш — только app-private:

```kotlin
val cache = File(context.filesDir, "bg-media-analysis").apply { mkdirs() }
```

Пути не входят в ключ кеша; ключ — fingerprint PCM + конфиг анализа.

## 6. Воспроизведение и AAudio

`MiniaudioPlayer` — desktop-адаптер. На Android нужен отдельный
`AaudioPlayer : IPlaybackController`:

- `AAudioStreamBuilder` с `AAUDIO_FORMAT_PCM_FLOAT`, stereo, 48 kHz;
- callback читает потоковый декодер (MediaCodec или miniaudio) в input ring
  buffer;
- `sourcePosition` — следующий исходный кадр, который будет отрендерен;
- latency: `AAudioStream_getTimestamp(stream, CLOCK_MONOTONIC, &framePosition,
  &timeNanos)` плюс поправка на ring buffer; либо
  `AAudioStream_getFramesWritten` минус аппаратный буфер, плюс UI-тримминг;
- play/pause — старт/стоп потока; seek — пауза, сброс ring buffer, decoder
  seek, возобновление;
- rate — resampling (для превью допустимо изменение питча);
- `discontinuity++` на seek/loop/rate;
- `TransportSnapshot` заполняется теми же полями, что в desktop; BG Sync не
  меняется.

Важно: не блокировать audio callback на mutex, который держит UI; seek
выполняется после остановки потока или через lock-free команду.

## 7. Кеш и производительность

- Анализ выполняется в фоне (`Dispatchers.Default`/Executor), UI не блокируется.
- 5-минутный трек на desktop: ~1.36 с (см. `MEASUREMENTS.md`); на телефоне
  ожидаемо медленнее, поэтому кеш обязателен.
- Кеш читается до анализа; повторное открытие трека — миллисекунды.
- Не хранить спектрограмму в UI-модели Kotlin: для waveform/spectrogram
  использовать нативный рендер или компактную текстуру.
- Ограничение in-memory decoder 1 GiB; для очень длинных файлов позже —
  streaming/chunked analysis.

## 8. Тестирование на Android

1. Host CI: те же CTest-тесты на Windows/Linux (алгоритмы не зависят от ОС).
2. NDK compile-only job для arm64-v8a и x86_64.
3. Instrumentation-тест JNI: положить короткий WAV в assets, вызвать
   `analyzeFile`, проверить BPM/события/JSON и повторный cache hit.
4. Проверить отсутствие сети: в приложении нет HTTP-клиентов; политика
   Network Security Config не требуется, но можно запретить cleartext.
5. Проверить, что кеш лежит в app-private каталоге и не требует permissions.

## 9. Что не делаем сейчас

- Telegram-клиент, UI BeyGram, сервер, облачную синхронизацию.
- Видеоплеер (BG Video) и эффекты (BG Effects).
- 3D-модель персонажа.

## 10. Задел под BG Video на Android

Когда появится видео:

- `MediaCodec` + `Surface` отдаёт кадры с PTS;
- BG Sync остаётся audio-master (AAudio);
- video-presenter выбирает кадр по audible media time;
- `bgmedia_video` — отдельный NDK-таргет, который линкуется только
  приложением с видео;
- `TransportSnapshot`/`PlaybackClock` уже покрывают seek, pause, rate и
  latency, поэтому ядро анализа и хореографии не переписывается.
