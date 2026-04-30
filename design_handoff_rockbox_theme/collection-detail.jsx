// Album detail and Playlist detail views — songs within a collection
// Both share the same header-card + tracklist pattern

const aBg = "#f4f1ec";
const aFg = "#1a1714";
const aMuted = "#9a8e80";
const aMutedDeep = "#5a5048";
const aSelBg = "#1a1714";
const aSelFg = "#f4f1ec";
const aBorder = "rgba(26,23,20,0.08)";

function NowPlayingDot({ sel }) {
  // Three little vertical bars indicating "now playing"
  const c = sel ? "#f4f1ec" : "#1a1714";
  return (
    <div style={{ display: "flex", alignItems: "flex-end", gap: 1.5, height: 9, width: 10 }}>
      <span style={{ width: 2, height: "60%", background: c, borderRadius: 1 }} />
      <span style={{ width: 2, height: "100%", background: c, borderRadius: 1 }} />
      <span style={{ width: 2, height: "45%", background: c, borderRadius: 1 }} />
    </div>
  );
}

// =====================================================
// ALBUM DETAIL — art header + tracklist with currently-playing
// =====================================================
function AlbumDetail({ selectedIdx = 3, nowPlayingIdx = 3 }) {
  const tracks = [
    { n: "1", t: "Jynweythek Ylow", d: "1:46" },
    { n: "2", t: "Vordhosbn", d: "4:53" },
    { n: "3", t: "Kladfvgbung Micshk", d: "3:01" },
    { n: "4", t: "Avril 14th", d: "2:04" },
    { n: "5", t: "Mt Saint Michel", d: "8:08" },
    { n: "6", t: "Gwely Mernans", d: "5:50" },
    { n: "7", t: "Bbydhyonchord", d: "3:20" },
  ];

  return (
    <div style={{
      width: 320, height: 240, background: aBg,
      fontFamily: "'Nunito', system-ui, sans-serif", color: aFg,
      position: "relative", overflow: "hidden",
    }}>
      {/* Header bar */}
      <div style={{
        padding: "9px 14px 8px",
        borderBottom: `1px solid ${aBorder}`,
        display: "flex", justifyContent: "space-between", alignItems: "center",
      }}>
        <div style={{ display: "flex", alignItems: "center", gap: 6, minWidth: 0 }}>
          <span style={{ fontSize: 14, fontWeight: 700, color: aMuted, lineHeight: 1 }}>‹</span>
          <span style={{ fontSize: 12, fontWeight: 700, letterSpacing: -0.1 }}>Albums</span>
        </div>
        <span style={{ fontSize: 10, fontWeight: 600, color: aMuted, fontVariantNumeric: "tabular-nums" }}>4 / 11</span>
      </div>

      {/* Album hero */}
      <div style={{
        padding: "10px 14px 8px",
        display: "flex", gap: 12, alignItems: "center",
      }}>
        <div style={{
          width: 56, height: 56, borderRadius: 3, flexShrink: 0,
          background: `repeating-linear-gradient(135deg, oklch(0.78 0.04 30) 0 5px, oklch(0.74 0.04 30) 5px 10px)`,
          boxShadow: "0 1px 0 rgba(0,0,0,0.05), 0 0 0 1px rgba(0,0,0,0.08)",
        }} />
        <div style={{ flex: 1, minWidth: 0 }}>
          <div style={{ fontSize: 14, fontWeight: 800, letterSpacing: -0.2, lineHeight: 1.1 }}>Drukqs</div>
          <div style={{ fontSize: 11, fontWeight: 600, marginTop: 2, color: aMutedDeep }}>Aphex Twin</div>
          <div style={{ fontSize: 9.5, fontWeight: 500, marginTop: 2, color: aMuted, letterSpacing: 0.3 }}>2001 · 11 tracks · 1h 41m</div>
        </div>
      </div>

      {/* Tracks */}
      <div style={{
        padding: "2px 0",
        borderTop: `1px solid ${aBorder}`,
        marginTop: 2,
      }}>
        {tracks.map((tr, i) => {
          const sel = i === selectedIdx;
          const playing = i === nowPlayingIdx;
          return (
            <div key={tr.n} style={{
              padding: "4px 12px",
              margin: "0 6px",
              borderRadius: 3,
              background: sel ? aSelBg : "transparent",
              color: sel ? aSelFg : aFg,
              display: "flex", alignItems: "center", gap: 9,
              fontSize: 11.5,
              fontWeight: sel ? 700 : 500,
            }}>
              <span style={{
                width: 14, fontSize: 10, color: sel ? "rgba(244,241,236,0.7)" : aMuted,
                fontWeight: 600, fontVariantNumeric: "tabular-nums", textAlign: "right", flexShrink: 0,
              }}>{playing ? "" : tr.n}</span>
              {playing && <div style={{ marginLeft: -14, marginRight: 0, width: 14, display: "flex", justifyContent: "center" }}><NowPlayingDot sel={sel} /></div>}
              <span style={{ flex: 1, whiteSpace: "nowrap", overflow: "hidden", textOverflow: "ellipsis" }}>{tr.t}</span>
              <span style={{ fontSize: 10, fontWeight: 600, color: sel ? "rgba(244,241,236,0.7)" : aMuted, fontVariantNumeric: "tabular-nums" }}>{tr.d}</span>
            </div>
          );
        })}
      </div>
    </div>
  );
}

// =====================================================
// PLAYLIST DETAIL — small art mosaic + tracks
// =====================================================
function PlaylistDetail({ selectedIdx = 1, nowPlayingIdx = -1 }) {
  const tracks = [
    { t: "Lemonworld",          a: "The National",     d: "4:18" },
    { t: "Avril 14th",          a: "Aphex Twin",       d: "2:04" },
    { t: "Lover, You Should…",  a: "Bon Iver",         d: "5:34" },
    { t: "Holocene",            a: "Bon Iver",         d: "5:36" },
    { t: "Cherry-Coloured Funk", a: "Cocteau Twins",   d: "3:12" },
    { t: "Andorra",             a: "Caribou",          d: "5:16" },
    { t: "Re: Stacks",          a: "Bon Iver",         d: "6:41" },
  ];

  // 2x2 mosaic
  const Mosaic = () => (
    <div style={{
      width: 56, height: 56, flexShrink: 0,
      display: "grid", gridTemplateColumns: "1fr 1fr", gap: 1,
      background: "rgba(26,23,20,0.1)", padding: 1, borderRadius: 3,
      boxShadow: "0 1px 0 rgba(0,0,0,0.05), 0 0 0 1px rgba(0,0,0,0.08)",
    }}>
      {[20, 80, 220, 340].map((h, i) => (
        <div key={i} style={{
          background: `repeating-linear-gradient(135deg, oklch(0.78 0.04 ${h}) 0 4px, oklch(0.74 0.04 ${h}) 4px 8px)`,
        }} />
      ))}
    </div>
  );

  return (
    <div style={{
      width: 320, height: 240, background: aBg,
      fontFamily: "'Nunito', system-ui, sans-serif", color: aFg,
      position: "relative", overflow: "hidden",
    }}>
      <div style={{
        padding: "9px 14px 8px",
        borderBottom: `1px solid ${aBorder}`,
        display: "flex", justifyContent: "space-between", alignItems: "center",
      }}>
        <div style={{ display: "flex", alignItems: "center", gap: 6 }}>
          <span style={{ fontSize: 14, fontWeight: 700, color: aMuted, lineHeight: 1 }}>‹</span>
          <span style={{ fontSize: 12, fontWeight: 700, letterSpacing: -0.1 }}>Playlists</span>
        </div>
        <span style={{ fontSize: 10, fontWeight: 600, color: aMuted, fontVariantNumeric: "tabular-nums" }}>2 / 32</span>
      </div>

      <div style={{ padding: "10px 14px 8px", display: "flex", gap: 12, alignItems: "center" }}>
        <Mosaic />
        <div style={{ flex: 1, minWidth: 0 }}>
          <div style={{ fontSize: 14, fontWeight: 800, letterSpacing: -0.2, lineHeight: 1.1 }}>Late Nights</div>
          <div style={{ fontSize: 11, fontWeight: 600, marginTop: 2, color: aMutedDeep }}>Mix · Ambient & Folk</div>
          <div style={{ fontSize: 9.5, fontWeight: 500, marginTop: 2, color: aMuted, letterSpacing: 0.3 }}>32 tracks · 2h 14m · Updated 3d</div>
        </div>
      </div>

      <div style={{
        padding: "2px 0",
        borderTop: `1px solid ${aBorder}`,
        marginTop: 2,
      }}>
        {tracks.map((tr, i) => {
          const sel = i === selectedIdx;
          const playing = i === nowPlayingIdx;
          return (
            <div key={tr.t} style={{
              padding: "4px 12px",
              margin: "0 6px",
              borderRadius: 3,
              background: sel ? aSelBg : "transparent",
              color: sel ? aSelFg : aFg,
              display: "flex", alignItems: "center", gap: 9,
              fontSize: 11.5,
              fontWeight: sel ? 700 : 500,
            }}>
              <span style={{ width: 12, display: "flex", justifyContent: "center", flexShrink: 0 }}>
                {playing ? <NowPlayingDot sel={sel} /> : null}
              </span>
              <div style={{ flex: 1, minWidth: 0 }}>
                <div style={{ whiteSpace: "nowrap", overflow: "hidden", textOverflow: "ellipsis" }}>{tr.t}</div>
                <div style={{ fontSize: 9.5, fontWeight: 500, marginTop: 0, color: sel ? "rgba(244,241,236,0.7)" : aMuted, whiteSpace: "nowrap", overflow: "hidden", textOverflow: "ellipsis" }}>{tr.a}</div>
              </div>
              <span style={{ fontSize: 10, fontWeight: 600, color: sel ? "rgba(244,241,236,0.7)" : aMuted, fontVariantNumeric: "tabular-nums", flexShrink: 0 }}>{tr.d}</span>
            </div>
          );
        })}
      </div>
    </div>
  );
}

window.AlbumDetail = AlbumDetail;
window.PlaylistDetail = PlaylistDetail;
