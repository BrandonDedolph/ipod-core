package app

import (
	"image/color"

	"gioui.org/font"
	"gioui.org/font/gofont"
	"gioui.org/text"
	"gioui.org/unit"
	"gioui.org/widget/material"
)

// Palette is Linen, the theme the firmware itself draws in, taken from
// docs/screens/render.py and widened from the device's RGB565 to
// 24-bit. Using the device's own colours is not decoration: the app and
// the iPod are the same product, and a host tool in Material blue would
// look like somebody else's.
//
// Accent is the one saturated colour and it is rationed exactly as it
// is on the device — the progress bar and the single primary button on
// a card, nothing else. A screen where four things are orange is a
// screen where nothing is.
type Palette struct {
	Surface color.NRGBA // page ground
	Ink     color.NRGBA // primary text
	Muted   color.NRGBA // secondary text
	Muted2  color.NRGBA // tertiary text, hints
	MutedD  color.NRGBA // strong secondary, labels on plates
	Accent  color.NRGBA // progress + the primary button
	Border  color.NRGBA // hairlines
	Plate   color.NRGBA // card ground
	Trk     color.NRGBA // progress track, dividers
	SelSub  color.NRGBA // disabled text
	SelTrk  color.NRGBA // log pane text
	PillOff color.NRGBA // disabled button ground
}

// Linen is the palette.
func Linen() Palette {
	return Palette{
		Surface: rgb(0xF7F3EF),
		Ink:     rgb(0x191410),
		Muted:   rgb(0x7B716B),
		Muted2:  rgb(0x9C8E84),
		MutedD:  rgb(0x5A514A),
		Accent:  rgb(0xC56942),
		Border:  rgb(0xE6E3DE),
		Plate:   rgb(0xF7F7F7),
		Trk:     rgb(0xDEDBD6),
		SelSub:  rgb(0xB5B2AD),
		SelTrk:  rgb(0x423D3A),
		PillOff: rgb(0xCECAC5),
	}
}

func rgb(v uint32) color.NRGBA {
	return color.NRGBA{R: uint8(v >> 16), G: uint8(v >> 8), B: uint8(v), A: 0xFF}
}

// Mono is the log pane's typeface. The log carries device paths, byte
// counts and checksums, and a proportional font turns a column of those
// into a ragged wall.
var Mono = font.Font{Typeface: "Go Mono"}

// newTheme builds the Gio theme. The Go fonts ship inside the binary
// (golang.org/x/image/font/gofont), so the app has no font dependency
// on the machine it runs on — which matters on a fresh Windows install,
// where "download one exe and run it" is the whole promise.
func newTheme() (*material.Theme, Palette) {
	pal := Linen()
	th := material.NewTheme()
	th.Shaper = text.NewShaper(text.WithCollection(gofont.Collection()))
	th.TextSize = unit.Sp(14)
	th.Palette = material.Palette{
		Fg:         pal.Ink,
		Bg:         pal.Surface,
		ContrastBg: pal.Accent,
		ContrastFg: pal.Surface,
	}
	return th, pal
}
