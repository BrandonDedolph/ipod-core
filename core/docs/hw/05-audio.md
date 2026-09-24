# 05 — Audio path: I²S + Wolfson WM8758 DAC

The audio chain is:

```
[PCM in DRAM] → DMA0 → IISFIFO_WR → I²S serializer → WM8758 → headphone / line out
```

Decoder produces PCM in a ring buffer, the DMA engine continuously
feeds the I²S TX FIFO, the I²S block clocks the data out to the DAC,
and the DAC drives the analog output stage. The CPU only refills the
ring; the actual sample-by-sample transport is hardware.

The 5G and 5.5G both use the **WM8758** Wolfson DAC. (Some early
Rockbox docs claim 5.5G uses WM8975 but the iPod Video target config
defines `HAVE_WM8758` for both — `firmware/export/config/ipodvideo.h`
line 98. Audiophile listeners report subtle audible differences; the
chip itself is the same.)

## DAC (WM8758): I²C interface

The control port rides the SoC's on-chip I²C controller — the register
block, transaction sequence, and init are documented separately in
[09-i2c.md](09-i2c.md). This section covers only the codec-specific
framing layered on top of a raw I²C write.

Device address `0x1A`. Protocol is the classic Wolfson "9-bit control
word": a 7-bit register address plus 9 bits of data, packed into the two
payload bytes of a single I²C write. The register goes in bits 15:9 and
the data in bits 8:0, so:

- **byte 0** = `(reg << 1) | (data_bit8)` — the 7-bit register shifted up
  one, with data bit 8 riding in the LSB.
- **byte 1** = `data & 0xFF` — the low 8 data bits.

Our driver's `wm8758_write` (`core/hal/hw/wm8758.c`) performs this packing
and hands the two bytes to `i2c_send`.

Source: `firmware/target/arm/pp/wmcodec-pp.c:41-42,111`.

## DAC register map (subset Rockbox uses)

| Register           | Addr  | Purpose |
|--------------------|-------|---------|
| `RESET`            | `0x00`| Soft reset |
| `PWRMGMT1`         | `0x01`| VMID, BIASEN, BUFIOEN, PLLEN |
| `PWRMGMT2`         | `0x02`| Output enables (LOUT1, ROUT1) |
| `PWRMGMT3`         | `0x03`| DAC enables, mixer enables, OUT2 enables |
| `AINTFCE`          | `0x04`| Audio interface (I²S format, word length) |
| `CLKCTRL`          | `0x06`| Clock select, master/slave, BCLK/MCLK divs |
| `ADDCTRL`          | `0x07`| Sample rate filter config, slow clock |
| `DACCTRL`          | `0x0A`| Soft mute, oversampling rate |
| `LDACVOL` / `RDACVOL` | `0x0B` / `0x0C` | DAC volume (0..0xFF, full scale at 0xFF) |
| `LOUT1VOL` / `ROUT1VOL` | `0x34` / `0x35` | Headphone amp gain |
| `LOUT2VOL` / `ROUT2VOL` | `0x36` / `0x37` | Line-out amp gain |
| `LOUTMIX`          | `0x32`| Output mixer routing (DAC → L out) |
| `ROUTMIX`          | `0x33`| (R) |
| `PLLN` / `PLLK1` / `PLLK2` / `PLLK3` | `0x24`–`0x27` | PLL coefficients |

Source: `firmware/export/wm8758.h`.

## DAC init sequence

The bring-up is the WM8758B datasheet's **Recommended Power Up Sequence**,
and it runs only on a **cold** codec: at boot, on the Play after a
persistent pause has powered the codec down, and on the first play after a
close. **It is not per track.** A track change on a warm codec (Next/Prev,
a row played over a playing track, a rate change at a gapless boundary) is
`wm8758_retune` — see "Track change" below. Until 2026-09-22 the whole
reset + VMID cycle ran on every track, and every cycle was a thump on the
jack.

On a chainloaded device where Apple's firmware may have left codec state
behind, the sequence starts with a soft reset (write register `RESET`) so
it begins from datasheet defaults. Our driver (`core/hal/hw/wm8758.c`,
`wm8758_init`) implements it as register-write tables; the concrete bit
values are in the appendix.

1. **Soft reset** (`RESET`) — return to power-on defaults.
2. **Low bias mode** (`BIASCTRL` = BIASCUT).
3. **Output buffers + protection** (`OUTCTRL` = HP/LINE common | TSOPCTRL
   | TSDEN | VROI).
4. **Mute everything** before the rails come up: `L/ROUT1VOL`,
   `L/ROUT2VOL` = `0x140` (VU + mute), `OUT3MIX`/`OUT4MIX` = `0x40`.
5. **Enable headphone outputs** (`PWRMGMT2` = LOUT1EN | ROUT1EN) — while
   VMID is still at zero, so their outputs follow it up rather than
   snapping to it later.
6. **VMID-independent bias on** (`OUT4TOADC` = POBCTRL) — the amps are
   biased from AVDD until VMID exists.
7. **Enable DACs + mixers** (`PWRMGMT3` = DACENL/R | LMIXEN/RMIXEN).
8. **Bias rails up**, VMID on its normal-operation 75 kΩ divider
   (`PWRMGMT1` = PLLEN | BIASEN | BUFIOEN | VMIDSEL_75K). The datasheet
   starts the charge on THIS divider (VMIDSEL=01), not the 10 kΩ fast one:
   the coupling caps into the headphones charge at the VMID ramp rate, and
   the fast divider is a 7× bigger thump.
9. **Interface**: I²S, 16-bit (`AINTFCE`).
10. **Become I²S clock master** (`CLKCTRL` = MS). MCLK must already be
    running from the SoC (see below) or the codec masters a clock that
    doesn't exist → silence + a click.
11. **Program the rate** (PLL coefficients + `CLKCTRL` + `ADDCTRL` — see
    the appendix's resolved sequence).
12. **Route the DAC to the mixers** — `LOUTMIX` = DACL2LMIX, `ROUTMIX` =
    DACR2RMIX. **Critical**: without this the DAC is powered but
    unrouted, i.e. silence.
13. **Wait 100 ms** for VMID to rise (the datasheet's figure; a bounded
    `USEC_TIMER` wait in the driver). POBCTRL stays set through it.
14. **Clear low bias** (`BIASCTRL` = 0), DAC volume to full scale, then
    **unmute the headphone outputs and set the volume** (`L/ROUT1VOL`
    with ZC; the user's volume/balance/EQ are restored here through the
    `wm8758_set_restore` hook), `DACCTRL` = DACOSR128 | SOFTMUTE — the
    bring-up **leaves the DAC soft-muted**.
15. **Drop the VMID-independent bias LAST** (`OUT4TOADC` = 0): the amps now
    run from the VMID that rose underneath them. The driver used to clear
    it at step 12, ~3 ms into the charge, which handed the amps to a bias
    that did not exist yet.

No PCM is flowing when this returns (the TX FIFO was just reset and the
DMA has not been kicked), and a DAC unmuted over an idle serializer plays
whatever it emits on underrun. The stream unmutes itself once the DMA is
running (see "Mute").

Note that DAC digital volume is a single global attenuator shared by
every output path — it affects the headphone amp and the line-out
together, so per-output balance has to be trimmed at the mixer/amp
stage, not the DAC.

## Track change (warm codec)

`hal_audio_init` on a warm codec does not reset it. With the DAC already
soft-muted and the DMA cut (every stop leaves the codec that way, and init
stops a running stream itself first), `wm8758_retune` compares the rate the
stream wants with what the PLL and dividers are programmed for:

- **same rate**: no codec write at all;
- **different rate**: `PWRMGMT1` with PLLEN cleared, the six rate registers
  (`PLLN`, `PLLK1..3`, `CLKCTRL`, `ADDCTRL`), `PWRMGMT1` with PLLEN back,
  then a bounded ~5 ms lock wait. The codec masters BCLK/LRCLK from this
  PLL, so the link loses and re-finds its clocks under the mute with
  nothing in flight.

Then the I²S block is reset (FIFO flushed after any clock wobble) and the
DMA channel re-armed, exactly as on a cold init; the stream primes, kicks,
and unmutes. VMID, bias and the headphone amps never move.

## DAC sample-rate setup

Setting the sample rate reprograms the codec's internal PLL and output
divider. The PLL has two preset operating points, both from a 12 MHz
MCLK reference (the SoC feeds 24 MHz, the codec's `PLLPRESCALE` halves
it):

- **22.5792 MHz** output — the 44.1 kHz family (44.1/22.05/11.025 kHz).
- **24.576 MHz** output — the 48 kHz family (48/32/24/16/12/8 kHz).

SYSCLK is then 256 × the sample rate after the MCLKDIV divider. Each
supported rate maps to one preset plus an MCLKDIV setting and an
`ADDCTRL` sample-rate-class hint (the hint does not set the rate — the
PLL does). The concrete register values for all rates, and the fully
resolved 44.1 kHz write sequence, are in the numeric appendix below
("WM8758 sample-rate program: 44.1 kHz resolved"); our driver
implements only the 44.1 kHz case for now.

## SoC: I²S peripheral

I²S registers in MMIO at `0x70002800`:

| Register     | Addr        | Purpose |
|--------------|-------------|---------|
| `IISCONFIG`  | `0x70002800`| Format, enable, IRQ control |
| `IISCLK`     | `0x70002808`| Clock dividers |
| `IISFIFO_CFG`| `0x7000280C`| FIFO thresholds + clear |
| `IISFIFO_WR` | `0x70002840`| 32-bit TX data FIFO |
| `IISFIFO_RD` | `0x70002880`| 32-bit RX data FIFO (recording) |

### `IISCONFIG` bits

| Bit(s) | Name                | Value / meaning |
|--------|---------------------|-----------------|
| 31     | `IIS_RESET`         | Soft reset |
| 29     | `IIS_TXFIFOEN`      | Enable TX FIFO |
| 28     | `IIS_RXFIFOEN`      | Enable RX FIFO |
| 25     | `IIS_MASTER`        | 1 = SoC is I²S master, 0 = slave |
| 11:10  | `IIS_FORMAT`        | 0 = standard I²S (lead-in dummy bit), 2 = left-justified |
| 9:8    | `IIS_SIZE`          | 0 = 16-bit samples |
| 6:4    | `IIS_FIFO_FORMAT`   | 7 = LE16_2 (16-bit LE stereo pairs) |
| 1:0    | IRQ flags           | `IRQRX`, `IRQTX` |

### Initialization (`i2s_reset`)

Pulse `IISCONFIG`'s reset bit and clear it, then set three fields in
`IISCONFIG` — data format = standard I²S, sample size = 16-bit, FIFO
format = LE16_2 — each as a masked read-modify-write. Then set the FIFO
attention levels (`IISFIFO_CFG` RX-full-at-12, TX-empty-at-4) and flush
both FIFOs (`RXCLR | TXCLR`). The exact ordered values are in the
appendix ("`i2s_reset()` config sequence"). When the TX FIFO drops below
the TX-empty level, the DMA request line fires (the polled path instead
watches the live TX-free count — see the appendix).

## MCLK: SoC supplies it to the DAC

The PP5022 provides MCLK to the WM8758 (the DAC's "master clock" input —
separate from the bit clock the DAC then becomes I²S master of). Enable
the external device clocks (`DEV_EN` EXTCLOCKS bit) and select the 24 MHz
reference by clearing the select field at `0x70000018`; the full enable
sequence, including the I²S/CDI pad-mux, is in the appendix ("MCLK /
clock-gating enable path").

The DAC's internal PLL then produces SYSCLK = 256 × sample rate from
this 12 MHz reference (after its own divider).

> Order matters: MCLK must be running before we set `CLKCTRL_MS`
> on the DAC. Otherwise the DAC starts mastering on a clock that
> doesn't exist and the audible result is silence + a click on first
> sample.

## DMA: pumping PCM into the I²S FIFO

For continuous playback (not the polled first-sound path), DMA channel 0
feeds the I²S TX FIFO so the CPU only refills a ring buffer. Bring-up:
enable the DMA master, set channel 0's request line to the I²S source at
FIQ priority, point the channel's peripheral address at the TX FIFO port,
and set its increment mode to "peripheral address fixed, 32-bit
transfers". The per-transfer command word carries the byte count together
with the direction/single/wait-request/interrupt flags — and per the
hardware the count field is written as `bytes − 4`. Concrete addresses
and the command-word bit encoding are in the numeric appendix below
("DMA engine").

PCM frame layout in DMA is the same 32-bit packing the polled path uses:
one word per stereo frame, left in the low 16 bits, right in the high 16.
The I²S serializer splits each word into two 16-bit serial frames on the
wire automatically.

### FIQ playback handler

A DMA-complete interrupt (routed at FIQ priority) advances the ring's
read pointer by the bytes just transferred, then either queues the next
chunk or, at end of chunk, asks the engine for fresh PCM and restarts.
Reading the channel's status register clears the pending IRQ. The audio
thread refills the ring; DMA + the FIQ keep the FIFO from underrunning.
If the thread is starved (e.g. during HDD spin-up) the FIFO drains and
the DAC repeats whatever the serializer last held — usually silence with
a brief click.

## Volume control

Two attenuators in series: DAC volume (digital, applies before the
output amps) and output amp gain (analog).

- **DAC digital volume** — `LDACVOL`/`RDACVOL`, an 8-bit field, 0.5 dB per
  step: `0xFF` = 0 dB (full scale), `0x00` = mute. It is global, so it
  attenuates every output path, which is why it is not the user's volume knob;
  its one use here is the EQ pre-cut (see "WM8758 tone controls" below).
  Writes latch only when the `DACVU` bit is set on the second (right)
  write — the classic Wolfson "write left, then write right with the
  update bit" so both channels change together.
- **Headphone amp gain** — `LOUT1VOL`/`ROUT1VOL`, a 6-bit field
  (0..0x3F, roughly −57..+6 dB). Set the zero-cross (`ZC`) bit so gain
  changes wait for a signal zero-crossing (no zipper noise), and the
  `OUT1VU` update bit on the right write to latch both channels.

Line-out (OUT2) follows the same two-write, `VU`-on-the-second pattern
with `LOUT2VOL`/`ROUT2VOL`. The concrete bit values are in the appendix.

## Lineout

Line-out shares the DAC and mixers with the headphone path; enabling it
is a matter of adding the OUT2 left/right enable bits to `PWRMGMT3` on
top of the DAC and mixer enables (and dropping them to disable). The
dock connector's line-out pins are wired to OUT2.

## Mute

Muting writes `DACCTRL` = DACOSR128 | SOFTMUTE; unmuting writes
`DACCTRL` = DACOSR128. The oversampling bit is held constant in both
directions — flipping it with the mute (as the driver once did) is a DAC
filter reconfiguration at the instant of every pause and play.

SOFTMUTE is a **ramp**, not a cut, and the I2C write returns before the
transaction is even on the wire, so:

- **Pause** (`hal_audio_stop`): write the mute, then wait the ramp out
  (`wm8758_mute` does — 1024 sample periods at the programmed rate, ~23 ms
  at 44.1 kHz) with the DMA still feeding real PCM, and only then stop the
  DMA. The ramp fades music, not the serializer's underrun output.
- **Play** (`hal_audio_start`): kick the DMA first, then unmute. The
  unmute ramp then fades real audio in. Unmuting before the kick (as the
  driver once did) played the idle serializer for the duration of two
  buffer fills, plus the volume/EQ restore on a fresh bring-up.
- **A warm codec is never reset** (a track started from a row, a rate
  change): see "Track change" — a retune under the mute, no rails moved.

## Shutdown

`wm8758_powerdown` — the datasheet's **Recommended Power Down Sequence**,
run on the persistent-pause timer (`hal_audio_suspend`), at close (end of
queue, stop, standby, disk mode) and once at boot (`hal_audio_boot_quiet`,
against whatever the previous image left live):

1. Soft-mute the DAC, mute the headphone outputs (ours: nothing is live
   while VMID falls).
2. Thermal shutdown off (`OUTCTRL` without TSDEN).
3. Begin VMID discharge (`OUT4TOADC` = POBCTRL | VMIDTOG), then `PWRMGMT1`
   = PLLEN | BIASEN — VMID divider and I/O buffer off — so the rail drains.
4. **Wait ~300 ms** for VMID to fully drain (a bounded `USEC_TIMER` wait in
   the driver; the datasheet gives no figure, it depends on the VMID
   capacitor, so this is device-gated).
5. Clear `PWRMGMT1`, `PWRMGMT2`, `PWRMGMT3` (all rails off).

On the I²S side, the DMA is already stopped (the stop preceded this); the
TX FIFO enable and the I²S + MCLK clock gates are dropped after.

The 300 ms VMID drain is non-negotiable — skipping it cuts the headphone
amps with VMID at midrail, a loud DC pop through the coupling caps. The
driver used to skip it (three back-to-back writes, ~150 µs after the
discharge began).

## Source citations

| Topic                | File |
|----------------------|------|
| DAC driver           | `firmware/drivers/audio/wm8758.c` |
| DAC I²C glue         | `firmware/target/arm/pp/wmcodec-pp.c` |
| I²S setup            | `firmware/target/arm/pp/i2s-pp.c` |
| PCM/DMA pump         | `firmware/target/arm/pp/pcm-pp.c` |
| Register map         | `firmware/export/wm8758.h`, `firmware/export/pp5020.h` |

---

# Numeric register reference (resolved values)

The sections above describe the audio path in prose with symbolic
constants. This appendix resolves every constant a driver needs to
concrete numbers, so `core/hal/hw/pp5022.h` (I²S/DMA/clock gating) and
`core/hal/hw/wm8758.h` (codec registers) can transcribe from here. Values
are datasheet/register facts cross-checked against the Rockbox reference;
cites are `file:line`.

## WM8758 codec registers (7-bit register addresses)

| Reg | Addr | Reg | Addr |
|-----|------|-----|------|
| `WM_RESET`    | `0x00` | `WM_PLLN`     | `0x24` |
| `WM_PWRMGMT1` | `0x01` | `WM_PLLK1`    | `0x25` |
| `WM_PWRMGMT2` | `0x02` | `WM_PLLK2`    | `0x26` |
| `WM_PWRMGMT3` | `0x03` | `WM_PLLK3`    | `0x27` |
| `WM_AINTFCE`  | `0x04` | `WM_OUT4TOADC`| `0x2a` |
| `WM_CLKCTRL`  | `0x06` | `WM_OUTCTRL`  | `0x31` |
| `WM_ADDCTRL`  | `0x07` | `WM_LOUTMIX`  | `0x32` |
| `WM_DACCTRL`  | `0x0a` | `WM_ROUTMIX`  | `0x33` |
| `WM_LDACVOL`  | `0x0b` | `WM_LOUT1VOL` | `0x34` |
| `WM_RDACVOL`  | `0x0c` | `WM_ROUT1VOL` | `0x35` |
| `WM_OUT3MIX`  | `0x38` | `WM_LOUT2VOL` | `0x36` |
| `WM_OUT4MIX`  | `0x39` | `WM_ROUT2VOL` | `0x37` |
| `WM_BIASCTRL` | `0x3d` |              |        |

Register addresses are 7 bits; register *data* is 9 bits (bit 8 is a real
data bit — e.g. the volume-update `VU` latch bits and several `*EN` bits
live at bit 8). Source: `firmware/export/wm8758.h:45-315`.

## WM8758 bit values (init / volume / mute path)

| Constant | Reg | Value | Meaning |
|----------|-----|-------|---------|
| `PWRMGMT1_VMIDSEL_OFF`  | PWRMGMT1 | `0x000` | VMID divider off |
| `PWRMGMT1_VMIDSEL_75K`  | PWRMGMT1 | `0x001` | VMID 75 kΩ — normal operation (playback) |
| `PWRMGMT1_VMIDSEL_500K` | PWRMGMT1 | `0x002` | VMID 500 kΩ (low-power STANDBY hold; poor supply rejection — audible HDD/piezo/hiss on the device when used for playback) |
| `PWRMGMT1_VMIDSEL_10K`  | PWRMGMT1 | `0x003` | VMID 10 kΩ (fast startup) |
| `PWRMGMT1_BUFIOEN`      | PWRMGMT1 | `0x004` | tie-off buffer enable |
| `PWRMGMT1_BIASEN`       | PWRMGMT1 | `0x008` | analog bias enable |
| `PWRMGMT1_PLLEN`        | PWRMGMT1 | `0x020` | codec PLL enable |
| `PWRMGMT2_LOUT1EN`      | PWRMGMT2 | `0x080` | left headphone (OUT1) |
| `PWRMGMT2_ROUT1EN`      | PWRMGMT2 | `0x100` | right headphone (OUT1) |
| `PWRMGMT3_DACENL`       | PWRMGMT3 | `0x001` | left DAC enable |
| `PWRMGMT3_DACENR`       | PWRMGMT3 | `0x002` | right DAC enable |
| `PWRMGMT3_LMIXEN`       | PWRMGMT3 | `0x004` | left output mixer |
| `PWRMGMT3_RMIXEN`       | PWRMGMT3 | `0x008` | right output mixer |
| `PWRMGMT3_ROUT2EN`      | PWRMGMT3 | `0x020` | line-out OUT2 right |
| `PWRMGMT3_LOUT2EN`      | PWRMGMT3 | `0x040` | line-out OUT2 left |
| `AINTFCE_FORMAT_I2S`    | AINTFCE  | `0x010` | I²S data format (2<<3) |
| `AINTFCE_IWL_16BIT`     | AINTFCE  | `0x000` | 16-bit word length |
| `CLKCTRL_MS`            | CLKCTRL  | `0x001` | codec is I²S clock master |
| `CLKCTRL_BCLKDIV_2`     | CLKCTRL  | `0x004` | BCLK = SYSCLK/2 |
| `CLKCTRL_MCLKDIV_2`     | CLKCTRL  | `0x040` | SYSCLK = fPLLOUT/2 |
| `CLKCTRL_CLKSEL`        | CLKCTRL  | `0x100` | clock source = PLL |
| `ADDCTRL_SLOWCLKEN`     | ADDCTRL  | `0x001` | slow clock (ZC timeout) |
| `ADDCTRL_SR_48kHz`      | ADDCTRL  | `0x000` | rate-class hint (see note) |
| `DACCTRL_DACOSR128`     | DACCTRL  | `0x008` | 128× oversample (unmuted state) |
| `DACCTRL_SOFTMUTE`      | DACCTRL  | `0x040` | soft-mute the DAC |
| `LDACVOL_DACVU`         | LDACVOL  | `0x100` | latch L+R DAC volume now |
| `OUT1VOL_MUTE`          | L/ROUT1VOL | `0x040` | headphone amp mute |
| `OUT1VOL_ZC`            | L/ROUT1VOL | `0x080` | zero-cross gain change |
| `OUT1VOL_VU`            | L/ROUT1VOL | `0x100` | latch OUT1 gain now |
| `LOUTMIX_DACL2LMIX`     | LOUTMIX  | `0x001` | route left DAC → left mix |
| `ROUTMIX_DACR2RMIX`     | ROUTMIX  | `0x001` | route right DAC → right mix |
| `OUTCTRL_VROI`          | OUTCTRL  | `0x001` | tie disabled outputs to VREF |
| `OUTCTRL_TSDEN`         | OUTCTRL  | `0x002` | thermal-shutdown enable |
| `OUTCTRL_TSOPCTRL`      | OUTCTRL  | `0x004` | thermal-sense op control |
| `OUTCTRL_LINE_COM`      | OUTCTRL  | `0x080` | line-out common-mode |
| `OUTCTRL_HP_COM`        | OUTCTRL  | `0x100` | headphone common-mode |
| `OUT4TOADC_POBCTRL`     | OUT4TOADC| `0x004` | VMID-independent bias / pop ctrl |
| `OUT4TOADC_VMIDTOG`     | OUT4TOADC| `0x010` | VMID discharge toggle (close) |
| `BIASCTRL_BIASCUT`      | BIASCTRL | `0x100` | low-power bias cut |
| `PLLN_PLLPRESCALE`      | PLLN     | `0x010` | MCLK /2 prescale into PLL |
| `OUT34MIX_MUTE`         | OUT3/4MIX| `0x040` | OUT3/OUT4 mixer mute |

The preinit mute writes use `L/ROUT1VOL = L/ROUT2VOL = 0x140` (VU-set +
mute bit) and `OUT3MIX = OUT4MIX = 0x40` (mute). Source:
`firmware/export/wm8758.h:48-315`, `firmware/drivers/audio/wm8758.c:94-128`.

> **`ADDCTRL_SR` is a filter-class hint, not the real rate.** On the iPod
> the actual sample rate comes from the codec PLL preset + MCLKDIV, so the
> 44.1 kHz case legitimately programs the 48 kHz `SR` field value (`0x00`).
> Worth a driver comment so it doesn't read as a bug.

## WM8758 tone controls (5-band EQ)

The WM8758B carries a 5-band equaliser that can be switched onto either the
ADC (record) or DAC (playback) path. `core` drives all five bands on playback:
the two shelves are the Bass/Treble tone control, and an **EQ preset**
(`core/ui/eq.c`) is a full five-band curve. Source: **Wolfson WM8758B
datasheet, "Equaliser" section, register map R18–R22** (a primary hardware
datasheet, transcribed here — not Rockbox).

| Reg | Addr | Band | Role |
|-----|------|------|------|
| `WM_EQ1` | `0x12` | band 1 | **low shelf (Bass)** |
| `WM_EQ2` | `0x13` | band 2 | peaking |
| `WM_EQ3` | `0x14` | band 3 | peaking |
| `WM_EQ4` | `0x15` | band 4 | peaking |
| `WM_EQ5` | `0x16` | band 5 | **high shelf (Treble)** |

9-bit data layout, shared by all five registers:

- **`EQxG[4:0]`** (bits 4:0) — band gain, **`code = 12 − gain_dB`**, so
  `0x0C` = 0 dB (flat), `0x00` = +12 dB, `0x18` = −12 dB, 1 dB/step, range
  ±12 dB. Values outside `0x00..0x18` are reserved.
- **`EQxC[1:0]`** (bits 6:5) — band centre/cutoff select. The code means a
  different frequency in each band:

  | code | `EQ1C` shelf | `EQ2C` peak | `EQ3C` peak | `EQ4C` peak | `EQ5C` shelf |
  |------|------|------|------|------|------|
  | `00` | 80 Hz  | 230 Hz | 650 Hz  | 1.8 kHz | 5.3 kHz |
  | `01` | 105 Hz | 300 Hz | 850 Hz  | 2.4 kHz | 6.9 kHz |
  | `10` | 135 Hz | 385 Hz | 1.1 kHz | 3.2 kHz | 9 kHz |
  | `11` | 175 Hz | 500 Hz | 1.4 kHz | 4.1 kHz | 11.7 kHz |

  ⚠️ **Verify against the datasheet before trusting the three mid rows.** The
  shelf columns have been exercised on the device (the tone control has shipped
  on them); the `EQ2C`/`EQ3C`/`EQ4C` centres and the `EQxBW` polarity below were
  transcribed from the WM8758B PDF from memory and have **not** been listened to
  on hardware. A wrong code there is a band centred somewhere else — audibly a
  differently-shaped preset, never a fault.
- **`EQ3DMODE`** (bit 8, **`EQ1` register only**) — EQ path select:
  `0` = applied to the ADC (record), `1` = applied to the **DAC (playback)**.
- **`EQxBW`** (bit 8 of `EQ2`/`EQ3`/`EQ4`) — peaking-band bandwidth,
  `0` = wide, `1` = narrow. Band 5 has no bit 8, and band 1's is the path
  select, so **neither shelf may ever carry a bandwidth bit** —
  `hal_eq_set()` drops it there rather than trusting its caller.

| Constant | Reg | Value | Meaning |
|----------|-----|-------|---------|
| `EQ_GAIN_MASK`     | EQ1..EQ5 | `0x01F`| `EQxG[4:0]` gain field |
| `EQ_GAIN_0DB`      | EQ1..EQ5 | `0x0C` | flat (0 dB) gain code |
| `EQ_CUTOFF_SHIFT`  | EQ1..EQ5 | `5`    | position of `EQxC[1:0]` |
| `EQ_CUTOFF_MASK`   | EQ1..EQ5 | `0x060`| `EQxC[1:0]` field |
| `EQ_DAC_MODE`      | EQ1      | `0x100`| route EQ to the DAC (playback) |
| `EQ_BW_NARROW`     | EQ2..EQ4 | `0x100`| `EQxBW` = narrow bandwidth |
| `EQ1_CUTOFF_105HZ` | EQ1      | `0x020`| Bass shelf corner = 105 Hz (`EQ1C=01`) |
| `EQ5_CUTOFF_6K9`   | EQ5      | `0x020`| Treble shelf corner = 6.9 kHz (`EQ5C=01`) |
| `EQ_BAND_COUNT`    | —        | `5`    | EQ1..EQ5 |

### Driver policy (`core/hal/hw/volume.c`)

`hal_eq_set(gain_db[5], cutoff[5], narrow[5])` is the one register encoder and
the only way tone reaches the codec. The Bass/Treble sliders are not a second
entry point: they are the flat curve with the two shelves moved — mids at
`EQ_GAIN_0DB` on centre code `00` — which is what `ui/eq.c`'s
`eq_effective_curve()` hands over at EQ Off, so the words are the ones this
firmware has always emitted.

- **`EQ_DAC_MODE` is set on `EQ1` only when some band is non-zero.** At a flat
  curve the EQ stays on the (silent) ADC path and playback is bit-identical to
  no-EQ — the tone control and "EQ Off" are inert until the user moves them.
  On-device listen test confirms the shelves' polarity (boost vs. cut) and
  corner choice.
- **DAC pre-cut.** A boosting band is digital gain ahead of the DAC, so a
  full-scale track through, say, a +6 dB low shelf would clip. Every curve is
  therefore written with `LDACVOL`/`RDACVOL` set to
  `DACVOL_0DB − 2 × (largest boost in dB)` (the field is 0.5 dB per step), i.e.
  the boost is paid for in headroom rather than in distortion. A curve that only
  cuts leaves the DAC at `0xFF`, byte-identical to the bring-up sequence.
- **Write order follows the sign of the pre-cut change**, so the attenuation is
  never smaller than the boost that is live, at any instant *including the few
  I2C transactions inside `hal_eq_set`*:

  | change | order |
  |---|---|
  | new pre-cut **>** the one the codec holds (e.g. Off → Bass Booster) | `DACVOL` pair, then `EQ1..EQ5` — cut before boosting |
  | new pre-cut **<** it (e.g. Bass Booster → Off) | `EQ1..EQ5`, then the `DACVOL` pair — unboost before restoring level |
  | equal | `DACVOL` first (nothing to sequence; deterministic) |

  The driver tracks what the codec *holds*, not what the cached curve implies:
  `hal_codec_restore()` zeroes it first, because the cold-path `wm8758_init()`
  has just written `0xFF` over it, so a replay re-cuts before it re-boosts.
  Both directions are pinned as literal bus bytes in
  `tests/hw_mmio/volume_trace_test.c`.
- None of these registers has a zero-cross latch (unlike `OUT1VOL`), so a large
  step can be audible; `DACVU` on the right DAC write at least latches both
  channels together. If a preset change pops on the device, soft-mute
  (`DACCTRL_SOFTMUTE`, already used by `wm8758_mute`) around `hal_eq_set` is the
  fallback — **not yet needed or tried on hardware**.
- **The curve is cached in RAM** and replayed by `hal_codec_restore()` through
  the `wm8758_set_restore` hook, because every cold bring-up (boot, the Play
  after a persistent pause, the first play after a close) resets the codec.

The preset gains themselves are **not** a hardware fact and do not live here:
they are `core/ui/eq.c`'s table, host-tested, and this file only says how a
curve becomes register words.

## WM8758 sample-rate program: 44.1 kHz resolved

`audiohw_set_frequency(HW_FREQ_44)` reduces to six register writes. PLL
preset 0 gives fPLLOUT = 22.5792 MHz; MCLKDIV ÷2 → SYSCLK = 11.2896 MHz =
256 × 44.1 kHz.

| Order | Register | Value |
|-------|----------|-------|
| 1 | `PLLN`  (0x24) | `0x17` (`PLLPRESCALE \| N=7`) |
| 2 | `PLLK1` (0x25) | `0x21` |
| 3 | `PLLK2` (0x26) | `0x161` |
| 4 | `PLLK3` (0x27) | `0x26` |
| 5 | `CLKCTRL` (0x06) | `0x145` (`CLKSEL \| MCLKDIV_2 \| BCLKDIV_2 \| MS`) |
| 6 | `ADDCTRL` (0x07) | `0x001` (`SR_48kHz \| SLOWCLKEN`) |

Source: `firmware/drivers/audio/wm8758.c:238-276` (preset tables + write
order); the 48 kHz preset (preset 1) is `PLLN=0x18, K1=0x0C, K2=0x93,
K3=0xE9`.

## SoC I²S block — numeric bit values

Registers (all 32-bit, absolute; no base-relative offsets in the
reference): `IISCONFIG = 0x70002800`, `IISCLK = 0x70002808`,
`IISFIFO_CFG = 0x7000280C`, `IISFIFO_WR = 0x70002840`,
`IISFIFO_RD = 0x70002880`. Source: `firmware/export/pp5020.h:399-406`.

### `IISCONFIG` (0x70002800)

| Symbol | Value | Meaning |
|--------|-------|---------|
| `IIS_RESET`             | `0x80000000` | bit 31: soft-reset pulse |
| `IIS_TXFIFOEN`          | `0x20000000` | bit 29: enable TX FIFO / transmit |
| `IIS_RXFIFOEN`          | `0x10000000` | bit 28: enable RX FIFO |
| `IIS_MASTER`            | `0x02000000` | bit 25: SoC drives bus clocks (NOT set on iPod — codec is master) |
| `IIS_FORMAT_MASK`       | `0x00000C00` | bits 11:10 |
| `IIS_FORMAT_IIS`        | `0x00000000` | standard I²S, leading dummy bit |
| `IIS_SIZE_MASK`         | `0x00000300` | bits 9:8 |
| `IIS_SIZE_16BIT`        | `0x00000000` | 16-bit samples |
| `IIS_FIFO_FORMAT_MASK`  | `0x00000070` | bits 6:4 |
| `IIS_FIFO_FORMAT_LE16_2`| `0x00000070` | the value programmed on PP502x |
| `IIS_IRQTX`             | `0x00000002` | bit 1: TX IRQ flag |
| `IIS_IRQRX`             | `0x00000001` | bit 0: RX IRQ flag |

Source: `firmware/export/pp5020.h:409-458`. IRQ line: `IIS_IRQ = 10`
(`1<<10`), `pp5020.h:110,126`.

### `IISFIFO_CFG` (0x7000280C)

| Field / symbol | Value | Meaning |
|----------------|-------|---------|
| `IIS_TX_FREE` (read) | bits 21:16 (`0x003F0000`) | **live** count of free TX slots |
| `IIS_RX_FULL` (read) | bits 29:24 (`0x3F000000`) | live count of full RX slots |
| `IIS_RXCLR`          | `0x00001000` | bit 12: flush RX FIFO |
| `IIS_TXCLR`          | `0x00000100` | bit 8: flush TX FIFO |
| `IIS_RX_FULL_LVL_12` | `0x00000030` | RX attention at 12 slots |
| `IIS_TX_EMPTY_LVL_4` | `0x00000001` | TX DMA/IRQ request at 4 free slots |

`TX_FREE = (IISFIFO_CFG >> 16) & 0x3F`. TX FIFO depth = **16** slots
(derived: `IIS_TX_IS_EMPTY` ⇔ `TX_FREE >= 16`). Source:
`firmware/export/pp5020.h:463-497`.

### `i2s_reset()` config sequence (PP502x)

1. Pulse `IISCONFIG |= IIS_RESET` then clear it.
2. `IISCONFIG` FORMAT field ← `IIS_FORMAT_IIS` (0).
3. `IISCONFIG` SIZE field ← `IIS_SIZE_16BIT` (0).
4. `IISCONFIG` FIFO-format field ← `IIS_FIFO_FORMAT_LE16_2` (`0x70`).
5. `IISFIFO_CFG |= IIS_RX_FULL_LVL_12 | IIS_TX_EMPTY_LVL_4` (`0x31`).
6. `IISFIFO_CFG |= IIS_RXCLR | IIS_TXCLR` (`0x1100`) — flush both FIFOs.

`IIS_MASTER` is set only on AS3514 (Sansa) targets — **not** iPod, where
the WM8758 masters the bus clocks, so the SoC leaves `IISCLK` alone.
Source: `firmware/target/arm/pp/i2s-pp.c:53-95`.

## MCLK / clock-gating enable path

Bring the codec MCLK and I²S block alive **before** `i2s_reset()`. All in
the `0x60006000` clock block except the final EXT-clock select:

| Step | Write | Value |
|------|-------|-------|
| Pulse I²S out of reset | `DEV_RS \|= DEV_I2S` then `&= ~DEV_I2S` | `DEV_I2S = 0x800` |
| Ungate I²S block clock | `DEV_EN \|= DEV_I2S` | bit 11 = `0x800` |
| Enable external device clocks (codec MCLK) | `DEV_EN \|= DEV_EXTCLOCKS` | bit 1 = `0x002` |
| Select 24 MHz EXT clock | `*(0x70000018) &= ~0xC` | clear bits 3:2 |
| Route CDI+I2S pads to I²S fn | `*(0x70000020) &= ~0x300` | `DEV_INIT2` bits 9:8 |
| Route second pad group to I²S fn | `*(0x70000010) &= ~0x03000000` | `DEV_INIT1` bits 25:24 |

Source: `firmware/target/arm/pp/wmcodec-pp.c:48-70`, `pp5020.h:153,158,383-384`.
The other three encodings of the `0x70000018` bits 3:2 field are
undocumented; the reference only ever clears them ("EXT dev clock 24 MHz").

> The two `DEV_INIT` pad-mux clears are **load-bearing on a chainloaded
> device**: if Apple's flash ROM left the I²S pads in GPIO mode, the codec
> still ACKs on the (separate) I²C control bus but no bit-clock / data
> reaches it — silent audio with no error. Both fields are cleared on
> every PP502x iPod; `DEV_INIT1` bits 25:24 are of unconfirmed purpose
> (the reference comments them "mini2?") but clearing them mirrors the
> reference and is safe.

## Polled TX-FIFO write (first-sound path)

For the first-sound milestone we bypass DMA and pace samples by polling.
After codec init + `i2s_reset()`:

1. Start transmit: `IISCONFIG |= IIS_TXFIFOEN` (`0x20000000`).
2. Per stereo frame, spin until `((IISFIFO_CFG >> 16) & 0x3F) != 0` (TX FIFO
   has a free slot).
3. Write one 32-bit packed word to `IISFIFO_WR` (0x70002840):
   `word = ((uint32_t)right << 16) | (left & 0xFFFF)`.

`IISFIFO_WR` accepts a full 32-bit packed stereo store (confirmed:
`pcm-pp.c:599-603` writes 32-bit values, including `= 0` for silence).
**Word order** (left = low 16, right = high 16) is inferred from the
little-endian "left then right" halfword store order — verify by ear on
device (a wrong order only swaps L/R, still produces sound). Rockbox's own
polled fill gates on the same `TX_FREE` field (`pcm-pp.c:270-272`).

## DMA engine (continuous playback — NOT the first-sound path)

Recorded for the follow-up continuous-playback driver; the polled path
above needs none of it. Two blocks: a master control at `0x6000A000` and a
per-channel file at `0x6000B000` (stride `0x20`; channel 0 = playback).

| Register | Address | Purpose |
|----------|---------|---------|
| `DMA_MASTER_CONTROL` | `0x6000A000` | master enable (bit 31 = `0x80000000`) |
| `DMA_REQ_STATUS`     | `0x6000A008` | per-source request enable (`1 << req_id`) |
| `DMA0_CMD`           | `0x6000B000` | command + byte count (see below) |
| `DMA0_STATUS`        | `0x6000B004` | status; **reading it clears the IRQ** |
| `DMA0_RAM_ADDR`      | `0x6000B010` | source RAM address |
| `DMA0_FLAGS`         | `0x6000B014` | flags |
| `DMA0_PER_ADDR`      | `0x6000B018` | peripheral address = `&IISFIFO_WR` |
| `DMA0_INCR`          | `0x6000B01C` | increment/width |

`DMA0_CMD` fields: SIZE = bits 15:0 (**written as bytes − 4**), REQ_ID =
bits 19:16 (IIS request id = `2`), `WAIT_REQ = 1<<24`, `SINGLE = 1<<26`,
`RAM_TO_PER = 1<<27`, `INTR = 1<<30`, `START = 1<<31`. Playback config
(before OR-ing size and START) = `0x4D020000`. `DMA0_INCR = RANGE_FIXED
(1<<16) | WIDTH_32BIT (2<<28) = 0x20010000`. Enable the IIS request line
with `DMA_REQ_STATUS |= 1<<2` (`0x4`). Completion maps to CPU interrupt
source **26** (`DMA_MASK = 0x04000000`), routed at FIQ priority via
`CPU_INT_PRIORITY`. Source: `firmware/export/pp5020.h` (DMA defs),
`firmware/target/arm/pp/pcm-pp.c` (channel-0 setup + FIQ handler).
