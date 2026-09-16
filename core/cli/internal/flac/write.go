package flac

// write.go adds the other half of this package: putting a PICTURE block back
// into a file. Everything here obeys one rule — the audio frames are never
// rewritten unless the whole file is rewritten, and even then they are copied
// byte for byte. A test hashes them before and after on every path.
//
// The metadata region is everything between the fLaC marker and the first
// audio frame: a chain of blocks, each a 4-byte header (1 bit "last", 7 bits
// type, 24 bits body length) followed by its body. Editing it means rebuilding
// the chain in memory and then choosing between two ways of landing it:
//
//   in place   the new chain is the same length as the old one, or short
//              enough that the difference can be given back as PADDING. A
//              single WriteAt covers the bytes that actually changed; the
//              file size, and every audio byte, are untouched.
//   rewrite    otherwise: a whole new file next to the original (marker,
//              chain, fresh PADDING, then the audio copied from the first
//              frame), fsynced and renamed over it. The original is complete
//              and valid until the rename.

import (
	"bufio"
	"bytes"
	"errors"
	"fmt"
	"image"
	"image/color"
	"io"
	"os"
	"path/filepath"

	_ "image/jpeg" // DecodeConfig for the picture's dimensions
	_ "image/png"
)

// blockPadding is the PADDING metadata block type.
const blockPadding = 1

// blockHeaderLen is the fixed size of a metadata block header.
const blockHeaderLen = 4

// maxBlockBody is the largest body a 24-bit length field can describe. This is
// a hard format limit, unlike MaxBlockSize (this package's read-side sanity
// cap), so a picture over it cannot be written at all.
const maxBlockBody = 1<<24 - 1

// DefaultPadding is the PADDING block written when a file has to be rewritten:
// enough room for the next cover swap to happen in place.
const DefaultPadding = 8 << 10

// Options tunes the writers. The zero value is the default everywhere.
type Options struct {
	// Padding is the size of the PADDING block body written when the file is
	// rewritten. Zero means DefaultPadding. Ignored on the in-place path,
	// where the padding is whatever space is left over.
	Padding int
}

func options(opts []Options) Options {
	var o Options
	if len(opts) > 0 {
		o = opts[0]
	}
	if o.Padding <= 0 {
		o.Padding = DefaultPadding
	}
	if o.Padding > maxBlockBody {
		o.Padding = maxBlockBody
	}
	return o
}

// WritePicture stores pic in the FLAC file at path, replacing any picture of
// the same type that is already there (Type zero means PictureTypeFrontCover,
// the front cover, which is the only type this project writes).
//
// Width, Height, Depth and Colors are taken from the image itself — the
// caller's values are ignored — and MIME is sniffed when pic.MIME is empty, so
// the block can never disagree with the bytes it carries. The picture data
// must be a PNG or a JPEG; anything else is an error and the file is not
// touched.
//
// The audio frames are never modified. When the existing metadata leaves
// enough room the new block is written in place and the file's size does not
// change; otherwise the file is rewritten through a temporary file and
// renamed, leaving the original intact until the rename. The modification time
// is deliberately NOT preserved: the change has to be visible to anything that
// caches art by mtime.
func WritePicture(path string, pic Picture, opts ...Options) error {
	if len(pic.Data) == 0 {
		return errors.New("flac: picture has no data")
	}
	if pic.Type == 0 {
		pic.Type = PictureTypeFrontCover
	}
	if err := describeImage(&pic); err != nil {
		return err
	}
	body := buildPicture(pic)
	if len(body) > maxBlockBody {
		return fmt.Errorf("flac: PICTURE block would be %d bytes, over the %d byte format limit", len(body), maxBlockBody)
	}
	return editChain(path, options(opts), func(blocks []metaBlock) ([]metaBlock, error) {
		out := keepBlocks(blocks, func(b metaBlock) bool {
			return b.typ == blockPicture && pictureType(b.body) == pic.Type
		})
		return append(out, metaBlock{typ: blockPicture, body: body}), nil
	})
}

// RemovePictures drops every PICTURE block from the file at path. The space
// they used becomes PADDING, so the file size and the audio frames are
// unchanged and a later WritePicture lands in place.
func RemovePictures(path string, opts ...Options) error {
	return editChain(path, options(opts), func(blocks []metaBlock) ([]metaBlock, error) {
		return keepBlocks(blocks, func(b metaBlock) bool { return b.typ == blockPicture }), nil
	})
}

// keepBlocks returns the chain minus every PADDING block and minus every block
// drop reports. PADDING always goes: the writers re-create exactly one, sized
// to whatever is left over, as the last block.
func keepBlocks(blocks []metaBlock, drop func(metaBlock) bool) []metaBlock {
	out := make([]metaBlock, 0, len(blocks)+1)
	for _, b := range blocks {
		if b.typ == blockPadding || drop(b) {
			continue
		}
		out = append(out, b)
	}
	return out
}

// pictureType reads the type field off a PICTURE body without parsing the
// rest of it: a short body is not a picture of any type we would replace.
func pictureType(body []byte) uint32 {
	if len(body) < 4 {
		return ^uint32(0)
	}
	return uint32(body[0])<<24 | uint32(body[1])<<16 | uint32(body[2])<<8 | uint32(body[3])
}

// metaBlock is one metadata block held in memory. (The test file's rawBlock is
// a different, older helper for hand-assembling malformed chains.)
type metaBlock struct {
	typ  byte
	body []byte
}

// chain is a file's whole metadata region.
type chain struct {
	blocks   []metaBlock
	region   []byte // the bytes from offset 4 up to the first frame
	audioOff int64  // offset of the first audio frame
}

// readChain parses the metadata region of f. It reads bodies rather than
// seeking past them, so a file that is truncated anywhere inside the chain is
// an error here — before anything has been written.
func readChain(f *os.File) (*chain, error) {
	r := bufio.NewReaderSize(f, 64<<10)
	var marker [4]byte
	if _, err := io.ReadFull(r, marker[:]); err != nil || string(marker[:]) != "fLaC" {
		return nil, ErrNotFLAC
	}

	c := &chain{audioOff: 4}
	for n := 0; ; n++ {
		var hdr [blockHeaderLen]byte
		if _, err := io.ReadFull(r, hdr[:]); err != nil {
			return nil, fmt.Errorf("flac: truncated metadata block header %d: %w", n, shortErr(err))
		}
		last := hdr[0]&0x80 != 0
		typ := hdr[0] & 0x7f
		length := uint32(hdr[1])<<16 | uint32(hdr[2])<<8 | uint32(hdr[3])
		if length > MaxBlockSize {
			return nil, fmt.Errorf("flac: metadata block %d (type %d) is %d bytes, over the %d byte cap", n, typ, length, MaxBlockSize)
		}
		body := make([]byte, length)
		if _, err := io.ReadFull(r, body); err != nil {
			return nil, fmt.Errorf("flac: truncated metadata block %d (type %d), want %d bytes: %w", n, typ, length, shortErr(err))
		}
		if n == 0 {
			if typ != blockStreamInfo {
				return nil, fmt.Errorf("flac: first metadata block is type %d, want STREAMINFO", typ)
			}
			if len(body) < streamInfoLen {
				return nil, fmt.Errorf("flac: STREAMINFO is %d bytes, want at least %d", len(body), streamInfoLen)
			}
		}
		c.blocks = append(c.blocks, metaBlock{typ: typ, body: body})
		c.region = append(c.region, hdr[:]...)
		c.region = append(c.region, body...)
		c.audioOff += blockHeaderLen + int64(length)
		if last {
			break
		}
	}
	return c, nil
}

// encodeChain lays blocks out as a metadata region, setting the last-block
// flag on the final block and nowhere else.
func encodeChain(blocks []metaBlock) []byte {
	out := make([]byte, 0, 64)
	for i, b := range blocks {
		out = appendBlock(out, b.typ, i == len(blocks)-1, b.body)
	}
	return out
}

// padBlocks returns PADDING blocks that occupy exactly total bytes, headers
// included (total must be at least blockHeaderLen). More than one is needed
// only when the space given back is over the 16 MiB a single block can
// describe, which real files never do — but a panic there would be ours.
func padBlocks(total int) []metaBlock {
	var out []metaBlock
	for total > 0 {
		body := total - blockHeaderLen
		if body > maxBlockBody {
			body = maxBlockBody
			if total-blockHeaderLen-body < blockHeaderLen {
				body -= blockHeaderLen // leave room for the next header
			}
		}
		out = append(out, metaBlock{typ: blockPadding, body: make([]byte, body)})
		total -= blockHeaderLen + body
	}
	return out
}

// testHookBeforeRename runs in the rewrite path once the temporary file is
// complete and fsynced but before it is renamed over the original. Only tests
// set it: returning an error from it is exactly the crash the rename is meant
// to survive.
var testHookBeforeRename func(tmpPath string) error

// editChain applies edit to the file's block chain and stores the result,
// in place when it fits and by rewriting the file when it does not.
func editChain(path string, o Options, edit func([]metaBlock) ([]metaBlock, error)) error {
	f, err := os.OpenFile(path, os.O_RDWR, 0)
	if err != nil {
		return err
	}
	closed := false
	defer func() {
		if !closed {
			f.Close()
		}
	}()

	c, err := readChain(f)
	if err != nil {
		return fmt.Errorf("%s: %w", path, err)
	}
	blocks, err := edit(c.blocks)
	if err != nil {
		return fmt.Errorf("%s: %w", path, err)
	}
	if len(blocks) == 0 || blocks[0].typ != blockStreamInfo {
		return fmt.Errorf("%s: flac: refusing to write a chain that does not start with STREAMINFO", path)
	}

	// Does it fit in what the old region already occupies? Slack of 0 means an
	// exact fit; 4 or more becomes a PADDING block (its own header is 4 bytes).
	// A slack of 1..3 cannot be expressed as a block, so it falls through to
	// the rewrite.
	region := encodeChain(blocks)
	switch slack := len(c.region) - len(region); {
	case slack == 0:
	case slack >= blockHeaderLen:
		region = encodeChain(append(blocks, padBlocks(slack)...))
	default:
		region = encodeChain(append(blocks, metaBlock{typ: blockPadding, body: make([]byte, o.Padding)}))
		return rewrite(f, &closed, path, c, region)
	}
	if len(region) != len(c.region) {
		// Belt and braces: the in-place path must never move the audio.
		return fmt.Errorf("%s: flac: internal error, new metadata region is %d bytes, old is %d", path, len(region), len(c.region))
	}
	return writeInPlace(f, path, c.region, region)
}

// writeInPlace overwrites the metadata region with one that is exactly as
// long. Only the bytes that actually differ are written, so STREAMINFO (and
// usually everything up to the old PADDING) is not touched at all, and nothing
// outside [4, first frame) is ever written.
func writeInPlace(f *os.File, path string, old, region []byte) error {
	i := 0
	for i < len(region) && region[i] == old[i] {
		i++
	}
	if i < len(region) {
		if _, err := f.WriteAt(region[i:], int64(blockHeaderLen+i)); err != nil {
			return fmt.Errorf("%s: %w", path, err)
		}
	}
	if err := f.Sync(); err != nil {
		return fmt.Errorf("%s: %w", path, err)
	}
	return f.Close()
}

// rewrite builds the new file beside the old one and renames it over the top.
// Until the rename the original is untouched and playable; after it the new
// file is on disk and fsynced. src stays open for the audio copy and is closed
// before the rename, which is what Windows needs to replace it.
func rewrite(src *os.File, closed *bool, path string, c *chain, region []byte) error {
	st, err := src.Stat()
	if err != nil {
		return fmt.Errorf("%s: %w", path, err)
	}
	dir, base := filepath.Split(path)
	if dir == "" {
		dir = "."
	}
	tmp, err := os.CreateTemp(dir, "."+base+".core-*")
	if err != nil {
		return err
	}
	tmpName := tmp.Name()
	done := false
	defer func() {
		if !done {
			tmp.Close()
			os.Remove(tmpName)
		}
	}()

	w := bufio.NewWriterSize(tmp, 1<<20)
	if _, err := w.WriteString("fLaC"); err != nil {
		return err
	}
	if _, err := w.Write(region); err != nil {
		return err
	}
	audio := io.NewSectionReader(src, c.audioOff, st.Size()-c.audioOff)
	if _, err := io.Copy(w, audio); err != nil {
		return fmt.Errorf("%s: copying audio frames: %w", path, err)
	}
	if err := w.Flush(); err != nil {
		return err
	}
	// Keep the original's permissions. Filesystems without modes (9p, FAT over
	// a mount) refuse this, and that is not a reason to abandon the write.
	_ = tmp.Chmod(st.Mode().Perm())
	if err := tmp.Sync(); err != nil {
		return err
	}
	if err := tmp.Close(); err != nil {
		return err
	}
	if testHookBeforeRename != nil {
		if err := testHookBeforeRename(tmpName); err != nil {
			return err
		}
	}
	if err := src.Close(); err != nil {
		return err
	}
	*closed = true
	if err := os.Rename(tmpName, path); err != nil {
		return err
	}
	done = true
	syncDir(dir)
	return nil
}

// syncDir flushes a directory entry so the rename survives a power loss. It is
// a no-op wherever directories cannot be opened (Windows), which is why the
// error is dropped.
func syncDir(dir string) {
	d, err := os.Open(dir)
	if err != nil {
		return
	}
	_ = d.Sync()
	d.Close()
}

// describeImage fills in MIME and the dimension fields from the picture data.
// Only the image header is decoded, so a large cover costs nothing.
func describeImage(p *Picture) error {
	cfg, format, err := image.DecodeConfig(bytes.NewReader(p.Data))
	if err != nil {
		return fmt.Errorf("flac: picture data is not a decodable PNG or JPEG: %w", err)
	}
	if p.MIME == "" {
		p.MIME = "image/" + format
	}
	if cfg.Width <= 0 || cfg.Height <= 0 {
		return fmt.Errorf("flac: picture is %dx%d", cfg.Width, cfg.Height)
	}
	p.Width = uint32(cfg.Width)
	p.Height = uint32(cfg.Height)
	p.Depth, p.Colors = colorDepth(cfg.ColorModel)
	return nil
}

// colorDepth maps a decoded image's colour model onto the PICTURE block's two
// colour fields: bits per pixel, and the palette size for indexed images (0
// for everything else, per the FLAC spec).
func colorDepth(m color.Model) (depth, colors uint32) {
	if pal, ok := m.(color.Palette); ok {
		return 8, uint32(len(pal))
	}
	switch m {
	case color.GrayModel:
		return 8, 0
	case color.Gray16Model:
		return 16, 0
	case color.NRGBAModel, color.RGBAModel:
		return 32, 0
	case color.NRGBA64Model, color.RGBA64Model:
		return 64, 0
	case color.CMYKModel:
		return 32, 0
	default:
		// YCbCr (every baseline JPEG) and anything else RGB-shaped.
		return 24, 0
	}
}
