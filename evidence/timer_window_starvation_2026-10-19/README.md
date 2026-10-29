# M1 timer window starvation (aienos#434 gate AIENOS-CK-QEMU-GATES, rc_nonzero), 2026-10-19

## Symptom
Judge run of aienos#434 head 49e6f6a: `AIENOS-CK-QEMU-GATES` FAIL rc_nonzero. The failing child was the
Allen gate (`[allen] exit 1`), whose M1 checks (`scripts/lib_ck_m1_checks.sh`) rejected the timer window:
"timer IRQ delivered fewer than five ticks" or "tick statistics out of range". The same head passes
when the Spark is quiet (5/5 `qemu_ck_allen_test.sh` runs PASS), so the code under test was not at fault.

## Cause
`ck_timer_window` is wall-clock (200 ms). QEMU TCG shares the Spark with the whole hive (load 20 to 67).
A starved vCPU runs fewer than five 10 ms ticks inside the 200 ms window, and `ck_timer_tick` read the
counter a second time after re-arming, so a host stall between the two reads made one interval long and the
next one short (min_us as low as 9 against a 10000 us period).

## Fix (native/kernel/arch/timer.c, scripts/ck_gates.sh)
1. An interval runs from IRQ entry to IRQ entry, the same counter value the next deadline is built from, so
   it is never shorter than the period.
2. The window keeps counting, up to 5 s, until five ticks have arrived. A dead timer still reports 0 ticks.
3. A failing child's own FAIL lines are now copied into the gate's evidence blob (the child log lives in a
   temp folder that is removed at exit, which is why the first failure could not be read afterwards).

## Measurement (Spark, 20 boots of `scripts/qemu_ck_boot_test.sh` per arm, pinned to CPU 3 with 5 busy loops
on the same CPU at the same nice level; `pinned-stress.sh`)
| arm | pass | fail |
|---|---|---|
| before the fix (origin/main 18e2765) | 2 | 18 |
| after the fix (fix/timer-window-starvation) | 20 | 0 |

Red: `red-before-fix-fails.txt`, `red-before-fix-stats.txt`. Green: `green-after-fix-stats.txt`.
Quiet machine, 5 runs of the Allen gate before the fix: 5/5 PASS (so the fix changes nothing when idle).
Also run after the fix: `scripts/ck_gates.sh --self-test` PASS, `make -C native/kernel test` PASS (CK_CORE_HOST).
