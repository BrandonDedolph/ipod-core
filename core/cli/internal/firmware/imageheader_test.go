package firmware

import (
	"bytes"
	"testing"
)

func TestDirectoryEntryRoundtrip(t *testing.T) {
	original := DirectoryEntry{
		ContainerID: [4]byte{'!', 'A', 'T', 'A'},
		ImageType:   [4]byte{'s', 'o', 's', 'o'}, // "OSOS"
		ImageID:     0xDEADBEEF,
		DevOffset:   0x10000,
		Length:      0x80000,
		LoadAddr:    0x10000000,
		EntryOffset: 0x40,
		Checksum:    0x12345678,
		Version:     2,
		LoadAddr2:   0x10000040,
	}

	var buf bytes.Buffer
	if err := WriteDirectoryEntry(&buf, original); err != nil {
		t.Fatalf("WriteDirectoryEntry: %v", err)
	}
	if got, want := buf.Len(), 40; got != want {
		t.Fatalf("encoded length = %d, want %d", got, want)
	}

	decoded, err := ReadDirectoryEntry(&buf)
	if err != nil {
		t.Fatalf("ReadDirectoryEntry: %v", err)
	}
	if decoded != original {
		t.Errorf("roundtrip mismatch:\n got %#v\nwant %#v", decoded, original)
	}
}

func TestDirectoryEntry_LogicalImageType(t *testing.T) {
	// On-disk "soso" reversed = "osos" (lowercase to match the on-disk bytes).
	e := DirectoryEntry{ImageType: [4]byte{'s', 'o', 's', 'o'}}
	if got := e.LogicalImageType(); got != "osos" {
		t.Errorf("LogicalImageType = %q, want %q", got, "osos")
	}
}

func TestDirectoryEntry_IsOSOS(t *testing.T) {
	osos := DirectoryEntry{ImageType: [4]byte{'s', 'o', 's', 'o'}}
	rsrc := DirectoryEntry{ImageType: [4]byte{'c', 'r', 's', 'r'}}
	if !osos.IsOSOS() {
		t.Error("OSOS entry should report IsOSOS()")
	}
	if rsrc.IsOSOS() {
		t.Error("RSRC entry should not report IsOSOS()")
	}
}

func TestDirectoryMarker(t *testing.T) {
	good := [4]byte{']', 'i', 'h', '['}
	bad := [4]byte{'h', 'i', '!', '!'}

	if err := CheckDirectoryMarker(good); err != nil {
		t.Errorf("CheckDirectoryMarker(good) returned %v", err)
	}
	if err := CheckDirectoryMarker(bad); err == nil {
		t.Error("CheckDirectoryMarker(bad) returned nil, want error")
	}
}

func TestReadDirectoryEntry_Truncated(t *testing.T) {
	// Only 20 bytes — too short.
	_, err := ReadDirectoryEntry(bytes.NewReader(make([]byte, 20)))
	if err == nil {
		t.Error("expected error on truncated read, got nil")
	}
}
