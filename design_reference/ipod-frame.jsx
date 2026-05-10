// iPod Classic Video frame - 5th gen
// Native screen: 320x240 px, displayed at 2x for visibility (640x480)

const IPOD_W = 320;
const IPOD_H = 240;

function IPodVideo({ children, scale = 2, color = "silver", showWheel = true, hold = false, onHoldToggle, onWheel, onCenter, onMenu, onPrev, onNext, onPlay }) {
  const onWheelRef = React.useRef(onWheel);
  React.useEffect(() => { onWheelRef.current = onWheel; }, [onWheel]);
  const screenW = IPOD_W * scale;
  const screenH = IPOD_H * scale;
  const padding = 24 * scale * 0.5;
  const wheelSize = screenW * 0.62;

  const bodyW = screenW + padding * 2;
  const bodyH = screenH + padding * 2 + (showWheel ? wheelSize + padding : 0) + 18 * scale;

  const isSilver = color === "silver";
  const bodyBg = isSilver
    ? "linear-gradient(180deg, #e8e8ea 0%, #d4d4d6 100%)"
    : "linear-gradient(180deg, #2a2a2c 0%, #18181a 100%)";
  const wheelBg = isSilver ? "#dadadc" : "#3a3a3c";
  const wheelText = isSilver ? "#5a5a5e" : "#b0b0b4";
  const centerBg = isSilver ? "#f0f0f2" : "#1f1f21";

  return (
    <div
      style={{
        width: bodyW,
        height: bodyH,
        background: bodyBg,
        borderRadius: 28,
        padding: padding,
        boxShadow: "0 30px 60px -20px rgba(0,0,0,0.25), 0 0 0 1px rgba(0,0,0,0.08), inset 0 1px 0 rgba(255,255,255,0.6)",
        display: "flex",
        flexDirection: "column",
        alignItems: "center",
        gap: padding,
        boxSizing: "content-box",
        fontFamily: "'Nunito', system-ui, sans-serif",
      }}
    >
      {/* Top edge with Hold switch */}
      <div style={{
        width: bodyW - padding * 2,
        height: 14 * scale,
        marginTop: -padding * 0.4,
        marginBottom: padding * 0.2,
        display: "flex",
        justifyContent: "flex-end",
        alignItems: "center",
        paddingRight: 6 * scale,
      }}>
        <button
          onClick={onHoldToggle}
          aria-label={hold ? "Hold (locked)" : "Hold (unlocked)"}
          title={hold ? "Hold — click to unlock" : "Click to lock (Hold)"}
          style={{
            background: isSilver ? "#cdcdcf" : "#222224",
            border: "none",
            width: 26 * scale,
            height: 9 * scale,
            borderRadius: 2,
            display: "flex",
            alignItems: "center",
            justifyContent: hold ? "flex-start" : "flex-end",
            padding: 1 * scale,
            cursor: "pointer",
            boxShadow: "inset 0 1px 2px rgba(0,0,0,0.15)",
            position: "relative",
          }}
        >
          {/* orange dot when locked */}
          {hold && (
            <span style={{
              position: "absolute",
              left: 2 * scale,
              top: "50%",
              transform: "translateY(-50%)",
              width: 3 * scale,
              height: 3 * scale,
              borderRadius: "50%",
              background: "oklch(0.65 0.16 50)",
            }} />
          )}
          <span style={{
            width: 10 * scale,
            height: 7 * scale,
            background: isSilver ? "#f4f4f6" : "#4a4a4c",
            borderRadius: 1.5,
            boxShadow: "0 1px 1px rgba(0,0,0,0.15), inset 0 1px 0 rgba(255,255,255,0.4)",
          }} />
        </button>
      </div>
      {/* Screen bezel */}
      <div
        style={{
          width: screenW,
          height: screenH,
          background: "#0a0a0c",
          borderRadius: 6,
          padding: 6,
          boxShadow: "inset 0 2px 6px rgba(0,0,0,0.6), 0 0 0 1px rgba(0,0,0,0.3)",
        }}
      >
        <div
          style={{
            width: screenW - 12,
            height: screenH - 12,
            overflow: "hidden",
            position: "relative",
            background: "#000",
          }}
        >
          <div
            style={{
              width: IPOD_W,
              height: IPOD_H,
              transform: `scale(${(screenW - 12) / IPOD_W})`,
              transformOrigin: "top left",
              position: "absolute",
              top: 0,
              left: 0,
            }}
          >
            {children}
          </div>
        </div>
      </div>

      {/* Click wheel */}
      {showWheel && (
        <div
          ref={(el) => {
            if (!el || el._wheelBound) return;
            el._wheelBound = true;
            let dragging = false;
            let lastAngle = 0;
            let accum = 0;
            const TICK = 0.35; // radians per tick (~20°)
            const angleAt = (e) => {
              const r = el.getBoundingClientRect();
              const cx = r.left + r.width / 2;
              const cy = r.top + r.height / 2;
              const x = (e.touches ? e.touches[0].clientX : e.clientX) - cx;
              const y = (e.touches ? e.touches[0].clientY : e.clientY) - cy;
              return Math.atan2(y, x);
            };
            const onDown = (e) => {
              if (e.target.closest("button")) return; // let buttons handle clicks
              dragging = true;
              lastAngle = angleAt(e);
              accum = 0;
              e.preventDefault();
            };
            const onMove = (e) => {
              if (!dragging) return;
              const a = angleAt(e);
              let d = a - lastAngle;
              if (d > Math.PI) d -= 2 * Math.PI;
              if (d < -Math.PI) d += 2 * Math.PI;
              accum += d;
              while (accum > TICK) {
                const cb = onWheelRef.current;
                if (cb) cb(1);
                accum -= TICK;
              }
              while (accum < -TICK) {
                const cb = onWheelRef.current;
                if (cb) cb(-1);
                accum += TICK;
              }
              lastAngle = a;
              e.preventDefault();
            };
            const onUp = () => { dragging = false; };
            el.addEventListener("mousedown", onDown);
            window.addEventListener("mousemove", onMove);
            window.addEventListener("mouseup", onUp);
            el.addEventListener("touchstart", onDown, { passive: false });
            window.addEventListener("touchmove", onMove, { passive: false });
            window.addEventListener("touchend", onUp);
          }}
          style={{
            width: wheelSize,
            height: wheelSize,
            borderRadius: "50%",
            background: wheelBg,
            position: "relative",
            display: "flex",
            alignItems: "center",
            justifyContent: "center",
            boxShadow: "inset 0 0 0 1px rgba(0,0,0,0.06), inset 0 2px 4px rgba(0,0,0,0.04)",
            cursor: onWheel ? "grab" : "default",
            touchAction: "none",
            userSelect: "none",
          }}
        >
          {/* MENU */}
          <button
            onClick={onMenu}
            style={{
              position: "absolute",
              top: wheelSize * 0.06,
              left: "50%",
              transform: "translateX(-50%)",
              background: "transparent",
              border: "none",
              color: wheelText,
              fontSize: wheelSize * 0.07,
              fontWeight: 700,
              letterSpacing: 1,
              fontFamily: "'Nunito', sans-serif",
              cursor: "pointer",
            }}
          >
            MENU
          </button>
          {/* Prev */}
          <button
            onClick={onPrev}
            aria-label="Previous"
            style={{
              position: "absolute",
              left: wheelSize * 0.05,
              top: "50%",
              transform: "translateY(-50%)",
              background: "transparent",
              border: "none",
              color: wheelText,
              cursor: "pointer",
              padding: 4,
            }}
          >
            <svg width={wheelSize * 0.09} height={wheelSize * 0.06} viewBox="0 0 24 16" fill="currentColor">
              <path d="M2 2v12M22 2L11 8l11 6z" stroke="currentColor" strokeWidth="2.5" strokeLinejoin="round" fill="currentColor" />
            </svg>
          </button>
          {/* Next */}
          <button
            onClick={onNext}
            aria-label="Next"
            style={{
              position: "absolute",
              right: wheelSize * 0.05,
              top: "50%",
              transform: "translateY(-50%)",
              background: "transparent",
              border: "none",
              color: wheelText,
              cursor: "pointer",
              padding: 4,
            }}
          >
            <svg width={wheelSize * 0.09} height={wheelSize * 0.06} viewBox="0 0 24 16" fill="currentColor">
              <path d="M22 2v12M2 2l11 6L2 14z" stroke="currentColor" strokeWidth="2.5" strokeLinejoin="round" fill="currentColor" />
            </svg>
          </button>
          {/* Play/Pause */}
          <button
            onClick={onPlay}
            aria-label="Play/Pause"
            style={{
              position: "absolute",
              bottom: wheelSize * 0.06,
              left: "50%",
              transform: "translateX(-50%)",
              background: "transparent",
              border: "none",
              color: wheelText,
              cursor: "pointer",
              padding: 4,
              display: "flex",
              gap: 3,
              alignItems: "center",
            }}
          >
            <svg width={wheelSize * 0.05} height={wheelSize * 0.05} viewBox="0 0 16 16" fill="currentColor">
              <rect x="2" y="2" width="3" height="12" />
              <rect x="7" y="2" width="3" height="12" />
            </svg>
            <svg width={wheelSize * 0.05} height={wheelSize * 0.05} viewBox="0 0 16 16" fill="currentColor">
              <path d="M3 2l11 6-11 6z" />
            </svg>
          </button>
          {/* Center button */}
          <button
            onClick={onCenter}
            style={{
              width: wheelSize * 0.34,
              height: wheelSize * 0.34,
              borderRadius: "50%",
              background: centerBg,
              border: "none",
              boxShadow: "inset 0 1px 2px rgba(0,0,0,0.08), 0 1px 2px rgba(0,0,0,0.04)",
              cursor: "pointer",
            }}
          />
        </div>
      )}
    </div>
  );
}

window.IPodVideo = IPodVideo;
window.IPOD_W = IPOD_W;
window.IPOD_H = IPOD_H;
