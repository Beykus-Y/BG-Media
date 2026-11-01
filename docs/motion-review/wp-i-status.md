# WP-I (weight over the support foot): unmerged work

Base: 3956408 (head of the closed PR #31, branch claude/project-thread-n4qwku).
Commits on this branch after 3956408. The balance smoothing is committed as WIP; the JSON was not regenerated for it.

- 0001-0003: weight over the support foot (check_weight.py, bgdance.py, regenerated JSON),
  pelvis compression after landings, finger-curl ripple without key steps, and a soft tanh
  clamp plus Gaussian on the ClipFootwork pelvis shift.
- 9999 (WIP): zero-phase smoothing of the balance shift (BALANCE_SMOOTH 0.10 beat) and an
  OVER_SUPPORT_Z knob. Regenerated JSON is not included.

Measured on sidestep at 123 BPM, minimal.vrm, 60 fps (hips acceleration in m/s^2, max / p95):
- Base 3956408: footwork off 9.6 / 6.7; footwork model 26.7 / 7.4.
- 0001-0003: off 28.1 / 11.7; model 69.9 / 12.1. Weight now 2.4 cm off the support foot (was 7.2).
- With 9999: off 11.4 / 8.5; model 63.6 / 8.8. Sidestep p90 weight is 7.02 cm, just over the 7.0 gate.

Still open: the model-mode spike at sidestep beat ~6.6. The cause is the "cross in front"
step (dances_b.py, L.step(4.5, 5.0, 8, 16, -20)). The leg proxy can't do the cross, so the
planner un-crosses the landing by 19-26 cm. The pelvis then is no longer over the moved
foot, and the IK reach (ikErr flickers 0/9 mm) jerks the hips. Re-authoring the cross is
not enough on its own; the ClipFootwork pelvis follow needs to match the over-support
gain for a moved stance.
