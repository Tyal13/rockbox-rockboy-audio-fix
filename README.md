# Rockbox Game Boy Emulator Project

**High-performance Game Boy emulator for iPod Classic running Rockbox, built from the ground up.**

[![License: GPL v2](https://img.shields.io/badge/License-GPLv2-blue.svg)](https://www.gnu.org/licenses/old-licenses/gpl-2.0.en.html)
[![Target: iPod Classic 7G](https://img.shields.io/badge/Target-iPod%20Classic%207G-silver)](https://www.rockbox.org/wiki/IpodClassic)
[![Rockbox: 4.0](https://img.shields.io/badge/Rockbox-4.0-orange)](https://www.rockbox.org)

This project started as a fix for Rockbox's broken rockboy audio, then evolved into a full replacement emulator (pgb) built on [Peanut-GB](https://github.com/deltabeard/Peanut-GB). The goal: native-speed Game Boy emulation with perfect audio on a 216 MHz ARM device with 64MB RAM.

<!-- TODO: Add hero photo of iPod running a game -->
![iPod Classic running pgb](assets/screenshots/hero.png)

## Legal Notice

This project modifies only open-source Rockbox emulator plugin code (GPL v2) and MIT-licensed Peanut-GB. No ROMs, game files, copyrighted assets, or proprietary code are included, distributed, or referenced in this repository. Users are responsible for sourcing their own legally obtained ROM files.

## Project Phases

### Phase 1: Rockboy Audio Fix (Complete, Deployed)

Fixed **7 bugs** in the original rockboy audio pipeline that caused harsh, crackly, metallic audio on iPod Classic:

| # | Severity | Bug | Impact |
|---|----------|-----|--------|
| 1 | Critical | Forced 11 kHz sample rate on iPod (`HW_HAVE_11` misread) | 75% of audio samples discarded |
| 2 | Critical | Broken double-buffering (only one buffer half used) | Constant crackling/pops |
| 3 | Critical | Non-volatile ISR flag on ARM (no memory barriers) | Random audio glitches |
| 4 | Moderate | Yield-loop blocks emulation thread | Frame drops cascade into audio underruns |
| 5 | Moderate | Sample rate drift from integer truncation | Pitch inaccuracy |
| 6 | Minor | No anti-aliasing filter on downsampled channels | Harsh aliasing artifacts |
| 7 | Minor | Silent failure when Rockbox audio already active | No user feedback |

**Result:** Clean, warm chiptune audio at 44100 Hz with proper double-buffering.

Fixed source files are in [`src/`](src/) and the compiled plugin in [`releases/rockboy-ipod6g.rock`](releases/rockboy-ipod6g.rock).

### Phase 2: PGB - New Emulator (In Progress)

Rockboy (based on gnuboy from 2000) runs heavy Game Boy titles at ~5 fps on iPod Classic. Rather than trying to optimize 10K+ lines of legacy code, we built a new plugin from scratch using Peanut-GB as the emulation core.

<!-- TODO: Add side-by-side comparison photo: rockboy fps vs pgb fps -->
![Performance comparison](assets/screenshots/comparison.png)

#### Performance Optimizations

Every optimization was driven by profiling on real hardware. The iPod Classic 7G has a 216 MHz ARM926EJ-S with 16KB I-cache, 16KB D-cache, and 128KB IRAM (80KB available to plugins).

| Optimization | Technique | Why It Helps |
|-------------|-----------|--------------|
| **Direct ROM access** | Replace function pointer callbacks with direct array reads | Eliminates thousands of indirect calls per frame |
| **IRAM placement** | Hot functions (19KB) + gb struct (17KB) + APU (1.5KB) in fast IRAM | Bypasses I-cache entirely for critical code paths |
| **O2 on IRAM functions** | `-O2` only on IRAM-resident functions via `__attribute__` | Code bloat has zero cache penalty when in IRAM |
| **Non-blocking audio** | Drop frame if ring buffer full instead of busy-waiting | Prevents audio callback from stalling emulation |
| **Bit-shift volume** | Shift instead of multiply for per-sample volume | Saves ~3000 multiplies per audio frame |
| **Direct LCD writes** | Write pixels directly to LCD framebuffer in 1:1 mode | Skips intermediate buffer copy |
| **32-bit pixel writes** | Pack two 16-bit pixels per store operation | Halves memory store instructions |
| **Split-screen DMA** | Push top/bottom halves on alternating frames (scaled modes) | Halves per-frame bus transfer |
| **Dynamic interlace** | Render 72 lines instead of 144 when below 59 fps | Halves pixel generation cost adaptively |

<!-- TODO: Add photo of FPS counter showing 60+ fps -->
![FPS counter on iPod](assets/screenshots/fps-display.png)

#### Current Performance (iPod Classic 7G)

| Mode | Typical FPS | Notes |
|------|-------------|-------|
| 1:1 (native 160x144) | 50-67 fps | Most games at or near 60 fps |
| Full/Fit (scaled to 320x240) | 31-40 fps | Limited by LCD DMA bandwidth |
| Text-heavy scenes | 40-51 fps (1:1) | Window layer doubles tile rendering work |

#### Features

- **5-slot save states** with RTC timestamps and visual slot selection
- **Autosave modes:** Off, On Exit, Frequent (~60 sec intervals)
- **3 display modes:** 1:1 (centered), Fit (aspect-correct), Full (stretched)
- **4-level DMG palette** with authentic Game Boy colors
- **Non-blocking audio** at 22050 Hz with ring buffer
- **FPS display:** Off, Number only, Full (with "fps" label)
- **Clean menu system:** Save/Load State, Screen Size, Volume, FPS Display, Autosave, Quit

Source files are in [`pgb/`](pgb/) and the compiled plugin in [`releases/pgb-ipod6g.rock`](releases/pgb-ipod6g.rock).

### Phase 3: Game Boy Color Support (Planning)

The next goal is full Game Boy Color support while maintaining 60+ fps. This means adding CGB hardware features to the pgb emulator:

- Dual VRAM banks (2x 8KB)
- WRAM banking (8 banks)
- CGB color palette registers (BG + OBJ)
- Double-speed CPU mode
- HDMA (H-Blank DMA)
- Auto-detection of GBC ROMs via cartridge header byte

The target is a single emulator that plays both DMG and GBC games seamlessly. When you open a `.gb` or `.gbc` file on the iPod, it just works.

<!-- TODO: Add photo of iPod running a GBC game (once implemented) -->
![GBC game running on iPod](assets/screenshots/gbc-preview.png)

## Hardware Target

| Spec | Value |
|------|-------|
| Device | iPod Classic 7th Gen |
| SoC | Samsung S5L8702 |
| CPU | ARM926EJ-S @ 216 MHz (boosted) |
| RAM | 64 MB SDRAM |
| IRAM | 128 KB (80 KB available to plugins) |
| Storage | iFlash adapter, 250 GB SD card |
| Display | 320x240 RGB565 LCD |
| Firmware | Rockbox 4.0 |

## Quick Install

> For iPod Classic 6G or 7G running Rockbox 4.0.

### PGB (recommended, Game Boy)

1. Download `pgb-ipod6g.rock` from [`releases/`](releases/)
2. Copy to your iPod: `.rockbox/rocks/viewers/pgb.rock`
3. Open any `.gb` file from the Rockbox file browser

### Rockboy Audio Fix (Game Boy + Game Boy Color)

1. Download `rockboy-ipod6g.rock` from [`releases/`](releases/)
2. Copy to your iPod: `.rockbox/rocks/games/rockboy.rock`
3. Open any `.gb` or `.gbc` file from the Rockbox file browser

## Building from Source

Requires the Rockbox cross-compiler toolchain (`arm-elf-eabi-gcc`):

```bash
# Clone Rockbox source
git clone https://github.com/Rockbox/rockbox.git
cd rockbox

# Build the ARM cross-compiler (one-time setup)
cd tools && ./rockboxdev.sh
cd ..

# Configure for iPod Classic
mkdir build && cd build
../tools/configure   # Select target 29 (ipod6g)

# Build pgb plugin
make $PWD/apps/plugins/pgb/pgb.rock

# Build rockboy plugin (with audio fix)
make $PWD/apps/plugins/rockboy.rock
```

macOS-specific build issues are documented in [docs/BUILD.md](docs/BUILD.md).

## Repository Structure

```
.
├── pgb/                    # PGB emulator source (Phase 2)
│   ├── pgb.c              # Main plugin: input, menu, LCD, audio
│   ├── peanut_gb.h        # Peanut-GB core (vendored, IRAM-optimized)
│   ├── minigb_apu.c       # APU audio emulation
│   ├── minigb_apu.h       # APU header
│   ├── SOURCES            # Rockbox build list
│   └── pgb.make           # Rockbox build rules
├── src/                    # Rockboy audio fix source (Phase 1)
│   ├── rbsound.c          # Fixed PCM audio bridge
│   ├── sound.c            # Fixed sound channel emulation
│   └── emu.c              # Main emulation loop
├── releases/               # Pre-built ARM binaries
│   ├── pgb-ipod6g.rock
│   └── rockboy-ipod6g.rock
├── docs/                   # Build guides and notes
├── patches/                # Upstream-ready patches
└── assets/screenshots/     # Project photos (add your own!)
```

## Screenshots

> Photos coming soon. Add your own screenshots to `assets/screenshots/` and submit a PR!

<!-- TODO: Add screenshots gallery -->
<!-- Suggested photos:
  - hero.png: iPod Classic running a game in pgb
  - comparison.png: Side-by-side rockboy vs pgb fps
  - fps-display.png: Close-up of FPS counter
  - menu.png: PGB menu screen
  - save-states.png: Save state slot selection
  - gbc-preview.png: GBC game running (Phase 3)
  - hardware.png: iPod Classic 7G with iFlash mod
-->

## Roadmap

- [x] Root cause analysis of 7 rockboy audio bugs
- [x] Rockboy audio fix: 44100 Hz, proper double-buffering, ARM barriers
- [x] Compiled and deployed rockboy fix to real iPod hardware
- [x] PGB: New emulator core (Peanut-GB) ported to Rockbox plugin API
- [x] PGB: IRAM optimization (hot functions + gb struct in fast memory)
- [x] PGB: Direct ROM/RAM access (bypass callback overhead)
- [x] PGB: Non-blocking audio with ring buffer
- [x] PGB: Direct LCD framebuffer writes for 1:1 mode
- [x] PGB: Split-screen DMA for scaled display modes
- [x] PGB: 5-slot save states with autosave
- [x] PGB: Dynamic interlace for adaptive performance
- [ ] PGB: Further scaled-mode optimization (target: 60 fps)
- [ ] GBC: Add Game Boy Color hardware support to pgb
- [ ] GBC: Auto-detect DMG vs GBC ROMs
- [ ] GBC: Color palette rendering
- [ ] GBC: VRAM/WRAM banking
- [ ] GBC: Double-speed CPU mode
- [ ] Submit patches upstream to Rockbox via Gerrit

## Supporting This Project

If this project is useful to you, consider supporting its development:

[![GitHub Sponsors](https://img.shields.io/badge/Sponsor-GitHub-ea4aaa?logo=github)](https://github.com/sponsors/Tyal13)

This project is and will always be free and open source.

## Related Projects

- [Rockbox Explicit Content Filter](https://github.com/Tyal13/rockbox-explicit-filter): Automated explicit content detection for Rockbox
- [Peanut-GB](https://github.com/deltabeard/Peanut-GB): Single-header Game Boy emulator (MIT)
- [Rockbox](https://github.com/Rockbox/rockbox): Open-source firmware for portable media players

## License

- PGB plugin code: GPL v2 (matching Rockbox)
- Peanut-GB core: MIT (c) 2018-2023 Mahyar Koshkouei
- MiniGB APU: MIT, based on MiniGBS by Alex Baines

## Credits

- **Adam Herrmann** ([@Tyal13](https://github.com/Tyal13)): Architecture, optimization, and integration
- **Mahyar Koshkouei**: Peanut-GB emulation core
- **Alex Baines**: MiniGB APU audio emulation
- **Rockbox Project**: Open-source firmware platform
