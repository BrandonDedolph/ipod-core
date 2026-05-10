// Missing system screens — boot, shutdown, file browser, EQ, theme picker, WPS info pages

// ============================================================
// BOOT / SPLASH SCREEN — shown on power-on
// ============================================================
function BootSplash({ progress = 0.6 }) {
  return (
    <div style={{
      width: 320, height: 240,
      background: "#f4f1ec",
      fontFamily: "'Nunito', system-ui, sans-serif",
      display: "flex", flexDirection: "column",
      alignItems: "center", justifyContent: "center",
      gap: 14, position: "relative",
      color: "#1a1714",
    }}>
      {/* Wordmark */}
      <div style={{ display: "flex", alignItems: "center", gap: 10 }}>
        <svg width="42" height="42" viewBox="0 0 42 42">
          <circle cx="21" cy="21" r="19" fill="none" stroke="#1a1714" strokeWidth="2" />
          <circle cx="21" cy="21" r="6" fill="#1a1714" />
          <circle cx="21" cy="21" r="2.2" fill="#f4f1ec" />
        </svg>
        <div>
          <div style={{ fontSize: 24, fontWeight: 800, letterSpacing: -0.6, lineHeight: 1 }}>Rockbox</div>
          <div style={{ fontSize: 10, fontWeight: 600, color: "#7a7068", letterSpacing: 1.4, textTransform: "uppercase", marginTop: 3 }}>iPod Video · 5G</div>
        </div>
      </div>

      {/* Loading progress */}
      <div style={{ position: "absolute", bottom: 28, left: 60, right: 60 }}>
        <div style={{ height: 2, background: "rgba(26,23,20,0.12)", borderRadius: 1, overflow: "hidden" }}>
          <div style={{ width: `${progress * 100}%`, height: "100%", background: "#1a1714" }} />
        </div>
        <div style={{ marginTop: 7, textAlign: "center", fontSize: 9, color: "#9a8e80", fontWeight: 700, letterSpacing: 0.8, textTransform: "uppercase" }}>
          Loading database…
        </div>
      </div>

      {/* Tiny version stamp */}
      <div style={{ position: "absolute", bottom: 6, right: 8, fontSize: 8, color: "#c4b9ae", fontFamily: "'JetBrains Mono', monospace" }}>
        v3.15 · build 2024.11
      </div>
    </div>
  );
}

// ============================================================
// SHUTDOWN / SLEEP — shown when powering off
// ============================================================
function ShutdownScreen() {
  return (
    <div style={{
      width: 320, height: 240,
      background: "#0e0d0c",
      fontFamily: "'Nunito', system-ui, sans-serif",
      display: "flex", flexDirection: "column",
      alignItems: "center", justifyContent: "center",
      gap: 12, color: "#a89e92",
    }}>
      <svg width="36" height="36" viewBox="0 0 36 36">
        <circle cx="18" cy="18" r="14" fill="none" stroke="#7a736a" strokeWidth="2" strokeDasharray="3 3" opacity="0.4" />
        <circle cx="18" cy="18" r="3.5" fill="#7a736a" />
      </svg>
      <div style={{ fontSize: 13, fontWeight: 700, letterSpacing: 0.4, textTransform: "uppercase", color: "#a89e92" }}>Goodnight</div>
      <div style={{ fontSize: 10, color: "#6a635a", marginTop: -4 }}>Saving your spot…</div>
    </div>
  );
}

// ============================================================
// FILE BROWSER — raw filesystem view
// ============================================================
const FB_ITEMS = [
  { t: "..", k: "up" },
  { t: "Music", k: "dir", n: 1284 },
  { t: "Podcasts", k: "dir", n: 47 },
  { t: "Recordings", k: "dir", n: 3 },
  { t: "Playlists", k: "dir", n: 6 },
  { t: "rockbox.ipod", k: "file", s: "1.4 MB" },
  { t: "config.cfg", k: "file", s: "4 KB" },
  { t: "lyrics.txt", k: "file", s: "2 KB" },
  { t: "notes.txt", k: "file", s: "8 KB" },
];
function FileBrowser({ selectedIdx = 1 }) {
  return (
    <div style={{
      width: 320, height: 240,
      background: "#f4f1ec",
      fontFamily: "'Nunito', system-ui, sans-serif",
      color: "#1a1714",
      position: "relative",
    }}>
      <div style={{
        padding: "6px 12px 5px",
        borderBottom: "1px solid rgba(26,23,20,0.08)",
        display: "flex", justifyContent: "space-between", alignItems: "center",
      }}>
        <div style={{ display: "flex", alignItems: "center", gap: 5, minWidth: 0 }}>
          <span style={{ fontSize: 14, fontWeight: 700, color: "#9a8e80", lineHeight: 1 }}>‹</span>
          <span style={{ fontSize: 11, fontWeight: 700, fontFamily: "'JetBrains Mono', monospace", color: "#5a5048", whiteSpace: "nowrap", overflow: "hidden", textOverflow: "ellipsis" }}>/Music/Aphex Twin/Drukqs</span>
        </div>
        <span style={{ fontSize: 9, fontWeight: 700, color: "#9a8e80", letterSpacing: 0.5, textTransform: "uppercase" }}>11.4 GB free</span>
      </div>
      <div style={{ padding: "2px 0" }}>
        {FB_ITEMS.slice(0, 9).map((item, i) => {
          const sel = i === selectedIdx;
          return (
            <div key={item.t} style={{
              display: "flex", alignItems: "center", gap: 8,
              padding: "3px 12px", margin: "0 6px",
              borderRadius: 3,
              background: sel ? "#1a1714" : "transparent",
              color: sel ? "#f4f1ec" : "#1a1714",
              fontSize: 11, fontFamily: "'JetBrains Mono', monospace", fontWeight: sel ? 700 : 500,
            }}>
              <span style={{ width: 10, opacity: sel ? 0.7 : 0.5, fontSize: 10 }}>
                {item.k === "up" ? "↑" : item.k === "dir" ? "▸" : "·"}
              </span>
              <span style={{ flex: 1, whiteSpace: "nowrap", overflow: "hidden", textOverflow: "ellipsis" }}>{item.t}{item.k === "dir" ? "/" : ""}</span>
              <span style={{ fontSize: 9, opacity: sel ? 0.7 : 0.45, fontWeight: 600 }}>
                {item.k === "dir" ? `${item.n} items` : item.s || ""}
              </span>
            </div>
          );
        })}
      </div>
    </div>
  );
}

// ============================================================
// EQUALIZER — 5 vertical sliders
// ============================================================
function Equalizer({ selectedIdx = 2, values = [3, 4, 2, -1, -3] }) {
  const bands = ["60", "230", "910", "3.6k", "14k"];
  const max = 12;
  return (
    <div style={{
      width: 320, height: 240,
      background: "#f4f1ec",
      fontFamily: "'Nunito', system-ui, sans-serif",
      color: "#1a1714",
      position: "relative",
    }}>
      <div style={{
        padding: "8px 14px 7px",
        borderBottom: "1px solid rgba(26,23,20,0.08)",
        display: "flex", justifyContent: "space-between", alignItems: "center",
      }}>
        <div style={{ display: "flex", alignItems: "center", gap: 6 }}>
          <span style={{ fontSize: 14, fontWeight: 700, color: "#9a8e80", lineHeight: 1 }}>‹</span>
          <span style={{ fontSize: 12.5, fontWeight: 700, letterSpacing: -0.1 }}>Equalizer</span>
        </div>
        <span style={{ fontSize: 10, fontWeight: 700, color: "#9a8e80" }}>Custom</span>
      </div>

      {/* Sliders */}
      <div style={{
        display: "flex", justifyContent: "space-around", alignItems: "flex-end",
        height: 178, padding: "8px 24px 0",
      }}>
        {bands.map((b, i) => {
          const sel = i === selectedIdx;
          const v = values[i];
          const norm = (v + max) / (2 * max); // 0..1, 0.5 = 0dB
          const trackH = 100;
          return (
            <div key={b} style={{ display: "flex", flexDirection: "column", alignItems: "center", gap: 5 }}>
              {/* dB label */}
              <span style={{ fontSize: 8, fontWeight: 700, fontVariantNumeric: "tabular-nums",
                color: sel ? "#1a1714" : "#9a8e80", letterSpacing: 0.4 }}>
                {v > 0 ? `+${v}` : v}
              </span>
              {/* track */}
              <div style={{
                position: "relative",
                width: sel ? 4 : 2,
                height: trackH,
                background: sel ? "rgba(26,23,20,0.18)" : "rgba(26,23,20,0.1)",
                borderRadius: 2,
              }}>
                {/* zero line */}
                <div style={{ position: "absolute", left: -3, right: -3, top: trackH / 2, height: 1, background: "rgba(26,23,20,0.18)" }} />
                {/* thumb */}
                <div style={{
                  position: "absolute",
                  left: "50%",
                  top: trackH * (1 - norm),
                  transform: "translate(-50%, -50%)",
                  width: sel ? 14 : 10, height: sel ? 6 : 4,
                  background: "#1a1714",
                  borderRadius: 2,
                  boxShadow: sel ? "0 0 0 2px rgba(26,23,20,0.12)" : "none",
                }} />
              </div>
              {/* band label */}
              <span style={{ fontSize: 9, fontWeight: sel ? 700 : 600, color: sel ? "#1a1714" : "#7a7068",
                fontVariantNumeric: "tabular-nums", letterSpacing: 0.3 }}>
                {b}
              </span>
              <span style={{ fontSize: 7, fontWeight: 600, color: "#9a8e80", letterSpacing: 0.4, textTransform: "uppercase" }}>Hz</span>
            </div>
          );
        })}
      </div>

      <div style={{ position: "absolute", bottom: 6, left: 14, right: 14, display: "flex", justifyContent: "space-between", fontSize: 9, fontWeight: 700, letterSpacing: 0.5, textTransform: "uppercase", color: "#9a8e80" }}>
        <span>Preset · Custom</span>
        <span>Pre-amp · 0 dB</span>
      </div>
    </div>
  );
}

// ============================================================
// THEME PICKER — Settings → Theme
// ============================================================
const THEMES = [
  { id: 1, name: "Linen", sub: "Warm light · text-forward", swatch: "#f4f1ec", ink: "#1a1714" },
  { id: 2, name: "Paper", sub: "Minimal · big art", swatch: "#faf8f4", ink: "#1a1714" },
  { id: 3, name: "Ink", sub: "True dark · terracotta", swatch: "#0e0d0c", ink: "oklch(0.7 0.12 40)" },
  { id: 4, name: "Card", sub: "Floating card surface", swatch: "#eeeae3", ink: "#1a1714" },
];
function ThemePicker({ selectedIdx = 1, currentId = 2 }) {
  return (
    <div style={{
      width: 320, height: 240,
      background: "#f4f1ec",
      fontFamily: "'Nunito', system-ui, sans-serif",
      color: "#1a1714",
      position: "relative",
    }}>
      <div style={{
        padding: "8px 14px 7px",
        borderBottom: "1px solid rgba(26,23,20,0.08)",
        display: "flex", justifyContent: "space-between", alignItems: "center",
      }}>
        <div style={{ display: "flex", alignItems: "center", gap: 6 }}>
          <span style={{ fontSize: 14, fontWeight: 700, color: "#9a8e80", lineHeight: 1 }}>‹</span>
          <span style={{ fontSize: 12.5, fontWeight: 700, letterSpacing: -0.1 }}>Theme</span>
        </div>
        <span style={{ fontSize: 10, fontWeight: 700, color: "#9a8e80" }}>{THEMES.length} themes</span>
      </div>

      <div style={{ padding: "5px 6px" }}>
        {THEMES.map((th, i) => {
          const sel = i === selectedIdx;
          const cur = th.id === currentId;
          return (
            <div key={th.id} style={{
              display: "flex", alignItems: "center", gap: 10,
              padding: "6px 8px", margin: "2px 0",
              borderRadius: 4,
              background: sel ? "#1a1714" : "transparent",
              color: sel ? "#f4f1ec" : "#1a1714",
            }}>
              {/* Swatch */}
              <div style={{
                width: 32, height: 32, borderRadius: 3,
                background: th.swatch,
                border: sel ? "1px solid rgba(244,241,236,0.3)" : "1px solid rgba(26,23,20,0.15)",
                display: "flex", alignItems: "center", justifyContent: "center",
                flexShrink: 0,
                position: "relative",
              }}>
                <div style={{ width: 14, height: 4, background: th.ink, borderRadius: 1, opacity: 0.85 }} />
                <div style={{ position: "absolute", bottom: 4, left: 4, width: 10, height: 1.5, background: th.ink, opacity: 0.4, borderRadius: 1 }} />
              </div>
              <div style={{ flex: 1, minWidth: 0 }}>
                <div style={{ fontSize: 12, fontWeight: 700, letterSpacing: -0.1 }}>{th.name}</div>
                <div style={{ fontSize: 9.5, fontWeight: 500, color: sel ? "rgba(244,241,236,0.7)" : "#7a7068", marginTop: 1 }}>
                  {th.sub}
                </div>
              </div>
              {cur && (
                <span style={{ fontSize: 9, fontWeight: 800, letterSpacing: 0.6, textTransform: "uppercase",
                  color: sel ? "#f4f1ec" : "#9a8e80", flexShrink: 0 }}>
                  Current
                </span>
              )}
            </div>
          );
        })}
      </div>
    </div>
  );
}

// ============================================================
// WPS INFO PAGES — alternate Now Playing displays
// (cycle by pressing center button on Now Playing)
// ============================================================

// Page A: Big art (image-dominant)
function NowPlayingBigArt() {
  return (
    <div style={{
      width: 320, height: 240,
      background: "#0e0d0c",
      fontFamily: "'Nunito', system-ui, sans-serif",
      position: "relative",
      color: "#e8e4dd",
    }}>
      <div style={{ position: "absolute", inset: 0, display: "flex", alignItems: "center", justifyContent: "center" }}>
        <AlbumArt size={180} hue={50} />
      </div>
      {/* Bottom title gradient */}
      <div style={{
        position: "absolute", bottom: 0, left: 0, right: 0,
        padding: "30px 14px 10px",
        background: "linear-gradient(180deg, rgba(14,13,12,0) 0%, rgba(14,13,12,0.9) 60%)",
      }}>
        <div style={{ fontSize: 13, fontWeight: 700, letterSpacing: -0.1 }}>Avril 14th</div>
        <div style={{ fontSize: 10, fontWeight: 500, color: "#a89e92", marginTop: 1 }}>Aphex Twin · Drukqs</div>
      </div>
      {/* Top status */}
      <div style={{ position: "absolute", top: 7, left: 10, right: 10, display: "flex", justifyContent: "space-between", alignItems: "center", fontSize: 9, fontWeight: 700, letterSpacing: 0.4, color: "#e8e4dd", textTransform: "uppercase" }}>
        <span>1 of 3</span>
        <Battery level={0.78} color="#e8e4dd" />
      </div>
    </div>
  );
}

// Page B: Peak meter (audio levels)
function NowPlayingPeakMeter() {
  // Mock levels: array of 0-1 per channel
  const L = 0.72, R = 0.65;
  const peakL = 0.84, peakR = 0.78;
  const renderMeter = (level, peak, label) => {
    const segs = 24;
    return (
      <div style={{ display: "flex", flexDirection: "column", alignItems: "center", gap: 5 }}>
        <span style={{ fontSize: 10, fontWeight: 800, color: "#5a5048", letterSpacing: 1 }}>{label}</span>
        <div style={{ display: "flex", flexDirection: "column", gap: 2 }}>
          {Array.from({ length: segs }).map((_, i) => {
            const ratio = (segs - i) / segs;
            const lit = ratio <= level;
            const isPeak = Math.abs(ratio - peak) < (1 / segs);
            const color =
              ratio > 0.85 ? "#c4502a" :
              ratio > 0.65 ? "#c08c2a" :
              "#1a1714";
            return (
              <div key={i} style={{
                width: 28, height: 4,
                background: lit ? color : (isPeak ? color : "rgba(26,23,20,0.08)"),
                opacity: lit ? 1 : (isPeak ? 0.5 : 1),
                borderRadius: 1,
              }} />
            );
          })}
        </div>
        <span style={{ fontSize: 8, fontWeight: 700, color: "#9a8e80", fontVariantNumeric: "tabular-nums" }}>
          {Math.round(20 * Math.log10(level)) || -1} dB
        </span>
      </div>
    );
  };
  return (
    <div style={{
      width: 320, height: 240,
      background: "#f4f1ec",
      fontFamily: "'Nunito', system-ui, sans-serif",
      color: "#1a1714",
      position: "relative",
      padding: "8px 0 0",
    }}>
      <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", padding: "0 12px 6px", borderBottom: "1px solid rgba(26,23,20,0.08)" }}>
        <span style={{ fontSize: 11, fontWeight: 700, letterSpacing: -0.1 }}>Avril 14th</span>
        <span style={{ fontSize: 9, fontWeight: 700, letterSpacing: 0.5, color: "#9a8e80", textTransform: "uppercase" }}>Page 2 of 3</span>
      </div>
      <div style={{ display: "flex", justifyContent: "center", gap: 22, marginTop: 14 }}>
        {renderMeter(L, peakL, "L")}
        {renderMeter(R, peakR, "R")}
      </div>
      <div style={{ position: "absolute", bottom: 8, left: 14, right: 14, display: "flex", justifyContent: "space-between", fontSize: 9, fontWeight: 700, letterSpacing: 0.5, textTransform: "uppercase", color: "#7a7068" }}>
        <span>Pre-amp 0dB</span>
        <span>1:42 / 4:00</span>
      </div>
    </div>
  );
}

// Page C: Detailed track info
function NowPlayingTrackInfo() {
  const rows = [
    ["Title", "Avril 14th"],
    ["Artist", "Aphex Twin"],
    ["Album", "Drukqs (Disc 1)"],
    ["Track", "4 of 11"],
    ["Year", "2001"],
    ["Genre", "IDM / Electronic"],
    ["Format", "MP3 · 192 kbps"],
    ["Sample rate", "44.1 kHz"],
    ["Length", "4:00"],
    ["Path", "/Music/Aphex Twin/Drukqs/04 Avril 14th.mp3"],
  ];
  return (
    <div style={{
      width: 320, height: 240,
      background: "#f4f1ec",
      fontFamily: "'Nunito', system-ui, sans-serif",
      color: "#1a1714",
      position: "relative",
    }}>
      <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", padding: "8px 12px 6px", borderBottom: "1px solid rgba(26,23,20,0.08)" }}>
        <span style={{ fontSize: 11, fontWeight: 700, letterSpacing: -0.1 }}>Track Info</span>
        <span style={{ fontSize: 9, fontWeight: 700, letterSpacing: 0.5, color: "#9a8e80", textTransform: "uppercase" }}>Page 3 of 3</span>
      </div>
      <div style={{ padding: "6px 14px" }}>
        {rows.map(([k, v]) => (
          <div key={k} style={{ display: "flex", padding: "1.5px 0", fontSize: 10, lineHeight: 1.3 }}>
            <span style={{ flex: "0 0 78px", fontWeight: 700, color: "#9a8e80", letterSpacing: 0.3, textTransform: "uppercase", fontSize: 8.5 }}>{k}</span>
            <span style={{ flex: 1, fontWeight: 600, color: "#1a1714", fontFamily: k === "Path" ? "'JetBrains Mono', monospace" : "inherit", fontSize: k === "Path" ? 9 : 10, whiteSpace: "nowrap", overflow: "hidden", textOverflow: "ellipsis" }}>{v}</span>
          </div>
        ))}
      </div>
    </div>
  );
}

// ============================================================
// CHARGING SCREEN — full-screen battery view (when plugged in & off)
// ============================================================
function ChargingScreen({ level = 0.62, charging = true }) {
  const pct = Math.round(level * 100);
  return (
    <div style={{
      width: 320, height: 240,
      background: "#0e0d0c",
      fontFamily: "'Nunito', system-ui, sans-serif",
      display: "flex", flexDirection: "column",
      alignItems: "center", justifyContent: "center",
      gap: 14, color: "#e8e4dd",
      position: "relative",
    }}>
      {/* Big battery */}
      <div style={{ position: "relative" }}>
        <svg width="160" height="74" viewBox="0 0 160 74">
          <rect x="2" y="2" width="146" height="70" rx="6" fill="none" stroke="#5a5048" strokeWidth="2.5" />
          <rect x="151" y="22" width="7" height="30" rx="2" fill="#5a5048" />
          <rect x="9" y="9" width={Math.max(8, 132 * level)} height="56" rx="2"
                fill={charging ? "oklch(0.78 0.16 145)" : level < 0.2 ? "oklch(0.65 0.18 30)" : "#e8e4dd"} />
        </svg>
        {charging && (
          <svg width="40" height="56" viewBox="0 0 40 56" style={{
            position: "absolute", top: 9, left: "50%", transform: "translateX(-50%)",
          }}>
            <polygon points="22,2 4,30 18,30 14,54 36,24 22,24" fill="#0e0d0c" stroke="#0e0d0c" strokeWidth="0.5" />
          </svg>
        )}
      </div>

      <div style={{
        fontSize: 36, fontWeight: 800, letterSpacing: -1,
        fontVariantNumeric: "tabular-nums", lineHeight: 1,
      }}>
        {pct}<span style={{ fontSize: 18, fontWeight: 600, color: "#a89e92", marginLeft: 1 }}>%</span>
      </div>
      <div style={{ fontSize: 11, fontWeight: 700, letterSpacing: 1.2, textTransform: "uppercase", color: charging ? "oklch(0.78 0.16 145)" : "#7a736a", marginTop: -8 }}>
        {charging ? "Charging" : "Not charging"}
      </div>
      <div style={{ fontSize: 9.5, fontWeight: 600, color: "#7a736a", marginTop: -8 }}>
        {charging ? `Full in ${Math.max(1, Math.round((1 - level) * 180))} min` : "Connect cable"}
      </div>

      {/* Tiny clock */}
      <div style={{ position: "absolute", top: 8, right: 12, fontSize: 10, fontWeight: 700, color: "#7a736a", fontVariantNumeric: "tabular-nums", letterSpacing: 0.4 }}>
        2:14 AM
      </div>
    </div>
  );
}

// ============================================================
// LOCKED — full-screen Hold confirmation (long press / first activation)
// ============================================================
function LockedScreen({ playing = true }) {
  return (
    <div style={{
      width: 320, height: 240,
      background: "#f4f1ec",
      fontFamily: "'Nunito', system-ui, sans-serif",
      color: "#1a1714",
      position: "relative",
    }}>
      {/* Dim now-playing context behind */}
      <div style={{ position: "absolute", inset: 0, opacity: 0.35 }}>
        <Theme1NowPlaying />
      </div>

      {/* Persistent corner lock — stays after the plate dismisses */}
      <div style={{ position: "absolute", top: 6, right: 8, color: "#1a1714", opacity: 0.6 }}>
        <svg width="9" height="11" viewBox="0 0 9 11" fill="none">
          <rect x="0.5" y="4.5" width="8" height="6" rx="1" fill="currentColor" />
          <path d="M2.2 4.5V3a2.3 2.3 0 1 1 4.6 0v1.5" stroke="currentColor" strokeWidth="1.1" fill="none" />
        </svg>
      </div>

      {/* Center plate */}
      <div style={{
        position: "absolute", top: "50%", left: "50%",
        transform: "translate(-50%, -50%)",
        background: "#1a1714", color: "#f4f1ec",
        width: 180, height: 110,
        padding: "16px 22px",
        borderRadius: 6,
        display: "flex", flexDirection: "column", alignItems: "center", justifyContent: "center", gap: 10,
        boxShadow: "0 8px 24px -8px rgba(0,0,0,0.5)",
      }}>
        <svg width="28" height="32" viewBox="0 0 28 32">
          <rect x="5" y="14" width="18" height="14" rx="2" fill="#f4f1ec" />
          <path d="M9 14 V9 a5 5 0 0 1 10 0 V14" fill="none" stroke="#f4f1ec" strokeWidth="2.2" strokeLinecap="round" />
          <circle cx="14" cy="21" r="1.6" fill="#1a1714" />
        </svg>
        <div style={{ fontSize: 13, fontWeight: 800, letterSpacing: 0.6, textTransform: "uppercase" }}>Locked</div>
      </div>
    </div>
  );
}

// ============================================================
// UNLOCKED — confirmation flash when Hold is turned off
// ============================================================
function UnlockedScreen() {
  return (
    <div style={{
      width: 320, height: 240,
      background: "#f4f1ec",
      fontFamily: "'Nunito', system-ui, sans-serif",
      color: "#1a1714",
      position: "relative",
    }}>
      <div style={{ position: "absolute", inset: 0, opacity: 0.55 }}>
        <Theme1NowPlaying />
      </div>

      {/* Center plate — open lock */}
      <div style={{
        position: "absolute", top: "50%", left: "50%",
        transform: "translate(-50%, -50%)",
        background: "#f4f1ec", color: "#1a1714",
        width: 180, height: 110,
        padding: "16px 22px",
        borderRadius: 6,
        display: "flex", flexDirection: "column", alignItems: "center", justifyContent: "center", gap: 10,
        boxShadow: "0 0 0 1px rgba(26,23,20,0.12), 0 8px 24px -8px rgba(0,0,0,0.25)",
      }}>
        <svg width="26" height="30" viewBox="0 0 26 30">
          <rect x="4" y="13" width="18" height="14" rx="2" fill="#1a1714" />
          {/* open shackle — leans right */}
          <path d="M8 13 V8 a5 5 0 0 1 10 0" fill="none" stroke="#1a1714" strokeWidth="2.2" strokeLinecap="round" transform="translate(-3 0)" />
          <circle cx="13" cy="20" r="1.4" fill="#f4f1ec" />
        </svg>
        <div style={{ fontSize: 12, fontWeight: 800, letterSpacing: 0.6, textTransform: "uppercase" }}>Unlocked</div>
      </div>
    </div>
  );
}

window.ChargingScreen = ChargingScreen;
window.LockedScreen = LockedScreen;
window.UnlockedScreen = UnlockedScreen;

window.BootSplash = BootSplash;
window.ShutdownScreen = ShutdownScreen;
window.FileBrowser = FileBrowser;
window.Equalizer = Equalizer;
window.ThemePicker = ThemePicker;
window.NowPlayingBigArt = NowPlayingBigArt;
window.NowPlayingPeakMeter = NowPlayingPeakMeter;
window.NowPlayingTrackInfo = NowPlayingTrackInfo;
window.ShutdownScreen = ShutdownScreen;
window.FileBrowser = FileBrowser;
window.Equalizer = Equalizer;
window.ThemePicker = ThemePicker;
window.NowPlayingBigArt = NowPlayingBigArt;
window.NowPlayingPeakMeter = NowPlayingPeakMeter;
window.NowPlayingTrackInfo = NowPlayingTrackInfo;