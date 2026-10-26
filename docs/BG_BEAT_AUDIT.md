# BG Beat: исправление метрического уровня и честной неопределённости

Этот документ фиксирует работу по критическим замечаниям независимого аудита
HEAD `540dd41`. Исходные аудио-стимулы и реальные треки **не добавлены в Git**.

## 1. Первопричина

Проблема 12/8 → 198.7 BPM была не одной ошибкой, а комбинацией четырёх
механизмов:

1. **Плотность grid score.** `coverage` и `onsetHitRate` монотонно растут с
   числом предсказанных ударов. Сетка 198 BPM имеет втрое больше шансов
   коснуться onset, чем пульс 66 BPM.
2. **Неполное семейство уровней.** Кандидаты строились только как `×0.5`,
   `×2`, `×2/3`, `×1.5`; отношение `3:1` вообще не генерировалось.
3. **Broadband phase.** DP и phase search могли сесть на hi-hat/offbeat bass;
   low-frequency pulse не участвовал в выборе фазы.
4. **Downbeat по индексу.** `i % meter` сдвигал всю разметку после пропуска
   удара, а глобальный STFT timestamp систематически опаздывал/спешил.

Ключевая формула density normalization:

```text
chance = 2 * toleranceFrames * onsetPeakCount / frameCount
precisionAdjusted = clamp((beatHitRate - chance) / (1 - chance), 0..1)
```

Recall/coverage в выбор уровня не входят; pulse evidence считается по
отфильтрованному low-frequency envelope:

```text
pulse = 0.70 * lowHitRate + 0.30 * lowOnOffContrast
score = (0.55 * (0.35 * broadband + 0.65 * pulse) + 0.45 * acScore) * prior
```

`acScore` — autocorrelation поддержка выбранного lag; `prior` слабый,
не фиксирует конкретный BPM.

## 2. Что изменено

- Кандидаты теперь включают `1/3, 1/2, 2/3, 3/4, 1, 4/3, 3/2, 2, 3` от
  autocorrelation-пиков.
- В выборе уровня больше не используются `coverage`/`onsetHitRate`; precision
  нормируется на вероятность случайного совпадения, а уровень оценивается по
  low-band pulse evidence (hit rate, on/off contrast) и autocorrelation.
- `lowEnergyRatio` фильтрует низкочастотный pulse envelope от утечки
  клавиш/снейра; это отдельная метрическая улика, а не замена broadband onset.
- Субгармоника поддерживается только при наличии pulse evidence у более
  медленного уровня. Это не фиксированный BPM-prior.
- DP beat tracker использует `broadband onset + low pulse`; phase rescue
  применяется только если текущая сетка почти лишена low-band поддержки
  (`basePulse < 0.15`), поэтому syncopated bass не уводит beat на offbeat.
- Downbeat phase считается по накопленному **grid index** с учётом фактического
  времени/периода, а не по индексу массива. Пропущенный удар больше не сдвигает
  все последующие такты.
- Для onset/beat timestamp используется середина нового аудиоинтервала
  STFT-кадра. Это устраняет систематическое опережение примерно на 13.5 мс;
  константа не добавлялась.
- Confidence разделён:
  - `bpmConfidence` — периодичность/BPM (эвристика);
  - `bpmLevelConfidence` — выбранный метрический уровень;
  - `beatPhaseConfidence` — качество beat grid;
  - `downbeatConfidence` / `meterConfidence` — фаза такта.
  Это scores, а не вероятности; калибровка на большом корпусе не заявляется.
- `octaveAmbiguous` расширен до метрической неоднозначности `2:1, 3:1, 3:2,
  4:3`; UI говорит `METRIC AMBIGUOUS`, а не `STABLE`.
- При низкой `meterConfidence` UI не рисует downbeats/bar labels как факт и
  пишет `meter uncertain`.
- Bass accents: `nearestBeat` выставляется только в local-period окне
  (≤28% IBI), а Choreography больше не пере-привязывает offbeat-бас к
  ближайшему beat. BassDip guard проверен тестом на 198.5 BPM.
- `kAnalyzerVersion`, schema и cache format подняты до **2**. Старые кеши
  становятся miss и не могут вернуть доминирующий 198.7 BPM.

## 3. Сравнение до/после на аудиторском корпусе

F-measure и downbeat F считаются с допуском 70 мс. `bias` — средняя ошибка
времени совпавших ударов. Старые значения получены на `540dd41`; новые — на
текущем HEAD.

| Стимул | Truth BPM | Старый BPM | Новый BPM | Старый F | Новый F | Старый downbeat F | Новый downbeat F | Старый bias, мс | Новый bias, мс |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| ballad80_q | 80.00 | 80.09 | 80.09 | .999 | 1.000 | 1.000 | 1.000 | −13.5 | +3.9 |
| ballad80_drift | 80.82 | 80.86 | 80.86 | 1.000 | 1.000 | 1.000 | 1.000 | −13.5 | +3.9 |
| house125 | 125.00 | 125.10 | 125.10 | .999 | .999 | .000 | 1.000 | −13.5 | +3.9 |
| pop99_dense | 99.00 | 99.12 | 99.12 | .997 | .997 | 1.000 | 1.000 | −13.5 | +3.9 |
| swing80_drift | 80.98 | 80.98 | 80.98 | .999 | .999 | .000 | 1.000 | −13.4 | +4.0 |
| offbeat100 | 100.00 | 199.61 | **99.81** | .664 | **1.000** | .000 | **1.000** | −13.0 | +4.4 |
| halftime80_drift | 80.69 | 160.26 | **80.82** | .669 | **1.000** | .017 | **1.000** | −13.5 | +3.9 |
| ternary66_q | 66.17 | 198.71 | **66.24** | .499 | **.999** | .399 | **.995** | −13.5 | +4.0 |
| ternary66_sparse | 66.27 | 199.01 | **66.34** | .500 | **1.000** | .403 | **1.000** | −13.2 | +4.3 |
| ternary66_drift | 67.15 | 67.17 | 67.10 | .999 | .999 | 1.000 | 1.000 | −13.4 | +4.0 |

Ключевые случаи: offbeat и half-time больше не уходят в удвоение, 12/8
возвращается к пульсу, а downbeat-фаза перестаёт быть систематически сдвинутой
на 4-ю долю. `ballad80_triplet` (80 BPM с триолями) также остаётся на 79.95 и
F 0.997.

## 4. Реальная музыка

Локально вне репозитория были доступны два MP3. Они не копировались в Git.
Эталонный BPM для первого трека неизвестен, поэтому таблица — наблюдение, а
не доказательство корректности.

| Трек | Старый результат | Новый результат | Комментарий |
|---|---|---|---|
| «Ты уже спасён» | 198.5 BPM, conf 37%, stable, 1120 beats | **121.97 BPM**, conf 35%, level 25%, metric ambiguous, 687 beats | Истина неизвестна. В альтернативах остаются 194.57 (0.95) и 66.16 (0.94); нельзя утверждать, что 121.97 — музыкальный пульс. |
| Kai Angel — Andy Warhol | 125.0 BPM, conf ~49%, правильный по референсу | **124.98 BPM**, conf 57%, level 27%, 369 beats | BPM подтверждён. Meter 3/4 с confidence 34% — UI скрывает downbeats. |

Первый трек больше не выдаёт 198.5 как уверенный факт: уровень и downbeat
confidence низкие, альтернативы видны. Это честнее, но **не** доказывает, что
новый основной кандидат 121.97 музыкально верен.

## 5. Regression corpus

Добавлены воспроизводимые генераторы и тесты:

- straight 80, 92, 100, 123, 125, 163 BPM;
- offbeat 100 BPM;
- ternary 66.2 BPM (12/8, tatum ×3);
- half-time 80 BPM;
- shuffle 80 BPM;
- dense hats 99 BPM;
- syncopated bass 100 BPM;
- drift 80 BPM;
- 92 → 123 BPM;
- silence, bass vs broadband swell, ambiguous 92/138.

`bgmedia_fixturegen` пишет соответствующие WAV в `fixtures/`. Проверяются BPM,
one-to-one beat F1, downbeat F1, signed timing bias, `nearestBeat` для
syncopated bass и cache invalidation. Всего в проекте **82 теста**, 9
CTest-групп (включая 26 кейсов BG Motion 3.0).

## 6. Что осталось открытым

- Confidence не откалиброван на большом размеченном корпусе. Reliability
  diagram и вероятностная калибровка — отдельная задача.
- Downbeat остаётся эвристикой. На равномерном 4-on-the-floor без басовой
  подсказки фаза 1/3 объективно неоднозначна; используется слабый first-beat
  tie-break, а `meterConfidence` остаётся низкой. UI не выдаёт её за факт.
- B-06 (компрессия spectral flux) и B-07 (полное согласование tempo/phase
  envelopes) не переписаны: текущий эффект не был подтверждён на реальном
  корпусе.
- B-09 (наклон regression) и часть M-02/M-03/M-04 относятся к следующему
  этапу BG Motion, не к BG Beat.
- Реальные MP3 с эталонной разметкой отсутствуют; выводы по реальной музыке
  ограничены двумя локальными файлами и не являются валидацией.
- S-01/S-02/S-03/D-02 (latency API, decode в callback, потоки) не входили в
  этот этап.

## 7. Воспроизведение

```powershell
powershell -ExecutionPolicy Bypass -File scripts\build_windows.ps1
build\windows\bin\bgmedia_tests.exe analysis
build\windows\bin\bgmedia_tests.exe cache
build\windows\bin\bgmedia_fixturegen.exe build\windows\fixtures
```

Старые кеши после обновления должны показывать `cache: miss`; это ожидаемо и
является частью исправления, а не регрессией.
