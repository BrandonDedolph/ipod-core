// Rockbox theme screens — all designed at native 320×240
// Common: Nunito rounded font, warm-light or true-dark, careful spacing

// ============================================================
// Shared atoms
// ============================================================
const screenBase = (bg) => ({
  width: 320,
  height: 240,
  background: bg,
  fontFamily: "'Nunito', system-ui, sans-serif",
  position: "relative",
  overflow: "hidden",
  color: "#1a1714",
  WebkitFontSmoothing: "antialiased",
});

// Album-art placeholder: subtle striped square so we don't draw fake art
function AlbumArt({ size = 96, hue = 30, label = "" }) {
  return (
    <div
      style={{
        width: size,
        height: size,
        borderRadius: 4,
        background: `repeating-linear-gradient(135deg, oklch(0.78 0.04 ${hue}) 0 6px, oklch(0.74 0.04 ${hue}) 6px 12px)`,
        position: "relative",
        boxShadow: "0 1px 0 rgba(0,0,0,0.04)",
        flexShrink: 0,
        display: "flex",
        alignItems: "center",
        justifyContent: "center",
      }}
    >
      <div style={{
        position: "absolute",
        inset: 4,
        border: "1px solid rgba(0,0,0,0.08)",
      }} />
      {label && (
        <div style={{
          fontSize: 8,
          fontFamily: "'JetBrains Mono', monospace",
          color: "rgba(0,0,0,0.4)",
          letterSpacing: 0.5,
        }}>{label}</div>
      )}
    </div>
  );
}

// Battery glyph with optional inline percentage + charging bolt
function Battery({ level = 0.7, color = "#1a1714", showPct = true, charging = false }) {
  const pct = Math.round(level * 100);
  if (!showPct) {
    return (
      <svg width="18" height="9" viewBox="0 0 18 9" style={{ display: "block" }}>
        <rect x="0.5" y="0.5" width="14" height="8" rx="1" fill="none" stroke={color} strokeWidth="1" />
        <rect x="15" y="2.5" width="1.5" height="4" fill={color} />
        <rect x="2" y="2" width={11 * level} height="5" fill={color} />
        {charging && <path d="M9 1.5 L6.5 5 L8 5 L7 8 L10 4 L8.5 4 Z" fill={color} />}
      </svg>
    );
  }
  return (
    <svg width="32" height="11" viewBox="0 0 32 11" style={{ display: "block" }}>
      <rect x="0.5" y="0.5" width="28" height="10" rx="1.5" fill="none" stroke={color} strokeWidth="1" />
      <rect x="29" y="3" width="2" height="5" fill={color} />
      <rect x="2" y="2" width={25 * level} height="7" fill={color} opacity="0.18" />
      {charging ? (
        <g>
          <path d="M14 1.5 L10.5 5.5 L13 5.5 L11.5 9.5 L15.5 5 L13 5 Z" fill={color} />
          <text x="22" y="8.4" textAnchor="middle"
                fontFamily="'Nunito', sans-serif"
                fontSize="7" fontWeight="800" fill={color}>{pct}%</text>
        </g>
      ) : (
        <text x="15" y="8.4" textAnchor="middle"
              fontFamily="'Nunito', sans-serif"
              fontSize="7" fontWeight="800" fill={color}>{pct}%</text>
      )}
    </svg>
  );
}

// Shuffle / repeat / play-state / star-rating / bitrate badges
function ShuffleIcon({ size = 11, color = "#1a1714", on = true }) {
  const op = on ? 1 : 0.25;
  return (
    <svg width={size} height={size * 0.78} viewBox="0 0 14 11" style={{ display: "block", opacity: op }}>
      <path d="M1 2 L4 2 L8 8.5 L11 8.5" stroke={color} strokeWidth="1.4" fill="none" strokeLinecap="round" strokeLinejoin="round" />
      <path d="M1 8.5 L4 8.5 L5.2 6.5" stroke={color} strokeWidth="1.4" fill="none" strokeLinecap="round" />
      <path d="M7.2 4.5 L8 3.2 L11 3.2" stroke={color} strokeWidth="1.4" fill="none" strokeLinecap="round" strokeLinejoin="round" />
      <path d="M9.5 1.4 L11.7 3.2 L9.5 5" stroke={color} strokeWidth="1.4" fill="none" strokeLinecap="round" strokeLinejoin="round" />
      <path d="M9.5 6.7 L11.7 8.5 L9.5 10.3" stroke={color} strokeWidth="1.4" fill="none" strokeLinecap="round" strokeLinejoin="round" />
    </svg>
  );
}
function RepeatIcon({ size = 11, color = "#1a1714", mode = "off" }) {
  // mode: "off" | "all" | "one"
  const op = mode === "off" ? 0.25 : 1;
  return (
    <svg width={size} height={size * 0.78} viewBox="0 0 14 11" style={{ display: "block", opacity: op }}>
      <path d="M3 2 L11 2 L11 5" stroke={color} strokeWidth="1.4" fill="none" strokeLinecap="round" strokeLinejoin="round" />
      <path d="M11 9 L3 9 L3 6" stroke={color} strokeWidth="1.4" fill="none" strokeLinecap="round" strokeLinejoin="round" />
      <path d="M9 0.4 L11.4 2 L9 3.6" stroke={color} strokeWidth="1.4" fill="none" strokeLinecap="round" strokeLinejoin="round" />
      <path d="M5 7.4 L2.6 9 L5 10.6" stroke={color} strokeWidth="1.4" fill="none" strokeLinecap="round" strokeLinejoin="round" />
      {mode === "one" && (
        <text x="7" y="7.4" textAnchor="middle" fontFamily="'Nunito', sans-serif" fontSize="5" fontWeight="800" fill={color}>1</text>
      )}
    </svg>
  );
}
function PlayPauseIcon({ size = 9, color = "#7a7068", playing = true }) {
  if (playing) {
    return (
      <svg width={size * 0.78} height={size} viewBox="0 0 7 9" style={{ display: "block" }}>
        <polygon points="0.5,0.5 6.5,4.5 0.5,8.5" fill={color} />
      </svg>
    );
  }
  return (
    <svg width={size * 0.78} height={size} viewBox="0 0 7 9" style={{ display: "block" }}>
      <rect x="0.5" y="0.5" width="2" height="8" fill={color} />
      <rect x="4.5" y="0.5" width="2" height="8" fill={color} />
    </svg>
  );
}
function StarRating({ value = 4, max = 5, size = 8, color = "#1a1714", muted = "rgba(26,23,20,0.2)" }) {
  return (
    <div style={{ display: "inline-flex", gap: 1.5 }}>
      {Array.from({ length: max }).map((_, i) => (
        <svg key={i} width={size} height={size} viewBox="0 0 10 10" style={{ display: "block" }}>
          <polygon points="5,0.5 6.3,3.7 9.7,3.9 7.1,6.1 7.9,9.5 5,7.7 2.1,9.5 2.9,6.1 0.3,3.9 3.7,3.7"
                   fill={i < value ? color : muted} />
        </svg>
      ))}
    </div>
  );
}
function FormatBadge({ format = "MP3", bitrate = 192, color = "#1a1714", muted = "rgba(26,23,20,0.55)" }) {
  return (
    <span style={{
      display: "inline-flex", alignItems: "center", gap: 4,
      fontSize: 8, fontWeight: 800, letterSpacing: 0.5,
      fontFamily: "'Nunito', sans-serif",
      textTransform: "uppercase",
    }}>
      <span style={{ color, padding: "1px 4px", border: `1px solid ${color}`, borderRadius: 2 }}>{format}</span>
      <span style={{ color: muted, fontWeight: 600, fontVariantNumeric: "tabular-nums" }}>{bitrate}kbps</span>
    </span>
  );
}

// Progress bar
function Progress({ value = 0.43, width = 280, height = 3, fg = "#1a1714", bg = "rgba(26,23,20,0.15)", rounded = true }) {
  return (
    <div style={{
      width, height,
      background: bg,
      borderRadius: rounded ? height : 0,
      overflow: "hidden",
    }}>
      <div style={{
        width: `${value * 100}%`,
        height: "100%",
        background: fg,
        borderRadius: rounded ? height : 0,
      }} />
    </div>
  );
}

// ============================================================
// THEME 1 — "Linen" — warm light, text-forward
// ============================================================
function Theme1NowPlaying() {
  return (
    <div style={screenBase("#f4f1ec")}>
      {/* Status bar */}
      <div style={{
        display: "flex", justifyContent: "space-between", alignItems: "center",
        padding: "8px 12px", fontSize: 11, fontWeight: 600, color: "#1a1714",
      }}>
        <span style={{ letterSpacing: 0.3 }}>Now Playing</span>
        <div style={{ display: "flex", gap: 6, alignItems: "center" }}>
          <ShuffleIcon size={10} color="#1a1714" on={true} />
          <RepeatIcon size={10} color="#1a1714" mode="all" />
          <Battery level={0.78} />
        </div>
      </div>

      <div style={{ padding: "8px 18px 0", display: "flex", gap: 14 }}>
        <AlbumArt size={84} hue={30} />
        <div style={{ flex: 1, paddingTop: 2, minWidth: 0 }}>
          <div style={{ fontSize: 9, fontWeight: 700, letterSpacing: 1, color: "#9a8e80", textTransform: "uppercase" }}>Track 04 of 11</div>
          <div style={{ fontSize: 17, fontWeight: 700, lineHeight: 1.15, marginTop: 3, color: "#1a1714", letterSpacing: -0.2 }}>
            Avril 14th
          </div>
          <div style={{ fontSize: 12, fontWeight: 500, marginTop: 2, color: "#5a5048" }}>
            Aphex Twin
          </div>
          <div style={{ fontSize: 11, fontWeight: 400, marginTop: 1, color: "#9a8e80" }}>
            Drukqs
          </div>
          <div style={{ marginTop: 5, display: "flex", alignItems: "center", gap: 6 }}>
            <StarRating value={4} size={8} color="#1a1714" muted="rgba(26,23,20,0.18)" />
            <FormatBadge format="MP3" bitrate={192} color="#1a1714" muted="rgba(26,23,20,0.5)" />
          </div>
        </div>
      </div>

      {/* Up next */}
      <div style={{
        position: "absolute", bottom: 42, left: 18, right: 18,
        display: "flex", alignItems: "center", gap: 6,
        fontSize: 9, color: "#9a8e80",
      }}>
        <span style={{ fontWeight: 800, letterSpacing: 0.6, textTransform: "uppercase" }}>Up next</span>
        <span style={{ width: 1, height: 8, background: "rgba(26,23,20,0.15)" }} />
        <span style={{ fontWeight: 500, fontSize: 10, color: "#5a5048", whiteSpace: "nowrap", overflow: "hidden", textOverflow: "ellipsis" }}>
          Mt Saint Michel + Saint Michael's Mount
        </span>
      </div>

      {/* Progress */}
      <div style={{ position: "absolute", bottom: 14, left: 18, right: 18 }}>
        <div style={{ display: "flex", justifyContent: "space-between", marginBottom: 5, fontSize: 10, fontWeight: 600, color: "#5a5048", fontVariantNumeric: "tabular-nums" }}>
          <span>1:42</span>
          <span style={{ opacity: 0.5 }}>−2:18</span>
        </div>
        <Progress value={0.43} width={284} fg="#1a1714" bg="rgba(26,23,20,0.12)" />
      </div>
    </div>
  );
}

// ============================================================
// THEME 2 — "Paper" — minimal, big art, single line of text
// ============================================================
function Theme2NowPlaying({ playing = true }) {
  return (
    <div style={screenBase("#faf8f4")}>
      <div style={{
        display: "flex", flexDirection: "column", alignItems: "center",
        paddingTop: 22,
      }}>
        <AlbumArt size={108} hue={50} />
      </div>

      <div style={{
        position: "absolute", bottom: 22, left: 0, right: 0,
        padding: "0 16px",
        textAlign: "center",
      }}>
        <div style={{ fontSize: 14, fontWeight: 700, color: "#1a1714", letterSpacing: -0.1, whiteSpace: "nowrap", overflow: "hidden", textOverflow: "ellipsis" }}>
          Avril 14th
        </div>
        <div style={{ fontSize: 11, fontWeight: 500, color: "#7a7068", marginTop: 1 }}>
          Aphex Twin · Drukqs
        </div>
        <div style={{ marginTop: 6, display: "flex", alignItems: "center", gap: 8, justifyContent: "center", fontSize: 10, fontWeight: 600, color: "#5a5048", fontVariantNumeric: "tabular-nums" }}>
          <span>1:42</span>
          <Progress value={0.43} width={180} height={2} fg="#1a1714" bg="rgba(26,23,20,0.1)" />
          <span>4:00</span>
        </div>
      </div>

      {/* Top status bar */}
      <div style={{ position: "absolute", top: 8, left: 10, display: "flex", alignItems: "center", gap: 5 }}>
        <PlayPauseIcon playing={playing} color="#7a7068" size={9} />
        <span style={{ fontSize: 9, fontWeight: 700, color: "#7a7068", letterSpacing: 0.6, textTransform: "uppercase" }}>
          {playing ? "Playing" : "Paused"}
        </span>
      </div>
      <div style={{ position: "absolute", top: 8, right: 10, display: "flex", alignItems: "center", gap: 5 }}>
        <ShuffleIcon size={9} color="#7a7068" on={true} />
        <RepeatIcon size={9} color="#7a7068" mode="all" />
        <Battery level={0.78} color="#7a7068" />
      </div>

      {/* Bottom rail: rating left, format right */}
      <div style={{ position: "absolute", bottom: 5, left: 12 }}>
        <StarRating value={4} size={7} color="#7a7068" muted="rgba(122,112,104,0.25)" />
      </div>
      <div style={{ position: "absolute", bottom: 5, right: 12 }}>
        <FormatBadge format="MP3" bitrate={192} color="#7a7068" muted="rgba(122,112,104,0.65)" />
      </div>
    </div>
  );
}

// ============================================================
// THEME 3 — "Ink" — true dark, terracotta accent
// ============================================================
function Theme3NowPlaying() {
  const accent = "oklch(0.7 0.12 40)";
  return (
    <div style={{ ...screenBase("#0e0d0c"), color: "#e8e4dd" }}>
      {/* Status */}
      <div style={{
        display: "flex", justifyContent: "space-between", alignItems: "center",
        padding: "8px 12px", fontSize: 10, fontWeight: 600, color: "#7a736a",
        letterSpacing: 0.5, textTransform: "uppercase",
      }}>
        <div style={{ display: "flex", alignItems: "center", gap: 6 }}>
          <PlayPauseIcon playing={true} color="#7a736a" size={9} />
          <span>Playing</span>
        </div>
        <div style={{ display: "flex", alignItems: "center", gap: 6 }}>
          <ShuffleIcon size={9} color="#7a736a" on={true} />
          <RepeatIcon size={9} color="#7a736a" mode="all" />
          <Battery level={0.78} color="#7a736a" />
        </div>
      </div>

      <div style={{ padding: "10px 18px 0", display: "flex", gap: 14 }}>
        <div style={{
          width: 76, height: 76, borderRadius: 3,
          background: `repeating-linear-gradient(135deg, oklch(0.32 0.02 40) 0 5px, oklch(0.28 0.02 40) 5px 10px)`,
          border: "1px solid rgba(255,255,255,0.06)",
          flexShrink: 0,
        }} />
        <div style={{ flex: 1, minWidth: 0, paddingTop: 2 }}>
          <div style={{ fontSize: 16, fontWeight: 700, color: "#f4ede2", letterSpacing: -0.2, lineHeight: 1.15 }}>
            Avril 14th
          </div>
          <div style={{ fontSize: 11, fontWeight: 500, marginTop: 3, color: "#a89e92" }}>
            Aphex Twin
          </div>
          <div style={{ fontSize: 10, fontWeight: 400, marginTop: 1, color: "#6a635a" }}>
            Drukqs · 2001
          </div>
          <div style={{ marginTop: 5, display: "flex", alignItems: "center", gap: 6 }}>
            <StarRating value={4} size={7} color={accent} muted="rgba(255,255,255,0.12)" />
            <span style={{ display: "inline-flex", alignItems: "center", gap: 3, fontSize: 8, fontWeight: 700, color: accent, letterSpacing: 0.8, textTransform: "uppercase" }}>
              <span style={{ width: 3, height: 3, borderRadius: "50%", background: accent }} />
              <span>4 / 11</span>
            </span>
          </div>
        </div>
      </div>

      {/* Up next */}
      <div style={{
        position: "absolute", bottom: 36, left: 18, right: 18,
        display: "flex", alignItems: "center", gap: 6,
        fontSize: 9, color: "#6a635a",
      }}>
        <span style={{ fontWeight: 800, letterSpacing: 0.6, textTransform: "uppercase", color: "#7a736a" }}>Next</span>
        <span style={{ width: 1, height: 7, background: "rgba(255,255,255,0.1)" }} />
        <span style={{ fontWeight: 500, fontSize: 10, color: "#a89e92", whiteSpace: "nowrap", overflow: "hidden", textOverflow: "ellipsis" }}>
          Mt Saint Michel + Saint Michael's Mount
        </span>
        <span style={{ marginLeft: "auto", flexShrink: 0 }}>
          <FormatBadge format="MP3" bitrate={192} color="#7a736a" muted="rgba(106,99,90,0.7)" />
        </span>
      </div>

      {/* Bottom progress */}
      <div style={{ position: "absolute", bottom: 12, left: 18, right: 18 }}>
        <Progress value={0.43} width={284} height={2} fg={accent} bg="rgba(255,255,255,0.08)" />
        <div style={{ display: "flex", justifyContent: "space-between", marginTop: 4, fontSize: 9, fontWeight: 600, color: "#7a736a", fontVariantNumeric: "tabular-nums", letterSpacing: 0.5 }}>
          <span>1:42</span>
          <span>4:00</span>
        </div>
      </div>
    </div>
  );
}

// ============================================================
// THEME 4 — "Card" — light with subtle floating card for art
// ============================================================
function Theme4NowPlaying() {
  return (
    <div style={screenBase("#eeeae3")}>
      {/* Top status */}
      <div style={{
        display: "flex", justifyContent: "space-between", alignItems: "center",
        padding: "7px 12px", fontSize: 10, fontWeight: 700, color: "#5a5048",
        letterSpacing: 0.5,
      }}>
        <span>4 OF 11</span>
        <div style={{ display: "flex", alignItems: "center", gap: 5 }}>
          <ShuffleIcon size={9} color="#5a5048" on={true} />
          <RepeatIcon size={9} color="#5a5048" mode="all" />
          <Battery level={0.78} color="#5a5048" />
        </div>
      </div>

      {/* Centered card */}
      <div style={{
        margin: "8px 18px 0",
        background: "#faf8f4",
        borderRadius: 6,
        padding: 12,
        display: "flex",
        gap: 12,
        boxShadow: "0 1px 0 rgba(0,0,0,0.05), 0 0 0 1px rgba(0,0,0,0.04)",
      }}>
        <AlbumArt size={68} hue={70} />
        <div style={{ flex: 1, minWidth: 0, display: "flex", flexDirection: "column", justifyContent: "center" }}>
          <div style={{ fontSize: 14, fontWeight: 700, color: "#1a1714", letterSpacing: -0.1, lineHeight: 1.15 }}>
            Avril 14th
          </div>
          <div style={{ fontSize: 11, fontWeight: 500, marginTop: 3, color: "#5a5048" }}>
            Aphex Twin
          </div>
          <div style={{ fontSize: 10, fontWeight: 400, marginTop: 1, color: "#9a8e80" }}>
            Drukqs
          </div>
          <div style={{ marginTop: 5, display: "flex", alignItems: "center", gap: 6 }}>
            <StarRating value={4} size={7} color="#1a1714" muted="rgba(26,23,20,0.18)" />
            <FormatBadge format="MP3" bitrate={192} color="#5a5048" muted="rgba(90,80,72,0.7)" />
          </div>
        </div>
      </div>

      {/* Up next strip */}
      <div style={{
        margin: "8px 18px 0",
        display: "flex", alignItems: "center", gap: 6,
        fontSize: 9, color: "#9a8e80",
      }}>
        <span style={{ fontWeight: 800, letterSpacing: 0.6, textTransform: "uppercase", color: "#5a5048" }}>Up next</span>
        <span style={{ fontWeight: 500, fontSize: 10, color: "#5a5048", whiteSpace: "nowrap", overflow: "hidden", textOverflow: "ellipsis", flex: 1 }}>
          Mt Saint Michel + Saint Michael's Mount
        </span>
      </div>

      {/* Big chunky progress at bottom */}
      <div style={{ position: "absolute", bottom: 14, left: 18, right: 18 }}>
        <div style={{ display: "flex", justifyContent: "space-between", marginBottom: 4, fontSize: 9, fontWeight: 700, color: "#5a5048", fontVariantNumeric: "tabular-nums" }}>
          <span>1:42</span>
          <span style={{ opacity: 0.5 }}>4:00</span>
        </div>
        <div style={{
          height: 6,
          borderRadius: 3,
          background: "rgba(26,23,20,0.1)",
          overflow: "hidden",
        }}>
          <div style={{
            width: "43%",
            height: "100%",
            background: "#1a1714",
            borderRadius: 3,
          }} />
        </div>
      </div>
    </div>
  );
}

// ============================================================
// MAIN MENU (shared style — Linen / light)
// ============================================================
function MainMenu({ selectedIdx = 1, dark = false }) {
  const items = ["Music", "Playlists", "Podcasts", "Audiobooks", "Settings", "Now Playing"];
  const bg = dark ? "#0e0d0c" : "#f4f1ec";
  const fg = dark ? "#e8e4dd" : "#1a1714";
  const muted = dark ? "#7a736a" : "#9a8e80";
  const selBg = dark ? "rgba(255,255,255,0.08)" : "#1a1714";
  const selFg = dark ? "#f4ede2" : "#f4f1ec";

  return (
    <div style={{ ...screenBase(bg), color: fg }}>
      {/* Header */}
      <div style={{
        padding: "9px 14px 8px",
        borderBottom: dark ? "1px solid rgba(255,255,255,0.06)" : "1px solid rgba(26,23,20,0.08)",
        display: "flex", justifyContent: "space-between", alignItems: "center",
      }}>
        <span style={{ fontSize: 13, fontWeight: 700, letterSpacing: -0.1 }}>iPod</span>
        <Battery level={0.78} color={fg} />
      </div>

      {/* List */}
      <div style={{ padding: "6px 0" }}>
        {items.map((label, i) => {
          const sel = i === selectedIdx;
          return (
            <div key={label} style={{
              padding: "7px 14px",
              margin: "0 6px",
              borderRadius: 4,
              background: sel ? selBg : "transparent",
              color: sel ? selFg : fg,
              fontSize: 13,
              fontWeight: sel ? 700 : 500,
              display: "flex",
              justifyContent: "space-between",
              alignItems: "center",
              letterSpacing: -0.05,
            }}>
              <span>{label}</span>
              <span style={{ fontSize: 12, opacity: sel ? 0.7 : 0.4 }}>›</span>
            </div>
          );
        })}
      </div>
    </div>
  );
}

// ============================================================
// TRACK LIST
// ============================================================
function TrackList({ selectedIdx = 3, dark = false }) {
  const tracks = [
    { n: "01", t: "Vordhosbn", d: "4:53" },
    { n: "02", t: "Kladfvgbung Micshk", d: "3:01" },
    { n: "03", t: "Omgyjya Switch7", d: "4:12" },
    { n: "04", t: "Avril 14th", d: "4:00" },
    { n: "05", t: "Mt Saint Michel + Saint Michaels Mount", d: "8:08" },
    { n: "06", t: "Gwely Mernans", d: "5:50" },
    { n: "07", t: "Bbydhyonchord", d: "3:20" },
  ];
  const bg = dark ? "#0e0d0c" : "#f4f1ec";
  const fg = dark ? "#e8e4dd" : "#1a1714";
  const muted = dark ? "#7a736a" : "#9a8e80";
  const selBg = dark ? "rgba(255,255,255,0.08)" : "#1a1714";
  const selFg = dark ? "#f4ede2" : "#f4f1ec";

  return (
    <div style={{ ...screenBase(bg), color: fg }}>
      <div style={{
        padding: "9px 14px 8px",
        borderBottom: dark ? "1px solid rgba(255,255,255,0.06)" : "1px solid rgba(26,23,20,0.08)",
        display: "flex", justifyContent: "space-between", alignItems: "center",
      }}>
        <span style={{ fontSize: 12, fontWeight: 700, letterSpacing: -0.1 }}>Drukqs</span>
        <span style={{ fontSize: 10, fontWeight: 600, color: muted }}>11 tracks</span>
      </div>

      <div style={{ padding: "4px 0" }}>
        {tracks.map((tr, i) => {
          const sel = i === selectedIdx;
          return (
            <div key={tr.n} style={{
              padding: "5px 12px",
              margin: "0 6px",
              borderRadius: 3,
              background: sel ? selBg : "transparent",
              color: sel ? selFg : fg,
              fontSize: 11.5,
              fontWeight: sel ? 700 : 500,
              display: "flex",
              alignItems: "center",
              gap: 8,
            }}>
              <span style={{ fontVariantNumeric: "tabular-nums", fontSize: 10, color: sel ? selFg : muted, opacity: sel ? 0.7 : 1, fontWeight: 600, width: 16 }}>{tr.n}</span>
              <span style={{ flex: 1, whiteSpace: "nowrap", overflow: "hidden", textOverflow: "ellipsis" }}>{tr.t}</span>
              <span style={{ fontVariantNumeric: "tabular-nums", fontSize: 10, color: sel ? selFg : muted, opacity: sel ? 0.7 : 1, fontWeight: 600 }}>{tr.d}</span>
            </div>
          );
        })}
      </div>
    </div>
  );
}

window.Theme1NowPlaying = Theme1NowPlaying;
window.Theme2NowPlaying = Theme2NowPlaying;
window.Theme3NowPlaying = Theme3NowPlaying;
window.Theme4NowPlaying = Theme4NowPlaying;
window.MainMenu = MainMenu;
window.TrackList = TrackList;
window.AlbumArt = AlbumArt;
window.Battery = Battery;
window.Progress = Progress;
window.ShuffleIcon = ShuffleIcon;
window.RepeatIcon = RepeatIcon;
window.PlayPauseIcon = PlayPauseIcon;
window.StarRating = StarRating;
window.FormatBadge = FormatBadge;
window.screenBase = screenBase;
