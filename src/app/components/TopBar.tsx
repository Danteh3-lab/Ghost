import { useState } from "react";
import { Search, Bell, Shield } from "lucide-react";

interface TopBarProps {
  onSearch: (q: string) => void;
  activeView: "feed" | "detail";
  onViewChange: (v: "feed" | "detail") => void;
  selectedAgentHostname: string | null;
  notificationCount: number;
}

export function TopBar({ onSearch, activeView, onViewChange, selectedAgentHostname, notificationCount }: TopBarProps) {
  const [query, setQuery] = useState("");

  return (
    <div
      className="flex items-center gap-4 px-4 py-2.5 border-b flex-shrink-0"
      style={{ backgroundColor: "#0d1117", borderColor: "#30363d", height: 52 }}
    >
      {/* Brand */}
      <div className="flex items-center gap-2 flex-shrink-0">
        <Shield size={16} style={{ color: "#58a6ff" }} />
        <span
          style={{
            color: "#58a6ff",
            fontFamily: "JetBrains Mono, monospace",
            fontSize: 12,
            fontWeight: 600,
            letterSpacing: "0.15em",
          }}
        >
          GHOSTNET
        </span>
        <span
          style={{
            color: "#30363d",
            fontFamily: "JetBrains Mono, monospace",
            fontSize: 11,
          }}
        >
          //
        </span>
        <span
          style={{
            color: "#8b949e",
            fontFamily: "JetBrains Mono, monospace",
            fontSize: 10,
            letterSpacing: "0.05em",
          }}
        >
          C2 COMMAND CENTER
        </span>
      </div>

      {/* Nav tabs */}
      <div className="flex items-center gap-1 ml-2">
        <button
          onClick={() => onViewChange("feed")}
          className="px-3 py-1 rounded transition-all"
          style={{
            backgroundColor: activeView === "feed" ? "#21262d" : "transparent",
            color: activeView === "feed" ? "#e6edf3" : "#8b949e",
            fontFamily: "JetBrains Mono, monospace",
            fontSize: 11,
            borderBottom: activeView === "feed" ? "1px solid #58a6ff" : "1px solid transparent",
          }}
        >
          LIVE FEED
        </button>
        {selectedAgentHostname && (
          <button
            onClick={() => onViewChange("detail")}
            className="px-3 py-1 rounded transition-all"
            style={{
              backgroundColor: activeView === "detail" ? "#21262d" : "transparent",
              color: activeView === "detail" ? "#e6edf3" : "#8b949e",
              fontFamily: "JetBrains Mono, monospace",
              fontSize: 11,
              borderBottom: activeView === "detail" ? "1px solid #58a6ff" : "1px solid transparent",
            }}
          >
            AGENT: {selectedAgentHostname}
          </button>
        )}
      </div>

      {/* Search */}
      <div
        className="flex items-center gap-2 px-3 py-1.5 rounded flex-1 max-w-xs"
        style={{ backgroundColor: "#21262d", border: "1px solid #30363d" }}
      >
        <Search size={12} style={{ color: "#8b949e" }} />
        <input
          type="text"
          placeholder="Search keystrokes, hosts, windows..."
          value={query}
          onChange={(e) => {
            setQuery(e.target.value);
            onSearch(e.target.value);
          }}
          className="bg-transparent outline-none flex-1 placeholder-[#8b949e]"
          style={{
            color: "#e6edf3",
            fontFamily: "JetBrains Mono, monospace",
            fontSize: 11,
          }}
        />
      </div>

      <div className="ml-auto flex items-center gap-3">
        {/* Notification bell */}
        <div className="relative">
          <button
            className="p-1.5 rounded transition-colors hover:bg-[#21262d]"
            style={{ color: "#8b949e" }}
          >
            <Bell size={15} />
          </button>
          {notificationCount > 0 && (
            <div
              className="absolute -top-0.5 -right-0.5 w-4 h-4 rounded-full flex items-center justify-center"
              style={{ backgroundColor: "#f85149" }}
            >
              <span style={{ color: "#fff", fontSize: 8, fontFamily: "JetBrains Mono, monospace" }}>
                {notificationCount}
              </span>
            </div>
          )}
        </div>

        {/* Avatar */}
        <div
          className="w-7 h-7 rounded-full flex items-center justify-center flex-shrink-0"
          style={{ backgroundColor: "#21262d", border: "1px solid #30363d" }}
        >
          <span style={{ color: "#58a6ff", fontFamily: "JetBrains Mono, monospace", fontSize: 11, fontWeight: 600 }}>
            RT
          </span>
        </div>
      </div>
    </div>
  );
}
