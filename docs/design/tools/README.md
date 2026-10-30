# docs/design/tools

Scripts that build and check the Allen design system. None of them touch a live device.

- `extract-font.mjs`: pulls Allen's stroke font out of `native/kernel/ui/allen_font.c` for the mockups.
- `make-screens.mjs`, `make-found.mjs`: generate the first-run screen mockups and the "what Allen found" screen.
- `shoot.mjs`: dependency-free Chrome driver that takes the screenshots and frame-time readings.
- `dogfood-wizard.sh`: runs the setup wizard in QEMU the way a stranger would (both intents, three ways of getting
  an AI), keeps a screenshot of every screen, the sound as a .wav and when each screen arrived. Needs a model server
  on 127.0.0.1:18091 and the shared QEMU gate lock; exits 2 when it cannot run, 3 when the lock is busy.
  The report is [../DOGFOOD-2026-10-20.md](../DOGFOOD-2026-10-20.md).
