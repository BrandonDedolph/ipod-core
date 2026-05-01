# 07 — USB: ARC USBOTG controller + MSC stack

The PP5022 has an integrated USB-OTG controller built around the
**ARC (Applied Micro)** IP block. We use it as a device only —
host mode is not implemented.

The full stack has three layers:

1. **PHY + controller** — register-level driver for the ARC core
   (`firmware/target/arm/usb-drv-arc.c`).
2. **Core** — endpoint state machine, descriptor delivery,
   SET_CONFIGURATION dispatch (`firmware/usbstack/usb_core.c`).
3. **Mass Storage Class** — BBB transport, SCSI command dispatch,
   exclusive-storage interlock (`firmware/usbstack/usb_storage.c`).

We need all three, but we don't need any of the other class drivers
Rockbox ships (HID, ADB, charging-only-mode).

USB is historically the most painful subsystem to bring up clean.
This doc captures the iPod-specific PHY init quirks plus the MSC
contract Rockbox implements. ARC controller documentation, where it
exists, is in NXP / Freescale i.MX docs (the same IP appears there).

## Hardware addresses

| Item                  | Address       | Purpose |
|-----------------------|---------------|---------|
| Device controller     | `0x70000000`–`0x70000FFF` | ARC USBOTG MMIO |
| `DEV_EN`              | `0x6000600C`  | bit `0x08` = USB0, `0x400000` = USB1 |
| `DEV_RS`              | `0x60006004`  | analogous reset bits |
| GPIO insert detect    | `GPIOL[4]` (`0x10` mask of `GPIOL_INPUT_VAL`) | High = USB cable present |

Source: `firmware/target/arm/pp/usb-fw-pp502x.c`, `firmware/export/pp5020.h`.

## ARC controller register layout

Offsets relative to USB controller base.

| Register                | Offset  | Purpose |
|-------------------------|---------|---------|
| `REG_ID`                | `0x000` | Device ID |
| `REG_USBCMD`            | `0x140` | bit 0 = RUN, bit 1 = RESET, IRQ threshold |
| `REG_USBSTS`            | `0x144` | INT (0x01), ERR (0x02), PORT_CHANGE (0x04), RESET (0x40) |
| `REG_USBINTR`           | `0x148` | IRQ enable (matches USBSTS bits) |
| `REG_DEVICEADDR`        | `0x154` | USB device address (bits 31:25) |
| `REG_ENDPOINTLISTADDR`  | `0x158` | QH array base (must be 2 KB aligned) |
| `REG_PORTSC1`           | `0x184` | Speed (bits 27:26), Connect (`0x01`) |
| `REG_OTGSC`             | `0x1A4` | bit `0x200` = `A_VBUS_VALID` |
| `REG_USBMODE`           | `0x1A8` | `0x02` = device mode |
| `REG_ENDPTSETUPSTAT`    | `0x1AC` | Setup-packet pending flags (bit 0 = EP0) |
| `REG_ENDPTCOMPLETE`     | `0x1BC` | Completion flags |
| `REG_ENDPTCTRL(n)`      | `0x1C0 + 4n` | EP `n` control |

### Endpoint control register fields

For bulk endpoints:

| Field            | Bit  | Meaning |
|------------------|------|---------|
| `TX_ENABLE`      | 23   | Enable TX direction |
| `RX_ENABLE`      | 7    | Enable RX direction |
| `TX_TYPE`/`RX_TYPE` | 19:18 / 3:2 | 0 = control, 1 = bulk, 2 = int, 3 = iso |
| `TX_EP_STALL`    | 16   | Stall TX |
| `RX_EP_STALL`    | 0    | Stall RX |
| `TX_DATA_TOGGLE_RST` | 22 | Reset DATA toggle to DATA0 |
| `RX_DATA_TOGGLE_RST` | 6  | (RX) |

## Endpoints used for MSC

Standard Bulk-Only Mass Storage allocation:

- **EP0** — control, bidirectional, 64 bytes (FS) / 512 bytes (HS).
- **EP1** — bulk-IN (device → host).
- **EP2** — bulk-OUT (host → device).

(Source: `firmware/usbstack/usb_storage.c` lines 325–328.)

## Queue heads and transfer descriptors

ARC uses queue-head + transfer-descriptor (QH/dTD) DMA, similar to EHCI.

```c
struct queue_head {
    u32 max_pkt_length;   // bits 30:16 = max packet
    u32 curr_dtd_ptr;
    struct transfer_descriptor dtd;   // 32-byte overlay
    u32 setup_buffer[2];              // 8 bytes for EP0 setup
    u32 reserved;
    u32 status;                       // SW: halt status
    u32 length;                       // SW: bytes transferred
    u32 wait;                         // SW: blocking wait flag
};

struct transfer_descriptor {
    u32 next_td_ptr;        // next dTD pointer or terminate (0x1)
    u32 size_ioc_sts;       // length (30:16) | IOC (0x8000) | status (7:0)
    u32 buff_ptr0;          // 5 page pointers, each 4 KB
    u32 buff_ptr1;
    u32 buff_ptr2;
    u32 buff_ptr3;
    u32 buff_ptr4;
    u32 reserved;
};
```

Max transfer per dTD: **64 KB** (5 × 4 KB pages, page-aligned).
Rockbox chains 4 dTDs per endpoint = up to **256 KB** queued at once.

## Mass Storage Class: BBB protocol

### Command Block Wrapper (host → device)

```c
struct command_block_wrapper {
    u32 signature;            // 'USBC' = 0x43425355
    u32 tag;
    u32 data_transfer_length;
    u8  flags;                // bit 7: 1 = IN, 0 = OUT
    u8  lun;
    u8  command_length;       // 6..16
    u8  command_block[16];    // SCSI CDB
};
```

### Command Status Wrapper (device → host)

```c
struct command_status_wrapper {
    u32 signature;            // 'USBS' = 0x53425355
    u32 tag;                  // echo CBW tag
    u32 data_residue;
    u8  status;               // 0 = good, 1 = fail
};
```

## SCSI commands Rockbox handles

(`usb_storage.c` `handle_scsi()` from line 770.)

| Op                  | Code  | Purpose |
|---------------------|-------|---------|
| `TEST_UNIT_READY`   | `0x00`| Verify exclusive lock acquired |
| `INQUIRY`           | `0x12`| Vendor / product strings; mark removable |
| `REQUEST_SENSE`     | `0x03`| Return key / ASC / ASCQ from last error |
| `READ_CAPACITY_10`  | `0x25`| Block count + size (512 or 1024) |
| `MODE_SENSE_6`/`_10`| `0x1A`/`0x5A` | Writable flag, geometry |
| `READ_10`           | `0x28`| Read 32-bit LBA, 16-bit count |
| `READ_16`           | `0x88`| Read 64-bit LBA (drives > 2 TB) |
| `WRITE_10`          | `0x2A`| Write 32-bit LBA |
| `WRITE_16`          | `0x8A`| Write 64-bit LBA |
| `WRITE_BUFFER`      | `0x3B`| iTunes proprietary RTC sync (skip) |
| `START_STOP_UNIT`   | `0x1B`| Eject / power |
| `ALLOW_MEDIUM_REMOVAL` | `0x1E` | Tracked but not enforced |
| `REPORT_LUNS`       | `0xA0`| List LUNs |

### Sense data layout

```c
struct sense_data {
    u8 ResponseCode;                 // 0x70 = current error
    u8 fei_sensekey;                 // NOT_READY=2, ILLEGAL_REQ=5, UNIT_ATTN=6
    u8 AdditionalSenseCode;          // ASC: NOT_PRESENT=0x3A, INVALID_CMD=0x20
    u8 AdditionalSenseCodeQualifier; // ASCQ: 0x01 = becoming ready
};
```

## Exclusive-storage interlock

Rockbox can't safely expose the disk over MSC while it's also using
it for music playback — a host write would corrupt the FAT or stomp
buffered data. The handoff uses an explicit lock.

Flow:

1. **USB inserted** — GPIO IRQ fires. Driver enters device mode.
2. **Host enumerates** + sends `SET_CONFIGURATION(n)`. Core calls
   `usb_request_exclusive_storage()`.
3. **Lock acquisition** — broadcasts `SYS_USB_CONNECTED` to all
   threads; each must ACK before the lock is granted.
4. **Unmount** — `usb_slave_mode(true)` calls `disk_unmount_all()`.
   Rockbox releases the ATA controller entirely.
5. **MSC commands** read/write via `storage_read_sectors()` /
   `storage_write_sectors()` directly (no FS layer).
6. **Disconnect** — `usb_release_exclusive_storage()` re-mounts
   filesystems via `disk_mount_all()`.

Every SCSI command first checks:

```c
if (!usb_exclusive_storage()) {
    send_csw(UMS_STATUS_FAIL);
    cur_sense_data.sense_key = SENSE_NOT_READY;
    return;
}
```

(`usb_storage.c` line 827.)

For our firmware, this means: a "safe to update" screen on USB
connect is naturally implemented by *not* re-mounting the filesystems
until the user disconnects.

## VBUS / connection detection

```c
bool usb_drv_powered(void) {
    return (REG_OTGSC & OTGSC_A_VBUS_VALID) ? true : false;
}

bool usb_plugged(void) {
    return (GPIOL_INPUT_VAL & 0x10) == 0x10;
}
```

GPIO interrupt fires on cable insert/remove; debounced 200 ms
(`HZ/5`) before broadcasting `USB_INSERTED` / `USB_EXTRACTED`.

## Init sequence

### `usb_reset_controller()` — `firmware/target/arm/pp/usb-fw-pp502x.c` 102–132

```c
DEV_EN   |= 0x400008;             // enable USB0 + USB1
DEV_RS   |= 0x400008;             // reset
DEV_RS   &= ~0x400008;            // release

DEV_INIT2 |= 0x80000000;          // INIT_USB
while (!(*(vu32*)0x70000028 & 0x80)) ;   // wait for PLL

*(vu32*)0x7000003C |= 0x47A;       // XMB_RAM_CFG (cache/DMA routing)
```

### PP5022/5024 PHY quirk

This is the load-bearing magic that makes USB enumerate at all on
the iPod Video. Without it, `usb_reset_controller` returns success
but the host sees nothing on the bus.

```c
// usb_drv_reset, lines 394–416
outl(inl(0x70000060) |  0xF,     0x70000060);       // PHY init
outl(inl(0x70000028) |  0x10000, 0x70000028);       // PHY clock on
outl(inl(0x70000028) & ~0x10000, 0x70000028);       // clock off
// ...
outl(inl(0x70000028) |  0x800,   0x70000028);       // enable PLL
while ((inl(0x70000028) & 0x80) == 0) ;             // wait PLL lock
```

Source comment credits the iPodLinux project for reverse-engineering
this. Treat the values as opaque.

### `usb_drv_init()` — `usb-drv-arc.c` 455–500

```c
usb_drv_reset();                              // soft reset + PHY quirk
REG_USBMODE = 0x02;                           // device mode
init_control_queue_heads();                   // EP0 setup
REG_ENDPOINTLISTADDR = (u32)qh_array;         // 2 KB aligned
REG_USBINTR = INT_EN | ERR_EN | PTC_EN | RESET_EN;
REG_USBCMD |= USBCMD_RUN;

// Configure bulk endpoints
REG_ENDPTCTRL(1) = TX_BULK | TX_ENABLE;
REG_ENDPTCTRL(2) = RX_BULK | RX_ENABLE;
```

## Known quirks

1. **Pipe completion vs IOC**: "Controller sets the pipe bit to one
   even if the TD doesn't have IOC bit set." Must verify via
   `DTD_STATUS_ACTIVE`, not just the completion register. (`usb_storage.c` 1100–1107.)

2. **Unconfigured EP control**: "Caution: Leaving an unconfigured
   endpoint control will cause undefined behavior for data PID
   tracking." All EPs must be configured before the first SETUP. (lines 493–495.)

3. **Zero-length termination (ZLT)**: MSC bulk transfers set
   `QH_ZLT_SEL` to allow short final packets without short-packet
   stalls. (line 1015.)

4. **Virtual sector size**: iPod Video exposes **2048-byte logical
   sectors** to USB even though the underlying ATA is 512-byte. This
   is a quirk of how iTunes expects to see the drive. Set
   `MAX_VIRT_SECTOR_SIZE = 2048`. (line 2300+.)

## Read/write buffer pipelining

```c
#define READ_BUFFER_SIZE  (1024 * 64)   // 64 KB
#define WRITE_BUFFER_SIZE (1024 * 24)   // 24 KB
```

Double-buffered: ATA fills `data[1]` while USB drains `data[0]`,
then they swap. Effectively turns USB latency and ATA latency into
parallel work.

## What we won't bother with

- HID class (Rockbox uses it for media-key consumer-control reports).
- ADB (the ancient pre-iPod Apple connector protocol).
- Charging-only mode (where the device connects but doesn't expose
  storage). We can do this just by not enabling the MSC class on
  certain USB connections.
- USB host mode (we never need to be host).

## Source citations

| Topic                | File |
|----------------------|------|
| GPIO + PHY init      | `firmware/target/arm/pp/usb-fw-pp502x.c` |
| ARC driver           | `firmware/target/arm/usb-drv-arc.c` |
| MSC class            | `firmware/usbstack/usb_storage.c` |
| Core / control       | `firmware/usbstack/usb_core.c` |
| Storage interlock    | `firmware/usb.c` |
| Register definitions | `firmware/export/pp5020.h` |
