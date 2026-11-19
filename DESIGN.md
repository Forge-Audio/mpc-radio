# MPC Radio - design

## The sound

**The idea in one sentence.** A real FM dial inside the plugin: turn it and you sweep through hiss,
crackle, sliding whistles and half-heard ghosts until a live internet station snaps in perfectly
clean, and then you choose what kind of speaker it is coming out of.

The radio is **always live**. It used to have a second band of stations the plugin made itself, chosen
with a Live switch; the musician asked for "just live", so that band no longer sounds. Its code and
its three controls (Live, Key, Sync) are still in the source, in their old places, so that projects
saved with the earlier version load. Live is no longer read, and Key and Sync have nothing left to act on.

### The dial (87.5 to 108.0 MHz)

Sound from the internet comes in through MP Forge's own stream (`online.json`, `mpforge::AudioStream`).
The plugin's code never uses the network itself. Only one stream can play at a time.

| MHz | On the dial | Station |
|---|---|---|
| 88.6 | SOUL | SomaFM Seven Inch Soul |
| 90.4 | HIPHOP | SomaFM Fluid |
| 92.2 | FUNK | FIP Groove |
| 94.0 | REGGAE | SomaFM Heavyweight Reggae |
| 95.8 | JAZZ | FIP Jazz |
| 97.6 | AMBIENT | SomaFM Drone Zone |
| 99.4 | ROCK | Radio Paradise Rock Mix |
| 101.2 | HOUSE | SomaFM Beat Blender |
| 103.0 | WORLD | Radio Paradise World/Etc Mix |
| 104.8 | TALK | NPR Program Stream |
| 106.6 | CLASSIC | France Musique Classique Plus |

CLASSIC used to be Radio Swiss Classic. Its published address only passes the listener on to
another address (a 302 to one of several numbered servers), and MP Forge's stream asks an address
once and does not follow it, so the station stayed in static. Radio France's addresses are the
stream itself, as FUNK and JAZZ already show, so CLASSIC now sits on Radio France's non-stop
classical channel. Its place in `online.json`, its name and its frequency are unchanged.

- **Tuning.** The station nearest the dial is the one wanted. It is opened once the dial has stayed
  near it for a quarter of a second, so sweeping the dial does not open every station it passes.
  Its clean, fringe and ghost gains come from its distance to the dial (clean inside +-0.12 MHz, gone
  by 0.45, ghost reaching 0.85), so by the time another station is nearer, this one is already silent
  and the change cannot click.
- **Arriving.** A stream takes a second or two to fill. Until its sound is really playing its level
  is zero, so the air (hiss, crackle, whistle) carries on as if still between stations, and then the
  station fades in over about 200 ms and the air closes. If a stream drops, the static comes back.
- **Level.** A stream can arrive at full scale, so it is taken down 6 dB, and then runs through the
  ghost bus, air, Wow, Drive, Speaker, Echo, Room, Volume and final clipper.
- **With no network** (and while MP Forge checks the plugin) the radio is static and whistles only.

### Signal flow, one line per stage

1. **Tuner.** Tune is glided (about 60 ms) into a dial position. The distance from the dial to the
   nearest station gives three gains: *clean* (1 inside +-0.12 MHz, gone by 0.45), *fringe* (the
   station thin and gritty just off its frequency) and *ghost* (set by Ghosts, fading in and out).
2. **Station.** The one live stream nearest the dial, read for the whole block on every call.
3. **Ghost bus.** Fringe and ghost signals are summed to mono, band-passed (500 Hz to 2.8 kHz)
   and bent with a gentle tanh, so a ghost never sounds like the clean station.
4. **Air.** Hiss (filtered stereo noise) and crackle (random pops) scaled by Static; a whistle whose
   pitch falls to nothing as you close in on the station, scaled by Squeal. All of it is multiplied
   by "how far from locked", so a locked station is silent air: transparent.
5. **Wow.** A short delay swept by two slow waves and one fast one: tape pitch wobble. Off = bypassed.
6. **Drive.** tanh overdrive with the output turned down as the gain rises. Off = bypassed.
7. **Speaker.** Six speakers, chosen with Speaker Type; the Speaker knob says how far the sound goes
   towards the chosen one. See "The speakers" below. Off = bypassed.
8. **Echo.** Stereo delay, send-style, time glides like tape, repeats filtered by Echo Tone and a
   120 Hz high-pass, soft clip in the loop.
9. **Room.** Small four-line feedback network (about 0.8 s) for "a radio in a kitchen".
10. **Volume**, and a clipper that is a straight wire below 0.9 and only rounds off rare peaks.
    The musician did not need Mix, so the output is always all radio: the Mix control is still in the
    source, in its old place, for saved projects, but it is on no page and is no longer read.

### The speakers

One chain for every type: 12 dB low cut -> cone grit (a gentle tanh, level held) -> 12 dB high cut,
which also takes the grit's harsh top off -> a body resonance (a band-pass added back in) -> the two
sides folded towards one -> level made up. Each type is one row of numbers for that chain with
Speaker at 100 %. Lower down, the two corners sit part of the way there from full range (20 Hz and
20 kHz), and the resonance, the fold to mono and the grit grow in step with the knob.

| Type | Low cut | High cut | Body resonance | Stereo | Grit | What is heard |
|---|---|---|---|---|---|---|
| Pocket | 380 Hz | 3.2 kHz | none | stereo | none | a small plastic transistor radio: no bass and no top. The speaker the radio always had, number for number |
| Boombox | 55 Hz | 11 kHz | +5 dB at 95 Hz | stereo | a little | big woofers: a thump under everything and a bright top |
| Car | 70 Hz | 5.2 kHz | +6 dB at 140 Hz | narrowed | a little | door speakers: boomy, dull on top, close |
| Phone | 420 Hz | 2.9 kHz | +6 dB at 1.9 kHz | mono | medium | a telephone earpiece: hard, nasal, small |
| Wood | 110 Hz | 4.2 kHz | +5 dB at 210 Hz | mono | some | an old cabinet set: boxy and warm |
| Horn | 650 Hz | 3.6 kHz | +8 dB at 1.25 kHz | mono | heavy | a tannoy horn: honking and overdriven |

- **Changing type** does not switch anything. Every number of the chain glides to the new speaker's
  over about a tenth of a second, so the sound reshapes and cannot click.
- **Loudness.** Each type has its own make-up level, set so that turning Speaker up or changing the
  type changes the tone and not the volume. The grit is a tanh with its output turned down as its
  gain rises, and it sits before the high cut. The grit is dirt on purpose.
- **Worst case.** Low and high cut resonance never pass 0.8, the body resonance never passes 0.9, and
  every frequency is clamped before it reaches a filter. A full-scale tone exactly on a body
  resonance is rounded by the cone grit before it and by the final clipper after it.

### Controls

| Control | Range | What it does |
|---|---|---|
| Volume | -40 to +6 dB | master level, nothing else |
| Tune | 87.5 to 108.0 MHz, 0.1 steps | the dial. Three clicks either side of a station are still clean |
| Static | 0 to 100 % | hiss and crackle between stations. 0 = dead silence between stations |
| Squeal | 0 to 100 % | whistles that slide as you tune |
| Ghosts | 0 to 100 % | how far and how loud a neighbouring station bleeds between frequencies |
| Key (on no page) | C to B | kept for saved projects; nothing left for it to transpose |
| Sync (on no page) | Off / On | kept for saved projects; nothing left for it to time |
| Mix (on no page) | 0 to 100 % | kept for saved projects; no longer read, the output is always all radio |
| Speaker | 0 to 100 % | full range down to the chosen speaker |
| Drive | 0 to 100 % | overdriven amplifier, loudness held steady |
| Wow | 0 to 100 % | tape pitch wobble, up to about a third of a semitone |
| Room | 0 to 100 % | small room reverb |
| Echo | 0 to 100 % | echo level |
| Echo Time | 40 to 1500 ms | echo time; gliding it bends the pitch |
| Repeats | 0 to 95 % | echo feedback |
| Echo Tone | 500 Hz to 12 kHz | how dark the repeats get |
| Live (on no page) | Off / On | kept for saved projects; no longer read, the radio is always live |
| Signal (on no page) | 0 to 1 | set by the plugin: the signal lamps and the red live lamp follow it |
| Speaker Type | Pocket, Boombox, Car, Phone, Wood, Horn | which speaker the Speaker knob turns the radio into. Added last |

### Presets

The twelve places in the list are the same as before, so a project that remembers a preset by its
place still finds one there; the eight that pointed at the plugin's own stations now point at live ones.
There is no preset called Default; the first one, Late Night Jazz, holds every starting value.

**The screen follows a preset.** Choosing a preset moves the controls inside the plugin, but the MPC's
screen is not told, so the Tune dial and needle used to stay on the old station while the new one
played. About 50 ms after a preset or project has loaded, the plugin now reports where each control on
the screen stands (Tune first, then the rest, one per control tick), so the dials, the needle, the
slider and the speaker picture all move to the preset. It only reads and reports; no value is changed.

Late Night Jazz, Kitchen Hip Hop (Pocket), Between Stations (Wood), Soul On The Phone (Phone),
Dub Talk (Horn), Warped Classic, Clean House, Drift Off, Live Soul 45s (Wood), Live Dub Reggae
(Boombox), Live Kitchen Talk (Pocket), Car Radio Rock (Car).

### What moves by itself, what the hands move

- By itself: the broadcast, ghosts fading in and out, crackle density, whistle wobble, the Wow waves.
- The musician asked for a dial that *stays locked*: the tuning never drifts. Only the hand moves it.
- The hands: Tune is the instrument. Sweeping it is a performance and is worth sampling on its own.

### How it listens

- It does not listen to the track it sits on: the output is all radio. The radio itself does not follow
  the song: a broadcast cannot be transposed or re-timed.

### Kept clean, and where it is dirty on purpose

- Every gain is ramped per 16 samples or smoothed per sample; delays are read between samples.
- Crackle is clicks by design. The ghost bus, Drive and the speakers' cone grit distort by design,
  all with a gentle curve.
- Feedback in Echo is at most 0.95 with a soft clip in the loop; the Room network loses 20 % per pass.

## The look

**The idea in one sentence.** The front of a mid-90s portable radio: pale putty plastic, a long
smoked-glass tuning window lit mint green with a red needle, a round perforated speaker, chunky dials,
and one loud squiggle of teal and magenta straight off a 1994 paper cup.

### Colours

| Name | Hex | Job |
|---|---|---|
| Putty | `#cfcbbf` | the ground: the radio's plastic body |
| Shadow putty | `#a9a598` | recesses, lower edges, the grille's holes rim |
| Smoked glass | `#15191c` | the tuning window and the wells the controls sit in |
| Dial mint | `#9ff2c9` | lit scale, station names, values |
| Needle red | `#ff3b4e` | the one loud colour: needle, signal lamps, the live lamp, name-plate stroke |
| Teal | `#18a3a8` | quiet accent: the kit's dials, the squiggle |
| Grape | `#6a3fa0` | second graphic accent: triangles and stripes |
| Ink | `#24262b` | lettering on the putty |

Walnut (`#a06c42` to `#5c3a22`) is used once only, for the wooden cabinet in the speaker bay.

### Fonts

- **Futura (bold)** for the name plate and section titles: the face of 90s consumer electronics.
- **DIN Condensed** for control names, the dial's numbers and the station names: narrow, engineered,
  like silk-screen print.
- Values stay in the MPC's **Titillium Web**: ink on the putty, and dial mint for the two readouts
  (Tune and Speaker Type), which each sit on their own strip of glass.
- All capitals everywhere.

The name plate reads RADIO, not the plugin's full name, because the screen must not carry the word "MPC".

### Treatment

- **Knobs.** Three sizes: Tune is the hero (160 px) in a ringed well with tick marks and a glass
  readout beside it; the air and effect knobs are medium (104 to 110 px); Speaker Type is small
  (76 px) with a glass readout beside it like Tune's. Every dial sits in a sunk well with a tick
  ring. Name lettered under each medium dial, value under the name.
- **Fader.** Volume is one long boombox slider (566 px) across the whole OUTPUT section, drawn by
  hand in place of the kit's fader: a brushed-aluminium plate with a printed tick scale, a black slot,
  and a chunky black ribbed cap on a chrome carrier with a cream index line. Its scale has a tick every
  5 dB, a red mark at 0 dB and a red zone above it. It is a picture that follows the control
  (`volume-slide`, 64 frames) and sits in front of the kit's fader, which stays underneath and still
  does the work. Mix had a matching slider beside it until the musician said it was not needed.
- **Aluminium** `#d8d5cb` (fading `#eeece4` to `#bebbb0`) is the plates' colour: a warm silver one
  step lighter than the putty.
- **Kit.** `mono` in teal `#18a3a8`; the focus ring is needle red.
- **Backdrop.** MAIN is the front of the radio: tuning window across the top with the eleven live
  stations printed under the scale, speaker grille on the left, control deck on the right, one small
  red lamp in a sunk well right under the Volume slider, and four moulded vent slots under the Tune
  dial where the lamp used to be. FX is the same radio's lower deck: the speaker bay on the left
  (a glass window, a shelf, the Type dial and its readout under it), two sunk panels (SOUND, ECHO)
  on the right. Light comes from the top left: bright top edges, darker bottom edges, a soft fade
  down the body.
- **Live lamp.** The musician asked for the ON AIR sign and the LIVE title to go and for "just a red
  light". The picture is still called `on-air` (its name ties it to its place) but is now a round red
  jewel lamp in a chrome bezel. They then asked for it smaller, right underneath the output fader,
  with a small live label: it is drawn at 72 px (a 36 px bezel, half its old size), centred under the
  Volume slider between VOLUME and the value, with LIVE in DIN Condensed at 20 beside it. It follows the hidden Signal control
  (six frames): dark red between stations, a faint glow while a station is arriving, lit with a halo
  on the plastic once it plays.
- **Speaker bay.** `speaker-type` follows Speaker Type (six frames, one speaker each): a teal pocket
  radio with its aerial up, a charcoal boombox, a grape hatchback with music coming out of its door,
  a cream telephone handset on its own (it had a curly cord until the musician asked for it to go),
  a walnut cabinet set, a silver tannoy horn with a red rim.
- **What moves.** The red needle slides along the scale as Tune turns (64 frames). The five signal
  lamps and the red live lamp follow Signal, which the plugin sets from how well the dial is really
  locked. The slider cap moves with Volume. The speaker in the bay changes with its type.
- **The memorable thing.** The long lit tuning window with station names printed under the scale.
- **Left in the folder, not on the screen:** `live-dial`, `live-slide` and `sync-slide` belonged to
  the Live and Song Sync switches, and `mix-slide` to the Mix slider, which are gone.
