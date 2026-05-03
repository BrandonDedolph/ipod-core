package tagcache

import "bytes"

// stringTable accumulates null-terminated UTF-8 strings, deduplicating
// by content. The reserved offset 0 is the empty string ("\0"), so any
// caller that wants a no-op string (e.g. an absent field) can point at 0.
type stringTable struct {
	buf    bytes.Buffer
	offset map[string]uint32
}

func newStringTable() *stringTable {
	st := &stringTable{offset: make(map[string]uint32)}
	st.intern("") // reserve offset 0 = ""
	return st
}

// intern returns the byte offset of `s` within the table, adding it
// (with a null terminator) on first sight.
func (s *stringTable) intern(v string) uint32 {
	if off, ok := s.offset[v]; ok {
		return off
	}
	off := uint32(s.buf.Len())
	s.offset[v] = off
	s.buf.WriteString(v)
	s.buf.WriteByte(0)
	return off
}

func (s *stringTable) bytes() []byte { return s.buf.Bytes() }
