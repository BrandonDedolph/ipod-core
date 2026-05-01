# 04 — ATA / IDE storage controller

The PP5022 has an integrated PIO + UDMA-capable ATA (IDE) controller.
The iPod 5G/5.5G connects it to a 1.8" Toshiba HDD; users with iFlash
or CF-adapter mods present the same interface but with an SSD on the
other end. Rockbox handles both transparently.

This is one of the more complex subsystems because:

- Multiple addressing modes (LBA28 / LBA48), multiple transfer modes
  (PIO / MWDMA / UDMA at 5 different speeds), and a tangle of ATA
  commands.
- The PP IDE controller has known quirks (reading status during DMA
  hangs the core; alignment requirements differ for read vs write).
- Power management is split between the iPod's PMIC, the SoC, and
  the drive itself.

This doc covers what the host CPU needs to do to drive it; the ATA
spec itself is the reference for command semantics.

## Register layout

Two register regions: PIO (control + data) and DMA (when used).

### PIO registers (relative to `IDE_BASE`)

| Register         | Offset  | Width | Purpose |
|------------------|---------|-------|---------|
| `ATA_DATA`       | `+0x1E0`| 16    | Data port (256 words per sector) |
| `ATA_ERROR`      | `+0x1E4`| 8     | Read: error codes |
| `ATA_FEATURE`    | `+0x1E4`| 8     | Write: feature/subcommand |
| `ATA_NSECTOR`    | `+0x1E8`| 8     | Sector count |
| `ATA_SECTOR`     | `+0x1EC`| 8     | LBA[7:0] |
| `ATA_LCYL`       | `+0x1F0`| 8     | LBA[15:8] |
| `ATA_HCYL`       | `+0x1F4`| 8     | LBA[23:16] |
| `ATA_SELECT`     | `+0x1F8`| 8     | Device/head + LBA[27:24] |
| `ATA_COMMAND`    | `+0x1FC`| 8     | Write command / read status |
| `ATA_ALT_STATUS` | `+0x3F8`| 8     | Read: status without side effects |
| `ATA_CONTROL`    | `+0x3F8`| 8     | Write: nIEN, SRST |

### Per-CPU base

| CPU    | `IDE_BASE`   | DMA base    |
|--------|--------------|-------------|
| PP5020 | `0xC3000000` | `0xC3000400`|
| PP5002 | `0xC0003000` | (no DMA)    |

The iPod 5G/5.5G uses PP5022 — same as PP5020 for ATA purposes.

### DMA registers (PP5020/22)

| Register             | Address       | Purpose |
|----------------------|---------------|---------|
| `IDE_DMA_CONTROL`    | `0xC3000400`  | DMA enable / direction / start |
| `IDE_DMA_LENGTH`     | `0xC3000408`  | Length in bytes − 4 |
| `IDE_DMA_ADDR`       | `0xC300040C`  | Buffer physical address |

### Configuration registers (PP5020/22)

| Register              | Address       | Purpose |
|-----------------------|---------------|---------|
| `IDE0_PRI_TIMING0`    | `0xC3000000`  | PIO timing |
| `IDE0_PRI_TIMING1`    | `0xC3000004`  | MWDMA + UDMA timing |
| `IDE0_CFG`            | `0xC3000028`  | Controller config + DMA INTRQ status |
| `IDE0_CNTRLR_STAT`    | `0xC30001E0`  | Controller status |

## Status / error bits

| Status flag    | Value | Meaning |
|----------------|-------|---------|
| `STATUS_BSY`   | `0x80`| Drive busy |
| `STATUS_RDY`   | `0x40`| Ready to accept commands |
| `STATUS_DF`    | `0x20`| Drive fault |
| `STATUS_DRQ`   | `0x08`| Data request |
| `STATUS_ERR`   | `0x01`| Error during command |

| Error flag     | Value | Meaning |
|----------------|-------|---------|
| `ERROR_IDNF`   | `0x10`| ID not found / invalid LBA |
| `ERROR_ABRT`   | `0x04`| Command aborted (unsupported) |

## Initialization sequence

Roughly follows ATA spec ch. 9.1.

```c
ata_device_init();   // controller timing + DMA config
ata_enable(true);    // power IDE block
ide_power_enable(true);  // power the drive itself
sleep(HZ/4);         // 250 ms — disk voltage stabilizes
```

> **Quirk**: "Accessing the PP IDE controller too early after powering
> up the disk makes the core hang for a short time, causing an audio
> dropout. iPod Mini G2 needs at least HZ/5 to get rid of the dropout."
> Use a generous sleep on cold boot.

### Hard reset (cold start)

1. `ata_reset()` — target-specific; on PP this is a no-op.
2. Write `ATA_SELECT` to choose master.
3. Spin until `BSY` clears (up to 30 s).

### Soft reset (after error)

```c
ATA_OUT8(ATA_CONTROL, CONTROL_nIEN | CONTROL_SRST);
sleep(1);          // ≥ 5 µs

#ifdef HAVE_ATA_DMA
ATA_OUT8(ATA_CONTROL, 0);             // clear nIEN, enable INTRQ for DMA
#else
ATA_OUT8(ATA_CONTROL, CONTROL_nIEN);
#endif
sleep(1);          // > 2 ms

wait_for_rdy();   // up to 30 s, 8 retries
```

### Identify drive (CMD `0xEC`)

```c
ATA_OUT8(ATA_SELECT, ata_device);
wait_for_rdy();
ATA_OUT8(ATA_COMMAND, 0xEC);
wait_for_start_of_transfer();
read_256_words_from(ATA_DATA);
```

The 256-word identify block is the source of truth for: total LBA,
LBA48 capability, supported features, multi-sector size, transfer
modes, model string, firmware version, drive class (HDD vs SSD/CF).

### Set features (transfer mode + power management)

| Feature | Subcmd | Param | Capability check (identify word) |
|---------|--------|-------|----------------------------------|
| Force PIO mode      | `0x03`| `0x08 | mode_2_to_4` | word 83 bit 14 |
| Set DMA mode        | `0x03`| `0x20..0x4x` | word 88 (UDMA) / word 63 (MWDMA) |
| Advanced PM         | `0x05`| `0x80` (min w/o standby) | word 83 bit 3 |
| Acoustic management | `0x42`| `0x80` (lowest noise) | word 83 bit 9 |
| Volatile write cache| `0x02`| 0 (enable) | word 82 bit 5 |
| Read look-ahead     | `0xAA`| 0 (enable) | word 82 bit 6 |

Rockbox sequence: PIO → DMA → APM → AAM → cache → look-ahead.

### Set multiple mode (CMD `0xC6`)

```c
ATA_OUT8(ATA_NSECTOR, max_sectors_per_drq);   // 1..256
ATA_OUT8(ATA_COMMAND, 0xC6);
wait_for_rdy();
```

`max_sectors_per_drq` comes from identify word 47 or 59. Setting this
reduces interrupt overhead on multi-sector transfers.

### Security freeze lock (CMD `0xF5`)

If `identify[82] & 0x02` (security feature supported):

```c
ATA_OUT8(ATA_COMMAND, 0xF5);
wait_for_rdy();
```

Prevents anyone (including a malicious USB host) from issuing ATA
security commands later.

## PIO timing values

The PP IDE controller takes timing values for each PIO mode in
`IDE0_PRI_TIMING0`. At 80 MHz operation:

```c
static const u32 pio80mhz[] = {
    0xC293, 0x43A2, 0x11A1, 0x7232, 0x3131    // modes 0..4
};
IDE0_PRI_TIMING0 = pio80mhz[selected];
IDE0_PRI_TIMING1 = 0x80002150;
```

Source: `firmware/target/arm/pp/ata-pp5020.c` lines 54–87.

## DMA timing values

```c
static const u32 tm_mwdma[] = {
    0xF9F92,    // MWDMA 0
    0x56562,    // MWDMA 1
    0x45451,    // MWDMA 2
};
static const u32 tm_udma[] = {
    0x800037C1, // UDMA 0  @ ≥ 30 MHz
    0x80003491, // UDMA 1  @ ≥ 30 MHz   (stable at 24 MHz too)
    0x80003371, // UDMA 2  @ ≥ 30 MHz   (stock iPod mode)
    0x80003271, // UDMA 3  needs CPU boost
    0x80003071, // UDMA 4  needs CPU boost
};
IDE0_PRI_TIMING1 = tm_udma[mode];
```

> **Stability note from the source**: UDMA 2 stable at 30 MHz; UDMA 1
> stable at 24 MHz; iPod 4G uses slower mode due to reported
> instabilities.

## Read / write paths

### Multi-sector PIO read (LBA28; CMD `0xC4`)

```c
ATA_OUT8(ATA_NSECTOR, count & 0xFF);             // 0 = 256
ATA_OUT8(ATA_SECTOR,  lba & 0xFF);
ATA_OUT8(ATA_LCYL,   (lba >>  8) & 0xFF);
ATA_OUT8(ATA_HCYL,   (lba >> 16) & 0xFF);
ATA_OUT8(ATA_SELECT, ((lba >> 24) & 0xF) | 0x40 | device);
ATA_OUT8(ATA_COMMAND, 0xC4);

for each sector:
    wait_for_start_of_transfer();    // BSY = 0, DRQ = 1
    read_256_words_from(ATA_DATA);
    check_status();                  // ERR/DF
    wait_for_end_of_transfer();      // BSY = 0, RDY = 1, DRQ = 0
```

Insert ~400 ns of NOPs between command write and first status read
(command pipeline guard).

### LBA48 read (CMD `0x29`)

The 48-bit form writes each register *twice* (high then low), then
issues the EXT command:

```c
ATA_OUT8(ATA_NSECTOR, (count >>  8) & 0xFF);   // high
ATA_OUT8(ATA_NSECTOR,  count        & 0xFF);   // low
ATA_OUT8(ATA_SECTOR,  (lba >> 24) & 0xFF);     // bits 31..24
ATA_OUT8(ATA_SECTOR,   lba        & 0xFF);     // bits  7..0
ATA_OUT8(ATA_LCYL,    (lba >> 32) & 0xFF);     // bits 39..32
ATA_OUT8(ATA_LCYL,    (lba >>  8) & 0xFF);     // bits 15..8
ATA_OUT8(ATA_HCYL,    (lba >> 40) & 0xFF);     // bits 47..40
ATA_OUT8(ATA_HCYL,    (lba >> 16) & 0xFF);     // bits 23..16
ATA_OUT8(ATA_SELECT, 0x40 | device);
ATA_OUT8(ATA_COMMAND, 0x29);
```

### DMA read (CMD `0xC8` or `0x25`)

Setup phase:

```c
bool ata_dma_setup(void *buf, u32 nbytes, bool write) {
    if (!write && ((unsigned long)buf & 15)) return false;  // 16-byte align reads
    IDE_DMA_CONTROL |= 2;            // enable
    IDE_DMA_LENGTH   = nbytes - 4;
    IDE_DMA_ADDR     = (u32)buf;
    IDE_DMA_CONTROL |= 0x08;         // direction = read
    IDE0_CFG        |= 0x8000;       // arm DMA
    return true;
}
```

Issue command (same register sequence as PIO above), then:

```c
void ata_dma_finish(void) {
    IDE_DMA_CONTROL |= 1;            // start
    ata_wait_intrq();                // poll IDE0_CFG bit 0x08, NOT ATA_STATUS
    IDE0_CFG        &= ~0x8000;      // disarm DMA
    IDE_DMA_CONTROL &= ~0x80000001;  // stop
}
```

> **CRITICAL QUIRK**: "Reading standard ATA status while DMA is in
> progress causes failures and hangs." Use the `IDE0_CFG` INTRQ bit
> instead.

### Write differences

- Command codes: `0xC5` (LBA28) / `0x39` (LBA48 EXT) / `0xCA` (DMA) /
  `0x35` (LBA48 DMA EXT).
- DMA write alignment: 4-byte (vs 16-byte for read).
- Cache: call `commit_dcache()` before DMA write setup, and
  `commit_discard_dcache()` after DMA read finish.
- For HDDs (not SSDs), DMA writes are skipped; PIO is used. SSDs/CF
  use DMA both directions.

### Per-sector error handling

```c
if (status & (BSY | ERR | DF)) {
    soft_reset();
    if (error & ERROR_IDNF) break;   // bad LBA — don't retry
    goto retry;
}
```

### Timeouts

| Operation                  | Timeout |
|----------------------------|---------|
| `wait_for_bsy`             | 30 s    |
| `wait_for_rdy` (post-BSY)  | 10 s    |
| Multi-sector inner loop    | 5 s per sector |
| DMA finish (`ata_wait_intrq`) | 10 s |

## Spin-up / spin-down

### Idle timeout

`ata_spindown(seconds)` arms a timer; when no I/O for that duration,
`ata_sleepnow()` is called.

### `ata_sleepnow()`

```c
if (ata_state >= ATA_SPINUP) {
    mutex_lock(&ata_mutex);
    if (ata_state == ATA_ON) {
        ata_perform_flush_cache();      // FLUSH_CACHE first
        ata_perform_sleep();            // STANDBY IMMEDIATE (CMD 0xE0)
        ata_state = ATA_SLEEPING;
        power_off_tick = current_tick + 2*HZ;
    }
    mutex_unlock(&ata_mutex);
}
```

### `STANDBY IMMEDIATE` (CMD `0xE0`)

```c
ATA_OUT8(ATA_SELECT, ata_device);
wait_for_rdy();
ATA_OUT8(ATA_COMMAND, 0xE0);
wait_for_rdy();
```

Effects:
- Drive flushes its own DRAM cache to media.
- Heads park.
- Drive is in PM2 (Standby).
- **Safe to cut power after this returns.**

### Spin-up detection

The first I/O after sleep transitions `ata_state` from `SLEEPING` to
`SPINUP`, then to `ON` after the first `DRQ` arrives. The elapsed time
is recorded in `spinup_time` for telemetry.

## Power-management commands

### Flush cache

```c
ATA_OUT8(ATA_COMMAND, 0xEA);   // FLUSH_CACHE_EXT (preferred)
// or 0xE7 (CMD_FLUSH_CACHE) for older drives
```

Selection logic:

1. If drive has no cache → skip.
2. If LBA48 and word 83 bit 13 set → use EXT (`0xEA`).
3. Else if word 83 bit 12 set (ATA-6+) → use non-EXT (`0xE7`).
4. Else if word 80 ≥ `1 << 5` (ATA-5+) → use non-EXT.
5. Else → mark `canflush = false`; skip.

Always issue before `STANDBY IMMEDIATE` and on unmount.

## HDD vs SSD/CF detection

```c
bool ata_disk_isssd(void) {
    return (identify[217] == 0x0001)             // nominal rotation = "non-rotating"
        || ((identify[168] & 0x0F) >= 0x06)       // form factor
        || (identify[169] & (1 << 0))             // TRIM
        || (identify[163] > 0)                    // CF advanced timing
        || ((identify[83] & (1 << 2))             // CFA command set
            && ((identify[160] & (1 << 15)) == 0));
}
```

Differences from HDD:

| Behavior        | HDD        | SSD / CF  |
|-----------------|------------|-----------|
| DMA reads       | Yes        | Yes       |
| DMA writes      | No (PIO)   | Yes       |
| Spin-down       | Yes        | Skipped   |
| `ata_disk_can_sleep` | check identify words 82 / 85 bit 3 | usually false |

## Source citations

| Topic                | File |
|----------------------|------|
| Generic driver       | `firmware/drivers/ata.c` |
| Target macros        | `firmware/target/arm/pp/ata-target.h` |
| PP5020 specifics     | `firmware/target/arm/pp/ata-pp5020.c` |
| Register definitions | `firmware/export/pp5020.h` |
| ATA constants        | `firmware/export/ata-defines.h` |
