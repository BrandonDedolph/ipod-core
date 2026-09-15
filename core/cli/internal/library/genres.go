package library

import (
	_ "embed"
	"encoding/json"
	"fmt"
	"os"
	"strings"
)

// GenreMapEmbedded is the LoadGenreMap path that selects the copy of
// tools/artist_genres.json compiled into the binary. A shipped `core` has no
// repository beside it, and the first on-device sync through core.exe wrote
// an index with 919 empty genre fields because the default map was looked up
// by walking up from the executable to a tools/ folder that was not there.
// The embedded copy is held equal to the repo file by TestEmbeddedGenreMap.
const GenreMapEmbedded = "embedded"

//go:embed artist_genres.json
var embeddedGenreMap []byte

// LoadGenreMap reads the per-artist primary genre map (tools/artist_genres.json
// shape: either {"genres": {artist: genre}} or a bare {artist: genre} object).
// Keys starting with "_" are comments and are dropped. An empty path means "use
// tag genres only" and returns an empty map; GenreMapEmbedded selects the
// built-in copy.
//
// The key is the FOLDER-derived artist, matched exactly — album folders are
// named "Album - Artist", so the map must use the folder's spelling, not the
// tag's.
func LoadGenreMap(path string) (map[string]string, error) {
	if path == "" {
		return map[string]string{}, nil
	}
	if path == GenreMapEmbedded {
		return ParseGenreMap(embeddedGenreMap, "embedded artist_genres.json")
	}
	raw, err := os.ReadFile(path)
	if err != nil {
		return nil, err
	}
	return ParseGenreMap(raw, path)
}

// ParseGenreMap decodes the artist -> genre JSON; name is used in errors.
func ParseGenreMap(raw []byte, path string) (map[string]string, error) {
	var doc map[string]json.RawMessage
	if err := json.Unmarshal(raw, &doc); err != nil {
		return nil, fmt.Errorf("%s: expected an object of artist -> genre: %w", path, err)
	}
	if sub, ok := doc["genres"]; ok {
		doc = nil
		if err := json.Unmarshal(sub, &doc); err != nil {
			return nil, fmt.Errorf("%s: expected an object of artist -> genre: %w", path, err)
		}
	}
	out := make(map[string]string, len(doc))
	for k, v := range doc {
		if strings.HasPrefix(k, "_") {
			continue
		}
		var s string
		if err := json.Unmarshal(v, &s); err != nil {
			// build_index.py would happily carry a non-string here and only
			// fail later, inside struct.pack, with no mention of the file.
			return nil, fmt.Errorf("%s: genre for %q is not a string", path, k)
		}
		out[k] = s
	}
	return out, nil
}
