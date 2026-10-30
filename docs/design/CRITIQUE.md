# Allen against 54 design systems: what we take, what we refuse

Read 2026-10-20 from the 54 DESIGN.md templates in the `popular-web-designs` skill. Every value below is
quoted from those files (three readers, one third each; seven quotes spot-checked by grep, all found). Allen's
screen is one night sky with one light in it, so most of what these systems do to sell something is refused.

## What changed in Allen because of this

1. **Lit is a glow, not a fill.** NVIDIA's "a signal, not a surface" and Framer's two-state pill agree with
   Allen's lit choice: a glow-blue gradient at about a third strength, a brighter hairline, a soft halo.
   Linear's solid indigo fill was the alternative and was refused.
2. **Edges are hairlines, depth is a step in surface colour.** Vercel and Cal draw borders as a 1 px ring at
   8 to 10 % opacity; Linear, Composio and Supabase step surfaces instead of casting shadows; HashiCorp says
   "If you can see the shadow, it's too strong." Allen's choices use a 1 px glow ring at 10 %, lit at 45 %.
3. **Muted text is a colour, never a lower alpha.** Superhuman, x.ai and Composio fade secondary text with
   alpha. Allen measured that: the kernel's hint at 150/256 alpha falls to 3.26:1 on night (floor 4.5). So
   hint and faint are solid colours (7.73:1 and 6.02:1). This is the one place we go against the majority.
4. **One weight family, hierarchy by size.** Mistral ("Hierarchy comes from size and color, never weight")
   and Runway use one face for every role. Allen has one stroke face at three sizes.
5. **Pills for actions, rounded rectangles for choices.** Apple (980 px), Revolut and Ollama (9999 px) keep
   pills for anything pressed. Allen: buttons and the bar are pills, the three choices are 14 px rounded rows
   so their second line of words has room.
6. **Slow on purpose.** Cursor's 150 ms and OpenCode's 100 to 150 ms are the fastest colour changes in the
   set; Ollama says "interactions should feel instant". Allen's word rise is 600 ms and its state blend
   700 ms, and only the lit choice moves fast (100 ms) because it follows a key press. VoltAgent's glow that
   "expands and contracts like a heartbeat" (2 px to 8 px) is the one other system that breathes.

## Refused across the board

Red and green for state (IBM, Stripe, Supabase, Kraken, OpenCode, Miro, VoltAgent), a second accent colour
(Sanity, Zapier, PostHog, Sentry, Cursor), buttons that grow or shrink (Wise, Notion, Intercom, Clay), all
caps and wide tracking (SpaceX, Supabase, Warp, x.ai, BMW), heavy or tight display type (Expo, Framer,
Vercel, ClickHouse), photos or video as the background (Runway, Warp), light slabs breaking the night
(Apple, Superhuman, Mintlify), and a black focus ring on near-black (Resend).

## The 54, one line each

| System | Take | Refuse |
| --- | --- | --- |
| Airbnb | off-black text, never pure | hot red CTA, 3-layer shadow |
| Airtable | nothing | blue glare shadow |
| Apple | 2px solid focus in the accent; pills | light slabs, blurred bar |
| BMW | weight 300 question, 1.30 line | caps, weight 900, square corners |
| Cal | border as 1px ring at 8-10% | inset bevel on buttons |
| Claude | 1.60 body line, warm second family | terracotta CTA, illustrations |
| Clay | nothing | tilting, jumping hover |
| ClickHouse | lit item marked by 1px accent border | neon on pure black, weight 900 |
| Cohere | one large container radius (22px) | purple bands, red focus ring |
| Coinbase | pill CTA, 2px focus outline | whole-fill colour swap on hover |
| Composio | border ladder 0.04 to 0.12 | cyan halos, hard offset shadow |
| Cursor | colour 150ms, shadow 200ms (floor to exceed) | crimson hover, heavy shadow |
| ElevenLabs | light 300 display, airy tracking | caps buttons, warm shadows |
| Expo | nothing new (pill CTA) | -1.6 to -3px tracking, 700-900 |
| Figma | a focus marker that is not colour | rainbow gradient |
| Framer | frosted unlit pill, solid lit pill | -5.5px tracking, 0.85 line |
| HashiCorp | shadows at 0.05 or none | four product accents |
| IBM | 2px input edge that changes on focus | 0px radius, red and green |
| Intercom | short state words | scale bounce, green press |
| Kraken | state as a tint behind the word | green success badge |
| Linear | surface steps 0.02-0.05; off-white text | solid indigo fill for lit |
| Lovable | soft wide focus glow | generic blue ring |
| MiniMax | unchosen at 0.5, chosen solid | pink and purple gradients |
| Mintlify | pill input with hairline | bright cloud wash |
| Miro | nothing | five pastels plus green |
| Mistral | hierarchy from size and colour | five-layer amber shadow |
| MongoDB | accent once per section | neon green underline |
| Notion | depth felt, not seen | scale bounce |
| NVIDIA | lit as signal, not surface | sharp corners, caps |
| Ollama | 9999 interactive, 12 containers | "feel instant" |
| OpenCode | 800-900px reading column, 1.50 | red/green, 100-150ms |
| Pinterest | nothing | red fill button |
| PostHog | hover by opacity, not new colour | orange and gold flashes |
| Raycast | +0.2px tracking on small light text | 3D key caps |
| Replicate | dotted underline for pressable words | blaze gradient, 128px type |
| Resend | 1px frosty border on dark pills | black focus ring on dark |
| Revolut | pill 9999, quiet 0.125rem focus | 136px billboard headline |
| Runway | one face, size-only hierarchy | photo and video backdrop |
| Sanity | depth by surface colour step | electric blue hover, coral |
| Sentry | content glowing out of the dark | lime, pink, caps |
| SpaceX | ghost pill, starlight text #f0f0fa | all caps |
| Spotify | pill text field, 1px inset ring | heavy shadow, green |
| Stripe | light weight, aligned digits | tinted shadows, green badges |
| Supabase | border ladder, no shadows | emerald, mono caps |
| Superhuman | state by opacity and underline | purple into white page |
| Together AI | 1px 0.12 white border on glass | pastel gradients, magenta |
| Uber | one selected pill inverts | pure black on pure white |
| Vercel | 1px ring at 8%, 2px focus | -2.4px display tracking |
| VoltAgent | slow glow pulse 2px to 8px | emerald glow, coral danger |
| Warp | focus as brighter border; warm off-white | caps micro-labels, photos |
| Webflow | nothing | six accents, five-layer shadows |
| Wise | 1px inset ring on input focus | grow and shrink buttons |
| x.ai | borders 0.1 to 0.2 on activation | 0px corners, mono caps |
| Zapier | big choice rows, 20px 24px padding | orange CTA and focus |
