# VRM 1.0 рядом с VRM 0.x

Загрузчик (`VrmLoader`) сам определяет версию файла и приводит VRM 1.0 к тем же
соглашениям, на которых построены ретаргетер, клипы, пружины, камера и
инструменты. Ни один слой выше `bgmedia_vrm` не ветвится по версии; модели 0.x
(Model1, Nemesis) идут по прежнему пути без изменений.

Код: `src/vrm/VrmLoader.cpp` (ядро glTF и выбор версии), `src/vrm/VrmSchema.cpp`
(чтение схем 0.x/1.0), `src/vrm/VrmJson.hpp` (JSON DOM, общий для обоих),
`src/animation/NodeConstraints.cpp` (рантайм `VRMC_node_constraint`).
Тесты: `tests/test_vrm1.cpp` (`bgmedia_tests vrm1.`).

## Определение версии

| В файле                         | `VrmModel::version` | `specVersion`          |
|---------------------------------|---------------------|------------------------|
| `extensions.VRMC_vrm`           | `V1`                | из `VRMC_vrm.specVersion` (по умолчанию `1.0`) |
| `extensions.VRM`                | `V0`                | из `VRM.specVersion` (по умолчанию `0.x`)      |
| оба                             | `V1` + предупреждение |                      |
| ни одного                       | ошибка загрузки     |                        |

## Что адаптируется

| Аспект | VRM 0.x | VRM 1.0 | Как сведено |
|---|---|---|---|
| Ориентация | смотрит в −Z | смотрит в +Z | корни сцены 1.0 поворачиваются на 180° вокруг +Y (`VrmModel::sourceToModel`). Дочерние узлы, inverse bind matrices и вершины не трогаются: скиннинг (`joint global × IBM`) получает поворот через суставы. Камера (`yaw = π` = фронт), клипы и инструменты работают как с 0.x. |
| Rest-позы костей | UniVRM нормализует (identity) | могут быть любыми | ретаргетер и так считает дельты в мировых осях от bind-поворотов; тест сравнивает 1.0 с поворотами у **каждого** узла против 0.x — расхождение < 0.001 мм. |
| Humanoid | `humanBones[]` | `humanBones{}` | имена 1.0 приводятся к 0.x: `ThumbMetacarpal/Proximal/Distal` → `ThumbProximal/Intermediate/Distal` (те же кости). Исходное имя — в `HumanoidEntry::sourceName`. |
| Expressions | `blendShapeMaster` (веса 0..100, пресеты `joy/sorrow/fun/a/i/u/e/o/blink_l…`) | `expressions.preset/custom` (веса 0..1, привязка к узлу) | общий `Expression`: пресеты 1.0, веса 0..1, morph/material color/texture transform binds, `isBinary`, override blink/lookAt/mouth. `_MainTex_ST` 0.x переводится в смещение с началом UV сверху-слева. Невалидные binds отбрасываются с предупреждением. |
| LookAt | `firstPerson.lookAt*` (`xRange/yRange` + кривая) | `lookAt.rangeMap*` (`inputMaxValue/outputScale`) | общий `LookAt`; кривая 0.x сводится к концам. `offsetFromHeadBone` переводится в оси движка (0.x: Z из осей Unity; 1.0: через bind-поворот головы, т.е. её rest-ориентацию и поворот сцены). |
| First person | `meshAnnotations` по мешам | по узлам | `FirstPersonAnnotation` по узлам. |
| Spring bone | `secondaryAnimation` | `VRMC_springBone` | те же цепочки `SpringBoneChain`. Коллайдер-капсула → ряд сфер с шагом ≤ радиуса (до 128 сфер, дальше предупреждение); группы коллайдеров 1.0 (на нескольких узлах) раскладываются по узлам. Параметры сегментов (последний сустав — хвост) усредняются на цепочку. Суставы могут пропускать промежуточные узлы (по спеке): они не симулируются, а жёстко едут за симулированным предком; так же едут дочерние узлы ниже последнего сустава. Пружина, начинающаяся на чужом суставе, становится веткой с якорем; пружина, начинающаяся ниже чужого сустава, крепится к нему (`attached`). `gravityDir` поворачивается вместе со сценой. |
| Node constraints | — | `VRMC_node_constraint` (roll / aim / rotation) | `NodeConstraintSolver` выполняется ретаргетером после позы и до пружин (порядок эталонных рантаймов). Зависимости сортируются; циклы отбрасываются. Для 0.x — пустой и ничего не делает. |
| MToon | `materialProperties` (`VRM/MToon`) | `VRMC_materials_mtoon` на материале | `Material::toon`, `shader`, `shadeColor` (`shadeColorFactor`). `KHR_materials_emissive_strength` умножает emissive. |
| Meta | `title, author, licenseName, allowedUserName, …Ussage…` | `name, authors[], licenseUrl, avatarPermission, allowRedistribution, …` | 1.0 заполняет и поля 0.x (ближайшие эквиваленты), и свои. `Metadata::restrictsRedistribution()` — общая проверка для пометки «PRIVATE / LOCAL». |

## Ограничения

- Морф-таргеты не загружаются в меш и не рендерятся: expressions сейчас — это
  данные (для будущего лица/моргания), а не анимация. То же для lookAt.
- Пружины 1.0 задают параметры на сустав; рантайм пока симулирует один набор
  на цепочку (среднее по сегментам). Экспортёры, включая миграцию UniVRM из
  0.x, пишут их одинаковыми.
- `shadeColorFactor` 1.0 линейный; `_ShadeColor` 0.x рендерер исторически
  тоже трактует как линейный, поэтому одна и та же модель в 0.x и 1.0 может
  чуть отличаться по оттенку тени.
- Реальных VRM 1.0 моделей в репозитории нет; проверка идёт на синтетическом
  двойнике `minimal.vrm`. Перед включением в продакшн стоит прогнать
  `bgmedia_vrm_preview` / `motion_diag` на настоящем VRM 1.0 (например, после
  конвертации Model1 в UniVRM 1.x).
