Not the kernel. Not an implementation destination. Retired when L802 step 3 lands in .om.

# Boot fixture for aien-architecture-mv#314

## What this is, in plain words

A small test stand that lets an Omega program run on a bare emulated Arm computer with **no Linux
underneath it**. The program is the exact same file that also runs on Linux; this stand answers the
four services such a program asks for (read a request, write output, read the model at a position,
finish) and nothing else.

It exists so arch#314 can check "one model, one Omega program, no Linux in the path" end to end today,
while the real Omega kernel cannot be written yet: the Omega compiler has no words for privileged chip
instructions (page tables, exception vectors, the interrupt controller). Those words are the compiler
owner's (L408) prerequisite (c). Until they land, the stand is written in the seed assembler `asm2`,
the same tool the Omega seed itself is built with. It is **not** a kernel design and nothing should be
built on top of it. When the Omega kernel's boot step exists in `.om`, this folder is deleted.

What it is **not** evidence of: physical boot (it runs on QEMU), the GPU (it is one CPU core), a disk
driver (the emulator places the model bytes in memory before start; we call that "emulator-placed
storage"), or speed on the real machine.

## How a boot goes

1. **Atlas ROM** (the audited seed ROM, `omega-mv/seed/atlas/atlas.s2`, only its pinned checksum
   changed) checks the SHA-256 of a 256-byte second stage and refuses to run it if one bit differs.
2. **Stage 1** (`stage1.tmpl.s2`, 256 bytes) pins the length and SHA-256 of the bundle (the fixture
   plus the program file) and checks it with the ROM's own SHA-256 routine before jumping.
3. **The fixture** (`nucleus.s2`) checks its settings block, checks the program file (ELF header,
   segment sizes and places; a segment that is both writable and executable is refused), maps memory
   so that the program can only touch its own code (read and run only) and its own data (read and
   write only, never run), turns on memory protection, the interrupt controller and a per-request
   deadline, then starts the program at the lowest privilege level.
4. While the program runs, every service call, every byte moved, every fault and every deadline is
   counted. A program that faults, overruns its deadline or exits with an error is ended, its memory
   is wiped, and it is started fresh on the next request line. The fixture's own lines are framed so
   `demux.om` (Omega) can split them from the program's output.

## Services (the whole list; anything else returns "not supported", -38)

| number | name | what the fixture does |
|---|---|---|
| 63 | read | fd 0 only: the next request line (one line per call) from the request region. Arms the deadline. |
| 64 | write | fd 1 or 2: bytes to the serial port (ESC doubled so output can't fake a fixture line) |
| 67 | pread64 | fd 3 only: model bytes at the given position from the model region |
| 93 / 94 | exit | ends the run, accounting line, next run or halt |

Bad buffer (outside the program's own memory, into its own code for a read, longer than 1 GiB) -> -14.
Bad descriptor -> -9. Negative position -> -22. The program never sees an address of the fixture.

## Memory (emulated machine with 6 GiB)

| what | where | who may touch it |
|---|---|---|
| fixture code, page tables, saved registers | 0x1_0000_0000 (bundle: fixture first MiB, program file after) | fixture only |
| model bytes | 0x1_4000_0000, up to 1 GiB | fixture only, read only |
| settings block, request lines | 0x1_8000_0000, 0x1_8010_0000 | fixture only, read only |
| program code copy | backing 0x4040_0000, seen by the program at its own address | program read + run |
| program data (stack and pools, wiped every run) | backing 0x4060_0000.., seen at the program's address | program read + write, never run |
| serial port, interrupt controller | 0x0900_0000, 0x0800_0000 | fixture only |

## Files

- `table-el1.s2`: 38 privileged instruction lines for asm2, each citing its section of the Arm
  Architecture Reference Manual (DDI 0487 M.d) or the GICv3 manual (IHI 0069H). `table-el1.expect` is
  the reviewed disassembler reading of every line; the gate compares them.
- `nucleus.s2`: the fixture. `stage1.tmpl.s2`: stage 1 (the build fills in length and checksum).
- `fixture.sh`: builds the chain and boots it (`build`, `run tcg|kvm`). `gate.sh`: every check below.
- `demux.om`: Omega tool that splits a serial log. `probes/preadp.om`: Omega program that reads the
  whole model through service 67 and prints its length and a hash. `probes/probe.s2`: a deliberately
  misbehaving program (asm2, because Omega cannot express raw addresses or unknown service numbers).

## Gate

`./gate.sh [outdir]` runs under full emulation (TCG) and, when `/dev/kvm` is usable, under KVM
(the host chip runs the guest directly; still an emulated machine around it). Results: see
`evidence/` and the arch#314 issue comment for the run of record.

## Run of record (2026-10-24, gate RC=0, 28 checks)

`evidence/2026-10-24/gate.log` and the serial logs beside it, on omega-mv main 0d72560a (omc sha256 734f0a39...),
aienux main 5550b70f, asm2 sha256 6988abe0..., QEMU 8.2.2, host DGX Spark (aarch64). Same results under TCG and KVM:

- seed `echo.om` and `bytecopy.om` (55,705 bytes reversed): fixture output byte-identical to the hosted run of the same ELF.
- `preadp.om` read all 807,694,368 bytes of the pinned Llama-3.2-1B Q4_K_M file through service 67 in 1,542 calls and printed
  the same length and hash as on Linux; the fixture counted exactly 807,694,368 model bytes read.
  Guest time for that read: 0.72 s under KVM (calc:137e72d5), 6.83 s under TCG (calc:8d077fa5), hosted 0.72 s wall.
  These are emulator numbers, not a speed claim for any physical boot.
- misbehaving probe: 3 unknown service numbers -> -38; 5 bad buffers -> -14, bad descriptor -> -9, negative position -> -22;
  loads from fixture, model, settings, serial port, interrupt controller and unmapped memory, and a store into its own code,
  each ended only that run (data abort, class 36); a jump into its own data ended the run (instruction abort, class 32);
  `eret` and a write to VBAR_EL1 at the lowest level were undefined (class 0); an endless loop was ended by the 0.5 s
  deadline; exit 7 was reported; after each, a fresh run on wiped memory served the next request correctly.
  Totals: 13 runs, 10 faults, 1 deadline.
- chain controls: one flipped bit in the fixture or in the program -> stage 1 refused; in stage 1 -> the ROM refused;
  writable+executable segment, bad ELF magic, bad settings -> the fixture refused. The program never started in any of them.
