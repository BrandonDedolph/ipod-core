// Package fwpart models the iPod firmware partition: the image
// directory, the OSOS entry, capacity, the write set and the read-back
// verification. The code lives in fwpart.go; it reads and plans, and
// never writes. The checklist below is the contract any code added to
// this package must satisfy; it is kept verbatim from the deleted
// `core install` stub, which is where it was written.
//
// SAFETY CHECKLIST FOR THE WRITE PATH — READ BEFORE IMPLEMENTING
//
// install, flash and recover are stubs today, so none of the guards
// below exist yet. They are written down here because the cost of
// getting this wrong is someone else's 80 GB disk, and because the
// documented flow (detect -> confirm -> elevate -> write) has no step
// that proves the block device being written is actually an iPod.
//
// Any code that writes to a block device MUST do all of the following.
// A reviewer should reject a write path missing any one of them.
//
//	(a) PROVE THE TARGET IS AN IPOD — three independent checks, all
//	    required, before a single byte is written:
//	      1. USB VID/PID matches Apple (0x05AC) and a known iPod Video
//	         product ID. IMPLEMENTED AS: the vendor/product strings the
//	         OS already has (SCSI inquiry on Windows, sysfs on Linux,
//	         diskutil on macOS) must say Apple or iPod — see
//	         internal/disk.identify. Reading the real descriptors
//	         portably means libusb, which means cgo, which this binary
//	         does not use; the inquiry strings come from the same
//	         device and, unlike a descriptor, name the block device we
//	         are about to open.
//	      2. The Apple firmware-partition preamble is present and
//	         byte-exact at the start of partition 0, and the directory
//	         marker "]ih[" is at offset 0x100 (see
//	         core/docs/hw/08-boot-dock.md and internal/firmware). A
//	         device that enumerates right but has no preamble is not a
//	         device to write to.
//	      3. Disk size is consistent with the detected model. A 2 TB
//	         "iPod" is a USB enclosure someone left plugged in.
//	    VID/PID alone is not sufficient: the OS block-device path can
//	    be reassigned between enumeration and open.
//
//	(b) BACK UP FIRST — dump the entire existing firmware partition to
//	    a timestamped file (e.g. ~/.local/share/core/backups/
//	    fwpart-<serial>-<RFC3339>.bin) and fsync it before the first
//	    write. The Apple preamble must be preserved byte-exact; the
//	    boot ROM will not load a partition without it, and the only
//	    copy of a given device's may be the one on that device.
//
//	(c) VERIFY BY READ-BACK — after writing, re-read the written extent
//	    and compare a checksum against the source. Report failure
//	    loudly and point at the backup from (b). A write that is not
//	    read back is a write that was not verified.
//
//	(d) --dry-run AND A TYPED CONFIRMATION — --dry-run prints the exact
//	    device, extents and byte counts and writes nothing. Without it,
//	    require the user to type the device path (not "y") to proceed,
//	    unless --yes was passed. --yes exists today and skips a prompt
//	    nobody has written; it must not become a way to skip (a)–(c).
//
// The global --device flag selects the target when more than one iPod
// is connected (internal/disk.ErrMultipleDevices); read it with
// deviceFlag(cmd), and resolve it with internal/disk.SelectIPod.

package fwpart
