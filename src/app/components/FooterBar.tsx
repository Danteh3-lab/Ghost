import { Circle } from "lucide-react";

interface FooterBarProps {
  activeCount: number;
  idleCount: number;
  offlineCount: number;
  totalKeystrokes: number;
  sessionStart: string;
}

export function FooterBar({ activeCount, idleCount, offlineCount, totalKeystrokes, sessionStart }: FooterBarProps) {
  return (
    <div
      className="flex items-center gap-6 px-4 border-t flex-shrink-0"
      style={{
        backgroundColor: "#0d1117",
        borderColor: "#30363d",
        height: 36,
      }}
    >
      <div className="flex items-center gap-5">
        <div className="flex items-center gap-1.5">
          <Circle size={6} fill="#3fb950" style={{ color: "#3fb950" }} />
          <span style={{ color: "#8b949e", fontFamily: "JetBrains Mono, monospace", fontSize: 10 }}>
            ACTIVE
          </span>
          <span style={{ color: "#3fb950", fontFamily: "JetBrains Mono, monospace", fontSize: 10, fontWeight: 600 }}>
            {activeCount}
          </span>
        </div>

        <div className="flex items-center gap-1.5">
          <Circle size={6} fill="#d29922" style={{ color: "#d29922" }} />
          <span style={{ color: "#8b949e", fontFamily: "JetBrains Mono, monospace", fontSize: 10 }}>
            IDLE
          </span>
          <span style={{ color: "#d29922", fontFamily: "JetBrains Mono, monospace", fontSize: 10, fontWeight: 600 }}>
            {idleCount}
          </span>
        </div>

        <div className="flex items-center gap-1.5">
          <Circle size={6} fill="#f85149" style={{ color: "#f85149" }} />
          <span style={{ color: "#8b949e", fontFamily: "JetBrains Mono, monospace", fontSize: 10 }}>
            OFFLINE
          </span>
          <span style={{ color: "#f85149", fontFamily: "JetBrains Mono, monospace", fontSize: 10, fontWeight: 600 }}>
            {offlineCount}
          </span>
        </div>
      </div>

      <div
        className="h-3 border-l"
        style={{ borderColor: "#30363d" }}
      />

      <div className="flex items-center gap-1.5">
        <span style={{ color: "#8b949e", fontFamily: "JetBrains Mono, monospace", fontSize: 10 }}>
          TOTAL KEYS
        </span>
        <span style={{ color: "#58a6ff", fontFamily: "JetBrains Mono, monospace", fontSize: 10, fontWeight: 600 }}>
          {totalKeystrokes.toLocaleString()}
        </span>
      </div>

      <div className="ml-auto flex items-center gap-1.5">
        <div
          className="w-1.5 h-1.5 rounded-full animate-pulse"
          style={{ backgroundColor: "#3fb950" }}
        />
        <span style={{ color: "#8b949e", fontFamily: "JetBrains Mono, monospace", fontSize: 10 }}>
          SESSION SINCE {sessionStart}
        </span>
      </div>
    </div>
  );
}
