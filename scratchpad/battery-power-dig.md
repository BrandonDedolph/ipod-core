# Battery + Power dig — PCF50605 on iPod 5G/5.5G (PP5022)

Cleanroom facts scout for a UI battery indicator + charging screen. FACTS
(addresses, ADC channel, scaling, sequences) extracted from Rockbox source and
cross-checked against the repo's existing `core/docs/hw/06-power.md`. No code
copied. **This report confirms and extends the existing power doc** — its
register facts all verified below — so the new work here is the *driver seam*
(the I2C read path the codec path never needed), not re-documenting the numbers.

## TL;DR / recommended bring-up path

Battery voltage on the 5G/5.5G is **not** a PP502x ADC — it comes from the
**PCF50605 PMU over the same on-SoC I2C bus the WM8758 codec already uses**
(7-bit device address `0x08`). Read it by writing the ADC-select register then
reading two result bytes; scale `mV = (raw10 * 6000) >> 10`. Charger-present and
"currently charging" are **plain GPIO bits** (no I2C), so power-state detection
is trivially cheap and can land first. **Safest order:** (1) add a register-read
path to `i2c.c` (today it is write-only), (2) wire the two charger GPIOs and show
a live "external / charging" flag — zero calibration risk, (3) read + display
**raw millivolts** on-screen, (4) only then apply the %-curve and calibrate the
low end against the real cell. Everything past step 2 is device-gated: the
scaling and curve cannot be validated without the hardware.

---

## 1. Battery voltage read

**Source of the reading: PCF50605 PMU over I2C, channel `ADCVIN1` (0x2).**
It is a PMU-internal 10-bit ADC with a built-in resistive divider — *not* a
PP502x SoC ADC. Confirmed: Rockbox reads it via `pcf50605_write/_read`, never a
SoC ADC block.

### Bus / device
| Item | Value | Source |
|------|-------|--------|
| I2C controller | on-SoC PP502x master @ `0x7000C000` (already driven by `core/hal/hw/i2c.c`) | `09-i2c.md`, repo |
| PMU device addr | **`0x08`** (7-bit; same for read + write) | `firmware/drivers/pcf50605.c` — `pcf50605_read`→`i2c_readbyte(0x8,…)`, `pcf50605_write`→`pp_i2c_send(0x8,…)` |
| Register model | register-pointer: write `[reg]`, then read `[data…]` (or write `[reg,val]`) | same |

### ADC read sequence (channel-select → convert → read result)
From `firmware/target/arm/ipod/adc-ipod-pcf.c`:

1. **Select + start**: `pcf50605_write(0x2F, (channelnum << 1) | 0x01)`
   - `0x2F` = **ADCC1** (ADC control / channel select). Bit0 = start.
   - Battery channel `channelnum = 0x02` (**ADCVIN1**, "resistive divider").
   - So the byte written is `(0x02 << 1) | 0x01 = 0x05`.
2. **Read result**: `pcf50605_read_multiple(0x30, data, 2)`
   - `0x30` = **ADCS1** (high byte), `0x31` = **ADCS2** (low bits).
3. **Assemble 10-bit**: `value = (data[0] << 2) | (data[1] & 0x03)` → `0..1023`.

> Settling: Rockbox's read is straight-line (select then read) with the I2C
> transaction latency itself as the conversion delay; there is no explicit
> ready-bit poll in the iPod path. On bring-up, if the first reads look wrong,
> add a short delay or a re-read after channel select — **device-gated**, cannot
> confirm without hardware.

### Raw → millivolts
From `firmware/target/arm/ipod/powermgmt-ipod-pcf.c`:
```
mV = (raw10 * 6000) >> 10;     // BATTERY_SCALE_FACTOR = 6000 (full-scale mV)
```
Full-scale = 6000 mV over 10 bits; the divider is inside the PCF50605 (ADCVIN1),
so this is direct — no external divider math. (Matches `06-power.md` exactly.)

### How this reuses the existing I2C driver
- **A PMU register *write* already works today**: `i2c_send(0x08, (u8[]){reg, val}, 2)`
  is exactly the `pcf50605_write(reg, val)` shape and needs *no* new code — the
  Phase-1 write path in `core/hal/hw/i2c.c` handles ≤4-byte writes to any device.
- **The read is the gap.** `i2c.c` deliberately built **no read path** (codec is
  write-only; see its header comment and `09-i2c.md` "Read transaction … not
  needed for Phase 1"). Battery needs a register-pointer read:
  write the 1-byte register address, then a read transaction of N bytes.
  Rockbox's reference read path (`i2c-pp.c:70-115`) sets `I2C_ADDR |= 0x01`
  (R/W bit), sets `I2C_CTRL |= I2C_READ` (0x20), strobes, waits BUSY clear, then
  reads back `I2C_DATA(0..len-1)`. All the register symbols/bits already exist in
  `core/hal/hw/pp5022.h` (`I2C_READ`, `I2C_ADDR_RW`, `I2C_DATA_ADDR(n)`,
  `I2C_BUSY`). So it's ~30 lines against constants already present.

---

## 2. Voltage → percent curve (iPod Video / 5.5G, approximate)

11-point piecewise-linear, discharge == charge for this target. From
`powermgmt-ipod-pcf.c` (`percent_to_volt_discharge`, iPod Video):

| % | 0 | 10 | 20 | 30 | 40 | 50 | 60 | 70 | 80 | 90 | 100 |
|---|---|----|----|----|----|----|----|----|----|----|-----|
| mV | 3600 | 3720 | 3750 | 3780 | 3810 | 3840 | 3880 | 3950 | 4020 | 4100 | 4180 |

```
static const uint16_t v_curve[11] =
    {3600,3720,3750,3780,3810,3840,3880,3950,4020,4100,4180};

int pct_from_mv(int mv){
    if (mv <= v_curve[0])  return 0;
    for (int i = 1; i < 11; i++)
        if (mv < v_curve[i]) {
            int span = v_curve[i] - v_curve[i-1];
            return (i-1)*10 + ((mv - v_curve[i-1])*10 + span/2)/span;
        }
    return 100;
}
```
**Approximate.** Curve is tuned to the 2005 Apple cell; the low end (0–10%,
3600–3720 mV) is the steep/noisy region and modern replacement cells often read
high there. Treat % as cosmetic; drive real shutdown off raw mV thresholds
(below), not off %.

Brown-out thresholds (raw mV, from same file, iPod Video):
`battery_level_disksafe = 3500` (spin down HDD), `battery_level_shutoff = 3300`
(force power-off).

---

## 3. Charging / external-power detect (GPIO, no I2C)

From `firmware/target/arm/ipod/power-ipod.c`, iPod Nano/Video branch:

| Signal | Register | Bit | Polarity | Meaning |
|--------|----------|-----|----------|---------|
| Main charger present (FireWire/USB power on the barrel/dock) | `GPIOL_INPUT_VAL` | `0x08` | **active-low** (bit clear = present) | `POWER_INPUT_MAIN_CHARGER` |
| USB charger / enumeration | `GPIOL_INPUT_VAL` | `0x10` | **active-high** (bit set = present) | `POWER_INPUT_USB_CHARGER` |
| Currently charging | `GPIOB_INPUT_VAL` | `0x01` | **active-low** (bit clear = charging) | `charging_state()` |

```
external = ((GPIOL_INPUT_VAL & 0x08) == 0) || ((GPIOL_INPUT_VAL & 0x10) != 0);
charging = ((GPIOB_INPUT_VAL & 0x01) == 0);   // bit clear = charging
// "full while plugged" ≈ external && !charging
```
Notes:
- **No PMU/I2C read is used for power-state detection** — verified in
  `power-ipod.c`; only GPIO. So the charging screen's presence/charging flags are
  purely GPIO polls, independent of the (harder) I2C battery read.
- `GPIOB`/`GPIOL` port-input registers are **not yet defined** in
  `core/hal/hw/pp5022.h` (only `GPIOA_INPUT_VAL @ 0x6000D030` and the GPIOC pair
  exist). GPIO A–D input slots follow the base `0x6000D000` + per-port `+0x30`
  pattern already used for GPIOA; **the L-group (`GPIOL`) lives in the higher
  GPIO bank and its base must be pulled from Rockbox `pp5020.h` before use** —
  flag as a small address-lookup TODO, do not guess the GPIOL base.
- Charge-current gating (not needed to *display* status, but adjacent): `SUSP` =
  `GPIOL_OUTPUT_VAL` bit `0x04` (high = suspend charging), `HPWR` =
  `GPIOA_OUTPUT_VAL` bit `0x04` (high = allow 500 mA, low = 100 mA). See
  `06-power.md`. Leave charger control to a later phase.

---

## 4. Proposed driver API + effort / risk

```c
/* core/hal/hw/power.h (proposed) */
int  battery_millivolts(void);   /* PCF50605 ADC read → mV; <0 on I2C error   */
int  battery_percent(void);      /* pct_from_mv(battery_millivolts()); approx */
int  power_is_external(void);    /* GPIOL 0x08 low || 0x10 high               */
int  power_is_charging(void);    /* GPIOB 0x01 low                            */
```

### Effort split
| Piece | Where | Cost | Risk |
|-------|-------|------|------|
| `power_is_external` / `power_is_charging` | GPIO reads in a new `power.c` | tiny | none (needs GPIOL/GPIOB base lookup) |
| PMU register **write** | already works via `i2c_send(0x08,{reg,val},2)` | none | none |
| PMU register **read** (register-pointer) | **new path in `i2c.c`** using existing `I2C_READ`/`I2C_ADDR_RW`/`I2C_DATA_ADDR`/`I2C_BUSY` consts | ~30 LOC | low (mirror Rockbox read sequence) |
| `battery_millivolts` (select 0x2F=0x05, read 0x30/0x31, assemble, ×6000>>10) | `power.c` | small | **device-gated** — scaling unverifiable off-hardware |
| `battery_percent` curve | pure C table + lerp | small | approximate by nature |

### Safest first step
1. Land `power_is_external` / `power_is_charging` (GPIO only) → the charging
   screen can react to plug/unplug with zero calibration risk.
2. Add the I2C read path; unit-test it against the mock-bus/golden-trace harness
   the codec path uses (write `0x2F=0x05`, read 2 bytes) — no hardware needed to
   prove the *transaction shape*.
3. On device: display **raw mV** first, sanity-check ~3300–4200 range, then turn
   on the %-curve. Calibrate the 0–20% end against the actual cell before trusting
   the shutdown thresholds.

---

## Source citations
| Fact | File |
|------|------|
| PMU I2C addr `0x08`, read/write reg model | `firmware/drivers/pcf50605.c` |
| ADC select `0x2F`, channel `0x02` (ADCVIN1), result `0x30`/`0x31`, 10-bit assemble | `firmware/target/arm/ipod/adc-ipod-pcf.c` |
| `×6000 >> 10` scaling, curve, shutoff/disksafe mV | `firmware/target/arm/ipod/powermgmt-ipod-pcf.c` |
| Charger/charging GPIO bits (GPIOL 0x08/0x10, GPIOB 0x01) | `firmware/target/arm/ipod/power-ipod.c` |
| On-SoC I2C read sequence reference | `firmware/target/arm/pp/i2c-pp.c:70-115` |
| Repo: existing write-only I2C driver + register consts | `core/hal/hw/i2c.c`, `core/hal/hw/pp5022.h`, `core/docs/hw/09-i2c.md` |
| Repo: existing (verified-matching) power doc | `core/docs/hw/06-power.md` |
