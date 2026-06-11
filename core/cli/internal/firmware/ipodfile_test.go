package firmware

import (
	"bytes"
	"encoding/binary"
	"errors"
	"io"
	"testing"
)

func TestWriteIPodFile_HeaderLayout(t *testing.T) {
	image := []byte{0x10, 0x20, 0x30}
	var buf bytes.Buffer
	if err := WriteIPodFile(&buf, ModelIPodVideo, ModelNameIPodVideo, image); err != nil {
		t.Fatalf("WriteIPodFile: %v", err)
	}

	out := buf.Bytes()
	if got, want := len(out), IPodFileHeaderSize+len(image); got != want {
		t.Fatalf("output length = %d, want %d", got, want)
	}

	// Bytes 0–3: BE32 checksum = 0x05 (model seed) + 0x10 + 0x20 + 0x30 = 0x65.
	if got := binary.BigEndian.Uint32(out[0:4]); got != 0x65 {
		t.Errorf("checksum = %#x, want %#x", got, 0x65)
	}
	// Bytes 4–7: model name.
	if got := string(out[4:8]); got != "ipvd" {
		t.Errorf("model name = %q, want %q", got, "ipvd")
	}
	// Bytes 8+: image bytes verbatim.
	if !bytes.Equal(out[IPodFileHeaderSize:], image) {
		t.Errorf("image bytes = %x, want %x", out[IPodFileHeaderSize:], image)
	}
}

func TestIPodFileRoundtrip(t *testing.T) {
	image := bytes.Repeat([]byte{0xAB}, 4096)
	var buf bytes.Buffer
	if err := WriteIPodFile(&buf, ModelIPodVideo, ModelNameIPodVideo, image); err != nil {
		t.Fatalf("WriteIPodFile: %v", err)
	}

	name, decoded, err := ReadIPodFile(&buf)
	if err != nil {
		t.Fatalf("ReadIPodFile: %v", err)
	}
	if name != ModelNameIPodVideo {
		t.Errorf("name = %v, want %v", name, ModelNameIPodVideo)
	}
	if !bytes.Equal(decoded, image) {
		t.Error("image bytes mismatch after roundtrip")
	}
}

func TestIPodFileRoundtrip_Nano(t *testing.T) {
	// A Nano image is written with the 0x04 seed; ReadIPodFile must
	// derive that same seed from the embedded "nano" name rather than
	// assuming Video's 0x05 (which would falsely report a mismatch).
	image := []byte{0x10, 0x20, 0x30}
	var buf bytes.Buffer
	if err := WriteIPodFile(&buf, ModelIPodNano, ModelNameIPodNano, image); err != nil {
		t.Fatalf("WriteIPodFile: %v", err)
	}

	// Sanity: the stored checksum really is seeded with 0x04.
	if got := binary.BigEndian.Uint32(buf.Bytes()[0:4]); got != 0x64 {
		t.Fatalf("stored checksum = %#x, want %#x (0x04 seed)", got, 0x64)
	}

	name, decoded, err := ReadIPodFile(&buf)
	if err != nil {
		t.Fatalf("ReadIPodFile: %v", err)
	}
	if name != ModelNameIPodNano {
		t.Errorf("name = %v, want %v", name, ModelNameIPodNano)
	}
	if !bytes.Equal(decoded, image) {
		t.Error("image bytes mismatch after roundtrip")
	}
}

func TestReadIPodFile_UnknownModelName(t *testing.T) {
	// A structurally valid file with a model name we have no seed for:
	// the name and bytes still come back so recovery tools can inspect,
	// but with ErrUnknownModelName since the checksum can't be verified.
	unknown := ModelName{'x', 'y', 'z', 'w'}
	image := []byte{0x10, 0x20, 0x30}
	var buf bytes.Buffer
	var hdr [IPodFileHeaderSize]byte
	binary.BigEndian.PutUint32(hdr[0:4], 0x65) // value irrelevant; never checked
	copy(hdr[4:8], unknown[:])
	buf.Write(hdr[:])
	buf.Write(image)

	name, decoded, err := ReadIPodFile(&buf)
	if !errors.Is(err, ErrUnknownModelName) {
		t.Errorf("err = %v, want ErrUnknownModelName", err)
	}
	if name != unknown {
		t.Errorf("name = %v, want %v", name, unknown)
	}
	if !bytes.Equal(decoded, image) {
		t.Error("image bytes should be returned even for an unknown model name")
	}
}

func TestModelNumForName(t *testing.T) {
	if num, ok := ModelNumForName(ModelNameIPodVideo); !ok || num != ModelIPodVideo {
		t.Errorf("ModelNumForName(ipvd) = %#x, %v; want %#x, true", uint32(num), ok, uint32(ModelIPodVideo))
	}
	if num, ok := ModelNumForName(ModelNameIPodNano); !ok || num != ModelIPodNano {
		t.Errorf("ModelNumForName(nano) = %#x, %v; want %#x, true", uint32(num), ok, uint32(ModelIPodNano))
	}
	if _, ok := ModelNumForName(ModelName{'?', '?', '?', '?'}); ok {
		t.Error("ModelNumForName(????) ok = true, want false")
	}
}

func TestReadIPodFile_ChecksumMismatch(t *testing.T) {
	// Hand-craft a file with a bad checksum.
	image := []byte{0x10, 0x20, 0x30}
	var buf bytes.Buffer
	var hdr [IPodFileHeaderSize]byte
	binary.BigEndian.PutUint32(hdr[0:4], 0xDEADBEEF) // wrong
	copy(hdr[4:8], ModelNameIPodVideo[:])
	buf.Write(hdr[:])
	buf.Write(image)

	name, decoded, err := ReadIPodFile(&buf)
	if !errors.Is(err, ErrIPodChecksumMismatch) {
		t.Errorf("err = %v, want ErrIPodChecksumMismatch", err)
	}
	// Bytes returned anyway so recovery tools can inspect.
	if !bytes.Equal(decoded, image) {
		t.Error("image bytes should be returned even on mismatch")
	}
	if name != ModelNameIPodVideo {
		t.Errorf("name = %v, want %v", name, ModelNameIPodVideo)
	}
}

func TestReadIPodFile_TruncatedHeader(t *testing.T) {
	// Only 4 bytes — short of the 8-byte header.
	_, _, err := ReadIPodFile(bytes.NewReader([]byte{0, 0, 0, 0}))
	if !errors.Is(err, ErrShortIPodFile) {
		t.Errorf("err = %v, want ErrShortIPodFile", err)
	}
}

func TestReadIPodFile_EmptyInput(t *testing.T) {
	_, _, err := ReadIPodFile(bytes.NewReader(nil))
	// io.ReadFull returns io.EOF when nothing was read; our wrapper treats
	// that as a short file.
	if !errors.Is(err, ErrShortIPodFile) && !errors.Is(err, io.EOF) {
		t.Errorf("err = %v, want ErrShortIPodFile or io.EOF", err)
	}
}

func TestWriteIPodFile_EmptyImage(t *testing.T) {
	// An image of zero bytes should still emit a valid 8-byte header
	// with checksum == model seed.
	var buf bytes.Buffer
	if err := WriteIPodFile(&buf, ModelIPodVideo, ModelNameIPodVideo, nil); err != nil {
		t.Fatalf("WriteIPodFile(empty): %v", err)
	}
	if got, want := buf.Len(), IPodFileHeaderSize; got != want {
		t.Fatalf("empty image emitted %d bytes, want %d", got, want)
	}
	if got := binary.BigEndian.Uint32(buf.Bytes()[0:4]); got != uint32(ModelIPodVideo) {
		t.Errorf("checksum on empty image = %#x, want %#x", got, uint32(ModelIPodVideo))
	}
}
