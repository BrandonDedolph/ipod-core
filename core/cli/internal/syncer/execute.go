// SPDX-License-Identifier: Apache-2.0

package syncer

import (
	"context"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"strings"
	"time"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/coreart"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/devicefs"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/flac"
)

// Report is what a sync did. Counts, bytes, and Written — every path the sync
// wrote, in the order it wrote them, which is how the tests hold the "index
// last" rule without reading timestamps off a FAT volume with two-second
// resolution.
type Report struct {
	Albums  int `json:"albums"`
	Copied  int `json:"copied"`
	Skipped int `json:"skipped"`
	Renamed int `json:"renamed"`

	CopiedBytes  int64 `json:"copied_bytes"`
	SkippedBytes int64 `json:"skipped_bytes"`

	ArtWritten int `json:"art_written"`
	ArtSkipped int `json:"art_skipped"`
	// ArtMissing counts albums whose source carries no embedded cover.
	// That is a warning, never an error: the device draws a placeholder.
	ArtMissing int `json:"art_missing"`

	PlaylistsWritten   int `json:"playlists_written"`
	PlaylistsUnchanged int `json:"playlists_unchanged"`
	PlaylistEntries    int `json:"playlist_entries"`
	PlaylistDropped    int `json:"playlist_dropped"`

	Pruned      int   `json:"pruned"`
	PrunedBytes int64 `json:"pruned_bytes"`

	ConfigCreated bool `json:"config_created"`
	LogCreated    bool `json:"log_created"`

	IndexWritten bool `json:"index_written"`
	IndexBytes   int  `json:"index_bytes"`
	IndexRecords int  `json:"index_records"`

	Written  []string `json:"written"`
	Warnings []string `json:"warnings"`
	DryRun   bool     `json:"dry_run"`

	Elapsed time.Duration `json:"elapsed"`
}

// Execute carries out a plan.
//
// The order is the contract, not an implementation detail:
//
//	per album: rename, copy, art     — the files the index will name
//	playlists                        — they point at those files
//	prune                            — only with --prune --yes
//	CORECFG.DAT, CORELOG.BIN         — created only when absent or invalid
//	Music/CORELIB.IDX                — LAST, via temp + rename
//
// A failure anywhere before the last step returns with the index untouched, so
// the device keeps an index that still matches the files it still has. The
// Report says what had already been done.
func Execute(ctx context.Context, p *Plan, o Options) (*Report, error) {
	if p == nil {
		return nil, fmt.Errorf("syncer: no plan to execute")
	}
	start := time.Now()
	rep := &Report{
		Albums:       p.Albums,
		Warnings:     append([]string(nil), p.Warnings...),
		IndexBytes:   p.Index.Size,
		IndexRecords: p.Index.Records,
		DryRun:       o.DryRun,
	}

	if o.DryRun {
		// A dry run reports the plan and writes nothing at all — not the
		// music, not the playlists, and not CORECFG.DAT either. "It only
		// created the config file" is not a dry run.
		rep.Copied, rep.CopiedBytes = len(p.Copy), p.CopyBytes
		rep.Skipped, rep.SkippedBytes = len(p.Skip), p.SkipBytes
		rep.Renamed = len(p.Rename)
		for _, a := range p.Art {
			if a.Write {
				rep.ArtWritten++
			} else {
				rep.ArtSkipped++
			}
		}
		for _, pl := range p.Playlists {
			if pl.Unchanged {
				rep.PlaylistsUnchanged++
			} else {
				rep.PlaylistsWritten++
			}
			rep.PlaylistEntries += len(pl.Entries)
			rep.PlaylistDropped += len(pl.Dropped)
		}
		rep.Pruned, rep.PrunedBytes = len(p.Prune), p.PruneBytes
		rep.ConfigCreated, rep.LogCreated = p.Config, p.Log
		rep.IndexWritten = !p.Index.Unchanged
		rep.Elapsed = time.Since(start)
		return rep, nil
	}

	// Nothing is deleted on the strength of one flag. --prune lists; --prune
	// --yes removes. The refusal happens before any write so the user can
	// read the list and decide without half a sync behind them.
	if o.Prune && !o.Yes && len(p.Prune) > 0 {
		return nil, fmt.Errorf("%w: %d item(s) under %s/ are not in the source tree",
			ErrPruneNeedsYes, len(p.Prune), devicefs.MusicDir)
	}
	// A source that scanned to nothing — a typo in --src, an unmounted
	// drive, an empty folder — would make every album on the device an
	// orphan, and --prune --yes would then empty Music/. That is never what
	// the flags meant. Refuse before anything is written.
	if o.Prune && o.Yes && len(p.Prune) > 0 && p.Tracks == 0 {
		return nil, fmt.Errorf("%w: the source tree has no tracks, so --prune would remove every album on the device (%d item(s)); check --src",
			ErrPruneEmptySource, len(p.Prune))
	}

	emit := func(e Event) {
		if o.Progress != nil {
			o.Progress(e)
		}
	}
	warn := func(format string, a ...any) {
		msg := fmt.Sprintf(format, a...)
		rep.Warnings = append(rep.Warnings, msg)
		emit(Event{Kind: EventWarning, Message: msg})
	}

	musicDir := filepath.Join(o.Dst, devicefs.MusicDir)
	if err := os.MkdirAll(musicDir, 0o777); err != nil {
		return rep, err
	}

	// --- music -------------------------------------------------------------
	emit(Event{Kind: EventPhase, Phase: "copy"})
	byAlbum := groupOps(p)
	buf := make([]byte, CopyBufferSize)
	for i, name := range p.AlbumOrder {
		if err := ctx.Err(); err != nil {
			return rep, err
		}
		g := byAlbum[name]
		ev := Event{Kind: EventAlbum, Album: name, Index: i + 1, Total: len(p.AlbumOrder)}

		for _, op := range g.rename {
			if err := os.MkdirAll(filepath.Dir(op.Dst), 0o777); err != nil {
				return rep, err
			}
			if err := os.Rename(op.Src, op.Dst); err != nil {
				return rep, fmt.Errorf("rename %s -> %s: %w", op.Src, op.Dst, err)
			}
			rep.Renamed++
			rep.Written = append(rep.Written, op.Dst)
			ev.Renamed++
		}
		for _, op := range g.copy {
			if err := ctx.Err(); err != nil {
				return rep, err
			}
			n, err := copyFile(ctx, op.Src, op.Dst, buf)
			if err != nil {
				return rep, fmt.Errorf("copy %s -> %s: %w", op.Src, op.Dst, err)
			}
			rep.Copied++
			rep.CopiedBytes += n
			rep.Written = append(rep.Written, op.Dst)
			ev.Copied++
			ev.Bytes += n
		}
		rep.Skipped += len(g.skip)
		ev.Skipped = len(g.skip)
		for _, op := range g.skip {
			rep.SkippedBytes += op.Size
		}

		switch {
		case g.art == nil:
			ev.Art = "off"
		case !g.art.Write:
			ev.Art = "skip"
			rep.ArtSkipped++
		default:
			wrote, err := writeArt(*g.art)
			if err != nil {
				// Art is decoration; music is not. A cover that will not
				// render must not cost the user the sync.
				warn("%s: art failed (%v)", name, err)
				ev.Art = "fail"
				break
			}
			if !wrote {
				rep.ArtMissing++
				ev.Art = "none"
				warn("%s: no embedded cover; the device will draw a placeholder", name)
				break
			}
			rep.ArtWritten++
			ev.Art = "ok"
			rep.Written = append(rep.Written,
				filepath.Join(g.art.Dir, coreart.ArtName),
				filepath.Join(g.art.Dir, coreart.ThumbName))
		}
		emit(ev)
	}

	// --- playlists ---------------------------------------------------------
	if len(p.Playlists) > 0 {
		emit(Event{Kind: EventPhase, Phase: "playlists"})
	}
	for _, pl := range p.Playlists {
		rep.PlaylistEntries += len(pl.Entries)
		rep.PlaylistDropped += len(pl.Dropped)
		if pl.Unchanged {
			rep.PlaylistsUnchanged++
			continue
		}
		if err := devicefs.WriteM3U8(pl.Dst, pl.Entries); err != nil {
			return rep, err
		}
		rep.PlaylistsWritten++
		rep.Written = append(rep.Written, pl.Dst)
	}

	// --- prune -------------------------------------------------------------
	if o.Prune && o.Yes && len(p.Prune) > 0 {
		emit(Event{Kind: EventPhase, Phase: "prune"})
		for _, victim := range p.Prune {
			// The plan only ever lists paths under Music/; this is the
			// last check before the one destructive call in the package.
			if !underDir(victim, musicDir) {
				return rep, fmt.Errorf("prune %s: refusing to remove a path outside %s", victim, musicDir)
			}
			if err := os.RemoveAll(victim); err != nil {
				return rep, fmt.Errorf("prune %s: %w", victim, err)
			}
			rep.Pruned++
		}
		rep.PrunedBytes = p.PruneBytes
	}

	// --- device files ------------------------------------------------------
	emit(Event{Kind: EventPhase, Phase: "device files"})
	created, err := devicefs.EnsureConfig(o.Dst)
	if err != nil {
		return rep, err
	}
	rep.ConfigCreated = created
	if created {
		rep.Written = append(rep.Written, filepath.Join(o.Dst, devicefs.ConfigName))
	}
	created, err = devicefs.EnsureLog(o.Dst, 0)
	if err != nil {
		return rep, err
	}
	rep.LogCreated = created
	if created {
		rep.Written = append(rep.Written, filepath.Join(o.Dst, devicefs.LogName))
	}

	// --- the index, last ---------------------------------------------------
	emit(Event{Kind: EventPhase, Phase: "index"})
	if !p.Index.Unchanged {
		if err := writeIndex(p.Index); err != nil {
			return rep, err
		}
		rep.IndexWritten = true
		rep.Written = append(rep.Written, p.Index.Dst)
	}

	rep.Elapsed = time.Since(start)
	emit(Event{Kind: EventDone, Phase: "done"})
	return rep, nil
}

// underDir reports whether p is strictly inside dir (case-insensitively,
// since the volume is).
func underDir(p, dir string) bool {
	rel, err := filepath.Rel(strings.ToLower(filepath.Clean(dir)), strings.ToLower(filepath.Clean(p)))
	if err != nil {
		return false
	}
	return rel != "." && rel != ".." && !strings.HasPrefix(rel, ".."+string(filepath.Separator))
}

// albumOps is one album's share of the plan.
type albumOps struct {
	copy, skip, rename []FileOp
	art                *ArtOp
}

func groupOps(p *Plan) map[string]*albumOps {
	m := map[string]*albumOps{}
	get := func(name string) *albumOps {
		g := m[name]
		if g == nil {
			g = &albumOps{}
			m[name] = g
		}
		return g
	}
	for _, op := range p.Copy {
		g := get(op.Album)
		g.copy = append(g.copy, op)
	}
	for _, op := range p.Skip {
		g := get(op.Album)
		g.skip = append(g.skip, op)
	}
	for _, op := range p.Rename {
		g := get(op.Album)
		g.rename = append(g.rename, op)
	}
	for i := range p.Art {
		get(p.Art[i].Album).art = &p.Art[i]
	}
	for _, name := range p.AlbumOrder {
		get(name)
	}
	return m
}

// writeArt renders the two sidecars. It returns false (with no error) when the
// album's source file has no embedded picture.
func writeArt(op ArtOp) (bool, error) {
	m, err := flac.ReadFile(op.SrcFLAC)
	if err != nil {
		return false, err
	}
	if err := os.MkdirAll(op.Dir, 0o777); err != nil {
		return false, err
	}
	res, err := coreart.WriteAlbum(op.Dir, m)
	if err != nil {
		return false, err
	}
	return !res.NoPicture, nil
}

// writeIndex puts CORELIB.IDX down through a temp file in the same folder and
// a rename, so a reader — including a device that is mounted while this runs —
// sees either the old index or the new one, never a half-written file whose
// header count disagrees with its length. The firmware refuses such a file and
// falls back to scanning every track's tags, which takes minutes.
func writeIndex(op IndexOp) error {
	dir := filepath.Dir(op.Dst)
	if err := os.MkdirAll(dir, 0o777); err != nil {
		return err
	}
	// A plain 8.3 name: the temp file lands on FAT, and a leading dot plus a
	// long random suffix is a long-name entry for something that exists for
	// half a second.
	tmp := filepath.Join(dir, "CORELIB.TMP")
	f, err := devicefs.OpenWriteThrough(tmp)
	if err != nil {
		return err
	}
	defer os.Remove(tmp) // a no-op once the rename below succeeded
	if _, err := f.Write(op.Bytes); err != nil {
		f.Close()
		return fmt.Errorf("write %s: %w", tmp, err)
	}
	if err := f.Sync(); err != nil {
		f.Close()
		return fmt.Errorf("flush %s: %w", tmp, err)
	}
	if err := f.Close(); err != nil {
		return err
	}
	if err := os.Chmod(tmp, 0o644); err != nil {
		return err
	}
	return os.Rename(tmp, op.Dst)
}

// copyFile streams src to dst through the caller's buffer, flushes it to the
// medium, and gives the copy the source's modification time — which is what
// makes the next sync able to skip it. Without the Chtimes every run would
// re-send the whole library.
func copyFile(ctx context.Context, src, dst string, buf []byte) (int64, error) {
	in, err := os.Open(src)
	if err != nil {
		return 0, err
	}
	defer in.Close()
	si, err := in.Stat()
	if err != nil {
		return 0, err
	}
	if err := os.MkdirAll(filepath.Dir(dst), 0o777); err != nil {
		return 0, err
	}
	out, err := devicefs.OpenWriteThrough(dst)
	if err != nil {
		return 0, err
	}

	var n int64
	err = func() error {
		for {
			if err := ctx.Err(); err != nil {
				return err
			}
			r, rerr := in.Read(buf)
			if r > 0 {
				w, werr := out.Write(buf[:r])
				n += int64(w)
				if werr != nil {
					return werr
				}
				if w != r {
					return io.ErrShortWrite
				}
			}
			if rerr == io.EOF {
				return nil
			}
			if rerr != nil {
				return rerr
			}
		}
	}()
	if err == nil {
		err = out.Sync()
	}
	if cerr := out.Close(); err == nil {
		err = cerr
	}
	if err != nil {
		// A partial file would be indistinguishable from a good one next
		// run only by size — which is exactly the check that would catch
		// it — but leaving it invites a device that plays half a track.
		os.Remove(dst)
		return n, err
	}
	if err := os.Chtimes(dst, si.ModTime(), si.ModTime()); err != nil {
		return n, fmt.Errorf("set mtime on %s: %w", dst, err)
	}
	return n, nil
}
