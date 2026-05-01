package ipod

import "testing"

func TestGenerationString(t *testing.T) {
	cases := []struct {
		g    Generation
		want string
	}{
		{Gen5_30, "5G (30 GB)"},
		{Gen5_5, "5.5G (60/80 GB)"},
		{GenUnknown, "unknown"},
	}
	for _, c := range cases {
		if got := c.g.String(); got != c.want {
			t.Errorf("Generation(%d).String() = %q, want %q", c.g, got, c.want)
		}
	}
}

func TestModeString(t *testing.T) {
	cases := []struct {
		m    Mode
		want string
	}{
		{ModeAppleOS, "apple-os"},
		{ModeAppleDisk, "apple-disk-mode"},
		{ModeOurUpdate, "core-update-mode"},
		{ModeRockbox, "rockbox"},
		{ModeUnknown, "unknown"},
	}
	for _, c := range cases {
		if got := c.m.String(); got != c.want {
			t.Errorf("Mode(%d).String() = %q, want %q", c.m, got, c.want)
		}
	}
}

func TestProductIDsDistinct(t *testing.T) {
	ids := []int{ProductIDIPodVideo30, ProductIDIPodVideo60, ProductIDIPodVideoDFU}
	seen := map[int]bool{}
	for _, id := range ids {
		if seen[id] {
			t.Errorf("duplicate product ID: %#x", id)
		}
		seen[id] = true
	}
}
