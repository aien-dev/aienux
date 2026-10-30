# Allen iMac bundle qualification, 2026-10-23

Executable source: `03f493a3bae88478ea702330253f6a6f45dd7d1d`, including saved-conversation recall and read-only DMAR inventory. No executable implementation changed in this deployment pass.

Fresh results are in `results.tsv`. The original bootdisk failure is retained: its old harness typed goodbye into the first-run setup wizard, so the expected goodbye save never occurred. The correction completes the existing wizard with real key events, explicitly waits for its states and completion, and verifies it stays closed on subsequent boots. Every original install, A/B update, byte-preservation and two-valid-memory-copy assertion remains. The corrected full four-boot test passed. Independent read-only review L685 returned no substantive findings.

The actual relay serving UDP47033 was started from this source and returned a real model reply through the kernel courier protocol; see `production-relay-smoke.log`. Separate live UDP and QEMU code-display tests use the existing Omega server at 18091. Recorded-reply tests are labeled separately. Host/QEMU execution is not physical iMac qualification. Durations in correctness-test logs are diagnostic, not quiet hardware performance measurements.

The full strict workspace integration run in `allen-dmar-verify-all-20261022.log` is a prior run, not rerun in this deployment pass. Its source inventory was freshly hash-checked against the current unchanged files. Fresh courier tests, warning checks, format check, mandatory crypto/secret checks and iMac regressions accompany it. The required pinned directory-index gate is retained and refreshed only under the legacy-gate exception; no verifier, threshold or assertion is disabled.

Deployment target: one signed LAB image, diagnostics enabled, GPU execution and GPU microcode disabled, existing private bench key. No key or key-bearing image is published here. The image is a LAN laboratory trial: courier UDP is unauthenticated and DMA confinement remains NOT QUALIFIED. Do not enter secrets into this trial.

Physical acceptance remains NOT_RUN until the iMac reports the final deployment commit and a real exchange. A normal internal-disk boot currently starts the old image; select the existing USB NETLDR stick using the Option boot picker to fetch the staged bundle. Actual staging state and final image hash are recorded separately after build/signature verification.
