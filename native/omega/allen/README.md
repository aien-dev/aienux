# Allen in seeded Omega (L694, WIP)

This directory contains handwritten seeded Omega implementations of Allen's
transport-independent state, protocol, memory codecs, scene state, code layout
and paint primitives. It compiles directly to static native Linux AArch64 ELF.
It has no foreign implementation, interpreter, C translator, launcher or fallback.

It is **not the complete iMac Allen replacement**. The old production C/Rust
paths remain live. No service, machine, physical disk or boot path was changed.

Build on Linux AArch64 from the pinned public compiler source. These are
component build instructions, not a released interactive alpha. From this
repository root, with a fresh `build/allen-toolchain` directory:

```sh
git clone https://github.com/aien-dev/omega-bridge.git build/allen-toolchain
git -C build/allen-toolchain checkout --detach 0fc7d8662cb799e7c206b17c18ea4c3ec2b8fea9
sh build/allen-toolchain/tools/bootstrap.sh --offline
```

The bootstrap reconstructs the compiler from seed source and checks its
self-rebuild equality and fixed digest. Independent diverse compiler ancestry
qualification is a separate claim; self-rebuild equality does not establish it.
No private compiler folder or preinstalled Omega binary is needed. Since the
old C look (imac/old-look) the gate pins omega-mv `feat/x86efi-exit-sites`
8da48140 (draft #977), whose `const`, `bitcast` and larger exit-site tables
look-paint.om and the firmware unit need; the expected compiler SHA-256 is
`a1d3217674511f3880962cf4bfd8ec0f254104cf6cb03834b005237808f6ade4`
(`~/work/IMacLook/omc2` on the Spark; pass it as `OMEGA_COMPILER`).

Run the complete component gate:

```sh
sh native/omega/allen/gate.sh target/allen-omega-L694/fresh-output
```

On the shared Spark, prefix bootstrap and gate commands with `quiet-build`.
For a compiler reconstructed elsewhere, set `OMEGA_COMPILER` to its executable
path. The same fixed digest is mandatory; this setting does not permit an
unqualified compiler. Paths containing spaces are supported. Build-orchestration
checks are available through `sh native/omega/allen/test-build.sh COMPILER FRESH_OUTPUT`.

The output directory must be fresh. `build-one.sh` hashes the compiler before
**every invocation**, rejects empty/wrong-machine/nonstatic ELF and marks only
qualified output executable. Failed compilation leaves no published executable;
existing outputs, including dangling links, are never replaced.
The gate builds all modules together for every test,
executes handwritten Omega test programs, compares a byte-exact independent memory
fixture and rejects compiled behavioral mutants. The build uses POSIX shell,
Git (source reconstruction), GNU file/text utilities, readelf, dd, xxd and
sha256sum as external development tools. Existing Rust courier `--split` was used
once as a fixture oracle, never as an Omega runtime dependency.

## Modules and API boundary

* `runtime.om`: one outstanding job, chat/code/follow-up, immediate code ack,
  ID exhaustion refusal, stale answer rejection, cancel/reset,20s slow-once and
 30s silence timeout. Injected monotonic milliseconds, no clock syscall.
 32 transcript lines of159 bytes, evict oldest whole line, restoration entry.
* `intent.om`: legacy intent word tables and JOB composition, preserving Allen
  and restored self/person names. No new canned ordinary answer table.
* `code.om`: CODE headers/escapes,128-byte pieces/32768-byte code,256 pieces,
 IEEE CRC32,800 lines, reorder/loss/duplicate handling; display only when whole.
* `reply.om`: SAY multipart/WAIT folding,159-byte answer capacity, one AGAIN
 retry for missing/bad code before giving up. Model text is always external input.
* `courier.om`: byte-compatible ALLEN-COURIER datagrams, session binding,
 JOB reassembly4096 bytes, ordered LINE queue8x319 bytes and duplicate receipt.
 This is protocol core, not a socket, Ethernet driver or cryptographic peer check.
* `scheduler.om`: serialized client lifecycle over that protocol. `qstart(run,now)`
 requires a nonzero caller-supplied boot/session ID. `qsubmit(now)` snapshots
 In/Inlen (1..4096 bytes), maps CR/LF to spaces, increments the bounded wire ID,
 and queues 1000-byte JOB parts. A new submission is refused while the previous
 job is unacknowledged or a send result is outstanding. Once acknowledged, a new
 submission explicitly abandons the previous reply queue; the runtime must still
 enforce its one-outstanding-conversation rule. `qcancel` discards pending sends,
 received lines and job state, retaining session ID/clock/keepalive state.
 `qnext(now,link)` publishes one datagram in Outb/Outlen, prioritizing pending GOT,
 then HELLO, then JOB. Consume those bytes before any other serializer. Call
 `qsent(now,success)` for each offered datagram, using actual whole-datagram send
 success, not an assumption; failed sends remain due. `qincoming(now)` consumes
 In/Inlen and returns refused0/accepted1/duplicate2. Drain accepted lines with
 `qtake`. GOT is only scheduled after a line is retained or recognized as a
 duplicate; queue-full and out-of-order input get no receipt. One pending GOT
 slot provides backpressure rather than silently losing acknowledgments.
 HELLO uses2s before readiness/after expiry and10s while ready; HI remains ready
 for strictly less than30s. Whole JOB sets retry after2s until ACK or LINE.
 All mutating timed entries refuse backward clocks. `qready(now)` is read-only.
 The provider owns a genuine monotonic millisecond clock, link state and actual
 datagram transport. No clock/socket, Ethernet or iMac attachment is claimed.
* `memory.om`:4096-byte AIENALM1 v2/v3/v4 and AIENNOT1 codecs. `Mem` is canonical
 decoded payload, `Unit` is input/staging, `Enc` is committed encoder output.
 SHA256(header0:32 || payload) is **unkeyed integrity**, never authorization or
 rollback freshness. Those independent proofs remain absent in this legacy format.
 `loadbeg` resets load state, `loadcopy(slot)` considers each valid copy once.
 `txbegin -> txwrite -> txflush -> txread` publishes store generation only after
 exact readback. Provider reads back into Unit and writes Enc to Tx's target.
 `nbegin -> nwrite -> nflush -> nread` verifies note writes before parent saves;
 provider must update the parent's note count only after nread succeeds. No disk
 syscall/driver is bound. Crash tests use explicit fixed simulated disk buffers.
* `context.om`: restores a512-slot catalog from already committed note records
 and appends bounded memory context to actual application JOBs. After loading
 canonical Mem through the memory codec, call `ctxbegin`; for each declared slot,
 place its4096-byte record in Unit and call `ctxtake(index)` exactly once, in any
 order. Return0 means refused/no visit,1 valid retained,2 damaged visit retained
 as gone. Deleted/damaged notes never enter model context. The catalog becomes
 ready only after every declared slot was visited. Cx contains declared count,
 visited count, readiness, damaged count and loading flag. Loading is refused
 during a live conversation or note/parent write transaction. The provider must
 keep the chosen Mem generation stable and restart catalog loading after any
 memory reload; this is not an incremental concurrent store interface.
 `ctxmeta(limit)` stages byte-exact legacy self/name/starts/fact and newest-live-
 note metadata into Meta/Mx without touching Unit, Enc or outgoing bytes.
 `ctxappend` appends all or none within the4096-byte JOB cap, preserving outgoing
 bytes/length/error on refusal. `appask` validates context before reading its
 variable-layout identity fields, includes the newest six live notes, and refuses
 incomplete catalogs/oversized context before conversation or wire IDs advance.
 SHA validation is still unkeyed integrity, not authentication. No physical disk
 loader/writer, note search index or concurrent mutation/commit provider is bound.
* `view.om`: legacy code row wrapping, continuation numbering and saturated scroll,
 bounded4096 rows. The full code remains held when the row index reaches capacity.
* `scene.om`: mood/say/eight thing kinds, placement/emphasis/arrival/direction,
 typed field bounds, four things/six fields/2048-byte input/12 lines.
* `scene-input.om`: `scdecode` consumes In/Inlen through bounded scene cleanup,
 restores the exact caller input and length, and fills the same Sc/Things/Stext
 output as the strict `scene` parser. Input and normalized output each have a
 2048-byte cap. NUL or normalization overflow refuses the whole scene and clears
 published scene metadata. ASCII tabs/spaces collapse; CR, other control bytes
 and non-ASCII bytes are dropped, matching the legacy ASCII display boundary.
 Case-insensitive exact `<think>`/`</think>` tags hide their entire lines and all
 enclosed lines; an unclosed thought block hides the remainder. Lines beginning
 with triple backticks are discarded, not the enclosed scene. Supported markdown
 prefixes and keyword emphasis are stripped. Known kind/placement/mood words are
 case-folded without changing displayed field text. The first twelve retained
 canonical lines are eligible, including for mood-only and fallback decisions.
 If no recognized scene exists, the first short unrecognized line becomes the
 answer (159-byte cap); rejected known structured commands do not become prose.
 This fallback is deliberately stricter than legacy truncation/ellipsis.
 Nm[0..8] reports thinking, normalized bytes, tidying count, hidden lines,
 scratch-line bytes, eligible mood seen, invalid/overflow, fallback bytes and
 retained canonical-line count. Like other modules, calls must be serialized.
 This scene API is not yet connected to the application reply/display provider.
* `paint.om`: legacy integer color operations and soft-light pixel evaluation.
 Executed192x128 halo/heart component pose, colors from `ck_mind_draw`. This is
 not the full Allen sky/network animation, font, canvas item renderer or UI.

Fixed global pools model the platform boundary because seeded Omega globals
cannot be passed as array parameters. Functions are serialized, not reentrant.
Caller-owned metadata is trusted only where stated; lengths and external wire
values are validated at entry. Kernel/platform ABI and object ownership wrappers
must be specified when a qualified native provider exists.

## Bound conversation/provider API

`application.om` connects the runtime, intent/job builder, courier scheduler,
CODE assembly and SAY/WAIT handling. This is an executed native core entry path,
not a bound socket/device loop. The older development `main.om` stdio entry
remains separate; it does not exercise this datagram binding.

1. `appinit(run,now)` initializes one boot session using a nonzero unique run ID
   and actual monotonic milliseconds supplied by the future qualified provider.
   Reinitialization is refused rather than reusing job IDs.
2. Put a prompt into In/Inlen, then `appask(now)`. It validates admission before
   adding a transcript entry, builds the real JOB text and queues it. Code requests
   expose the existing acknowledgement immediately. Restored facts and up to six
   newest live notes are included in the queued job. Busy, outstanding-send,
   pending-receipt, invalid-prompt and ID-exhaustion cases refuse admission.
3. `apppoll(now,link)` advances silence/slow deadlines and returns whether one
   datagram is offered in Outb/Outlen. Send those exact bytes and call
   `appsent(now,success)` with the real whole-datagram outcome. Do not call core
   serializers yourself while a send is pending.
4. Copy a received datagram into In/Inlen and call `appreceive(now)`. Its boolean
   reports **transport acceptance**, not acceptance of the inner reply. A[2]
   distinguishes visible text1, queued code recovery2, code give-up3, refused
   payload4, slow notice5 and silence timeout6; zero means no UI event. Consume
   events after each successful call; later successful calls may replace them.
   Answer/R[8], Code and transcript remain the actual retained content.
5. `appcancel` cancels both layers; `appreset` additionally clears runtime session
   display/history state, without resetting conversation or wire counters.

A missing-code response queues AGAIN with the original conversation ID. The
triggering LINE is acknowledged first, then AGAIN uses a **new wire sequence**.
Consequently the two counters can differ; subsequent valid responses still bind
correctly. Incoming reply parsing preserves any outstanding outgoing datagram.
Old CODE cannot populate or change a code buffer during a newer chat request.
WAIT updates the silence deadline without fabricating an answer. Silence timeout
cancels courier retries as well as the conversation. Calls remain serialized;
fixed global arrays are not thread-safe. Provider must not mix direct core queue
operations with this application API.

## Runnable bound provider entry

`provider-core.om` and `provider.om` expose the actual bound application through
versioned length-framed stdin/stdout. See [PROVIDER.md](PROVIDER.md) for the exact
40-byte header, operations and status/event/body format. The gate produces the
static native executable at `<output>/provider`, plus native input and output
verification programs. For example the already-built fixture can drive it:

```sh
printf 0 | <output>/provider-input > /path/to/private/events.bin
<output>/provider < /path/to/private/events.bin > /path/to/private/results.bin
<output>/provider-output < /path/to/private/results.bin
```

These are deterministic test events, not a live model conversation. The entry
actually calls the memory/catalog/application/scheduler/code functions, reads
and writes real streams, and keeps offered datagrams pending until an explicit
SENT outcome. It emits actual retained answers and completed code, never canned
ordinary replies. Restore records are supplied by the caller, not read from disk.
Malformed/oversized/truncated frames stop immediately with exit65; well-framed
semantic refusals return status0. Real pipe replay with byte-at-a-time writes and
six framing/refusal cases run in the full gate. The old line-oriented `main.om`
remains separate and its byte-exact goldens are unchanged.

This is an executable host integration boundary, **not** a qualified socket,
monotonic clock, storage/device provider, live-model service or EFI/iMac build.

## Stable application presentation

`presentation.om` binds successful `dask`/`drecv` application events to retained
scene state and completed code rows. See [PRESENTATION.md](PRESENTATION.md).
The display snapshot survives hidden/invalid replies and later reuse of the code
assembly buffer. Input bytes and pending wire output are preserved; reset clears
retained presentation bytes. Native tests exercise real courier envelopes through
these wrappers, not merely direct scene-parser calls.

The provider executable now calls these display wrappers for ASK/RECV/RESET,
while exporting the same raw answer/code records as before. Native provider tests
check retained scene/code updates, refused-frame isolation and complete reset;
compiled bypass mutants catch regressions in each attachment. This is a host-side
presentation binding, not a pixel renderer or display device. Existing SAY folding and its
159-byte bound remain; multiline model-scene transport is not yet attached.

## Native stroke font

`font-data.om` retains Allen's existing95 printable-ASCII glyph designs and91
integer sine samples; `font.om` is a handwritten fixed-buffer Omega rasterizer.
It supports cap heights1..64 and weights1..2500, maps nonprintable bytes to `?`,
and produces one antialiased glyph mask at a time without a C font runtime.
The full gate compares native output for294 glyph/size/weight/fallback records
against frozen bytes collected from the unmodified legacy implementation. That
legacy code is an external reference oracle, not an Omega compiler or runtime.
`<output>/font-specimen.ppm` is drawn entirely by the native Omega executable.

See [FONT.md](FONT.md) and `evidence/allen-omega-L694/L758/`. This implements the font
rasterization component, not scene layout, frame animation or the display driver.
The presentation/provider attachment still needs the actual screen renderer.

## Intentional refusals and unfinished compatibility

Overflow refuses whole prompts/answers/scene fields instead of silent truncation.
Reply capacity159, prompt capacity159, code32768, job4096, note127. Conflicting
CODE duplicates and differing language headers are refused rather than overwriting.
Numeric overflow, backward timestamps, request ID wrap and v4 sky pad are refused.
Job instructions and record bytes retain legacy structure; envelope metadata is
never added to Store v1 superblocks. No identity migration fields are rewritten.

Remaining portable work: remaining scene aliases/missing-colon repairs, field
ellipsis, timer interpretation, transcript-to-notes restore,
full notes indexing/retrieval, catalog update/store integration, transport binding,
full sky/catalogue/font/animated-network/canvas visual parity, integration and
broader malformed-input coverage. Partial modules are not whole compatibility.

Required target gates: qualified seeded x86 EFI target (FR9), privileged/MMIO and
interrupt/native device access(FR8), durable I/O(FR1), bounded networking(FR16),
keyboard/framebuffer/disk/NIC providers, iMac QEMU boot/chat/keyboard/bootdisk,
then signed physical trial. No C bridge can satisfy these. L408 owns compiler
work; L690 owns Hermes JSON/WebSocket/admission. Neither tree was modified.
