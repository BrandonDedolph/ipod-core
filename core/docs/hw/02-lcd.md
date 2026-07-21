# 02 — LCD subsystem (BCM video coprocessor + panel)

The iPod Video does not drive the LCD panel directly. Instead, a
**Broadcom video coprocessor** (the "BCM" — exact model never
publicly identified) sits between the PP5022 and the LCD; it owns the
panel-specific init, framebuffer, and update timing, and exposes a
generic command/data interface to the host CPU.

This is convenient (we don't care about panel-specific gamma curves
or the difference between 5G and 5.5G panels — the BCM hides it) and
inconvenient (the BCM has no datasheet; everything below is reverse-
engineered from the Apple firmware blob it loads at boot).

## Key insight: no runtime panel detection

The BCM firmware (a binary blob in the iPod's `vmcs` ROM section)
already knows whether this is a 5G or 5.5G panel. The host CPU does
not need to detect; it just uploads the blob, then issues `LCD_UPDATE`
commands. **Same Rockbox build runs on both panels** because the
BCM's loaded firmware varies, not the driver.

This means we don't have to re-implement panel-specific LCD register
sequences. We just need to drive the BCM correctly.

## Memory-mapped BCM interface

The BCM occupies `0x30000000`–`0x30070000` of MMIO. Each "register"
is actually a port; the 16-bit and 32-bit "aliases" below are the
**same address**, not separate ones. The BCM decodes only PP address
bits 16..18 — the low address bits are undecoded — so a 32-bit store
(or an `stmia` burst) to a port is consumed as consecutive 16-bit
pushes. This is why the pixel-stream fast path can blast 32-bit
words (two RGB565 pixels each) at `0x30000000` without per-pixel
addressing. *(Corrected 2026-06-11 against Rockbox `lcd-video.c` /
ipodloader2 `fb.c`: an earlier revision called these "separate
addresses for 16-bit and 32-bit access aliases".)*

| Name             | Address      | Width | Purpose |
|------------------|--------------|-------|---------|
| `BCM_DATA`       | `0x30000000` | 16    | Data port (writes pixel/parameter data) |
| `BCM_DATA32`     | `0x30000000` | 32    | 32-bit alias of `BCM_DATA` |
| `BCM_WR_ADDR`    | `0x30010000` | 16    | Set write destination (in BCM internal mem) |
| `BCM_WR_ADDR32`  | `0x30010000` | 32    | 32-bit alias |
| `BCM_RD_ADDR`    | `0x30020000` | 16    | Set read source |
| `BCM_RD_ADDR32`  | `0x30020000` | 32    | 32-bit alias |
| `BCM_CONTROL`    | `0x30030000` | 16    | Status flags + command strobe |
| `BCM_ALT_DATA`   | `0x30040000` | 16    | Alternate channel (used during bootstrap) |
| `BCM_ALT_WR_ADDR`| `0x30050000` | 16    | Alt write addr (bootstrap) |
| `BCM_ALT_RD_ADDR`| `0x30060000` | 16    | Alt read addr (bootstrap) |
| `BCM_ALT_CONTROL`| `0x30070000` | 16    | Alt control (bootstrap handshake) |

Source: `firmware/target/arm/ipod/video/lcd-video.c` lines 40–57.

### `BCM_CONTROL` (`0x30030000`)

| Bit / value     | Meaning |
|-----------------|---------|
| `0x02`          | Write ready — BCM can accept new write addr/data |
| `0x10`          | Read ready — BCM has data on `BCM_DATA32` |
| Write `0x31`    | Strobe to execute the queued command |

The read handshake (verified against Rockbox `lcd-video.c`
`bcm_read32`, 2026-06-11 — an earlier revision implied the data was
read "on `BCM_RD_ADDR`"):

1. Poll `BCM_RD_ADDR` (16-bit read) until bit 0 is set — read port
   ready to accept an address.
2. Write the BCM-internal address as a 32-bit store to `BCM_RD_ADDR32`.
3. Poll `BCM_CONTROL` until `& 0x10` — data available.
4. Read the value as a 32-bit load from `BCM_DATA32`.

The write handshake: write the address as a 32-bit store to
`BCM_WR_ADDR32`, then poll `BCM_CONTROL` until `& 0x02` before
pushing data at `BCM_DATA32`.

## Internal BCM addresses

The BCM has its own SDRAM. Three absolute addresses (all in BCM-internal
memory, addressed via `bcm_write_addr` / `bcm_write32`) matter to us:

| Symbol           | Absolute addr | Purpose |
|------------------|---------------|---------|
| `BCMA_CMDPARAM`  | `0xE0000`     | Command parameter region — also serves as the framebuffer for `LCD_UPDATE` (full-frame writes start here; `LCD_UPDATERECT` puts its 8-word rect header here with pixels at `0xE0020`, see below) |
| `BCMA_COMMAND`   | `0x1F8`       | Command-code register — write the `BCM_CMD(...)` encoded value here, then strobe `BCM_CONTROL = 0x31` to execute |
| `BCMA_STATUS`    | `0x1FC`       | Status word — read by the finishup/poll path after a command (verified against ipodloader2 `fb.c`, 2026-06-11) |

Pixel `(x, y)` in the framebuffer is at offset `(LCD_WIDTH * 2) * y +
(x * 2)` from `0xE0000`, RGB565 little-endian.

The framebuffer-as-parameter-region overlap is intentional: for the
full-frame `LCD_UPDATE` command the BCM expects the full 153,600 bytes
of pixel data starting at `BCMA_CMDPARAM`, then the command itself is
written to `BCMA_COMMAND` (a separate register, not an offset).

## Commands

Commands are written as a magic-encoded constant: `BCM_CMD(x) =
((~x << 16) | x)`. The encoding makes the BCM's own command parser
reject corrupt writes (it checks the bit-inverse against the low half).

| Command            | Code           | Purpose |
|--------------------|----------------|---------|
| `BCMCMD_LCD_UPDATE`| `BCM_CMD(0)`   | Update entire 320×240 frame from `0xE0000` |
| `BCMCMD_SELFTEST`  | `BCM_CMD(1)`   | M25 diagnostics; <40 s, displays status on-panel |
| `BCMCMD_TV_PALBMP` | `BCM_CMD(2)`   | Output PAL test pattern on TV out |
| `BCMCMD_TV_NTSCBMP`| `BCM_CMD(3)`   | Output NTSC test pattern |
| `BCMCMD_LCD_UPDATERECT` | `BCM_CMD(5)` | Partial-region update — 8-word param header at `0xE0000`, pixels at `0xE0020` (see "Partial updates" below) |
| `BCMCMD_LCD_SLEEP` | `BCM_CMD(8)`   | Blank LCD, low-power |
| `BCM_CMD(0xC)`     | `BCM_CMD(12)`  | Some shutdown sub-step (undocumented) |
| `BCMCMD_TV_MVOFF`  | `BCM_CMD(0xE)` | Disable Macrovision on TV out |

Source: `lcd-video.c` lines 74–89.

## Bootstrap and firmware upload

Cold boot or post-`ROLO` (Rockbox-on-Rockbox reboot): the BCM is
powered off and needs its firmware re-uploaded.

```c
// Stage 1 — power
GPO32_VAL |= 0x4000;
sleep(HZ/20);                    // 50 ms

STRAP_OPT_A &= ~0xF00;           // clear strap config; STRAP_OPT_A = 0x70000008
outl(0x1313, 0x70000040);        // strap pins for boot

// Stage 2 — wait for BCM
while (BCM_ALT_CONTROL & 0x80) ;
while (!(BCM_ALT_CONTROL & 0x40)) ;

// Send 8-byte handshake
static const u8 boot_seq[8] = {
    0xA1, 0x81, 0x91, 0x02, 0x12, 0x22, 0x72, 0x62
};
for (i = 0; i < 8; i++)  BCM_CONTROL     = boot_seq[i];
for (i = 3; i < 8; i++)  BCM_ALT_CONTROL = boot_seq[i];

// Post-handshake sync (verified against Rockbox lcd-video.c,
// 2026-06-11): wait until both read ports come ready, then
// dummy-read the write-address ports to flush them.
while (!((BCM_RD_ADDR & 1) && (BCM_ALT_RD_ADDR & 1))) ;
(void)BCM_WR_ADDR;
(void)BCM_ALT_WR_ADDR;

// Stage 3 — upload firmware blob to BCM SRAM
while (BCM_ALT_CONTROL & 0x80) ;
while (!(BCM_ALT_CONTROL & 0x40)) ;

bcm_write_addr(BCMA_SRAM_BASE);  // 0x0 in BCM-internal mem
// Upload length rounds to an even number of 16-bit units:
// ((flash_vmcs_length + 3) >> 1) & ~1.
lcd_write_data(flash_vmcs_offset, flash_vmcs_length);

// Initialize BCM processor
bcm_write32(BCMA_COMMAND, 0);
bcm_write32(0x10000C00, 0xC0000000);     // map BCM SDRAM
while (!(bcm_read32(0x10000C00) & 1)) ;  // wait for mapping
bcm_write32(0x10000C00, 0);

// Start BCM firmware execution
bcm_write32(0x10000400, 0xA5A50002);
while (bcm_read32(BCMA_COMMAND) == 0) yield();
```

The firmware blob (`vmcs` section) is in the iPod's flash ROM; located
via the flash directory at `0x200FFE00` (ROM base `0x20000000` +
`0xFFE00`). `flash_get_section(ROM_ID('v','m','c','s'), ...)` finds it. Its length and offset are stored in `flash_vmcs_offset` / `flash_vmcs_length`.

*(Corrected 2026-06-11 against Rockbox `lcd-video.c`: an earlier
revision gave the directory address as `0x20FFE00`, dropping a zero —
the flash ROM is mapped at `0x20000000`, not `0x2000000`.)*

**Magic constants we don't fully understand:**
- `0x10001400` — modified during shutdown (mask `0xF0`).
- `0x10000C00` — BCM SDRAM mapping control.
- `0x10000400` — BCM firmware startup trigger; magic value `0xA5A50002`.

These came from iPodLinux reverse-engineering and aren't documented
beyond "this is what works."

Source: `lcd-video.c` lines 539–599.

## Host-side port init (always runs)

Separate from the BCM bootstrap, Rockbox `lcd_init_device()` does a
host-side GPIO/port setup that runs **even when the BCM is already
alive** (verified against Rockbox `lcd-video.c`, 2026-06-11):

```c
GPO32_ENABLE     |=  0xC000;   // BCM power rail + companion bit as GPO
GPIOC_ENABLE     &= ~0x80;     // release C7
GPIOC_ENABLE     |=  0x40;     // C6 = BCM interrupt pin, GPIO mode
GPIOC_OUTPUT_EN  &= ~0x40;     //   ... as input
GPO32_ENABLE     &= ~1;        // release GPO32 bit 0
```

Probe for a live BCM: `GPO32_VAL & 0x4000` nonzero ⇒ the BCM is
powered (and, after a chainload, already initialized — see below).

## Chainload handoff state

ipodloader2 performs **no BCM bootstrap** — the Apple flash ROM has
already powered the BCM and uploaded its firmware by the time any
loader runs. ipodloader2's `fb.c` just issues update commands, and it
finishes its final frame **synchronously** (it polls completion before
jumping to the loaded image). So at handoff to us the BCM is powered,
awake, and idle, with the panel initialized. ipodloader2 also leaves
the backlight **on** when booting a non-Apple image. (Verified against
ipodloader2 `fb.c`, 2026-06-11.)

Rockbox's `BOOTLOADER` build exploits the same guarantee with a
simplified update variant: stream this frame's params/data first, wait
for the *previous* update to go idle **before issuing the command**
(skippable on the very first frame, since handoff guarantees idle),
write the command, strobe `0x31`, and return **without** waiting for
completion. Our Phase-1 driver follows this bootloader variant.

**Ordering matters — the wait goes AFTER the pixel stream, not before
it.** In Rockbox the ~150 KB stream (`lcd_update_rect`) runs before the
idle poll (`lcd_unblock_and_update`), so by the time it polls, the prior
update has retired and the poll returns on its first read. Polling right
after the previous strobe — before streaming — instead hits the BCM at
peak busy and spins its whole budget, which on real 5.5G hardware
presents as the "second present stalls" hang (only the first frame ever
shows). Our driver keeps the wait in `bcm_frame_commit` (after the
caller's stream), and re-issues `LCD_UPDATE` + strobe if the BCM is
still busy past a bounded poll budget — the analog of Rockbox's 50 ms
`BCM_UPDATE_TIMEOUT` re-kick for a latched-busy BCM.

## LCD update protocol

After modifying the host-side framebuffer, host calls `lcd_update()` /
`lcd_update_rect(x, y, w, h)`. The function copies host pixels into
BCM internal memory, then issues `LCD_UPDATE`.

```c
void lcd_update_rect(int x, int y, int w, int h) {
    if (x + w >= LCD_WIDTH)  w = LCD_WIDTH  - x;
    if (y + h >= LCD_HEIGHT) h = LCD_HEIGHT - y;

    // Width and x must be even (BCM bus alignment)
    w = (w + (x & 1) + 1) & ~1;
    x &= ~1;

    lcd_block_tick();           // serialize against the periodic update kick

    fb_data *src = FBADDR(x, y);
    u32 dst = BCMA_CMDPARAM + (LCD_WIDTH*2)*y + (x*2);

    bcm_write_addr(dst);
    if (w == LCD_WIDTH) {
        lcd_write_data(src, w*h);
    } else {
        do {
            bcm_write_addr(dst);
            dst += LCD_WIDTH*2;
            lcd_write_data(src, w);
            src += LCD_WIDTH;
        } while (--h > 0);
    }

    lcd_unblock_and_update();   // tick will issue LCD_UPDATE on the next pass
}
```

The actual command issuance happens in the periodic tick:

```c
u32 stat = bcm_read32(BCMA_COMMAND);
bool busy = (stat == BCMCMD_LCD_UPDATE || stat == 0xFFFF);
if (update_pending && !busy) {
    bcm_write32(BCMA_COMMAND, BCMCMD_LCD_UPDATE);
    BCM_CONTROL = 0x31;                         // strobe execute
    update_timeout = current_tick + BCM_UPDATE_TIMEOUT;  // 50 ms
}
```

If the BCM doesn't return to idle within 50 ms (`HZ/20`), the tick
re-kicks the update — Rockbox sees occasional stalls in practice.

After waking from sleep, the first update can take up to 500 ms
(`BCM_LCDINIT_TIMEOUT = HZ/2`) because the BCM is doing internal LCD
panel init.

### Constraints
- `x` and `width` must be even (BCM bus alignment). The driver
  rounds them — `x` down and `width` up to cover the requested
  rect — rather than odd values being "silently ignored".
- The data write throughput must keep ahead of BCM consumption; if
  not, BCM stalls (no observed bug in practice).

### Partial updates (`BCMCMD_LCD_UPDATERECT`)

Rockbox always does full-frame updates, but a working partial-rect
protocol ships in ipodloader2 `fb.c` (inherited from iPodLinux;
verified 2026-06-11):

1. `bcm_write_addr(0xE0000)`, then push an 8-word parameter header:

   ```c
   { 0x34,                 // header magic / opcode
     start_horiz,          // left   (inclusive)
     start_vert,           // top    (inclusive)
     max_horiz,            // right  (inclusive)
     max_vert,             // bottom (inclusive)
     count_bytes,          // pixel byte count
     count_bytes,          // repeated
     0 }
   ```

2. Stream the rect's pixel data starting at internal addr `0xE0020`.
3. `bcm_write32(0x1F8, 0xFFFA0005)` — `BCM_CMD(5)`.
4. Strobe `BCM_CONTROL = 0x31`.

*(Corrected 2026-06-11: an earlier revision said the params were
expected at `0xE00000` with unknown layout. That `0xE00000` figure
survives only as unverified upstream speculation — the shipped
ipodloader2 layout above is at `0xE0000`/`0xE0020`.)*

### Partial present — what our driver actually does

Our `lcd_present_rect(fb, x, y, w, h)` does **not** use
`BCMCMD_LCD_UPDATERECT` (`BCM_CMD(5)`). It uses the safer, better-proven
mechanism the Rockbox `lcd_update_rect` in "LCD update protocol" above
uses: the BCM holds a **persistent** 320×240 framebuffer in its own
SDRAM at `BCMA_CMDPARAM` (`0xE0000`), so we overwrite **only the changed
pixels there**, at their normal full-frame stride offsets, then issue the
ordinary full-frame `BCMCMD_LCD_UPDATE` (`BCM_CMD(0)`). The BCM scans out
its whole framebuffer; the pixels we didn't touch keep last frame's
values. This trims the ~150 KB pixel upload (the slow PP→BCM bus is the
real cost) without a new, less-certain command, and it reuses our
device-proven commit handshake (idle-wait **after** the stream, re-kick,
`LCD_UPDATE`, `0x31` strobe) unchanged.

Exact BCM traffic our `lcd_present_rect` emits (all addresses are
BCM-internal; `stride = LCD_WIDTH*2 = 640` bytes):

- **Full-width rect** (`w == LCD_WIDTH`, the common row-band case): the
  rows are contiguous in BCM memory, so it is one address + one stream:
  1. `bcm_write_addr(BCMA_CMDPARAM + y*stride)`
  2. stream `w*h` pixels contiguously from `fb + y*LCD_WIDTH`, packed two
     RGB565 per 32-bit store (low half = even/earlier pixel).
- **Narrower rect** (`w < LCD_WIDTH`): each destination row is separated
  by a full-width gap, so re-address per row:
  - for `r` in `0..h-1`: `bcm_write_addr(BCMA_CMDPARAM + (y+r)*stride +
    x*2)` then stream that row's `w` pixels from `fb + (y+r)*LCD_WIDTH +
    x`.
- then the shared commit: wait-for-idle (skipped on first frame),
  `bcm_write32(BCMA_COMMAND, BCMCMD_LCD_UPDATE)`, strobe
  `BCM_CONTROL = 0x31`.

`x` and `w` are forced even before streaming (BCM bus alignment — a
32-bit store pushes two pixels, so a row must start on an even column and
span an even count): `w = (w + (x&1) + 1) & ~1; x &= ~1;` (`x` down, `w`
up, so the rounded rect still covers the request), matching the
"Constraints" rounding above. A full-frame rect
(`x=0,y=0,w=LCD_WIDTH,h=LCD_HEIGHT`) reduces to offset 0 + a contiguous
`LCD_WIDTH*LCD_HEIGHT` stream, i.e. **byte-identical** to the full-frame
`lcd_present_fb` path — which is why `lcd_present_fb` just calls
`lcd_present_rect(fb, 0, 0, LCD_WIDTH, LCD_HEIGHT)`.

**Needs on-device visual confirmation** that the BCM framebuffer indeed
persists untouched regions across a partial `LCD_UPDATE` (Rockbox relies
on this, but our full-frame path has never depended on it). Source of the
mechanism: `lcd_update_rect` / `lcd_unblock_and_update` in "LCD update
protocol" above (Rockbox `lcd-video.c`, verified 2026-06-11).

## Tearing

There is no vblank synchronization. The BCM may begin reading the
framebuffer before the CPU finishes writing — bottom of the frame can
show stale pixels for one frame.

Mitigation in Rockbox: `lcd_block_tick()` prevents *concurrent*
updates (two `LCD_UPDATE` commands in flight), but not within-update
tearing. In practice tearing is rarely visible because the BCM scans
fast and the host writes are bursty.

For us: if we want tear-free rendering we'd need to time `LCD_UPDATE`
relative to the BCM's own scan. There's no documented vsync IRQ, but
the tick callback could be a starting point.

## Backlight control

Backlight is **not** in the LCD path — it's a separate GPIO PWM,
duplicated below for completeness from `firmware/target/arm/ipod/backlight-nano_video.c`.

| GPIO          | Bit  | Purpose |
|---------------|------|---------|
| `GPIOB`       | 0x08 | Backlight circuit enable |
| `GPIOD`       | 0x80 | Backlight dimmer (PWM by software toggle) |
| `GPIOL`       | 0x80 | Backlight LED on/off |

### Brightness (1–32 range)

Brightness is set by manually pulse-toggling `GPIOD[7]` while
adjusting the up/down direction:

```c
if (current_dim < val) {
    do {
        disable_irq();
        GPIO_CLEAR_BITWISE(GPIOD_OUTPUT_VAL, 0x80);
        udelay(10);
        GPIO_SET_BITWISE(GPIOD_OUTPUT_VAL, 0x80);
        restore_irq();
        udelay(10);
    } while (++current_dim < val);
} else if (current_dim > val) {
    do {
        disable_irq();
        GPIO_CLEAR_BITWISE(GPIOD_OUTPUT_VAL, 0x80);
        udelay(200);                              // long low → dimmer
        GPIO_SET_BITWISE(GPIOD_OUTPUT_VAL, 0x80);
        restore_irq();
        udelay(10);
    } while (--current_dim > val);
}
```

`udelay` is **microseconds**: each step is ~20 µs going up
(10 + 10 µs) and ~210 µs going down (200 + 10 µs) — not "~10 ms" as
an earlier revision claimed (corrected 2026-06-11). The level is
preserved internally in the backlight driver chip; the GPIO toggles
are commands to it.

### Enable / disable

```c
void backlight_enable(bool on) {
    if (on) {
        GPIO_SET_BITWISE  (GPIOB_OUTPUT_VAL, 0x08);
        GPIO_SET_BITWISE  (GPIOD_OUTPUT_VAL, 0x80);
        sleep(HZ/100);                  // 10 ms settle
        current_dim = 16;               // assumed default
        backlight_brightness(brightness);
    } else {
        GPIO_CLEAR_BITWISE(GPIOD_OUTPUT_VAL, 0x80);
        GPIO_CLEAR_BITWISE(GPIOB_OUTPUT_VAL, 0x08);
        sleep(HZ/20);                   // 50 ms off
    }
}
```

Init also configures the GPIOs as outputs and sets `GPIOL[7]` (LED
master enable) high.

## Sleep / wake

LCD sleep blanks the panel and powers down the BCM completely (LCD
won't wake without a full bootstrap re-run).

```c
void bcm_powerdown(void) {
    backlight_enable(false);
    bcm_write32(0x10001400, bcm_read32(0x10001400) & ~0xF0);
    bcm_command(BCMCMD_LCD_SLEEP);   // blank panel
    bcm_command(BCM_CMD(0xC));       // additional shutdown step (undocumented)
    GPO32_VAL &= ~0x4000;            // cut BCM power
}
```

Wake is the bootstrap sequence again, plus a special "first update"
that doubles as LCD-controller init:

```c
void lcd_awake(void) {
    if (!display_on && flash_vmcs_length != 0) {
        // Ensure ≥ 50 ms since last power-off (avoid power glitches)
        long w = update_timeout + HZ/20 - current_tick;
        if (w > 0 && w < HZ/20) sleep(w);

        bcm_init();                         // full re-bootstrap

        state = LCD_INITIAL;
        display_on = true;
        lcd_update();
        update_timeout = current_tick + BCM_LCDINIT_TIMEOUT;  // 500 ms

        // Wait for first update before turning backlight on
        waking = true;
        tick_add_task(&lcd_tick);
        semaphore_wait(&initwakeup, TIMEOUT_BLOCK);

        send_event(LCD_EVENT_ACTIVATION, NULL);
    }
}
```

If we wake the backlight before the first update completes, the user
sees a 500 ms white flash.

## Things Rockbox doesn't implement

| Feature              | Status   |
|----------------------|----------|
| Rotation / flip      | Stub returns silently |
| Invert display       | Stub returns silently |
| Contrast control     | Stub returns silently |
| Partial update       | Command exists; params unknown |
| Software dither      | Used only on the YUV blit fast path |

## YUV fast path

For motion-JPEG video playback, Rockbox has a hand-tuned ARM assembly
routine (`lcd-as-video.S`) that converts YUV420 → RGB565 and writes
directly to the BCM, processing 16 pixels per loop iteration. Not
relevant for music UI but worth knowing exists.

YUV→RGB565 coefficients:

```
R = (74*(Y - 16)             + 101*(Cr - 128)) >> 9
G = (74*(Y - 16) -  24*(Cb - 128) -  51*(Cr - 128)) >> 8
B = (74*(Y - 16) + 128*(Cb - 128))                  >> 9
```

## Source citations

| Topic                | File |
|----------------------|------|
| Driver               | `firmware/target/arm/ipod/video/lcd-video.c` |
| ASM data write       | `firmware/target/arm/ipod/video/lcd-as-video.S` |
| Backlight            | `firmware/target/arm/ipod/backlight-nano_video.c` |
