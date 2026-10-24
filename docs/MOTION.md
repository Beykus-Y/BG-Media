# BG Motion 2.0 — непрерывная хореография

## 1. Задача

Старый BG Motion вычислял позу только из активного `DanceCue`. В момент смены
cue менялись руки, корпус и ноги, потому что у каждой позы была своя формула без
перехода. BG Motion 2.0 заменяет это на **непрерывный keyframe timeline**,
который сэмплируется по media time.

Ключевое свойство: поза является чистой функцией

```text
pose = f(Choreography, mediaSeconds)
```

Никакого накопленного состояния, зависимости от FPS или порядка вызовов нет.
Это делает pause, resume, seek и произвольный доступ к позиции корректными по
построению.

## 2. Модель данных

```cpp
struct MotionKeyframe {
    double time;
    CharacterPose pose;
    Easing easingToNext;
    bool accent;
    int beatIndex;
};

struct AccentTarget {
    double time;          // exact musical beat
    CharacterPose pose;   // pose that must be reached here
    MoveKind move;
    bool bassAccent;
    bool downbeat;
    ...
};

struct Choreography {
    std::vector<DanceCue> cues;       // legacy/UI list
    std::vector<MotionKeyframe> keyframes; // authoritative smooth track
    std::vector<MotionClip> clips;    // authoring/debug
    std::vector<AccentTarget> accents;
    ...
};
```

`CharacterPose` расширен суставами (`headNod`, `kneeLeft/Right`,
`elbowLeft/Right`, `chestTwist`, `hipShift`, `footLeft/Right`), но старые поля
остались на своих местах. `ChoreographySampler::sample()` теперь использует
гладкий timeline; `sampleStepped()` сохранён для сравнения и обратной
совместимости.

## 3. Как строится таймлайн

`ChoreographyBuilder` генерирует ключи в четырёх слоях:

1. **Под-битовая сетка** — 8 фаз на каждый интервал между ударами. Это
   достаточно, чтобы Bounce, Sway, ArmWave и HeadNod имели форму, но не
   превращались в «покадровую» анимацию.
2. **Точные музыкальные события** — каждый beat является отдельным
   keyframe, независимо от сетки.
3. **Anticipation/release** — ключи на границах подготовки к акценту и его
   затухания.
4. **Границы движений и тишины** — переходы между барами и fade в freeze.

`AccentTarget` создаётся для каждого beat. Его поза вычисляется тем же
`evaluatedPose()`, что и ключ в этот момент. Поэтому сэмпл ровно в `target.time`
возвращает тот же keyframe pose. Это не «примерно около удара», а точное
совпадение по построению и по тесту.

## 4. Движения и координация

Библиотека движений:

| MoveKind | Что делает |
|---|---|
| `Bounce` | вертикальный groove, сгибание коленей, руки/корпус в фазе |
| `Sway` | перенос веса, lean, hip shift, chest twist, голова |
| `ArmWave` | противофазная волна руками с локтями и head turn |
| `HeadNod` | акцент головой с малым bob и расслабленными руками |
| `BassDip` | глубокий присед на bass accent, подготовленный заранее |
| `AccentPose` | акцентированная фиксация с поднятыми руками |
| `Pulse`, `Pose`, `Spin` | совместимые движения предыдущей версии |

Базовое движение выбирается по бару:

```text
bar 0 -> ArmWave
bar 1 -> Sway
bar 2 -> Bounce
bar 3 -> HeadNod
```

Bass accent заменяет акцент на `BassDip`; downbeat без bass accent — на
`AccentPose`. Все движения используют одну и ту же `CharacterPose`, поэтому
рендерер не переключает форматы.

### Переходы

Последние `movementBlend × beatDuration` секунд перед новым баром предыдущее
движение кроссфейдится в новое. Кроссфейд заканчивается **на** границе бара:
на акценте уже активна новая поза. Это исключает ситуацию, когда старая поза
побеждает на сильной доле.

Используется `smoothStep` по весу движения. Позы обеих сторон вычисляются в
одной и той же media time, затем смешиваются; это не crossfade между кадрами.

### Anticipation и release

Для акцента в момент `t_b`:

```text
t_b - preparation      t_b              t_b + hold + release
    weight 0     --->  weight 1  --->          weight 0
```

* preparation — доля локального beat-интервала (настраивается
  `ChoreographyStyle::anticipation`);
* hold/release — короткая фиксация и плавный возврат;
* amplitude — функция силы акцента и локального темпа.

Перед ударом движение уже началось, но **целевая поза достигается ровно в
`t_b`**, потому что overlay weight в этот момент равен 1, а в timeline стоит
keyframe с целевой позой.

## 5. Интерполяция

`ChoreographySampler`:

1. Бинарным поиском находит соседние keyframes.
2. Если время совпало с ключом (в пределах 1e-6 с), возвращает позу ключа
   без интерполяции. Поэтому акцент не «размазывается» между кадрами.
3. Для `Easing::Cubic` использует кубический Hermite с центральными
   производными по соседним ключам. Это даёт непрерывность позиции и скорости
   на внутренних ключах (C1) и естественную инерцию.
4. Для `Linear`, `EaseIn`, `EaseOut`, `EaseInOut`, `SmoothStep` использует
   соответствующий easing profile.
5. `rotation` интерполируется по кратчайшей дуге через turns, а не как обычное
   число. Например, `0.9 -> 0.1` проходит через 0, а не через 0.5.
6. `move`, `blend`, `phase`, `impact` — диагностические поля; физические
   каналы интерполируются отдельно.

Ключи генерируются с `Easing::Cubic`, поэтому основной танец имеет
непрерывную скорость. Разные easing-профили доступны для авторских ключей,
акцентов и будущих клипов.

## 6. Синхронизация с плеером

BG Motion не читает часы и не знает про FPS. Он получает media time из BG Sync:

* **pause** — media time не меняется, поза замирает;
* **resume** — сэмплирование продолжается с той же позиции;
* **seek** — поза вычисляется для новой media time, без истории;
* **rate** — меняется скорость течения media time, а не частота сэмплирования;
* **tempo change** — beat-интервалы берутся из BG Beat; transition-зоны
  строятся по фактическим времени, а не по фиксированному FPS;
* **output latency** — BG Sync сдвигает audible time, поэтому акцент
  отображается тогда, когда он слышен.

## 7. Быстрый темп

При уменьшении интервала между ударами BG Motion **уменьшает амплитуду**, а не
ускоряет суставы до неестественных скоростей:

```text
tempoAmplitude = clamp(beatInterval / 0.5, 0.52, 1.0)
```

Плюс окна anticipation/release ограничиваются локальным beat-интервалом, чтобы
акценты не перекрывались. Это позволяет сохранять читаемую хореографию на
160–200+ BPM.

## 8. Desktop demo

В карточке **CHOREOGRAPHY** есть три chip-кнопки:

* **SMOOTH / STEPPED** (`V`) — сравнение BG Motion 2.0 и старого ступенчатого
  сэмплера;
* **DEBUG** (`D`) — joint markers, траектории рук/головы, keyframe/beat ruler на
  сцене и диагностика `phase / blend / impact / key`;
* **LAB OFF / 92 / 123 / 163 / 180** (`B`) — синтетическая beat-сетка заданного
  BPM поверх текущего аудиотранспорта. Она не зависит от ошибок BG Beat:
  хореография строится из ровных ударов, а аудио остаётся транспортом.

CLI-эквиваленты для скриншотов:

```bash
bgmedia_desktop --file fixtures/beat_092.wav \
  --capture motion.bmp --capture-time 5.2 \
  --motion-lab 123 --motion-debug --no-audio
```

## 9. Что проверяют тесты

`tests/test_motion.cpp` (13 кейсов):

- каждый `AccentTarget` совпадает с позой сэмплера в точный beat (порог
  2e-4);
- anticipation начинается до beat, accent clip заканчивается после beat;
- нет разрывов на keyframe/cue границах (максимальная дельта на 1 мс < 0.07;
  на 2 мс по всему треку < 0.15);
- результат одинаков при 30/60/120 FPS и при произвольном порядке запросов;
- pause/seek/resume не имеют скрытого состояния;
- rate 1.5× и PlaybackClock дают ту же media-time позу;
- разные easing-профили дают разные значения;
- кратчайшая дуга поворота работает;
- изменение BPM не даёт скачка позы;
- при 200 BPM амплитуда меньше, чем при 100 BPM;
- присутствуют все шесть новых/основных движений;
- старый `sampleStepped` остаётся доступным;
- offbeat-бас с `nearestBeat=-1` не превращается в BassDip на быстром темпе;
- freeze в тишине имеет нулевую энергию и impact.

Полный набор сейчас: **47 тестов**, 6 CTest-групп, 0 падений.

## 10. Границы и дальнейшее развитие

- BG Motion выдаёт нормализованные позы; физическая IK/ограничения суставов —
  ответственность конкретного presenter/rig adapter.
- Desktop-робот использует двухзвенную IK для ног, чтобы стопы не скользили.
  Это демонстрационный presenter, а не часть core.
- Downbeat/meter остаются вероятностными (confidence из BG Beat).
- В будущем можно добавить animation graph, additive layers, facial cues,
  trajectory constraints и 3D rig adapter без изменения формата
  `CharacterPose`/keyframe timeline.
- Видеослой не затрагивается: BG Video сможет использовать тот же media-time
  clock и сэмплировать poses/effects по PTS.
