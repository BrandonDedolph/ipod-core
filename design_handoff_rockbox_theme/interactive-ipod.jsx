// Interactive iPod prototype — full navigation tree
// Stack-based: each screen is a frame with a list, selection, and an action

function InteractiveIPod({ themeId = 1, scale = 2 }) {
  // Stack of frames. Top = current.
  // Each frame: { type, sel }
  const [stack, setStack] = React.useState([{ type: "main", sel: 0 }]);
  const [playing, setPlaying] = React.useState(true);
  const [progress, setProgress] = React.useState(0.43);
  const [hold, setHold] = React.useState(false);
  const [holdFlash, setHoldFlash] = React.useState(null); // null | "locked" | "unlocked"
  const holdFlashTimer = React.useRef(null);
  const flashHold = (kind) => {
    setHoldFlash(kind);
    if (holdFlashTimer.current) clearTimeout(holdFlashTimer.current);
    holdFlashTimer.current = setTimeout(() => setHoldFlash(null), 1100);
  };
  const toggleHold = () => {
    setHold(h => {
      flashHold(h ? "unlocked" : "locked");
      return !h;
    });
  };
  const [volume, setVolume] = React.useState(0.6);
  const [showVolume, setShowVolume] = React.useState(false);
  const volumeTimer = React.useRef(null);
  const flashVolume = () => {
    setShowVolume(true);
    if (volumeTimer.current) clearTimeout(volumeTimer.current);
    volumeTimer.current = setTimeout(() => setShowVolume(false), 1200);
  };

  const blockedByHold = () => {
    if (!hold) return false;
    flashHold("locked");
    return true;
  };

  React.useEffect(() => {
    const top = stack[stack.length - 1];
    if (!playing || top.type !== "playing") return;
    const id = setInterval(() => {
      setProgress((p) => (p >= 1 ? 0 : p + 0.003));
    }, 200);
    return () => clearInterval(id);
  }, [playing, stack]);

  const top = stack[stack.length - 1];

  // Lengths per list type for clamping
  const listLen = (type) => {
    const fromMenus = (window.MENU_LENGTHS || {})[type];
    if (fromMenus) return fromMenus;
    return ({ main: 6, albumDetail: 7, playlistDetail: 7 }[type] || 1);
  };

  const setSel = (fn) => setStack((s) => {
    const ns = [...s];
    const t = { ...ns[ns.length - 1] };
    t.sel = fn(t.sel);
    ns[ns.length - 1] = t;
    return ns;
  });

  const push = (type) => setStack((s) => [...s, { type, sel: 0 }]);
  const pop = () => setStack((s) => s.length > 1 ? s.slice(0, -1) : s);

  const onMenu = () => {
    if (blockedByHold()) return;
    if (top.type === "playing") pop();
    else pop();
  };

  const onCenter = () => {
    if (blockedByHold()) return;
    if (top.type === "main") {
      const t = ["music", "playlists", "podcasts", "audiobooks", "settings", "playing"][top.sel];
      if (t === "playing") push("playing"); else push(t);
    } else if (top.type === "music") {
      const t = ["playlistDetail", "artists", "albums", "songs", "genres", null, "audiobooks"][top.sel];
      if (t) push(t);
    } else if (top.type === "albums") {
      push("albumDetail");
    } else if (top.type === "playlists") {
      push("playlistDetail");
    } else if (top.type === "podcasts") {
      push("podEpisodes");
    } else if (top.type === "settings") {
      const t = ["settingsPlayback", "settingsSound", null, null, null, null, null, null][top.sel];
      if (t) push(t);
    } else if (top.type === "albumDetail" || top.type === "playlistDetail" || top.type === "songs" || top.type === "podEpisodes") {
      push("playing");
    } else if (top.type === "playing") {
      setPlaying((p) => !p);
    }
  };

  const onPrev = () => {
    if (blockedByHold()) return;
    if (top.type === "playing") {
      setProgress((p) => Math.max(0, p - 0.05));
    } else {
      setSel((i) => Math.max(0, i - 1));
    }
  };
  const onNext = () => {
    if (blockedByHold()) return;
    if (top.type === "playing") {
      setProgress((p) => Math.min(1, p + 0.05));
    } else {
      setSel((i) => Math.min(listLen(top.type) - 1, i + 1));
    }
  };
  const onWheel = (delta) => {
    if (blockedByHold()) return;
    if (top.type === "playing") {
      setVolume((v) => Math.max(0, Math.min(1, v + delta * 0.04)));
      flashVolume();
    } else {
      setSel((i) => {
        const len = listLen(top.type);
        return Math.max(0, Math.min(len - 1, i + delta));
      });
    }
  };

  const onPlay = () => {
    if (blockedByHold()) return;
    if (top.type !== "playing") push("playing");
    else setPlaying((p) => !p);
  };

  const NowPlayingComp = [Theme1Live, Theme2Live, Theme3Live, Theme4Live][themeId - 1] || Theme1Live;
  const dark = themeId === 3;

  // Render the current screen
  let screen = null;
  switch (top.type) {
    case "main":             screen = <MainMenu selectedIdx={top.sel} dark={dark} />; break;
    case "music":            screen = <MusicMenu selectedIdx={top.sel} />; break;
    case "artists":          screen = <ArtistsList selectedIdx={top.sel} />; break;
    case "albums":           screen = <AlbumsList selectedIdx={top.sel} />; break;
    case "songs":            screen = <SongsList selectedIdx={top.sel} />; break;
    case "genres":           screen = <GenresList selectedIdx={top.sel} />; break;
    case "playlists":        screen = <PlaylistsList selectedIdx={top.sel} />; break;
    case "podcasts":         screen = <PodcastsList selectedIdx={top.sel} />; break;
    case "podEpisodes":      screen = <PodcastEpisodes selectedIdx={top.sel} />; break;
    case "audiobooks":       screen = <AudiobooksList selectedIdx={top.sel} />; break;
    case "settings":         screen = <SettingsMenu selectedIdx={top.sel} />; break;
    case "settingsPlayback": screen = <SettingsPlayback selectedIdx={top.sel} />; break;
    case "settingsSound":    screen = <SettingsSound selectedIdx={top.sel} />; break;
    case "albumDetail":      screen = <AlbumDetail selectedIdx={top.sel} nowPlayingIdx={3} />; break;
    case "playlistDetail":   screen = <PlaylistDetail selectedIdx={top.sel} nowPlayingIdx={1} />; break;
    case "playing":          screen = <NowPlayingComp progress={progress} playing={playing} hold={hold} />; break;
    default:                 screen = <MainMenu selectedIdx={0} dark={dark} />;
  }

  // For non-Now-Playing screens, overlay a small lock just left of the battery
  const isMenu = top.type !== "playing";

  return (
    <IPodVideo scale={scale} color={dark ? "black" : "silver"}
      hold={hold} onHoldToggle={toggleHold}
      onMenu={onMenu} onCenter={onCenter} onPrev={onPrev} onNext={onNext} onPlay={onPlay} onWheel={onWheel}>
      <div style={{ position: "relative", width: 320, height: 240 }}>
        {screen}
        {/* Inline lock indicator for menu screens — sits just left of the menu's battery */}
        {hold && isMenu && (
          <div style={{ position: "absolute", top: 7, right: 32, color: dark ? "#a89e92" : "#7a7068" }}>
            <svg width="8" height="10" viewBox="0 0 9 11" fill="none" style={{ display: "block" }}>
              <rect x="0.5" y="4.5" width="8" height="6" rx="1" fill="currentColor" />
              <path d="M2.2 4.5V3a2.3 2.3 0 1 1 4.6 0v1.5" stroke="currentColor" strokeWidth="1.1" fill="none" />
            </svg>
          </div>
        )}
        {/* Volume overlay (shown briefly while spinning the wheel on Now Playing) */}
        {showVolume && top.type === "playing" && (
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
            {/* Speaker icon */}
            <svg width="14" height="14" viewBox="0 0 14 14" fill="currentColor">
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
        )}
        {holdFlash && (
          <div style={{
            position: "absolute", inset: 0,
            display: "flex", alignItems: "center", justifyContent: "center",
            background: holdFlash === "locked" ? "rgba(0,0,0,0.4)" : "rgba(0,0,0,0.15)",
            animation: "holdFlash 1.1s ease-out",
            fontFamily: "'Nunito', sans-serif",
            pointerEvents: "none",
          }}>
            <div style={{
              background: holdFlash === "locked" ? "#1a1714" : "#f4f1ec",
              color: holdFlash === "locked" ? "#f4f1ec" : "#1a1714",
              width: 180, height: 110,
              borderRadius: 6,
              display: "flex", flexDirection: "column", alignItems: "center", justifyContent: "center", gap: 10,
              boxShadow: holdFlash === "locked"
                ? "0 8px 24px -8px rgba(0,0,0,0.5)"
                : "0 0 0 1px rgba(26,23,20,0.12), 0 8px 24px -8px rgba(0,0,0,0.25)",
            }}>
              {holdFlash === "locked" ? (
                <svg width="28" height="32" viewBox="0 0 28 32">
                  <rect x="5" y="14" width="18" height="14" rx="2" fill="#f4f1ec" />
                  <path d="M9 14 V9 a5 5 0 0 1 10 0 V14" fill="none" stroke="#f4f1ec" strokeWidth="2.2" strokeLinecap="round" />
                  <circle cx="14" cy="21" r="1.6" fill="#1a1714" />
                </svg>
              ) : (
                <svg width="26" height="30" viewBox="0 0 26 30">
                  <rect x="4" y="13" width="18" height="14" rx="2" fill="#1a1714" />
                  <path d="M8 13 V8 a5 5 0 0 1 10 0" fill="none" stroke="#1a1714" strokeWidth="2.2" strokeLinecap="round" transform="translate(-3 0)" />
                  <circle cx="13" cy="20" r="1.4" fill="#f4f1ec" />
                </svg>
              )}
              <div style={{ fontSize: 13, fontWeight: 800, letterSpacing: 0.6, textTransform: "uppercase" }}>
                {holdFlash === "locked" ? "Locked" : "Unlocked"}
              </div>
            </div>
          </div>
        )}
        <style>{`@keyframes holdFlash { 0%{opacity:0} 10%{opacity:1} 70%{opacity:1} 100%{opacity:0} }`}</style>
      </div>
    </IPodVideo>
  );
}

// Live now-playing components (kept here for self-containment)
const fmt = (sec) => {
  const m = Math.floor(sec / 60);
  const s = Math.floor(sec % 60);
  return `${m}:${s.toString().padStart(2, "0")}`;
};
const TOTAL = 240;

const LockGlyph = ({ color = "currentColor" }) => (
  <svg width="8" height="10" viewBox="0 0 9 11" fill="none" style={{ display: "block" }}>
    <rect x="0.5" y="4.5" width="8" height="6" rx="1" fill={color} />
    <path d="M2.2 4.5V3a2.3 2.3 0 1 1 4.6 0v1.5" stroke={color} strokeWidth="1.1" fill="none" />
  </svg>
);

function Theme1Live({ progress, playing, hold }) {
  const elapsed = TOTAL * progress;
  const remain = TOTAL - elapsed;
  return (
    <div style={{ width: 320, height: 240, background: "#f4f1ec", fontFamily: "'Nunito', system-ui, sans-serif", color: "#1a1714", position: "relative", overflow: "hidden" }}>
      <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", padding: "8px 12px", fontSize: 11, fontWeight: 600 }}>
        <span style={{ letterSpacing: 0.3 }}>{playing ? "Now Playing" : "Paused"}</span>
        <div style={{ display: "flex", alignItems: "center", gap: 6 }}>
          {hold && <LockGlyph color="#1a1714" />}
          <Battery level={0.78} />
        </div>
      </div>
      <div style={{ padding: "8px 18px 0", display: "flex", gap: 14 }}>
        <AlbumArt size={88} hue={30} />
        <div style={{ flex: 1, paddingTop: 2, minWidth: 0 }}>
          <div style={{ fontSize: 9, fontWeight: 700, letterSpacing: 1, color: "#9a8e80", textTransform: "uppercase" }}>Track 04 of 11</div>
          <div style={{ fontSize: 17, fontWeight: 700, lineHeight: 1.15, marginTop: 4, letterSpacing: -0.2 }}>Avril 14th</div>
          <div style={{ fontSize: 12, fontWeight: 500, marginTop: 2, color: "#5a5048" }}>Aphex Twin</div>
          <div style={{ fontSize: 11, fontWeight: 400, marginTop: 1, color: "#9a8e80" }}>Drukqs</div>
        </div>
      </div>
      <div style={{ position: "absolute", bottom: 24, left: 18, right: 18 }}>
        <div style={{ display: "flex", justifyContent: "space-between", marginBottom: 6, fontSize: 10, fontWeight: 600, color: "#5a5048", fontVariantNumeric: "tabular-nums" }}>
          <span>{fmt(elapsed)}</span>
          <span style={{ opacity: 0.5 }}>−{fmt(remain)}</span>
        </div>
        <Progress value={progress} width={284} fg="#1a1714" bg="rgba(26,23,20,0.12)" />
      </div>
    </div>
  );
}

function Theme2Live({ progress, playing, hold }) {
  const elapsed = TOTAL * progress;
  return (
    <div style={{ width: 320, height: 240, background: "#faf8f4", fontFamily: "'Nunito', system-ui, sans-serif", color: "#1a1714", position: "relative", overflow: "hidden" }}>
      <div style={{ display: "flex", flexDirection: "column", alignItems: "center", paddingTop: 16 }}>
        <AlbumArt size={120} hue={50} />
      </div>
      <div style={{ position: "absolute", bottom: 0, left: 0, right: 0, padding: "0 16px 14px", textAlign: "center" }}>
        <div style={{ fontSize: 14, fontWeight: 700, letterSpacing: -0.1 }}>Avril 14th</div>
        <div style={{ fontSize: 11, fontWeight: 500, color: "#7a7068", marginTop: 1 }}>Aphex Twin · Drukqs</div>
        <div style={{ marginTop: 8, display: "flex", alignItems: "center", gap: 8, justifyContent: "center", fontSize: 10, fontWeight: 600, color: "#5a5048", fontVariantNumeric: "tabular-nums" }}>
          <span>{fmt(elapsed)}</span>
          <Progress value={progress} width={180} height={2} fg="#1a1714" bg="rgba(26,23,20,0.1)" />
          <span>4:00</span>
        </div>
      </div>
      {/* Status row: play/pause + battery */}
      <div style={{ position: "absolute", top: 8, left: 10, display: "flex", alignItems: "center", gap: 5 }}>
        {playing ? (
          <svg width="7" height="9" viewBox="0 0 7 9" style={{ display: "block" }}>
            <polygon points="0.5,0.5 6.5,4.5 0.5,8.5" fill="#7a7068" />
          </svg>
        ) : (
          <svg width="7" height="9" viewBox="0 0 7 9" style={{ display: "block" }}>
            <rect x="0.5" y="0.5" width="2" height="8" fill="#7a7068" />
            <rect x="4.5" y="0.5" width="2" height="8" fill="#7a7068" />
          </svg>
        )}
        <span style={{ fontSize: 9, fontWeight: 700, color: "#7a7068", letterSpacing: 0.6, textTransform: "uppercase" }}>
          {playing ? "Playing" : "Paused"}
        </span>
      </div>
      <div style={{ position: "absolute", top: 8, right: 10, display: "flex", alignItems: "center", gap: 6 }}>
        {hold && <LockGlyph color="#7a7068" />}
        <Battery level={0.78} color="#7a7068" />
      </div>
    </div>
  );
}

function Theme3Live({ progress, playing, hold }) {
  const elapsed = TOTAL * progress;
  const accent = "oklch(0.7 0.12 40)";
  return (
    <div style={{ width: 320, height: 240, background: "#0e0d0c", fontFamily: "'Nunito', system-ui, sans-serif", color: "#e8e4dd", position: "relative", overflow: "hidden" }}>
      <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", padding: "8px 12px", fontSize: 10, fontWeight: 600, color: "#7a736a", letterSpacing: 0.5, textTransform: "uppercase" }}>
        <span>{playing ? "Playing" : "Paused"}</span>
        <div style={{ display: "flex", alignItems: "center", gap: 6 }}>
          {hold && <LockGlyph color="#7a736a" />}
          <Battery level={0.78} color="#7a736a" />
        </div>
      </div>
      <div style={{ padding: "10px 18px 0", display: "flex", gap: 14 }}>
        <div style={{ width: 80, height: 80, borderRadius: 3, background: `repeating-linear-gradient(135deg, oklch(0.32 0.02 40) 0 5px, oklch(0.28 0.02 40) 5px 10px)`, border: "1px solid rgba(255,255,255,0.06)", flexShrink: 0 }} />
        <div style={{ flex: 1, minWidth: 0, paddingTop: 4 }}>
          <div style={{ fontSize: 16, fontWeight: 700, color: "#f4ede2", letterSpacing: -0.2, lineHeight: 1.15 }}>Avril 14th</div>
          <div style={{ fontSize: 11, fontWeight: 500, marginTop: 3, color: "#a89e92" }}>Aphex Twin</div>
          <div style={{ fontSize: 10, fontWeight: 400, marginTop: 1, color: "#6a635a" }}>Drukqs · 2001</div>
          <div style={{ marginTop: 8, display: "inline-flex", alignItems: "center", gap: 4, fontSize: 9, fontWeight: 700, color: accent, letterSpacing: 0.8, textTransform: "uppercase" }}>
            <span style={{ width: 4, height: 4, borderRadius: "50%", background: accent }} />
            <span>4 / 11</span>
          </div>
        </div>
      </div>
      <div style={{ position: "absolute", bottom: 18, left: 18, right: 18 }}>
        <Progress value={progress} width={284} height={2} fg={accent} bg="rgba(255,255,255,0.08)" />
        <div style={{ display: "flex", justifyContent: "space-between", marginTop: 5, fontSize: 9, fontWeight: 600, color: "#7a736a", fontVariantNumeric: "tabular-nums", letterSpacing: 0.5 }}>
          <span>{fmt(elapsed)}</span>
          <span>4:00</span>
        </div>
      </div>
    </div>
  );
}

function Theme4Live({ progress, playing, hold }) {
  const elapsed = TOTAL * progress;
  return (
    <div style={{ width: 320, height: 240, background: "#eeeae3", fontFamily: "'Nunito', system-ui, sans-serif", color: "#1a1714", position: "relative", overflow: "hidden" }}>
      <div style={{ display: "flex", justifyContent: "space-between", alignItems: "center", padding: "7px 12px", fontSize: 10, fontWeight: 700, color: "#5a5048", letterSpacing: 0.5 }}>
        <span>4 OF 11</span>
        <span>{fmt(elapsed)} / 4:00</span>
        <div style={{ display: "flex", alignItems: "center", gap: 6 }}>
          {hold && <LockGlyph color="#5a5048" />}
          <Battery level={0.78} color="#5a5048" />
        </div>
      </div>
      <div style={{ margin: "8px 18px 0", background: "#faf8f4", borderRadius: 6, padding: 12, display: "flex", gap: 12, boxShadow: "0 1px 0 rgba(0,0,0,0.05), 0 0 0 1px rgba(0,0,0,0.04)" }}>
        <AlbumArt size={70} hue={70} />
        <div style={{ flex: 1, minWidth: 0, display: "flex", flexDirection: "column", justifyContent: "center" }}>
          <div style={{ fontSize: 14, fontWeight: 700, letterSpacing: -0.1, lineHeight: 1.15 }}>Avril 14th</div>
          <div style={{ fontSize: 11, fontWeight: 500, marginTop: 3, color: "#5a5048" }}>Aphex Twin{!playing && " · paused"}</div>
          <div style={{ fontSize: 10, fontWeight: 400, marginTop: 1, color: "#9a8e80" }}>Drukqs</div>
        </div>
      </div>
      <div style={{ position: "absolute", bottom: 18, left: 18, right: 18 }}>
        <div style={{ height: 6, borderRadius: 3, background: "rgba(26,23,20,0.1)", overflow: "hidden" }}>
          <div style={{ width: `${progress * 100}%`, height: "100%", background: "#1a1714", borderRadius: 3 }} />
        </div>
      </div>
    </div>
  );
}

window.InteractiveIPod = InteractiveIPod;
