/*
 * core/kernel/main.c — C entry point, called from boot/crt0.S.
 *
 * Boot bring-up (proven on real 5.5G hardware): UART banner, 30 MHz clock,
 * unified cache, 100 Hz tick + IRQs, then — if the BCM LCD is powered — the
 * disk/audio stack. From there kernel_main runs the FILE BROWSER: it mounts
 * the FAT32 volume, enumerates the root directory for playable tracks
 * (FLAC/MP3), and hands control to a clickwheel-driven list UI. Selecting a
 * track streams it off the disk through the read-ahead shim into the codec and
 * out the DMA-fed DAC; MENU stops and returns to the list.
 */

#include "hw/pp5022.h"
#include "hw/mmio.h"
#include "hw/uart.h"
#include "hw/lcd.h"
#include "hw/i2c.h"
#include "hw/wm8758.h"
#include "hw/i2s.h"
#include "hw/audio.h"
#include "hw/ata.h"
#include "hw/clickwheel.h"
#include "hal.h"
#include "../fs/fat32.h"
#include "sched.h"
#include "timer.h"
#include "irq.h"
#include "clock.h"
#include "cache.h"
#include "console.h"
#include "pcm_ring.h"
#include "../codecs/decoder.h"
#include "../codecs/arena.h"
#include "../codecs/readahead.h"
#include "../codecs/dr_flac/flac.h"
#include "../codecs/dr_mp3/mp3.h"

/*
 * Idle-task CPU sleep. Program the per-core countdown to wake this core
 * after ~`ms` milliseconds and halt until then (01-soc-pp5022.md, "Sleep
 * / wake"). We use PROC_WAIT_CNT (self-wakes on the countdown), NOT
 * PROC_SLEEP — with no interrupt controller installed yet, sleep-until-
 * interrupt would never wake. Only the CPU runs the kernel in Phase 1,
 * so CPU_CTL is correct. Three NOPs after the write per the doc's
 * pipeline rule.
 */
static void cpu_wait_ms(uint8_t ms) {
    mmio_write32(CPU_CTL_ADDR, PROC_WAIT_CNT | PROC_CNT_MSEC | ms);
    __asm__ volatile("nop\n\tnop\n\tnop");
}

/*
 * Streaming playback. The drive can't sustain uncompressed PCM over PIO
 * (~172 KB/s needed, ~173 KB/s ceiling), so we stream the COMPRESSED file
 * (FLAC ~40 KB/s, MP3 less) and decode on the fly. An SPSC ring decouples the
 * producer (decode_pump, foreground) from the consumer (the DMA-completion
 * ISR). dr_flac / dr_mp3 run freestanding on a static arena — no libc.
 */
#define RING_FRAMES   (1u << 16)         /* 65536 frames = 256 KB ~ 1.49 s      */
#define DECODE_FRAMES 4096u              /* frames per decode() call (prime)    */
#define PLAY_STEP_FRAMES 1024u           /* frames per decode step in the play   */
                                         /* loop: small so the loop returns to   */
                                         /* poll the wheel ~every 18ms (a 4096    */
                                         /* chunk blocked ~74ms, missing MENU).  */
#define ARENA_BYTES   (128u * 1024u)     /* MP3 arena high-water ~96 KB; FLAC   */
                                         /* ~40 KB. Sized for the larger.       */
#define RA_BYTES      (32u * 1024u)      /* read-ahead block buffer (see below) */

static int16_t    ring_storage[RING_FRAMES * 2];
static int16_t    decode_buf[DECODE_FRAMES * 2];        /* decoder output stage */
static uint8_t    arena_buf[ARENA_BYTES] __attribute__((aligned(8)));
static uint8_t    ra_buf[RA_BYTES]       __attribute__((aligned(8)));

static pcm_ring_t g_ring;
static decoder_t  g_dec;
static int        g_eos;                 /* decoder hit end of stream           */

/*
 * fat32-backed decoder_source_t: the decoder pulls its compressed input from a
 * file through this. fat32_stream is forward-only, so a backward seek re-opens
 * from the first cluster and skips forward — fine because the codecs only seek
 * during open() (to size the file / skip metadata), never in the decode loop.
 */
typedef struct {
    fat32_t       *fs;
    uint32_t       first_clus;
    uint32_t       fsize;
    fat32_stream_t st;
    uint32_t       phys;                 /* where the fat32 stream physically is */
    uint32_t       pos;                  /* logical position; a seek moves ONLY  */
                                         /* this — the physical walk is deferred */
} fat_src_t;

static fat_src_t g_fsrc;

/* Long-lived source structs: the decoder borrows a POINTER to the source it is
 * opened on (dr_flac/dr_mp3 stash it for their whole life), so these must
 * outlive the decode loop — hence file statics, not play_file locals. The
 * read-ahead shim (g_ra) wraps the raw fat_src source (g_file_src) and presents
 * the buffered source (g_dec_src) the decoder actually reads through. */
static decoder_source_t g_file_src;      /* raw fat_src bytes                    */
static readahead_t      g_ra;            /* block-buffering shim                 */
static decoder_source_t g_dec_src;       /* buffered source handed to the codec  */
static decoder_arena_t  g_arena;

static void fat_src_open(fat_src_t *s, fat32_t *fs, uint32_t clus, uint32_t sz)
{
    s->fs = fs;
    s->first_clus = clus;
    s->fsize = sz;
    s->phys = 0;
    s->pos = 0;
    fat32_stream_open(&s->st, fs, clus, sz);
}

/* Bring the physical stream up to the logical position — only when a read
 * actually needs data there. Forward is a cheap FAT-walk skip; backward
 * re-opens from the start then skips. This is what makes dr_flac's open-time
 * seek-to-EOF (to size the file) free: it never reads at EOF, so we never
 * physically walk there. */
static void fat_src_sync(fat_src_t *s)
{
    if (s->pos < s->phys) {              /* rewind: re-open at 0                 */
        fat32_stream_open(&s->st, s->fs, s->first_clus, s->fsize);
        s->phys = 0;
    }
    while (s->phys < s->pos) {           /* skip forward via the FAT chain       */
        uint32_t got = fat32_stream_skip(&s->st, s->pos - s->phys);
        if (got == 0) {
            break;
        }
        s->phys += got;
    }
}

static size_t fat_src_read(void *ud, void *buf, size_t bytes)
{
    fat_src_t *s = (fat_src_t *)ud;
    fat_src_sync(s);
    int32_t got = fat32_stream_read(&s->st, buf, (uint32_t)bytes);
    if (got <= 0) {
        return 0;
    }
    s->phys += (uint32_t)got;
    s->pos  += (uint32_t)got;
    return (size_t)got;
}
static int fat_src_seek(void *ud, int offset, int origin)
{
    fat_src_t *s = (fat_src_t *)ud;
    long target = (origin == DECODER_SEEK_SET) ? (long)offset
                : (origin == DECODER_SEEK_END) ? (long)s->fsize + offset
                                               : (long)s->pos + offset;
    if (target < 0 || (uint32_t)target > s->fsize) {
        return 0;
    }
    s->pos = (uint32_t)target;           /* lazy: physical move deferred to read */
    return 1;
}
static int64_t fat_src_tell(void *ud)
{
    return (int64_t)((fat_src_t *)ud)->pos;
}

/* hal_audio source (runs in the DMA ISR): drain the ring. A short return on
 * underrun makes the HAL zero-pad the rest — a glitch, never a stall. */
static int ring_source(void *ud, int16_t *buf, int frames)
{
    (void)ud;
    return (int)pcm_ring_read(&g_ring, buf, (uint32_t)frames);
}

/* Producer: decode (FLAC or MP3) into the ring until it's full or end-of-stream.
 * Codec-agnostic — it only calls g_dec.ops->decode, so the same loop drives
 * whichever decoder open() installed. Foreground only. */
static void decode_pump(void)
{
    while (!g_eos && pcm_ring_free(&g_ring) >= DECODE_FRAMES) {
        int got = g_dec.ops->decode(&g_dec, decode_buf, (int)DECODE_FRAMES);
        if (got <= 0) {
            g_eos = 1;
            break;
        }
        pcm_ring_write(&g_ring, decode_buf, (uint32_t)got);
    }
}

/* Decode at most ONE chunk into the ring, then return. Used inside the play
 * loop so decoding never monopolizes the loop: however slow the codec is (a
 * soft-float MP3 frame can take many ms), the loop still gets back to polling
 * the wheel and repainting each pass, so the UI never appears frozen. Returns
 * the frames decoded this step (0 at EOS or when the ring is already full). */
static int decode_step(void)
{
    if (g_eos || pcm_ring_free(&g_ring) < PLAY_STEP_FRAMES) {
        return 0;
    }
    int got = g_dec.ops->decode(&g_dec, decode_buf, (int)PLAY_STEP_FRAMES);
    if (got <= 0) {
        g_eos = 1;
        return 0;
    }
    pcm_ring_write(&g_ring, decode_buf, (uint32_t)got);
    return got;
}

/* FAT32 block callback: read absolute 512-byte LBAs off the disk.
 *
 * Retries a few times with a short wait: the drive spins down during a browse
 * idle, and the first PIO read after spin-down can error/time out while the
 * platter comes back up — which showed up as an intermittent "OPEN FAILED
 * FFFFFFFF" when starting a song after sitting in the list. A wait + retry
 * rides over the spin-up; a genuinely bad read still fails after all tries. */
static int disk_read(void *ud, uint32_t lba, uint32_t count, void *buf)
{
    (void)ud;
    for (int attempt = 0; attempt < 4; attempt++) {
        if (ata_read_sectors(lba, count, buf) == 0) {
            return 0;
        }
        sleep_ms(120);                   /* give a spun-down drive time to wake */
    }
    return -1;
}

/* ---------------------------------------------------------------------------
 * File browser
 * ------------------------------------------------------------------------- */

#define BROWSE_MAX 128
#define NAME_MAX   40                    /* one 40-cell console row             */

typedef struct {
    char     name[NAME_MAX + 1];         /* display name (uppercased, font-safe) */
    uint32_t clus;
    uint32_t size;
    uint8_t  fmt;                        /* 0 = FLAC, 1 = MP3 (only when !is_dir) */
    uint8_t  is_dir;                     /* 1 = subdirectory                     */
} browse_entry_t;

static browse_entry_t g_browse[BROWSE_MAX];
static int            g_browse_n;

/* Directory navigation: the cluster of the directory currently listed, plus a
 * stack of parent clusters so MENU can climb back up. Depth 0 == the root. */
#define DIR_STACK_MAX 12
static uint32_t g_cur_dir;
static uint32_t g_dir_stack[DIR_STACK_MAX];
static int      g_dir_depth;

/* Album art for the current folder. Each album folder carries a pre-scaled
 * "folder.art" sidecar (host tools/coreart.py): a CoreArt RGB565 bitmap the
 * device blits straight onto the now-playing screen — no on-device JPEG decode
 * or scaling. g_art_clus/size are captured while enumerating the folder;
 * load_folder_art() reads + validates it into g_art_raw once per play. */
#define ART_MAX_DIM  120
#define ART_HDR_LEN  12                  /* "CART" + u16 ver/w/h/reserved        */
static uint32_t g_art_clus, g_art_size;  /* folder.art location in the cur dir   */
static uint8_t  g_art_raw[ART_HDR_LEN + ART_MAX_DIM * ART_MAX_DIM * 2];
static int      g_art_ok, g_art_w, g_art_h;

/* Case-insensitive ASCII match of a dirent name against a literal. */
static int name_eq_ci(const char *a, const char *b)
{
    for (; *a && *b; a++, b++) {
        char ca = *a, cb = *b;
        if (ca >= 'a' && ca <= 'z') ca = (char)(ca - 32);
        if (cb >= 'a' && cb <= 'z') cb = (char)(cb - 32);
        if (ca != cb) return 0;
    }
    return *a == '\0' && *b == '\0';
}

/* Read + validate the captured folder.art into g_art_raw; sets g_art_ok/w/h. */
static void load_folder_art(fat32_t *fs)
{
    g_art_ok = 0;
    if (g_art_clus == 0 || g_art_size < ART_HDR_LEN ||
        g_art_size > sizeof g_art_raw) {
        return;
    }
    int32_t n = fat32_read_file(fs, g_art_clus, g_art_raw, g_art_size);
    if (n < (int32_t)ART_HDR_LEN) {
        return;
    }
    if (g_art_raw[0] != 'C' || g_art_raw[1] != 'A' ||
        g_art_raw[2] != 'R' || g_art_raw[3] != 'T') {
        return;
    }
    int w = g_art_raw[6] | (g_art_raw[7] << 8);
    int h = g_art_raw[8] | (g_art_raw[9] << 8);
    if (w <= 0 || h <= 0 || w > ART_MAX_DIM || h > ART_MAX_DIM) {
        return;
    }
    if ((int32_t)(ART_HDR_LEN + w * h * 2) > n) {
        return;
    }
    g_art_w = w;
    g_art_h = h;
    g_art_ok = 1;
}

/* Map a filename byte to a glyph the 8x8 font can draw: uppercase letters,
 * digits, and the few name punctuation marks; everything else becomes a
 * space. Keeps the list legible without a full ASCII font. */
static char disp_char(char c)
{
    if (c >= 'a' && c <= 'z') return (char)(c - 'a' + 'A');
    if (c >= 'A' && c <= 'Z') return c;
    if (c >= '0' && c <= '9') return c;
    if (c == '.' || c == '_' || c == '-') return c;
    return ' ';
}

/* MP3 playback is parked: dr_mp3's float synthesis can't hit real-time on this
 * FPU-less CPU (buffer starves -> stutter), and FLAC is lossless so there's no
 * quality reason to prefer it. The device is FLAC-only; a companion loader app
 * ensures music lands as FLAC. Flip to 1 to re-surface MP3 files (they'll open
 * but stutter) once a fixed-point/COP decoder exists. */
#define CORE_ENABLE_MP3 0

/* Classify by extension: 0 = FLAC (.fla/.flac), 1 = MP3 (.mp3), -1 = skip. */
static int classify_ext(const char *name)
{
    int dot = -1;
    for (int i = 0; name[i]; i++) {
        if (name[i] == '.') dot = i;
    }
    if (dot < 0) return -1;

    char ext[5];
    int n = 0;
    for (const char *e = name + dot + 1; *e && n < 4; e++) {
        char c = *e;
        if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
        ext[n++] = c;
    }
    ext[n] = '\0';

    if (n == 3 && ext[0] == 'F' && ext[1] == 'L' && ext[2] == 'A') return 0;
    if (n == 4 && ext[0] == 'F' && ext[1] == 'L' && ext[2] == 'A' && ext[3] == 'C') return 0;
#if CORE_ENABLE_MP3
    if (n == 3 && ext[0] == 'M' && ext[1] == 'P' && ext[2] == '3') return 1;
#endif
    return -1;
}

/* fat32_readdir callback: collect subdirectories + playable files into
 * g_browse. Directories are shown with a leading '>' marker (and rendered in
 * cyan); non-playable files are skipped. */
static int browse_collect(void *ud, const fat32_dirent_t *e)
{
    (void)ud;

    /* Capture the folder's album-art sidecar (not shown as a list row). */
    if (!e->is_dir && name_eq_ci(e->name, "folder.art")) {
        g_art_clus = e->first_clus;
        g_art_size = e->size;
        return 0;
    }

    if (g_browse_n >= BROWSE_MAX) return 1;   /* array full: stop enumeration */

    if (e->is_dir) {
        browse_entry_t *b = &g_browse[g_browse_n++];
        int i = 0;
        b->name[i++] = '>';                   /* dir marker glyph */
        for (const char *p = e->name; *p && i < NAME_MAX; p++) {
            b->name[i++] = disp_char(*p);
        }
        b->name[i] = '\0';
        b->clus   = e->first_clus;
        b->size   = 0;
        b->fmt    = 0;
        b->is_dir = 1;
        return 0;
    }

    int fmt = classify_ext(e->name);
    if (fmt < 0) return 0;
    browse_entry_t *b = &g_browse[g_browse_n++];
    int i = 0;
    for (; e->name[i] && i < NAME_MAX; i++) {
        b->name[i] = disp_char(e->name[i]);
    }
    b->name[i] = '\0';
    b->clus   = e->first_clus;
    b->size   = e->size;
    b->fmt    = (uint8_t)fmt;
    b->is_dir = 0;
    return 0;
}

#define LIST_TOP_ROW 2
#define LIST_ROWS    26                  /* rows LIST_TOP_ROW .. 27              */

/* Wheel scroll feel. The driver reports the raw differenced detent count (up to
 * ~half a rotation per poll), so adding it straight to the selection flung the
 * list. Accumulate detents and advance one row per WHEEL_CLICKS_PER_ITEM,
 * clamping a single event so a fast flick can't teleport to the end. */
#define WHEEL_CLICKS_PER_ITEM 3          /* higher = less sensitive             */
#define WHEEL_MAX_DELTA       6          /* max raw detents honoured per event  */

/* Draw one list row as a full-width bar so the selected entry reads as a solid
 * highlight (name padded with spaces to 40 cells). `fg_unsel` colours the row
 * when it isn't the selection (dirs cyan, files white). */
static void draw_row(int row, const char *s, int selected,
                     uint16_t fg_unsel, uint16_t bg)
{
    uint16_t fg = selected ? CON_BLACK : fg_unsel;
    uint16_t rb = selected ? CON_WHITE : bg;
    char line[NAME_MAX + 1];
    int i = 0;
    for (; s[i] && i < NAME_MAX; i++) line[i] = s[i];
    for (; i < NAME_MAX; i++)         line[i] = ' ';
    line[NAME_MAX] = '\0';
    console_str(0, row, line, fg, rb);
}

static void browse_render(int sel, int top, uint16_t bg)
{
    console_clear(bg);
    console_str(2, 0, "CORE PLAYER", CON_CYAN, bg);
    if (g_dir_depth > 0) {
        console_str(28, 0, "MENU:UP", CON_YELLOW, bg);   /* not at root */
    }

    if (g_browse_n == 0) {
        console_str(2, 4, "EMPTY FOLDER", CON_YELLOW, bg);
        return;
    }
    for (int r = 0; r < LIST_ROWS; r++) {
        int idx = top + r;
        if (idx >= g_browse_n) break;
        const browse_entry_t *e = &g_browse[idx];
        uint16_t fg = e->is_dir ? CON_CYAN : CON_WHITE;
        draw_row(LIST_TOP_ROW + r, e->name, idx == sel, fg, bg);
    }
}

/* Boot splash: centred CORE branding shown the moment the panel is ours, so
 * the disk-spin-up / mount delay reads as "loading" rather than the leftover
 * chainloader framebuffer. */
static void boot_splash(uint16_t bg)
{
    console_clear(bg);
    console_str(14, 12, "CORE PLAYER", CON_CYAN,   bg);   /* 11 chars, centred */
    console_str(16, 15, "LOADING",     CON_YELLOW, bg);   /*  7 chars, centred */
    lcd_present_fb(console_framebuffer());
}

/* Render two decimal digits at (col,row). */
static void draw_dd(int col, int row, uint32_t v, uint16_t fg, uint16_t bg)
{
    console_char(col,     row, (char)('0' + (v / 10) % 10), fg, bg);
    console_char(col + 1, row, (char)('0' + v % 10),        fg, bg);
}

/* Now-playing screen: title, track name, elapsed MM:SS / total MM:SS, and a
 * '='-bar progress meter. Elapsed is wall-clock since play start (the fixed
 * 1 MHz USEC_TIMER), which tracks the DAC closely enough for a UI. */
static void nowplaying_render(const browse_entry_t *b, uint32_t elapsed_s,
                              uint32_t total_s, uint32_t buf_pct, uint16_t bg)
{
    console_clear(bg);

    /* Pre-scaled album art (folder.art), centred near the top. Falls back to a
     * text header when the folder has no art sidecar. */
    if (g_art_ok) {
        int ax = (LCD_WIDTH - g_art_w) / 2;
        console_blit565(ax, 8, g_art_w, g_art_h,
                        (const uint16_t *)(g_art_raw + ART_HDR_LEN));
    } else {
        console_str(2, 1, "NOW PLAYING", CON_CYAN, bg);
    }

    /* Track name + metadata below the art (rows 17..25 clear the 120px art). */
    console_str(2, 17, b->name, CON_WHITE, bg);

    /* Clock line: "MM:SS / MM:SS" */
    draw_dd(2,  19, elapsed_s / 60, CON_WHITE, bg);
    console_char(4, 19, ':', CON_WHITE, bg);
    draw_dd(5,  19, elapsed_s % 60, CON_WHITE, bg);
    console_str(8, 19, "/", CON_WHITE, bg);
    draw_dd(10, 19, total_s / 60, CON_WHITE, bg);
    console_char(12, 19, ':', CON_WHITE, bg);
    draw_dd(13, 19, total_s % 60, CON_WHITE, bg);

    /* Progress bar over 36 cells. */
    int width = 36;
    int fill = (total_s > 0) ? (int)((elapsed_s * (uint32_t)width) / total_s) : 0;
    if (fill > width) fill = width;
    for (int i = 0; i < width; i++) {
        console_char(2 + i, 21, i < fill ? '=' : '-',
                     i < fill ? CON_GREEN : CON_WHITE, bg);
    }

    /* Buffer health: the ring's low-water fill % since the last repaint. Near
     * 100% => the decoder is comfortably ahead of the DAC. */
    console_str(2, 23, "BUF", buf_pct < 20u ? CON_RED : CON_GREEN, bg);
    draw_dd(6, 23, buf_pct > 99u ? 99u : buf_pct,
            buf_pct < 20u ? CON_RED : CON_GREEN, bg);

    console_str(2, 25, "MENU = STOP", CON_YELLOW, bg);
}

/*
 * Open, stream, and play one track. Returns 1 if the user pressed MENU to stop,
 * 0 if the track played to its end (or failed to open) — the browser uses that
 * to decide whether to auto-advance to the next track. Wraps the raw fat_src in
 * the read-ahead shim so the codec's many small header/tag reads collapse into
 * a few big disk reads (the fix for the ~27 s MP3 startup). Fixed 44.1 kHz /
 * stereo DAC path; a track that opens as anything else is skipped.
 */
static int play_file(fat32_t *fs, const browse_entry_t *b, uint16_t bg)
{
    /* Raw file source -> read-ahead shim -> codec. */
    fat_src_open(&g_fsrc, fs, b->clus, b->size);
    g_file_src.read = fat_src_read;
    g_file_src.seek = fat_src_seek;
    g_file_src.tell = fat_src_tell;
    g_file_src.userdata = &g_fsrc;
    readahead_init(&g_ra, &g_file_src, ra_buf, sizeof ra_buf);
    readahead_as_source(&g_ra, &g_dec_src);

    decoder_arena_init(&g_arena, arena_buf, sizeof arena_buf);
    decoder_alloc_t alloc = decoder_arena_allocator(&g_arena);
    int oc = (b->fmt == 1) ? mp3_open_stream(&g_dec, &g_dec_src, &alloc)
                           : flac_open_stream(&g_dec, &g_dec_src, &alloc);
    if (oc != 0) {
        console_clear(CON_RED);
        console_str(2, 3, "OPEN FAILED", CON_WHITE, CON_RED);
        console_hex32(2, 5, (uint32_t)oc, CON_WHITE, CON_RED);
        lcd_present_fb(console_framebuffer());
        sleep_ms(1000);
        return 0;
    }
    if (g_dec.sample_rate != 44100u || g_dec.channels != 2u) {
        console_clear(CON_RED);
        console_str(2, 3, "UNSUPPORTED RATE", CON_WHITE, CON_RED);
        console_hex32(2, 5, g_dec.sample_rate, CON_WHITE, CON_RED);
        lcd_present_fb(console_framebuffer());
        sleep_ms(1000);
        g_dec.ops->close(&g_dec);
        return 0;
    }

    uint32_t total_s = (g_dec.total_frames > 0)
                     ? (uint32_t)(g_dec.total_frames / 44100u) : 0;

    /* Load this folder's pre-scaled album art (captured during enumeration)
     * into RAM once, so the now-playing screen can blit it every frame. */
    load_folder_art(fs);

    /* Prime the ring, then start the DMA-fed DAC. */
    pcm_ring_init(&g_ring, ring_storage, RING_FRAMES);
    g_eos = 0;
    decode_pump();

    if (hal_audio_init(44100u, 2u) != 0) {
        g_dec.ops->close(&g_dec);
        return 0;
    }
    hal_audio_set_source(ring_source, 0);
    hal_audio_start();
    uint32_t start_us = mmio_read32(USEC_TIMER_ADDR);
    int stopped = 0;                     /* set when the user presses MENU */

    /* Play loop: decode ONE bounded chunk per pass (so a slow codec can't
     * freeze the UI), poll the wheel (MENU stops), and repaint the now-playing
     * screen once a second. Track the ring's low-water fill between repaints so
     * the BUF% readout exposes whether the decoder is keeping real-time.
     *
     * Present: the album art (y 8..128) is static for the whole track, so we
     * full-present ONCE (first paint) and thereafter partial-present only the
     * animated strip below it (clock / progress / BUF, y 128..240). That shrinks
     * the IRQ-masked pixel push ~3x each second — the full-frame present was
     * masking IRQs long enough to starve the audio DMA ISR (the DAC underran
     * even with the ring full), which is the ~5s FLAC stutter. Relies on the BCM
     * persisting the un-repainted art region across a partial LCD_UPDATE. */
    #define NP_ANIM_Y 128                /* first row below the 120px art        */
    #define NP_ANIM_H 112                /* 128..240                             */
    uint32_t last_shown = 0xFFFFFFFFu;
    uint32_t low_fill   = RING_FRAMES;   /* min ring fill seen since last repaint */
    int      first_paint = 1;
    while (!g_eos || pcm_ring_fill(&g_ring) > 0u) {
        decode_step();

        uint32_t fill = pcm_ring_fill(&g_ring);
        if (fill < low_fill) {
            low_fill = fill;
        }

        wheel_event_t ev;
        if (clickwheel_poll(&ev)) {
            if (ev.buttons & WHEEL_BTN_MENU) {        /* stop -> back to list */
                stopped = 1;
                break;
            }
        }

        uint32_t elapsed_s = (mmio_read32(USEC_TIMER_ADDR) - start_us) / 1000000u;
        if (elapsed_s != last_shown) {
            uint32_t buf_pct = (low_fill * 100u) / RING_FRAMES;
            nowplaying_render(b, elapsed_s, total_s, buf_pct, bg);
            if (first_paint) {
                lcd_present_fb(console_framebuffer());   /* art + text, once */
                first_paint = 0;
            } else {
                lcd_present_rect(console_framebuffer(),
                                 0, NP_ANIM_Y, LCD_WIDTH, NP_ANIM_H);
            }
            last_shown = elapsed_s;
            low_fill   = fill;           /* reset low-water for the next second */
        }
    }

    sleep_ms(200);                       /* let the FIFO/DMA drain the tail */
    hal_audio_stop();
    g_dec.ops->close(&g_dec);
    return stopped;
}

/* (Re)enumerate the directory at cluster `dir_clus` into g_browse. */
static void browse_load(fat32_t *fs, uint32_t dir_clus)
{
    g_cur_dir  = dir_clus;
    g_browse_n = 0;
    g_art_clus = 0;                      /* re-captured by browse_collect below */
    g_art_size = 0;
    fat32_readdir(fs, dir_clus, browse_collect, 0);
}

/* Play the file at index `start`, then auto-advance through the following FILES
 * in the current listing until the user presses MENU or the folder runs out.
 * Directories are skipped by auto-advance. Returns the index of the last track
 * touched so the caller can leave the selection there. */
static int play_from(fat32_t *fs, int start, uint16_t bg)
{
    int i = start;
    for (;;) {
        int stopped = play_file(fs, &g_browse[i], bg);
        if (stopped) {
            return i;                    /* user backed out here */
        }
        int nxt = -1;                    /* next non-dir entry after i */
        for (int j = i + 1; j < g_browse_n; j++) {
            if (!g_browse[j].is_dir) { nxt = j; break; }
        }
        if (nxt < 0) {
            return i;                    /* end of folder */
        }
        i = nxt;
    }
}

/*
 * The browser: list the current directory and handle the wheel. Wheel scrolls
 * the selection; SELECT descends into a highlighted folder or plays a
 * highlighted track (auto-advancing to the rest of the folder); MENU climbs
 * back up a directory. Starts at the root and never returns.
 */
_Noreturn static void browse_and_play(fat32_t *fs, uint16_t bg)
{
    clickwheel_init();
    g_dir_depth = 0;
    browse_load(fs, fs->root_clus);

    int sel = 0, top = 0;
    int wheel_accum = 0;
    browse_render(sel, top, bg);
    lcd_present_fb(console_framebuffer());

    for (;;) {
        wheel_event_t ev;
        int dirty = 0;
        if (clickwheel_poll(&ev)) {
            if (ev.wheel_delta && g_browse_n > 0) {
                int wd = ev.wheel_delta;
                if (wd >  WHEEL_MAX_DELTA) wd =  WHEEL_MAX_DELTA;
                if (wd < -WHEEL_MAX_DELTA) wd = -WHEEL_MAX_DELTA;
                wheel_accum += wd;
                int move = wheel_accum / WHEEL_CLICKS_PER_ITEM;
                wheel_accum -= move * WHEEL_CLICKS_PER_ITEM;
                if (move != 0) {
                    sel += move;
                    if (sel < 0)            sel = 0;
                    if (sel >= g_browse_n)  sel = g_browse_n - 1;
                    if (sel < top)              top = sel;
                    if (sel >= top + LIST_ROWS) top = sel - (LIST_ROWS - 1);
                    dirty = 1;
                }
            }
            if ((ev.buttons & WHEEL_BTN_SELECT) && g_browse_n > 0) {
                if (g_browse[sel].is_dir) {
                    /* Descend, remembering the parent so MENU can return. */
                    if (g_dir_depth < DIR_STACK_MAX) {
                        g_dir_stack[g_dir_depth++] = g_cur_dir;
                        browse_load(fs, g_browse[sel].clus);
                        sel = 0;
                        top = 0;
                    }
                } else {
                    sel = play_from(fs, sel, bg);
                    if (sel >= top + LIST_ROWS) top = sel - (LIST_ROWS - 1);
                }
                dirty = 1;               /* repaint the list afterwards */
            }
            if (ev.buttons & WHEEL_BTN_MENU) {
                /* Climb back up a directory (no-op at the root). */
                if (g_dir_depth > 0) {
                    browse_load(fs, g_dir_stack[--g_dir_depth]);
                    sel = 0;
                    top = 0;
                    dirty = 1;
                }
            }
        }
        if (dirty) {
            browse_render(sel, top, bg);
            lcd_present_fb(console_framebuffer());
        }
        /* Light throttle so the idle browser doesn't spin the panel/CPU. */
        for (volatile uint32_t d = 0; d < (1u << 16); d++) {
        }
    }
}

/* Per-task stack for the idle task (carved from .bss). Only reached if the LCD
 * is unpowered or the disk won't mount — the browser above never returns. */
#define TASK_STACK_SIZE 1024
static uint8_t idle_stack[TASK_STACK_SIZE];

/* Disk scratch (uint16_t for the 2-byte alignment ata_read_sectors needs). */
static uint16_t mbr_sector[1024];

/*
 * Idle task: never exits. Sleep the CPU for a short countdown, then yield.
 * The scheduler is only started on the no-LCD / mount-failure fallback path.
 */
_Noreturn static void idle_task(void) {
    uart_puts("core: idle task entered\n");
    for (;;) {
        cpu_wait_ms(10);
        sched_yield();
    }
}

_Noreturn void kernel_main(void) {
    /* Free-running 1 MHz microsecond counter (USEC_TIMER, 01-soc-pp5022.md
     * "Timers"), sampled at the very top so later milestones can report
     * elapsed-since-boot in real time, INDEPENDENT of the 100 Hz tick. */
    uint32_t boot_us0 = mmio_read32(USEC_TIMER_ADDR);

    uart_init();

    uart_puts("core: kernel alive (iPod 5G/5.5G, PP5022)\n");

    /* Hex-path self-test: if this doesn't read 1234ABCD on the terminal,
     * distrust every register dump that follows. */
    uart_puts("core: uart self-test ");
    uart_put_hex32(0x1234ABCD);
    uart_putc('\n');

    uart_puts("core: PROCESSOR_ID ");
    uart_put_hex32(PROCESSOR_ID);
    uart_putc('\n');

    /* Come off the 24 MHz boot clock up to CPUFREQ_NORMAL (30 MHz) before the
     * rest of bring-up so it runs at a sane speed. */
    uart_puts("core: clock init -> 30 MHz\n");
    clock_init();
    uart_puts("core: cpu freq ");
    uart_put_hex32(cpu_frequency());
    uart_putc('\n');

    /* Turn on the unified cache now — decode is far too slow with it off.
     * Write-back, so the audio DMA path flushes (cache_commit) before the DMA
     * reads a freshly-filled buffer. */
    uart_puts("core: cache init\n");
    cache_init();

    /* 100 Hz system tick + core IRQ unmask BEFORE audio: continuous playback is
     * DMA-driven and needs its completion interrupt delivered, and sleep_ms
     * spins on the IRQ-fed tick. */
    uart_puts("core: timer init @ 100 Hz, enabling IRQs\n");
    timer_init();
    arch_irq_enable();

    uart_puts("core: tick ");
    uart_put_hex32(current_tick());
    uart_puts(" (pre-sleep)\n");
    sleep_ms(100);
    uart_puts("core: tick ");
    uart_put_hex32(current_tick());
    uart_puts(" (post-sleep, ~+10)\n");

    /* LCD probe gates the whole disk/audio/UI stack: nonzero BCM power => real
     * hardware. The clicky emulator has no BCM (lcd_init() false there), so
     * this block is skipped, keeping the emulator smoke green; the register
     * grammar is proven host-side by the mock-bus trace tests. */
    if (lcd_init()) {
        /* Boost to 80 MHz for the whole disk/decode path. The UI runs here too;
         * we never drop back because the browser never returns (fine for
         * bring-up — the device is on a cable during testing). */
        cpu_boost();

        /* Paint the boot splash immediately, so the panel shows CORE branding
         * instead of the chainloader's leftover framebuffer (a blue field with
         * a green stripe) while the disk spins up and the volume mounts. */
        boot_splash(CON_BLACK);

        /* Read the MBR, find the FAT32 data partition (type 0B/0C), mount it. */
        uint8_t *mbr = (uint8_t *)mbr_sector;
        uint32_t sig = 0, fat_lba = 0;
        int      mnt = -1;
        fat32_t  fs;
        if (ata_init() == 0 && ata_read_sectors(0, 1, mbr) == 0) {
            sig = (uint32_t)mbr[510] | ((uint32_t)mbr[511] << 8);
            for (int p = 0; p < 4; p++) {
                const uint8_t *e = &mbr[0x1BE + 16 * p];
                if (e[4] == 0x0B || e[4] == 0x0C) {
                    fat_lba = (uint32_t)e[8] | ((uint32_t)e[9] << 8) |
                              ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
                    break;
                }
            }
        }
        if (sig == 0xAA55u && fat_lba != 0) {
            /* The MBR start-LBA may be in native 512-byte units OR (stock 80 GB,
             * 2048-byte FAT sectors) in 2048-byte units. Try as-is, then x4;
             * fat32_mount validates the boot signature + BPB. */
            mnt = fat32_mount(&fs, disk_read, 0, fat_lba);
            if (mnt != 0) {
                mnt = fat32_mount(&fs, disk_read, 0, fat_lba * 4u);
            }
        }

        uint32_t btus = mmio_read32(USEC_TIMER_ADDR) - boot_us0;
        uart_puts("core: mount rc ");
        uart_put_hex32((uint32_t)mnt);
        uart_puts(" BTUS ");
        uart_put_hex32(btus);
        uart_putc('\n');

        if (mnt == 0) {
            uart_puts("core: entering browser\n");
            browse_and_play(&fs, CON_BLACK);   /* never returns */
        }

        /* Mount failed: show a diagnostic error screen and fall through to the
         * idle scheduler so the device isn't a black brick. */
        console_clear(CON_RED);
        console_str  (2, 3, "MOUNT FAILED", CON_WHITE, CON_RED);
        console_str  (2, 6, "SIG",  CON_WHITE, CON_RED);
        console_hex32(8, 6, sig,               CON_WHITE, CON_RED);
        console_str  (2, 8, "MNT",  CON_WHITE, CON_RED);
        console_hex32(8, 8, (uint32_t)mnt,     CON_WHITE, CON_RED);
        console_str  (2, 10, "PART", CON_WHITE, CON_RED);
        console_hex32(8, 10, fat_lba,          CON_WHITE, CON_RED);
        lcd_present_fb(console_framebuffer());
        cpu_unboost();
    } else {
        uart_puts("core: lcd bcm NOT powered, skipping disk + UI\n");
    }

    /* Fallback only (no LCD or mount failure): idle the core under the
     * cooperative scheduler. sched_start never returns. */
    uart_puts("core: sched init (idle fallback)\n");
    sched_init();
    if (sched_add_task(idle_task, idle_stack, sizeof idle_stack, "idle") < 0) {
        uart_puts("core: FATAL sched_add_task failed\n");
        for (;;) {
        }
    }
    sched_start();
}
