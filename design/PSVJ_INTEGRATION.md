# ps1-acid-rom ↔ ps1-vj-mix integration (M9 contract)

ps1-acid-rom is itself a PS1 program, so when it runs under the
`pcsx-redux` VJ fork it is **already** producing the same primitive
stream `ps1-vj-mix` is built to consume (the fork captures every GPU
operation and pushes it over the shared-memory ring). Step LEDs, voice
labels, cursor outlines — all of it ends up in the mixer.

What M9 adds is **a deterministic side-channel** the mixer can lock on
to synchronise its own VJ params (glitch chance, crossfade rate,
twin-self delay, etc.) to whatever the sequencer is currently doing.

## Side channel: the "sync stripe"

Every frame, ps1-acid-rom emits a row of four `Prim::Rectangle`s in the
top-right corner of the screen, off the play area, encoding the current
sequencer state:

| Stripe cell | x       | y | size  | color encodes                                  |
|-------------|---------|---|-------|------------------------------------------------|
| 0           | 304     | 0 | 4x4 | `R = playStep`, `G = chainLength`, `B = 0x00`   |
| 1           | 308     | 0 | 4x4 | `R = playingPattern`, `G = currentPattern`, `B = reverb_state` |
| 2           | 312     | 0 | 4x4 | `R..B = m_voiceIdx[0..2]` (303A / 303B / 808)   |
| 3           | 316     | 0 | 4x4 | `R = m_voiceIdx[909]`, `G = running ? 0xff : 0`, `B = 0x00` |

These cells are deliberately tiny and in the screen corner — the
sequencer UI ignores them, but the mixer side can recognise them via
their fixed positions in every frame buffer and read the encoded values
out of their colors.

## ps1-vj-mix side (TODO for the next session)

`ps1-vj-mix`'s mixer needs to:

1. **Recognise the stripe.** When attached to channel A or B, scan
   primitives with `position = (304..316, 0)` and `size = (4, 4)`.
   Stable across frames because we emit the stripe unconditionally
   each `frame()`.

2. **Decode + expose as a "psvj_clock" Param.** Make the decoded values
   available on the mixer side as a struct, e.g.

   ```c
   struct PsvjClock {
       uint8_t step;          // 0..15
       uint8_t chainLength;   // 1..8
       uint8_t playingPattern;
       uint8_t currentPattern;
       uint8_t reverbState;   // bit 0 = global enabled
       uint8_t voiceIdx[4];
       bool running;
   };
   ```

3. **Wire it into VJ params.** Suggested mappings (open for tuning):
   - `step` → drives `VJ_CHANCE` envelope (rises on accent steps)
   - `chainLength > 1` → enables `VJ_CHAOS`
   - `playingPattern` transitions (rising edge of `currentPattern`
     change while `running`) → trigger `VJ_TEXTURE_SWAP`
   - `reverbState` → modulates `VJ_DEPTH`
   - `running == false` → ramp `VJ_MASTER` to 0 (no chaos when stopped)

4. **MIDI clock out (optional, deferred).** The stripe is the
   in-band signal; if we want a real MIDI clock for external gear, the
   mixer would convert `step` increments into 24 PPQN ticks and push
   to a MIDI port. That's a `ps1-vj-mix` side feature, not ours.

## Why a sync stripe and not SIO / SPU sniff

- **SIO** would require `pcsx-redux` fork plumbing on its serial side;
  too much repo-cross overhead for a sync signal.
- **SPU key-on sniff** does already happen as a side effect (the mixer
  sees the SPU writes), but voice→event mapping is fragile when
  voices are reused across rows. The stripe is per-frame, explicit,
  and survives any future voice routing changes.

## Forwards-compatibility

If we need more state later (e.g. per-row volume, swing offset), add
new stripe cells *to the right* of cell 3 and bump a version byte in
cell 0's `B` channel (currently fixed at 0). The mixer falls back to
ignoring unknown cells.
