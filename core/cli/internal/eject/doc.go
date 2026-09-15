// Package eject flushes a volume and hands it back to the operating
// system, so the cable can be pulled without leaving a FAT directory
// that names files whose clusters were never written.
//
// It is one exported function, Eject, with a different body per OS —
// the volume IOCTLs on Windows, udisks on Linux, diskutil on macOS.
// The bodies were `core eject`'s until S10: the desktop app needs the
// same button, and two implementations of "is it safe to unplug now"
// is exactly the kind of duplication that ends with one of them
// claiming a flush that did not happen.
//
// Every implementation writes what it actually did to w. That is not
// decoration: the user is about to unplug hardware, and "flushed but
// not ejected, run this yourself" and "dismounted and ejected" are
// different facts that a bare error return cannot distinguish.
package eject
