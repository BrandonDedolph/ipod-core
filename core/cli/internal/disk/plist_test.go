package disk

import "testing"

// A trimmed but real-shaped `diskutil list -plist` reply, with the
// nesting that matters: WholeDisks as a flat array of strings and
// AllDisksAndPartitions as dicts containing arrays of dicts.
const diskutilListPlist = `<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
	<key>AllDisks</key>
	<array>
		<string>disk0</string>
		<string>disk4</string>
		<string>disk4s1</string>
	</array>
	<key>AllDisksAndPartitions</key>
	<array>
		<dict>
			<key>DeviceIdentifier</key><string>disk4</string>
			<key>Size</key><integer>80026361856</integer>
			<key>Partitions</key>
			<array>
				<dict>
					<key>Content</key><string>DOS_FAT_32</string>
					<key>DeviceIdentifier</key><string>disk4s1</string>
					<key>MountPoint</key><string>/Volumes/IPOD</string>
					<key>Size</key><integer>79894364160</integer>
				</dict>
			</array>
		</dict>
	</array>
	<key>WholeDisks</key>
	<array>
		<string>disk0</string>
		<string>disk4</string>
	</array>
</dict>
</plist>
`

const diskutilInfoPlist = `<?xml version="1.0" encoding="UTF-8"?>
<plist version="1.0">
<dict>
	<key>BusProtocol</key><string>USB</string>
	<key>DeviceBlockSize</key><integer>2048</integer>
	<key>DeviceIdentifier</key><string>disk4</string>
	<key>DeviceNode</key><string>/dev/disk4</string>
	<key>Ejectable</key><true/>
	<key>Internal</key><false/>
	<key>MediaName</key><string>Apple iPod Media</string>
	<key>Size</key><integer>80026361856</integer>
	<key>SolidState</key><false/>
	<key>VolumeName</key><string></string>
</dict>
</plist>
`

func TestParsePlistDiskutilList(t *testing.T) {
	v, err := parsePlist([]byte(diskutilListPlist))
	if err != nil {
		t.Fatalf("parsePlist: %v", err)
	}
	top := plistDict(v)
	if top == nil {
		t.Fatal("the root is not a dict")
	}

	whole := plistArr(top["WholeDisks"])
	if len(whole) != 2 || whole[0] != "disk0" || whole[1] != "disk4" {
		t.Errorf("WholeDisks = %v, want [disk0 disk4]", whole)
	}

	all := plistArr(top["AllDisksAndPartitions"])
	if len(all) != 1 {
		t.Fatalf("AllDisksAndPartitions has %d entries, want 1", len(all))
	}
	d := plistDict(all[0])
	if got := plistStr(d, "DeviceIdentifier"); got != "disk4" {
		t.Errorf("DeviceIdentifier = %q", got)
	}
	if got := plistInt(d, "Size"); got != 80026361856 {
		t.Errorf("Size = %d, want 80026361856", got)
	}
	parts := plistArr(d["Partitions"])
	if len(parts) != 1 {
		t.Fatalf("Partitions has %d entries, want 1", len(parts))
	}
	p := plistDict(parts[0])
	if got := plistStr(p, "DeviceIdentifier"); got != "disk4s1" {
		t.Errorf("partition DeviceIdentifier = %q", got)
	}
	if got := plistStr(p, "MountPoint"); got != "/Volumes/IPOD" {
		t.Errorf("MountPoint = %q", got)
	}
}

func TestParsePlistDiskutilInfo(t *testing.T) {
	v, err := parsePlist([]byte(diskutilInfoPlist))
	if err != nil {
		t.Fatalf("parsePlist: %v", err)
	}
	m := plistDict(v)
	if got := plistInt(m, "DeviceBlockSize"); got != 2048 {
		t.Errorf("DeviceBlockSize = %d, want 2048", got)
	}
	if got := plistStr(m, "MediaName"); got != "Apple iPod Media" {
		t.Errorf("MediaName = %q", got)
	}
	if !plistBool(m, "Ejectable") {
		t.Error("Ejectable did not read as true")
	}
	if plistBool(m, "Internal") {
		t.Error("Internal did not read as false")
	}
	if plistBool(m, "NoSuchKey") {
		t.Error("an absent key read as true")
	}
	if got := plistStr(m, "VolumeName"); got != "" {
		t.Errorf("an empty <string/> read as %q", got)
	}
}

// TestParsePlistTolerance: a key diskutil adds in a future macOS must
// not break the parse, and neither must a value kind we do not model.
func TestParsePlistTolerance(t *testing.T) {
	src := `<plist version="1.0"><dict>
	<key>Known</key><string>yes</string>
	<key>Real</key><real>1.5</real>
	<key>Data</key><data>AAEC</data>
	<key>Nested</key><dict><key>Deep</key><array><integer>1</integer><integer>2</integer></array></dict>
	</dict></plist>`
	v, err := parsePlist([]byte(src))
	if err != nil {
		t.Fatalf("parsePlist: %v", err)
	}
	m := plistDict(v)
	if plistStr(m, "Known") != "yes" {
		t.Error("a known key was lost next to unmodelled ones")
	}
	if f, ok := m["Real"].(float64); !ok || f != 1.5 {
		t.Errorf("Real = %v", m["Real"])
	}
	if got := plistInt(m, "Real"); got != 1 {
		t.Errorf("plistInt on a <real> = %d, want 1", got)
	}
	inner := plistDict(m["Nested"])
	if len(plistArr(inner["Deep"])) != 2 {
		t.Errorf("a nested array did not survive: %v", inner)
	}
}

func TestParsePlistErrors(t *testing.T) {
	if _, err := parsePlist([]byte(`<html><body>not a plist</body></html>`)); err == nil {
		t.Error("parsePlist accepted a document with no <plist> element")
	}
	if _, err := parsePlist([]byte(`<plist version="1.0"></plist>`)); err == nil {
		t.Error("parsePlist accepted an empty <plist>")
	}
	if _, err := parsePlist([]byte(`<plist><dict><key>n</key><integer>x</integer></dict></plist>`)); err == nil {
		t.Error("parsePlist accepted a non-numeric <integer>")
	}
}
