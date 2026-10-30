# Allen's presence: what the sources say, and what we built from them

Research note for the Allen window in the test-computer kernel
(`native/kernel/ui/`), written against primary sources. Each claim names the
source that owns it. Where a source says nothing, that is said too.

Question asked: *how do you think outside the box, and does speaking a thing
into existence make it come true?* Then: *what should the interface of an
agent look like if it is not chat bubbles, tabs and endless scrolling?*

## 1. Speaking things into existence

### 1.1 Words that do things (Austin, 1962)

J. L. Austin, *How to Do Things with Words* (the 1955 William James
Lectures, Oxford University Press 1962), Lecture I: some utterances do not
describe anything and are not true or false; saying them *is* doing the
thing. His examples: "I do" at a wedding, "I name this ship the Queen
Elizabeth", "I bet you sixpence". He calls these **performatives**. Lecture
II lists the conditions under which a performative works: there must be an
accepted convention, the right people and circumstances, and the procedure
must be carried out completely and correctly, or the act "misfires".

What this owns: within a convention, a sentence can bring a state of affairs
into being. Outside a convention it cannot. Naming a ship works because a
whole practice stands behind it.

### 1.2 Saying the plan makes the plan happen more often (Gollwitzer, 1999)

Peter M. Gollwitzer, "Implementation intentions: Strong effects of simple
plans", *American Psychologist* 54(7), 1999, pp. 493 to 503. An
**implementation intention** is a sentence of the form "when situation X
arises, I will do Y". Across the studies reviewed, people who formed such a
sentence completed the intended action far more often than people who only
held the goal. The paper's explanation: the sentence hands control of the
action to the situation, so it starts without a fresh decision.

The 2006 meta-analysis (Gollwitzer and Sheeran, "Implementation intentions
and goal achievement", *Advances in Experimental Social Psychology* 38, pp.
69 to 119) puts the effect across 94 studies at a medium to large size
(d = .65).

What this owns: spoken or written intentions change what the speaker does
next. They do not act on the world directly; they act on the speaker.

### 1.3 What the sources do not say

No primary source consulted claims that speech changes physics. The claim
"speaking it into existence makes it come true" is true in two honest
senses and false in one:

- true as a performative: inside a convention, saying it makes it so
  (Austin);
- true as an intention: saying the plan out loud makes you carry it out
  more often (Gollwitzer);
- false as magic: no measured effect of words on events the speaker cannot
  reach. DOCS SILENT on any such effect.

### 1.4 How Allen uses this

Inside Allen's window every sentence is a performative, because the
convention is built in. "Call yourself Nova" makes him Nova, on disk, from
then on (`svc/allen_reply.c`, `svc/allen_memory.c` version 3). "Remember my
car is red" makes it remembered. "Goodbye" puts the computer to sleep.
Nothing is a request that goes into a queue; the saying is the doing, and
the window shows the doing (the line rises into him, the water turns, he
answers). That is the honest version of speaking things into existence:
build the convention so the words have somewhere to land.

## 2. Thinking outside the box

"Outside the box" is a phrase from the nine-dot puzzle (join nine dots in a
three-by-three grid with four straight strokes without lifting the pen; the
strokes must leave the square the dots suggest). The puzzle is in Sam Loyd's
*Cyclopedia of Puzzles* (1914, p. 301, as "Christopher Columbus's egg
puzzle"). The experimental literature on it, Weisberg and Alba, "An
examination of the alleged role of 'fixation' in the solution of several
'insight' problems", *Journal of Experimental Psychology: General* 110(2),
1981, pp. 169 to 192, found that telling people the strokes may leave the
square helped very little by itself; what helped was experience with the
specific move. The lesson the source owns: the box is not a mental habit you
can be talked out of; it is a lack of the concrete move. New moves come from
building and trying, not from exhortation.

For the interface this meant: do not redesign chat; build the thing that is
not chat and see what it needs. Section 3 is what it needed.

## 3. An interface that is not chat

### 3.1 Calm technology (Weiser and Brown, 1996)

Mark Weiser and John Seely Brown, "The Coming Age of Calm Technology",
Xerox PARC, 5 October 1996 (later in *Beyond Calculation*, Springer 1997).
Calm technology "engages both the center and the periphery of our
attention, and in fact moves back and forth between the two". Their
examples: an inner office window, and Natalie Jeremijenko's Dangling String,
a plastic cord that twitches with network traffic. Three claims they make:
information should live in the periphery until needed; moving it to the
centre and back should be easy; the periphery is where you can "attune to
many more things".

Allen's window is built on this. The sky, the galaxies and the water are the
periphery: always moving, never demanding. Allen himself is the centre, and
a typed line moves attention there and back without a mode change. There is
one bar, one voice, no tabs and no history to scroll: what was said rises
into him and is kept by him (notes, memory), not stacked on the screen.

### 3.2 Smooth change, not cuts

Every change of state in the window is a movement over time with an eased
curve (`toward()` and the fluid weights in `ui/allen_ui.c`, `ui/flow.c`):
the dusk when sleeping (section 3.3) and the dawn when waking (0.9 s), the shift
between idle, listening, thinking and speaking as a blend of weights over
0.7 s, the words rising and fading. No frame is a cut. The requirement came
from the brief (transitions "part of the experience"); the method is
Weiser and Brown's periphery: a cut forces attention to the centre, a flow
does not.

### 3.3 Goodbye: the event horizon

"Goodbye" is the one performative that ends the session, and the brief asked
that it be seen: everything on the screen drawn into the black hole in the
middle, the accretion disk at its most beautiful, then black, then the panel
off. The dusk (`horizon_at()` in `ui/allen_ui.c`, `ck_night_fall()` and
`ck_night_base_from()` in `ui/night.c`) lasts 4.2 s and is built from three
things the sources own:

- **Where the light falls from.** The picture on the screen at the moment
  of goodbye is kept (with copies at half, quarter and so on down to a
  thirty-second of its size, so that light crowded a hundredfold is averaged
  rather than picked at random). A pixel at distance r from the hole then
  shows what was at sqrt(r^2 + F), F growing from 0 to the square of the
  distance to the farthest corner by 3.4 s. That is free fall with a speed
  proportional to 1/r (the radial part of a drain), integrated: r^2 falls
  linearly in time, so the sky barely stirs far out and rushes near the
  hole. The turn is a fixed part of a turn per doubling of the distance
  fallen from (angular momentum kept: the azimuthal speed also goes as 1/r,
  and its integral is a logarithm), eased off further out so the edges of
  the picture do not visibly rotate. Past the screen's edges the sky is
  mirrored, so stars keep arriving and no edge is seen falling in.
- **Why it brightens.** Light that has been crowded in is drawn brighter
  (up to four times, warmer as it brightens), so the inner spiral glows as
  the matter arrives: the disk is made of the picture. On top of it a smooth
  disk is added by distance from the shadow: dark inside 1.15 shadow radii
  (the gap between a black hole's photon ring and the inner edge of its
  disk), brightest at 1.5, dying away by 3.3, warm white at the inner edge
  (255, 222, 180) through gold (255, 212, 150) to the blue of the sky's own
  glow (150, 170, 255), brighter on the side turning toward the eye (the
  Doppler beaming every picture of an accretion disk shows), with a thin
  bright photon ring at 1.06 shadow radii.
- **How it ends.** From the disk's height (2.8 s) the picture's light is let
  go, quickly at first and gently at the end (1 - (1 - t)^3), the edges of the
  screen ahead of the middle; the disk dims from 3.1 to 3.9 s while the shadow
  grows over it; at 4.2 s every pixel is black and nothing moves. Waking is
  the plain 0.9 s dawn: the sky returns, there is no spectacle in reverse.

The lens machinery of the startup's merging holes (`lens_at()`) bends the
light round the shadow as before. Everything is integer: distances come
from 2^(log2(r^2) / 2) with the existing log table and a 256-entry table of
2^(k/256), so no square root is taken per pixel. The whole screen moves in
every frame of the dusk; near the hole every pixel is worked out, further
out two at a time each way. Receipts: the host window test
(`scripts/ck_allen_ui_test.sh`) renders six frames through the dusk and the
closed horizon (`target/native-kernel-allen-ui/allen-8-horizon-*.png`),
checks that after the disk's height the light only falls, that the middle
is brighter than the edges mid-dusk and that the last frame is pure black,
and prints the dusk's own frame times next to the whole run's (the numbers
of the run that landed this are in the branch's handoff).

## 4. The water: Stam's stable fluids

Jos Stam, "Stable Fluids", *SIGGRAPH 99*, pp. 121 to 128, and "Real-Time
Fluid Dynamics for Games", Game Developers Conference, March 2003. The 2003
paper gives the whole solver in about a page of C: `add_source`, `diffuse`
(Gauss-Seidel relaxation, unconditionally stable), `advect` (semi-Lagrangian:
trace each cell's centre back along the velocity and sample), `project`
(a Poisson solve for pressure, subtracted so the field is divergence-free),
with `set_bnd` for the walls, and the velocity step
diffuse, project, advect, project, then the density step diffuse, advect.

`ui/flow.c` is that solver in 16.16 fixed point (the kernel has no floating
point in the drawing path), on a 64 by 36 grid with a one-cell border, ten
relaxation sweeps, stepped every 16 ms with catch-up and a cap of eight
steps per frame. Three things are ours, not Stam's: the forces (an idle
turn with a breath, a thinking vortex that deepens over four seconds, a
listening current from the bar up to Allen, rings when words come), the ink
sources (a trickle from the heart, a ring round him while he thinks, motes
from nowhere so the water is never empty), and the colour (cool blue to warm
white by local speed). Receipts: the host window test
(`scripts/ck_allen_ui_test.sh`) prints the frame times; with the water, one
host core draws a 3840 by 2160 frame in 17.7 ms on average against 9.2 ms
without it (run of 2026-10-18, 3912 frames). The kernel draws with three
cores in strips.

## 5. Allen and the far-off worker

Allen does not become a language model. He stays what he is on this
computer (greetings, memory, notes, facts, by heart) and, when a line is
beyond him and a courier is ready, he writes the job himself
(`svc/allen_intent.c`): what is wanted (make, explain, do, chat), who he is,
who asked, and the shape the answer must take, then keeps thinking (the
vortex deepens) until the answer comes back and he says it. The worker never
speaks to the person; Allen does. The courier is a line each way on a wire
(`svc/courier.h`); the first wire is the second serial port
(`x86/courier_x86.c`, PC16550D datasheet: LSR bit 0 Data Ready, bit 5 THRE),
relayed on the host by `crates/allen-courier` (standard library only) to a
chat server. Ethernet follows once the card may receive (aienos#411).

This is the prompting Allen was taught: not a template he fills, but the
three parts of a good instruction, which the sources above also point at.
Say who is speaking and to whom (Austin's convention), say the shape of the
act wanted (Gollwitzer's "when X, do Y"), and keep the periphery quiet
(Weiser and Brown): short sentences, no lists, no headings, in his voice.

## Sources

- Austin, J. L. *How to Do Things with Words*. Oxford University Press, 1962
  (Lectures I and II).
- Gollwitzer, P. M. "Implementation intentions: Strong effects of simple
  plans." *American Psychologist* 54(7), 1999, 493 to 503.
- Gollwitzer, P. M. and Sheeran, P. "Implementation intentions and goal
  achievement: A meta-analysis of effects and processes." *Advances in
  Experimental Social Psychology* 38, 2006, 69 to 119.
- Loyd, S. *Cyclopedia of Puzzles*. 1914, p. 301.
- Weisberg, R. W. and Alba, J. W. "An examination of the alleged role of
  'fixation' in the solution of several 'insight' problems." *Journal of
  Experimental Psychology: General* 110(2), 1981, 169 to 192.
- Weiser, M. and Brown, J. S. "The Coming Age of Calm Technology." Xerox
  PARC, 1996.
- Stam, J. "Stable Fluids." *SIGGRAPH 99*, 121 to 128.
- Stam, J. "Real-Time Fluid Dynamics for Games." GDC 2003.
- National Semiconductor, *PC16550D Universal Asynchronous
  Receiver/Transmitter with FIFOs*, Table II and section 8.5 (recorded with
  `docs-read` on 2026-10-18).
