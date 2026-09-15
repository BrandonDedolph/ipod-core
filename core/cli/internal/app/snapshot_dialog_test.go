package app

import "strings"

// snapshotDialog is TestSnapshotFlashDialog's renderer. It lives in its
// own file rather than in snapshot.go because a production build has no
// way to open a dialog without a job behind it, and an exported hook
// that could would be a way to put a confirmation on screen that
// confirms nothing.
func snapshotDialog(st State, w, h int, out string) error {
	return snapshotWith(st, w, h, out, func(u *UI) {
		u.dlg = &dialogRequest{
			title:   "Write this image to the iPod?",
			body:    strings.TrimSpace(flashPlanSample),
			prompt:  `Type the device path  \\.\PhysicalDrive2  to confirm:`,
			want:    `\\.\PhysicalDrive2`,
			okLabel: "Write it",
			reply:   make(chan string, 1),
		}
		u.confirmEd.SetText(`\\.\PhysicalDrive`)
	})
}

const flashPlanSample = `
device      \\.\PhysicalDrive2  iPod Video 5.5G 80 GB  80.0 GB
partition   0: 129,024 .. 131,604,480 (131,475,456 bytes), sector 2048
image       C:\Users\brandon-home\ipod-bringup\core.ipod
            237,640 bytes, checksum 0x01589d64 (.ipod header verified)

OSOS entry  devOffset 0x4800, body at 0x5000, capacity 7,618,560
  old       len 237,640  chksum 0x01589d64
  new       len 241,912  chksum 0x0163b2a0
write 1     body      0x5000 .. 0x40E38  (241,912 bytes, zero-padded to 0x800)
write 2     directory 0x4200 .. 0x4400  (one sector, this row only)

backup      C:\Users\brandon-home\AppData\Roaming\core\backups\fwpart-131475456-2026-09-15T01-12-04Z.bin
            the WHOLE partition, fsynced and re-parsed before any write

Nothing else is touched: not the preamble, not the partition table, not
Apple's RSRC/AUPD/HIBE images. The write is read back and compared.

Windows will show a UAC prompt for:
  C:\Users\brandon-home\ipod-bringup\core.exe
`
