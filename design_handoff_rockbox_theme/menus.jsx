// Additional menu screens — all in the Linen system at native 320x240
// Lists support a scrolling viewport: when selection goes past the visible
// window, the list scrolls and a slim scrollbar appears on the right.

const ListBg = "#f4f1ec";
const ListFg = "#1a1714";
const ListMuted = "#9a8e80";
const ListMutedDeep = "#5a5048";
const ListSelBg = "#1a1714";
const ListSelFg = "#f4f1ec";
const ListBorder = "rgba(26,23,20,0.08)";

const screenWrap = (extra = {}) => ({
  width: 320, height: 240, background: ListBg,
  fontFamily: "'Nunito', system-ui, sans-serif", color: ListFg,
  position: "relative", overflow: "hidden",
  WebkitFontSmoothing: "antialiased",
  ...extra,
});

function StatusStrip({ time = "10:42 AM", battery = 0.78, charging = false, hold = false, color = ListMuted }) {
  return (
    <div style={{
      padding: "5px 12px 0",
      display: "flex", justifyContent: "space-between", alignItems: "center",
      fontSize: 9, fontWeight: 700, letterSpacing: 0.5,
      color, textTransform: "uppercase", fontVariantNumeric: "tabular-nums",
    }}>
      <span>{time}</span>
      <span style={{ display: "flex", alignItems: "center", gap: 6 }}>
        {hold && (
          <svg width="8" height="10" viewBox="0 0 9 11" fill="none" style={{ display: "block" }}>
            <rect x="0.5" y="4.5" width="8" height="6" rx="1" fill={color} />
            <path d="M2.2 4.5V3a2.3 2.3 0 1 1 4.6 0v1.5" stroke={color} strokeWidth="1.1" fill="none" />
          </svg>
        )}
        <Battery level={battery} color={color} charging={charging} />
      </span>
    </div>
  );
}

function ScreenHeader({ title, right, onBack = true }) {
  return (
    <div style={{
      padding: "9px 14px 8px",
      borderBottom: `1px solid ${ListBorder}`,
      display: "flex", justifyContent: "space-between", alignItems: "center",
      gap: 8,
    }}>
      <div style={{ display: "flex", alignItems: "center", gap: 6, minWidth: 0 }}>
        {onBack && <span style={{ fontSize: 14, fontWeight: 700, color: ListMuted, lineHeight: 1 }}>‹</span>}
        <span style={{ fontSize: 12.5, fontWeight: 700, letterSpacing: -0.1, whiteSpace: "nowrap", overflow: "hidden", textOverflow: "ellipsis" }}>{title}</span>
      </div>
      <span style={{ fontSize: 10, fontWeight: 600, color: ListMuted, flexShrink: 0 }}>{right}</span>
    </div>
  );
}

// Compute the scroll window so selection stays visible.
// Centered approach: keep ~1-2 items of context above selection when possible.
// Pure derivation - no state, fully deterministic from selectedIdx.
function useScrollWindow(selectedIdx, total, visible) {
  if (total <= visible) return 0;
  // Try to keep selection roughly 1/3 from the top, with edge clamping
  const offset = Math.floor(visible / 3);
  let start = selectedIdx - offset;
  start = Math.max(0, start);
  start = Math.min(start, total - visible);
  return start;
}

// Slim scrollbar on the right edge
function Scrollbar({ start, visible, total }) {
  if (total <= visible) return null;
  const trackH = 200; // ~screen height minus header
  const thumbH = Math.max(18, (visible / total) * trackH);
  const thumbY = (start / Math.max(1, total - visible)) * (trackH - thumbH);
  return (
    <div style={{
      position: "absolute", right: 2, top: 36, width: 3, height: trackH,
      background: "rgba(26,23,20,0.06)", borderRadius: 2,
    }}>
      <div style={{
        position: "absolute", left: 0, right: 0, top: thumbY, height: thumbH,
        background: "rgba(26,23,20,0.35)", borderRadius: 2,
      }} />
    </div>
  );
}

// Generic windowed list. Renders only items in the visible window.
function ScrollList({ items, selectedIdx, visible = 9, renderItem }) {
  const start = useScrollWindow(selectedIdx, items.length, visible);
  const slice = items.slice(start, start + visible);
  return (
    <>
      <div style={{ padding: "4px 0" }}>
        {slice.map((item, i) => renderItem(item, start + i, (start + i) === selectedIdx))}
      </div>
      <Scrollbar start={start} visible={visible} total={items.length} />
    </>
  );
}

function Row({ children, sel, sub, right, leading, onChevron = true, dense = false }) {
  return (
    <div style={{
      padding: dense ? "5px 12px" : "7px 12px",
      margin: "0 6px 0 6px",
      marginRight: 10,
      borderRadius: 4,
      background: sel ? ListSelBg : "transparent",
      color: sel ? ListSelFg : ListFg,
      display: "flex", alignItems: "center", gap: 9,
      fontSize: 12.5,
      fontWeight: sel ? 700 : 500,
      letterSpacing: -0.05,
    }}>
      {leading}
      <div style={{ flex: 1, minWidth: 0 }}>
        <div style={{ whiteSpace: "nowrap", overflow: "hidden", textOverflow: "ellipsis" }}>{children}</div>
        {sub && <div style={{ fontSize: 10, fontWeight: 500, marginTop: 1, color: sel ? "rgba(244,241,236,0.7)" : ListMuted, whiteSpace: "nowrap", overflow: "hidden", textOverflow: "ellipsis" }}>{sub}</div>}
      </div>
      {right && <span style={{ fontSize: 10, fontWeight: 600, color: sel ? "rgba(244,241,236,0.75)" : ListMuted, fontVariantNumeric: "tabular-nums" }}>{right}</span>}
      {onChevron && !right && <span style={{ fontSize: 11, color: sel ? "rgba(244,241,236,0.6)" : "rgba(26,23,20,0.3)" }}>›</span>}
    </div>
  );
}

function Chip({ hue = 30 }) {
  return (
    <div style={{
      width: 22, height: 22, borderRadius: 2,
      background: `repeating-linear-gradient(135deg, oklch(0.78 0.04 ${hue}) 0 4px, oklch(0.74 0.04 ${hue}) 4px 8px)`,
      flexShrink: 0,
    }} />
  );
}

// =====================================================
// MUSIC SUB-MENU
// =====================================================
const MUSIC_ITEMS = ["Playlists", "Artists", "Albums", "Songs", "Genres", "Composers", "Audiobooks"];
function MusicMenu({ selectedIdx = 1 }) {
  return (
    <div style={screenWrap()}>
      <ScreenHeader title="Music" right="" />
      <ScrollList items={MUSIC_ITEMS} selectedIdx={selectedIdx} visible={9}
        renderItem={(label, i, sel) => <Row key={label} sel={sel}>{label}</Row>} />
    </div>
  );
}

// =====================================================
// ARTISTS — long alphabetical list
// =====================================================
const ARTISTS = [
  "Aphex Twin","Beach House","Boards of Canada","Bon Iver","Brian Eno",
  "Caribou","Cocteau Twins","DJ Shadow","Four Tet","Fleet Foxes",
  "Grouper","Jon Hopkins","Khruangbin","Mount Kimbie","My Bloody Valentine",
  "Nicolas Jaar","Nils Frahm","Oneohtrix Point Never","Radiohead","Sigur Rós",
  "Slowdive","The National","Thom Yorke","Tycho","Warpaint","William Basinski",
];
function ArtistsList({ selectedIdx = 0 }) {
  return (
    <div style={screenWrap()}>
      <ScreenHeader title="Artists" right={`${selectedIdx + 1} / ${ARTISTS.length}`} />
      <ScrollList items={ARTISTS} selectedIdx={selectedIdx} visible={11}
        renderItem={(a, i, sel) => <Row key={a} sel={sel} dense>{a}</Row>} />
    </div>
  );
}

// =====================================================
// ALBUMS
// =====================================================
const ALBUMS = [
  { t: "Drukqs", a: "Aphex Twin", h: 30 },
  { t: "Selected Ambient Works 85–92", a: "Aphex Twin", h: 50 },
  { t: "Bloom", a: "Beach House", h: 220 },
  { t: "Music Has the Right to…", a: "Boards of Canada", h: 90 },
  { t: "Music for Airports", a: "Brian Eno", h: 200 },
  { t: "Andorra", a: "Caribou", h: 60 },
  { t: "Treasure", a: "Cocteau Twins", h: 320 },
  { t: "Endtroducing…..", a: "DJ Shadow", h: 10 },
  { t: "Rounds", a: "Four Tet", h: 130 },
  { t: "Helplessness Blues", a: "Fleet Foxes", h: 80 },
  { t: "Dragging a Dead Deer", a: "Grouper", h: 280 },
  { t: "Immunity", a: "Jon Hopkins", h: 250 },
  { t: "Mordechai", a: "Khruangbin", h: 40 },
];
function AlbumsList({ selectedIdx = 0 }) {
  return (
    <div style={screenWrap()}>
      <ScreenHeader title="Albums" right={`${selectedIdx + 1} / ${ALBUMS.length}`} />
      <ScrollList items={ALBUMS} selectedIdx={selectedIdx} visible={8}
        renderItem={(al, i, sel) => <Row key={al.t} sel={sel} dense leading={<Chip hue={al.h} />} sub={al.a}>{al.t}</Row>} />
    </div>
  );
}

// =====================================================
// SONGS
// =====================================================
const SONGS = [
  { t: "Avril 14th", a: "Aphex Twin", d: "2:04" },
  { t: "Bbydhyonchord", a: "Aphex Twin", d: "3:20" },
  { t: "Beachcoma", a: "Caribou", d: "5:18" },
  { t: "Cherry-Coloured Funk", a: "Cocteau Twins", d: "3:12" },
  { t: "Dayvan Cowboy", a: "Boards of Canada", d: "5:00" },
  { t: "Gwely Mernans", a: "Aphex Twin", d: "5:50" },
  { t: "Heliosphan", a: "Aphex Twin", d: "4:51" },
  { t: "Holocene", a: "Bon Iver", d: "5:36" },
  { t: "Immunity", a: "Jon Hopkins", d: "9:36" },
  { t: "Lemonworld", a: "The National", d: "4:18" },
  { t: "Open Eye Signal", a: "Jon Hopkins", d: "7:55" },
  { t: "Punisher", a: "Phoebe Bridgers", d: "3:26" },
  { t: "Re: Stacks", a: "Bon Iver", d: "6:41" },
  { t: "Sleep on the Wing", a: "Bon Iver", d: "2:35" },
  { t: "Vordhosbn", a: "Aphex Twin", d: "4:53" },
];
function SongsList({ selectedIdx = 0 }) {
  return (
    <div style={screenWrap()}>
      <ScreenHeader title="Songs" right={`${selectedIdx + 1} / 1,284`} />
      <ScrollList items={SONGS} selectedIdx={selectedIdx} visible={11}
        renderItem={(s, i, sel) => <Row key={s.t} sel={sel} dense sub={s.a} right={s.d} onChevron={false}>{s.t}</Row>} />
    </div>
  );
}

// =====================================================
// GENRES
// =====================================================
const GENRES = [
  { t: "Ambient", c: 184 },{ t: "Classical", c: 92 },{ t: "Electronic", c: 312 },
  { t: "Folk", c: 68 },{ t: "Hip-Hop", c: 142 },{ t: "Indie", c: 224 },
  { t: "Jazz", c: 86 },{ t: "Soundtrack", c: 54 },{ t: "World", c: 38 },
];
function GenresList({ selectedIdx = 0 }) {
  return (
    <div style={screenWrap()}>
      <ScreenHeader title="Genres" right={`${GENRES.length} genres`} />
      <ScrollList items={GENRES} selectedIdx={selectedIdx} visible={9}
        renderItem={(g, i, sel) => <Row key={g.t} sel={sel} dense right={`${g.c}`}>{g.t}</Row>} />
    </div>
  );
}

// =====================================================
// PLAYLISTS
// =====================================================
const PLAYLISTS = [
  { t: "Recently Added", c: 47, sub: "Updated yesterday" },
  { t: "Late Nights", c: 32, sub: "Mix · Ambient" },
  { t: "Long Drives", c: 84, sub: "Mix · Indie / Folk" },
  { t: "Focus", c: 26, sub: "Smart playlist" },
  { t: "On-the-Go 1", c: 12, sub: "Created on iPod" },
  { t: "90 Minute Mix", c: 21, sub: "Last played 3d ago" },
  { t: "Morning", c: 18, sub: "Mix · Acoustic" },
  { t: "Workout", c: 42, sub: "Mix · Electronic" },
  { t: "Recently Played", c: 25, sub: "Smart playlist" },
];
function PlaylistsList({ selectedIdx = 0 }) {
  return (
    <div style={screenWrap()}>
      <ScreenHeader title="Playlists" right={`${PLAYLISTS.length} lists`} />
      <ScrollList items={PLAYLISTS} selectedIdx={selectedIdx} visible={7}
        renderItem={(p, i, sel) => <Row key={p.t} sel={sel} sub={p.sub} right={`${p.c}`}>{p.t}</Row>} />
    </div>
  );
}

// =====================================================
// PODCASTS
// =====================================================
const PODCASTS = [
  { t: "The Daily", a: "The New York Times", n: 3, h: 20 },
  { t: "Song Exploder", a: "Hrishikesh Hirway", n: 1, h: 350 },
  { t: "99% Invisible", a: "Roman Mars", n: 0, h: 60 },
  { t: "Radiolab", a: "WNYC Studios", n: 2, h: 200 },
  { t: "Reply All", a: "Gimlet", n: 0, h: 280 },
  { t: "Dithering", a: "Ben Thompson & John Gruber", n: 5, h: 100 },
  { t: "Hidden Brain", a: "Shankar Vedantam", n: 1, h: 160 },
  { t: "Planet Money", a: "NPR", n: 0, h: 130 },
];
function PodcastsList({ selectedIdx = 0 }) {
  const total = PODCASTS.reduce((s, p) => s + p.n, 0);
  return (
    <div style={screenWrap()}>
      <ScreenHeader title="Podcasts" right={`${total} unplayed`} />
      <ScrollList items={PODCASTS} selectedIdx={selectedIdx} visible={7}
        renderItem={(p, i, sel) => (
          <div key={p.t} style={{
            padding: "5px 12px",
            margin: "0 10px 0 6px",
            borderRadius: 4,
            background: sel ? ListSelBg : "transparent",
            color: sel ? ListSelFg : ListFg,
            display: "flex", alignItems: "center", gap: 9,
            fontSize: 12,
            fontWeight: sel ? 700 : 500,
          }}>
            <Chip hue={p.h} />
            <div style={{ flex: 1, minWidth: 0 }}>
              <div style={{ whiteSpace: "nowrap", overflow: "hidden", textOverflow: "ellipsis" }}>{p.t}</div>
              <div style={{ fontSize: 10, fontWeight: 500, marginTop: 1, color: sel ? "rgba(244,241,236,0.7)" : ListMuted, whiteSpace: "nowrap", overflow: "hidden", textOverflow: "ellipsis" }}>{p.a}</div>
            </div>
            {p.n > 0 && (
              <span style={{
                fontSize: 9.5, fontWeight: 800,
                background: sel ? "rgba(244,241,236,0.18)" : "#1a1714",
                color: "#f4f1ec",
                borderRadius: 8, padding: "2px 6px",
                fontVariantNumeric: "tabular-nums",
              }}>{p.n}</span>
            )}
          </div>
        )} />
    </div>
  );
}

// =====================================================
// PODCAST EPISODES
// =====================================================
const PODCAST_EPISODES = [
  { t: "When the Tide Recedes", d: "Apr 24 · 38 min", played: false },
  { t: "Inside the Forecast", d: "Apr 22 · 42 min", played: false },
  { t: "A Brief History of Public Radio", d: "Apr 19 · 51 min", played: false },
  { t: "What We Owe the Future", d: "Apr 17 · 35 min", played: true },
  { t: "Bonus: Live from Chicago", d: "Apr 15 · 1h 12m", played: true },
  { t: "The Long Now", d: "Apr 12 · 44 min", played: true },
  { t: "Listening to Light", d: "Apr 10 · 36 min", played: true },
  { t: "Voices in the Garden", d: "Apr 7 · 41 min", played: true },
];
function PodcastEpisodes({ selectedIdx = 0 }) {
  return (
    <div style={screenWrap()}>
      <ScreenHeader title="Song Exploder" right="" />
      <div style={{ padding: "8px 14px 4px", display: "flex", gap: 10, alignItems: "center" }}>
        <Chip hue={350} />
        <div style={{ flex: 1, minWidth: 0 }}>
          <div style={{ fontSize: 12, fontWeight: 700 }}>Hrishikesh Hirway</div>
          <div style={{ fontSize: 10, fontWeight: 500, color: ListMuted, marginTop: 1 }}>284 episodes · 1 unplayed</div>
        </div>
      </div>
      <div style={{ padding: "2px 0", marginTop: 4, position: "relative" }}>
        <ScrollListInner items={PODCAST_EPISODES} selectedIdx={selectedIdx} visible={7} headerOffset={88}
          renderItem={(e, i, sel) => (
            <div key={e.t} style={{
              padding: "5px 12px",
              margin: "0 10px 0 6px",
              borderRadius: 4,
              background: sel ? ListSelBg : "transparent",
              color: sel ? ListSelFg : ListFg,
              display: "flex", alignItems: "center", gap: 9,
              fontSize: 11.5,
              fontWeight: sel ? 700 : 500,
            }}>
              <span style={{
                width: 6, height: 6, borderRadius: "50%",
                background: e.played ? "transparent" : (sel ? "#f4f1ec" : "#1a1714"),
                border: e.played ? `1px solid ${sel ? "rgba(244,241,236,0.4)" : "rgba(26,23,20,0.25)"}` : "none",
                flexShrink: 0,
              }} />
              <div style={{ flex: 1, minWidth: 0 }}>
                <div style={{ whiteSpace: "nowrap", overflow: "hidden", textOverflow: "ellipsis" }}>{e.t}</div>
                <div style={{ fontSize: 9.5, fontWeight: 500, marginTop: 1, color: sel ? "rgba(244,241,236,0.7)" : ListMuted }}>{e.d}</div>
              </div>
            </div>
          )} />
      </div>
    </div>
  );
}

// Variant of ScrollList that lets us position scrollbar with custom top offset
function ScrollListInner({ items, selectedIdx, visible, renderItem, headerOffset = 36 }) {
  const start = useScrollWindow(selectedIdx, items.length, visible);
  const slice = items.slice(start, start + visible);
  if (items.length <= visible) {
    return <>{slice.map((item, i) => renderItem(item, start + i, (start + i) === selectedIdx))}</>;
  }
  const trackH = 240 - headerOffset - 4;
  const thumbH = Math.max(18, (visible / items.length) * trackH);
  const thumbY = (start / Math.max(1, items.length - visible)) * (trackH - thumbH);
  return (
    <>
      {slice.map((item, i) => renderItem(item, start + i, (start + i) === selectedIdx))}
      <div style={{
        position: "absolute", right: 2, top: headerOffset, width: 3, height: trackH,
        background: "rgba(26,23,20,0.06)", borderRadius: 2,
      }}>
        <div style={{
          position: "absolute", left: 0, right: 0, top: thumbY, height: thumbH,
          background: "rgba(26,23,20,0.35)", borderRadius: 2,
        }} />
      </div>
    </>
  );
}

// =====================================================
// AUDIOBOOKS
// =====================================================
const AUDIOBOOKS = [
  { t: "Project Hail Mary", a: "Andy Weir", p: 0.42, h: 30 },
  { t: "The Overstory", a: "Richard Powers", p: 0.78, h: 90 },
  { t: "Annihilation", a: "Jeff VanderMeer", p: 1, h: 170 },
  { t: "Piranesi", a: "Susanna Clarke", p: 0.12, h: 240 },
  { t: "A Gentleman in Moscow", a: "Amor Towles", p: 0, h: 60 },
  { t: "Klara and the Sun", a: "Kazuo Ishiguro", p: 0.55, h: 120 },
  { t: "The Bee Sting", a: "Paul Murray", p: 0.08, h: 200 },
];
function AudiobooksList({ selectedIdx = 0 }) {
  return (
    <div style={screenWrap()}>
      <ScreenHeader title="Audiobooks" right={`${AUDIOBOOKS.length} books`} />
      <ScrollList items={AUDIOBOOKS} selectedIdx={selectedIdx} visible={6}
        renderItem={(b, i, sel) => (
          <div key={b.t} style={{
            padding: "6px 12px",
            margin: "0 10px 0 6px",
            borderRadius: 4,
            background: sel ? ListSelBg : "transparent",
            color: sel ? ListSelFg : ListFg,
            display: "flex", alignItems: "center", gap: 9,
            fontSize: 12,
            fontWeight: sel ? 700 : 500,
          }}>
            <Chip hue={b.h} />
            <div style={{ flex: 1, minWidth: 0 }}>
              <div style={{ whiteSpace: "nowrap", overflow: "hidden", textOverflow: "ellipsis" }}>{b.t}</div>
              <div style={{ fontSize: 10, fontWeight: 500, marginTop: 1, color: sel ? "rgba(244,241,236,0.7)" : ListMuted, whiteSpace: "nowrap", overflow: "hidden", textOverflow: "ellipsis" }}>{b.a}</div>
              <div style={{ marginTop: 4, height: 2, borderRadius: 1, background: sel ? "rgba(244,241,236,0.2)" : "rgba(26,23,20,0.1)", overflow: "hidden" }}>
                <div style={{ width: `${b.p * 100}%`, height: "100%", background: sel ? "#f4f1ec" : "#1a1714" }} />
              </div>
            </div>
            <span style={{ fontSize: 9.5, fontWeight: 600, color: sel ? "rgba(244,241,236,0.7)" : ListMuted, fontVariantNumeric: "tabular-nums", flexShrink: 0 }}>
              {b.p === 1 ? "✓" : `${Math.round(b.p * 100)}%`}
            </span>
          </div>
        )} />
    </div>
  );
}

// =====================================================
// SETTINGS
// =====================================================
const SETTINGS_ITEMS = [
  { t: "Playback", v: null },{ t: "Sound", v: null },{ t: "Theme", v: "Linen" },
  { t: "Display", v: null },{ t: "Shortcuts", v: null },{ t: "Language", v: "English" },
  { t: "About", v: null },{ t: "Reset Settings", v: null },
];
function SettingsMenu({ selectedIdx = 0 }) {
  return (
    <div style={screenWrap()}>
      <ScreenHeader title="Settings" right="" />
      <ScrollList items={SETTINGS_ITEMS} selectedIdx={selectedIdx} visible={9}
        renderItem={(s, i, sel) => <Row key={s.t} sel={sel} dense right={s.v} onChevron={!s.v}>{s.t}</Row>} />
    </div>
  );
}

const PLAYBACK_ITEMS = [
  { t: "Shuffle", v: "Off", type: "select" },
  { t: "Repeat", v: "All", type: "select" },
  { t: "Crossfade", v: true, type: "toggle" },
  { t: "Crossfade Length", v: "4 sec", type: "select" },
  { t: "Replaygain", v: "Album", type: "select" },
  { t: "Skip Length", v: "10 sec", type: "select" },
  { t: "Resume on Startup", v: true, type: "toggle" },
];
function SettingsPlayback({ selectedIdx = 1 }) {
  return (
    <div style={screenWrap()}>
      <ScreenHeader title="Playback" right="" />
      <ScrollList items={PLAYBACK_ITEMS} selectedIdx={selectedIdx} visible={9}
        renderItem={(s, i, sel) => (
          <div key={s.t} style={{
            padding: "5px 12px",
            margin: "0 10px 0 6px",
            borderRadius: 4,
            background: sel ? ListSelBg : "transparent",
            color: sel ? ListSelFg : ListFg,
            display: "flex", alignItems: "center", gap: 9,
            fontSize: 12,
            fontWeight: sel ? 700 : 500,
          }}>
            <span style={{ flex: 1, whiteSpace: "nowrap", overflow: "hidden", textOverflow: "ellipsis" }}>{s.t}</span>
            {s.type === "toggle" ? (
              <span style={{
                width: 22, height: 12, borderRadius: 6,
                background: s.v ? (sel ? "#f4f1ec" : "#1a1714") : (sel ? "rgba(244,241,236,0.25)" : "rgba(26,23,20,0.18)"),
                position: "relative", flexShrink: 0,
              }}>
                <span style={{
                  position: "absolute", top: 1.5, left: s.v ? 11 : 1.5,
                  width: 9, height: 9, borderRadius: "50%",
                  background: s.v ? (sel ? "#1a1714" : "#f4f1ec") : (sel ? "#f4f1ec" : "#fff"),
                  transition: "left .2s",
                }} />
              </span>
            ) : (
              <span style={{ fontSize: 11, fontWeight: 600, color: sel ? "rgba(244,241,236,0.75)" : ListMutedDeep }}>{s.v}</span>
            )}
          </div>
        )} />
    </div>
  );
}

const SOUND_ITEMS = [
  { t: "Volume", v: 0.62, label: "62%" },
  { t: "Bass", v: 0.55, label: "+2 dB" },
  { t: "Treble", v: 0.50, label: "0 dB" },
  { t: "Balance", v: 0.50, label: "Center" },
  { t: "Stereo Width", v: 0.65, label: "120%" },
];
function SettingsSound({ selectedIdx = 0 }) {
  return (
    <div style={screenWrap()}>
      <ScreenHeader title="Sound" right="" />
      <ScrollList items={SOUND_ITEMS} selectedIdx={selectedIdx} visible={5}
        renderItem={(s, i, sel) => (
          <div key={s.t} style={{
            padding: "6px 12px",
            margin: "0 10px 0 6px",
            borderRadius: 4,
            background: sel ? ListSelBg : "transparent",
            color: sel ? ListSelFg : ListFg,
            fontSize: 11.5,
            fontWeight: sel ? 700 : 500,
          }}>
            <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", marginBottom: 4 }}>
              <span>{s.t}</span>
              <span style={{ fontSize: 10, fontWeight: 600, color: sel ? "rgba(244,241,236,0.75)" : ListMutedDeep, fontVariantNumeric: "tabular-nums" }}>{s.label}</span>
            </div>
            <div style={{ height: 3, borderRadius: 2, background: sel ? "rgba(244,241,236,0.18)" : "rgba(26,23,20,0.1)", overflow: "hidden" }}>
              <div style={{ width: `${s.v * 100}%`, height: "100%", background: sel ? "#f4f1ec" : "#1a1714" }} />
            </div>
          </div>
        )} />
    </div>
  );
}

function SettingsAbout() {
  const rows = [
    ["Model", "iPod Video 5G"],["Capacity", "30 GB"],["Available", "11.4 GB"],
    ["Songs", "1,284"],["Podcasts", "47"],["Audiobooks", "5"],["Firmware", "Rockbox 3.15.1"],
  ];
  return (
    <div style={screenWrap()}>
      <ScreenHeader title="About" right="" />
      <div style={{ padding: "10px 16px" }}>
        {rows.map(([k, v]) => (
          <div key={k} style={{
            display: "flex", justifyContent: "space-between",
            padding: "5px 0",
            borderBottom: `1px solid ${ListBorder}`,
            fontSize: 11.5,
          }}>
            <span style={{ color: ListMutedDeep, fontWeight: 500 }}>{k}</span>
            <span style={{ fontWeight: 700, fontVariantNumeric: "tabular-nums" }}>{v}</span>
          </div>
        ))}
      </div>
    </div>
  );
}

// Expose lengths so the controller knows how far to allow scrolling
window.MENU_LENGTHS = {
  music: MUSIC_ITEMS.length, artists: ARTISTS.length, albums: ALBUMS.length,
  songs: SONGS.length, genres: GENRES.length, playlists: PLAYLISTS.length,
  podcasts: PODCASTS.length, podEpisodes: PODCAST_EPISODES.length,
  audiobooks: AUDIOBOOKS.length, settings: SETTINGS_ITEMS.length,
  settingsPlayback: PLAYBACK_ITEMS.length, settingsSound: SOUND_ITEMS.length,
};

window.MusicMenu = MusicMenu;
window.ArtistsList = ArtistsList;
window.AlbumsList = AlbumsList;
window.SongsList = SongsList;
window.GenresList = GenresList;
window.PlaylistsList = PlaylistsList;
window.PodcastsList = PodcastsList;
window.PodcastEpisodes = PodcastEpisodes;
window.AudiobooksList = AudiobooksList;
window.SettingsMenu = SettingsMenu;
window.SettingsPlayback = SettingsPlayback;
window.SettingsSound = SettingsSound;
window.SettingsAbout = SettingsAbout;
window.StatusStrip = StatusStrip;
