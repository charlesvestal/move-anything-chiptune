# Chiptune for Move Anything

NES and Game Boy chiptune synthesizer for [Move Anything](https://github.com/charlesvestal/move-anything).

## Emulation

Two classic sound chips running as cycle-accurate emulations:

**NES 2A03** (Ricoh RP2A03, as in the Nintendo Entertainment System):
- 2 pulse-wave channels with 4 duty cycles (12.5%, 25%, 50%, 75%)
- 1 triangle-wave channel (fixed waveform, no volume control)
- 1 noise channel (white noise and metallic/periodic modes)
- Emulated by [Nes_Snd_Emu](https://github.com/jamesathey/Nes_Snd_Emu) (Shay Green / blargg), a band-limited synthesis library that generates alias-free output at any sample rate

**Game Boy DMG** (Sharp LR35902 APU, as in the original Game Boy):
- 2 pulse-wave channels with 4 duty cycles (12.5%, 25%, 50%, 75%)
- 1 programmable wave channel (32-sample 4-bit wavetable)
- 1 noise channel
- Emulated by [Gb_Snd_Emu](http://www.slack.net/~ant/libs/audio.html#Gb_Snd_Emu) (Shay Green / blargg), using the same band-limited synthesis approach as the NES library

Both libraries use Blip_Buffer for band-limited sample generation, producing clean audio without the aliasing artifacts typical of naive square-wave synthesis. Register writes are sample-accurate.

## Features

- 32 presets (16 NES, 16 GB) covering leads, pads, bass, percussion, and FX
- Up to 4-voice polyphony with automatic voice allocation
- ADSR envelope per voice
- Vibrato with configurable depth and rate
- Pitch bend support
- 8 programmable GB wavetables (sine, saw, triangle, square, pulse, staircase, metallic, bass)
- Works standalone or as a sound generator in Signal Chain patches

## Prerequisites

- [Move Anything](https://github.com/charlesvestal/move-anything) installed on your Ableton Move
- SSH access enabled: http://move.local/development/ssh

## Install

### Via Module Store (Recommended)

1. Launch Move Anything on your Move
2. Select **Module Store** from the main menu
3. Navigate to **Sound Generators** > **Chiptune**
4. Select **Install**

### Build from Source

Requires Docker (recommended) or ARM64 cross-compiler.

```bash
git clone --recursive https://github.com/charlesvestal/move-anything-chiptune
cd move-anything-chiptune
./scripts/build.sh
./scripts/install.sh
```

## Controls

| Control | Function |
|---------|----------|
| Jog wheel | Browse presets |
| Knobs 1-4 | Attack, Decay, Sustain, Release |
| Knobs 5-8 | Duty, Vibrato Depth, Vibrato Rate, Volume |

## Presets

### NES

| # | Name | Description |
|---|------|-------------|
| 0 | NES Lead | Warm 50% pulse lead |
| 1 | NES Bright | Punchy 25% pulse |
| 2 | NES Thin | Thin 12.5% pulse |
| 3 | NES Pad | Slow attack pad with vibrato |
| 4 | NES Pluck | Short pluck |
| 5 | NES Stab | Quick stab |
| 6 | NES Poly | 3-voice polyphonic (50%) |
| 7 | NES Poly Bright | 3-voice polyphonic (25%) |
| 8 | NES Unison | 2-voice detuned unison |
| 9 | NES Brass | Slow attack brass |
| 10 | Tri Bass | Triangle wave bass |
| 11 | Tri Kick | Triangle with pitch drop |
| 12 | NES Bell | Long decay bell |
| 13 | NES Hat | Short noise hi-hat |
| 14 | NES Snare | White noise snare |
| 15 | NES Zap | Noise with pitch drop |

### Game Boy

| # | Name | Description |
|---|------|-------------|
| 16 | GB Lead | Warm 50% pulse lead |
| 17 | GB Bright | Punchy 25% pulse |
| 18 | GB Thin | Thin 12.5% pulse |
| 19 | GB Poly | 3-voice polyphonic (50%) |
| 20 | GB Poly Bright | 3-voice polyphonic (25%) |
| 21 | GB Unison | 2-voice detuned unison |
| 22 | GB Vibrato | Lead with vibrato |
| 23 | GB Pluck | Short pluck |
| 24 | GB Pad | Slow attack pad |
| 25 | Wave Bass | Wavetable bass |
| 26 | Wave Pad | Wavetable pad with vibrato |
| 27 | Wave Sub | Deep sub bass |
| 28 | Wave Growl | Aggressive wavetable |
| 29 | Wave Metal | Metallic wavetable |
| 30 | GB Brass | Slow attack brass |
| 31 | GB Bell | Long decay bell |

## Credits

- **Nes_Snd_Emu**: [Shay Green (blargg)](http://www.slack.net/~ant/) / [jamesathey fork](https://github.com/jamesathey/Nes_Snd_Emu) (LGPL-2.1)
- **Gb_Snd_Emu**: [Shay Green (blargg)](http://www.slack.net/~ant/libs/audio.html#Gb_Snd_Emu) (LGPL-2.1)
- **Move Anything port**: Charles Vestal

## License

MIT License (plugin code) / LGPL-2.1 (blargg libraries)
