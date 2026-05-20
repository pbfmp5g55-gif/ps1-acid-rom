# ps1-acid-rom

PlayStation 1 native sequencer inspired by Propellerhead ReBirth RB-338 (1997).

**4 instruments on screen, one ISO:**

- 2× acid bass synth (TB-303-style) — DCO + envelope-modulated lowpass filter, accent/slide
- 1× analog drum machine (TR-808-style) — circuit-derived voices (BD/SD/CP/HH/CY/CB/Toms)
- 1× hybrid drum machine (TR-909-style) — analog BD/SD/Toms + sample HH/Cym

All circuits are derived from community-published reverse-engineering work
(x0xb0x, Eric Archer's 808 analysis, Yocto kit BOMs, etc.) — no Roland service
manuals copied. Original Roland patents have all expired (1980–1983 designs).

## Build

ISO is built by GitHub Actions. See `.github/workflows/build.yml`. Artifact:
`ps1-acid-rom.iso` and `ps1-acid-rom.ps-exe`.

Host-side DSP test harness (Windows/Linux) lives in `host_tests/` — lets you
hear each voice via wav output without touching PS1 hardware.

## Run

Burn the ISO to disc, or load `ps1-acid-rom.ps-exe` in pcsx-redux:

```
pcsx-redux.exe -bios openbios.bin -loadexe ps1-acid-rom.ps-exe -fastboot
```

## Integration with PS-VJ

Designed to plug into the `ps1-primitive-vj` / `ps1-vj-mix` pipeline:
sequencer events become primitive draws (step LEDs, knob positions),
which the VJ chain intercepts and transforms. Eventually patterns and
trigger events also drive MIDI clock out to the VJ params.

## License

MIT. The PSYQo framework and nugget toolchain are included as submodules
and retain their original MIT license.
