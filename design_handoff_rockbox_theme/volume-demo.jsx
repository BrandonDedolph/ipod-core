// Standalone volume overlay + interactive demo

function VolumeOverlay({ volume = 0.6, dark = false }) {
  return (
    <div style={{
      position: "absolute",
      left: "50%",
      top: "50%",
      transform: "translate(-50%, -50%)",
      background: dark ? "rgba(20,18,16,0.92)" : "rgba(250,248,244,0.95)",
      color: dark ? "#f4ede2" : "#1a1714",
      border: dark ? "1px solid rgba(255,255,255,0.08)" : "1px solid rgba(26,23,20,0.1)",
      borderRadius: 6,
      padding: "10px 14px",
      display: "flex",
      alignItems: "center",
      gap: 10,
      fontFamily: "'Nunito', sans-serif",
      boxShadow: "0 6px 16px -8px rgba(0,0,0,0.3)",
      minWidth: 200,
    }}>
      <svg width="14" height="14" viewBox="0 0 14 14" fill="currentColor" style={{ flexShrink: 0 }}>
        <path d="M1 5h2.5L7 2v10L3.5 9H1z" />
        {volume > 0.05 && <path d="M9 5.2c0.7 0.4 1.1 1 1.1 1.8s-0.4 1.4-1.1 1.8" stroke="currentColor" strokeWidth="1" fill="none" />}
        {volume > 0.4 && <path d="M10 3.5c1.5 0.7 2.4 2 2.4 3.5s-0.9 2.8-2.4 3.5" stroke="currentColor" strokeWidth="1" fill="none" />}
      </svg>
      <div style={{ flex: 1 }}>
        <div style={{
          width: "100%",
          height: 6,
          background: dark ? "rgba(255,255,255,0.12)" : "rgba(26,23,20,0.12)",
          borderRadius: 3,
          overflow: "hidden",
        }}>
          <div style={{
            width: `${volume * 100}%`,
            height: "100%",
            background: dark ? "oklch(0.7 0.12 40)" : "#1a1714",
            transition: "width 0.1s",
          }} />
        </div>
      </div>
      <div style={{
        fontSize: 11, fontWeight: 700, fontVariantNumeric: "tabular-nums",
        minWidth: 28, textAlign: "right",
      }}>
        {Math.round(volume * 100)}
      </div>
    </div>
  );
}

function VolumeDemo() {
  const [vol, setVol] = React.useState(0.6);
  const [dark, setDark] = React.useState(false);
  return (
    <div style={{
      display: "flex", flexDirection: "column", gap: 14, alignItems: "center",
      fontFamily: "'Nunito', sans-serif",
    }}>
      <div style={{ position: "relative", width: 320, height: 240, boxShadow: "0 0 0 1px rgba(0,0,0,0.15), 0 4px 16px -6px rgba(0,0,0,0.2)" }}>
        {dark ? <Theme3NowPlaying /> : <Theme2NowPlaying playing={true} />}
        <VolumeOverlay volume={vol} dark={dark} />
      </div>
      <div style={{ display: "flex", alignItems: "center", gap: 10, width: 320 }}>
        <span style={{ fontSize: 11, fontWeight: 600, color: "#5a5048" }}>Volume</span>
        <input
          type="range"
          min="0" max="1" step="0.01"
          value={vol}
          onChange={(e) => setVol(parseFloat(e.target.value))}
          style={{ flex: 1 }}
        />
        <span style={{ fontSize: 11, fontWeight: 700, color: "#1a1714", minWidth: 32, textAlign: "right", fontVariantNumeric: "tabular-nums" }}>{Math.round(vol * 100)}%</span>
      </div>
      <label style={{ display: "flex", alignItems: "center", gap: 6, fontSize: 11, fontWeight: 600, color: "#5a5048", cursor: "pointer" }}>
        <input type="checkbox" checked={dark} onChange={(e) => setDark(e.target.checked)} />
        Show on dark theme
      </label>
    </div>
  );
}

window.VolumeOverlay = VolumeOverlay;
window.VolumeDemo = VolumeDemo;
