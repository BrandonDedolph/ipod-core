package app

import (
	"bytes"
	"encoding/binary"
	"errors"
	"fmt"
	"hash/fnv"
	"image"
	"image/color"
	"os"
	"path/filepath"
	"sort"
	"strings"
	"sync"
	"time"

	"gioui.org/f32"
	"gioui.org/layout"
	"gioui.org/op"
	"gioui.org/op/clip"
	"gioui.org/op/paint"
	"gioui.org/unit"
	"gioui.org/widget"
	"gioui.org/widget/material"
	xdraw "golang.org/x/image/draw"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/coreart"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flac"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/syncer"
)

// The album grid: the body of the window (direction B).
//
// Three things live here and they are deliberately separate. AlbumsFromPlan
// is pure — a syncer plan in, tiles out — so "which albums are NEW" is a
// table test and not something only a screenshot can answer. thumbCache is
// the only concurrency in the drawing half: a tile asks for a picture, gets
// a placeholder now and the picture on a later frame, and the decode happens
// on a worker rather than on the goroutine that has to produce 60 frames a
// second. And AlbumThumbnail is the decode itself, which is the device's own
// sidecar when there is one and the embedded cover when there is not.

// Tile geometry. The mockup's grid is eight columns in a 900 px window; the
// columns are computed from the width so a wider window gets more of them and
// the minimum window still gets four.
const (
	tileMin = 96
	tileMax = 120
	tileGap = 12
)

// AlbumsFromPlan turns one dry-run plan into the grid.
//
// The plan is the only source: it already lists every source file, which album
// it belongs to and whether the iPod has it, so a second scan would be a second
// answer to the same question.
//
// The state rule is the plan's own arithmetic. No copies at all means the whole
// album is on the device. Every track a copy means the device has none of it —
// which is what NEW means to a person, and it is true whether the folder is
// missing or was emptied by hand. Anything in between is CHANGED.
func AlbumsFromPlan(plan *syncer.Plan) []Album {
	if plan == nil {
		return nil
	}
	byName := map[string]*Album{}
	at := func(op syncer.FileOp) *Album {
		a := byName[op.Album]
		if a == nil {
			a = &Album{Device: op.Album, Title: op.Album, Dir: filepath.Dir(op.Src)}
			byName[op.Album] = a
		}
		if a.Dir == "" || a.Dir == "." {
			a.Dir = filepath.Dir(op.Src)
		}
		if op.ModTime.After(a.Mod) {
			a.Mod = op.ModTime
		}
		return a
	}
	for _, op := range plan.Copy {
		a := at(op)
		a.Tracks++
		a.Copy++
		a.Bytes += op.Size
	}
	for _, op := range plan.Skip {
		at(op).Tracks++
	}
	for _, op := range plan.Rename {
		// A rename is a file the device already holds under another
		// spelling: no bytes cross the cable, so it is not a copy, but
		// the album is not untouched either.
		a := at(op)
		a.Tracks++
	}
	// The art ops know the album's source folder even for an album whose
	// every file was skipped in a plan that did not carry paths.
	for _, op := range plan.Art {
		if a := byName[op.Album]; a != nil && op.SrcFLAC != "" && (a.Dir == "" || a.Dir == ".") {
			a.Dir = filepath.Dir(op.SrcFLAC)
		}
	}

	order := plan.AlbumOrder
	if len(order) == 0 {
		for name := range byName {
			order = append(order, name)
		}
		sort.Strings(order)
	}
	out := make([]Album, 0, len(byName))
	seen := map[string]bool{}
	for _, name := range order {
		a := byName[name]
		if a == nil || seen[name] {
			continue
		}
		seen[name] = true
		switch {
		case a.Copy == 0:
			a.State = TileOnDevice
		case a.Copy == a.Tracks:
			a.State = TileNew
		default:
			a.State = TileChanged
		}
		out = append(out, *a)
	}
	return out
}

// --- the thumbnail cache -----------------------------------------------

// thumbState is where one album's picture has got to.
type thumbState int

const (
	thumbPending thumbState = iota
	thumbReady
	thumbFailed
)

// thumbItem is one album's entry.
type thumbItem struct {
	mod   time.Time
	state thumbState
	img   image.Image
	// op is built once, on the UI goroutine, the first time the tile is
	// drawn. Rebuilding an ImageOp every frame re-uploads the texture.
	op    paint.ImageOp
	hasOp bool
}

// thumbCache keeps one decoded cover per album folder, keyed by the folder AND
// its modification time: a Fix that embeds a cover changes the folder's mtime,
// and a grid that went on showing the placeholder until the app restarted would
// make the fix look like it had not worked.
//
// Decoding happens on a small pool of workers. 101 JPEG decodes on the UI
// goroutine is a window that stops repainting for several seconds; a bounded
// pool is also what stops a thousand-album library opening a thousand files at
// once.
type thumbCache struct {
	load       func(dir string) (image.Image, error)
	invalidate func()

	mu    sync.Mutex
	items map[string]*thumbItem
	sem   chan struct{}
	loads int
}

// thumbWorkers is how many covers are decoded at once.
const thumbWorkers = 4

func newThumbCache(load func(string) (image.Image, error), invalidate func()) *thumbCache {
	if invalidate == nil {
		invalidate = func() {}
	}
	return &thumbCache{
		load:       load,
		invalidate: invalidate,
		items:      map[string]*thumbItem{},
		sem:        make(chan struct{}, thumbWorkers),
	}
}

// get returns the cover for one album if it has been decoded, and starts a
// decode when there is nothing yet (or when the folder has changed since the
// last one). It never blocks: the UI goroutine calls it inside a layout.
//
// Everything about an entry is read and written under the mutex, the ImageOp
// included — the entry is filled in by a worker goroutine and read by the UI
// goroutine, and memoising the op is the one write the drawing side makes.
func (c *thumbCache) get(dir string, mod time.Time) (paint.ImageOp, bool) {
	if c == nil || c.load == nil || dir == "" {
		return paint.ImageOp{}, false
	}
	c.mu.Lock()
	it := c.items[dir]
	if it != nil && it.mod.Equal(mod) {
		defer c.mu.Unlock()
		if it.state != thumbReady || it.img == nil {
			return paint.ImageOp{}, false
		}
		if !it.hasOp {
			it.op, it.hasOp = paint.NewImageOp(it.img), true
		}
		return it.op, true
	}
	// Either nothing is known, or what is known describes an older
	// version of this folder.
	it = &thumbItem{mod: mod, state: thumbPending}
	c.items[dir] = it
	c.loads++
	c.mu.Unlock()

	go func() {
		c.sem <- struct{}{}
		defer func() { <-c.sem }()
		img, err := c.load(dir)
		c.mu.Lock()
		// Only publish if this is still the entry the grid is waiting
		// for: a folder that changed twice while one decode was running
		// must not come back as the older picture.
		if cur := c.items[dir]; cur == it {
			if err != nil || img == nil {
				it.state = thumbFailed
			} else {
				it.img, it.state = img, thumbReady
			}
		}
		c.mu.Unlock()
		c.invalidate()
	}()
	return paint.ImageOp{}, false
}

// image returns the decoded cover for dir, if there is one.
func (c *thumbCache) image(dir string) (image.Image, bool) {
	c.mu.Lock()
	defer c.mu.Unlock()
	it := c.items[dir]
	if it == nil || it.state != thumbReady {
		return nil, false
	}
	return it.img, true
}

// Loads is how many decodes have been started. It is the cache's whole
// observable behaviour, and it is what the hit/miss test asserts on.
func (c *thumbCache) Loads() int {
	c.mu.Lock()
	defer c.mu.Unlock()
	return c.loads
}

// --- decoding an album's cover ------------------------------------------

// ThumbSize is the size covers are decoded down to for the grid. It is the
// device's own folder.art size, which means the sidecar is used as it is.
const ThumbSize = coreart.ArtSize

// ErrNoThumb reports that an album folder has no picture anywhere: no
// sidecar, no FLAC, no embedded cover. It is an ordinary outcome — the tile
// falls back to the coloured plate with the album's initial — so it is a
// sentinel rather than a message.
var ErrNoThumb = errors.New("app: this album has no cover picture")

// AlbumThumbnail decodes one album folder's cover for the grid.
//
// The device's own sidecar comes first: folder.art is already a 120x120
// bitmap of exactly this picture, it costs a 28 KB read and no JPEG decode,
// and using it means the grid shows what the iPod shows. Failing that, the
// album's art source (the first FLAC, which is the file the sidecars and the
// index read their cover from) is decoded and scaled down.
func AlbumThumbnail(dir string) (image.Image, error) {
	if strings.TrimSpace(dir) == "" {
		return nil, ErrNoThumb
	}
	for _, name := range []string{coreart.ArtName, coreart.ThumbName} {
		if b, err := os.ReadFile(filepath.Join(dir, name)); err == nil {
			if img, err := DecodeCART(b); err == nil {
				return img, nil
			}
		}
	}
	first, err := coreart.FirstFLAC(dir)
	if err != nil || first == "" {
		return nil, ErrNoThumb
	}
	m, err := flac.ReadFile(first)
	if err != nil {
		return nil, err
	}
	pic := m.FrontCover()
	if pic == nil {
		return nil, ErrNoThumb
	}
	img, err := coreart.FromPicture(pic)
	if err != nil {
		return nil, err
	}
	return Downscale(img, ThumbSize), nil
}

// DecodeCART reads a CoreArt sidecar (folder.art / folder.thm) back into an
// image. The container is the one core/ui/artcache.c validates: "CART", a
// version, the two dimensions, then RGB565 pixels little-endian, row-major.
//
// The five-bit channels are expanded by replicating the high bits into the
// low ones (v<<3 | v>>2), which is what the panel does and what makes a
// round-trip through the sidecar look like the original rather than like a
// picture that lost its whites.
func DecodeCART(b []byte) (image.Image, error) {
	if len(b) < coreart.HeaderLen || !bytes.Equal(b[:4], coreart.Magic[:]) {
		return nil, fmt.Errorf("app: not a CoreArt sidecar")
	}
	ver := binary.LittleEndian.Uint16(b[4:6])
	w := int(binary.LittleEndian.Uint16(b[6:8]))
	h := int(binary.LittleEndian.Uint16(b[8:10]))
	if ver != coreart.Version || w <= 0 || h <= 0 || w > coreart.MaxDim || h > coreart.MaxDim {
		return nil, fmt.Errorf("app: CoreArt header says version %d, %dx%d", ver, w, h)
	}
	if len(b) < coreart.HeaderLen+2*w*h {
		return nil, fmt.Errorf("app: CoreArt sidecar is %d bytes, %dx%d needs %d",
			len(b), w, h, coreart.HeaderLen+2*w*h)
	}
	img := image.NewNRGBA(image.Rect(0, 0, w, h))
	px := b[coreart.HeaderLen:]
	for y := 0; y < h; y++ {
		for x := 0; x < w; x++ {
			v := binary.LittleEndian.Uint16(px[2*(y*w+x):])
			r := uint8((v >> 11) & 0x1F)
			g := uint8((v >> 5) & 0x3F)
			bl := uint8(v & 0x1F)
			img.SetNRGBA(x, y, color.NRGBA{
				R: r<<3 | r>>2,
				G: g<<2 | g>>4,
				B: bl<<3 | bl>>2,
				A: 0xFF,
			})
		}
	}
	return img, nil
}

// Downscale fits img inside a size x size box, keeping its aspect ratio. A
// cover is square in practice; the ratio is kept anyway, because a 4:3 scan of
// a gatefold squashed into a square is the kind of thing that looks like a
// rendering bug.
func Downscale(img image.Image, size int) image.Image {
	if img == nil || size <= 0 {
		return img
	}
	b := img.Bounds()
	if b.Dx() <= size && b.Dy() <= size {
		return img
	}
	w, h := b.Dx(), b.Dy()
	if w >= h {
		h = h * size / w
		w = size
	} else {
		w = w * size / h
		h = size
	}
	if w < 1 {
		w = 1
	}
	if h < 1 {
		h = 1
	}
	dst := image.NewNRGBA(image.Rect(0, 0, w, h))
	xdraw.CatmullRom.Scale(dst, dst.Bounds(), img, b, xdraw.Src, nil)
	return dst
}

// --- drawing ------------------------------------------------------------

// albumsPane is the whole Albums tab.
func (u *UI) albumsPane(gtx C) D {
	if len(u.st.Albums) == 0 {
		return u.gridEmpty(gtx)
	}
	cols, tile := gridMetrics(gtx.Constraints.Max.X, gtx.Metric.PxPerDp)
	rows := (len(u.st.Albums) + cols - 1) / cols
	u.ensureTileClicks(len(u.st.Albums))

	list := material.List(u.th, &u.grid)
	list.Indicator.Color = u.pal.SelSub
	list.Indicator.HoverColor = u.pal.Muted2
	return list.Layout(gtx, rows, func(gtx C, row int) D {
		return layout.Inset{Bottom: unit.Dp(tileGap)}.Layout(gtx, func(gtx C) D {
			children := make([]layout.FlexChild, 0, cols*2)
			for c := 0; c < cols; c++ {
				i := row*cols + c
				if c > 0 {
					children = append(children, layout.Rigid(layout.Spacer{Width: unit.Dp(tileGap)}.Layout))
				}
				if i >= len(u.st.Albums) {
					// An empty cell keeps the last row's tiles the same
					// size as every other row's.
					children = append(children, layout.Flexed(1, func(gtx C) D {
						return D{Size: image.Pt(gtx.Constraints.Min.X, 0)}
					}))
					continue
				}
				a := u.st.Albums[i]
				idx := i
				children = append(children, layout.Flexed(1, func(gtx C) D {
					return u.tile(gtx, a, idx, tile)
				}))
			}
			return layout.Flex{Alignment: layout.Start}.Layout(gtx, children...)
		})
	})
}

// gridMetrics is the reflow: as many columns as fit with tiles at least
// tileMin wide, then tiles no wider than tileMax.
func gridMetrics(width int, pxPerDp float32) (cols, tile int) {
	if pxPerDp <= 0 {
		pxPerDp = 1
	}
	min := int(float32(tileMin) * pxPerDp)
	max := int(float32(tileMax) * pxPerDp)
	gap := int(float32(tileGap) * pxPerDp)
	if width < min {
		return 1, width
	}
	cols = (width + gap) / (min + gap)
	if cols < 1 {
		cols = 1
	}
	tile = (width - gap*(cols-1)) / cols
	if tile > max {
		cols = (width + gap) / (max + gap)
		if cols < 1 {
			cols = 1
		}
		tile = (width - gap*(cols-1)) / cols
	}
	return cols, tile
}

// ensureTileClicks keeps one Clickable per tile. They are kept across frames
// (a Clickable that is rebuilt loses the press it is in the middle of).
func (u *UI) ensureTileClicks(n int) {
	for len(u.tileClicks) < n {
		u.tileClicks = append(u.tileClicks, &widget.Clickable{})
	}
}

// tile draws one album: the picture (or a coloured plate with its initial),
// the state badge, and the title underneath.
func (u *UI) tile(gtx C, a Album, idx, size int) D {
	click := u.tileClicks[idx]
	return layout.Flex{Axis: layout.Vertical}.Layout(gtx, layout.Rigid(func(gtx C) D {
		return click.Layout(gtx, func(gtx C) D {
			return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
				layout.Rigid(func(gtx C) D {
					gtx.Constraints = layout.Exact(image.Pt(gtx.Constraints.Max.X, size))
					return u.tileArt(gtx, a)
				}),
				layout.Rigid(layout.Spacer{Height: unit.Dp(4)}.Layout),
				layout.Rigid(func(gtx C) D {
					l := material.Label(u.th, unit.Sp(11), tileTitle(a))
					l.Color = u.pal.Muted
					l.MaxLines = 1
					l.Truncator = " "
					return l.Layout(gtx)
				}),
			)
		})
	}))
}

// tileTitle is what goes under the picture: the album half of
// "Artist - Album" when there is one, because every tile in a row carrying
// the same artist's name truncated at the same point is a column of identical
// strings.
func tileTitle(a Album) string {
	t := a.Title
	if t == "" {
		t = filepath.Base(a.Dir)
	}
	if i := strings.Index(t, " - "); i > 0 && i+3 < len(t) {
		return t[i+3:]
	}
	return t
}

// tileArt draws the square: the cover if the cache has one, otherwise the
// coloured plate with the album's initial, then the badge.
func (u *UI) tileArt(gtx C, a Album) D {
	sz := gtx.Constraints.Max
	r := gtx.Dp(4)
	defer clip.UniformRRect(image.Rectangle{Max: sz}, r).Push(gtx.Ops).Pop()

	drawn := false
	if img, ok := u.thumbs.get(a.Dir, a.Mod); ok {
		if b := img.Size(); b.X > 0 && b.Y > 0 {
			// Cover the square: scale by the larger ratio and let the
			// clip take the overhang, the way a CSS background-size:
			// cover does. The transform goes on before the source, so
			// the paint that follows uses both.
			s := float32(sz.X) / float32(b.X)
			if sy := float32(sz.Y) / float32(b.Y); sy > s {
				s = sy
			}
			stack := scaleOp(gtx, s)
			img.Add(gtx.Ops)
			paint.PaintOp{}.Add(gtx.Ops)
			stack.Pop()
			drawn = true
		}
	}
	if !drawn {
		u.plainTile(gtx, a, sz)
	}
	if a.State == TileOnDevice {
		// Already on the iPod: the same picture, quieter, so what is
		// about to move is what the eye lands on.
		paint.FillShape(gtx.Ops, veil(u.pal.Surface),
			clip.Rect{Max: sz}.Op())
	}
	u.badge(gtx, a, sz)
	return D{Size: sz}
}

// veil is the Surface colour at a third opacity: the dimming for an album that
// is already on the device.
func veil(c color.NRGBA) color.NRGBA {
	c.A = 0x66
	return c
}

// plainTile is the fallback picture: a flat colour from the album's name with
// its initial in the middle. It is what a library with no embedded covers
// looks like (direction B's stated tradeoff), and it is what the canned demo
// states draw, so the grid has something to be reviewed against with no files
// on the machine at all.
func (u *UI) plainTile(gtx C, a Album, sz image.Point) {
	paint.FillShape(gtx.Ops, tileColor(a.Title+a.Dir), clip.Rect{Max: sz}.Op())
	letter := initial(tileTitle(a))
	if letter == "" {
		return
	}
	gtx.Constraints = layout.Exact(sz)
	layout.Center.Layout(gtx, func(gtx C) D {
		l := material.Label(u.th, unit.Sp(float32(sz.Y)/gtx.Metric.PxPerSp/2.6), letter)
		l.Color = color.NRGBA{R: 0xFF, G: 0xFF, B: 0xFF, A: 0x9A}
		l.MaxLines = 1
		return l.Layout(gtx)
	})
}

// initial is the first letter of a title, upper-cased.
func initial(s string) string {
	for _, r := range s {
		if r > ' ' {
			return strings.ToUpper(string(r))
		}
	}
	return ""
}

// tileColor is a deterministic muted colour per album: the same album is the
// same colour on every run, and nothing is ever the accent, which belongs to
// the one primary button and the progress bar.
func tileColor(seed string) color.NRGBA {
	h := fnv.New32a()
	_, _ = h.Write([]byte(seed))
	v := h.Sum32()
	// Six dusty hues from the Linen family, each with three values, so a
	// screen of tiles reads as one palette rather than as a paint chart.
	hues := [][3]uint8{
		{0x8A, 0x6A, 0x5A}, {0x5C, 0x6B, 0x7A}, {0xB8, 0xA6, 0x8A},
		{0x4D, 0x5A, 0x4A}, {0x9A, 0x8B, 0x9C}, {0xC9, 0xA4, 0x7A},
		{0x6B, 0x7C, 0x8A}, {0x8C, 0x6F, 0x66}, {0x7D, 0x8A, 0x6A},
		{0xA5, 0x80, 0x60}, {0x6E, 0x6A, 0x80}, {0x58, 0x6B, 0x6E},
	}
	base := hues[v%uint32(len(hues))]
	shade := int(v/uint32(len(hues))%3) - 1 // -1, 0, +1
	adj := func(c uint8) uint8 {
		n := int(c) + shade*14
		if n < 0 {
			n = 0
		}
		if n > 255 {
			n = 255
		}
		return uint8(n)
	}
	return color.NRGBA{R: adj(base[0]), G: adj(base[1]), B: adj(base[2]), A: 0xFF}
}

// badge is the NEW / CHANGED chip in the tile's top right corner.
func (u *UI) badge(gtx C, a Album, sz image.Point) {
	text := a.State.Badge()
	if text == "" {
		return
	}
	bg, fg := u.pal.Accent, u.pal.Surface
	if a.State == TileChanged {
		bg, fg = u.pal.Ink, u.pal.Surface
	}
	gtx.Constraints = layout.Exact(sz)
	layout.NE.Layout(gtx, func(gtx C) D {
		return layout.Inset{Top: unit.Dp(5), Right: unit.Dp(5)}.Layout(gtx, func(gtx C) D {
			return layout.Stack{}.Layout(gtx,
				layout.Expanded(func(gtx C) D {
					r := gtx.Dp(3)
					paint.FillShape(gtx.Ops, bg,
						clip.UniformRRect(image.Rectangle{Max: gtx.Constraints.Min}, r).Op(gtx.Ops))
					return D{Size: gtx.Constraints.Min}
				}),
				layout.Stacked(func(gtx C) D {
					return layout.Inset{Top: unit.Dp(1), Bottom: unit.Dp(1),
						Left: unit.Dp(4), Right: unit.Dp(4)}.Layout(gtx, func(gtx C) D {
						l := material.Label(u.th, unit.Sp(9), text)
						l.Color = fg
						l.MaxLines = 1
						return l.Layout(gtx)
					})
				}),
			)
		})
	})
}

// gridEmpty is the Albums tab with no plan yet: what is missing and the one
// thing to do about it, rather than a blank rectangle.
func (u *UI) gridEmpty(gtx C) D {
	head, note := "Nothing scanned yet", ""
	switch {
	case strings.TrimSpace(u.st.Source) == "":
		head = "No music folder yet"
		note = "Choose the folder that holds your albums — one folder per album, " +
			"named \"Album - Artist\" — and this becomes your library."
	case u.st.Device.Volume == "":
		head = "Waiting for the iPod's drive"
		note = "The albums appear once the iPod's music volume has been read."
	default:
		note = "Reading " + u.st.Source + " and comparing it with the iPod."
	}
	return u.topPlate(gtx, func(gtx C) D {
		return layout.UniformInset(unit.Dp(18)).Layout(gtx, func(gtx C) D {
			gtx.Constraints.Min.X = gtx.Constraints.Max.X
			return layout.Flex{Axis: layout.Vertical}.Layout(gtx,
				layout.Rigid(u.line(16, u.pal.Ink, head)),
				layout.Rigid(layout.Spacer{Height: unit.Dp(8)}.Layout),
				layout.Rigid(u.wrap(12, u.pal.Muted, note)),
				layout.Rigid(layout.Spacer{Height: unit.Dp(12)}.Layout),
				layout.Rigid(func(gtx C) D {
					return u.buttonRow(gtx,
						btn{&u.browseBtn, "Choose the music folder…", true, !u.st.Busy() && u.pickerOK},
						btn{&u.rescanBtn, "Scan again", false, !u.st.Busy() && u.st.Source != ""},
					)
				}),
			)
		})
	})
}

// scaleOp pushes a uniform scale about the origin, which is how a picture
// of one size is painted into a box of another.
func scaleOp(gtx C, s float32) op.TransformStack {
	return op.Affine(f32.Affine2D{}.Scale(f32.Pt(0, 0), f32.Pt(s, s))).Push(gtx.Ops)
}
