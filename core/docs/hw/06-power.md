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
- RTC and PMIC config: preserved (always-on domain).
- IRAM: zeroed by us before sleep.
- CPU registers, GPIO, SoC peripherals: lost — full re-init on wake.

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

Source: `firmware/export/pcf5060x.h`.

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
