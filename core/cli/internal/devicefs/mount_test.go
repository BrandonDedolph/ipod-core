// SPDX-License-Identifier: Apache-2.0

package devicefs

import (
	"strings"
	"testing"
)

// A trimmed but real /proc/self/mountinfo from this WSL2 box: an ext4 root, a
// 9p /mnt/c and /mnt/d (the drive letter the iPod shows up as from Windows —
// and the mount that was found STALE here on 2026-07-27), plus a WSL1-style
// drvfs row and one whose type is plain but whose super options carry the
// drvfs marker.
const fakeMountInfo = `
21 27 0:20 / /sys rw,nosuid,nodev,noexec,relatime shared:7 - sysfs sysfs rw
23 27 0:5 / /dev rw,nosuid,relatime shared:2 - devtmpfs none rw,size=4056768k
27 1 8:32 / / rw,relatime - ext4 /dev/sdc rw,discard,errors=remount-ro,data=ordered
40 27 0:44 / /mnt/wsl rw,relatime shared:9 - tmpfs none rw
56 27 0:56 / /mnt/c rw,noatime - 9p C:\ rw,dirsync,aname=drvfs;path=C:\;uid=1000;gid=1000,mmap,access=client
58 27 0:58 / /mnt/d rw,noatime - 9p D:\ rw,dirsync,aname=drvfs;path=D:\;uid=1000;gid=1000,mmap,access=client
60 27 0:60 / /mnt/e rw,noatime - drvfs E:\ rw,dirsync,uid=1000
62 27 0:62 / /mnt/f rw,noatime - fuseblk F:\ rw,aname=drvfs
70 27 0:70 / /media/ipod rw,relatime - vfat /dev/sdb1 rw,fmask=0022,shortname=mixed
`

func mounts(t *testing.T) []mountEntry {
	t.Helper()
	m := parseMountInfo(strings.NewReader(strings.TrimSpace(fakeMountInfo)))
	if len(m) != 9 {
		t.Fatalf("parsed %d rows, want 9: %+v", len(m), m)
	}
	return m
}

func TestRefusesMount(t *testing.T) {
	m := mounts(t)
	cases := []struct {
		path   string
		refuse bool
		why    string
	}{
		{"/mnt/d", true, "the drive letter the iPod appears as, over 9p"},
		{"/mnt/d/Music", true, "a directory under it"},
		{"/mnt/d/Music/Artist - Album/01. Song.flac", true, "a file under it"},
		{"/mnt/c/Users/brandon-home/Music/MC", true, "the source tree is on 9p too"},
		{"/mnt/e/whatever", true, "WSL1 drvfs"},
		{"/mnt/f/whatever", true, "an unfamiliar type carrying the drvfs marker"},
		{"/media/ipod", false, "a real vfat mount is exactly what we want"},
		{"/media/ipod/Music", false, "and anything under it"},
		{"/home/brando/Projects/ipod_theme", false, "ordinary ext4"},
		{"/mnt/wsl", false, "tmpfs, not a Windows drive"},
		{"/", false, "the root itself"},
		// The longest matching mount point wins: /mnt/d must not be chosen
		// for a path that merely starts with those characters.
		{"/mnt/dave", false, "a sibling whose name starts like the mount point"},
	}
	for _, tc := range cases {
		err := refusesMount(tc.path, m)
		if tc.refuse && err == nil {
			t.Errorf("%s: accepted, want refused (%s)", tc.path, tc.why)
		}
		if !tc.refuse && err != nil {
			t.Errorf("%s: refused (%v), want accepted (%s)", tc.path, err, tc.why)
		}
	}
}

// The refusal has to tell the user what to do instead; "permission denied"
// style opacity is how somebody ends up copying a library into a stale mount
// for twenty minutes.
func TestRefusesMountMessage(t *testing.T) {
	err := refusesMount("/mnt/d/Music", mounts(t))
	if err == nil {
		t.Fatal("no error")
	}
	msg := err.Error()
	for _, want := range []string{"/mnt/d", "9p", "Windows", "D:"} {
		if !strings.Contains(msg, want) {
			t.Errorf("the message does not mention %q: %s", want, msg)
		}
	}
}

func TestRefusesMountEmptyTable(t *testing.T) {
	if err := refusesMount("/mnt/d", nil); err != nil {
		t.Errorf("with no mount table at all we must not block: %v", err)
	}
}

func TestParseMounts(t *testing.T) {
	// The older /proc/mounts shape, including the kernel's \040 escaping.
	m := parseMounts(strings.NewReader(
		"C:\\134 /mnt/c 9p rw,aname=drvfs 0 0\n" +
			"/dev/sdb1 /media/my\\040ipod vfat rw 0 0\n"))
	if len(m) != 2 {
		t.Fatalf("parsed %d rows, want 2", len(m))
	}
	if m[1].Point != "/media/my ipod" {
		t.Errorf("mount point = %q, want %q", m[1].Point, "/media/my ipod")
	}
	if err := refusesMount("/mnt/c/x", m); err == nil {
		t.Error("a 9p row from /proc/mounts was accepted")
	}
	if err := refusesMount("/media/my ipod/Music", m); err != nil {
		t.Errorf("a vfat row was refused: %v", err)
	}
}

// RefusesMount itself must not blow up, and must not refuse the ordinary
// temp directory the rest of these tests write into.
func TestRefusesMountRealTree(t *testing.T) {
	if err := RefusesMount(t.TempDir()); err != nil {
		t.Errorf("the test's own temp dir was refused: %v", err)
	}
}
