#!/usr/bin/env python3
"""
clicky_smoke.py — boot the real firmware image in the clicky emulator and
hold its SER0 transcript to a golden file.

WHY THIS EXISTS. boot/crt0.S (vector table, COP park, MMAP0 remap from an
IRAM stub, .bss zero, banked stacks, the IRQ entry) and kernel/switch.S
(context switch + task trampoline) are the code whose failure mode is a
device that does not boot, recoverable only by the Select+Play hardware
reset. Every host-side test in tests/meson.build compiles the C around
them and stubs the asm out; none of them executes a single one of those
instructions. Until this script they had been run only by hand in the
emulator. This runs them in CI, from the linked image, with real ARMv4T
semantics, and fails the build when the boot log changes.

WHAT IT PROVES. Each `core: ` line in the golden is a milestone the
firmware can only print by having got there: `kernel alive` is C running
at link addresses after the remap; `timer init ... enabling IRQs` is the
banked IRQ stack + vector table armed; `idle task entered` is printed
from inside a task that is only reachable through switch_context and
task_trampoline in kernel/switch.S. The transcript must match the golden
EXACTLY — a doubled line is the COP-park regression (both cores racing
through kernel_main; caught by hand 2026-06-11), a missing one is a hang
or a crash. After the last line the emulator is held for a settle window:
a timer interrupt is delivered inside it (verified with the emulator's
GDB stub: irq_vector_entry at 0x128 is hit right after idle_task), so a
broken IRQ entry lands in the fault vectors, whose panic() prints over
the same UART and fails the "nothing further" check; a broken return
from the IRQ (the `ldmfd ... ^`) does the same or takes the emulator
down, which is also a failure.

WHAT IT CANNOT PROVE. Anything the emulator does not model: the BCM
display controller (lcd_init() returns 0 there, so the disk/UI path is
never taken — the on-device boot goes the other way at exactly the `lcd
bcm` line), the PLL lock, I2C/I2S/DAC, the drive, the click wheel, and
the real boot ROM handoff (clicky's HLE loader fakes it). None of the
timing numbers the firmware prints elsewhere mean anything here.

The run stops as soon as the golden is satisfied, so the wall-clock cap
is a failure path, not the normal duration (~5 s locally; the 12 MB .bss
zero loop is most of it).

Usage:
  clicky_smoke.py --clicky <clicky-desktop> --fw <image.fw> \
                  --golden tests/clicky/boot_uart.golden [--settle 3] \
                  [--timeout 120] [--stderr <file>]

The emulator's minifb window needs a display; its headless build exits
before running a single instruction (main returns while the emulator
thread is still starting), so run this under `xvfb-run -a`. The image is
produced by tests/scripts/make_clicky_fw.py from build-hw/core.bin.
"""

import argparse
import os
import queue
import re
import signal
import subprocess
import sys
import threading
import time

PREFIX = "core: "
HEX32 = re.compile(r"[0-9A-Fa-f]{8}")


def load_golden(path):
    """Golden lines as compiled regexes; `<hex32>` matches eight hex digits."""
    lines = []
    with open(path, encoding="utf-8") as f:
        for raw in f:
            line = raw.rstrip("\r\n")
            if not line or line.startswith("#"):
                continue
            if not line.startswith(PREFIX):
                sys.exit(f"{path}: golden line does not start with {PREFIX!r}: {line!r}")
            parts = [re.escape(p) for p in line.split("<hex32>")]
            lines.append((line, re.compile("^" + HEX32.pattern.join(parts) + "$")))
    if not lines:
        sys.exit(f"{path}: golden is empty")
    return lines


def pump(stream, q):
    """Reader thread: push each decoded stdout line, then None at EOF.

    CR is stripped from BOTH ends. uart_putc sends "\\r\\n" for a newline, and
    there is also a stray CR before the very first line: uart_init programs
    the 115200 divisor (0x0D, which is CR) into DLL, which shares +0x00
    with THR, and clicky's serial model has no DLAB, so it transmits the
    divisor as a character. Harmless, and not something to assert on.
    """
    for raw in iter(stream.readline, b""):
        q.put(raw.decode("utf-8", "replace").strip("\r\n"))
    q.put(None)


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    ap.add_argument("--clicky", required=True, help="path to the clicky-desktop binary")
    ap.add_argument("--fw", required=True, help="firmware-partition image (make_clicky_fw.py)")
    ap.add_argument("--golden", required=True, help="golden transcript file")
    ap.add_argument("--settle", type=float, default=3.0,
                    help="seconds to hold the emulator after the last golden line (default 3)")
    ap.add_argument("--timeout", type=float, default=120.0,
                    help="wall-clock cap on reaching the last golden line (default 120)")
    ap.add_argument("--stderr", default=None,
                    help="write the emulator's stderr (its MMIO log) here instead of inheriting")
    args = ap.parse_args()

    golden = load_golden(args.golden)

    # Quiet the emulator's own log: every stubbed register access is a WARN
    # or ERROR line on stderr and there are hundreds per boot. The UART
    # transcript is stdout, which is all we assert on.
    env = dict(os.environ, RUST_LOG=os.environ.get("RUST_LOG", "error"))
    err_fh = open(args.stderr, "wb") if args.stderr else None
    proc = subprocess.Popen(
        [args.clicky, "--hle", args.fw, "--hdd", "null:len=64MiB"],
        stdout=subprocess.PIPE,
        stderr=err_fh if err_fh else None,
        env=env,
        start_new_session=True,   # so the kill below takes the emulator's threads with it
    )
    q = queue.Queue()
    threading.Thread(target=pump, args=(proc.stdout, q), daemon=True).start()

    transcript = []          # every `core: ` line seen, in order
    matched = 0              # golden lines satisfied so far
    settled_at = None        # when the last golden line arrived
    failure = None
    t0 = time.monotonic()

    def fail(msg):
        nonlocal failure
        failure = msg

    while failure is None:
        now = time.monotonic()
        if settled_at is None:
            if now - t0 > args.timeout:
                fail(f"timed out after {args.timeout:.0f}s with {matched}/{len(golden)} "
                     f"golden lines matched")
                break
            wait = 0.5
        else:
            if now - settled_at >= args.settle:
                break                       # PASS: settled with nothing further
            wait = args.settle - (now - settled_at)
        try:
            line = q.get(timeout=wait)
        except queue.Empty:
            continue
        if line is None:
            # The emulator exited on its own. That is never success: on a
            # fatal memory exception clicky writes sysdump.log and returns.
            fail(f"emulator exited (rc={proc.wait()}) with {matched}/{len(golden)} "
                 f"golden lines matched")
            break
        # Everything the firmware writes to SER0 lands on stdout, but so
        # does the window backend's start-up chatter ("Failed to create
        # server-side surface decoration" under Wayland, for one). The
        # chatter all comes before the first UART byte, so: ignore lines
        # until the first `core: `, and treat EVERY line after it as UART.
        # That matters for the settle window — panic() writes
        # "*** PANIC: ..." with no `core: ` prefix, and a filter on the
        # prefix alone would wave the very failure this exists to catch
        # straight through.
        if not transcript and not line.startswith(PREFIX):
            continue
        if not line:
            continue                        # the blank line panic() leads with
        transcript.append(line)
        if settled_at is not None:
            fail(f"UART output after the last golden line: {line!r}")
            break
        expect_text, expect_re = golden[matched]
        if not expect_re.match(line):
            fail(f"line {matched + 1}: expected {expect_text!r}, got {line!r}")
            break
        matched += 1
        if matched == len(golden):
            settled_at = time.monotonic()

    # Tear down whatever is left. The emulator never exits on its own on a
    # good run (the idle task loops forever), so this is the normal path.
    try:
        os.killpg(proc.pid, signal.SIGKILL)
    except ProcessLookupError:
        pass
    proc.wait()
    if err_fh:
        err_fh.close()

    elapsed = time.monotonic() - t0
    if failure is None:
        print(f"OK: clicky boot smoke — {len(golden)} milestones in order, "
              f"quiet for {args.settle:.0f}s after the last, {elapsed:.1f}s total")
        return 0

    print(f"FAIL: clicky boot smoke — {failure}", file=sys.stderr)
    print("--- transcript (core: lines, in order) ---", file=sys.stderr)
    for line in transcript:
        print(line, file=sys.stderr)
    print("--- expected ---", file=sys.stderr)
    for text, _ in golden:
        print(text, file=sys.stderr)
    if args.stderr:
        print(f"--- emulator stderr: {args.stderr} (last lines) ---", file=sys.stderr)
        try:
            with open(args.stderr, encoding="utf-8", errors="replace") as f:
                for line in f.readlines()[-30:]:
                    print(line.rstrip(), file=sys.stderr)
        except OSError:
            pass
    return 1


if __name__ == "__main__":
    sys.exit(main())
