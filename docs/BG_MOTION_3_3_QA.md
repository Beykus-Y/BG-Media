# BG Motion 3.3: QA matrix and ground drag metric

Scope: issues #13 (30 FPS ground drag) and #14 (cross-model QA matrix).

## Why the 30 FPS number moved

The ground drag figure was summed per sample. A sample counted as drag when its
chord speed from the previous sample exceeded 0.15 m/s. The chord is the average
speed over one frame, so the gate depends on the frame rate:

- On an analytic ankle track with speed `0.10 + 0.10 sin(2 pi t)` m/s (gate open
  where the speed exceeds 0.15 m/s, truth 18.27 cm), the per-sample formula gives
  16.73 cm at 30 FPS (-8.4 %) and 18.27 cm at 60 FPS and above.
- On the synthetic `minimal.vrm` rig, the same pose reports 0.34 cm (left) and
  0.32 cm (right) of drag at 60 FPS for `sidestep` with the reference replay at
  0.00 cm. The pose is a function of media time (seek error 0.000 mm), so the
  difference comes from the metric, not from playback.

The metric now integrates on a fixed 240 Hz reference grid (`qa::measureGroundDrag`
in `tests/motion_metrics.hpp`):

- ankle positions between samples use uniform Catmull-Rom interpolation (a straight
  line turns every sample into a constant-speed chord and biases the gate at low
  rates);
- the speed gate is a central difference over +-2 grid steps (about 8 ms);
- drag distance and time are integrated over grid steps, not per sample.

`qa::measureGroundDragPerSample` keeps the old formula so reports can show both.
`bgmedia_motion_diag` prints both numbers.

Not yet shown: whether the authored Side Step's 5.9 cm at 30 FPS on the real Model1
and Nemesis is metric-only or includes a real floor violation. That needs the private
models and `bgmedia_motion_diag`/`bgmedia_qa_matrix` run against them.

## Tools

- `bgmedia_qa_matrix` (`tools/qa_matrix/main.cpp`): replays each clip at 240 Hz
  (truth) and at the scenario fps, for overlay off/full and spring off/on. It reports
  ground drag from both replays, the per-sample legacy figure, planted-run slide,
  worst IK error and seek error (a fresh player jumping to six sampled times against
  the sequential replay, spring off only).

  ```
  bgmedia_qa_matrix --model tests/fixtures/minimal.vrm --json docs/qa_matrix_synthetic.json
  bgmedia_qa_matrix --model models/Model1.vrm --model models/Nemesis.vrm --json <dir>/matrix.json
  ```

  Baseline on the synthetic rig: `docs/qa_matrix_synthetic.json`.

- `bgmedia_motion_diag`: unchanged, now prints the reference-grid drag and the
  per-sample figure. It still needs a private model and track for mesh crossings.

## Not covered yet

- Skinned-mesh triangle intersections (hand/forearm/torso, thigh/thigh, sleeve/vest,
  hair/arms): the synthetic fixture has a single triangle. Use `bgmedia_motion_diag`
  with the private model until a mesh-level pass exists.
- Acceleration and jerk peaks at the reference rate.
- Scenario regression budgets. The matrix measures; the tests judge.

## Tests (CI, synthetic only)

- `quality.ground_drag_does_not_depend_on_the_sampling_rate`: the analytic track above
  at 30, 60, 120 and 240 Hz must each agree with the 10 kHz truth within 3 %. The
  per-sample formula misses this by 8.4 % at 30 Hz.
- `quality.ground_drag_ignores_planted_and_raised_feet`.
