# 03 — Click wheel + buttons + hold switch

The iPod Video's click wheel is a capacitive touch sensor (Synaptics
or compatible) that streams 32-bit packets to the SoC over a parallel
memory-mapped interface. Each packet contains an absolute angular
position (0–95) plus the state of five buttons. Quadrature decoding,
debounce, and acceleration are all done in software.

The hold switch is a separate GPIO; when held, the wheel block is
power-gated entirely.

## Hardware interface

| Register         | Address      | Width  | Purpose |
|------------------|--------------|--------|---------|
| `CLICKWHEEL_DATA`| `0x7000C140` | 32 bits | Packet read; valid when bit 31 set |
| (control A)      | `0x7000C100` | 32 bits | Configuration / interrupt enable |
| (control B)      | `0x7000C104` | 32 bits | Interrupt acknowledge |
| `DEV_OPTO`       | bit in `DEV_EN` (`0x6000600C`) | — | Power gate for wheel block |

Source: `firmware/target/arm/ipod/button-clickwheel.c` lines 54–60.

## Packet format

Each 32-bit word read from `CLICKWHEEL_DATA`:

```
 31     30     21:16     12:8       7:0
 ┌───┬─────┬──────────┬─────────┬────────┐
 │ V │  T  │ position │ buttons │ flags  │
 └───┴─────┴──────────┴─────────┴────────┘
```

- Bit 31 (V) — valid (always set on real packets).
- Bit 30 (T) — touch present (finger on wheel).
- Bits 21–16 — absolute angular position, 0–0x5F (96 steps full circle).
- Bits 12–8 — button state bitmap (see below).
- Bits 7–0 — flags / packet type. Valid header check on PP5020:
  `(status & 0x800000FF) == 0x8000001A`.

Each angular step ≈ 3.75°.

## Button bits

| Button   | Bit | Mask        |
|----------|-----|-------------|
| SELECT (center) | 8  | `0x00000100` |
| RIGHT    | 9   | `0x00000200` |
| LEFT     | 10  | `0x00000400` |
| PLAY     | 11  | `0x00000800` |
| MENU     | 12  | `0x00001000` |

All five buttons are encoded in the same packet as the wheel — there
are *no* separate GPIO pins for them. Hardware debouncing happens in
the wheel chip; the firmware adds a 50 µs read delay after the
interrupt fires.

## Quadrature / wheel-delta decoding

(`button-clickwheel.c` lines 159–179.) The wheel reports absolute
position; relative motion is computed in software:

```c
delta = new - old;
if (delta < -48) delta += 96;   // wrap forward through 0x5F → 0x00
if (delta >  48) delta -= 96;   // wrap backward through 0x00 → 0x5F

if (delta >=  WHEEL_SENSITIVITY) emit(BUTTON_SCROLL_FWD);
if (delta <= -WHEEL_SENSITIVITY) emit(BUTTON_SCROLL_BACK);

old = new;
```

`WHEEL_SENSITIVITY` = 4 on the Video (it's 6 on the Nano — the Nano's
wheel is smaller).

## Hold switch

GPIO A bit 5 at `GPIOA_INPUT_VAL` (`0x6000D030`).

| Polarity (Video) | Logic |
|------------------|-------|
| held             | bit clear (active-low) |
| unlocked         | bit set |

```c
bool held = !(GPIOA_INPUT_VAL & 0x20);
```

When held, the firmware power-gates the wheel block:

```c
DEV_EN &= ~DEV_OPTO;
```

(This is what makes the wheel feel "dead" while held.)

The bootloader checks the hold switch immediately after CPU init to
decide whether to boot Apple's firmware (held) or our firmware
(released). See [08-boot-dock.md](08-boot-dock.md).

## Acceleration / velocity tracking

Software-only; the hardware just streams packets at ~10 ms cadence.

(`button-clickwheel.c` lines 183–198.) Each event computes velocity in
degrees/second and applies an exponential moving average with α = 1/16:

```c
long usec_now = read_microsecond_timer();
long dt = (usec_now - last_usec) & 0x7FFFFFFF;

long inst = dt ? (1000000L * delta) / dt : 0;   // clicks/sec
inst = (inst * 360) / 96;                        // → deg/sec

velocity = (15 * velocity + inst) / 16;          // EMA
if (velocity > 0xFFFFFF) velocity = 0xFFFFFF;    // 24-bit clamp

last_usec = usec_now;
```

Acceleration is reset on:

- **Direction reversal** — `velocity = 0`, accumulator = 0.
- **Time gap > 250 ms** (`WHEEL_REPEAT_TIMEOUT`) — `velocity = 0`.
- **Finger lift > 150 ms** (`WHEEL_UNTOUCH_TIMEOUT`) — full reset.

The 150 ms touch-lost threshold is intentional: brief lifts during
fast scrolling (when the user momentarily loses contact) shouldn't
reset the acceleration state.

## Event posting

The wheel ISR posts one event per qualifying packet to the global
button queue, with the velocity in the message bits:

```c
button_queue_post(
    wheel_keycode | repeat_flag,
    (1 << 31)   |    // accel-applies flag
    (1 << 24)   |    // post count = 1
    velocity         // lower 24 bits, deg/sec
);
```

The UI layer reads bit 31 to decide whether to apply velocity-based
list scrolling (one wheel-tick maps to N items where N is a function
of velocity).

## Initialization

(`opto_i2c_init`, `button-clickwheel.c` lines 95–105.)

```c
DEV_EN |= DEV_OPTO;          // power up
DEV_RS |= DEV_OPTO;          // assert reset
udelay(5);
DEV_RS &= ~DEV_OPTO;         // release reset

DEV_INIT1 |= INIT_BUTTONS;   // enable button latch logic

outl(0xC00A1F00, 0x7000C100);   // control A — config + IRQ enable
outl(0x01000000, 0x7000C104);   // control B — clear pending IRQ
```

After reading a packet in the ISR, ack the interrupt:

```c
outl(inl(0x7000C104) | 0x0C000000, 0x7000C104);
outl(0x400A1F00, 0x7000C100);    // re-arm IRQ (note 0x4 vs 0xC top nibble)
```

## IRQ wiring

`I2C_IRQ` (#40, in the high-priority bank — bit 40 in `CPU_HI_INT_EN`).
Latency from press to ISR entry: ~10 ms (the wheel chip's internal
sampling rate).

## Notes for re-implementation

- The 50 µs delay between IRQ entry and `CLICKWHEEL_DATA` read is
  empirical — `ipodlinux` originally had 250 µs; Rockbox dropped it
  to 50 with no observed loss of reliability.
- If you queue events unconditionally you'll flood the queue during
  fast scrolling. Rockbox's mitigation: only post if the queue is
  currently empty.
- The wheel's resolution is fixed at 96 steps; you cannot increase it
  by reading more often. To make scrolling feel finer, pre-divide
  delta in software (Rockbox doesn't, since `WHEEL_SENSITIVITY = 4`
  already gates).

## Source citations

| Topic                  | File                                    |
|------------------------|-----------------------------------------|
| Driver                 | `firmware/target/arm/ipod/button-clickwheel.c` |
| Target button mapping  | `firmware/target/arm/ipod/button-target.h` |
| Generic queue          | `firmware/drivers/button.c`             |
| GPIO definitions       | `firmware/export/pp5020.h`              |
