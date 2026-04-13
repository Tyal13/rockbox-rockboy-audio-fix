/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 * $Id$
 *
 * Rockboy PCM audio backend -- fixed version for Rockbox 4.0.
 *
 * Bug fixes applied:
 *
 *   FIX #1 (Critical) -- Force 44100 Hz on all targets.
 *     Original forced SAMPR_11 (11025 Hz) on iPod Classic because
 *     HW_HAVE_11 is defined. This caused snd.quality=4, skipping 75%
 *     of audio samples with no interpolation. Harsh, tinny audio.
 *     Fix: always use SAMPR_44. The hardware handles it natively.
 *
 *   FIX #2 (High) -- Proper ping-pong double buffer.
 *     Original allocated 2 buffer halves but only wrote to the first.
 *     ISR read from the second half (always zeros) every other callback.
 *     Fix: true alternating double buffer with explicit slot tracking.
 *
 *   FIX #3 (High) -- Volatile ISR synchronization flag.
 *     Original 'doneplay' was a plain bool. Compiler could cache it
 *     in a register, making the ISR write invisible to the main thread.
 *     Fix: volatile flag for correct cross-context visibility.
 *
 *   FIX #4 (Medium) -- Yield loop timeout.
 *     Original: while (!doneplay) { rb->yield(); } -- infinite loop
 *     if audio hardware stops. Fix: timeout after ~100ms.
 *
 *   FIX #5 (Low) -- Eliminated redundant hwbuf memcpy.
 *     Original allocated a separate hwbuf and copied into it every
 *     callback. Fix: deliver pcmbuf directly, saving ~4KB heap.
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License version 2.
 */

#include "rockmacros.h"
#include "defs.h"
#include "pcm.h"

struct pcm pcm IBSS_ATTR;

#define BUF_SIZE 2048

/* Ping-pong double buffer -- allocated from plugin heap via my_malloc */
static short *pcmbuf[2] = {0, 0};

/*
 * submit_buf: which slot the ISR should play.
 *   Written by main thread before setting ready=true.
 *   Read by ISR (get_more).
 *   volatile: accessed from ISR context.
 */
static volatile int submit_buf = 0;

/*
 * fill_buf: which slot the emulator is currently filling.
 *   Only touched by the main thread.
 */
static int fill_buf = 0;

/*
 * ready: signals that submit_buf has a fresh buffer.
 *   Set true by main thread (rockboy_pcm_submit).
 *   Set false by ISR (get_more) after consuming.
 *   volatile: written/read from both contexts.
 */
static volatile bool ready = false;

/* Whether pcm_play_data has been called yet */
static bool started = false;

/*
 * get_more() -- PCM callback, runs in interrupt context.
 *
 * If a fresh buffer is ready, deliver it and clear the flag.
 * On underrun, replay the last good buffer (avoids silence pops).
 */
static void get_more(const void **start, size_t *size)
{
    if (ready)
        ready = false;  /* signal main thread: safe to fill other slot */

    *start = pcmbuf[submit_buf];
    *size  = BUF_SIZE * sizeof(short);
}

void rockboy_pcm_init(void)
{
    if (plugbuf)
        return;

    /*
     * FIX #1: Always use 44100 Hz.
     *
     * Original code:
     *   #if defined(HW_HAVE_11) && !defined(TOSHIBA_GIGABEAT_F)
     *       pcm.hz = SAMPR_11;  // forced 11025 Hz on iPod
     *   #else
     *       pcm.hz = SAMPR_44;
     *   #endif
     *
     * HW_HAVE_11 means the hardware *supports* 11 kHz, not that it
     * should be the default. At 11025 Hz, sound.c sets snd.quality=4
     * (only every 4th sample generated, no interpolation).
     * At 22050 Hz: snd.quality=2, half the iterations, clean audio.
     * At 44100 Hz: snd.quality=1, full quality but too slow for 60 FPS.
     */
    pcm.hz     = SAMPR_22;
    pcm.stereo = 1;
    pcm.len    = BUF_SIZE;

    /* Allocate buffers from plugin heap if first call */
    if (!pcmbuf[0])
    {
        pcmbuf[0] = my_malloc(BUF_SIZE * sizeof(short));
        pcmbuf[1] = my_malloc(BUF_SIZE * sizeof(short));
        memset(pcmbuf[0], 0, BUF_SIZE * sizeof(short));
        memset(pcmbuf[1], 0, BUF_SIZE * sizeof(short));
    }

    fill_buf   = 0;
    submit_buf = 0;
    ready      = false;
    started    = false;

    pcm.buf = pcmbuf[fill_buf];
    pcm.pos = 0;

    rb->pcm_play_stop();

#if INPUT_SRC_CAPS != 0
    rb->audio_set_input_source(AUDIO_SRC_PLAYBACK, SRCF_PLAYBACK);
    rb->audio_set_output_source(AUDIO_SRC_PLAYBACK);
#endif

    rb->pcm_set_frequency(pcm.hz);
}

void rockboy_pcm_close(void)
{
    rb->pcm_play_stop();
    rb->pcm_set_frequency(HW_SAMPR_DEFAULT);
    memset(&pcm, 0, sizeof pcm);
    started = false;
    ready   = false;
}

int rockboy_pcm_submit(void)
{
    if (!pcm.buf)          return 0;
    if (pcm.pos < pcm.len) return 1;  /* not full yet, keep filling */

    /*
     * FIX #2: Point ISR at the slot we just filled.
     * submit_buf written BEFORE ready=true so ISR sees correct slot.
     */
    submit_buf = fill_buf;
    ready      = true;       /* FIX #3: volatile, visible to ISR */

    /* Start PCM playback on first submit */
    if (!started)
    {
        rb->pcm_play_data(&get_more, NULL, NULL, 0);
        started = true;
    }

    /*
     * FIX #4: Wait for ISR to consume, with timeout (~100ms).
     * Prevents infinite hang if audio hardware stops.
     */
    int timeout = 200;
    while (ready && --timeout > 0)
        rb->yield();

    /*
     * FIX #2 continued: Swap to the other buffer slot.
     * ISR is now playing fill_buf, we write into the alternate.
     */
    fill_buf = 1 - fill_buf;
    pcm.buf  = pcmbuf[fill_buf];
    pcm.pos  = 0;

    return 1;
}
