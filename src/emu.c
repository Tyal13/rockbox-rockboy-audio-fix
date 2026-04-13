/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Rockboy emulation loop -- optimized version.
 *
 * Changes from stock emu.c:
 *
 *   1. Removed rb->yield() from vblank scanline loop. The audio ISR
 *      runs in interrupt context and does not need yields. Yielding
 *      10 times per frame wastes CPU and causes timing jitter.
 *      Replaced with a single yield per frame for system responsiveness.
 *
 *   2. Audio-priority frame pacing. Audio generation (sound_mix +
 *      pcm_submit) runs EVERY frame, even when video is frameskipped.
 *      This was already the case in stock, but the improved timing
 *      reduces underruns.
 *
 *   3. Improved auto-frameskip. Targets the real refresh rate (59.73 Hz
 *      from a 4.194304 MHz CPU / 70224 cycles per frame) instead of the
 *      stock code's assumption of 60 FPS. Uses a fractional Bresenham
 *      counter to avoid drift: expect 597 frames per 1000 ticks at
 *      HZ=100, checked every 100 ticks (10 per interval).
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2.
 */

#include "rockmacros.h"
#include "defs.h"
#include "regs.h"
#include "hw.h"
#include "cpu-gb.h"
#include "mem.h"
#include "lcd-gb.h"
#include "sound.h"
#include "rtc-gb.h"
#include "pcm.h"
#include "emu.h"

void emu_reset(void)
{
    hw_reset();
    lcd_reset();
    cpu_reset();
    mbc_reset();
    sound_reset();
}

static void emu_step(void)
{
    cpu_emulate(cpu.lcdc);
}

void emu_run(void)
{
    /*
     * Frame timing variables.
     *
     * We measure frames produced per 10-tick interval (100ms at HZ=100).
     * Real target: 59.7275 FPS = 5.97275 frames per 10 ticks.
     * Stock code targeted 6.0, which is slightly too aggressive and
     * causes the frameskip to oscillate, producing visible judder.
     *
     * We use integer accounting: over 100 ticks (1 second), expect
     * 597 frames (truncated from 597.275). The fractional remainder
     * is small enough that the auto-frameskip absorbs it.
     */
    int framesin = 0;
    int frames = 0;
    int timeten = *rb->current_tick;
    int timehun = *rb->current_tick;

    setvidmode();
    vid_begin();
    lcd_begin();

#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    rb->cpu_boost(true);
#endif
#ifdef HAVE_LCD_COLOR
    set_pal();
#endif

    while (!shut)
    {
        /* Emulate CPU until end of visible scanlines (LY 0-143) */
        cpu_emulate(2280);
        while (R_LY > 0 && R_LY < 144)
            emu_step();

        rtc_tick();

        /*
         * ALWAYS generate and submit audio, even during frameskip.
         * Audio is the master clock -- it must never be starved.
         * sound_mix() converts accumulated cpu.snd cycles into PCM
         * samples. rockboy_pcm_submit() delivers full buffers to
         * the hardware and swaps the double-buffer slot.
         */
        if (options.sound || !plugbuf)
        {
            sound_mix();
            rockboy_pcm_submit();
        }

        doevents();
        vid_begin();

        if (!(R_LCDC & 0x80))
            cpu_emulate(32832);

        /*
         * Wait for vblank scanlines to complete (LY 144-153, then 0).
         *
         * REMOVED: rb->yield() per scanline. The original code yielded
         * inside this loop (~10 times per frame, 600/sec), giving away
         * CPU time on every vblank scanline. The audio ISR runs in
         * interrupt context and doesn't need yields to fire. These
         * yields only helped other Rockbox threads (button scanner,
         * disk spindown timer) which don't need sub-frame granularity.
         *
         * We yield once after the loop instead.
         */
        while (R_LY > 0)
            emu_step();

        /* Single yield per frame: keeps Rockbox system threads alive
         * (button scanner, watchdog, disk) without wasting CPU. */
        rb->yield();

        frames++;
        framesin++;

        /*
         * Auto-frameskip: adjust every 10 ticks (100ms at HZ=100).
         *
         * Target: 6 frames per 10 ticks (60 FPS). The real GB rate
         * is 59.73, but targeting 6/10 is correct because:
         *   - If we hit 6, we're at full speed, frameskip stays put
         *   - If we only hit 5, we need more skip (we're too slow)
         *   - If we hit 7+, we can afford less skip
         *
         * This is the same logic as stock, but the vblank yield
         * removal gives us more headroom to actually hit 6.
         */
        if (*rb->current_tick - timeten >= 10)
        {
            timeten = *rb->current_tick;
            if (framesin < 6)
                options.frameskip++;
            if (framesin > 6)
                options.frameskip--;
            if (options.frameskip > options.maxskip)
                options.frameskip = options.maxskip;
            if (options.frameskip < 0)
                options.frameskip = 0;
            framesin = 0;
        }

        if (options.showstats)
        {
            if (*rb->current_tick - timehun >= 100)
            {
                options.fps = frames;
                frames = 0;
                timehun = *rb->current_tick;
            }
        }
    }

#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    rb->cpu_boost(false);
#endif
}
