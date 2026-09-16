package app

import (
	"fmt"
	"strings"

	"gioui.org/layout"
	"gioui.org/unit"
	"gioui.org/widget/material"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/disk"
)

// The Details tab: everything the four cards used to carry that is not a
// decision — the device's facts, the firmware, the backup, the music folder
// and the log.
//
// It exists for the day something goes wrong (the mockup's recommendation:
// "everything technical stays out of sight; a Details view keeps the log and
// the checks"). Nothing here is needed to sync an iPod, and nothing that IS
// needed lives only here: the music folder is also in the Library tab's
// header, and the one primary action is in the top bar.

// detailsPane is the whole tab: facts above, log below.
func (u *UI) detailsPane(gtx C) D {
	return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
		layout.Flexed(1, u.factsPane),
		layout.Rigid(layout.Spacer{Height: unit.Dp(10)}.Layout),
		layout.Flexed(0.9, u.logPane),
	)
}

// factsPane is the scrolling half: three sections, each a plate.
func (u *UI) factsPane(gtx C) D {
	list := material.List(u.th, &u.cards)
	list.Indicator.Color = u.pal.SelSub
	list.Indicator.HoverColor = u.pal.Muted2
	return list.Layout(gtx, 1, func(gtx C, _ int) D {
		return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
			layout.Rigid(u.deviceFacts),
			layout.Rigid(layout.Spacer{Height: unit.Dp(8)}.Layout),
			layout.Rigid(u.firmwareFacts),
			layout.Rigid(layout.Spacer{Height: unit.Dp(8)}.Layout),
			layout.Rigid(u.musicFacts),
		)
	})
}

// deviceFacts is the old Device card: what is attached, what it reports about
// itself, and what is on its music volume.
func (u *UI) deviceFacts(gtx C) D {
	d, lib := u.st.Device, u.st.Library
	return u.card(gtx, "THIS IPOD", func(gtx C) D {
		var rows []layout.FlexChild
		add := func(w layout.Widget) { rows = append(rows, layout.Rigid(w)) }

		if !d.Found {
			add(u.line(14, u.pal.Ink, "No iPod found"))
			add(u.wrap(11, u.pal.Muted, "Put it in disk mode — hold Select+Menu to reset, then "+
				"Select+Play at the Apple logo — and plug it into a port that carries data."))
			if d.Err != "" {
				add(u.line(11, u.pal.Muted2, d.Err))
			}
			if d.ElevationAdviceNeeded {
				add(u.wrap(11, u.pal.Accent, "Reading a raw disk needs Administrator: close this "+
					"and start it with Run as administrator."))
			}
			return layout.Flex{Axis: layout.Vertical}.Layout(gtx, rows...)
		}

		add(u.nameRow)

		// The model string goes LAST: the path, the drive letter and the
		// serial are the three facts that tell two iPods apart, and this
		// line is one line.
		where := d.Path
		if d.Volume != "" {
			where += "  ·  " + d.Volume
		}
		if d.Serial != "" {
			where += "  ·  " + d.Serial
		}
		if d.Model != "" {
			where += "  ·  " + d.Model
		}
		add(u.line(11, u.pal.Muted, where))

		gate, gateCol := "tested hardware", u.pal.MutedD
		if !d.Tested {
			gate, gateCol = "UNTESTED hardware", u.pal.Accent
		}
		add(u.line(11, gateCol, gate+"  ·  sector "+itoa(d.SectorSize)+" B  ·  OSOS "+okWord(d.OSOSOK)))
		if d.OSOSNote != "" {
			add(u.line(11, u.pal.Muted2, d.OSOSNote))
		}

		line := "library  not read yet"
		switch {
		case lib.Present:
			line = fmt.Sprintf("library  %d songs · %d albums · index %s B",
				lib.Songs, lib.Albums, comma(lib.IndexBytes))
		case lib.Note != "":
			line = "library  " + lib.Note
		}
		add(u.line(11, u.pal.MutedD, line))
		if lib.Present {
			add(u.line(11, u.pal.Muted, "CORECFG.DAT "+okWord(lib.ConfigValid)+
				"  ·  CORELOG.BIN "+okWord(lib.LogValid)+"  ·  "+itoa(lib.Genres)+" genres"))
		}

		rows = append(rows,
			layout.Rigid(layout.Spacer{Height: unit.Dp(8)}.Layout),
			layout.Rigid(func(gtx C) D {
				return u.buttonRow(gtx,
					btn{&u.cancelBtn, u.cancelLabel(), false, u.st.CanCancel()},
				)
			}),
		)
		return layout.Flex{Axis: layout.Vertical}.Layout(gtx, rows...)
	})
}

// firmwareFacts is the old Firmware card: what is installed, what the latest
// release is, and the three writes (update, flash a file, back the partition
// up). The Update button is here as well as in the top bar because this is
// where a person who wants a specific image comes.
func (u *UI) firmwareFacts(gtx C) D {
	busy := u.st.Busy()
	installed := u.st.Device.Firmware
	if installed == "" {
		installed = "unknown"
	}
	latest := "—  press Check"
	if u.st.Release.Checked {
		latest = u.st.Release.Tag
		if u.st.Release.Err != "" {
			latest = "could not check: " + firstLine(u.st.Release.Err)
		}
	}
	return u.card(gtx, "FIRMWARE", func(gtx C) D {
		return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
			layout.Rigid(u.line(12, u.pal.Ink, "installed  "+installed)),
			layout.Rigid(u.line(12, u.pal.MutedD, "latest     "+latest)),
			layout.Rigid(layout.Spacer{Height: unit.Dp(6)}.Layout),
			layout.Rigid(func(gtx C) D {
				return layout.Flex{Alignment: layout.Middle}.Layout(gtx,
					layout.Flexed(1, func(gtx C) D {
						return u.field(gtx, &u.flashEd, "path to core.ipod or core.bin")
					}),
					layout.Rigid(layout.Spacer{Width: unit.Dp(6)}.Layout),
					layout.Rigid(func(gtx C) D {
						return u.button(gtx, btn{&u.flashOpenBtn, "Flash file…", false, !busy && u.pickerOK})
					}),
				)
			}),
			layout.Rigid(layout.Spacer{Height: unit.Dp(6)}.Layout),
			layout.Rigid(u.wrap(11, u.pal.Muted2, u.flashNote())),
			layout.Rigid(layout.Spacer{Height: unit.Dp(8)}.Layout),
			layout.Rigid(func(gtx C) D {
				return u.buttonRow(gtx,
					btn{&u.checkBtn, "Check", false, !busy},
					btn{&u.updateBtn, "Update", false, !busy && u.st.Release.Checked && u.st.Release.Tag != ""},
					btn{&u.flashBtn, "Flash", false, !busy && strings.TrimSpace(u.flashEd.Text()) != ""},
					btn{&u.backupBtn, "Backup", false, !busy && u.st.Device.Found},
				)
			}),
		)
	})
}

// musicFacts is the music folder and what the last plan made of it. The field
// is here as well as in the Library tab because this is the tab a person opens
// when something is not where they expect it.
func (u *UI) musicFacts(gtx C) D {
	busy := u.st.Busy()
	return u.card(gtx, "MUSIC FOLDER", func(gtx C) D {
		return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
			layout.Rigid(u.line(11, u.pal.Muted, `Source folders named "Album - Artist", each holding .flac`)),
			layout.Rigid(layout.Spacer{Height: unit.Dp(6)}.Layout),
			layout.Rigid(func(gtx C) D { return u.sourceRow(gtx, busy) }),
			layout.Rigid(layout.Spacer{Height: unit.Dp(6)}.Layout),
			layout.Rigid(u.line(11, u.pal.Muted2, u.destLine())),
			layout.Rigid(layout.Spacer{Height: unit.Dp(8)}.Layout),
			layout.Rigid(func(gtx C) D {
				ready := !busy && u.st.Device.Volume != ""
				return u.buttonRow(gtx,
					btn{&u.dryRunBtn, "Dry run", false, ready},
					btn{&u.pruneBtn, "Sync + prune", false, ready},
					btn{&u.ejectBtn, "Eject", false, !busy && u.st.Device.Volume != ""},
				)
			}),
		)
	})
}

// sourceRow is the music folder field and its Browse button. One widget, two
// places (the Library tab's header and this tab), one Editor: two fields
// holding the same path would disagree the moment somebody typed in one.
func (u *UI) sourceRow(gtx C, busy bool) D {
	return layout.Flex{Alignment: layout.Middle}.Layout(gtx,
		layout.Flexed(1, func(gtx C) D {
			return u.field(gtx, &u.sourceEd, `C:\Users\you\Music`)
		}),
		layout.Rigid(layout.Spacer{Width: unit.Dp(6)}.Layout),
		layout.Rigid(func(gtx C) D {
			return u.button(gtx, btn{&u.browseBtn, "Browse…", false, !busy && u.pickerOK})
		}),
	)
}

func (u *UI) destLine() string {
	if !u.pickerOK {
		return "No folder picker here (zenity/kdialog) — type the path"
	}
	if u.st.Device.Volume == "" {
		return "Destination: no iPod volume yet"
	}
	return "Destination: " + u.st.Device.Volume + "  Music/ · CORELIB.IDX · CORECFG.DAT"
}

// footerLine is the window's bottom strip, left half: what is on the iPod.
func (u *UI) footerLine() string {
	lib := u.st.Library
	if lib.Present {
		return fmt.Sprintf("%d albums · %d songs · %d genres on the iPod",
			lib.Albums, lib.Songs, lib.Genres)
	}
	if n := len(u.st.Albums); n > 0 {
		return fmt.Sprintf("%d albums in your music folder", n)
	}
	return "no library read yet"
}

// footerSource is the bottom strip's right half: the music folder, which the
// mockup keeps in the footer with a Change beside it.
func (u *UI) footerSource() string {
	if src := strings.TrimSpace(u.st.Source); src != "" {
		return "Music folder: " + src
	}
	return "No music folder chosen"
}

// sizeLine is the top bar's third fact: how full the iPod is. It is the
// device's whole size until something reads the volume's free space, which
// nothing here does yet — so it says what it knows and not what it guesses.
func (u *UI) sizeLine() string {
	if u.st.Device.Size <= 0 {
		return ""
	}
	return disk.HumanSize(u.st.Device.Size)
}
