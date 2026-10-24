# BG Media 3D — VRM 0.x и скелетная анимация

Этот документ описывает первую полноценную интеграцию 3D-персонажа в BG Media:
реальный импорт `Model1.vrm`, SDL_GPU-рендерер, humanoid-rig, retargeting
BG Motion и синхронизацию с существующим аудиоплеером. Музыкальный анализ,
BG Beat, BG Motion и BG Sync не были переписаны и не зависят от SDL/GPU.

Локальная проверка: `artifacts/desktop3d_playing.png` — реальный скриншот
запущенного `bgmedia_desktop.exe` во время воспроизведения `beat_123.wav`
(не кадр из Blender/VRoid/внешнего просмотрщика). Файл не коммитится: модель
и её производные имеют ограничения `OnlyAuthor` / `Redistribution_Prohibited`.
См. `models/README.md`.

## 1. Итог

- Модель `models/Model1.vrm` импортируется как настоящий GLB/glTF 2.0 с
  VRM 0.x расширением.
- На экране — реальный персонаж с текстурами, глазами, волосами и одеждой,
  а не манекен, куб или статистика скелета.
- Персонаж стоит в 3D-сцене с перспективной камерой, освещением, полом,
  depth buffer и перспективой; камера вращается мышью.
- BG Motion (`Bounce`, `Sway`, `ArmWave`, `HeadNod`, `BassDip`,
  `AccentPose`, а также `Pulse`, `Spin`, `Pose`) применяется к настоящим
  humanoid-костям через generic retargeter.
- Используется существующая цепочка **BG Beat → BG Motion → BG Sync →
  VRM retargeting → renderer**. Второго аудиоплеера нет.
- Анимация сэмплируется по media time. Пауза, seek и rate не привязаны к FPS.
- 2D-робот сохранён и доступен по переключателю `2D` / клавише `3`.
- Старые тесты проходят; добавлены тесты импорта VRM, rig/retargeting,
  deterministic sampling и ASan-прогон.

## 2. Архитектура

```text
apps/desktop (SDL3 window, SDL_Renderer 2D UI, miniaudio playback)
  ├── bgmedia_miniaudio          decoder/player adapter
  ├── bgmedia_sdlgpu             SDL_GPU offscreen 3D adapter
  │     ├── bgmedia_vrm          importer / glTF / VRM 0.x
  │     └── bgmedia_rig          humanoid retargeter
  └── bgmedia_core               BG Beat + BG Motion + BG Sync
        └── bgmedia_vrm (public model types, no SDL)
```

Направление зависимостей:

- `bgmedia_core` — чистый C++20, без SDL, GPU, окна и аудиоустройства.
- `bgmedia_vrm` — cgltf + stb_image, только файл/память и математика.
- `bgmedia_rig` — зависит от `bgmedia_vrm` и `CharacterPose`, но не от SDL.
- `bgmedia_sdlgpu` — единственный слой, знающий SDL_GPU; принимает
  `RetargetedPose` и возвращает RGBA8-кадр.
- `apps/desktop` — композитинг 3D-кадра в существующий SDL_Renderer UI.

### 2.1 Почему offscreen, а не общий GPU-контекст

Основной UI BG Media построен на `SDL_Renderer`. Одновременное владение
одним окном и `SDL_Renderer`, и `SDL_GPU` не поддерживается. Поэтому
`bgmedia_sdlgpu`:

1. создаёт **отдельный** `SDL_GPUDevice` без claim окна;
2. рендерит сцену в offscreen `R8G8B8A8_UNORM_SRGB` + `D32_FLOAT`;
3. копирует результат в download transfer buffer и ждёт fence;
4. обновляет обычный `SDL_Texture` в SDL_Renderer и рисует его в UI.

Это документированный путь SDL, не sharing контекста. Цена — readback
(измерено ~0.3–0.5 мс на 960×640, D3D12). Если GPU/драйвер недоступен,
Desktop остаётся работоспособным в 2D-режиме и показывает диагностику импорта.

### 2.2 Рендерер

- SDL_GPU (D3D12 на Windows; API-код переносим на Vulkan/Metal).
- HLSL → DXIL, встроенные заголовки в `adapters/sdlgpu/shaders/generated`.
  Исходники лежат в `adapters/sdlgpu/shaders/*.hlsl`, пересборка:
  `scripts/compile_shaders.ps1` (нужен `dxc.exe` из Windows SDK).
- Два прохода в одном render pass:
  - полноэкранный градиент;
  - пол с сеткой и контактной тенью;
  - opaque/mask-меши (depth test + depth write);
  - alpha-blend меши (depth test, без depth write, back-to-front сортировка).
- Skinning выполняется на GPU: `jointMatrix[i] = model * nodeGlobal[joint] *
  inverseBind[i]`; до 192 матриц в push-constant uniform.
- Текстуры — RGBA8 sRGB с mip-chain (`SDL_GenerateMipmapsForGPUTexture`).
- Материалы: базовая текстура, baseColorFactor, alpha mask/blend,
  двусторонность, простое MToon-подобное освещение + rim.
- Double-sided culling отключён для мешей: VRM 0.x экспортёры иногда имеют
  неоднозначный winding, а 35 881 треугольник для персонажа — не проблема.
- Слой глаз (iris/highlight/eyeline/brow/lash) рисуется без обратной стороны и
  с depth-independent blend, потому что VRM/MToon кладёт эти плоскости почти
  coplanar поверх eye-white. Это исправляет «закрытые глаза» на baked-моделях.

## 3. Импорт VRM 0.x

Реализация: `src/vrm/VrmLoader.cpp`, `include/bgmedia/vrm/VrmModel.hpp`.

Поддерживается:

- GLB-контейнер: JSON + BIN chunks, проверка magic/version/length;
- nodes/scene hierarchy, TRS и matrix transforms, parent/child, цикл-детект;
- mesh primitives (TRIANGLES), POSITION/NORMAL/TEXCOORD_0/JOINTS_0/WEIGHTS_0;
- индексы u8/u16/u32, отсутствие индексов, отсутствие нормалей (пересчёт);
- skins, joints, inverseBindMatrices;
- 27 embedded PNG (декодируются только используемые base color maps);
- materials: alphaMode, alphaCutoff, doubleSided, KHR_materials_unlit,
  baseColorTexture, KHR_texture_transform offset/scale;
- VRM 0.x `extensions.VRM`: meta, humanoid.humanBones, materialProperties
  (`VRM/MToon`, `_ShadeColor`, `_Cutoff`, `_CullMode`);
- humanoid-маппинг 54 костей; 24 из них входят в стабильное ядро retargeter.

Проверки безопасности:

- лимиты файла, узлов, вершин, индексов, текстур и изображений;
- проверка accessor ranges и stride до любого чтения;
- запрет sparse accessors (для требуемых атрибутов);
- проверка индексов, joint-индексов и inverse bind;
- проверка конечности всех позиций/матриц;
- повреждённый/truncated GLB возвращает `LoadResult::error`, без исключений и UB;
- 150 случайных мутаций тестового VRM в тестах не падают.

Фактические данные `Model1.vrm`:

```text
GLB 18 539 292 bytes, 175 nodes, 3 meshes, 35 881 triangles,
15 materials, 27 textures (15 base-color decoded), 3 skins × 171 joints,
54 humanoid bindings, height ≈ 1.623 m, forward = -Z.
```

### 3.1 Что не поддерживается в v1

- VRM 1.0 (`VRMC_vrm`) — выдаётся понятная ошибка;
- morph targets / blend shape groups (лицо берётся в базовой позе; blink,
  lip sync и expressions не анимируются);
- secondary animation / spring bones (физика волос и юбки);
- MToon outline, rim/sphere/matcap, UV animation, normal maps;
- внешние buffer/image URI (VRM должен быть самодостаточным GLB);
- несколько материалов-слоёв с точным MToon renderQueue — используется
  упрощённый порядок opaque → blend.

Это осознанные ограничения первой версии; архитектура оставляет для них
места в импортёре и шейдерах.

## 4. Humanoid rig и BG Motion retargeting

`HumanoidRetargeter` (`src/animation/HumanoidRetargeter.cpp`) не содержит
индексов узлов Model1.vrm и не предполагает одинаковые пропорции.

На этапе конструктора:

- читаются bind local/global transforms и bind global rotations;
- вычисляются длины бёдер/голеней и позиции лодыжек;
- находятся hips, spine/chest/upperChest, neck/head, shoulders, arms, hands,
  legs, feet/toes;
- если обязательных костей нет, rig возвращает понятную ошибку.

На каждый кадр:

- neutral base pose опускает руки из T-pose и слегка сгибает колени;
- `bob`, `hipShift` переходят в translation таза в parent space;
- `lean`, `chestTwist`, `headNod`, `headTilt`, `headTurn` — в повороты
  вокруг анатомических осей (forward/up/right), выраженные в bind-пространстве
  кости через conjugation;
- `armLeft/armRight`, `elbowLeft/elbowRight` — в направление плеча и
  локтевой изгиб; руки не проходят через T-pose между движениями;
- ноги решаются two-bone IK в character space: стопа фиксируется по XZ,
  колено направлено вперёд, `footLeft/footRight` поднимают цель;
- стопа сохраняет горизонтальную ориентацию; ошибка IK проверяется тестом;
- `rotation` применяется как отдельный yaw всего персонажа, а не к тазу,
  чтобы не ломать IK;
- idle-режим добавляет очень маленькое дыхание, но `Freeze` остаётся freeze;
- результат — `nodeGlobals` и `modelTransform`; renderer строит skin palette.

Ключевые свойства:

- никаких world-space «на глаз» трансформаций;
- quaternion short-arc, нормализация, защита от NaN;
- детерминизм: одинаковые `(pose, mediaTime, options)` дают одинаковые матрицы
  бит-в-бит, независимо от порядка вызовов и FPS;
- seek/rewind воспроизводят ту же позу;
- нет привязки к FPS или wall-clock.

Маппинг BG Motion → rig:

| CharacterPose | Куда идёт |
|---|---|
| bob | вертикальное смещение hips |
| lean | наклон hips/spine/chest/upperChest |
| chestTwist | скручивание корпуса |
| hipShift | боковое смещение таза |
| armLeft/armRight | направление верхних рук |
| elbowLeft/elbowRight | локтевой изгиб |
| legLeft/legRight | амплитуда/фаза ног (IK сохраняет стопы) |
| kneeLeft/kneeRight | дополнительное опускание таза → сгиб коленей |
| footLeft/footRight | подъём цели стопы |
| headNod/headTilt/headTurn | neck + head |
| rotation | yaw всего персонажа |
| energy/impact/accented | визуальное состояние UI/сцены, не ломает скелет |

## 5. Синхронизация с музыкой

Существующая цепочка не изменена:

```text
Audio decode → Analyzer/BG Beat → ChoreographyBuilder/BG Motion
    → ChoreographySampler.sample(mediaSeconds) → CharacterPose
    → HumanoidRetargeter.apply(pose, mediaSeconds)
    → SDL_GPU renderer
```

- `PlaybackClock` компенсирует output latency.
- В 3D-режиме media time берётся из того же `currentPosition()`, что и в 2D.
- `pause` останавливает media time и поза замирает.
- `seek` сдвигает clock/позицию; следующий кадр сэмплирует точную позу.
- `rate` меняет media time через существующий плеер (pitch follows speed).
- Synthetic Motion Lab / Test Dance использует отдельную synthetic beat map
  только когда трек не загружен; это не второй плеер.

## 6. Desktop UI

- Переключатель `2D` / `3D` в toolbar, клавиши `2` и `3`, `G` — toggle.
- `LOAD` / `L` — аудио; `VRM` / `F2` — выбрать `.vrm`/`.glb`; drag-and-drop
  понимает и аудио, и VRM.
- 3D-сцена:
  - левая кнопка мыши — орбита;
  - колесо — zoom;
  - `C` / RESET CAM — сброс;
  - `-`/`+` — масштаб модели;
  - `,`/`.` — вертикальное положение модели;
  - `T` / TEST DANCE — синтетический танец 123 BPM без музыки;
  - `Space` — play/pause (в demo — play/pause синтетического танца);
  `--autoplay` запускает воспроизведение сразу после загрузки трека.
- Панели справа: импорт/rig, BG Sync/Motion, SDL_GPU renderer, controls.
- Видны: модель, сцена, текущий трек/синтетика, время, play/pause, rate,
  текущее движение, bar/beat, accent, foot IK, retarget ms, FPS, GPU ms,
  readback ms, VRAM, RSS, лицензия и предупреждения импорта.
- Если трек не загружен — idle preview и TEST DANCE.
- Ошибки импорта/GPU показываются в 3D-панели; 2D-режим не ломается.

## 7. Сборка и запуск

```powershell
powershell -ExecutionPolicy Bypass -File scripts/build_windows.ps1
ctest --test-dir build\windows --output-on-failure
build\windows\bin\bgmedia_desktop.exe
```

3D-демо:

```powershell
# локальная модель + синтетический танец, без музыки
build\windows\bin\bgmedia_desktop.exe --vrm models\Model1.vrm --vrm-demo

# музыка + модель; --autoplay сразу запускает воспроизведение
build\windows\bin\bgmedia_desktop.exe --file track.mp3 --vrm models\Model1.vrm --3d --autoplay
```

Headless 3D preview (та же цепочка importer/retargeter/renderer):

```powershell
build\windows\bin\bgmedia_vrm_preview.exe --model models\Model1.vrm ^
  --time 3.2 --out artifacts\preview.bmp
```

`bgmedia_vrm_preview` полезен для проверки рендера без UI; он не подменяет
Desktop-приложение и не используется для финального скриншота.

### 7.1 Шейдеры

```powershell
powershell -ExecutionPolicy Bypass -File scripts/compile_shaders.ps1
```

Скомпилированные DXIL-заголовки уже лежат в репозитории, поэтому обычная
сборка не требует `dxc.exe`. Для Linux/Metal в будущем нужно добавить
SPIR-V/MSL-варианты (исходный HLSL уже отделён от API-кода).

## 8. Тесты и измерения

`ctest --test-dir build\windows --output-on-failure`:

```text
1/8 audio          Passed
2/8 analysis       Passed
3/8 cache          Passed
4/8 sync           Passed
5/8 choreography   Passed
6/8 motion         Passed
7/8 vrm            Passed
8/8 retarget       Passed
100% tests passed
```

Новые тесты:

- `vrm.minimal_fixture_import` — собственный минимальный VRM 0.x fixture;
- `vrm.model1_if_available` — реальные counts/bones/текстуры/размеры
  (пропускается, если приватной модели нет);
- `vrm.rejects_malformed_containers` — truncation, bad magic/version/length,
  noise;
- `vrm.random_mutations_never_crash` — 150 мутаций;
- `retarget.neutral_pose_is_not_tpose` — руки не в T-pose;
- `retarget.bounce_and_bassdip_keep_feet_planted` — XZ стоп стабильны,
  maxFootError < 5 мм;
- `retarget.arms_head_and_torso_are_articulated`;
- `retarget.is_deterministic_and_frame_rate_independent` — бит-в-бит
  воспроизведение после seek/rewind и 30-секундный sweep;
- `retarget.model1_if_available` — реальный риг.

ASan (MSVC):

```powershell
powershell -ExecutionPolicy Bypass -File scripts/run_asan_windows.ps1
```

Результат: **56 passed, 0 failed**, без ASan-сообщений. Debug/ASan медленнее,
поэтому импорт Model1 там ~1.7 с вместо ~0.18 с Release.

Измерения (Release, Ryzen 7 5700X, Windows 11, D3D12, 960×640 offscreen):

| Метрика | Значение |
|---|---:|
| импорт Model1.vrm (включая 15 PNG) | ~176–179 мс |
| retarget одного кадра | ~0.02 мс |
| GPU frame (35 881 tris, 15 draws) | ~0.6–0.8 мс |
| readback 960×640 | ~0.3–0.5 мс |
| UI FPS (реальный запуск, vsync/8ms loop) | ~125–132 FPS |
| GPU textures (mip chain) | ~112 MB |
| process RSS во время 3D | ~160–168 MB |
| desktop capture | 1440×900 BMP |

GPU FPS выше 60, потому что desktop loop имеет собственный 8 мс pacing, а
SDL_Renderer backend на этой машине не ограничивает строго 60. Это реальные
значения, а не «целевые».

## 9. Правила использования модели

`Model1.vrm` и его текстуры **не коммитятся**. `.gitignore` исключает
`/models/*` (кроме README) и `/artifacts/`. Скриншот
`artifacts/desktop3d_playing.png` тоже локальный. Для публикации нужен
отдельный VRM с совместимой лицензией.

## 10. Следующие шаги

1. Morph targets / VRM expressions (blink, lip sync).
2. SpringBone secondary animation.
3. MToon outline + sphere/rim maps, нормал-мапы.
4. Vulkan/Metal shader variants (SPIR-V/MSL) и Linux/iOS проверка.
5. Более точный порядок MToon renderQueue и per-material cull/ZWrite.
6. Android-интеграция — отдельный этап, вне текущей задачи.
