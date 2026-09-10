# 10 — Headphone jack: conductors, insertion detect, and inline buttons

This file exists because the question "can an inline headphone button ever
work on this hardware?" will be asked again. The short answers, each argued
below with its evidence:

| Question | Answer | Confidence |
|---|---|---|
| Can the firmware sense an inline headphone button (play/pause, volume) on the 5G/5.5G? | **No.** There is no electrical path from any jack conductor to anything the SoC or the PMU can read, other than the insertion switch. | High — from Apple's own documentation plus the codec datasheet; see "Why buttons cannot work". |
| Is the jack a plain 3-conductor TRS? | **No — it is 4-pole.** Tip/ring = L/R audio, plus ground, plus **composite video out**. The fourth conductor is an *output*, which is exactly why it is useless for buttons. | High — Apple's spec sheet. |
| Does the jack have an insertion (plug-present) switch the firmware can read? | **Yes.** Apple's own service diagnostics test it and Apple's firmware pauses on unplug. The public iPodLinux GPIO table puts it on **GPIO port A bit 7**. | Line: medium (public wiki, cross-validated). **Polarity: unconfirmed** — see "Confirming it on the device". |

Nothing in this file was derived from Rockbox source. The GPIO assignment
comes from the iPodLinux **wiki** (documentation, GFDL), the codec facts
from the Wolfson WM8758B production datasheet, and the jack facts from
Apple's published specifications and service procedures.

## The physical jack

Apple's specification sheet for the "Fifth Generation iPod (Late 2006)"
lists under Input and Output: *"3.5-mm stereo headphone minijack"*, *"Audio
and composite video output"*, and under connectivity: *"composite video
(with AV cable, sold separately) and audio through headphone jack"*. The
AV cable in question (iPod AV Cable, M9765G/A) is a 3.5 mm **four-conductor**
plug breaking out to three RCA connectors. So the jack on this device has
four contacts:

| Conductor | Signal | Direction |
|---|---|---|
| Tip | Left audio | out (WM8758B LOUT1) |
| Ring 1 | Right audio | out (WM8758B ROUT1) |
| Ring 2 / Sleeve | Ground | — |
| Sleeve / Ring 2 | Composite video | **out** (BCM video encoder) |

Which of ring 2 and sleeve carries video on Apple's cable was not
established here (period AV cables used both conventions); it does not
change any conclusion below, because both remaining conductors are an
output and a ground.

What the jack does **not** have: the four-pin remote connector that
surrounded the headphone socket on the 1G–4G iPods is gone on the 5G. The
5G-era remote — the iPod Radio Remote — attaches through the 30-pin dock
connector, and remote/accessory identification on this device is the
dock's pin-21 resistor ladder into the PCF50605 ADC
([08-boot-dock.md](08-boot-dock.md), "Accessory detection"). There is no
jack-side accessory ID.

Apple's own compatibility statement for the Apple Earphones with Remote
and Mic (2009) says the remote and microphone work only on iPod nano
(4th generation) and later, iPod classic (120 GB) and later, and iPod touch
(2nd generation) and later; *"audio is supported by all iPod models"*. The
5G/5.5G is deliberately absent from the remote/mic list. That is Apple
telling us the same thing the pinout does.

## Why buttons cannot work

Three candidate paths were considered and each is closed.

### 1. The headset's own button conductor

A CTIA/OMTP inline-remote headset carries its buttons on the **microphone**
conductor: the play/pause button shorts MIC to ground, and the volume
buttons connect MIC to ground through fixed resistors. The host reads them
by supplying a bias voltage on MIC and measuring the resulting voltage.
Three things are needed on the host side: a bias source on that conductor,
something that measures it (a comparator or an ADC), and a wire from the
jack to it.

In this jack the plug's MIC contact and ground contact land on the jack's
**video-out** conductor and its **ground** conductor (in one order or the
other, per the table above). A button press therefore connects the
composite-video output to ground — and nothing measures that. There is no
bias on the video conductor (it is an AC-coupled DAC output), nothing
samples it, and the codec's microphone bias / inputs are not on the jack
at all (see the codec section). Whatever the headset does with its
button, the electrical result on this board is "video-out momentarily
shorted while nobody is looking", which no register reflects.

### 2. A load / impedance change on the L and R outputs

Could a press be seen as a change in the load on the audio lines? No:

- In a CTIA/OMTP headset the button is *not* in series or parallel with
  either audio driver — it sits between MIC and ground, which are separate
  conductors. Pressing it does not change what the L/R drivers see.
- Even for a hypothetical headset that did switch the speaker load, the
  WM8758B headphone driver has no output-current or load sense that is
  readable, and the SoC has no ADC on those lines. The only ADC on the
  board is inside the PCF50605 PMU, and its channels are the battery and
  the dock's accessory-ID pin ([06-power.md](06-power.md),
  [08-boot-dock.md](08-boot-dock.md)).

### 3. The codec (WM8758B)

The audio codec is the one chip on the audio path with any "jack" facility,
so it was checked against its production datasheet (Wolfson WM8758B,
PD Rev 4.4, January 2012):

- **It does have a jack-detect feature**, on three dual-purpose pins:
  `CSB/GPIO1`, `L2/GPIO2` and `R2/GPIO3` ("Output switching (jack
  detect)", registers R8/R9/R13: `JD_EN`, `JD_SEL`, `JD_EN0..1`). It is an
  **autonomous output-switching** function: the pin, after an internal
  debounce, selects one of two output-enable patterns (e.g. mute the
  headphone amp and enable line-out when a plug is removed). It changes
  which amplifiers are on; it does not report anything.
- **The control interface is write-only.** In 2-wire mode (the mode this
  board uses — 09-i2c.md, device address `0x1A` = the datasheet's
  `0011010`) the datasheet states that if *"the R/W bit is '1' when
  operating in write only mode, the WM8758B returns to the idle
  condition"*. No register — including any jack-detect status — can be
  read back over the bus.
- **There is no interrupt or status output pin.** `GPIO1` can be
  configured as an *output* carrying "Temp ok", "Amute active", "PLL clk",
  "PLL lock", or a fixed logic level — not the jack state — and in any
  case nothing documents that pin reaching the SoC.
- The codec's microphone pre-amp inputs (`LIP/LIN`, `RIP/RIN`) and its
  ADC exist on the die, but recording on this device is a **dock**
  accessory story (line-in on the dock connector); nothing jack-side is
  wired to them. (Our init never touches the record path — 05-audio.md.)

So even in the strongest imaginable case — Apple wiring a jack conductor to
a codec jack-detect pin — the SoC could not learn of it, because the codec
cannot be asked.

### Verdict

**Inline headphone buttons cannot be supported on the iPod 5G/5.5G.** No
jack conductor reaches a bias source, a comparator, an ADC channel, or a
GPIO — other than the mechanical insertion switch described next. The only
thing that would overturn this is a board schematic showing such a wire;
none is publicly known, and Apple's own accessory compatibility list is
consistent with there being none. Play/pause-from-a-cable on this device
means a dock-connector remote speaking the Apple Accessory Protocol over
the dock UART (08-boot-dock.md, "Autobaud"), which is a different feature
with a different cost.

## The insertion switch (plug-present detect)

This part is real, and it is what "pause when the headphones are pulled"
needs.

### Evidence that it exists

- **Apple's service diagnostics for this model test it.** The *iPod 5th
  Generation Testing Procedures* (Apple, 19 October 2005) include a
  "HeadphoneDetect" test that displays *"Headphone Detect Present"* as 0
  or 1 alongside the Hold switch state, with the tester instructed to
  remove the headphones and check the value. Software can read it; so it
  is a logic input, not an analog or codec-internal signal.
- **Apple's firmware acts on it.** The 5G is the first iPod whose firmware
  pauses playback when the plug is removed; the iFixit Q&A for the iPod
  5th Generation (Video) treats *"when the headphone is removed, the
  device stops playing music"* as the normal behaviour and a device that
  keeps playing as a broken jack-flex.

### Which GPIO

The iPodLinux wiki's GPIO page (public documentation of the platform,
`ipodlinux.org/GPIO`, mirrored) gives, for the G5 column:

| Port A bit | G5 entry |
|---|---|
| 4 | Dock attached (input) |
| 5 | Hold (neg input) |
| 7 | **Headphone attached (input)** |

and for port L: bit 2 "USB charging disable (output)", bit 3 "Ext. power
attached (neg input)", bit 4 "USB attached (input)", bit 7 "Backlight
(output)".

That port-L row is the cross-check. It matches what this firmware already
runs on the device, polarity included: L3 main-charger present is
active-low and L4 USB present is active-high ([06-power.md](06-power.md)),
L7 is the backlight LED enable ([02-lcd.md](02-lcd.md)), and A5 hold is
active-low ([03-clickwheel.md](03-clickwheel.md)). The table marks
active-low inputs "(neg input)" and leaves active-high ones as "(input)",
and it gets every one of our four known pins right under that convention.

So the candidate is `GPIOA_INPUT_VAL` (`0x6000D030`) **bit 7** (`0x80`),
and by the table's convention the bit reads **1 = plug seated**. The line
is well supported; the polarity is an inference from a notation
convention and is **not confirmed on this device** until the probe below
has run. A wrong polarity would pause playback every time headphones are
plugged *in*, which is why the driver ships with the feature gated off
(`HEADPHONE_DETECT_TRUSTED` = 0 in `core/hal/hw/headphone.h`) until the
transcript has been seen.

### Confirming it on the device (the probe)

`core/hal/hw/headphone.c` carries a compiled-out probe. Built with
`-DHEADPHONE_PROBE=1` it dumps every GPIO input port to the SER0 UART
whenever any bit changes — all twelve ports in one line, so a single
session identifies the pin without guessing which port to watch.

Build:

```
meson configure build-hw -Dc_args=-DHEADPHONE_PROBE=1
ninja -C build-hw
```

(Set `-Dc_args=` back to empty afterwards; the probe must never ship.)

Run: boot with the UART attached and **nothing plugged into the jack**,
wait for the `core: hpprobe cfg` block, then with the device otherwise
untouched (hold switch left alone, no charger, no wheel):

1. plug headphones in, wait two seconds,
2. pull them out, wait two seconds,
3. repeat 1–2 twice more.

Transcript shape:

```
core: hpprobe cfg A en=xx oe=xx ov=xx in=xx
...                                  (one line per port A..L, once)
core: hpprobe t=<us> A=xx B=xx C=xx D=xx E=xx F=xx G=xx H=xx I=xx J=xx K=xx L=xx
core: hpprobe diff A bit7 0->1
```

Every change to any input port prints one `t=` snapshot followed by one
`diff` line per changed bit. The expected result is exactly six `diff`
lines over the session, all naming the same port/bit, alternating
`0->1` on insert and `1->0` on remove (which confirms active-high) or the
reverse (active-low — set `HEADPHONE_DETECT_ACTIVE_LOW` to 1). If a
*different* bit flips in lockstep with the plug, that bit is the line;
update `HEADPHONE_DETECT_ADDR`/`HEADPHONE_DETECT_BIT`. If a bit flaps on
its own with the plug untouched, it is not the jack.

If nothing flips at all: the `cfg` block shows whether A7 was configured
as a GPIO input by the boot ROM (`en` bit 7 set, `oe` bit 7 clear). The
probe forces A7 into that configuration itself if it was not, and logs
that it did; if that still produces nothing, the pin is on a port whose
enable the ROM left clear, and the follow-up is to enable the remaining
non-output pins one port at a time. That second phase has not been needed
yet and is not implemented.

The probe cannot spam: it prints only on change, samples at most every
20 ms, and goes silent after 400 change events (a pin oscillating on its
own would otherwise fill the console). It compiles to nothing unless
`HEADPHONE_PROBE` is set.

### GPIO bank layout used by the probe

The PP502x GPIO block is three quads of four 8-bit ports. The A–D quad at
`0x6000D000` and the I–L quad at `0x6000D100` are established by drivers
running on this device (02-lcd.md, 06-power.md); the E–H quad at
`0x6000D080` is the arithmetic midpoint and, having no consumer, is
unconfirmed. Within a quad the per-port stride is `0x04` (A/E/I = +0x00 …
D/H/L = +0x0C) and the register groups are:

| Group | Offset | Meaning |
|---|---|---|
| ENABLE | +0x00 | 1 = pin is a GPIO (not its alternate function) |
| OUTPUT_EN | +0x10 | 1 = driven as an output |
| OUTPUT_VAL | +0x20 | value driven when an output |
| INPUT_VAL | +0x30 | live pin level |

Hence `GPIOA_INPUT_VAL` = `0x6000D030`, `GPIOB_INPUT_VAL` = `0x6000D034`,
… `GPIOL_INPUT_VAL` = `0x6000D13C`. Each register has a masked-write shadow
`+0x800` higher (02-lcd.md, "Atomic bit set/clear alias").

## The driver

`hal_headphones_present()` (hal.h) returns the **debounced** plug state:
1 seated, 0 absent, or -1 while the line is untrusted (the default until
the transcript above has been seen) — a caller must treat -1 as "do
nothing", never as "unplugged".

Debounce: a new raw level must hold continuously for **200 ms** before it
is reported; any return to the old level restarts the wait. 200 ms is the
value this firmware already uses for the USB cable-insert switch
([07-usb.md](07-usb.md)) and is an order of magnitude above the
few-millisecond contact bounce of a jack switch; the extra latency is
paid only on unplug (pause lands 200 ms late, which nobody hears) and
costs nothing on insert, because the recommended policy never
auto-resumes. A spurious pause mid-song is the failure to avoid, and a
long window is the cheapest defence against a plug being wiggled in a
pocket.

The first sample primes the state without waiting, so a device booted with
headphones in reads "present" immediately and never reports a phantom
unplug at start-up.

## Sources

| Fact | Source |
|---|---|
| 4-pole jack, composite video through the headphone jack | Apple, "Fifth Generation iPod (Late 2006) — Technical Specifications" (support.apple.com/112454) |
| Remote/mic earphones unsupported on the 5G | Apple, "Apple Earphones with Remote and Mic" compatibility statement (2009) |
| Radio Remote is a dock-connector accessory | Apple iPod Radio Remote (A1187) product documentation |
| HeadphoneDetect diagnostic test | Apple, "iPod 5th Generation Testing Procedures", 19 Oct 2005 |
| 5G firmware pauses on unplug | iFixit Answers 488250, iPod 5th Generation (Video) |
| Codec jack-detect, write-only bus, pin list | Wolfson WM8758B datasheet, PD Rev 4.4, Jan 2012: "General purpose input/output", "Output switching (jack detect)", "2-wire serial control mode" |
| GPIO A7 "Headphone attached (input)" | iPodLinux wiki, "GPIO" page (mirror: seshan.xyz/flow/files/ipodlinux/GPIO.html) |
