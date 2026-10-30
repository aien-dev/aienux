# Native Omega language-model experiment

`llama.om` implements a pretrained Llama-style decoder as native x86 code compiled by Omega. Embeddings, RMS normalization, attention, RoPE, softmax, SwiGLU, matrix multiplication, greedy selection and the token loop all execute in Omega. C/assembly provide the existing kernel's page tables, interrupt/entry ABI, signature verification and display adapter. No C inference engine, Python runtime, Linux process, external model server or GPU is used by this experiment.

This is a bounded first experiment, not Allen's conversational replacement. A model-enabled image generates up to 32 tokens once after Allen opens, starting with the fixed public beginning-of-sequence token. It displays the decoded output and elapsed time and emits `local_model:` diagnostics. User questions can cancel it. Normal Allen questions and the three existing tools keep their existing routes. No user conversation is submitted to or logged by this experiment.

## Pinned inputs and reuse

- Compiler: `aien-dev/omega-mv` commit `bf54b19fa590e8ae925c0e80815516dbf41e7326`. Compiler and source hashes are in `docs/IMAC_LOCAL_TOOLS.md`. `target x86linux` selects native instruction emission; the function has no Linux ABI or system calls.
- Weights: `karpathy/tinyllamas`, revision `0bd21da7698eaf29a0d7de3992de8a46ef624add`, `stories15M.bin`, 60,816,028 bytes, SHA-256 `cd590644d963867a2b6e5a1107f51fad663c41d79c149fbecbbb1f95fa81f49a`. Shape: dimension 288, hidden 768, 6 layers, 6 attention/KV heads, vocabulary 32,000, original context 256.
- Independent reference and tokenizer: `karpathy/llama2.c`, commit `350e04fe35433e6d2941dce5a1f53308f87058eb`. `tokenizer.bin` SHA-256 `50a52ef822ee9e83de5ce9d0be0a025a773d019437f58b5ff9dcafb063ece361`. See `LICENSE.llama2-c`.
- `fexp` is reused from `aien-dev/omega-dnn` commit `2b7e6b3fee48921337f954c6eae102c3df8e41c0`, `om/dnn.om`. See `LICENSE.omega-dnn` (Apache 2.0 with LLVM exception). Other math here is specialized to this checkpoint layout and rounds stored values to f32.
- The 260K checkpoint is a second correctness fixture, not the deployed model: same model repository/revision, `stories260K.bin`, SHA-256 `b0a507e7ad0f626624f17112325e66691f9076d622e1d3274d103d00299f2696`.

Reuse search covered the local clones, aien-dev/aien-spark code search, open work and branches, Omega DNN, Flint inference and Allen small-GCN. The existing small-GCN runner was C/GPU code. The large Flint MoE checkpoint exceeded iMac RAM. The Omega DNN exponential and existing signed CPL3 runner fit and are reused; this small decoder supplies the missing native Omega inference path.

## Closed execution profile

Target 2, ABI 3 accepts exactly 32 code pages, 1,024 private workspace pages, 16 stack pages, 120,000,000 microseconds CPU, 180,000,000 microseconds elapsed, one return, and zero capabilities or IPC. Existing ARM and small-tool resource limits remain unchanged. The signed 64-byte data initializer binds the SHA-256 of both weights and tokenizer. Signature, artifact allowlist, and both asset hashes are checked at boot. Each copied code image is hashed before execution.

Code is RX, weights/input/metadata are read-only and NX, workspace/stack are RW and NX. Unmapped guards separate regions. Weights occupy the fourth fixed function parameter; the existing three-tool ABI is unchanged. Read-only weight pages include only zero padding on the final page. No external CALL can select ABI 3. CPU is preempted through the existing nominal 1 ms timer and returns to device polling; private workspace, code, registers and page tables are wiped afterward. Immutable weights remain resident. These controls are not a claim of whole-system or DMA isolation.

Input: 64-bit words `[prompt_count, new_token_limit, checkpoint_words, token_ids...]`, exact byte length `(prompt_count+3)*8`. Prompt and output are each limited to 32 tokens; the private KV cache holds 64 positions. Only the two pinned checkpoint shapes/layouts are accepted. Output is 32-bit words: count, final logits offset, vocabulary, KV capacity; token IDs start at word 16. The first page is the publication buffer. Only successful returns publish bytes.

## Reproduce

Keep checkpoints and private signing keys outside Git. Build and test on a spare host, not the interactive Mac. `OUT` below is a private absolute output directory; `OMEGA` is the pinned compiler checkout; `REF` is the pinned llama2.c checkout.

```sh
"$OMEGA/seed/out/omc" < native/kernel/x86/model/llama.om > "$OUT/llama.raw"
make -C native/kernel OUT="$OUT/build" tool
"$OUT/build/host/ck_artifact_tool" model-bundle "$OWNER_SEED" "$OUT/llama.raw" \
  "$OUT/stories15M.bin" "$REF/tokenizer.bin" native/kernel/x86/model/model_bundle.h
x86_64-linux-gnu-gcc -static -O2 -Wall -Wextra -Werror \
  native/kernel/x86/model/host_probe.c native/kernel/x86/model/host_call.S \
  -o "$OUT/omega-model-probe"
cc -O2 -ffp-contract=off -DCK_LLAMA_REFERENCE="\"$REF/run.c\"" \
  native/kernel/x86/model/reference_probe.c -lm -o "$OUT/reference-probe"
bash scripts/check_local_model.sh "$OUT"
LOCAL_MODEL_OUT="$OUT" LOCAL_MODEL_WEIGHTS="$OUT/stories15M.bin" \
  LOCAL_MODEL_TOKENIZER="$REF/tokenizer.bin" bash scripts/qemu_ck_local_model_test.sh
```

The host oracle checks exact greedy IDs and every final logit against an independent reference, with the preselected tolerance `1e-3 + 1e-4 * abs(reference)`. It is `check_local_model.c`, built and run by `scripts/check_local_model.sh`. Invalid inputs must trap. `admission_test.c` checks copied code, asset hashes, signatures, mutation refusal and unchanged old profile limits. `tests.inc` exercises actual CPL3 inference, immutable weights/code, NX, guards, inaccessible kernel memory, cancellation, deadlines, cleanup and a successful old tool after model faults. Test fixtures are excluded from release images.

A deployable image uses `CK_LOCAL_MODEL=1`, `CK_LOCAL_MODEL_FILE`, `CK_LOCAL_TOKENIZER_FILE`, `CK_LOCAL_TEST=0`, and the same owner-key/lab flags as the current iMac. It has a distinct `-MODEL.EFI` output. The approximately 62 MB image fits the existing 64 MiB signed network-loader limit. Stage with `scripts/ck_imac_netboot_push.sh`; keep the previous image for rollback. Apple firmware must start the existing network booter before it can receive the queued image. A queued image, emulator result, or unauthenticated diagnostic alone does not prove physical execution.

Physical CPU timing and GPU inference are separate qualification steps. This experiment has no GPU kernels or Radeon compute-driver qualification. Full repository strict validation is also separate from these targeted checks.

The host checker also has a weight-free boundary test: `bash scripts/check_local_model.sh --self-test`. It rejects empty or incomplete vocabulary logits and probe output that exceeds its capture buffer.
