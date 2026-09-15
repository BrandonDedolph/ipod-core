// SPDX-License-Identifier: Apache-2.0

package devicefs

import (
	"encoding/binary"
	"encoding/hex"
	"hash/crc32"
	"os/exec"
	"path/filepath"
	"strings"
	"testing"
)

// The golden CORELOG.BIN header for a 4 MiB file (2048 blocks) and the fixed
// file id 0x0BADCAFE that tools/make_log.py's own fixture uses. Generated
// ONCE on 2026-09-14 from the tree at 928727d with
//
//	python3 -c "import importlib.util; \
//	  spec=importlib.util.spec_from_file_location('mk','tools/make_log.py'); \
//	  m=importlib.util.module_from_spec(spec); spec.loader.exec_module(m); \
//	  print(m.encode_header(2048, 0x0BADCAFE)[:24].hex())"
//
// Field by field: 434c4f47 magic 'CLOG', 0100 version 1, 0008 block size
// 2048, 00080000 block count 2048, fecaad0b file id, 0ec6b8d6 CRC-32 over
// bytes [0,16). Everything from byte 20 on is zero.
const logHeaderGoldenHead = "434c4f470100000800080000fecaad0b0ec6b8d6"

const logGoldenFileID = 0x0BADCAFE

func logHeaderGolden() string {
	return logHeaderGoldenHead + strings.Repeat("00", LogBlockBytes-len(logHeaderGoldenHead)/2)
}

func TestEncodeLogHeaderMatchesGolden(t *testing.T) {
	blk := EncodeLogHeader(LogDefaultBytes/LogBlockBytes, logGoldenFileID)
	if got, want := hex.EncodeToString(blk[:]), logHeaderGolden(); got != want {
		t.Errorf("log header mismatch\n got %s\nwant %s", firstDiff(got, want), firstDiff(want, got))
	}
}

// The reference implementation, re-run. tools/make_log.py's --emit writes a
// whole synthesized fixture rather than a bare header, so the header encoder
// is imported and called directly.
func TestEncodeLogHeaderMatchesMakeLog(t *testing.T) {
	repo := repoRoot(t)
	if repo == "" {
		t.Skip("not inside the ipod_theme tree (set CORE_REPO)")
	}
	py := python3(t)
	if py == "" {
		t.Skip("python3 not on PATH")
	}
	script := `
import importlib.util, sys
spec = importlib.util.spec_from_file_location("mk", sys.argv[1])
m = importlib.util.module_from_spec(spec)
spec.loader.exec_module(m)
print(m.encode_header(int(sys.argv[2]), int(sys.argv[3], 16)).hex())
`
	out, err := exec.Command(py, "-c", script,
		filepath.Join(repo, "tools", "make_log.py"),
		"2048", "0BADCAFE").CombinedOutput()
	if err != nil {
		t.Fatalf("make_log.encode_header: %v\n%s", err, out)
	}
	want := strings.TrimSpace(string(out))
	blk := EncodeLogHeader(2048, logGoldenFileID)
	if got := hex.EncodeToString(blk[:]); got != want {
		t.Errorf("header differs from make_log.py: %s", firstDiff(got, want))
	}
	if want != logHeaderGolden() {
		t.Errorf("the embedded golden is stale: make_log.py now emits %s",
			firstDiff(want, logHeaderGolden()))
	}
}

func TestDecodeLogHeader(t *testing.T) {
	blk := EncodeLogHeader(2048, 0x12345678)
	n, id, ok := DecodeLogHeader(blk[:])
	if !ok || n != 2048 || id != 0x12345678 {
		t.Fatalf("round trip: count=%d id=%#x ok=%v", n, id, ok)
	}

	reseal := func(b *[LogBlockBytes]byte) {
		binary.LittleEndian.PutUint32(b[logOffCRC:], crc32.ChecksumIEEE(b[:logCRCBytes]))
	}
	cases := []struct {
		name string
		mut  func(*[LogBlockBytes]byte)
	}{
		{"bad magic", func(b *[LogBlockBytes]byte) { b[0] ^= 0xFF; reseal(b) }},
		{"wrong version", func(b *[LogBlockBytes]byte) {
			binary.LittleEndian.PutUint16(b[logOffVersion:], LogVersion+1)
			reseal(b)
		}},
		{"wrong block size", func(b *[LogBlockBytes]byte) {
			binary.LittleEndian.PutUint16(b[logOffBSize:], 512)
			reseal(b)
		}},
		{"too few blocks", func(b *[LogBlockBytes]byte) {
			binary.LittleEndian.PutUint32(b[logOffCount:], LogMinBlocks-1)
			reseal(b)
		}},
		{"too many blocks", func(b *[LogBlockBytes]byte) {
			binary.LittleEndian.PutUint32(b[logOffCount:], LogMaxBlocks+1)
			reseal(b)
		}},
		{"bad crc", func(b *[LogBlockBytes]byte) { b[logOffCRC] ^= 0x01 }},
		{"all zero", func(b *[LogBlockBytes]byte) { *b = [LogBlockBytes]byte{} }},
	}
	for _, tc := range cases {
		t.Run(tc.name, func(t *testing.T) {
			h := EncodeLogHeader(2048, 1)
			tc.mut(&h)
			if _, _, ok := DecodeLogHeader(h[:]); ok {
				t.Error("accepted a header the firmware would reject")
			}
		})
	}
	if _, _, ok := DecodeLogHeader(make([]byte, LogBlockBytes-1)); ok {
		t.Error("accepted a short block")
	}
}
