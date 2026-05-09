package cli

import (
	"context"
	"errors"
	"fmt"
	"os"
	"os/signal"
	"path/filepath"
	"syscall"
	"time"

	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/artistart"
	"github.com/BrandonDedolph/ipod_theme/core/cli/internal/tagcache"
	"github.com/spf13/cobra"
)

func newTagcacheCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "tagcache",
		Short: "Build and inspect the binary music index (.tcdb)",
		Long: `Operations on the binary tagcache file the firmware mmaps at boot.

Run "core tagcache build <music-dir>" to scan a music directory and emit
a .tcdb. The firmware reads it instead of re-scanning + re-parsing tags
on every startup, which is too slow over USB-disk speeds on real iPod
hardware.`,
	}
	cmd.AddCommand(newTagcacheBuildCmd())
	cmd.AddCommand(newTagcacheDumpCmd())
	return cmd
}

func newTagcacheDumpCmd() *cobra.Command {
	cmd := &cobra.Command{
		Use:   "dump <file.tcdb>",
		Short: "Decode a .tcdb file and print its contents (debug aid)",
		Args:  cobra.ExactArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			data, err := os.ReadFile(args[0])
			if err != nil {
				return err
			}
			m, err := tagcache.Read(data)
			if err != nil {
				return err
			}
			out := cmd.OutOrStdout()
			fmt.Fprintf(out, "Songs (%d):\n", len(m.Songs))
			for i, s := range m.Songs {
				fmt.Fprintf(out,
					"  [%d] %q  artist=%d album=%d genre=%d composer=%d  art=%dB\n",
					i, s.Title,
					m.SongArtistIdx[i], m.SongAlbumIdx[i],
					m.SongGenreIdx[i], m.SongComposerIdx[i],
					len(s.ArtBytes))
			}
			fmt.Fprintf(out, "Artists (%d):   %v\n", len(m.UniqArtists),   m.UniqArtists)
			fmt.Fprintf(out, "Albums (%d):    %v\n", len(m.UniqAlbums),    m.UniqAlbums)
			fmt.Fprintf(out, "Genres (%d):    %v\n", len(m.UniqGenres),    m.UniqGenres)
			fmt.Fprintf(out, "Composers (%d): %v\n", len(m.UniqComposers), m.UniqComposers)
			fmt.Fprintf(out, "Artist groups:   %v\n", m.ArtistGroups)
			fmt.Fprintf(out, "Album groups:    %v\n", m.AlbumGroups)
			fmt.Fprintf(out, "Genre groups:    %v\n", m.GenreGroups)
			fmt.Fprintf(out, "Composer groups: %v\n", m.ComposerGroups)
			return nil
		},
	}
	return cmd
}

func newTagcacheBuildCmd() *cobra.Command {
	var (
		out      string
		force    bool
		fetchArt bool
	)
	cmd := &cobra.Command{
		Use:   "build <music-dir>",
		Short: "Scan a music directory and emit a .tcdb file",
		Args:  cobra.ExactArgs(1),
		RunE: func(cmd *cobra.Command, args []string) error {
			dir := args[0]
			if out == "" {
				out = filepath.Join(dir, "tagcache.tcdb")
			}
			songs, err := tagcache.Scan(dir)
			if err != nil {
				return err
			}
			model := tagcache.Build(songs)

			if fetchArt {
				if err := fetchArtistArt(cmd, model); err != nil {
					return err
				}
			}

			// Refuse to overwrite an existing file unless --force.
			// O_EXCL gives us the atomic check; the user gets a clear
			// error rather than silently losing a stale tagcache they
			// might still want.
			flag := os.O_WRONLY | os.O_CREATE | os.O_TRUNC
			if !force {
				flag = os.O_WRONLY | os.O_CREATE | os.O_EXCL
			}
			f, err := os.OpenFile(out, flag, 0o644)
			if err != nil {
				if os.IsExist(err) {
					return fmt.Errorf("%s already exists; pass --force to overwrite", out)
				}
				return fmt.Errorf("create %s: %w", out, err)
			}
			defer f.Close()
			if err := model.Write(f); err != nil {
				return err
			}
			fi, _ := f.Stat()
			fmt.Fprintf(cmd.OutOrStdout(),
				"%s: %d song%s, %d artist%s, %d album%s, %d genre%s, %d composer%s (%d bytes)\n",
				out,
				len(model.Songs),         pl(len(model.Songs)),
				len(model.UniqArtists),   pl(len(model.UniqArtists)),
				len(model.UniqAlbums),    pl(len(model.UniqAlbums)),
				len(model.UniqGenres),    pl(len(model.UniqGenres)),
				len(model.UniqComposers), pl(len(model.UniqComposers)),
				fi.Size(),
			)
			return nil
		},
	}
	cmd.Flags().StringVarP(&out, "out", "o", "",
		"Output file path (default: <music-dir>/tagcache.tcdb)")
	cmd.Flags().BoolVarP(&force, "force", "f", false,
		"Overwrite the output file if it already exists")
	cmd.Flags().BoolVar(&fetchArt, "fetch-art", false,
		"Fetch artist photos from MusicBrainz/Wikipedia/Commons and embed in the tagcache. "+
			"Rate-limited to 1 req/sec; takes a few minutes for a typical library on first run, "+
			"then is fast on rebuild via the local cache (~/.cache/core/artist-art).")
	return cmd
}

// fetchArtistArt walks UniqArtists and populates model.ArtistArt by
// hitting MusicBrainz / Wikidata / Commons for each. SIGINT during the
// run cancels the context cleanly so the user can ctrl-C out without
// leaving partial state.
//
// On a per-artist failure we keep going — the build is allowed to ship
// with fewer artist photos than artists; the firmware falls back to
// album art for entries without a fetched image.
func fetchArtistArt(cmd *cobra.Command, model *tagcache.Model) error {
	out := cmd.OutOrStdout()
	cacheDir, err := artistart.DefaultCacheDir()
	if err != nil {
		return fmt.Errorf("artist-art cache: %w", err)
	}
	if cacheDir == "" {
		fmt.Fprintln(out, "warning: no user cache dir; fetched artist art won't survive across builds")
	}

	ctx, cancel := context.WithCancel(cmd.Context())
	defer cancel()
	sigCh := make(chan os.Signal, 1)
	signal.Notify(sigCh, os.Interrupt, syscall.SIGTERM)
	defer signal.Stop(sigCh)
	go func() {
		<-sigCh
		fmt.Fprintln(out, "\ninterrupt — finishing current request and stopping")
		cancel()
	}()

	f := artistart.NewFetcher()
	defer f.Close()

	model.ArtistArt = make([][]byte, len(model.UniqArtists))
	hits, miss, fail := 0, 0, 0
	start := time.Now()
	const maxDim = 128
	const quality = 85
	for i, name := range model.UniqArtists {
		if ctx.Err() != nil {
			break
		}
		bytes, cached, err := artistart.CachedFetch(ctx, f, cacheDir, name, maxDim, quality)
		if err == nil {
			model.ArtistArt[i] = bytes
			hits++
			tag := "fetched"
			if cached {
				tag = "cached"
			}
			fmt.Fprintf(out, "  [%d/%d] %-40s %s (%d B)\n",
				i+1, len(model.UniqArtists), truncate(name, 40), tag, len(bytes))
		} else if errors.Is(err, artistart.ErrNotFound) {
			miss++
			fmt.Fprintf(out, "  [%d/%d] %-40s no photo\n",
				i+1, len(model.UniqArtists), truncate(name, 40))
		} else {
			fail++
			fmt.Fprintf(out, "  [%d/%d] %-40s error: %v\n",
				i+1, len(model.UniqArtists), truncate(name, 40), err)
		}
	}
	fmt.Fprintf(out, "artist art: %d ok, %d not-found, %d errors in %s\n",
		hits, miss, fail, time.Since(start).Round(time.Millisecond))
	return nil
}

func truncate(s string, n int) string {
	if len(s) <= n {
		return s
	}
	return s[:n-1] + "…"
}

func pl(n int) string {
	if n == 1 {
		return ""
	}
	return "s"
}
