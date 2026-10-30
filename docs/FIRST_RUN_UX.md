# First run: from power on to "Allen is ready"

Status: DRAFT 3 (L266, 2026-10-20). This is the spec the setup wizard (L267) and the driver builder (L268) build
against. Change it by ledger note on L266; the owner folds the notes in and bumps the draft number.

What this page fixes: every screen a person sees after AienOS is installed (or after `aienos-run` starts it), in
order, with the exact words, what happens on its own, what is asked, what is never asked, how long a step may run
before the person sees it moving, and how to undo each part.

## 0. Rules every screen follows

1. **Ask for intent and consent, nothing else.** The machine finds every fact it can find. The only questions are
   "what do you want this computer for", "may I do this", and secrets only the person holds (a Wi-Fi password, a key
   for an AI service), each asked once and only when needed.
2. **Nothing manual that the machine can discover.** No device lists to tick, no driver names, no disk layouts, no
   model file paths, no port numbers.
3. **Plain words.** No product names of our own parts on the screen (no "Aienux", "Interplane", "Store", "contract",
   "kernel"). Hardware is named by what it does first ("Graphics", "Screen", "Network"), then its maker's name.
   No marketing words: no "blazing", "seamless", "revolutionary", "magic". No exclamation marks.
4. **One thing at a time, in the middle of the screen.** Allen's look (night sky, Allen as light in the centre, one
   input bar at the bottom). No windows, menus, tabs or progress bars that jump. Every change eases in; nothing cuts.
5. **Always moving, never frozen.** Every step shows motion within its progress budget (section 3). If a step is
   slower than its budget, Allen says what it is waiting on in one line, in plain words.
6. **Every step can be skipped and resumed.** "Not now" is on every question. Setup picks up where it stopped after a
   restart. Saying "set up again" to Allen at any time starts it over without losing anything.
7. **Honest numbers only.** Any number on the screen (frames a second, model speed, memory) comes from a measurement
   made on this machine, or it is not shown. "Not measured yet" is a valid answer.
8. **Keyboard, mouse and touch all work.** Enter or a click picks the lit choice; arrow keys move between choices;
   Escape is "Not now". Typing a sentence goes to Allen on the intent screen; elsewhere in kernel images letters are ignored.
   (Kernel images today have no mouse driver: choices move with the arrow keys, Tab or a digit, Enter picks, Escape
   is "Not now". A mouse or touch works only in a window of a host that has one, such as `aienos-run`.)

## 1. The two ways people arrive

| Arrival | What they did | What starts the screens below |
|---|---|---|
| **Native** | Booted the AienOS USB stick or installed image (PC, Spark, Pi 5, iMac) | the computer's power button |
| **In a window** | Ran `aienos-run` on Windows, macOS or Linux (the try page, L264) | the runner opens one window; the window IS the screen below (the VM's own framebuffer and sound, not a web page) |

Both see the same screens, in the same order, with the same timing and the same sound. Differences are called out
per screen as **Native:** and **In a window:**.

## 2. The screens, in order

### Screen 1. Power on (not ours)

- **Native:** the computer's own maker logo for as long as its firmware takes (observed on the iMac: 13.5 s from
  power on to our first instruction, iMac report `boot_time` line, aienos_commit 0930240). We cannot shorten this;
  we never add to it.
- **In a window:** the runner window opens black, already the size of the screen it will be. No runner logo, no
  console text.
- **Words:** none.

### Screen 2. The night (the boot sequence)

The same on every machine: one shared module (`native/kernel/ui/`, section 5).

| Time on the night's clock | What the person sees and hears |
|---|---|
| 0 to 6.8 s | Black, then the eyes adjust: the brightest stars first, then the faint ones, then the Milky Way, in slow patches |
| 2.6 s | Two black holes start to bend the starlight behind them, then show their shadows, circling |
| 6.0 s | They spiral in, faster and closer |
| 11.4 s | The sound comes in: a short rising "blirp", the chirp two merging black holes make, raised into hearing |
| 12.0 s | They merge; a ripple runs out through the stars; the sound peaks and rings away by 12.3 s |
| 12.0 to 15.2 s | The last black hole rings, then dissolves; the light of the merger stays in the centre |

- **Words:** none. Nothing is asked. Nothing can go wrong on this screen that the person has to deal with.
- **What happens underneath, automatically:** the computer starts all its processor cores, finds its disks,
  network, sound, keyboard and mouse, and begins the device scan of screen 5. The night is not dead time; the
  machine works while it plays.
- **Keys:** a key press does nothing during the first run (the person is meant to see it once). On later boots any
  key brings Allen in at once (the night jumps to its last second).
- **Sound:** played through whatever the machine speaks with (HD Audio natively, the VM's sound device in a
  window). No sound device: the night plays silent, nobody is told.
- **Native:** if the screen cannot be drawn (no framebuffer), the machine goes on to screen 3 as text.
- **In a window:** if the host has no sound output, the runner plays silent; it never pops up an audio error.

### Screen 3. Allen arrives

From the merger's light Allen gathers in the centre (the mind drawn as light). Then, word by word under him:

> Hello. I'm Allen.
>
> I'm going to look after this computer with you.

After a pause (about as long as it takes to read the lines), the question comes up above the choices of screen 4:

> What would you like this computer to do best?

The bar at the bottom says: **Or just tell me**.

### Screen 4. The one question (intent)

Three choices, in this order, the first one lit:

> **Play games smoothly**
> Faster, steadier frames from your graphics and screen.
>
> **Run an AI model here**
> An AI that runs on this computer and stays on it.
>
> **Something else**
> Tell me in your own words.

Under them, small: **Not now**

- **Something else** opens the bar: "Tell me what you'd like to do." Allen answers in one line with what he
  understood ("You want a quiet machine for writing. I'll keep things simple and save power.") and a **That's right** /
  **Not quite** pair. **Not quite** reopens the bar.
- More than one goal: the person can say it ("both games and AI"); Allen keeps both and runs both paths in screen 7.
- **Not now:** Allen says "That's fine. I'll set up the basics and you can tell me later." and goes to screen 5
  with no intent; screen 7 is skipped.
- **Never asked here:** language (taken from the firmware, the keyboard layout, or the host in a window; the person
  can say "speak French" at any time), time zone (from the network), computer name (made from the maker and model,
  e.g. "Drake's iMac" if the host gives a user name, else "iMac").

### Screen 5. Getting to know this computer (automatic)

Allen says:

> Getting to know this computer.

Then what he finds appears one line at a time, each line easing in as soon as it is known (never all at once at the
end). The words follow this pattern; the bracketed parts come from the scan:

> Screen: [2560 by 1440], [144] times a second
> Graphics: [NVIDIA GeForce RTX 4070], [12 GB] of its own memory
> Processor: [AMD Ryzen 7 7800X3D], [8] cores
> Memory: [32 GB]
> Storage: [1 TB] fast drive, [2 TB] drive
> Network: wired, connected
> Sound: speakers and a microphone
> Keyboard and mouse: working

Then, if anything is on the network:

> Also nearby: [2] other AienOS computers, [a printer]

- **What happens underneath:** PCI, USB, ACPI, disks, displays (modes and refresh rates from the monitor's own
  description), processors and their kinds, and the network (other AienOS machines by Interplane discovery, L263;
  printers and storage later). In a window, the host launcher also hands in what the host has (its real graphics
  card, its installed AI models and running model servers, section 4 of L267's brief).
- **Not found / not working** lines are said plainly: "Wi-Fi: found, I don't have a way to drive it yet. I'll build
  one (screen 8)." Never red, never an error code.
- **Never asked here:** which devices to scan, permission to scan (scanning reads, it changes nothing).

### Screen 6. The one consent

Allen shows, in a short list, what he wants to do, then asks once:

> Here's what I'd like to set up:
>
> - [Use my own drivers for the graphics, network and storage]
> - [Set your screen to 144 times a second]
> - [Run the AI on your graphics card]
> - [Build drivers for the 2 parts I can't drive yet]
>
> I won't touch your other drives, and you can undo any of this.
>
> **Set it up**     **Show me each one**     **Not now**

- **Set it up:** everything in the list, in that order, with screens 7 to 9 showing progress.
- **Show me each one:** the same list, one line at a time, each with **Yes** / **Skip**.
- **Not now:** Allen keeps what already works (the machine is usable) and offers again the next time the person
  asks him anything about setup.
- The list only holds things that change the machine. Reading, measuring and testing in a sandbox never need consent.

### Screen 7. The intent path

#### 7a. Play games smoothly

> Making games smoother.

Lines appear as each is done:

> Screen set to [144] times a second.
> Graphics: [using my driver / using the maker's driver].
> Processor: set to keep the fast cores free for the game.
> Measured on this computer: [118] frames a second in my test scene. [Before: 97.]

- Every number is a measurement on this machine (rule 7). If nothing could be measured yet: "I couldn't measure
  this yet. I'll tell you when I can."
- What Allen cannot do yet is said once, with what would fix it: "I can't tune this graphics card's clock speed yet.
  I'm building a driver that can (screen 8)."
- **Never asked:** refresh rate, resolution, power plan, driver versions. The highest refresh the monitor reports at
  its native resolution is chosen; the person can say "lower the refresh rate" later.

#### 7b. Run an AI model here

> Finding the best way to run an AI here.

> Your graphics card has [12 GB] of its own memory. [Models up to about 14 billion parameters fit on it. The 30
> billion one fits if I share it with main memory, but it runs slower.]

Then the model choice (screen 7c).

- The fit numbers come from the model's measured size on disk plus its working memory, not from a table of guesses.
- **Never asked:** which GPU (the biggest one is used; both if the person later asks), quantisation, context length,
  batch size, server ports.

#### 7c. Which AI (the LLM pick)

Allen checks, in this order, without asking: models already on this computer (local disks; in a window, the host's
Ollama, LM Studio, llama.cpp model folders and any running model server), then models on nearby AienOS machines
(for example the Spark's model pair, the 27B and the 30B working together, L190). The default local AI is that
pair when a machine on the network serves it, else the 30B on this computer when it fits (L267 note).

- **Exactly one found:**
  > I found [Qwen 30B] on this computer. I'll use it.
  >
  > **Use it**     **Choose another**
  ("Use it" is lit; if the person does nothing for 10 s it is chosen and Allen says so.)
- **Several found:** a short list, the best fit first and lit, each with one plain line ("Runs on your graphics
  card", "On the Spark next door, fast"), plus **Connect my own**.
- **None found:**
  > I didn't find an AI model on this computer.
  >
  > **Get one for me** ([the 30B model, 18 GB download]; or the size that fits)
  > **Connect my own** (Claude, ChatGPT, or another service)
- **Connect my own:**
  > Which service?  **Claude**   **ChatGPT**   **Another service**
  >
  > Paste your key for [Claude]. I'll keep it sealed on this computer and won't show it again.
  (Another service also asks for its web address, with "the address your service's settings show for
  OpenAI-compatible apps" as the hint.) Allen tests it with one short request: "That works." or "That key didn't
  work. [Claude said the key isn't valid.] Try again?"
- **Never asked:** endpoints of known services, model file paths, model names the machine can read itself.

### Screen 8. Setting up the hardware (the driver builder, L268)

Starts right after screen 6's consent and keeps running in the background while the person uses the computer.

> Setting up your hardware.

One line per device, each with a calm state word:

> Graphics ([AMD Radeon Pro 580]): ready, my driver
> Network ([Broadcom]): ready, my driver
> Wi-Fi ([Broadcom BCM4360]): building a driver for it. This can take a while. You can keep using the computer.
> Webcam: ready, the maker's driver

- **States, and nothing else:** "ready, my driver", "ready, the maker's driver", "building a driver for it",
  "testing the driver I built", "waiting for you", "can't drive this yet".
- **Building:** Allen uses the AI chosen in screen 7c (by default the local pair or the 30B, as in 7c). The driver is
  written and tested in a sandbox against a model of the chip; the real device is never touched until the tests
  pass and the person agrees.
- **The second consent** (only when a built driver is ready for a real device):
  > I built a driver for your [Wi-Fi] and tested it without touching the real one. Turn it on?
  >
  > **Turn it on**     **Not now**
  If it fails on the real device, Allen puts the old one back by himself and says: "That didn't work on the real
  [Wi-Fi]. I've put the old driver back, and I'll keep working on it."
- **Never asked:** which driver to use, whether to unbind the maker's driver (Allen asks to turn his on; the swap is
  his job), log levels, test settings.

### Screen 9. "Allen is ready"

> Allen is ready.
>
> [Games will use your screen at 144 times a second.] [Your AI is Qwen 30B, on your graphics card.]
> [I'm still building a driver for your Wi-Fi; I'll tell you when it's ready.]
>
> Ask me anything, or tell me what you'd like to do.

The bar stays at the bottom: "Speak to Allen". The night stays in the sky behind him. Setup is over; anything left
(driver builds, downloads) carries on quietly and Allen mentions it only when it finishes or needs a yes.

## 3. Progress budgets

A budget is the longest a step may go before the person sees it move. These are design limits for L267/L268 to
meet and measure; they are not measurements. Each build reports its observed time against them.

| Step | Shows motion within | Says what it is waiting on after | Observed so far |
|---|---|---|---|
| Our first pixel after the firmware hands over (screen 2) | 1 s | (no words on this screen) | iMac: night on screen 15260 ms after power on, firmware 13548 ms of that (report, 0930240) |
| The night itself | plays 15.2 s, fixed | n/a | same clock on every machine (section 5) |
| Allen arrives after the night | immediately (it eases out of the merger light) | 5 s: "Still starting. [The disk is slow to answer.]" | not measured on the PC/VM yet |
| Each answer to a choice | 0.1 s (the choice lights up) | 2 s | not measured |
| First line of the scan (screen 5) | 1 s | 10 s with no new line | not measured |
| AI key test (7c) | 0.5 s (a moving dot by the key) | 10 s: "[Claude] is slow to answer." | not measured |
| A model download | 1 s (size and time left, from the measured rate) | 30 s with no progress: "The download stopped. I'll keep trying." | not measured |
| A driver build (screen 8) | 2 s (the state word changes) | 60 s per state: one line on what it is testing | not measured |

## 4. Undo and uninstall

| What | How to undo | What it leaves |
|---|---|---|
| Any setup choice | say "undo that" or "set up again", or choose it again in Allen's list | everything else stays as it is |
| A driver Allen turned on | "Put the old driver back" (one choice on the device's line), or automatic if it fails | the maker's driver was only set aside, never deleted |
| Screen refresh / processor setting | "Put the screen back" | the monitor's previous mode |
| A connected AI service key | "Forget my Claude key" | the key is erased from the sealed store |
| A downloaded model | "Delete the model" (Allen says how much space it gives back) | nothing else |
| **In a window** (aienos-run) | the runner's own "Remove AienOS" (`aienos-run --reset` removes the per-user disk; deleting the runner removes the rest; exact flags in L261/L262/L265) | the host computer is untouched; AienOS only ever lived in its cache folder and its own disk file |
| **Native, from the USB stick** | shut down and pull the stick | the computer starts as it did before; its own disks were not written |
| **Native, installed** | the installer's restore path (L260 owns it); until it exists, this row is UNKNOWN and the installer must not offer to install over an existing system | to be set by L260 |

## 5. The boot sequence is one module (for builders)

- Source: `native/kernel/ui/` (night.c, chirp.c, paint.c, allen_font.c, mind.c, allen_ui.c), byte-identical to
  what the iMac runs (aienos_commit 0930240). Every image builds the same files; nothing is redrawn per platform.
- Timing: the night's clock constants in `night.h` (CK_NIGHT_*), the sound's in `chirp.h` (CK_CHIRP_*). Change
  them there or nowhere.
- The sound is made from a formula at boot (a real merger's chirp shape, 200 Hz rising to 1000 Hz, peak at the
  merger). There is no sound file and no third-party audio; its licence is this repository's.
- Measurement of frame rate and dropped frames per machine: `docs/BOOT_EVERYWHERE.md` (L266).

## 6. Open questions (answer by ledger note on L266)

1. L267: does the wizard need words for a machine with no keyboard at all (Spark headless)? Draft answer: the
   wizard runs on the screen if one exists, else it waits for the first Interplane peer to open Allen's window for
   this machine.
2. L268: the exact state words in screen 8: are six enough?
3. L260: the installed (not live) undo path, row "Native, installed".
4. All: the 10 s auto-choose in 7c: keep or drop?

## Change log

- Draft 3 (2026-10-20): wording from L282's mockups taken (docs/design/screens/README.md, 9 changes: screens 3, 4, 6, 7b, 7c, 8)
  and the L267 build notes: no mouse in kernel images, letters outside the intent screen ignored. Also, from
  L267: the AI pick runs on the games path too (the driver builder needs an AI); there is no sealed key store on x86 yet.
  The current iMac build refuses custom service setup before asking for a key because its courier is unencrypted.
  A stale key-test state is refused locally and its key is wiped. Configure service credentials on the host and select
  its local model here. `llm.conf` names
  the key variable only; https services cannot be tested yet (no TLS in the relay, none hand-rolled);
  the host description reader is only in the launcher image.
- Draft 2 (2026-10-20): default local AI is the 27B + 30B pair when the network has it (L267 note); section 5
  points to the measured boot sequence.
- Draft 1 (2026-10-20): first version, from Drake's words in the L266/L267/L268 briefs and the iMac's boot.
