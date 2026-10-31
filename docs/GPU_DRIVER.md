# Our own GPU driver for the Spark (GB10)

Written for Drake. Ledger L332. Status at the bottom; every number has its source next to it.

## What a GPU driver does here

The GB10 graphics chip is a separate computer inside the Spark. It has its own small processor (NVIDIA calls it GSP)
that runs NVIDIA's own program, and it has job queues our code fills with work. A driver is the software on the main
processor that:

1. wakes the chip up and starts the chip's small processor with NVIDIA's program,
2. sets up memory so the chip and the main processor see the same model weights,
3. opens a job queue (a "channel") and tells the chip where it is,
4. after that, stays out of the way while our code puts work in the queue and rings a doorbell.

Today NVIDIA's open-source Linux driver (two main pieces, `nvidia.ko` and `nvidia-uvm.ko`, plus two display pieces,
`nvidia_modeset` and `nvidia_drm`) does steps 1 to 3. Omega and physics already do step 4 themselves: they write the
work into the queue and ring the doorbell straight from our own program, with no call into the driver at all
(physics nvrm/nvrm.c: the doorbell is a single memory write to a page the driver handed us once).

## What NVIDIA's driver costs us today

Rung 0 measures it. Results go in evidence/own-gpu-driver/rung0/ with the table below filled from them.

| What | Today with NVIDIA | Source |
|---|---|---|
| Driver calls per generated word | expected zero (work submit never calls the driver) | rung 0 cell (a), pending the window |
| Processor time the driver burns while idle | the display piece spins one core forever on this headless box | L158; rung 0 idle cells |
| Start-up calls before the first job | pending | rung 0 strace cell |
| Time to load the driver and wake the chip | not measured (never reloaded since boot) | rung 2 window 0, step 6 |

## What ours does instead

The driver core is written as device contracts: short, checked descriptions of the chip (registers, job queues, the
order things must happen in) that our compiler (`crates/aienos-devc`, ADR 0021) turns into C code. The first one is the
job queue: `native/kernel/gpu/contract/gb10_chan.devc`. Every number in it is tied to a quoted line of NVIDIA's own
source. The compiler refuses a contract that would be unsafe on the chip, for example one that rings the doorbell before
the work is published, forgets the memory barrier between the two, lets the queue overflow, or keeps memory in the
chip's cache that the main processor writes while the chip reads it (the mistake that cost two days in October).

The generated code has to behave exactly like the hand-written version from L193: same 225 checks, and a side-by-side
trace of every register write and the whole simulated memory after each of about 330 operations must match byte for byte.

The second contract is the chip start-up: `native/kernel/gpu/contract/gb10_gsp.devc`. It describes the eleven register
steps that hand NVIDIA's signed start-up package to the chip's security processor (SEC2), wait for it to check and start
the chip's own operating program, and confirm that program is running. The compiler refuses a start-up that writes the
package while an older message is still there, rings the bell before the message is complete, touches the chip's main
processor before the security processor releases it, or waits on anything without a time limit. A hand-written version
(`native/kernel/gpu/gsp_mmio.c`) runs against a simulated security processor; the generated one must pass the same 67
checks and make the very same register accesses in 16 scenarios, including every way the start-up can fail. Twenty
deliberate small errors in each version are all caught. None of this has run on the chip yet.

On Linux, a small add-on module `aien-gpu` (native/linux/aien-gpu) will host that core in place of NVIDIA's driver.
Its first version only reads the chip's identity, loads NVIDIA's firmware file and sets up one big shared memory area;
it binds to the chip only when told to, after NVIDIA's driver is gone.

## What stays on loan from NVIDIA, and why

The chip's small processor only runs a program signed by NVIDIA. A security block on the chip checks the signature before
it lets that program start (on the GB10 that is the SEC2 processor; this chip has no FSP), and we do not have NVIDIA's
signing key. So these files stay NVIDIA's, loaded the way
we load model weights, named here:

1. `gsp_ga10x.bin` (75,012,080 bytes, /lib/firmware/nvidia/580.173.02/), the chip's operating program.
2. The GSP boot loader and the signed first-stage loader (FMC) for GB10B, which ship inside nvidia.ko as embedded data
   (g_bindata_kgspGetBinArchiveGspRmBoot_GB10B.c, GspRmFmcGfwProdSigned_GB10B.c in the open driver source), together with
   the hash, signature and public key the driver hands to the security processor in one 868-byte message.

Everything on the main processor side becomes ours. If evidence ever shows the signature check can be satisfied another
way, it goes in as an open question with an experiment, and nothing is tried that could damage the chip.

## The rungs

0. Measure what NVIDIA's driver costs (above).
1. The driver core in our contract language, proven on the simulator (job queue and chip start-up done; next the
   shared memory area the chip program needs and the first message exchange with it).
2. Run it on the real chip under Linux, in a window the coordinator schedules, because it takes the GPU away from
   everything else for a while. Window 0 only reads the chip and gives it back (evidence/own-gpu-driver/rung2/RUNBOOK.md).
   Window 1, after window 0 passes, starts NVIDIA's signed chip program with our own code: our small Linux module lets
   our start-up tool touch only the 12 start-up registers (6 of them writable) and ring the security processor once;
   the tool builds the start-up package, hands it over and waits for the chip program to say it is ready. Every
   register access is recorded with its time. The one real risk: if the restart afterwards does not fully reset the
   GPU, it needs a power cycle at the wall before NVIDIA's driver works again. Status 2026-10-22: window 1 is built
   and passes its 56 simulator checks, with the real firmware package checked against its pinned fingerprints; it waits
   for window 0. Still open until it runs: whether the chip program asks our side to run a short list of chores before
   it says it is ready (if so, window 1 records the request and stops there).
3. The same core inside our own operating system on the Spark, once that boots natively (L318).

## Known unknowns (tested on the chip, never assumed)

- How the GB10 makes a channel runnable without NVIDIA's chip program: today the chip program does it on request.
- The doorbell's address on the chip when NVIDIA's driver is not there to hand us the page.
- Whether the main processor may write the address-translation flush register directly.
- GB10B keeps some cache lines "sticky" until flushed (uvm_gpu.h:1135, NVIDIA bug 4577236, fixed only in later chips):
  memory the main processor polls must be set up uncached; the contract compiler enforces that rule.

## The CPU sequencer

NVIDIA's driver runs a short program the chip sends back (`kgspExecuteSequencerBuffer` in
kernel_gsp.c of open-gpu-kernel-modules 580.173.02): register write, modify, poll, a delay, a
save, and four core operations. That program is now ours (`native/kernel/gpu/gsp_seq.c`). It
refuses an unknown opcode, a short payload, a save index of 8 or more, and any core operation
unless the caller supplies the callback, because those restart the chip. A copy with the save-index
check removed fails the test. The window-1 simulator runs a register-write sequencer and then
reaches INIT-DONE; an empty sequencer request still stops, as before. This has not run on the chip.
The live `nvidia.ko` is not replaced by this commit.

## Status

See the handoff ~/handoffs/2026-10-21-own-gpu-driver.md and the ledger line L332 for the live state.
