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

Address `0x1A`. Protocol is "9-bit I²C": each register write is a 16-bit
value, where the register address is left-shifted by 1 bit on the bus
and the MSB of the data goes into the LSB of the address byte:

```c
void wmcodec_write(int reg, int data) {
    pp_i2c_send(0x1A,
                (reg << 1) | ((data & 0x100) >> 8),  // address byte
                data & 0xFF);                         // data byte
}
```

(`firmware/target/arm/pp/wmcodec-pp.c` line 42.)

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

(`firmware/drivers/audio/wm8758.c`, `audiohw_preinit()`, lines 94–128.)

```c
// 1. Bias mode for low noise floor
wmcodec_write(BIASCTRL, BIASCTRL_BIASCUT);

// 2. Output buffers + thermal/over-current protection
wmcodec_write(OUTCTRL, OUTCTRL_HP_COM | OUTCTRL_LINE_COM
                    | OUTCTRL_TSOPCTRL | OUTCTRL_TSDEN | OUTCTRL_VROI);

// 3. Mute everything before bringing up rails
wmcodec_write(LOUT1VOL, 0x140);
wmcodec_write(ROUT1VOL, 0x140);
wmcodec_write(LOUT2VOL, 0x140);
wmcodec_write(ROUT2VOL, 0x140);
wmcodec_write(OUT3MIX,  0x40);
wmcodec_write(OUT4MIX,  0x40);

// 4. Enable headphone outputs
wmcodec_write(PWRMGMT2, PWRMGMT2_LOUT1EN | PWRMGMT2_ROUT1EN);

// 5. VMID independent bias toggle on
wmcodec_write(OUT4TOADC, OUT4TOADC_POBCTRL);

// 6. Enable DACs and mixers
wmcodec_write(PWRMGMT3, PWRMGMT3_DACENL | PWRMGMT3_DACENR
                     | PWRMGMT3_LMIXEN | PWRMGMT3_RMIXEN);

// 7. Bias rails up: VMID at 10K (fast settle)
wmcodec_write(PWRMGMT1, PWRMGMT1_PLLEN | PWRMGMT1_BIASEN
                     | PWRMGMT1_BUFIOEN | PWRMGMT1_VMIDSEL_10K);

// 8. I²S, 16-bit
wmcodec_write(AINTFCE, AINTFCE_FORMAT_I2S | AINTFCE_IWL_16BIT);

// 9. Become I²S clock master (MCLK must already be running from SoC)
wmcodec_write(CLKCTRL, CLKCTRL_MS);

// 10. Default 44.1 kHz (configures PLL + dividers)
audiohw_set_frequency(HW_FREQ_44);

// 11. Route DAC to outputs (CRITICAL — without this, no audio path)
wmcodec_write(LOUTMIX, LOUTMIX_DACL2LMIX);
wmcodec_write(ROUTMIX, ROUTMIX_DACR2RMIX);

// 12. Disable independent bias toggle before postinit
wmcodec_write(OUT4TOADC, 0);
```

Then `audiohw_postinit()` switches VMID to low-power 500 K divider,
clears low-bias, and unmutes:

```c
wmcodec_write(PWRMGMT1, PWRMGMT1_PLLEN | PWRMGMT1_BIASEN
                     | PWRMGMT1_BUFIOEN | PWRMGMT1_VMIDSEL_500K);
wmcodec_write(BIASCTRL, 0);
audiohw_mute(false);
```

> Source comment: *"Important: DAC is global and will also affect
> lineout."* Setting DAC volume changes both outputs unless you
> compensate via the per-channel mixer.

## DAC sample-rate setup

`audiohw_set_frequency(HW_FREQ_xx)` reprograms the DAC's PLL and
output divider. The PLL has two preset operating points:

```c
static const unsigned short pll_setups[2][4] = {
    // f1 = 12 MHz MCLK, R = 7.5264, fPLLOUT = 22.5792 MHz
    { PLLN_PLLPRESCALE | 0x7, 0x21, 0x161, 0x26 },
    // f1 = 12 MHz MCLK, R = 8.192,  fPLLOUT = 24.576 MHz
    { PLLN_PLLPRESCALE | 0x8, 0x0C, 0x093, 0xE9 },
};
```

44.1k uses the 22.5792 MHz output; 48k uses the 24.576 MHz output.
SYSCLK is then 256× the sample rate.

```c
static const u8 freq_setups[HW_NUM_FREQ] = {
    [HW_FREQ_48] = CLKCTRL_MCLKDIV_2  | ADDCTRL_SR_48kHz | 1,
    [HW_FREQ_44] = CLKCTRL_MCLKDIV_2  | ADDCTRL_SR_48kHz,        // shares 48k PLL
    [HW_FREQ_32] = CLKCTRL_MCLKDIV_3  | ADDCTRL_SR_32kHz | 1,
    [HW_FREQ_24] = CLKCTRL_MCLKDIV_4  | ADDCTRL_SR_24kHz | 1,
    [HW_FREQ_22] = CLKCTRL_MCLKDIV_4  | ADDCTRL_SR_24kHz,
    [HW_FREQ_16] = CLKCTRL_MCLKDIV_6  | ADDCTRL_SR_16kHz | 1,
    [HW_FREQ_12] = CLKCTRL_MCLKDIV_8  | ADDCTRL_SR_12kHz | 1,
    [HW_FREQ_11] = CLKCTRL_MCLKDIV_8  | ADDCTRL_SR_12kHz,
    [HW_FREQ_8]  = CLKCTRL_MCLKDIV_12 | ADDCTRL_SR_8kHz  | 1,
};
```

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

### Initialization (`i2s_reset` in `firmware/target/arm/pp/i2s-pp.c`)

```c
IISCONFIG |= IIS_RESET;
IISCONFIG &= ~IIS_RESET;

IISCONFIG = (IISCONFIG & ~IIS_FORMAT_MASK)      | IIS_FORMAT_IIS;
IISCONFIG = (IISCONFIG & ~IIS_SIZE_MASK)        | IIS_SIZE_16BIT;
IISCONFIG = (IISCONFIG & ~IIS_FIFO_FORMAT_MASK) | IIS_FIFO_FORMAT_LE16_2;

IISFIFO_CFG |= IIS_RX_FULL_LVL_12 | IIS_TX_EMPTY_LVL_4;
IISFIFO_CFG |= IIS_RXCLR | IIS_TXCLR;
```

When the FIFO is below `IIS_TX_EMPTY_LVL_4` slots, DMA fires.

## MCLK: SoC supplies it to the DAC

The PP5022 provides MCLK to the WM8758 (the DAC's "master clock"
input — separate from the bit clock that the DAC then becomes I²S
master of).

```c
// Enable external device clocks
DEV_EN |= DEV_EXTCLOCKS;

// Set EXT clock to 24 MHz
outl(inl(0x70000018) & ~0xC, 0x70000018);
```

The DAC's internal PLL then produces SYSCLK = 256 × sample rate from
this 12 MHz reference (after its own divider).

> Order matters: MCLK must be running before we set `CLKCTRL_MS`
> on the DAC. Otherwise the DAC starts mastering on a clock that
> doesn't exist and the audible result is silence + a click on first
> sample.

## DMA: pumping PCM into the I²S FIFO

```c
#define DMA_PLAY_CONFIG \
    ((DMA_REQ_IIS << DMA_CMD_REQ_ID_POS) | \
     DMA_CMD_RAM_TO_PER | DMA_CMD_SINGLE | \
     DMA_CMD_WAIT_REQ | DMA_CMD_INTR)

void dma_tx_init(void) {
    DMA_MASTER_CONTROL |= DMA_MASTER_CONTROL_EN;
    CPU_INT_PRIORITY   |= DMA_MASK;             // FIQ priority for DMA
    DMA_REQ_STATUS     |= 1ul << DMA_REQ_IIS;   // enable IIS request line
}

void dma_tx_setup(void) {
    DMA0_PER_ADDR = (u32)&IISFIFO_WR;           // peripheral = I²S FIFO
    DMA0_FLAGS    = DMA_FLAGS_UNK26;
    DMA0_INCR     = DMA_INCR_RANGE_FIXED        // peripheral addr fixed
                  | DMA_INCR_WIDTH_32BIT;       // 32-bit transfers
}
```

PCM frame layout in DMA: 32-bit words, `[R_16][L_16]` (left in LSW,
right in MSW). The I²S serializer splits into two 16-bit serial frames
on the wire automatically.

### FIQ playback handler

```c
void fiq_playback(void) {
    DMA0_STATUS;                                 // clear IRQ
    size_t got = (DMA0_CMD & 0xFFFF) + 4;        // bytes just transferred
    dma_play_data.addr += got;
    dma_play_data.size -= got;

    if (LIKELY(dma_play_data.size != 0)) {
        dma_tx_start(false);                     // queue next chunk
    } else if (pcm_play_dma_complete_callback(...)) {
        dma_play_data.addr = dma_tx_buf_prepare(...);  // request fresh PCM
        dma_tx_start(false);
    }
}
```

Audio thread refills the buffer; DMA + FIQ keep the FIFO from
underrunning. If the audio thread is starved (e.g., during HDD
spin-up), the FIFO drains and the DAC outputs whatever the I²S block
last sent — usually silence with a brief click.

## Volume control

Two attenuators in series: DAC volume (digital, applies before output
amps) and output amp gain (analog).

```c
// DAC volume (global; 0..0xFF, ~ -inf to 0 dB)
wmcodec_write(LDACVOL, dac_value);
wmcodec_write(RDACVOL, dac_value | RDACVOL_DACVU);  // VU bit on right = "apply now"

// Headphone amp gain (per channel; 0..0x3F, -12 to +35.25 dB)
wmcodec_write(LOUT1VOL, amp | LOUT1VOL_LOUT1ZC);     // ZC = zero-cross gain change
wmcodec_write(ROUT1VOL, amp | ROUT1VOL_ROUT1ZC | ROUT1VOL_OUT1VU);
```

Same pattern for line-out (OUT2), with `LOUT2VOL` / `ROUT2VOL`.

## Lineout

```c
void audiohw_enable_lineout(bool enable) {
    int p = PWRMGMT3_DACENR | PWRMGMT3_DACENL
          | PWRMGMT3_LMIXEN | PWRMGMT3_RMIXEN;
    if (enable) p |= PWRMGMT3_LOUT2EN | PWRMGMT3_ROUT2EN;
    wmcodec_write(PWRMGMT3, p);
}
```

The dock connector's line-out pins are wired to OUT2.

## Mute

```c
void audiohw_mute(bool mute) {
    wmcodec_write(DACCTRL, mute ? DACCTRL_SOFTMUTE
                                 : DACCTRL_DACOSR128);
}
```

`DACOSR128` enables 128× oversampling (which is also the implicit
"unmuted" state).

## Shutdown

```c
void audiohw_close(void) {
    audiohw_mute(true);
    wmcodec_write(OUTCTRL, OUTCTRL_HP_COM | OUTCTRL_VROI);

    wmcodec_write(OUT4TOADC, OUT4TOADC_VMIDTOG);    // begin VMID discharge
    wmcodec_write(PWRMGMT1, PWRMGMT1_PLLEN | PWRMGMT1_BIASEN
                          | PWRMGMT1_VMIDSEL_OFF);
    sleep(3*HZ/10);                                  // 300 ms — VMID must drain
    wmcodec_write(PWRMGMT2, 0);
    wmcodec_write(PWRMGMT3, 0);
    wmcodec_write(PWRMGMT1, 0);
}

// I²S
void sink_stop_pcm(void) {
    dma_tx_stop();
    while (!IIS_TX_IS_EMPTY) ;
    dma_play_data.state = 0;
}
```

The 300 ms VMID drain is non-negotiable — skipping it gives a loud
DC pop in the headphones.

## Source citations

| Topic                | File |
|----------------------|------|
| DAC driver           | `firmware/drivers/audio/wm8758.c` |
| DAC I²C glue         | `firmware/target/arm/pp/wmcodec-pp.c` |
| I²S setup            | `firmware/target/arm/pp/i2s-pp.c` |
| PCM/DMA pump         | `firmware/target/arm/pp/pcm-pp.c` |
| Register map         | `firmware/export/wm8758.h`, `firmware/export/pp5020.h` |
