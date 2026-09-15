package disk

import (
	"bytes"
	"encoding/xml"
	"fmt"
	"io"
	"strconv"
)

// A just-enough XML property-list reader, for `diskutil -plist`.
//
// macOS has no cgo-free way to enumerate disks, and the one interface
// that is both stable and scriptable is diskutil's plist output. The
// alternatives were a third-party plist module (a dependency the plan
// forbids) or parsing diskutil's human output (which changes between
// releases and is localised). So: an xml.Decoder walk over the six
// value kinds diskutil actually emits — dict, array, string, integer,
// true, false — with <data> and <date> read as their text and <real>
// as a float. Anything else is skipped rather than failing, because a
// future diskutil adding a key must not break `core info`.
//
// This file has no build tag on purpose. It is pure parsing with no
// macOS in it, so it compiles and is tested on the machine this project
// is developed on, which is Linux.

// parsePlist returns the plist's root value: map[string]any for a
// <dict>, []any for an <array>, string/int64/float64/bool for a scalar.
func parsePlist(data []byte) (any, error) {
	dec := xml.NewDecoder(bytes.NewReader(data))
	// diskutil emits a DOCTYPE with an external DTD reference; Strict
	// parsing is fine with that, but not with the stray high-bit bytes
	// a volume name can contain.
	dec.Strict = false
	for {
		tok, err := dec.Token()
		if err == io.EOF {
			return nil, fmt.Errorf("plist: no <plist> element")
		}
		if err != nil {
			return nil, fmt.Errorf("plist: %w", err)
		}
		if se, ok := tok.(xml.StartElement); ok && se.Name.Local == "plist" {
			v, found, err := plistNextValue(dec)
			if err != nil {
				return nil, err
			}
			if !found {
				return nil, fmt.Errorf("plist: <plist> element is empty")
			}
			return v, nil
		}
	}
}

// plistNextValue reads the next value element. found=false means the
// enclosing element ended instead.
func plistNextValue(dec *xml.Decoder) (value any, found bool, err error) {
	for {
		tok, err := dec.Token()
		if err == io.EOF {
			return nil, false, nil
		}
		if err != nil {
			return nil, false, fmt.Errorf("plist: %w", err)
		}
		switch t := tok.(type) {
		case xml.StartElement:
			v, err := plistElement(dec, t)
			return v, true, err
		case xml.EndElement:
			return nil, false, nil
		}
	}
}

func plistElement(dec *xml.Decoder, start xml.StartElement) (any, error) {
	switch start.Name.Local {
	case "dict":
		m := map[string]any{}
		for {
			tok, err := dec.Token()
			if err == io.EOF {
				return m, nil
			}
			if err != nil {
				return nil, fmt.Errorf("plist: %w", err)
			}
			switch t := tok.(type) {
			case xml.EndElement:
				return m, nil
			case xml.StartElement:
				if t.Name.Local != "key" {
					// A value with no key before it is malformed;
					// skip it rather than losing the whole document.
					if err := dec.Skip(); err != nil {
						return nil, fmt.Errorf("plist: %w", err)
					}
					continue
				}
				key, err := plistText(dec)
				if err != nil {
					return nil, err
				}
				v, found, err := plistNextValue(dec)
				if err != nil {
					return nil, err
				}
				if !found {
					return m, nil
				}
				m[key] = v
			}
		}
	case "array":
		arr := []any{}
		for {
			v, found, err := plistNextValue(dec)
			if err != nil {
				return nil, err
			}
			if !found {
				return arr, nil
			}
			arr = append(arr, v)
		}
	case "string", "data", "date":
		return plistText(dec)
	case "integer":
		s, err := plistText(dec)
		if err != nil {
			return nil, err
		}
		n, err := strconv.ParseInt(s, 10, 64)
		if err != nil {
			return nil, fmt.Errorf("plist: <integer>%s</integer>: %w", s, err)
		}
		return n, nil
	case "real":
		s, err := plistText(dec)
		if err != nil {
			return nil, err
		}
		f, err := strconv.ParseFloat(s, 64)
		if err != nil {
			return nil, fmt.Errorf("plist: <real>%s</real>: %w", s, err)
		}
		return f, nil
	case "true":
		return true, dec.Skip()
	case "false":
		return false, dec.Skip()
	default:
		return nil, dec.Skip()
	}
}

// plistText accumulates character data up to the matching end tag.
func plistText(dec *xml.Decoder) (string, error) {
	var b bytes.Buffer
	for {
		tok, err := dec.Token()
		if err == io.EOF {
			return b.String(), nil
		}
		if err != nil {
			return "", fmt.Errorf("plist: %w", err)
		}
		switch t := tok.(type) {
		case xml.CharData:
			b.Write(t)
		case xml.EndElement:
			return b.String(), nil
		case xml.StartElement:
			if err := dec.Skip(); err != nil {
				return "", fmt.Errorf("plist: %w", err)
			}
		}
	}
}

// Typed accessors. Each returns the zero value when the key is absent
// or the wrong kind, because every one of these keys is documented only
// by what diskutil happened to print on one macOS version.

func plistDict(v any) map[string]any {
	m, _ := v.(map[string]any)
	return m
}

func plistArr(v any) []any {
	a, _ := v.([]any)
	return a
}

func plistStr(m map[string]any, key string) string {
	s, _ := m[key].(string)
	return s
}

func plistInt(m map[string]any, key string) int64 {
	switch v := m[key].(type) {
	case int64:
		return v
	case float64:
		return int64(v)
	}
	return 0
}

func plistBool(m map[string]any, key string) bool {
	b, _ := m[key].(bool)
	return b
}
