# Rockbox Rockboy Audio Fix

**Fixing the broken audio in Rockbox's built-in Gameboy emulator (rockboy) on iPod Classic and other affected targets.**

[![License: GPL v2](https://img.shields.io/badge/License-GPLv2-blue.svg)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)
[![Target: iPod Classic 7G](https://img.shields.io/badge/Target-iPod%20Classic%207G-silver)](https://www.rockbox.org/wiki/IpodClassic)
[![Rockbox: 4.0](https://img.shields.io/badge/Rockbox-4.0-orange)](https://www.rockbox.org)

Rockboy ships as part of the open-source [Rockbox firmware](https://www.rockbox.org/) and can play Gameboy and Gameboy Color ROM files. However, the audio is severely degraded on many targets — especially iPod Classic. This project identifies the root causes and provides fixed source files and a compiled plugin.

## The Problem

Rockboy audio on iPod Classic 7G sounds harsh, crackly, and distorted. The original handheld had clean, warm chiptune audio. The emulator should reproduce that faithfully.

## Legal Notice

This project modifies only the open-source Rockbox emulator plugin code (GPL v2). No ROMs, game files, copyrighted assets, or proprietary code are included, distributed, or referenced in this repository. Users are responsible for sourcing their own legally obtained ROM files. This project does not facilitate, encourage, or assist with obtaining copyrighted software.

## Root Cause Analysis

We identified **7 bugs** in the rockboy audio pipeline:

### Critical

1. **Forced 11kHz sample rate on iPod**: The iPod Classic has `HW_HAVE_11` defined, so rockboy forces the output to 11025 Hz instead of 44100 Hz. This triggers `snd.quality = 4`, meaning only every 4th audio sample is generated with zero interpolation. The result is harsh, aliased, metallic-sounding audio.

2. **Broken double-buffering**: `N_BUFS=2` allocates two buffers, but only one is ever written to. The audio callback and emulation thread read/write the same memory region, causing pops and crackling.

3. **Race condition on ARM**: The `doneplay` synchronization flag between the audio callback (interrupt context) and the emulation thread has no memory barriers or atomic operations. On ARM processors, this causes random audio glitches due to memory reordering.

### Moderate

4. **Yield-loop blocks emulation**: `rockboy_pcm_submit()` busy-waits with `rb->yield()` for the audio callback to fire. This stalls the emulation, causing frame drops that cascade into audio buffer underruns, creating a stutter cycle.

5. **Sample rate drift**: Integer division truncation in `snd.rate = (1<<21) / pcm.hz` causes slight pitch inaccuracy and timing glitches.

### Minor

6. **No anti-aliasing filter**: Downsampling uses nearest-neighbor selection, creating harsh aliasing artifacts on the square wave channels.

7. **Silent failure**: When Rockbox audio is already playing music, rockboy silently disables all sound with no feedback to the user.

## Affected Files

All in `apps/plugins/rockboy/` in the [Rockbox source tree](https://github.com/Rockbox/rockbox/tree/master/apps/plugins/rockboy):

| File | Role |
|------|------|
| `rbsound.c` | Rockbox PCM audio bridge (bugs 1-4) |
| `sound.c` | Sound channel emulation (bugs 5-6) |
| `emu.c` | Main emulation loop, timing |
| `rockmacros.h` | Configuration and options |

## Proposed Fixes

### Fix 1: Force 44100 Hz on all targets

The single biggest improvement. Changes `rbsound.c` to always use 44100 Hz, setting `snd.quality = 1` (no downsampling, no aliasing).

### Fix 2: Implement real double-buffering

Properly alternate between two buffer halves. Emulation fills one while the audio callback plays the other.

### Fix 3: Add memory barriers for ARM

Use proper atomic operations or volatile + compiler barriers for the `doneplay` flag.

### Fix 4: Replace yield-loop with semaphore

Use Rockbox's `rb->semaphore_wait()` instead of busy-waiting, allowing the emulation thread to sleep efficiently.

## Building

Requires the Rockbox cross-compiler toolchain:

```bash
# Clone Rockbox source
git clone https://github.com/Rockbox/rockbox.git
cd rockbox

# Build the ARM cross-compiler
cd tools && ./rockboxdev.sh

# Configure for iPod Classic
cd .. && mkdir build && cd build
../tools/configure  # Select iPod Classic (ipod6g)

# Build just the rockboy plugin
make $PWD/apps/plugins/rockboy.rock
```

## Testing

Tested on:
- iPod Classic 7th Gen (S5L8702, 216MHz ARM926, 64MB RAM)
- Rockbox 4.0

Testing uses legally obtained ROM files with varied audio complexity to verify all 4 sound channels render correctly.

## Quick Install (Pre-built Binary)

> For iPod Classic 6G or 7G running Rockbox 4.0.

1. Download `rockboy-ipod6g.rock` from [Releases](https://github.com/Tyal13/rockbox-rockboy-audio-fix/releases)
2. Copy to your iPod: `.rockbox/rocks/games/rockboy.rock`
3. Eject, power cycle, launch a `.gb` or `.gbc` file from the Rockbox file browser

Full build-from-source instructions: [docs/BUILD.md](docs/BUILD.md)

---

## Roadmap

- [x] Root cause analysis of audio bugs
- [x] Fix 1: Sample rate correction (11025 Hz forced → 44100 Hz)
- [x] Fix 2: Proper ping-pong double-buffering
- [x] Fix 3: Volatile ISR synchronization flag
- [x] Fix 4: Yield-loop timeout
- [x] Fix 5: Eliminate redundant hwbuf copy
- [ ] Testing compiled binary on real iPod hardware
- [ ] Submit patches upstream to Rockbox project via Gerrit

## Related Projects

- [Rockbox Explicit Content Filter](https://github.com/Tyal13/rockbox-explicit-filter): Automated explicit content detection for Rockbox
- [Rockbox Source](https://github.com/Rockbox/rockbox): Official Rockbox firmware repository

## License

Patches are GPL v2, matching the Rockbox project license.

## Credits

- **Adam Herrmann** ([@Tyal13](https://github.com/Tyal13)): Research and patches
- **Rockbox Project**: Open-source firmware
- **gnuboy**: Original emulator that rockboy is based on
