# lm1b: a 1-billion-weight language model written in Omega

`lm1b.om` is one Omega program. The seed-born Omega compiler (`omc`) turns it into one small Linux
program for the Arm cores of the Spark. It reads the pinned Llama-3.2-1B-Instruct model file once,
keeps every weight in its own memory, then answers request lines one after another without reloading.
Its answers match the Rust reference engine (`crates/aienos-infer`, plain f32 path) to the last bit.

The same program bytes are meant to run later with no Linux under, inside a boot fixture. This folder
covers the hosted run only (on Linux). Part of arch#314.

## What works (measured)

| Check | Result | Raw log |
|---|---|---|
| Load: size, header, pinned manifest, full-file SHA-256 | `load ok`, 12.78 s wall (13.33 s in an earlier run) | `evidence/lm1b-load.time` |
| Job 1: cow prompt (19 ids), 8 greedy tokens | tokens and logit bits equal the reference | `evidence/lm1b-main.out`, `evidence/ref-cow8.txt` |
| Job 2: rainbow prompt (17 ids), 8 tokens | tokens and logit bits equal the reference | `evidence/ref-rainbow8.txt` |
| Job 3: cow prompt again, same process | identical to job 1 (weights stayed resident) | `evidence/lm1b-main.out` |
| Three jobs, one process | 303.92 s wall, 76 forward passes, exit 0 | `evidence/lm1b-main.time` |
| Negative controls and the whole rerun | `gate.sh` prints `RC=0` | `evidence/gate.log` |

"Logit bits" means the exact 32-bit pattern of the winning score at every step, so a match is a
bit-for-bit match of the whole calculation, not a near match. The reference prints those bits when
`AIENOS_BITS=1` is set; that one opt-in switch was added to `crates/aienos-infer/examples/model_probe.rs`
on this branch for this purpose (it changes nothing else).

Speed, measured on one Cortex-X925 core, hosted:
- Load: 12.78 s wall (`evidence/lm1b-load.time`). The 50 file reads take 0.25 s of it
  (`evidence/lm1b-load-strace.txt`); the rest is computing, mostly the SHA-256 of the whole file.
- Forward pass: 3.824 s on average over the 76 passes of the three-job run
  (`(303.92 - 13.33) / 76`, calc:7b4d2b843c61a5ee12038ecb8f05facfb9d0b506f083d8b07047978faa5566c7).
- The Rust reference on the same core: 0.7146 s per generated token (`evidence/ref-cow8.txt`), so this
  first Omega version is 5.35 times slower per pass
  (calc:f036175030a053abf4b2c7cb963433d2f47e0f9e6874a3fc0b9e2533f30bae2b; the Omega average includes
  prefill passes, which skip the large output matrix, so this ratio is an upper bound on the gap). No
  attempt at speed was made yet: the goal of this slice is identical numbers.

## The numeric contract

Every number is computed with the same operations in the same order as the Rust reference, one IEEE
rounding per operation, round to nearest even, no fused multiply-add (Rust never fuses; omc never fuses,
OMEGESE.md 9.9). In detail:
- Weights: Q4_K and Q6_K blocks are unpacked exactly as `src/quant.rs` does, and each 256-weight block's
  products are summed in index order starting from -0.0 (Rust's `sum` for f32), then added to a row total
  that starts at +0.0.
- Math: `sqrt` is the reference's five Newton steps from a bit seed (not the hardware square root),
  `exp` is its Cody-Waite reduction plus degree-6 series, `ln` its atanh series, `pow = exp(e * ln b)`.
- The rotation angles use `%` on f32, which is an exact remainder. It is implemented exactly here with
  whole-number long division on the mantissas (as musl's `fmodf`), not `x - trunc(x/y)*y`.
- Constants like 1/6 are correctly rounded f32 divisions done once at start-up; pi, 2 pi, ln 2 and the
  others are the exact f32 bit patterns Rust uses, entered with `bitcast`.
- Argmax takes the first index on ties (strictly greater wins).

## Memory budget

Omega has no heap: everything is a fixed pool in the program image (at most 1 GiB of pools).

| Pool | Bytes |
|---|---|
| The model file, whole, in 49 byte pools (48 of 16,777,216 bytes, the last 2,388,000, calc:7220fb82e9cb86995dfcefc580d79d848e22af2d8959971c81378ddd18d1187f) | 807,694,368 |
| Attention memory (keys and values): 16 layers x 256 positions x 512 values x 4 bytes x 2 (calc:f6585eec2ae1d95b472256131dc174ca953f0d978e0ce698dedff437d9d816a3) | 16,777,216 |
| Activations and logits (161,024 f32) | 644,096 |
| Norm weights (67,584 f32) | 270,336 |
| Row buffer, line buffers, tables, constants (calc:09d16d2723e799c6eb2e3874ee598e3ef9f4af941062d0e70ec3131ce356fd12) | 222,824 |
| Total of the pools above (calc:d52ac033781d8f2cbb8c4fb37a9a81666e7c58ce68e0c4db243e77da267b46ea) | 825,608,840 |

The compiled program's read-write region is 0x31b5cd50 bytes: the pools above, the 696 bytes of the
vendored SHA-256 state and the fixed 8 MiB stack, 712 bytes over the sum above
(calc:fe8d0bec2ca48588975eedf813c0e6a96ae84ea963c7c74e9ce200c2a17010bf), leaving 239,743,664 bytes
under 1 GiB (calc:9901dc10ccd11f1d1c93792dd00f1ee280766343394a3c580845e77dfcdd7629). Measured peak
resident memory: 791,568 KB (`evidence/lm1b-main.time`).

A weight row is copied (at most 6,720 bytes) from its pool into the row buffer before its dot product;
a row may cross from one pool into the next, and the copy handles that.

## I/O contract v1 (the boot fixture depends on this)

- fd 3 is the model, read only with `pread`. fd 0 carries requests, fd 1 carries responses. Nothing else.
- The program uses only these Linux services: read (63), write (64), pread64 (67), exit (93). No
  arguments, environment or clocks. `gate.sh` checks both the source words and the service numbers in
  the compiled bytes.
- Request line (ASCII, single spaces):
  `job <id> gen <n> budget <steps> stop <eos> fault <0|1> ids <t1> ... <tn>`
  - `<id>`: 1 to 32 printable characters, echoed back as written.
  - `budget`: the most forward passes this job may use. When it runs out the job ends
    `status cancelled` at a pass boundary; its attention memory is wiped; the weights stay; the next
    job runs normally.
  - `fault 1`: the program stops itself on purpose in the middle of the job (pass `n / 2`, layer 8)
    with trap 12, so the process exits with status 112. This is for testing the supervisor's recovery.
- Responses:
  - once: `load ok bytes <n> tensors <n> sha256 <hex> passes 0`
  - per job: `job <id> status <ok|cancelled> passes <n> tokens <id> ...`, then
    `bits <u32> ...` (the f32 bits of each chosen logit, decimal), then
    `acct job <id> rowbytes <weight bytes read> passes <n>` (counts only, no clocks).
  - a request that cannot run: `job <id|?> status refused reason <word>`, then the next line is served.
    Reasons: `syntax`, `toolong` (line over 65,535 bytes), `toomany` (more than 256 ids), `badid`
    (id not in the vocabulary), `empty`, `range` (gen 0 or fault not 0/1), `context` (prompt plus
    generated tokens would pass 256 positions). Requests are refused, never cut short. Empty lines
    are skipped.
  - a model that fails admission: `load refused reason <word>` and exit status 3. Reasons: `read`,
    `size`, `magic`, `header`, `manifest`, `sha256`.
- End of input on fd 0: exit 0.

## Admission (identity of the model)

At load the program checks, in order: the file is exactly 807,694,368 bytes; the header starts with
`GGUF`; the header rebuilt as text (version, every metadata key with its type and value, every tensor
with its name, type, dimensions and offset, the data start) is byte-for-byte equal to `manifest.txt`,
which is pinned inside the program; and the SHA-256 of the whole file equals the pinned digest.
`manifest.txt` was written by `oracle/manifest-dump` using the reference engine's own file parser, so
two independent readers agree on it (`gate.sh` regenerates and compares it). Hyperparameters (16
layers, width 2048, 32 heads, 8 key/value heads, feed-forward 8192, vocabulary 128,256, rope base
500000, norm epsilon) are part of that manifest.

## Files

| File | What |
|---|---|
| `lm1b.in.om` | the program, hand-written (edit this) |
| `gen.sh` | expands the template: the 49 pools and the code that names each pool (Omega cannot pass a pool to a function), the pinned manifest, tensor slots and digest |
| `lm1b.om` | the generated program (committed; `gate.sh` checks it is in sync) |
| `sha256.om` | SHA-256 in Omega, vendored unchanged from aien-dev/benchmarks-mv `om/data/sha256.om` at 567c5da |
| `manifest.txt` | the pinned model manifest |
| `oracle/manifest-dump/` | small Rust tool that prints the manifest with the reference parser |
| `prompts/` | the two prompts as token ids and the three-job request file |
| `gate.sh` | reruns everything from a clean checkout and prints `RC=0` |
| `evidence/` | raw logs of the runs quoted here |

Run: `sh gen.sh && omc < lm1b.om > lm1b && chmod +x lm1b && ./lm1b 3< MODEL < prompts/main-jobs.txt`.
Gate: `sh gate.sh` (needs the pinned omc, the pinned model and cargo; about 10 minutes, not measured
precisely; it copies the model once for the flipped-byte test).

## Negative controls (all in `gate.sh`)

- One byte of tensor data flipped in a copy of the model: `load refused reason sha256`, exit 3.
- A copy of the program whose manifest names type 14 instead of 12 for one tensor: `load refused reason
  manifest`, exit 3.
- 257 ids: `toomany`. 250 ids with gen 8: `context`. Id 128256: `badid`. Two malformed lines: `syntax`.
  The process keeps serving after each.
- Budget 5 on a 19-id prompt: `status cancelled passes 5`, then the next job's tokens and bits equal the
  reference.
- `fault 1`: exit status 112 and no reply line for that job.

## Limits

- CPU only, one core, scalar code. No GPU, no SIMD, no threads.
- Hosted on Linux only so far; the boot fixture is separate work.
- The tokenizer is outside: prompts arrive as token ids (made with the reference tokenizer).
- 256 positions per job.
- This is the plain f32 path of the reference, not llama.cpp's int8-activation path; on the cow prompt
  both give the same 8 tokens (`crates/aienos-infer/tests/fixtures/ref_cow.txt`).

## Compiler findings (recorded for the omc owner, ledger L408)

- A function declared `to f32` is refused with error 5 (`fn f to f32` / `ret` of an f32), although
  OMEGESE.md 9.9 lists floats as allowed returns; f64 returns work. Here every float helper returns the
  f32 bits as `uint` and the caller uses `bitcast`.
- A function may use at most 16 registers: parameters, every name alive in the current block, and the
  temporaries of the current statement. An access to an array of 4,096 or more elements uses one extra
  register. Large functions were split for this.
- A local name may not equal a global's name (error 10), so pool names `m0`..`m48` are taken.

## Provenance

omc sha256 734f0a39319fbfa930bcc62cb30dbf06d47d78e126372b74f5baf478e6126b61 (born by
`sh seed/omega-from-seed.sh` on aien-dev/omega-mv main 0d72560a). Model sha256
3f5a22426976ab26cfe84dba63c1d08391717abb1af893e10f1b2968d862dcc1. The ELF and source hashes of the
recorded run are in `evidence/shas.txt`.
