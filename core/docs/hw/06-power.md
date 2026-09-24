# 06 — Power: PCF50605 PMIC, battery, charging, sleep

The iPod Video's power management is mostly **NXP PCF50605** —
an integrated PMIC (PMU + RTC + ADC + charge controller interface) —
plus a separate **LTC4066** charger IC that the firmware controls
through GPIO current-limit signals.

The PCF50605 sits on I²C at bus address `0x08` (the *device* address;
unrelated to its internal register `OOCC1` further down which also
happens to be at offset `0x08` — easy to confuse). It owns:

- DCDC converters (1.2 V core, 3.0 V I/O).
- ADC (battery voltage, charge state, temperature on a channel we don't use).
- Standby / wake-up state machine.
- RTC.

This doc covers what the firmware needs to do to: read battery
voltage, decide on % SoC, detect charger presence, drive charge
current, enter sleep / wake.

## Battery voltage

### ADC

| Item                      | Value |
|---------------------------|-------|
| ADC channel               | `ADC_BATTERY` / `ADC_UNREG_POWER` (Rockbox channel 0) |
| PCF50605 register (sel)   | `0x2F` (ADCC1) |
| PCF50605 register (data)  | `0x30` / `0x31` (ADCS1, ADCS2 — 10-bit result) |
| Resolution                | 10 bits, 0..1023 |
| Full-scale voltage        | 6000 mV |
| Sample rate               | 5 reads per 2 s ≈ every 400 ms |

### Conversion formula

```c
voltage_mV = (raw_10bit * 6000) >> 10;
```

(`firmware/target/arm/ipod/powermgmt-ipod-pcf.c` line 110.)

The voltage divider on the input is built into the PCF50605
(channel `ADCVIN1` = 0x02), so the formula is direct.

## Battery percentage curve

Rockbox uses an 11-point piecewise-linear lookup. For the iPod
Video 5G/5.5G specifically, the discharge and charge curves are
identical:

| % SoC | mV    | Notes                              |
|-------|-------|------------------------------------|
| 0     | 3600  | Cutoff                             |
| 10    | 3720  | Steep discharge region             |
| 20    | 3750  |                                    |
| 30    | 3780  |                                    |
| 40    | 3810  |                                    |
| 50    | 3840  | Curve knee                         |
| 60    | 3880  |                                    |
| 70    | 3950  |                                    |
| 80    | 4020  |                                    |
| 90    | 4100  |                                    |
| 100   | 4180  | Full charge (Li-Ion plateau)       |

```c
unsigned short percent_to_volt_discharge[11] = {
    3600, 3720, 3750, 3780, 3810, 3840, 3880, 3950, 4020, 4100, 4180
};
```

(`firmware/target/arm/ipod/powermgmt-ipod-pcf.c` lines 59–101.)

Given a measured `mV`, find the bracket and lerp:

```c
int pct_from_mv(int mv) {
    for (int i = 1; i < 11; i++) {
        if (mv < curve[i]) {
            int span = curve[i] - curve[i-1];
            return (i-1)*10 + ((mv - curve[i-1]) * 10 + span/2) / span;
        }
    }
    return 100;
}
```

> Calibration drift: the curve is tuned for the original 2005 Apple
> cell. Replacement cells from 2024+ tend to read 5–10% high in the
> 0–10% range — the cutoff feels premature. Build in a per-cell
> calibration option.

## Charge state detection

The firmware doesn't command the charger IC directly. Instead it
reads three GPIO inputs:

| GPIO                | Bit  | Purpose                          | Polarity |
|---------------------|------|----------------------------------|----------|
| `GPIOL_INPUT_VAL`   | 0x08 | Main charger present (FireWire / USB power) | active-low |
| `GPIOL_INPUT_VAL`   | 0x10 | USB enumeration complete         | active-high |
| `GPIOB_INPUT_VAL`   | 0x01 | Currently charging               | active-low |

```c
unsigned int power_input_status(void) {
    unsigned int s = 0;
    if ((GPIOL_INPUT_VAL & 0x08) == 0)  s  = POWER_INPUT_MAIN_CHARGER;
    if ((GPIOL_INPUT_VAL & 0x10) != 0)  s |= POWER_INPUT_USB_CHARGER;
    return s;
}

bool charging_state(void) {
    return (GPIOB_INPUT_VAL & 0x01) ? false : true;   // bit clear = charging
}
```

(`firmware/target/arm/ipod/power-ipod.c` lines 48–93.)

## Charge current control

The LTC4066 is autonomous; the firmware can only **gate** it via two
GPIO outputs:

| GPIO              | Bit  | Function    | Effect when set / cleared |
|-------------------|------|-------------|---------------------------|
| `GPIOL_OUTPUT_VAL`| 0x04 | SUSP        | High = suspend all charging (USB suspend); low = normal |
| `GPIOA_OUTPUT_VAL`| 0x04 | HPWR        | High = 500 mA permitted; low = 100 mA limit |

```c
void usb_charging_maxcurrent_change(int milliamps) {
    if (milliamps >= 500) {
        GPIO_CLEAR_BITWISE(GPIOL_OUTPUT_VAL, 0x04);   // SUSP off
        GPIO_SET_BITWISE  (GPIOA_OUTPUT_VAL, 0x04);   // HPWR on
    } else if (milliamps >= 100) {
        GPIO_CLEAR_BITWISE(GPIOL_OUTPUT_VAL, 0x04);   // SUSP off
        GPIO_CLEAR_BITWISE(GPIOA_OUTPUT_VAL, 0x04);   // HPWR off
    } else {
        GPIO_SET_BITWISE  (GPIOL_OUTPUT_VAL, 0x04);   // SUSP on (zero current)
    }
}
```

The charger handles CV/CC topology, fast/trickle transitions, and
end-of-charge internally — we just say "0 / 100 / 500 mA cap."

### The pins have to be driven, and the budget is shared with the device

Two things the table above does not say, both learned on this device.

**Direction.** `charger_set_max_current()` (`hal/hw/battery.c`) writes the
level AND takes the pin over — `GPIOA_ENABLE` (`0x6000D000`) /
`GPIOA_OUTPUT_EN` (`0x6000D010`) for HPWR, `GPIOL_ENABLE` (`0x6000D10C`) /
`GPIOL_OUTPUT_EN` (`0x6000D11C`) for SUSP — every write through the masked
`+0x800` alias. The first version wrote `OUTPUT_VAL` only, on the assumption
that the boot ROM had configured the pins, and nothing ever checked. The
level is read back from the pad (`INPUT_VAL`) by `charger_pin_state()` and
shown on Settings → Battery as `HPWR n` / `SUSP n`, with `en=0` / `oe=0`
tails when the pin is not a driven GPIO. Bench: with the cable in, the
footer must read `HPWR 1 · SUSP 0` and no tails; then the LINE on that page
is the measurement (below).

**Budget.** The LTC4066 is a linear charger with the system load in front
of the cell, so the input limit is shared: charge current = limit − system
draw. At the 100 mA cap the budget IS the device. The event log pulled
2026-09-19 (CORELOG.BIN, ~15 000 lines across six days, `core: batt` every
5 s) says so directly, all with `ext 1 chg 1` — cable in, CHRG asserted:

| State (log-derived)              | Duration | Filtered mV        | Net    |
|----------------------------------|----------|--------------------|--------|
| paused, drive parked, awake      | 71 min   | 3861 → 3867        | +6 mV  |
| playing (drive up ~40 %)         | 45 min   | 4037 → 4048        | +11 mV |
| playing                          | 71 min   | 4048 → 4048        | 0      |
| mostly idle, drive parked        | 54 min   | 4048 → 4160        | +112   |
| asleep (suspend), then 45 min later | —     | 3832 → 4224 (raw)  | +390   |

The gauge ALSO drops on plug-in (96 → 92 in one session) because the
charge-lift correction and a spin-up sag land together, which is what made
"it isn't charging on the PC" the natural reading. The CHRG pin is not the
answer either: it stays asserted while the charger is starved — it means
"trying", not "getting anywhere". The trend is the only honest signal this
hardware has, hence `kernel/chargestat.c` and the Battery page.

So: **500 mA is the default** (Settings → Battery → Charge Rate, persisted;
`CHARGE_RATE_QUIET` = 100 mA is the way back). Out of spec on an
un-enumerated PC port, in practice what every root port supplies; a port
that folds back does so until replug and the setting covers it. The
2026-09-13 finding stands: on USB the headphone jack carries the drive, the
piezo and a whine through the cable's ground loop, louder at 500 mA — that
is what the quiet setting is for.

**Still unmeasured:** the actual current (needs an inline USB meter), and
whether the cell climbs while PLAYING at 500 mA. The page's line answers
the second in an hour of use.

## Battery capacity

| Variant             | Default mAh | Notes |
|---------------------|-------------|-------|
| 30 GB (5G)          | 400         | Thin model |
| 60 / 80 GB (5.5G)   | 600         | Thick model |
| User-selectable range | 300..3000 | for replacement / 3rd-party batteries |
| Adjustment increment| 50          |       |

(`firmware/export/config/ipodvideo.h` lines 141–149.)

The firmware multiplies estimated current (`CURRENT_NORMAL = 24 mA`,
`CURRENT_BACKLIGHT = 20 mA` extra, `CURRENT_RECORD = 35 mA` extra) by
remaining capacity to estimate runtime.

## Standby / sleep

The firmware enters standby by writing a single PCF50605 register:

| Register | Address | Purpose |
|----------|---------|---------|
| `OOCC1`  | `0x08`  | On/off control & configuration 1 |

```c
void pcf50605_standby_mode(void) {
    pcf50605_write(PCF5060X_OOCC1,
                   GOSTDBY | CHGWAK | EXTONWAK | wakeup_flags);
}
```

| Bit          | Value | Effect |
|--------------|-------|--------|
| `GOSTDBY`    | `0x01`| Trigger standby (latching) |
| `CHGWAK`     | `0x20`| Wake on charger insertion |
| `EXTONWAK`   | `0x40`| Wake on external (button / dock) |
| `RTCWAK`     | `0x80`| Wake on RTC alarm (set via `wakeup_flags`) |

> **Critical** (cited verbatim in `pcf50605.c` lines 70–73):
> "The following command puts the iPod into a deep sleep. Warning
> from the good people of ipodlinux — never issue this command
> without setting CHGWAK or EXTONWAK if you ever want to be able to
> power on your iPod again."

### Pre-sleep housekeeping (`power_off()`, `power-ipod.c` 161–191)

1. Clear the LCD (avoid ghosting on the panel).
2. Clear IRAM upper region (`0x4000C000` for PP5022, `0x14000` bytes;
   PP5020 is `0xC000` bytes). Apple's OF reads this to detect the
   "boot from sleep" flag — clearing it forces a clean boot path.
3. Call `pcf50605_standby_mode()`.

### Wake sources (after standby)
- Charger insertion (`CHGWAK`).
- External button / dock (`EXTONWAK`).
- RTC alarm (if `RTCWAK` was set in `wakeup_flags`).

### State across sleep
- RTC and PMIC config: preserved (always-on domain) — **and the RTC keeps
  counting**, which is why the firmware reads it at every boot and after every
  wake rather than carrying a time across the gap (see "Real-time clock").
- IRAM: zeroed by us before sleep.
- CPU registers, GPIO, SoC peripherals: lost — full re-init on wake.

## Real-time clock

The PCF50605 keeps a BCD calendar in the same always-on domain the standby
state machine lives in, so it counts through a suspend-to-RAM and through a PMU
standby, and it is the only clock on this hardware that survives a power-off.
It is the device's **only** time-of-day source: there is no network, and on the
cable it is Apple's ROM disk-mode stack answering, not ours, so the host cannot
tell the running firmware the time (it leaves a stamp in `CORECFG.DAT`
instead).

Driver: `hal/hw/rtc.c`, over the register-pointer read path in
[09-i2c.md](09-i2c.md). Calendar maths: `kernel/datetime.c` (2000..2099,
integer only).

### Register map — DERIVED, NOT CONFIRMED

**No RTC register appears anywhere else in `docs/hw/`.** The addresses below
are taken from the public NXP **PCF50606** datasheet's register map (the 50605
is the same family) and cross-checked against the six PCF50605 registers this
doc already lists — `OOCC1 0x08`, `DCDC1 0x1B`, `IOREGC 0x23`, `MBCS1 0x2C`,
`ADCC1 0x2F`, `ADCS1/2 0x30/0x31` — every one of which is a register the
datasheet map has at that address. That is evidence the RTC block is where the
datasheet puts it too; it is not proof. Nothing here was taken from GPL source.

**The cross-check is by ADDRESS AND FUNCTION, not by name.** One row is named
differently in the datasheet: there `0x2E` is `ADCC1` and `0x2F` is `ADCC2`,
the mux-and-start register — which is the register the firmware actually pokes
(`hal/hw/battery.c` writes channel|start to `0x2F` and the conversion happens),
under this doc's older name. So the agreement to read off that row is "the
register at `0x2F` does what we use it for", not "the name matches".

| Addr | Name | Meaning | Encoding | Confidence |
|------|------|---------|----------|------------|
| `0x0A` | `RTCSC` | seconds 00..59 | BCD: bits 6:4 tens, 3:0 units | high (map cross-check) |
| `0x0B` | `RTCMN` | minutes 00..59 | BCD | high |
| `0x0C` | `RTCHR` | hours 00..23 | BCD, 24-hour | high for the address; **medium for 24-hour** — confirm there is no 12-hour/AM-PM select bit |
| `0x0D` | `RTCWD` | weekday 0..6 | plain binary | medium — the base day is unknown. We WRITE Sunday=0 and IGNORE it on read |
| `0x0E` | `RTCDT` | day 01..31 | BCD | high |
| `0x0F` | `RTCMT` | month 01..12 | BCD | high |
| `0x10` | `RTCYR` | year 00..99 = 2000..2099 | BCD | high for the address; **reset value 00 → "unset": to confirm** |
| `0x11`..`0x17` | `RTCSCA`..`RTCYRA` | alarm, same order and encoding | same | medium (alarm only; not used) |
| `0x02` / `0x05` | `INT1` / `INT1M` | the alarm interrupt and its mask | the BIT is **unsettled**: the datasheet's INT1 map (ONKEYR 0x01, ONKEYF 0x02, ONKEY1S 0x04, EXTONR 0x08, EXTONF 0x10, SECOND 0x20, ALARM 0x40, bit 7 unused) puts it at `0x40`; an earlier reading of the same map put it at `0x80` | **to confirm** — no constant is defined for it (`hal/hw/rtc.h`) |
| `0x08` | `OOCC1` bit `RTCWAK` | wake from standby on the alarm | this doc's standby table says `0x80`; the datasheet map puts RTCWAK at bit 4 (`0x10`) and reads `0x40`/`0x80` as EXTONWAK's two bits — so the doc's value would set EXTONWAK-low, not RTCWAK | **CONFLICT — settle on the bench before any alarm work.** Nothing writes it today |

Not assumed anywhere: auto-increment across multi-byte *writes* (every write is
its own two-byte transaction), a stop-the-clock bit (the datasheet has none;
handled by write order below), or a backup cell — the 5G has none we know of, so
a fully drained main cell resets the register file, which is exactly the "unset"
case.

**The year 2000 is not representable.** `RTCYR` 00 is the reset value, so a
clock that has lost its cell reads as the year 2000 and cannot be told apart
from one that was deliberately set there. The firmware's range is therefore
**2001..2099**, and year 00 means "no time known".

### Read: two chunks, and a tear check

The controller carries at most four payload bytes per transaction (09-i2c.md),
so the calendar takes two reads — and a second boundary landing between them
would pair a time from before the carry with a date from after it (at 23:59:59,
a whole day wrong). Hence three transactions:

1. `i2c_read(0x08, 0x0A, buf, 4)` → SC MN HR WD
2. `i2c_read(0x08, 0x0E, buf, 3)` → DT MT YR
3. `i2c_read(0x08, 0x0A, buf, 1)` → SC again

If the second seconds reading is **lower** than the first, a carry happened in
the middle: re-read once and use the second pass.

Every byte is then checked as strict BCD (any nibble > 9 invalidates the whole
reading), and the assembled date is range-checked including the month's length.
This gate is what makes a wrong register map safe: it reads as "unset", never as
a plausible wrong date. It also catches the **NACK shape** — `i2c_read` cannot
see a missing ack (09-i2c.md), so an absent PMU hands back the DATA registers'
last written bytes, i.e. the register pointer `0x0A` in DATA0, which is not BCD.
The weekday register is never trusted; the weekday is computed from the date.

### Write: seconds first

Seven single-register writes, in the order **SC, MN, HR, WD, DT, MT, YR**.
Seconds first restarts the second counter, so the next minute carry is a whole
second away while the remaining six registers are written (microseconds) — which
is what makes the date safe to write without a stop-the-clock bit. Then a
one-byte read of `RTCYR` flushes the lazily-completed write path, and the caller
re-reads the whole calendar and compares (±1 s is agreement — the seconds may
carry while we look).

### Alarm

Tabled above, deliberately not implemented: two functions would do it, but
`RTCWAK` is in dispute and writing the wrong bit into `OOCC1` — the register
that triggers standby — is the one mistake on this chip that can leave an iPod
that will not wake.

### Bench procedure (DEVICE — none of the above is confirmed)

In order, on the first flash that carries the driver:

a. Read the raw bytes. The boot prints `core: rtc raw SC MN HR WD DT MT YR
   valid N epoch XXXXXXXX` (seven bytes exactly as read). Every nibble ≤ 9
   confirms BCD and the block placement; whatever Apple's firmware left behind
   also says whether the OF used the same registers.
b. Set a known time from Settings > Date & Time, read it back (the `rtc set`
   line), then power off with PLAY (PMU standby), wait an hour, boot: the clock
   must have advanced by the wall-clock hour. That is the always-on-domain
   claim.
c. Same across a suspend-to-RAM (PLL parked) — that proves the wake re-anchor.
d. Leave it a week and compare against a phone: drift.
e. Disconnect or fully drain the cell once and read the raw bytes back: that is
   the reset value, and it settles the "year 00 = unset" row.

## Brown-out / low-battery shutdown

| Threshold            | Voltage | Action |
|----------------------|---------|--------|
| `battery_level_disksafe` | 3500 mV | Spin down HDD (avoid corruption during low-V writes) |
| `battery_level_shutoff`  | 3300 mV | Force `power_off()` |

```c
unsigned short battery_level_shutoff  = 3300;
unsigned short battery_level_disksafe = 3500;
```

(`powermgmt-ipod-pcf.c` lines 30–56.)

The power thread polls voltage every ~400 ms; on threshold cross it
calls the standard shutdown path. There's no graceful save below
3300 mV — the system is unstable enough that "hit standby and
hope" is the strategy.

## Other PCF50605 registers we touch

| Register | Addr | Purpose |
|----------|------|---------|
| `OOCC1`  | `0x08`| Standby trigger |
| `ADCC1`  | `0x2F`| ADC channel select & start |
| `ADCS1`/`ADCS2` | `0x30`/`0x31` | 10-bit ADC result |
| `MBCS1`  | `0x2C`| Charge status (read-only) |
| `DCDC1`  | `0x1B`| Core voltage 1.2 V (`0xEC`); always on during standby |
| `IOREGC` | `0x23`| I/O voltage 3.0 V (`0xF5`) — GPIO + GPO supply |
| `RTCSC`..`RTCYR` | `0x0A`..`0x10` | Real-time clock — see "Real-time clock" above (datasheet-derived, unconfirmed) |

Source: `firmware/export/pcf5060x.h`; the RTC rows are from the public NXP
PCF50606 datasheet.

## Temperature monitoring

**Not implemented.** The PCF50605 has a `BATTEMP` ADC channel
(`0x04`), but Rockbox never reads it. There's no thermal cutoff or
charge-rate limiting based on cell temperature. We rely on the
LTC4066's internal thermal protection.

For us: this is worth implementing, especially given that 2024+
replacement cells may have different thermal behavior than 2005-era
cells. Adding it is a few lines: read channel `0x04`, compare to
threshold, gate `HPWR` if hot.

## Source citations

| Topic                | File |
|----------------------|------|
| ADC + curve          | `firmware/target/arm/ipod/powermgmt-ipod-pcf.c` |
| Charge GPIO control  | `firmware/target/arm/ipod/power-ipod.c` |
| Standby driver       | `firmware/drivers/pcf50605.c` |
| PMIC register defs   | `firmware/export/pcf5060x.h` |
| Target battery defaults | `firmware/export/config/ipodvideo.h` |
