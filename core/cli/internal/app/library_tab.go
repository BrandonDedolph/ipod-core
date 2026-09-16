package app

import (
	"fmt"
	"image"
	"image/color"
	"path/filepath"
	"strings"
	"time"

	"gioui.org/layout"
	"gioui.org/op/clip"
	"gioui.org/op/paint"
	"gioui.org/unit"
	"gioui.org/widget"
	"gioui.org/widget/material"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/artfetch"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/librarian"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/organizer"
)

// The Library tab: the mockup's Fix screen.
//
// It is a report, not a wizard (plan §1 decision 12). The left column is the
// problem list — one row per category, with the count and a tick box — and the
// right pane is the preview: the exact before and after for the selected
// category, and the cover art candidates with their thumbnails. One button
// applies everything ticked, as one job with one journal, and Undo takes that
// journal back.
//
// Nothing here writes anything. Every button starts a job in actions.go; this
// file reads the model and draws it.

// previewRows is how many before/after rows the preview pane lists. The whole
// list would be thousands of lines on a library that has never been organized,
// and a table nobody can reach the bottom of is not a preview — the count on
// the row is the number that matters, and the CLI (`core fix --dry-run`)
// prints every line for anyone who wants them all.
const previewRows = 40

// libraryPane is the whole tab.
func (u *UI) libraryPane(gtx C) D {
	// Below this width the two columns stop being two columns: the
	// preview would be 180 px of truncated paths, which is worse than
	// scrolling one column.
	if gtx.Constraints.Max.X < gtx.Dp(620) {
		return u.fixList(gtx)
	}
	return layout.Flex{Alignment: layout.Start}.Layout(gtx,
		layout.Rigid(func(gtx C) D {
			gtx.Constraints.Max.X = gtx.Dp(300)
			gtx.Constraints.Min.X = gtx.Constraints.Max.X
			return u.fixList(gtx)
		}),
		layout.Rigid(layout.Spacer{Width: unit.Dp(14)}.Layout),
		layout.Flexed(1, u.fixPreview),
	)
}

// fixList is the left column: what is wrong, then the cost and the buttons.
//
// The problem rows scroll; the cost line and the buttons do not. At the
// minimum window the rows alone are taller than the column, and a Fix all
// that has to be scrolled to is a Fix all that reads as missing — and the
// cost line above it is the one sentence a person must see before pressing
// it (plan §1 decision 7).
func (u *UI) fixList(gtx C) D {
	busy := u.st.Busy()
	rep := u.st.Report
	return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
		layout.Flexed(1, func(gtx C) D {
			list := material.List(u.th, &u.libList)
			list.Indicator.Color = u.pal.SelSub
			return list.Layout(gtx, 1, func(gtx C, _ int) D {
				return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
					layout.Rigid(u.line(18, u.pal.Ink, u.fixHead())),
					layout.Rigid(u.line(11, u.pal.Muted, u.fixSub())),
					layout.Rigid(layout.Spacer{Height: unit.Dp(8)}.Layout),
					layout.Rigid(func(gtx C) D { return u.sourceRow(gtx, busy) }),
					layout.Rigid(layout.Spacer{Height: unit.Dp(10)}.Layout),
					layout.Rigid(func(gtx C) D {
						rows := make([]layout.FlexChild, 0, len(Categories)*2)
						for i, c := range Categories {
							cat := c
							if i > 0 {
								rows = append(rows, layout.Rigid(layout.Spacer{Height: unit.Dp(6)}.Layout))
							}
							rows = append(rows, layout.Rigid(func(gtx C) D {
								return u.problemRow(gtx, cat, cat.Count(rep))
							}))
						}
						return layout.Flex{Axis: layout.Vertical}.Layout(gtx, rows...)
					}),
				)
			})
		}),
		layout.Rigid(layout.Spacer{Height: unit.Dp(10)}.Layout),
		// The cost goes directly above the button that spends it. A rename
		// IS a re-copy — the filename is the device's locator — and on a
		// real library that is gigabytes over USB that nobody asked for.
		layout.Rigid(u.wrap(13, u.pal.Ink, u.costLine())),
		layout.Rigid(layout.Spacer{Height: unit.Dp(8)}.Layout),
		layout.Rigid(func(gtx C) D {
			return u.buttonRow(gtx,
				btn{&u.fixAllBtn, "Fix all", true, !busy && u.canFix()},
				btn{&u.undoBtn, "Undo", false, !busy && u.st.Journal != ""},
				btn{&u.rescanBtn, "Rescan", false, !busy && strings.TrimSpace(u.st.Source) != ""},
			)
		}),
	)
}

// fixHead is the mockup's "3 things to fix", counted the way a person counts:
// the categories that have anything in them, not the items.
func (u *UI) fixHead() string {
	rep := u.st.Report
	if rep == nil {
		if u.st.Job != nil && u.st.Job.Kind == JobInspect && u.st.Busy() {
			return "Reading the tags…"
		}
		return "Not scanned yet"
	}
	n := 0
	for _, c := range Categories {
		if c.Count(rep) > 0 {
			n++
		}
	}
	if n == 0 {
		return "Nothing to fix"
	}
	return fmt.Sprintf("%d thing%s to fix", n, plural(n))
}

func (u *UI) fixSub() string {
	if src := strings.TrimSpace(u.st.Source); src != "" {
		return "in " + src
	}
	return "choose the folder that holds your albums"
}

// canFix reports whether Fix all would do anything: a ticked category with
// something in it, or at least one accepted cover.
func (u *UI) canFix() bool {
	rep := u.st.Report
	if rep == nil {
		return false
	}
	if u.st.FixNames && len(rep.Misnamed) > 0 {
		return true
	}
	if u.st.Organize && len(FolderMoves(rep)) > 0 {
		return true
	}
	if u.st.DiscFolders && len(DiscMoves(rep)) > 0 {
		return true
	}
	for _, d := range u.st.ArtChoices {
		if !d.Skip {
			return true
		}
	}
	return false
}

// costLine is decision 7 on screen: a rename is a re-copy, because the
// filename IS the device's locator, and a 2 GB re-copy nobody expected reads
// as a bug. On the bench library this line says 87 tracks and 2.3 GB, which
// is exactly the number a person should see before pressing Fix all and not
// afterwards.
func (u *UI) costLine() string {
	rep := u.st.Report
	if rep == nil {
		return "A scan says what would change, and what it would cost to sync."
	}
	if rep.RecopyTracks == 0 && rep.NewTracks == 0 {
		return "Nothing here changes what the next sync copies."
	}
	line := fmt.Sprintf("%d track%s will be re-copied to the iPod on the next sync (%s)",
		rep.RecopyTracks, plural(rep.RecopyTracks), proseSize(rep.RecopyBytes))
	if rep.NewTracks > 0 {
		line += fmt.Sprintf(", and %d for the first time (%s)",
			rep.NewTracks, proseSize(rep.NewBytes))
	}
	return line + "."
}

// proseSize is a size inside a sentence: ONE number, in the unit a person
// reads a download in.
//
// disk.HumanSize is the right thing on a facts row, where both the decimal
// and the binary number are wanted and the row is a row. In the middle of a
// sentence it produces "306800000 B (292.6 MiB)" — a parenthesis inside a
// parenthesis, and a raw byte count nobody reads — so this line rounds once
// and says it once.
func proseSize(n int64) string {
	switch f := float64(n); {
	case n <= 0:
		return "0 B"
	case n < 1e6:
		return fmt.Sprintf("%.0f kB", f/1e3)
	case n < 1e9:
		return fmt.Sprintf("%.0f MB", f/1e6)
	default:
		return fmt.Sprintf("%.1f GB", f/1e9)
	}
}

// problemRow is one category: a tick box (where ticking means anything), the
// two lines of text, and the count. The whole row selects the category the
// preview shows.
func (u *UI) problemRow(gtx C, c Category, n int) D {
	sel := u.st.Category == c
	bg := u.pal.Plate
	if sel {
		bg = u.pal.Trk
	}
	return u.catClicks[c].Layout(gtx, func(gtx C) D {
		return widget.Border{Color: u.pal.Border, CornerRadius: unit.Dp(6), Width: unit.Dp(1)}.Layout(gtx,
			func(gtx C) D {
				return layout.Stack{}.Layout(gtx,
					layout.Expanded(func(gtx C) D {
						r := gtx.Dp(6)
						paint.FillShape(gtx.Ops, bg,
							clip.UniformRRect(image.Rectangle{Max: gtx.Constraints.Min}, r).Op(gtx.Ops))
						return D{Size: gtx.Constraints.Min}
					}),
					layout.Stacked(func(gtx C) D {
						gtx.Constraints.Min.X = gtx.Constraints.Max.X
						return layout.Inset{Top: unit.Dp(8), Bottom: unit.Dp(8),
							Left: unit.Dp(10), Right: unit.Dp(10)}.Layout(gtx, func(gtx C) D {
							return layout.Flex{Alignment: layout.Middle}.Layout(gtx,
								layout.Rigid(func(gtx C) D { return u.tickOrDot(gtx, c, n) }),
								layout.Rigid(layout.Spacer{Width: unit.Dp(8)}.Layout),
								layout.Flexed(1, func(gtx C) D {
									return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
										layout.Rigid(u.line(13, u.pal.Ink, c.Title())),
										layout.Rigid(u.line(11, u.pal.Muted, c.Sub())),
									)
								}),
								layout.Rigid(layout.Spacer{Width: unit.Dp(6)}.Layout),
								layout.Rigid(u.text(16, u.countColor(n), itoa(n))),
							)
						})
					}),
				)
			})
	})
}

// tickOrDot draws the tick box for the two categories Fix all can apply
// wholesale, and a plain dot for the two it cannot: cover art is decided one
// album at a time in the preview, and nothing on the attention list is ever
// fixed automatically.
func (u *UI) tickOrDot(gtx C, c Category, n int) D {
	var b *widget.Bool
	switch c {
	case CatNames:
		b = &u.namesTick
	case CatFolders:
		b = &u.organizeTick
	case CatDiscs:
		b = &u.discTick
	}
	if b == nil || n == 0 {
		size := gtx.Dp(14)
		col := u.pal.Trk
		if n > 0 {
			col = u.pal.Muted2
		}
		paint.FillShape(gtx.Ops, col, clip.Ellipse{Max: image.Pt(size, size)}.Op(gtx.Ops))
		return D{Size: image.Pt(size, size)}
	}
	cb := material.CheckBox(u.th, b, "")
	cb.Color = u.pal.Ink
	cb.IconColor = u.pal.Accent
	cb.Size = unit.Dp(18)
	return cb.Layout(gtx)
}

func (u *UI) countColor(n int) color.NRGBA {
	if n == 0 {
		return u.pal.SelSub
	}
	return u.pal.Ink
}

// fixPreview is the right pane: the selected category's before/after table,
// and the art candidates when there are any.
func (u *UI) fixPreview(gtx C) D {
	return u.plate(gtx, func(gtx C) D {
		return layout.UniformInset(unit.Dp(14)).Layout(gtx, func(gtx C) D {
			gtx.Constraints.Min.X = gtx.Constraints.Max.X
			list := material.List(u.th, &u.prevList)
			list.Indicator.Color = u.pal.SelSub
			rows := u.previewContent()
			return list.Layout(gtx, len(rows), func(gtx C, i int) D {
				return rows[i](gtx)
			})
		})
	})
}

// previewContent builds the pane's rows as a list of widgets, so the whole
// pane scrolls as one thing whatever is in it.
func (u *UI) previewContent() []layout.Widget {
	rep := u.st.Report
	var out []layout.Widget
	add := func(w layout.Widget) { out = append(out, w) }

	if rep == nil {
		add(u.line(14, u.pal.Ink, "Nothing has been scanned yet"))
		add(u.wrap(12, u.pal.Muted, "Choose your music folder and this pane lists every change "+
			"the app would make: the old name, the new name, and why."))
		return out
	}

	cat := u.st.Category
	add(u.line(14, u.pal.Ink, fmt.Sprintf("%s · %d", cat.Title(), cat.Count(rep))))
	add(func(gtx C) D { return layout.Spacer{Height: unit.Dp(6)}.Layout(gtx) })

	switch cat {
	case CatArt:
		add(u.wrap(11, u.pal.Muted, "From the iTunes catalogue, then MusicBrainz and the Cover Art "+
			"Archive. Nothing is written until Fix all, and only for the albums you take."))
		add(func(gtx C) D { return layout.Spacer{Height: unit.Dp(8)}.Layout(gtx) })
		if len(rep.MissingArt) == 0 {
			add(u.line(12, u.pal.Muted2, "Every album has a cover."))
			break
		}
		if len(u.st.ArtCands) == 0 {
			add(u.wrap(12, u.pal.Muted, "No lookup has been made yet — it is the one thing here that "+
				"goes to the internet, so it is a button."))
			add(func(gtx C) D { return layout.Spacer{Height: unit.Dp(8)}.Layout(gtx) })
			add(func(gtx C) D {
				return u.buttonRow(gtx, btn{&u.artFindBtn, "Look for cover art",
					true, !u.st.Busy()})
			})
		}
		u.ensureArtButtons(len(rep.MissingArt))
		for i := range rep.MissingArt {
			a := rep.MissingArt[i]
			idx := i
			add(func(gtx C) D { return u.artRow(gtx, a, idx) })
		}
	case CatAttention:
		add(u.wrap(11, u.pal.Muted, "These are never changed automatically: the tags do not say "+
			"enough to name the file from, and guessing is how a library gets quietly wrong."))
		add(func(gtx C) D { return layout.Spacer{Height: unit.Dp(8)}.Layout(gtx) })
		for i, at := range rep.NeedsAttention {
			if i >= previewRows {
				add(u.line(11, u.pal.Muted2, more(len(rep.NeedsAttention)-previewRows)))
				break
			}
			item := at
			add(func(gtx C) D { return u.attentionRow(gtx, rep.Root, item) })
		}
	default:
		moves := rep.Misnamed
		switch cat {
		case CatFolders:
			moves = FolderMoves(rep)
		case CatDiscs:
			moves = DiscMoves(rep)
		}
		if cat == CatDiscs {
			add(u.wrap(11, u.pal.Muted, "A flat folder whose tags say two discs becomes Disc 1 / "+
				"Disc 2. It is off unless you tick it: a flat album folder is a legitimate way "+
				"to keep a double record, and this changes every one of its filenames."))
			add(func(gtx C) D { return layout.Spacer{Height: unit.Dp(8)}.Layout(gtx) })
		}
		if len(moves) == 0 {
			add(u.line(12, u.pal.Muted2, "Nothing in this category."))
			break
		}
		for i := range moves {
			if i >= previewRows {
				add(u.line(11, u.pal.Muted2, more(len(moves)-previewRows)))
				break
			}
			m := moves[i]
			add(func(gtx C) D { return u.moveRow(gtx, rep.Root, m) })
		}
	}
	return out
}

func more(n int) string {
	return fmt.Sprintf("… and %d more (core fix --dry-run prints them all)", n)
}

// moveRow is one before → after line: the reason in small caps, the old name
// struck through by being muted, the new one in ink.
func (u *UI) moveRow(gtx C, root string, m organizer.Move) D {
	return layout.Inset{Bottom: unit.Dp(6)}.Layout(gtx, func(gtx C) D {
		gtx.Constraints.Min.X = gtx.Constraints.Max.X
		return layout.Flex{Alignment: layout.Start}.Layout(gtx,
			layout.Rigid(func(gtx C) D {
				gtx.Constraints.Min.X = gtx.Dp(52)
				l := material.Label(u.th, unit.Sp(9), strings.ToUpper(m.Reason))
				l.Color = u.pal.Muted2
				l.MaxLines = 1
				return l.Layout(gtx)
			}),
			layout.Flexed(1, func(gtx C) D {
				return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
					layout.Rigid(u.line(11, u.pal.Muted, relPath(root, m.From))),
					layout.Rigid(u.line(12, u.pal.Ink, "→  "+relPath(root, m.To))),
				)
			}),
		)
	})
}

func (u *UI) attentionRow(gtx C, root string, a organizer.Attention) D {
	return layout.Inset{Bottom: unit.Dp(6)}.Layout(gtx, func(gtx C) D {
		gtx.Constraints.Min.X = gtx.Constraints.Max.X
		return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
			layout.Rigid(u.line(12, u.pal.Ink, relPath(root, a.Path))),
			layout.Rigid(u.line(11, u.pal.Muted, a.Reason)),
		)
	})
}

// artRow is one coverless album: the candidate's thumbnail, what was found,
// and the two buttons that decide it.
func (u *UI) artRow(gtx C, a librarian.AlbumRef, idx int) D {
	cands := u.st.ArtCands[a.Key]
	dec, decided := u.st.ArtChoices[a.Key]
	busy := u.st.Busy()
	return layout.Inset{Bottom: unit.Dp(8)}.Layout(gtx, func(gtx C) D {
		gtx.Constraints.Min.X = gtx.Constraints.Max.X
		return layout.Flex{Alignment: layout.Middle}.Layout(gtx,
			layout.Rigid(func(gtx C) D { return u.candThumb(gtx, cands) }),
			layout.Rigid(layout.Spacer{Width: unit.Dp(10)}.Layout),
			layout.Flexed(1, func(gtx C) D {
				return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
					layout.Rigid(u.line(12, u.pal.Ink, a.Artist+" — "+a.Album)),
					layout.Rigid(u.line(11, u.pal.Muted, candLine(cands, dec, decided))),
				)
			}),
			layout.Rigid(layout.Spacer{Width: unit.Dp(8)}.Layout),
			layout.Rigid(func(gtx C) D {
				take := !decided || dec.Skip
				return u.buttonRow(gtx,
					btn{u.artTake[idx], "Take", take, !busy && len(cands) > 0 && take},
					btn{u.artSkip[idx], "Skip", false, !busy && (!decided || !dec.Skip)},
				)
			}),
		)
	})
}

// candLine is the sentence under the album name: what was found and what the
// user has decided about it.
func candLine(cands []artfetch.Candidate, dec librarian.Decision, decided bool) string {
	switch {
	case decided && dec.Skip:
		return "skipped — this album keeps no cover"
	case decided:
		return fmt.Sprintf("taking %.2f  %s  %s — %s", dec.Candidate.Score,
			dec.Candidate.Provider, dec.Candidate.Artist, dec.Candidate.Album)
	case len(cands) == 0:
		return "nothing found yet"
	default:
		c := cands[0]
		return fmt.Sprintf("%.2f  %s  %s — %s", c.Score, c.Provider, c.Artist, c.Album)
	}
}

// candThumb draws the candidate's 200 px thumbnail, fetched through the
// backend by the same cache the grid uses (keyed by URL rather than by
// folder), or an empty plate while there is nothing.
func (u *UI) candThumb(gtx C, cands []artfetch.Candidate) D {
	size := gtx.Dp(48)
	sz := image.Pt(size, size)
	defer clip.UniformRRect(image.Rectangle{Max: sz}, gtx.Dp(3)).Push(gtx.Ops).Pop()
	paint.FillShape(gtx.Ops, u.pal.Trk, clip.Rect{Max: sz}.Op())
	if len(cands) == 0 || cands[0].ThumbURL == "" {
		return D{Size: sz}
	}
	if img, ok := u.artThumbs.get(cands[0].ThumbURL, time.Time{}); ok {
		if b := img.Size(); b.X > 0 && b.Y > 0 {
			s := float32(size) / float32(b.X)
			if sy := float32(size) / float32(b.Y); sy > s {
				s = sy
			}
			stack := scaleOp(gtx, s)
			img.Add(gtx.Ops)
			paint.PaintOp{}.Add(gtx.Ops)
			stack.Pop()
		}
	}
	return D{Size: sz}
}

// ensureArtButtons keeps one pair of buttons per coverless album.
func (u *UI) ensureArtButtons(n int) {
	for len(u.artTake) < n {
		u.artTake = append(u.artTake, &widget.Clickable{})
		u.artSkip = append(u.artSkip, &widget.Clickable{})
	}
}

// --- events -------------------------------------------------------------

// libraryEvents turns this frame's clicks in the Library tab into model
// changes and jobs. Everything that writes goes through the Runner; the
// decisions themselves are model edits on the UI goroutine.
func (u *UI) libraryEvents(gtx C, busy bool) {
	for i, c := range Categories {
		if u.catClicks[i].Clicked(gtx) {
			u.st.Category = c
			u.st.Tab = TabLibrary
		}
	}
	if u.namesTick.Update(gtx) {
		u.st.FixNames = u.namesTick.Value
	}
	if u.organizeTick.Update(gtx) {
		u.st.Organize = u.organizeTick.Value
	}
	if u.discTick.Update(gtx) {
		// The split has to be IN the plan before it can be applied, and
		// only an Inspect with the option on puts it there (librarian's
		// Options.DiscFolders). So ticking the box rescans; the row says
		// so, and the scan is a read.
		u.st.DiscFolders = u.discTick.Value
		if !busy && strings.TrimSpace(u.st.Source) != "" {
			u.later(func() { u.startInspect(u.st.Source) })
		}
	}
	if busy {
		return
	}
	if u.rescanBtn.Clicked(gtx) {
		u.startInspect(strings.TrimSpace(u.st.Source))
	}
	if u.artFindBtn.Clicked(gtx) {
		u.startCandidates()
	}
	if u.fixAllBtn.Clicked(gtx) {
		u.startFix()
	}
	if u.undoBtn.Clicked(gtx) {
		u.startUndo(u.st.Journal)
	}
	if u.st.Report == nil {
		return
	}
	for i := range u.st.Report.MissingArt {
		if i >= len(u.artTake) {
			break
		}
		key := u.st.Report.MissingArt[i].Key
		if u.artTake[i].Clicked(gtx) {
			if cands := u.st.ArtCands[key]; len(cands) > 0 {
				u.setChoice(key, librarian.Accept(cands[0]))
			}
		}
		if u.artSkip[i].Clicked(gtx) {
			u.setChoice(key, librarian.Skip())
		}
	}
}

// setChoice records one album's art decision.
func (u *UI) setChoice(key librarian.AlbumKey, d librarian.Decision) {
	if u.st.ArtChoices == nil {
		u.st.ArtChoices = map[librarian.AlbumKey]librarian.Decision{}
	}
	u.st.ArtChoices[key] = d
}

// choices is what Fix is handed: the two tick boxes and every album decision
// that is not a skip. A skip is dropped rather than sent, because
// librarian.Fix counts an album with no entry as skipped anyway and a map full
// of skips is a map that says nothing.
func (u *UI) choices() librarian.Choices {
	ch := librarian.Choices{
		FixNames:    u.st.FixNames,
		Organize:    u.st.Organize,
		DiscFolders: u.st.DiscFolders,
	}
	for k, d := range u.st.ArtChoices {
		if d.Skip {
			continue
		}
		if ch.Art == nil {
			ch.Art = map[librarian.AlbumKey]librarian.Decision{}
		}
		ch.Art[k] = d
	}
	return ch
}

// fixSummary is the sentence the Fix all confirmation leads with: what is
// about to happen to the files, and what it will cost on the next sync.
func (u *UI) fixSummary() string {
	rep := u.st.Report
	if rep == nil {
		return ""
	}
	ch := u.choices()
	var parts []string
	if n := CatNames.Count(rep); ch.FixNames && n > 0 {
		parts = append(parts, fmt.Sprintf("%d track(s) renamed from their tags", n))
	}
	if n := CatFolders.Count(rep); ch.Organize && n > 0 {
		parts = append(parts, fmt.Sprintf("%d track(s) moved into \"Album - Artist\" folders"+
			" (their album's art and playlist follow them)", n))
	}
	if n := CatDiscs.Count(rep); n > 0 && ch.DiscFolders {
		parts = append(parts, fmt.Sprintf("%d track(s) split into Disc N folders", n))
	}
	if n := len(ch.Art); n > 0 {
		parts = append(parts, fmt.Sprintf("%d cover(s) downloaded and written into the FLACs", n))
	}
	if len(parts) == 0 {
		return "Nothing is ticked."
	}
	return strings.Join(parts, "\n") + "\n\n" + u.costLine()
}

// --- small helpers ------------------------------------------------------

// relPath is a path as the preview shows it: relative to the scanned root,
// with the platform's separator kept, and the absolute path when it is not
// under the root at all.
func relPath(root, p string) string {
	if root == "" {
		return p
	}
	if r, err := filepath.Rel(root, p); err == nil && !strings.HasPrefix(r, "..") {
		return r
	}
	return p
}

func plural(n int) string {
	if n == 1 {
		return ""
	}
	return "s"
}
