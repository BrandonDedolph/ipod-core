package cli

import (
	"fmt"
	"os"
	"path/filepath"

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
		out   string
		force bool
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
	return cmd
}

func pl(n int) string {
	if n == 1 {
		return ""
	}
	return "s"
}
