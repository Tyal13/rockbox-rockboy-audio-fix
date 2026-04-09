# Rockboy Audio Bug Analysis

Deep dive into the audio bugs in `apps/plugins/rockboy/rbsound.c` and `sound.c`.

Tested on: **iPod Classic 7th Gen** running **Rockbox 4.0** (ipod6g target, ARM926EJ-S @ 216 MHz).

---

## Bug #1 — Critical: Wrong sample rate on iPod Classic

**File:** `rbsound.c:32-36`

**Original code:**
```c
#if defined(HW_HAVE_11) && !defined(TOSHIBA_GIGABEAT_F)
    pcm.hz = SAMPR_11;  // 11025 Hz
#else
    pcm.hz = SAMPR_44;  // 44100 Hz
#endif
```

`HW_HAVE_11` is defined in `firmware/export/pcm_sampr.h` based on:
```c
#if (HW_SAMPR_CAPS & SAMPR_CAP_11)
#define HW_HAVE_11
```

For iPod Classic 6G/7G (`firmware/export/config/ipod6g.h`):
```c
#define HW_SAMPR_CAPS (SAMPR_CAP_44 | SAMPR_CAP_22 | SAMPR_CAP_11 | ...)
```

`HW_HAVE_11` means the hardware **can** do 11 kHz -- not that it should be default. The cascade into `sound.c:sound_reset()`:

```c
snd.quality = 44100 / pcm.hz;
```

| `pcm.hz` | `snd.quality` | Effect |
|----------|---------------|--------|
| 44100 Hz | 1 | Every sample generated (correct) |
| 11025 Hz | **4** | 1 in 4 samples generated, no interpolation |

**Fix:** Remove the `HW_HAVE_11` branch. Always use `SAMPR_44`.

---

## Bug #2 — High: Broken double buffer (second half never written)

**Original code:**
```c
buf    = my_malloc(pcm.len * N_BUFS * sizeof(short));  // 2*2048 shorts
pcm.buf = buf;   // always points to buf[0], never updated

static void get_more(const void **start, size_t *size)
{
    memcpy(hwbuf, &buf[pcm.len * doneplay], BUF_SIZE * sizeof(short));
    // When doneplay=1: reads buf[2048..4095] -- always zero
```

`pcm.buf` is permanently `buf[0]`. Sound generation writes to `buf[0..2047]`. The second half is always zeroed. Hardware alternates between real audio and silence -- crackling result.

**Fix:** True ping-pong double buffer. `fill_buf` and `submit_buf` track which slot is being written vs. delivered.

---

## Bug #3 — High: Non-volatile ISR/main flag

```c
bool doneplay = 1;       // NOT volatile

while (!doneplay)        // main thread reads this
    rb->yield();

// In ISR:
doneplay = 1;            // ISR writes this
```

Without `volatile`, GCC `-O2` can cache `doneplay` in a register and never re-read it. The ISR write is invisible to the main thread -- permanent hang.

**Fix:** `volatile bool ready`.

---

## Bug #4 — Medium: No timeout on yield loop

```c
while (!doneplay)
    rb->yield();   // no escape if audio hardware stops
```

If the mixer stops (headphones unplugged, audio reset), this loops forever. Only a hard iPod reset escapes.

**Fix:** 200-iteration timeout (~100 ms). Buffer skipped on timeout, emulation continues.

---

## Bug #5 — Low: Redundant intermediate copy buffer

```c
static unsigned short *hwbuf = 0;   // 4 KB malloc

static void get_more(...)
{
    memcpy(hwbuf, &buf[...], BUF_SIZE * sizeof(short));
    *start = hwbuf;  // unnecessary indirection
```

The Rockbox mixer can receive a direct pointer to the audio buffer. No intermediate copy needed.

**Fix:** Deliver `pcmbuf[submit_buf]` directly. Saves 4 KB of plugin heap.

---

## Summary

| # | Severity | Root Cause | Symptom |
|---|----------|------------|---------|
| 1 | Critical | `HW_HAVE_11` forces 11025 Hz | Harsh, tinny, aliased audio |
| 2 | High | Second buffer half never written | Crackling, dropped audio |
| 3 | High | Non-volatile ISR flag | Potential permanent hang |
| 4 | Medium | No timeout on yield loop | Hang if audio hardware stops |
| 5 | Low | Redundant intermediate copy | 4 KB wasted heap, extra ISR work |
