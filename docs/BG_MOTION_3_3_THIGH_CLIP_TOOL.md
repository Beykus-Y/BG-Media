# BG Motion 3.3 — Thigh-clip measurement (issue #12, tooling only)

Files: `tools/support/TriangleTest.hpp` (triangle-triangle contact test),
`tools/mesh_clip/LegMode.hpp` (the `--legs` mode), `tools/mesh_clip/main.cpp` (options),
`tests/test_triangle.cpp` (unit tests, suite `bgmedia.triangle`).

**Scope honesty.** This is measurement only. It does not change any clip, retargeter
rule, footwork pass or collision proxy. The fix for issue #12 needs the private model
`Model1.vrm` and the Nemesis model, which are not in this repository or in the build
environment. The owner runs the commands below locally and sends the results back.
The tool was checked on the repository fixture and on a synthetic skinned scratch model
(not committed). It has not been run on `Model1.vrm` by the author.

## What it measures

For the authored Hip-Hop (`hiphop`) and Side Step (`sidestep`) clips, per frame:

- **Thigh contact.** Each skinned triangle whose three vertices have the humanoid
  `leftUpperLeg` (or `rightUpperLeg`) as dominant joint (weight >= 0.5) is a LEFT or
  RIGHT thigh triangle. Every left triangle is tested against the right ones (CPU
  skinning, same convention as the arm test). Pairs sharing a vertex are skipped.
  Pairs already touching in the neutral (arms-out) pose are masked for the whole run.
  A **crossing** is an unmasked touching pair with depth above `--crossing-mm`
  (default 0.5 mm).
- **Per frame:** crossing count, masked-contact count, max and mean depth, the 5 deepest
  pairs. Each pair gives the leg-triangle indices, mesh and primitive numbers, the
  primitive triangle number, the material name and the midpoint of the two centroids.
- **Knees and bend planes.** Knee = `leftLowerLeg` / `rightLowerLeg` joint, hip =
  upper leg, ankle = foot. Knee flexion is 0 for a straight leg. The bend-plane angle is
  the angle between the hip-knee-ankle plane normal and the model's lateral axis: 0 when
  the knee bends in the sagittal plane, 90 when it bends sideways. Null for a straight leg.
- **Pelvis.** Height of the hips above the lower ankle joint (metres, and relative to
  the model height). Yaw of the hip line against the neutral pose (degrees). Root yaw
  from the clip.
- **Feet.** Contact weight (0 swing, 1 planted) and phase per foot, from the
  DancePlayer frame.
- **Thigh proxy.** The retargeter's fitted thigh radius (and shin radius) is reported
  once in the header. It is reported as is, not compared with the mesh.

Footwork follows the desktop app: the authored foot tracks are re-planned for the model
(`setLegGeometry`). `--raw-footwork` measures the authored tracks as stored.
Overlays are on, as in the app (`--overlay 0` turns them off).

## Command for the owner (Nemesis and Model1)

From the repository root, with both private models under `models/` (untracked). The
Nemesis file name below is a placeholder: use the local file name. Nemesis is the model
issue #12 is about; Model1 is the regression check (its Hip-Hop must stay at 0 crossings).

Linux:

```sh
cmake -S . -B build/ci -G Ninja -DCMAKE_BUILD_TYPE=Release -DBGMEDIA_BUILD_DESKTOP=OFF \
      -DBGMEDIA_BUILD_TESTS=ON -DBGMEDIA_BUILD_TOOLS=ON -DBGMEDIA_BUILD_MINIAUDIO=ON
cmake --build build/ci --parallel --target bgmedia_mesh_clip
mkdir -p out/issue12
for m in Nemesis Model1; do
  for fps in 30 60 120; do
    ./build/ci/bin/bgmedia_mesh_clip --legs --model models/$m.vrm \
      --clips hiphop,sidestep --fps $fps --seconds 40 \
      --json out/issue12/${m}_legs_${fps}.json | tee out/issue12/${m}_legs_${fps}.txt
  done
done
```

Windows (PowerShell, build tree `build\windows` as in the README):

```powershell
cmake --build build\windows --parallel --target bgmedia_mesh_clip
New-Item -ItemType Directory -Force out\issue12 | Out-Null
foreach ($m in 'Nemesis', 'Model1') {
  foreach ($fps in 30, 60, 120) {
    build\windows\bin\bgmedia_mesh_clip.exe --legs --model models\$m.vrm `
      --clips hiphop,sidestep --fps $fps --seconds 40 `
      --json out\issue12\${m}_legs_$fps.json | Tee-Object out\issue12\${m}_legs_$fps.txt
  }
}
```

Optional, for comparison with the authored foot tracks: add `--raw-footwork` and write
`legs_raw_${fps}.json` / `.txt`.

Options that matter: `--top N` (worst frames listed per clip, default 10),
`--crossing-mm X` (crossing threshold, default 0.5), `--seconds` (default 40 in this mode),
`--fps` (same as `--hz`; default 60 in `--legs` mode). `--json` is only accepted with `--legs`.

## What to send back

1. The six console reports `out/issue12/<model>_legs_<fps>.txt`.
2. The six JSON files `out/issue12/<model>_legs_<fps>.json`.
3. The commit the tool was built from: `git rev-parse --short HEAD`.

The JSON holds no mesh geometry, but its numbers are measured from the private models, and
Model1's licence is Redistribution_Prohibited. Check the licence before sending the files
anywhere outside the owner's own use.

## JSON layout

- Header: `model`, `fps`, `seconds`, `footwork` (`planned` or `raw`), `crossingMm`,
  `legTriangles` (left and right counts), `maskedRestPairs`, `thighProxyRadiusM`,
  `shinProxyRadiusM`, `units`.
- `clips[]`: `clip`, `name`, `frames`, `summary` (`framesWithCrossings`, `maxCrossings`,
  `totalCrossings`, `maxDepthMm`, `meanDepthMm`, `worstFrames` with frame, time and the
  deepest pair), and `rows[]`, one per frame: `t`, `crossings`, `masked`, `maxDepthMm`,
  `meanDepthMm`, `pairs`, `knees` (metres, model space, +y up), `kneeFlexionDeg`,
  `bendPlaneDeg`, `pelvis`, `feet`.
- In a pair, `leftLegTri` and `rightLegTri` index the run's thigh-triangle lists. The
  `leftPrimTriangle` / `rightPrimTriangle` fields give the triangle number inside the
  primitive's index buffer, which is the stable identity in the model file.

## Depth and limits

- **Triangle test** (`TriangleTest.hpp`): Moller 1997 interval test, closed (touching
  counts), with a 1 micrometre plane tolerance. Zero-area triangles never intersect.
  It was cross-checked against a separating-axis reference on 2,000,000 random pairs
  (no mismatches, symmetric results).
- **Depth** is approximate. For each face normal, it is the smaller of the two
  translations along that normal that put the other triangle on one side of the face.
  The pair depth is the smaller of the two face-normal values. It is an upper bound of the
  minimum separating translation. For a flat face pushed straight through another face it
  is exact. Large triangles that cross edge-on can report large depths, so read the
  depth together with the pair position and the triangle size.
- **Masking** hides a thigh pair that already touches in the neutral pose for the whole
  run. `maskedRestPairs` in the header says how many are hidden.
- **Thighs only.** Lower legs, feet and non-humanoid child bones (skirt, stocking, spring
  bones) are excluded. Left-left and right-right self contact is not measured.
- **Grid** is a uniform grid over triangle AABBs (4 cm cells, at most 256 cells per
  triangle, larger triangles brute-forced), so memory stays bounded by the triangle count.
  The grid result was checked against an all-pairs scan on random scenes, including
  oversized triangles, with no mismatches.
- Run time is a few seconds for a 40 s clip at 120 FPS on the synthetic model. Model1 is
  not timed here.

## Tests

`ctest --test-dir build/ci -R bgmedia.triangle` runs the triangle suite: intersecting
pair with depth, separated pairs, coplanar touching and separated and overlapping cases,
shared-edge fold, vertex touch, near miss within epsilon, degenerate triangles.

Run on the fixture (no leg mesh, reports zero triangles and exits 0):

```sh
./build/ci/bin/bgmedia_mesh_clip --legs --model tests/fixtures/minimal.vrm --fps 60 --seconds 2
```
