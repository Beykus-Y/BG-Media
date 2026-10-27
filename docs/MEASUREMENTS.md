# BG Media — реальные измерения

Все числа ниже получены на этом репозитории, а не заявлены заранее. Это один
прогон на конкретной машине; алгоритмы детерминированы для одинакового PCM и
конфигурации, но время выполнения зависит от CPU/сборки.

## Машина и сборка

- CPU: AMD Ryzen 7 5700X, 8 ядер / 16 потоков, 3.4 GHz
- RAM: 32 GB
- OS: Windows 11 Pro 10.0.26200
- Компилятор: MSVC 19.44 (Visual Studio 2022 Preview)
- Сборка: Ninja, `CMAKE_BUILD_TYPE=Release`
- Core: 44.1 kHz, FFT 2048, hop 512, 80 mel-полос
- Замеры: `bgmedia_benchmark` и `bgmedia_analyze` из `build/windows/bin`

Команда воспроизведения:

```powershell
build\windows\bin\bgmedia_benchmark.exe --json docs\benchmark.json
```

## Качество на синтетическом корпусе

`beat precision/recall` считаются при допуске **60 мс** на интервале
`[1 c, duration−1 c]`. `mean err` — средняя абсолютная ошибка времени удара.
Все треки — оригинальные синтетические сигналы из `tools/support`.

| Тест | Ожидаемый BPM | Определённый BPM | BPM conf. | Precision / Recall | Mean err | Анализ | Realtime |
|---|---:|---:|---:|---:|---:|---:|---:|
| 92 BPM | 92 | 92.06 | 0.84 | 0.93 / 1.00 | 2.4 мс | 68 мс | 294× |
| 123 BPM | 123 | 123.00 | 0.69 | 0.93 / 1.00 | 3.4 мс | 125 мс | 160× |
| 163 BPM | 163 | 162.75 | 0.73 | 0.91 / 1.00 | 2.6 мс | 104 мс | 193× |
| 92 → 123 BPM | 92 / 123 | 104.72 weighted | 0.54 | 0.93 / 1.00 | 4.0 мс | 132 мс | 181× |
| Тишина 4–9 с | 92 | 92.10 | 0.63 | 0.75 / 1.00 | 2.1 мс | 36 мс | 360× |
| Басовые акценты | 92 | 92.04 | 0.73 | 0.93 / 1.00 | 3.7 мс | 67 мс | 300× |
| Неоднозначный ритм | 92 + 138 | 91.90 | 0.62 | 0.93 / 1.00 | 7.2 мс | 162 мс | 124× |

Метрический уровень и downbeat на regression-корпусе (детали и old/new:
[docs/BG_BEAT_AUDIT.md](BG_BEAT_AUDIT.md)):

| Стимул | Истинный пульс | Новый результат | Beat F1 | Downbeat F1 | Сдвиг времени |
|---|---:|---:|---:|---:|---:|
| Offbeat 100 BPM | 100 | 99.81 | 1.000 | 1.000 | +4.4 мс |
| 12/8 ternary | 66.2 | 66.24 | 0.999 | 0.995 | +4.0 мс |
| Half-time 80 BPM | 80.7 | 80.82 | 1.000 | 1.000 | +3.9 мс |
| Four-on-the-floor | 125 | 125.10 | 0.999 | 1.000 | +3.9 мс |
| Shuffle 80 BPM | 81.0 | 80.98 | 0.999 | 1.000 | +4.0 мс |

### Как читать эти числа

- **92/123/163**: ошибка BPM 0.00–0.25 BPM; recall 1.00, средняя ошибка
  времени 2–4 мс. Сдвиг −13.5 мс устранён исправлением timestamp convention
  STFT-фрейма, а не добавленной константой.
- **92 → 123**: глобальное `104.72` — **не** стабильный темп. Это взвешенное
  агрегированное значение для логов/сортировки. Реальная карта:
  `0.00–11.49 s: 92.06 BPM`, `13.99–24.00 s: 123.04 BPM`, между ними
  `TempoTransition`. `tempoStable=false`.
- **Тишина**: регион `4.005–8.998 s`; в нём нет ни одного удара или басового
  акцента. Precision 0.75 связан с ударами около границ региона; recall 1.00.
- **Басовые акценты**: на тестовом треке с басом только на downbeat-ах и
  громкими broadband swell-ами детектор не создал акцентов на swell-ах.
  Broadband-громкость не проходит low-band energy test. Offbeat-бас больше
  не привязывается насильно к ближайшему beat и не превращается в BassDip.
- **Неоднозначный ритм**: 92 и 138 BPM наложены; confidence 0.62,
  `octaveAmbiguous=true` (теперь это metric ambiguity), оба кандидата в
  `bpmCandidates`. Детектор не утверждает единственный «правильный» темп.
- **Confidence — эвристика**, не калиброванная вероятность. UI подписывает
  `HEURISTIC BPM SCORE` и отдельно показывает `level`/`phase`.

## 5-минутный трек

300-секундный синтетический трек 118 BPM (44.1 kHz, mono), 3 прогона:

| Метрика | Значение |
|---|---:|
| min | 1523 мс |
| median | 1651 мс |
| max | 1686 мс |
| realtime factor (median) | **182×** |
| обнаружено | 118.00 BPM, confidence 1.00 |
| удары | 589 |
| spectrogram frames | 12 920 |
| cache write | 4 мс |
| cache read | 9 мс |

То есть обычный 5-минутный трек анализируется примерно за **1.52–1.75 секунды** на
этой машине. Запись кеша включается в измерение только при первом анализе;
чтение кеша — единицы миллисекунд. Metric-level refinement и проверка ramp
добавили около 3% к прежним 1.53 с; это осознанная цена более устойчивой
метрики.
Замеры времени шумят между запусками: в другом прогоне медиана была 1.69 с.

## GUI и синхронизация с аудиовыходом

24-секундный WAV в desktop-приложении:

- анализ: **71 мс** (≈336× realtime) на фикстуре 24 с, при повторном
  открытии — cache hit **0.4 мс**;
- аудиоустройство: `Наушники (fifine SC9)`, расчётная задержка **32 мс**;
- smoke-сценарий `--audio-smoke` (реальный miniaudio device):

| Сценарий | Измеренная audible-позиция |
|---|---:|
| старт + 1.5 с воспроизведения | ≈1.52 с |
| pause, seek на 5.000 с | 4.968 с (компенсация latency) |
| resume + 0.8 с | 5.821 с |
| rate 1.25× + 0.8 с | delta 1.059 с (ожидаемо ~1.0 с) |

Это проверяет не только открытие устройства, но и прохождение
`sourcePosition → latency → PlaybackClock → cursor`. Ошибка в несколько
десятков миллисекунд ожидаема из-за периода callback и оценки задержки
устройства; в UI есть sync trim ±150 мс.

## Автотесты

```text
1/12 ... 12/12
100% tests passed, 0 tests failed out of 12
Total Test time (real) ≈ 6.5 s
```

115 unit/integration кейсов, включая WAV (8/16/24/32-bit, float), MP3/FLAC
decode, straight/offbeat/ternary/half-time/shuffle/dense/drift BPM,
downbeats, onset timestamp, tempo change, silence, bass semantics, cache
corruption/versioning, sync latency/seek/rate, choreography, BG Motion 2.0,
VRM/retargeting, BG Motion 3.0 (28 dance-кейсов) и BG Motion 3.1
(`timeline`, `warp`, `sync31`, 150/180 BPM, 6/8, 12/8, tempo ramp).

## BG Motion 2.0

13 motion-тестов проверяют не только «нет NaN», а конкретные инварианты:

| Проверка | Порог / результат |
|---|---|
| Совпадение `AccentTarget` с сэмплом в точный beat | `< 2e-4` |
| Разрыв на keyframe/cue границе (сэмпл ±1 мс) | `< 0.07` по нормированной позе |
| Непрерывность по треку (шаг 2 мс) | `< 0.15` |
| Детерминизм при 30/60/120 FPS и обратном порядке | побитово одинаковые позы |
| Изменение BPM | нет скачка `> 0.08` на границах |
| 200 BPM против 100 BPM | амплитуда быстрого трека `< 0.82` от медленного |
| Антиципация | preparation начинается до beat, release заканчивается после |
| Freeze в тишине | `energy = 0`, `impact = 0` |

Акцентная поза гарантируется тем, что `AccentTarget.pose` вычисляется в точный
beat, в timeline ставится keyframe с тем же значением, а сэмплер при совпадении
времени возвращает keyframe без интерполяционного размытия. Подробнее:
[docs/MOTION.md](MOTION.md).

Desktop Motion Lab (синтетическая сетка 123 BPM, debug-режим):
`docs/images/desktop-motion-debug.png`. Ступенчатый режим для A/B:
`docs/images/desktop-motion-stepped.png`.

## BG Motion 3.0

Измерения выполнены `bgmedia_motion_qa` на `Model1.vrm` (1.62 м), Release,
Ryzen 7 5700X, 240 Hz. Для каждого клипа брались BPM `0.8×`, `1.0×`, `1.25×`
от авторского.

| Клип | seam rot | seam pos | maxW °/s | slide mm | IK err mm | reach drop cm | swings |
|---|---:|---:|---:|---:|---:|---:|---:|
| Groove (77/96/120) | 0.000° | 0.0000 | 103 | 2.15 | 0.00 | 0.00 | 7–8 |
| Hip-Hop (74/92/115) | 0.000° | 0.0000 | 325 | 2.26 | 0.00 | 0.00 | 9 |
| Side Step (86/108/135) | 0.000° | 0.0000 | 320 | 2.51 | 0.00 | 0.00 | 14 |
| Energetic (101/126/158) | 0.000° | 0.0000 | 582 | 2.29 | 0.00 | 0.00 | 12 |
| Idle (48/60/75) | 0.000° | 0.0000 | 3 | 0.00 | 0.00 | 0.91 | 0 |

`maxW` — максимальная угловая скорость любой кости; `slide` — максимальное
смещение заретаргетенной опорной лодыжки; `reach drop` — автоматическое
опускание таза для достижимости опоры.

Все 20 пар из пяти стилей проверены на каждом из 80/123/180 BPM (60
переходов); переключение запрашивалось в середине beat (не на сетке):

| Метрика перехода | Худшее значение | Порог теста |
|---|---:|---:|
| угловая скорость | 591 °/s | < 900 °/s |
| угловое ускорение | 17 484 °/s² | < 4.0e4 °/s² |
| шаг таза за кадр | 0.00313 height (≈5.1 мм) | < 0.006 |
| шаг root за кадр | 0.00312 height | < 0.006 |
| шаг стопы за кадр | 0.00993 height (≈1.6 см) | < 0.02 |
| нефинитные позы | 0 | 0 |

Дополнительно проверено 60 быстрых переключений подряд (чаще длины fade):
`maxAngularSpeed < 1100 °/s`, `hipsStep < 0.008`, `footStep < 0.025`,
расписание остаётся < 80 сегментов.

Контактные листы реального Desktop (`bgmedia_desktop --capture`, тот же
retargeter/SDL_GPU) для Groove → Hip-Hop и Energetic → Idle подтверждают
отсутствие скачков на 24 кадрах вокруг каждого перехода. Артефакты лежат
локально в `artifacts/dance-captures/` и не коммитятся из-за лицензии модели.

## BG Motion 3.1 — музыкальная синхронизация

Новый инструмент `bgmedia_sync_qa` прогоняет 21 детерминированный сценарий
(60–180 BPM, 3/4, 6/8, 12/8, half-time, dense hats, offbeat, shifted accents,
silence, tempo ramp, sudden change) и измеряет beat/meter/downbeat, predicted
accent approach, качество анимации и стоимость. Машинный отчёт:
[docs/sync_qa_3_1.json](sync_qa_3_1.json); baseline того же корпуса до
analyzer-изменений: [docs/sync_qa_baseline_pre_refinement.json](sync_qa_baseline_pre_refinement.json).

| Метрика | BG Beat 3.0 | BG Motion 3.1 | Изменение |
|---|---:|---:|---:|
| Beat F1 (среднее) | 0.8685 | **0.9837** | +0.1152 |
| Median beat error (median по кейсам) | 1.73 мс | **1.71 мс** | −0.02 мс |
| P95 beat error (median по кейсам) | 5.80 мс | **5.80 мс** | 0 |
| Downbeat F1 (среднее) | 0.8129 | **0.9406** | +0.1277 |
| Meter correct | 20/21 | **21/21** | +1 |
| Downbeat trusted | 18/21 | 18/21 | 0 |
| Worst retargeted slide | — | **1.95 мм** | — |
| Non-finite | 0 | **0** | 0 |

Ключевые случаи:

| Сценарий | BPM 3.0 → 3.1 | Beat F1 3.0 → 3.1 | Median 3.0 → 3.1 |
|---|---|---|---:|
| 150 BPM | 75.0 → **149.9** | 0.674 → **1.000** | 37.3 → **6.2 мс** |
| 180 BPM | 59.9 → **179.7** | 0.505 → **1.000** | 1.5 → 1.5 мс |
| 6/8 | 59.9 → **119.8** | 0.676 → **1.000** | 1.7 → 5.1 мс |
| 12/8 | 90.1 → 90.2 | 1.000 → 1.000 | 1.7 → 1.7 мс |
| Tempo ramp 80→120 | 112.1 → 101.9 | 0.634 → **1.000** | 2.1 → 1.7 мс |
| Dense hats 160 | 80.0 → 80.0 (half-time) | 0.658 → 0.658 | 7.1 → 7.1 мс |
| Offbeat accents 100 | 100.0 → 100.0 | 1.000 → 1.000 | 27.6 → 27.6 мс |
| Silence gap | 92.1 → 92.1 | 1.000 → 1.000 | 2.0 → 2.0 мс |
| Half-time 80 | 80.3 → 80.3 | 1.000 → 1.000 | 2.4 → 2.4 мс |

Dense hats 160 на уровне анализа остаётся half-time (80 BPM), но
`DanceDirector::chooseRate` выбирает `rate=2.0`, effective **160.0 BPM**. Это
честный metric-level выбор, а не ошибка BPM. 180 BPM hysteresis также
предпочитает 1× (effective 179.7), а не half-time 90.

Стоимость 3.1 на 24-секундных сценариях:

| Метрика | Значение |
|---|---:|
| Analysis, среднее | 137.1 мс |
| Timeline build, среднее | 0.03 мс |
| Sample Off | 0.90 µs/frame |
| Sample Expressive | 1.98 µs/frame |
| Worst foot slide | 1.95 мм |
| Non-finite | 0 |

Точная акцентная поза на beat проверяется unit-тестом (`< 1e-4°`); QA
accent approach — predicted, не измеренная задержка монитора. Offbeat-100
имеет систематическое смещение ~27.6 мс — известное ограничение 3.1, см.
[docs/BG_MOTION_3_1.md](BG_MOTION_3_1.md).

## Ограничения измерений

- Синтетические сигналы дают более чистую картину, чем реальная музыка:
  реальные записи содержат свинг, реверберацию, сбивки, вокал и переменную
  громкость. Confidence и флаги неоднозначности — часть результата, а не
  украшение.
- BPM-температура и допуск 60 мс выбраны как практический компромисс для
  хореографии. Для более строгих задач можно уменьшать допуск и смотреть
  распределение ошибок.
- Время анализа зависит от CPU, компилятора и длины трека; при первом запуске
  SDL3-приложения дополнительно есть инициализация UI/аудио, не входящая в
  `analysisMs`.
- Значения кеша измеряются на локальном SSD; на мобильном flash они будут
  другими.
- Кеш привязан к `kAnalyzerVersion`; после изменения алгоритма старые записи
  становятся miss, а не дают устаревший результат. Текущая версия — 2;
  schema/cache format также 2 из-за новых confidence-полей.
- Реальные MP3 лежат вне репозитория и не коммитятся. Их эталонный BPM
  неизвестен; результаты по ним не используются как доказательство качества.

## Артефакты

- `docs/benchmark.json` — машинночитаемый вывод benchmark.
- `docs/images/desktop-analysis.png` — анализ и синхронизированный курсор.
- `docs/images/desktop-tempo-change.png` — mixed tempo + transition.
- `docs/images/desktop-silence.png` — пауза, freeze и отсутствие выдуманных
  ударов.
- `docs/images/desktop-empty.png` — стартовый экран.
- `docs/images/desktop-motion-debug.png` — BG Motion 2.0, joints/trails,
  Motion Lab 123 BPM.
- `docs/images/desktop-motion-stepped.png` — старый ступенчатый режим для
  сравнения.
- `docs/images/desktop-metric.png` — 12/8: metric ambiguity, alternatives и
  `meter uncertain`.

## BG Media 3D — VRM 0.x (дополнение 0.3.0)

Оборудование и сборка те же: Ryzen 7 5700X, 32 GB RAM, Windows 11,
MSVC 19.44 Release, D3D12. Модель: `models/Model1.vrm` (VRM 0.x, VRoid
Studio 1.22.1), 18 539 292 байт. Размер 3D-таргета: 960×640, offscreen
RGBA8 sRGB + D32.

| Метрика | Значение |
|---|---:|
| импорт GLB + JSON + 15 base-color PNG | 176–179 мс |
| nodes / meshes / triangles | 175 / 3 / 35 881 |
| humanoid bindings / core retarget bones | 54 / 24 |
| materials / textures / decoded textures | 15 / 27 / 15 |
| skins × joints | 3 × 171 |
| GPU frame, 15 draw calls, 35 881 tris | 0.6–0.8 мс |
| readback 960×640 | 0.3–0.5 мс |
| retarget одного кадра | ~0.02 мс |
| UI loop на этой машине (8 мс pacing) | ~125–132 FPS |
| GPU textures с mip-chain | ~112 MB |
| process RSS в 3D | ~160–168 MB |
| ASan Debug import Model1 | ~1.7 с |
| ASan тесты | 82 passed, 0 failed, sanitizer clean |

Проверка воспроизводимости: одинаковые `(CharacterPose, mediaTime, options)`
дают побитово одинаковые `nodeGlobals`; seek/rewind воспроизводит ту же позу.
Проверка опоры стоп: `maxFootError ≤ 2 мм` для Bounce/BassDip на Model1,
XZ-координаты стоп совпадают с bind-позициями в пределах 4 мм.

Скриншот работающего Desktop во время воспроизведения аудио:
`artifacts/desktop3d_playing.png` (локально, не коммитится из-за лицензии
модели). Он снят с окна реального `bgmedia_desktop.exe` во время Play, а не
получен из внешнего 3D-просмотрщика.


## BG Motion 3.2 measurements

Measured on the same local Windows desktop, Release build, Model1.vrm.

| Measurement | Result |
|---|---|
| Procedural dance generation (32 beats, 257 keys, 14 bone tracks) | 7.5–9 ms |
| Motion overlay composition (57 cues) | 1.3–1.7 µs/frame |
| Spring simulation (36 chains / 116 bones) | 0.08–0.21 ms/frame |
| Retargeting per frame | ~0.01 ms |
| Spring stability at 30 / 60 / 120 / 180 FPS | finite, max stretch < 0.25% |
| Spring collider penetration (worst, 150 frames) | < 8 mm |
| Planted-foot slide, neonPulse at 104 BPM | 3.43 mm |
| Foot IK error, neonPulse | 0.00 mm |
| Loop seam, neonPulse | 0.000 deg / 0.0000 height units |
| ASan | 141 passed, 0 failed, sanitizer clean |
| Release tests | 141 passed, 0 failed |
| Demo reel | 52 s, 720×450, 30 FPS |

The generated-dance CPU cost is one-time at library load; runtime composition
and spring simulation together stay below 0.2 ms/frame on the test machine.
