// SPDX-License-Identifier: Apache-2.0

package devicefs

import (
	"bufio"
	"fmt"
	"io"
	"os"
	"path/filepath"
	"runtime"
	"strings"
)

// refusedFSTypes are the filesystem types that mean "this is a Windows drive
// seen from inside WSL". /mnt/c and friends are 9p (WSL2) or drvfs (WSL1);
// virtiofs appears in some WSL preview builds.
//
// Writing an iPod's FAT32 volume through one of them is not reliable: the
// bytes go through a Windows-side translation layer that reorders and delays
// them, and tools/make_log.py's header says as much ("copy Windows-natively,
// not via /mnt/d"). On this box /mnt/d has been observed to be a STALE mount
// whose contents bear no relation to what is actually on the device. So the
// app refuses rather than writes a library that half-arrives.
var refusedFSTypes = map[string]string{
	"9p":       "9p (WSL)",
	"drvfs":    "drvfs (WSL)",
	"v9fs":     "9p (WSL)",
	"virtiofs": "virtiofs (WSL)",
}

// RefusesMount reports an error when path lives on a mount this program must
// not write the device through. On anything but Linux it returns nil: there
// is no such translation layer to get in the way.
func RefusesMount(path string) error {
	if runtime.GOOS != "linux" {
		return nil
	}
	mounts, err := readMounts()
	if err != nil || len(mounts) == 0 {
		// No /proc to consult: say nothing rather than block a real volume.
		return nil
	}
	return refusesMount(path, mounts)
}

// mountEntry is one row of /proc/self/mountinfo or /proc/mounts, reduced to
// what the decision needs.
type mountEntry struct {
	Point  string // mount point
	FSType string
	Source string
	Opts   string // super options, where the WSL "aname=drvfs" marker lives
}

// refusesMount is RefusesMount's decision, split out so the tests can feed it
// a fake mount table.
func refusesMount(path string, mounts []mountEntry) error {
	abs := absCandidate(path)
	best := -1
	for i, m := range mounts {
		if !underMount(abs, m.Point) {
			continue
		}
		if best < 0 || len(m.Point) > len(mounts[best].Point) {
			best = i
		}
	}
	if best < 0 {
		return nil
	}
	m := mounts[best]
	name, bad := refusedFSTypes[m.FSType]
	if !bad && strings.Contains(m.Opts, "aname=drvfs") {
		name, bad = "drvfs (WSL)", true
	}
	if !bad {
		return nil
	}
	return fmt.Errorf(
		"%s is on %s, mounted at %s — writes to a FAT volume through that layer are "+
			"not reliable, and this mount can be stale. Run this from Windows against the "+
			"iPod's drive letter instead (for example: core.exe sync --dst D:\\)",
		path, name, m.Point)
}

// absCandidate turns path into an absolute, symlink-resolved path, falling
// back to the nearest ancestor that exists — the destination directory may be
// about to be created.
func absCandidate(path string) string {
	abs, err := filepath.Abs(path)
	if err != nil {
		abs = path
	}
	p := abs
	for {
		if resolved, err := filepath.EvalSymlinks(p); err == nil {
			if p == abs {
				return resolved
			}
			return filepath.Join(resolved, strings.TrimPrefix(abs, p))
		}
		parent := filepath.Dir(p)
		if parent == p {
			return abs
		}
		p = parent
	}
}

// underMount reports whether abs is the mount point or lives under it.
func underMount(abs, point string) bool {
	if point == "" {
		return false
	}
	if abs == point {
		return true
	}
	if point == "/" {
		return strings.HasPrefix(abs, "/")
	}
	return strings.HasPrefix(abs, point+"/")
}

func readMounts() ([]mountEntry, error) {
	if f, err := os.Open("/proc/self/mountinfo"); err == nil {
		defer f.Close()
		return parseMountInfo(f), nil
	}
	f, err := os.Open("/proc/mounts")
	if err != nil {
		return nil, err
	}
	defer f.Close()
	return parseMounts(f), nil
}

// parseMountInfo reads /proc/self/mountinfo. Fields up to an optional run of
// tags are positional, then a lone "-" separator, then the filesystem type,
// the source and the super options:
//
//	36 35 0:32 / /mnt/c rw,noatime - 9p C:\ rw,aname=drvfs;path=C:\
func parseMountInfo(r io.Reader) []mountEntry {
	var out []mountEntry
	sc := bufio.NewScanner(r)
	sc.Buffer(make([]byte, 0, 64*1024), 1024*1024)
	for sc.Scan() {
		f := strings.Fields(sc.Text())
		if len(f) < 10 {
			continue
		}
		sep := -1
		for i := 6; i < len(f); i++ {
			if f[i] == "-" {
				sep = i
				break
			}
		}
		if sep < 0 || sep+2 >= len(f) {
			continue
		}
		e := mountEntry{
			Point:  unescapeOctal(f[4]),
			FSType: f[sep+1],
			Source: unescapeOctal(f[sep+2]),
		}
		if sep+3 < len(f) {
			e.Opts = f[sep+3]
		}
		out = append(out, e)
	}
	return out
}

// parseMounts reads the older /proc/mounts: source, point, type, options.
func parseMounts(r io.Reader) []mountEntry {
	var out []mountEntry
	sc := bufio.NewScanner(r)
	sc.Buffer(make([]byte, 0, 64*1024), 1024*1024)
	for sc.Scan() {
		f := strings.Fields(sc.Text())
		if len(f) < 4 {
			continue
		}
		out = append(out, mountEntry{
			Source: unescapeOctal(f[0]),
			Point:  unescapeOctal(f[1]),
			FSType: f[2],
			Opts:   f[3],
		})
	}
	return out
}

// unescapeOctal undoes the \040-style escaping the kernel applies to spaces,
// tabs, newlines and backslashes in mount paths.
func unescapeOctal(s string) string {
	if !strings.Contains(s, `\`) {
		return s
	}
	var b strings.Builder
	for i := 0; i < len(s); i++ {
		if s[i] == '\\' && i+3 < len(s) &&
			s[i+1] >= '0' && s[i+1] <= '7' &&
			s[i+2] >= '0' && s[i+2] <= '7' &&
			s[i+3] >= '0' && s[i+3] <= '7' {
			b.WriteByte((s[i+1]-'0')<<6 | (s[i+2]-'0')<<3 | (s[i+3] - '0'))
			i += 3
			continue
		}
		b.WriteByte(s[i])
	}
	return b.String()
}
