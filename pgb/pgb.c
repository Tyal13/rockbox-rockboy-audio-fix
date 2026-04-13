/***************************************************************************
 *             __________               __   ___.
 *   Open      \______   \ ____   ____ |  | _\_ |__   _______  ___
 *   Source     |       _//  _ \_/ ___\|  |/ /| __ \ /  _ \  \/  /
 *   Jukebox    |    |   (  <_> )  \___|    < | \_\ (  <_> > <  <
 *   Firmware   |____|_  /\____/ \___  >__|_ \|___  /\____/__/\_ \
 *                     \/            \/     \/    \/            \/
 *
 * PGB - Peanut-GB Game Boy emulator for Rockbox
 * Peanut-GB: MIT (c) 2018-2023 Mahyar Koshkouei
 * MiniGB APU: MIT, based on MiniGBS by Alex Baines
 * GNU GPL v2
 ***************************************************************************/

#include "plugin.h"
#include "lib/helper.h"

/* ---- Compatibility shims ---- */
#ifndef UINT_FAST8_MAX
typedef uint8_t uint_fast8_t; typedef uint16_t uint_fast16_t;
typedef uint32_t uint_fast32_t; typedef int8_t int_fast8_t;
typedef int16_t int_fast16_t; typedef int32_t int_fast32_t;
#define UINT_FAST8_MAX UINT8_MAX
#define UINT_FAST16_MAX UINT16_MAX
#define UINT_FAST32_MAX UINT32_MAX
#define INT_FAST8_MAX INT8_MAX
#define INT_FAST8_MIN INT8_MIN
#define INT_FAST16_MAX INT16_MAX
#define INT_FAST16_MIN INT16_MIN
#define INT_FAST32_MAX INT32_MAX
#define INT_FAST32_MIN INT32_MIN
#endif

#define abort() rb->splash(HZ*3, "Fatal error")
#include <time.h>

/* ---- Heap allocator ---- */
static void *heap_ptr;
static size_t heap_free;
static void heap_init(void) {
    heap_ptr = rb->plugin_get_audio_buffer(&heap_free);
}
static void *pgb_malloc(size_t size) {
    void *p; size = (size + 3) & ~3;
    if (size > heap_free) return NULL;
    p = heap_ptr; heap_ptr = (char*)heap_ptr + size; heap_free -= size;
    return p;
}

/* ---- Peanut-GB config ---- */
#define ENABLE_SOUND 1
#define ENABLE_LCD 1
#define PEANUT_GB_IS_LITTLE_ENDIAN 1
#define PEANUT_GB_12_COLOUR 0
#define PEANUT_GB_HIGH_LCD_ACCURACY 0
#define PEANUT_GB_USE_INTRINSICS 0

/* IRAM functions bypass I-cache, so O2 bloat has zero penalty.
 * Override ICODE_ATTR to also force O2 on hot functions. */
#undef ICODE_ATTR
#define ICODE_ATTR __attribute__((section(".icode"),optimize("O2")))

static uint8_t audio_read(const uint16_t addr);
static void audio_write(const uint16_t addr, const uint8_t val);

/* Direct ROM/RAM pointers: bypass function pointer callbacks in hot paths.
 * Eliminates thousands of indirect calls per frame. */
#define PGB_DIRECT_ACCESS 1
static uint8_t *pgb_rom_ptr;
static uint8_t *pgb_cart_ram_ptr;
static bool pgb_ram_dirty;  /* set by direct cart RAM writes */

enum { RB_LCD_WIDTH = LCD_WIDTH, RB_LCD_HEIGHT = LCD_HEIGHT };
#undef LCD_WIDTH
#undef LCD_HEIGHT
#include "peanut_gb.h"
#undef LCD_WIDTH
#undef LCD_HEIGHT

/* ---- Audio ---- */
#define PGB_AUDIO_RATE 22050
#define AUDIO_SAMPLE_RATE PGB_AUDIO_RATE
#define MINIGB_APU_AUDIO_FORMAT_S16SYS
#include "minigb_apu.h"

static struct minigb_apu_ctx apu_ctx;
static uint8_t audio_read(const uint16_t addr) {
    return minigb_apu_audio_read(&apu_ctx, addr);
}
static void audio_write(const uint16_t addr, const uint8_t val) {
    minigb_apu_audio_write(&apu_ctx, addr, val);
}

#define RING_SLOTS 4
#define RING_BUF_SAMPLES AUDIO_SAMPLES_TOTAL
#define RING_BUF_BYTES (RING_BUF_SAMPLES * sizeof(int16_t))

static int16_t *ring_buf[RING_SLOTS];
static volatile int ring_rd = 0, ring_wr = 0;
static bool audio_started = false;

/* Volume: index into vol_shift table. Simple right-shift for zero-cost volume.
 * The APU output is ~8x too hot, so baseline shift of 3 = comfortable 0dB.
 * Index: 0=Loud(>>1), 1=>>2, 2=>>3(0dB), 3=>>4(-6dB), 4=>>5(-12dB),
 *        5=>>6(-18dB), 6=Mute */
static int sound_vol = 2; /* default = 0 dB (>>3) */
static const uint8_t vol_shift[] = { 1, 2, 3, 4, 5, 6, 0xFF };
#define VOL_LEVELS 7

static void pcm_callback(const void **start, size_t *size) {
    *start = ring_buf[ring_rd]; *size = RING_BUF_BYTES;
    int next = (ring_rd + 1) % RING_SLOTS;
    if (next != ring_wr) ring_rd = next;
}

static void pgb_audio_init(void) {
    int i;
    for (i = 0; i < RING_SLOTS; i++) {
        ring_buf[i] = pgb_malloc(RING_BUF_BYTES);
        rb->memset(ring_buf[i], 0, RING_BUF_BYTES);
    }
    ring_rd = ring_wr = 0; audio_started = false;
    minigb_apu_audio_init(&apu_ctx);
    rb->pcm_play_stop(); rb->pcm_set_frequency(PGB_AUDIO_RATE);
}

ICODE_ATTR static void audio_submit_frame(void) {
    int next_wr = (ring_wr + 1) % RING_SLOTS;

    /* Non-blocking: if ring buffer is full, drop this frame's audio */
    if (next_wr == ring_rd) return;

    uint8_t shift = vol_shift[sound_vol];
    if (shift != 0xFF) {
        minigb_apu_audio_callback(&apu_ctx, ring_buf[ring_wr]);
        /* Volume: simple arithmetic shift, no multiply needed */
        int16_t *s = ring_buf[ring_wr];
        unsigned int i;
        for (i = 0; i < RING_BUF_SAMPLES; i++)
            s[i] >>= shift;
    } else {
        rb->memset(ring_buf[ring_wr], 0, RING_BUF_BYTES);
    }
    ring_wr = next_wr;
    if (!audio_started) {
        rb->pcm_play_data(&pcm_callback, NULL, NULL, 0);
        audio_started = true;
    }
}

static void audio_close(void) {
    rb->pcm_play_stop(); rb->pcm_set_frequency(HW_SAMPR_DEFAULT);
}

/* ---- LCD ---- */
#define GB_LCD_W 160
#define GB_LCD_H 144
#define SCALE_1X   0
#define SCALE_FULL 1
#define SCALE_FIT  2

static int scale_mode = 0;

/* FPS display: 0=Off, 1=Number only, 2=Full ("XX FPS") */
static int show_fps = 1;

static const uint16_t palette[4] = {
    0xFFFF, 0xAD55, 0x52AA, 0x0000
};

static uint16_t framebuf[GB_LCD_W * GB_LCD_H];

/* Precomputed row mapping for vertical scaling (240 entries) */
static uint8_t row_map[240];

/* Precomputed horizontal scale table for Fit mode (266 -> 160 mapping) */
static uint8_t col_map_fit[268]; /* dst_x -> src_x */

/* Cached LCD framebuffer pointer for direct writes in 1:1 mode */
static fb_data *lcd_fb;
static int lcd_x_off, lcd_y_off;

static void build_scale_tables(void) {
    int i;
    for (i = 0; i < RB_LCD_HEIGHT; i++)
        row_map[i] = i * GB_LCD_H / RB_LCD_HEIGHT;

    /* Build horizontal map for Fit: dst_x -> src_x */
    { int out_w = GB_LCD_W * RB_LCD_HEIGHT / GB_LCD_H;
      for (i = 0; i < out_w; i++)
          col_map_fit[i] = i * GB_LCD_W / out_w;
    }
}

static void cache_fb(void) {
    struct viewport *vp = *(rb->screens[SCREEN_MAIN]->current_viewport);
    lcd_fb = vp->buffer->fb_ptr;
    lcd_x_off = (RB_LCD_WIDTH - GB_LCD_W) / 2;
    lcd_y_off = (RB_LCD_HEIGHT - GB_LCD_H) / 2;
}

/* In 1:1 mode: write pixels DIRECTLY to LCD framebuffer (skip intermediate copy).
 * In scaled modes: write to framebuf for later scaling. */
static void lcd_draw_line_cb(struct gb_s *gb, const uint8_t *pixels,
                              const uint_fast8_t line) ICODE_ATTR;
static void lcd_draw_line_cb(struct gb_s *gb, const uint8_t *pixels,
                              const uint_fast8_t line) {
    (void)gb;

    if (scale_mode == SCALE_1X) {
        /* Direct write to LCD framebuffer: no intermediate copy needed.
         * Saves 46KB of memcpy per frame (144 lines * 320 bytes). */
        uint32_t *dest32 = (uint32_t *)(lcd_fb + (lcd_y_off + line) * RB_LCD_WIDTH + lcd_x_off);
        int x;
        for (x = 0; x < GB_LCD_W; x += 2) {
            uint32_t c0 = palette[pixels[x] & 3];
            uint32_t c1 = palette[pixels[x + 1] & 3];
            dest32[x >> 1] = c0 | (c1 << 16);
        }
    } else {
        /* Write to intermediate buffer for scaling */
        uint32_t *dest32 = (uint32_t *)&framebuf[line * GB_LCD_W];
        int x;
        for (x = 0; x < GB_LCD_W; x += 2) {
            uint32_t c0 = palette[pixels[x] & 3];
            uint32_t c1 = palette[pixels[x + 1] & 3];
            dest32[x >> 1] = c0 | (c1 << 16);
        }
    }
}

/* Split-screen DMA: push top half one frame, bottom half next.
 * Halves the LCD DMA cost (~12ms → ~6ms per frame). */
static int lcd_half = 0;

static void pgb_lcd_update(void) ICODE_ATTR;
static void pgb_lcd_update(void) {
    switch (scale_mode) {
    default:
    case SCALE_1X:
        rb->lcd_update_rect(lcd_x_off, lcd_y_off, GB_LCD_W, GB_LCD_H);
        break;

    case SCALE_FULL: {
        fb_data *fb = lcd_fb;
        int y, prev_src_y = -1;
        fb_data *prev_row = NULL;
        for (y = 0; y < RB_LCD_HEIGHT; y++) {
            int src_y = row_map[y];
            fb_data *dst_row = fb + y * RB_LCD_WIDTH;
            if (src_y == prev_src_y && prev_row) {
                rb->memcpy(dst_row, prev_row, RB_LCD_WIDTH * sizeof(fb_data));
            } else {
                uint16_t *src_row = &framebuf[src_y * GB_LCD_W];
                uint32_t *dst32 = (uint32_t *)dst_row;
                int x;
                for (x = 0; x < GB_LCD_W; x++) {
                    uint32_t p = src_row[x];
                    dst32[x] = p | (p << 16);
                }
            }
            prev_src_y = src_y; prev_row = dst_row;
        }
        /* Alternate top/bottom half DMA to halve bus transfer time */
        lcd_half ^= 1;
        if (lcd_half)
            rb->lcd_update_rect(0, 0, RB_LCD_WIDTH, RB_LCD_HEIGHT / 2);
        else
            rb->lcd_update_rect(0, RB_LCD_HEIGHT / 2, RB_LCD_WIDTH, RB_LCD_HEIGHT / 2);
        break;
    }
    case SCALE_FIT: {
        fb_data *fb = lcd_fb;
        int out_w = GB_LCD_W * RB_LCD_HEIGHT / GB_LCD_H;
        int x_off = (RB_LCD_WIDTH - out_w) / 2;
        int y, prev_src_y = -1;
        fb_data *prev_row = NULL;
        for (y = 0; y < RB_LCD_HEIGHT; y++) {
            int src_y = row_map[y];
            fb_data *dst_row = fb + y * RB_LCD_WIDTH + x_off;
            if (src_y == prev_src_y && prev_row) {
                rb->memcpy(dst_row, prev_row, out_w * sizeof(fb_data));
            } else {
                uint16_t *src = &framebuf[src_y * GB_LCD_W];
                int x;
                for (x = 0; x < out_w; x++)
                    dst_row[x] = src[col_map_fit[x]];
            }
            prev_src_y = src_y; prev_row = dst_row;
        }
        lcd_half ^= 1;
        if (lcd_half)
            rb->lcd_update_rect(x_off, 0, out_w, RB_LCD_HEIGHT / 2);
        else
            rb->lcd_update_rect(x_off, RB_LCD_HEIGHT / 2, out_w, RB_LCD_HEIGHT / 2);
        break;
    }
    }
}

/* ---- Input ---- */
#define INPUT_QUIT (-1)
#define INPUT_MENU (-2)
#define INPUT_OK    0

#ifdef HAVE_WHEEL_POSITION
static const uint8_t wheelmap[8] = {
    JOYPAD_UP, JOYPAD_A, JOYPAD_RIGHT, JOYPAD_START,
    JOYPAD_DOWN, JOYPAD_SELECT, JOYPAD_LEFT, JOYPAD_B
};
static int oldwheel = -1;
static uint8_t wheel_joypad = 0;
#endif

static int input_poll(struct gb_s *gb) {
    uint8_t joypad = 0;
#ifdef HAVE_WHEEL_POSITION
    {
        int wheel = rb->wheel_status();
        if (wheel >= 0) { wheel = (wheel + 6) / 12; if (wheel > 7) wheel = 0; }
        if (wheel != oldwheel) {
            wheel_joypad = (wheel >= 0) ? wheelmap[wheel] : 0;
            oldwheel = wheel;
        }
        joypad |= wheel_joypad;
    }
#endif
    unsigned int btn = rb->button_status();
    if (rb->button_hold()) return INPUT_MENU;
    if (btn & BUTTON_SELECT) joypad |= JOYPAD_B;
    gb->direct.joypad = 0xFF ^ joypad;
    return INPUT_OK;
}

/* ---- Save ---- */
struct pgb_priv {
    uint8_t *rom; size_t rom_size;
    uint8_t *cart_ram; size_t cart_ram_size;
    char sav_path[MAX_PATH]; bool dirty;
    char rom_dir[MAX_PATH];
    char rom_base[64];
};

/* Autosave mode: 0=Off, 1=On exit only, 2=Frequent (every ~60s) */
static int autosave_mode = 1;
#define AUTOSAVE_INTERVAL (60 * 60) /* 60 seconds in ticks (HZ=~60) */

static void save_cart_ram(struct pgb_priv *priv) {
    int fd;
    if (!pgb_ram_dirty || !priv->cart_ram || priv->cart_ram_size == 0) return;
    fd = rb->open(priv->sav_path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd >= 0) { rb->write(fd, priv->cart_ram, priv->cart_ram_size); rb->close(fd); }
    pgb_ram_dirty = false;
}
static void load_cart_ram(struct pgb_priv *priv) {
    int fd;
    if (!priv->cart_ram || priv->cart_ram_size == 0) return;
    fd = rb->open(priv->sav_path, O_RDONLY);
    if (fd >= 0) { rb->read(fd, priv->cart_ram, priv->cart_ram_size); rb->close(fd); }
}

/* ---- Save States ---- */
#define NUM_SAVE_SLOTS 5
#define SAVESTATE_MAGIC 0x50474231  /* "PGB1" */

struct savestate_header {
    uint32_t magic;
    uint32_t gb_size;
    uint32_t apu_size;
    uint32_t cart_ram_size;
    int32_t  save_year;   /* RTC timestamp of when state was saved */
    int32_t  save_mon;
    int32_t  save_day;
    int32_t  save_hour;
    int32_t  save_min;
    int32_t  save_sec;
};

static void get_state_path(struct pgb_priv *priv, int slot, char *buf, size_t bufsz) {
    rb->snprintf(buf, bufsz, "%s/%s.ss%d", priv->rom_dir, priv->rom_base, slot + 1);
}

/* Read slot header info. Returns true if slot has valid data.
 * If hdr_out is non-NULL, fills it with the header. */
static bool state_slot_info(struct pgb_priv *priv, int slot,
                            struct savestate_header *hdr_out) {
    char path[MAX_PATH]; int fd;
    struct savestate_header hdr;
    get_state_path(priv, slot, path, sizeof(path));
    fd = rb->open(path, O_RDONLY);
    if (fd < 0) return false;
    bool ok = (rb->read(fd, &hdr, sizeof(hdr)) == (ssize_t)sizeof(hdr) &&
               hdr.magic == SAVESTATE_MAGIC);
    rb->close(fd);
    if (ok && hdr_out) *hdr_out = hdr;
    return ok;
}

static void do_save_state(struct gb_s *gb, struct pgb_priv *priv, int slot) {
    char path[MAX_PATH]; int fd;
    struct savestate_header hdr;

    get_state_path(priv, slot, path, sizeof(path));
    fd = rb->open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) { rb->splash(HZ, "Save failed: open"); return; }

    hdr.magic = SAVESTATE_MAGIC;
    hdr.gb_size = sizeof(struct gb_s);
    hdr.apu_size = sizeof(struct minigb_apu_ctx);
    hdr.cart_ram_size = priv->cart_ram_size;
    { struct tm *t = rb->get_time();
      hdr.save_year = t->tm_year + 1900;
      hdr.save_mon  = t->tm_mon + 1;
      hdr.save_day  = t->tm_mday;
      hdr.save_hour = t->tm_hour;
      hdr.save_min  = t->tm_min;
      hdr.save_sec  = t->tm_sec;
    }

    rb->write(fd, &hdr, sizeof(hdr));
    rb->write(fd, gb, sizeof(struct gb_s));
    rb->write(fd, &apu_ctx, sizeof(struct minigb_apu_ctx));
    if (priv->cart_ram && priv->cart_ram_size > 0)
        rb->write(fd, priv->cart_ram, priv->cart_ram_size);
    rb->close(fd);
    rb->splashf(HZ/2, "State %d saved", slot + 1);
}

static void do_load_state(struct gb_s *gb, struct pgb_priv *priv, int slot) {
    char path[MAX_PATH]; int fd;
    struct savestate_header hdr;

    get_state_path(priv, slot, path, sizeof(path));
    fd = rb->open(path, O_RDONLY);
    if (fd < 0) { rb->splashf(HZ, "Slot %d empty", slot + 1); return; }

    if (rb->read(fd, &hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr) ||
        hdr.magic != SAVESTATE_MAGIC ||
        hdr.gb_size != sizeof(struct gb_s)) {
        rb->close(fd); rb->splash(HZ, "Bad save state"); return;
    }

    /* Save function pointers before overwriting */
    void *saved_rom_read = (void*)gb->gb_rom_read;
    void *saved_ram_read = (void*)gb->gb_cart_ram_read;
    void *saved_ram_write = (void*)gb->gb_cart_ram_write;
    void *saved_error = (void*)gb->gb_error;
    void *saved_lcd = (void*)gb->display.lcd_draw_line;
    void *saved_priv = gb->direct.priv;

    rb->read(fd, gb, sizeof(struct gb_s));

    /* Restore function pointers (addresses change between sessions) */
    gb->gb_rom_read = saved_rom_read;
    gb->gb_cart_ram_read = saved_ram_read;
    gb->gb_cart_ram_write = saved_ram_write;
    gb->gb_error = saved_error;
    gb->display.lcd_draw_line = saved_lcd;
    gb->direct.priv = saved_priv;
    gb->gb_serial_tx = NULL;
    gb->gb_serial_rx = NULL;
    gb->gb_bootrom_read = NULL;

    rb->read(fd, &apu_ctx, sizeof(struct minigb_apu_ctx));
    if (priv->cart_ram && hdr.cart_ram_size > 0 && hdr.cart_ram_size == priv->cart_ram_size)
        rb->read(fd, priv->cart_ram, priv->cart_ram_size);
    rb->close(fd);
    rb->splashf(HZ/2, "State %d loaded", slot + 1);
}

/* ---- Menu ---- */
static int pgb_menu(struct gb_s *gb, struct pgb_priv *priv) {
    int selected = 0; bool done = false; int result;
    rb->pcm_play_stop(); audio_started = false;
#ifdef HAVE_WHEEL_POSITION
    rb->wheel_send_events(true);
#endif
    while (rb->button_get(false) != BUTTON_NONE) rb->yield();
    while (rb->button_hold()) rb->sleep(HZ / 20);

    MENUITEM_STRINGLIST(menu, "PGB Menu", NULL,
        "Resume", "Save State", "Load State",
        "Screen Size", "Volume", "FPS Display", "Autosave", "Quit");

    while (!done) {
        result = rb->do_menu(&menu, &selected, NULL, false);
        switch (result) {
        case 0: done = true; break;
        case 1: { /* Save State */
            char labels[NUM_SAVE_SLOTS][48];
            struct opt_items items[NUM_SAVE_SLOTS];
            struct savestate_header shdr;
            int slot = 0, i;
            for (i = 0; i < NUM_SAVE_SLOTS; i++) {
                if (state_slot_info(priv, i, &shdr))
                    rb->snprintf(labels[i], sizeof(labels[i]),
                        "Slot %d (%04d-%02d-%02d %02d:%02d:%02d)",
                        i + 1, (int)shdr.save_year, (int)shdr.save_mon,
                        (int)shdr.save_day, (int)shdr.save_hour,
                        (int)shdr.save_min, (int)shdr.save_sec);
                else
                    rb->snprintf(labels[i], sizeof(labels[i]),
                        "Slot %d (empty)", i + 1);
                items[i].string = labels[i]; items[i].voice_id = -1;
            }
            rb->set_option("Save to slot", &slot, RB_INT, items, NUM_SAVE_SLOTS, NULL);
            do_save_state(gb, priv, slot);
            break;
        }
        case 2: { /* Load State */
            char labels[NUM_SAVE_SLOTS][48];
            struct opt_items items[NUM_SAVE_SLOTS];
            struct savestate_header shdr;
            int slot = 0, i;
            for (i = 0; i < NUM_SAVE_SLOTS; i++) {
                if (state_slot_info(priv, i, &shdr))
                    rb->snprintf(labels[i], sizeof(labels[i]),
                        "Slot %d (%04d-%02d-%02d %02d:%02d:%02d)",
                        i + 1, (int)shdr.save_year, (int)shdr.save_mon,
                        (int)shdr.save_day, (int)shdr.save_hour,
                        (int)shdr.save_min, (int)shdr.save_sec);
                else
                    rb->snprintf(labels[i], sizeof(labels[i]),
                        "Slot %d (empty)", i + 1);
                items[i].string = labels[i]; items[i].voice_id = -1;
            }
            rb->set_option("Load from slot", &slot, RB_INT, items, NUM_SAVE_SLOTS, NULL);
            do_load_state(gb, priv, slot);
            break;
        }
        case 3: { /* Screen Size */
            static const struct opt_items s[] = {
                {"1:1 (160x144)", -1}, {"Full (320x240)", -1}, {"Fit (266x240)", -1},
            };
            rb->set_option("Screen Size", &scale_mode, RB_INT, s, 3, NULL);
            break;
        }
        case 4: { /* Volume */
            static const struct opt_items v[] = {
                {"Loud (+12 dB)", -1}, {"High (+6 dB)", -1},
                {"Normal (0 dB)", -1}, {"Low (-6 dB)", -1},
                {"Quiet (-12 dB)", -1}, {"Whisper (-18 dB)", -1},
                {"Mute", -1},
            };
            rb->set_option("Volume", &sound_vol, RB_INT, v, VOL_LEVELS, NULL);
            break;
        }
        case 5: { /* FPS Display */
            static const struct opt_items f[] = {
                {"Off", -1}, {"Number", -1}, {"Full (##fps)", -1}
            };
            rb->set_option("FPS Display", &show_fps, RB_INT, f, 3, NULL);
            break;
        }
        case 6: { /* Autosave */
            static const struct opt_items a[] = {
                {"Off - no auto saving", -1},
                {"On exit - save when quitting", -1},
                {"Frequent - save every ~60 sec", -1},
            };
            rb->set_option("Autosave", &autosave_mode, RB_INT, a, 3, NULL);
            break;
        }
        case 7: /* Quit */
#ifdef HAVE_WHEEL_POSITION
            rb->wheel_send_events(false);
#endif
            return INPUT_QUIT;
        default: done = true; break;
        }
    }
#ifdef HAVE_WHEEL_POSITION
    rb->wheel_send_events(false);
#endif
    rb->pcm_play_data(&pcm_callback, NULL, NULL, 0);
    audio_started = true;
    return INPUT_OK;
}

/* ---- GB callbacks ---- */
static uint8_t rom_read_cb(struct gb_s *gb, const uint_fast32_t addr) {
    return ((struct pgb_priv*)gb->direct.priv)->rom[addr];
}
static uint8_t cart_ram_read_cb(struct gb_s *gb, const uint_fast32_t addr) {
    return ((struct pgb_priv*)gb->direct.priv)->cart_ram[addr];
}
static void cart_ram_write_cb(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val) {
    struct pgb_priv *priv = gb->direct.priv;
    priv->cart_ram[addr] = val; priv->dirty = true;
}
static void error_cb(struct gb_s *gb, const enum gb_error_e err, const uint16_t addr) {
    (void)gb; (void)addr; (void)err;
}

/* ---- ROM loading ---- */
static int load_rom(struct pgb_priv *priv, const char *path) {
    int fd; ssize_t n;
    fd = rb->open(path, O_RDONLY);
    if (fd < 0) { rb->splashf(HZ*3, "Cannot open ROM"); return -1; }
    priv->rom_size = rb->filesize(fd);
    priv->rom = pgb_malloc(priv->rom_size);
    if (!priv->rom) { rb->close(fd); rb->splashf(HZ*3, "No memory"); return -1; }
    n = rb->read(fd, priv->rom, priv->rom_size); rb->close(fd);
    if ((size_t)n != priv->rom_size) { rb->splashf(HZ*3, "ROM read err"); return -1; }
    rb->strlcpy(priv->sav_path, path, sizeof(priv->sav_path));
    char *dot = rb->strrchr(priv->sav_path, '.');
    if (dot) rb->strcpy(dot, ".sav"); else rb->strcat(priv->sav_path, ".sav");

    /* Extract directory and base name for save state paths */
    rb->strlcpy(priv->rom_dir, path, sizeof(priv->rom_dir));
    { char *slash = rb->strrchr(priv->rom_dir, '/');
      if (slash) {
          rb->strlcpy(priv->rom_base, slash + 1, sizeof(priv->rom_base));
          *slash = '\0';
      } else {
          rb->strlcpy(priv->rom_base, path, sizeof(priv->rom_base));
          rb->strcpy(priv->rom_dir, "/");
      }
      dot = rb->strrchr(priv->rom_base, '.');
      if (dot) *dot = '\0';
    }
    return 0;
}

/* ---- Main ---- */
enum plugin_status plugin_start(const void *parameter) {
    static struct gb_s gb IBSS_ATTR;  /* 16.9KB in fast IRAM */
    static struct pgb_priv priv;
    enum gb_init_error_e ret;
    bool running = true;
    int fps_frames = 0, fps_val = 0, last_fps_drawn = -1;
    long fps_tick, autosave_tick;
    char fps_str[16];

    rb->lcd_setfont(FONT_SYSFIXED);
    if (!parameter) { rb->splash(HZ*3, "Open a .gb or .gbc ROM"); return PLUGIN_OK; }

    heap_init();
    rb->lcd_clear_display(); rb->lcd_update();
    rb->memset(framebuf, 0, sizeof(framebuf));
    rb->memset(&priv, 0, sizeof(priv));

    rb->splash(HZ/2, "Loading ROM...");
    if (load_rom(&priv, (const char*)parameter) < 0) return PLUGIN_ERROR;

    ret = gb_init(&gb, rom_read_cb, cart_ram_read_cb, cart_ram_write_cb, error_cb, &priv);
    if (ret != GB_INIT_NO_ERROR) { rb->splashf(HZ*3, "Init err: %d", ret); return PLUGIN_ERROR; }

    { size_t sz = 0; gb_get_save_size_s(&gb, &sz); priv.cart_ram_size = sz; }
    if (priv.cart_ram_size > 0) {
        priv.cart_ram = pgb_malloc(priv.cart_ram_size);
        if (priv.cart_ram) { rb->memset(priv.cart_ram, 0xFF, priv.cart_ram_size); load_cart_ram(&priv); }
    }

    /* Set direct ROM/RAM pointers for fast access (bypasses callbacks) */
    pgb_rom_ptr = priv.rom;
    pgb_cart_ram_ptr = priv.cart_ram;
    pgb_ram_dirty = false;

    gb_init_lcd(&gb, lcd_draw_line_cb);
    pgb_audio_init();
    build_scale_tables();
    cache_fb();

    backlight_ignore_timeout();
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    rb->cpu_boost(true);
#endif
#ifdef HAVE_WHEEL_POSITION
    rb->wheel_send_events(false);
#endif

    rb->lcd_clear_display(); rb->lcd_update();
    fps_tick = *rb->current_tick;
    autosave_tick = fps_tick;

    /* Interlace renders half the scanlines (72 instead of 144), halving
     * __gb_draw_line work. Always on for scaled modes. Dynamic for 1:1. */
    gb.direct.interlace = (scale_mode != SCALE_1X);
    gb.direct.frame_skip = false; /* never use frame_skip (causes judder) */

    while (running) {
        int input_result = input_poll(&gb);
        if (input_result == INPUT_QUIT) { running = false; break; }
        if (input_result == INPUT_MENU) {
            int r = pgb_menu(&gb, &priv);
            if (r == INPUT_QUIT) { running = false; break; }
            rb->lcd_clear_display(); rb->lcd_update();
            cache_fb();
            fps_tick = *rb->current_tick; fps_frames = 0;
            last_fps_drawn = -1;
            autosave_tick = *rb->current_tick;
            gb.direct.interlace = (scale_mode != SCALE_1X);
            gb.direct.frame_skip = false;
            continue;
        }

        gb_run_frame(&gb);
        audio_submit_frame();
        pgb_lcd_update();

        fps_frames++;
        if (*rb->current_tick - fps_tick >= HZ) {
            fps_val = fps_frames;
            fps_frames = 0; fps_tick = *rb->current_tick;

            /* Dynamic interlace for 1:1 mode: enable when below 59fps,
             * disable when above 65fps. Wide gap prevents oscillation.
             * Scaled modes always keep interlace on. */
            if (scale_mode == SCALE_1X) {
                if (fps_val < 59 && !gb.direct.interlace)
                    gb.direct.interlace = true;
                else if (fps_val >= 65 && gb.direct.interlace)
                    gb.direct.interlace = false;
            }
        }
        if (show_fps && fps_val != last_fps_drawn) {
            if (show_fps == 2)
                rb->snprintf(fps_str, sizeof(fps_str), "%dfps", fps_val);
            else
                rb->snprintf(fps_str, sizeof(fps_str), "%d", fps_val);
            last_fps_drawn = fps_val;
        }
        if (show_fps) {
            /* Bottom-left corner */
            rb->lcd_putsxy(2, RB_LCD_HEIGHT - 10, fps_str);
            rb->lcd_update_rect(0, RB_LCD_HEIGHT - 12, 60, 12);
        }

        /* Frequent autosave check */
        if (autosave_mode == 2 && (*rb->current_tick - autosave_tick) >= AUTOSAVE_INTERVAL) {
            save_cart_ram(&priv);
            autosave_tick = *rb->current_tick;
        }
    }

    /* Save on exit if autosave is on */
    if (autosave_mode >= 1) save_cart_ram(&priv);

    audio_close();
#ifdef HAVE_ADJUSTABLE_CPU_FREQ
    rb->cpu_boost(false);
#endif
#ifdef HAVE_WHEEL_POSITION
    rb->wheel_send_events(true);
#endif
    backlight_use_settings();
    return PLUGIN_OK;
}
