# LEDrum

A drum voice for [Sidecar](https://github.com/trigcondition/sidecar-firmware) (the Analog Rytm expander), as a Pi
user machine (`.rmp`). It makes kicks, toms, snares, hats and zaps from an oscillator with FM or additive mixing, a pitch
envelope, a multimode filter, an amp envelope with an adjustable curve, transient samples and distortion.

LEDrum is a **modified version** of the drum voice in the open-source Erica Synths LXR-02 firmware. It is **not
affiliated with, endorsed by or approved by Erica Synths**. "Erica Synths", "LXR" and "LXR-02" are used only to credit
where the code comes from.

- Id `ledrum`, version 0.1.0, plugin API 3 (dual package, API 2 and API 3)
- Board: Pi 5 (tested on a Pi 5 with the Touch Display 2)
- Licence: GPL-3.0-only
- Listed in the [Sidecar community machine registry](https://github.com/trigcondition/sidecar-machines)

## Install

Copy `ledrum-0.1.0-pi5.rmp` (from the registry) into `/Plugins` on the Sidecar's SD card and restart.

## Knobs

Pitch follows the trig note: a plain trig (note 60) plays at the TUNE pitch, other notes transpose it.

| # | Knob | Short | What it does |
|---|------|-------|--------------|
| 1 | Coarse Tune | TUNE | base pitch in semitones |
| 2 | Fine Tune | FINE | plus or minus one semitone |
| 3 | Waveform | WAVE | Sine, Tri, Saw, Rect, Noise, PWM |
| 4 | Mod Tune | MOD | pitch of the sine modulator |
| 5 | FM Amount | FM | FM depth, or the modulator level in Add mode |
| 6 | Osc Mix | MIX | FM or Add |
| 7 | Filter Freq | FREQ | cutoff, 30 Hz to about 19 kHz |
| 8 | Filter Reso | RESO | resonance |
| 9 | Filter Type | FTYP | Off, LP, HP, BP, UBP, Notch |
| 10 | Amp Attack | ATK | 0 is instant, up to about 300 ms |
| 11 | Amp Decay | DEC | 8 ms to 5 s |
| 12 | Amp Slope | SLP | curve of the decay |
| 13 | Pitch Decay | PDEC | 3 ms to 1.5 s |
| 14 | Pitch Amt | PAMT | pitch envelope depth, up to 72 semitones |
| 15 | Transient | TRAN | Off, Offset (a short pitch snap) or one of the 12 LXR transient samples |
| 16 | Distortion | DIST | waveshaper drive |

Knobs 9 to 16 are on the second page, LEDRUM MORE.

## Build

On Linux (or WSL on Windows) with the AArch64 cross-compiler:

```
sudo apt install gcc-aarch64-linux-gnu g++-aarch64-linux-gnu python3
bash build.sh
```

This makes `ledrum.a64.rmp`. The registry's `ledrum-0.1.0-pi5.rmp` was built with this script and
`aarch64-linux-gnu-gcc` 15.2.0 (Ubuntu 15.2.0-16ubuntu1); with that compiler the SHA-256 matches the registry's. Other
compiler versions make a working package with a different hash.

## What is in here

| Path | What | Licence |
|------|------|---------|
| `ledrum.c` | the drum voice and the plugin glue | GPL-3.0-only |
| `manifest.json` | knob names, ranges and defaults for the packer | GPL-3.0-only |
| `shim/` | small headers that let the LXR data files build outside the STM32 tree | GPL-3.0-only |
| `lxr/wavetable.c`, `lxr/transientTables.c` | the LXR wavetables and transient samples, unchanged | GPL-3.0-only, Erica Synths, see `lxr/NOTICE.md` |
| `sdk/`, `tools/rmp_pack.py` | the Sidecar plugin SDK headers, linker script, libc shims and packer, unchanged | GPL-2.0-or-later, trigcondition and contributors, see `sdk/README.md` |

## Credits and licence

Based on the drum voice of the Erica Synths LXR-02 firmware, Copyright (C) 2012-2026 Erica Synths, GPL-3.0-only with
the additional terms in `lxr/NOTICE.md`. Changes from the original: rewritten for float processing at 48 kHz, new amp
and pitch envelopes, a new state-variable filter and a new knob mapping. The LXR wavetables and transient samples are
used unchanged.

Sidecar port by amnmkkr. The whole is licensed under the GNU General Public License version 3 (`LICENSE`), with the
additional terms of `lxr/NOTICE.md` for the parts that come from the LXR firmware. Not affiliated with Erica Synths or
Elektron.

A user machine is native code that runs inside the Sidecar engine with no sandbox. Install it at your own risk.
