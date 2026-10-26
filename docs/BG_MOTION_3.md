# BG Motion 3.0 — авторские танцевальные клипы

BG Motion 3.0 — следующий слой после BG Motion 2.0. Музыкальная цепочка не
изменилась: **BG Beat** строит сетку, **BG Sync** отдаёт audible media time, а
**BG Motion 3.0** сэмплирует по этой сетке заранее авторизованную хореографию.
Таймер анимации по-прежнему не управляет танцем.

```text
PCM ─► BG Beat ─► BeatClock ─► DanceSchedule ─► ClipPose
                                               │
                              BG Sync ─────────┘
                                               ▼
                                  HumanoidRetargeter.applyClip()
                                               ▼
                                  SDL_GPU VRM 0.x scene
```

BG Motion 2.0 (`CharacterPose`, `docs/MOTION.md`) остаётся доступным: он
используется в 2D-режиме и включается в 3D флагом `--legacy-motion`. BG Beat и
BG Sync не менялись.

---

## 1. Что нового

| Возможность | BG Motion 2.0 | BG Motion 3.0 |
|---|---|---|
| Источник движения | формулы + keyframe timeline | авторские клипы с запечёнными треками |
| Единица времени | media seconds | музыкальные beats через `BeatClock` |
| Ноги | амплитуда/фаза + IK | stage-space цели стоп, контакты, pivot |
| Переход | crossfade поз | crossfade поз + общий план на такт |
| Смена стиля | движение | Manual: на следующий entry beat; Auto: по фразам |
| Root motion | `CharacterPose.rotation` | yaw + перенос центра, с pivoting вокруг стоп |
| Переносимость | CharacterPose | JSON, humanoid bone names, height units |
| Проверка | motion-тесты | + 26 dance-тестов, QA-метрики и контактные листы |

Ключевое свойство осталось прежним: поза — **чистая функция**

```text
pose = f(расписание клипов, BeatClock, mediaSeconds)
```

Поэтому pause, resume, seek, произвольный FPS и перемотка назад не требуют
накопленного состояния. Единственное состояние — само расписание: какие клипы
и когда играют.

---

## 2. Быстрый старт

Нужна VRM 0.x-модель; по умолчанию приложение ищет её в `models/` и в
`build/windows/bin/assets/`.

```powershell
# один танец, Motion Lab 100 BPM, без музыки
build\windows\bin\bgmedia_desktop.exe --vrm models\Model1.vrm --3d ^
  --dance groove --dance-bpm 100

# показать все четыре танца по 8 битов, затем Idle
build\windows\bin\bgmedia_desktop.exe --vrm models\Model1.vrm ^
  --dance-plan groove,hiphop,sidestep,energetic,idle --dance-span 8 --dance-bpm 120

# Auto: стиль выбирается по анализу трека
build\windows\bin\bgmedia_desktop.exe --file track.mp3 --vrm models\Model1.vrm ^
  --3d --dance auto --autoplay

# Presentation: только сцена, без панелей
build\windows\bin\bgmedia_desktop.exe --vrm models\Model1.vrm ^
  --presentation --dance energetic --dance-bpm 126
```

`--dance` принимает `groove`, `hiphop`, `sidestep`, `energetic`, `idle`,
`auto` и `test`. `--dance-plan` принимает те же имена через запятую.
`--dance-span` задаёт длину одного стиля в музыкальных битах (по умолчанию 16).

### Управление в 3D

| Действие | Клавиша / кнопка |
|---|---|
| Groove / Hip-Hop / Side Step / Energetic | `1` / `2` / `3` / `4` |
| Idle | `0` или `5` |
| Manual / Auto | `A` или кнопки **MANUAL/AUTO** |
| Loop / one-shot | `N` или **LOOP/ONE SHOT** |
| TEST DANCE (все стили) | `T` или **TEST DANCE** |
| Presentation mode | `P` |
| BPM Motion Lab | `↑` / `↓` (с `Shift` — по 10) или `+` / `-` |
| Foot contact / skeleton / root debug | `F5` / `F6` / `F7` |
| Камера: орбита / zoom / reset | ЛКМ + drag / колесо / `C` |
| 2D / 3D | `G` |

В 3D-режиме с загруженными танцами клавиши `1`–`4` переключают стили, а не
2D/3D. Вернуться в 2D можно через `G` или chip **2D**.

При выборе стиля вручную смена начинается на ближайшем музыкальном entry beat
(обычно в пределах одного такта). В Auto режиме план строится сразу на весь
трек.

---

## 3. Библиотека

`assets/dances/` содержит пять клипов. Все они — авторская keyframe-
хореография, а не motion capture.

| id | Стиль | BPM | Диапазон | Beats | Energy | Что происходит |
|---|---|---:|---:|---:|---:|---|
| `idle` | Idle | 60 | 30–120 | 8 | 0.00 | дыхание, лёгкий перенос веса; идёт по wall-clock, не по музыке |
| `groove` | Groove | 96 | 68–128 | 16 | 0.30 | step-close, контрвращение таза и плеч, волна по корпусу |
| `hiphop` | Hip-Hop | 92 | 66–128 | 16 | 0.62 | bounce, cross-over, chest pop, изоляции, поворот на левой стопе |
| `sidestep` | Side Step | 108 | 80–150 | 16 | 0.55 | шаги в сторону, cross-front, подъёмы колена, toe tap |
| `energetic` | Energetic | 126 | 92–176 | 16 | 0.95 | широкий squat, выпады, четверть- и 180-градусный поворот, freeze |

Каждый танцевальный клип:

- зациклен и бесшовен: последний ключ совпадает с первым;
- содержит `accents`, `contacts` и `entryBeats`/`exitBeats`;
- использует 16 ключей на beat (у Idle — 12);
- не содержит ссылок на конкретную VRM-модель.

`idle` имеет `tempoSync: false`: один beat клипа равен одной секунде. Это
позволяет дыханию не ускоряться вместе с музыкой.

---

## 4. Музыкальное время

### 4.1 BeatClock

`BeatClock` — единственный источник соответствия `mediaTime ↔ musicalBeat`.

- `BeatClock::constant(bpm, firstBeatTime, beatsPerBar)` — Motion Lab, ручной
  BPM или синтетическая сетка.
- `BeatClock::fromAnalysis(analysis)` — сглаженная сетка BG Beat. Она
  интерполирует реальные beat-события, выдерживает пропуски, голосованием
  определяет bar offset и экстраполирует за пределы анализа.
- `beatAt(t)` и `timeAtBeat(b)` взаимно обратны; `beatInBar(beat)` даёт
  0-based позицию в такте.
- `bpmAt(t)` возвращает локальный темп сглаженной сетки.

Клип не знает, откуда взялась сетка. Он просто получает musical beat.

### 4.2 Metric level

`DanceDirector::chooseRate()` выбирает одно из значений **0.5 / 1 / 2**:
столько beats клипа приходится на один музыкальный beat.

- 1.0 — клип играет в своём темпе;
- 0.5 — клип в два раза медленнее музыки (half-time);
- 2.0 — клип в два раза быстрее музыки (double-time).

Выбирается уровень, который приводит эффективный темп клипа в его
`bpmRange`. Если ни один уровень не попадает в диапазон, выбирается ближайший,
а `DanceFrame.speedLimited` сообщает об ограничении. Диапазон клипа — это не
жёсткий запрет, а зона, в которой авторы гарантируют читаемую хореографию.

### 4.3 Расписание

`DanceSchedule` — массив `ScheduleSegment`:

```cpp
struct ScheduleSegment {
    const HumanoidClip* clip;
    double startTime;       // начало crossfade, media seconds
    double alignTime;       // музыкальная точка отсчёта
    double entryClipBeat;   // beat клипа в alignTime
    double rate;            // 0.5 / 1 / 2
    double fadeSeconds;
    bool loop;
    std::string reason;     // объяснение для UI/Auto
};
```

Для `tempoSync`-клипа:

```text
clipBeat(t) = entryClipBeat
            + (beatAt(t) - beatAt(alignTime)) * rate
```

Для Idle (`tempoSync == false`):

```text
clipBeat(t) = entryClipBeat + (t - alignTime) * clip.bpm / 60
```

`DanceSchedule::sample()` выбирает последний сегмент, который уже начался, и
возвращает его позу. Если идёт fade, поза смешивается с предыдущим сегментом.
Это делает seek и произвольный доступ корректными: будущее расписания не
зависит от того, в каком порядке вызывали `sample()`.

### 4.4 Переходы

Переход — часть расписания, а не скрытое состояние игрока.

1. Новый сегмент ставится на ближайший музыкальный **entry beat**.
2. `fadeSeconds` = два музыкальных beat, ограниченные диапазоном 0.45–1.1 с.
3. `startTime = alignTime - fadeSeconds / 2`: переход straddle-ит удар, а не
   начинается после него.
4. Смешивание идёт по `smootherstep` (`6t^5 - 15t^4 + 10t^3`), поэтому
   скорость и ускорение на концах перехода близки к нулю.
5. Кватернионы смешиваются shortest-arc slerp; yaw стоп и root — по кратчайшей
   дуге; позиции — линейно.
6. `speedLimited` и `transitionProgress` видны в UI.

`DancePlayer` при backward seek или большом forward jump вызывает
`reseed()`. Новый сегмент ставится без fade, но сохраняет **фазу такта**:
после seek в середине такта клип продолжается с соответствующей музыкальной
позиции, а не начинается с beat 0. Это же поведение проверяет
`dance.pause_resume_and_seek`.

### 4.5 Manual и Auto

**Manual** (`DancePlayer::select`):

- стиль выбирается пользователем;
- смена ждёт entry beat, если это не `immediate`;
- future-сегменты обрезаются, чтобы не накладывать несколько планов;
- one-shot проигрывает клип один раз и возвращается в Idle на ближайшем beat.

**Auto** (`DanceDirector::planAuto`):

- делит трек на фразы (по умолчанию 16 битов);
- считает энергию фразы из секций BG Beat, onset-плотности, басовых акцентов
  и (если есть) средней громкости спектрограммы;
- применяет гистерезис, чтобы стиль не дёргался каждую фразу;
- при низкой уверенности BG Beat остаётся на стабильном Groove;
- в тишине ставит Idle;
- генерирует один сегмент на смену стиля, а не на каждый beat;
- пишет причину решения в `ScheduleSegment.reason` и `describeSchedule()`.

Auto — это планировщик, а не анализатор: он не меняет BG Beat и не выдаёт
эвристику за измерение.

---

## 5. Формат клипа (`bgmotion.clip`, version 1)

Клип — JSON-файл. Верхний уровень:

| Поле | Тип | Назначение |
|---|---|---|
| `format` | string | всегда `"bgmotion.clip"` |
| `version` | int | `1` |
| `id` | string | уникальный id |
| `name`, `description`, `author`, `origin` | string | метаданные |
| `style` | string | `idle`, `groove`, `hiphop`, `sidestep`, `energetic`, `custom` |
| `bpm` | number | авторский темп |
| `bpmRange` | `[min, max]` | комфортный диапазон |
| `beats` | number | длина цикла |
| `beatsPerBar` | int | по умолчанию 4 |
| `keysPerBeat` | int | плотность ключей |
| `loop`, `seamless`, `tempoSync` | bool | поведение времени и шва |
| `energy` | 0..1 | подсказка планировщику |
| `entryBeats`, `exitBeats` | number[] | удобные точки входа/выхода |
| `accents` | object[] | `{beat, strength, kind}` |
| `contacts` | object[] | `{foot, start, end, kind}` |
| `tracks` | object | запечённые треки |

`keyCount = beats * keysPerBeat + 1`. Последний ключ — это замыкание цикла;
для `seamless` он должен совпадать с первым.

### 5.1 Система координат

Clip frame (right-handed, «dance frame»):

```text
+X = character left
+Y = up
+Z = forward
```

Длины — в долях роста модели (`1.0` = весь рост). Углы поворота — в оборотах
(turns), не в радианах и не в градусах.

Кости заданы **именами VRM humanoid** (`hips`, `spine`, `chest`, `leftUpperArm`,
`head`, ...). Ротация кости — локальная ротация относительно родителя в
bind-позе VRM. `identity` означает bind-позу.

### 5.2 Треки

```jsonc
{
  "tracks": {
    "bones": {
      "hips":          [q0x,q0y,q0z,q0w, q1x,...],
      "leftUpperArm":  [ ... ],
      "head":          [ ... ]
    },
    "hips": [x,y,z, x,y,z, ...],
    "root": [x,z,yaw, ...],
    "footLeft":  [x,lift,z,yaw, ...],
    "footRight": [x,lift,z,yaw, ...],
    "handLeft":  [curl, ...],
    "handRight": [curl, ...]
  }
}
```

- `bones` — массив кватернионов `(x,y,z,w)`, 4 значения на ключ. Ноги
  (`leftUpperLeg`, `leftLowerLeg`, `leftFoot` и правые) в клипах не
  записываются: они решаются IK из целей стоп.
- `hips` — смещение таза от bind-позиции, в height units, в root space.
- `root` — `[x, z, yaw]`: перенос центра в height units и yaw в оборотах.
- `footLeft` / `footRight` — `[x, lift, z, yaw]`: позиция в **stage space**
  относительно bind-лодыжки, `lift` — подъём над полом, `yaw` — обороты.
- `handLeft` / `handRight` — curl пальцев: `0` — раскрытая кисть, `1` — кулак.

### 5.3 Контакты

`contacts` описывают, когда стопа стоит на полу:

```json
{ "foot": "left", "start": 0.0, "end": 1.75, "kind": "stance" }
{ "foot": "left", "start": 10.0, "end": 11.6, "kind": "pivot" }
```

- `stance` — стопа неподвижна; позиция в клипе не должна меняться.
- `pivot` — стопа остаётся на месте, но может поворачиваться (yaw).
- вне контакта стопа в swing: её можно переносить.

`ClipPose::Foot::contact` — сглаженный 0..1 вес контакта (ramp 0.2 beat), а
`phase` — `Swing`, `Plant`, `Stance`, `Pivot` или `Release`.

### 5.4 Валидация

`parseClipJson()` и `validateClip()` проверяют:

- версию и формат;
- наличие `id`, `bpm`, `beats`, `keysPerBeat`, `tracks`;
- конечность чисел и единичную длину кватернионов;
- точное количество значений на трек;
- неизвестные имена костей;
- пересечение контактов на одной стопе;
- смещение стопы внутри `stance` (warning о slide);
- совпадение первого и последнего ключа для `seamless`;
- диапазоны `bpmRange`, `entryBeats`, `accents`.

Ошибка одного файла не останавливает загрузку остальных: `DanceLibrary`
записывает диагностику и пропускает плохой клип.

---

## 6. Retargeting

`HumanoidRetargeter::applyClip(ClipPose, mediaTime)` — отдельный путь от
`apply(CharacterPose)`, но с тем же generic-риггом.

1. Кости из clip frame переводятся в оси модели (`left`, `up`, `forward`).
2. Локальные delta применяются к bind-позе; не keyed-кости остаются в bind.
3. Пальцы сгибаются по `handLeft/handRight`.
4. Ноги не анимируются напрямую. Из `foot*` строятся stage-space цели:
   `stage = bindAnkle + toModel(foot.position) * height`.
5. Цели переводятся в root space с учётом yaw и `rootOffset`.
6. Two-bone IK решает бедро/голень; стопа удерживается уровнем пола, а её
   yaw/pitch берутся из клипа.
7. Если широкая стойка или перенос веса делают цель недостижимой, таз
   опускается на минимально необходимую величину (`reachHipDrop`), а не
   отрывает стопу от пола.
8. `stats.maxFootError` показывает остаточную недостижимость; на штатных
   клипах она нулевая.

Root-трансформация применяется рендерером, а не запекается в hips:

```text
T(modelOffset + rootOffset * userScale + pivot * userScale)
* modelTransform
* T(-pivot * userScale)
* S(userScale)
```

`pivot` — середина bind-лодыжек на полу. Поэтому персонаж поворачивается
вокруг опорной стопы, а не вокруг центра экрана. Камера при этом не следует
за root автоматически: сцена остаётся на месте, танцор перемещается в ней.

`HumanoidRetargeter::stagePosition(pose, node)` возвращает позицию узла с
учётом root yaw и переноса — это используется debug-видом foot/root trails.

---

## 7. Desktop UI

В 3D-режиме с загруженной библиотекой справа появляются три карточки:

1. **DANCE** — Manual/Auto, Loop/One Shot, Test Dance, пять стилей, BPM.
2. **CLIP STATE** — текущий клип, следующий, phase bar с accent-метками,
   скорость, диапазон, progress crossfade и причина выбора.
3. **DEBUG** — foot contact, skeleton, root motion; текущие фазы стоп,
   root yaw/смещение, reach drop, IK error и retarget ms.

Внизу сцены показываются название клипа, Motion Lab/BG Sync, bar/beat и
акцент. `--presentation` / `P` оставляет только сцену и подпись; `P` или `Esc`
возвращает интерфейс.

Важно: в 3D-режиме клавиши `1`–`4` принадлежат танцам. Это осознанный
компромисс, чтобы не добавлять ещё один модификатор; 2D/3D переключается
через `G`.

---

## 8. Своя хореография

Исходники танцев — Python DSL в `tools/dancegen/`:

```powershell
python tools\dancegen\generate_dances.py
python tools\dancegen\generate_dances.py --only groove
```

`bgdance.py` — authoring tool, а не генератор синусоид:

- `d.pose(beat, control=value, hit=True)` ставит key pose;
- `Curve` — монотонный cubic Hermite, который не overshoot-ит ключи;
- `Foot.step/hold/pivot/orbit` задаёт footwork в сантиметрах эталонного
  персонажа 160 см;
- `auto_balance()` переносит центр масс над загруженной стопой;
- `auto_roll()` наклоняет таз/плечи вслед за опорной ногой;
- `turn_about(t0, t1, point)` вращает root вокруг выбранной точки, а не
  вокруг центра;
- `contacts` экспортируются из footwork автоматически;
- последний ключ замыкает цикл, поэтому `seamless=true` выполняется по
  построению.

Правила автора:

1. Ставьте шаги на beat или на явно задуманный off-beat, но не «примерно».
2. Внутри `stance` не двигайте `x/z` стопы — только `pivot` может менять yaw.
3. На каждый loop должен быть вход и выход (`entryBeats`/`exitBeats`),
   совпадающие с сильными позициями.
4. Держите `maxRootExcursion` в разумных пределах: камера не обязана бегать
   за танцором.
5. Проверяйте `bgmedia_motion_qa` и контактный лист, а не только «нет NaN».

Клип можно написать и вручную по формату из раздела 5. После добавления
запустите `bgmedia_motion_qa` и `bgmedia_tests.exe dance.`.

---

## 9. QA и тесты

### 9.1 `bgmedia_motion_qa`

```powershell
build\windows\bin\bgmedia_motion_qa.exe --dances assets\dances ^
  --model models\Model1.vrm
```

Инструмент печатает по каждому клипу и BPM:

- `seamRot`, `seamPos`, `seamV` — разрыв позы и скорости на loop seam;
- `maxW`, `maxA` — максимальная угловая скорость/ускорение костей;
- `hipsV`, `footV` — скорость таза и стоп;
- `slide(mm)` — максимальное смещение **заретаргетенной** опорной лодыжки;
- `footErr(mm)` — остаточная ошибка IK;
- `reachDrop(cm)` — автоматическое опускание таза;
- `swings`, `swingMax`, `pathLen` — реальность шагов;
- `root yaw` — амплитуда поворота.

Затем идёт матрица всех переходов `from -> to` при 80/123/180 BPM с
максимальными шагами позы на 240 Hz.

### 9.2 Контактные листы

`bgmedia_motion_sheet` рендерит клип или расписание на реальном VRM через тот
же retargeter и SDL_GPU:

```powershell
build\windows\bin\bgmedia_motion_sheet.exe --model models\Model1.vrm ^
  --dances assets\dances --plan groove,hiphop --span 16 --bpm 100 ^
  --beats 0:32:16 --view front --cols 8 --out artifacts\sheet.bmp
```

Для покадровой проверки реального Desktop-приложения (каждый кадр — отдельный
запуск `bgmedia_desktop.exe` с `--capture`, поэтому результат детерминирован):

```powershell
python scripts\capture_dance_sequence.py --name transition ^
  --args "--dance-plan groove,hiphop,sidestep,energetic,idle --dance-span 8 --dance-bpm 120" ^
  --times 3.5:4.5:24 --bpm 120 --gif
```

### 9.3 Тесты

`bgmedia.dance` — 26 кейсов. Они проверяют не «нет исключения», а инварианты:

- формат, ошибки загрузчика, fuzz-коррупция, библиотека из пяти клипов;
- loop seam, ограниченные скорость/ускорение, root motion, различимость
  стилей, использование всего тела;
- контакты и отсутствие slide на минимальном VRM и на реальной модели;
- повороты вокруг опорной стопы, перенос на другой скелет, левую/правую руку;
- BeatClock, чистоту расписания, seek/pause/resume, one-shot;
- все 20 пар стилей на каждом из 80/123/180 BPM (60 переходов) и 60 быстрых
  переключений подряд;
- metric levels 0.5/1/2 в диапазоне 80–180 BPM;
- Manual entry beat, Auto-план по энергии/тишине/confidence, смену темпа.

Всего в проекте: **82 теста**, 9 CTest-групп:
`audio`, `analysis`, `cache`, `sync`, `choreography`, `motion`, `vrm`,
`retarget`, `dance`.

### 9.4 Измерения

На `Model1.vrm` (1.62 м) при 240 Hz, Release, Ryzen 7 5700X:

| Метрика | Значение |
|---|---:|
| loop seam: поворот | 0.000° у всех клипов |
| loop seam: позиция | 0.0000 height units |
| max angular speed | ≤ 582 °/s (Energetic на 158 BPM) |
| max angular acceleration | ≤ 15 440 °/s² (Hip-Hop на 115 BPM) |
| slide опорной стопы | ≤ 2.51 мм (Side Step на 135 BPM) |
| IK foot error | 0.00 мм |
| reach drop | 0.00 см у танцев, 0.91 см у Idle |
| foot swings за цикл | 7–14 |
| max root excursion | ≤ 0.24 height units |
| max root yaw | 180° у Energetic, 40° у Hip-Hop |
| худший переход (240 Hz) | 591 °/s, 17 484 °/s², hips ≈ 5.1 мм/кадр |

Пороговые значения тестов: `maxAngularSpeed < 1000 °/s`,
`maxAngularAcceleration < 2.5e5 °/s²`, `slide < 0.0035 height`,
`footError < 4 мм`, transition `hipsStep < 0.006 height`, `footStep < 0.02`.
Они выбраны с запасом: реальные значения заметно ниже.

---

## 10. Ограничения

- Клипы — авторская keyframe-хореография, не motion capture. Это даёт
  контроль и повторяемость, но не заменяет захват живой пластики.
- Поддерживается VRM 0.x humanoid. VRM 1.0, morph targets, blink, spring
  bones и полный MToon в этот слой не входят.
- Переход смешивает **позиции** стоп. Если оба клипа в этот момент опираются
  на разные точки, возможен короткий контролируемый slide внутри 0.45–1.1 с;
  тесты ограничивают его, но не устраняют физически.
- Нет floor collision, лестниц, прыжков с реальной баллистикой и учёта
  нескольких опор одновременно.
- Root motion ограничен по амплитуде; камера не является motion-capture
  камерой и не обязана следовать за танцором.
- Auto зависит от BG Beat: при низкой уверенности он намеренно остаётся на
  Groove, а не угадывает.
- Нет runtime-редактора клипов; authoring — Python DSL или JSON вручную.

---

## 11. Приватность и файлы

`models/Model1.vrm` и его встроенные текстуры **не входят в репозиторий**:
лицензия `OnlyAuthor` / `Redistribution_Prohibited`. `.gitignore` исключает
`/models/*` (кроме README), а также `*.vrm`, `*.glb` и каталоги экспорта
модели. Скриншоты с этой моделью лежат в `/artifacts/` и тоже не коммитятся.

Для публичных примеров используйте `tests/fixtures/minimal.vrm` или другую
VRM-модель с разрешающей лицензией. Код загрузчика, риггера и рендерера от
конкретной модели не зависит.

---

## 12. Связанные документы

- [MOTION.md](MOTION.md) — BG Motion 2.0: `CharacterPose`, keyframes, accents.
- [VRM_INTEGRATION.md](VRM_INTEGRATION.md) — VRM 0.x, SDL_GPU, retargeting.
- [ARCHITECTURE.md](ARCHITECTURE.md) — место BG Motion 3.0 в движке.
- [MEASUREMENTS.md](MEASUREMENTS.md) — измерения и условия экспериментов.
