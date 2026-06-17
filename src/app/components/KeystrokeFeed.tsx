import { useEffect, useRef, useState } from "react";
import { Terminal } from "lucide-react";
import type { Agent, KeystrokeEntry } from "../types";

const PROCESS_ACCENT: Record<string, string> = {
  "notepad.exe": "#58a6ff",
  "chrome.exe": "#3fb950",
  "outlook.exe": "#d29922",
  "firefox.exe": "#f97316",
  "cmd.exe": "#f85149",
  "powershell.exe": "#a855f7",
  "explorer.exe": "#22d3ee",
};

function getProcessColor(process: string): string {
  return PROCESS_ACCENT[process.toLowerCase()] ?? "#8b949e";
}

interface GroupedEntries {
  process: string;
  windowTitle: string;
  agentHostname: string;
  entries: KeystrokeEntry[];
}

function groupEntries(entries: KeystrokeEntry[]): GroupedEntries[] {
  const groups: GroupedEntries[] = [];
  let current: GroupedEntries | null = null;

  for (const entry of entries) {
    if (!current || current.windowTitle !== entry.windowTitle || current.agentHostname !== entry.agentHostname) {
      current = {
        process: entry.process,
        windowTitle: entry.windowTitle,
        agentHostname: entry.agentHostname,
        entries: [entry],
      };
      groups.push(current);
    } else {
      current.entries.push(entry);
    }
  }
  return groups;
}

interface KeystrokeFeedProps {
  entries: KeystrokeEntry[];
  agents: Agent[];
  searchQuery: string;
  onSelectAgent: (id: string) => void;
}

export function KeystrokeFeed({ entries, agents, searchQuery, onSelectAgent }: KeystrokeFeedProps) {
  const bottomRef = useRef<HTMLDivElement>(null);
  const [autoScroll, setAutoScroll] = useState(true);

  const filtered = searchQuery
    ? entries.filter(
        (e) =>
          e.text.toLowerCase().includes(searchQuery.toLowerCase()) ||
          e.windowTitle.toLowerCase().includes(searchQuery.toLowerCase()) ||
          e.agentHostname.toLowerCase().includes(searchQuery.toLowerCase())
      )
    : entries;

  const groups = groupEntries(filtered);

  useEffect(() => {
    if (autoScroll) {
      bottomRef.current?.scrollIntoView({ behavior: "smooth" });
    }
  }, [entries, autoScroll]);

  return (
    <div className="flex flex-col h-full">
      {/* Panel header */}
      <div
        className="flex items-center justify-between px-4 py-2.5 border-b flex-shrink-0"
        style={{ borderColor: "#30363d", backgroundColor: "#0d1117" }}
      >
        <div className="flex items-center gap-2">
          <Terminal size={13} style={{ color: "#58a6ff" }} />
          <span
            style={{
              color: "#8b949e",
              fontFamily: "JetBrains Mono, monospace",
              fontSize: 10,
              letterSpacing: "0.1em",
            }}
          >
            LIVE KEYSTROKE FEED
          </span>
          {searchQuery && (
            <span
              className="px-1.5 py-0.5 rounded"
              style={{
                backgroundColor: "#21262d",
                color: "#58a6ff",
                fontFamily: "JetBrains Mono, monospace",
                fontSize: 9,
              }}
            >
              FILTERED: {filtered.length}
            </span>
          )}
        </div>
        <label className="flex items-center gap-2 cursor-pointer">
          <span style={{ color: "#8b949e", fontFamily: "JetBrains Mono, monospace", fontSize: 10 }}>
            AUTO-SCROLL
          </span>
          <div
            className="w-8 h-4 rounded-full relative transition-colors cursor-pointer"
            style={{ backgroundColor: autoScroll ? "#58a6ff" : "#30363d" }}
            onClick={() => setAutoScroll((v) => !v)}
          >
            <div
              className="absolute top-0.5 w-3 h-3 rounded-full transition-all"
              style={{
                backgroundColor: "#e6edf3",
                left: autoScroll ? "17px" : "2px",
              }}
            />
          </div>
        </label>
      </div>

      {/* Feed */}
      <div
        className="flex-1 overflow-y-auto p-3 space-y-2"
        style={{ scrollbarWidth: "thin", scrollbarColor: "#30363d #0d1117" }}
      >
        {groups.map((group, gi) => {
          const accentColor = getProcessColor(group.process);
          const agent = agents.find((a) => a.hostname === group.agentHostname);
          return (
            <div
              key={gi}
              className="rounded"
              style={{
                backgroundColor: "#161b22",
                border: "1px solid #30363d",
                borderLeft: `3px solid ${accentColor}`,
              }}
            >
              {/* Card header */}
              <div
                className="flex items-center gap-3 px-3 py-2 border-b"
                style={{ borderColor: "#30363d" }}
              >
                <span
                  className="px-1.5 py-0.5 rounded"
                  style={{
                    backgroundColor: `${accentColor}1a`,
                    color: accentColor,
                    fontFamily: "JetBrains Mono, monospace",
                    fontSize: 9,
                    fontWeight: 600,
                    letterSpacing: "0.05em",
                  }}
                >
                  {group.process.toUpperCase()}
                </span>
                <span
                  className="flex-1 truncate"
                  style={{
                    color: "#8b949e",
                    fontFamily: "JetBrains Mono, monospace",
                    fontSize: 10,
                  }}
                >
                  {group.windowTitle}
                </span>
                {agent && (
                  <button
                    onClick={() => onSelectAgent(agent.id)}
                    className="flex items-center gap-1 px-1.5 py-0.5 rounded transition-colors hover:bg-[#21262d]"
                  >
                    <div
                      className="w-1.5 h-1.5 rounded-full"
                      style={{
                        backgroundColor: agent.status === "active" ? "#3fb950" : agent.status === "idle" ? "#d29922" : "#f85149",
                      }}
                    />
                    <span style={{ color: "#8b949e", fontFamily: "JetBrains Mono, monospace", fontSize: 9 }}>
                      {group.agentHostname}
                    </span>
                  </button>
                )}
              </div>

              {/* Keystroke entries */}
              <div className="px-3 py-2 space-y-1">
                {group.entries.map((entry) => (
                  <div key={entry.id} className="flex items-start gap-3">
                    <span
                      className="flex-shrink-0 mt-px"
                      style={{
                        color: "#30363d",
                        fontFamily: "JetBrains Mono, monospace",
                        fontSize: 10,
                      }}
                    >
                      {entry.timestamp}
                    </span>
                    <span
                      style={{
                        color: "#e6edf3",
                        fontFamily: "JetBrains Mono, monospace",
                        fontSize: 11,
                        wordBreak: "break-all",
                        lineHeight: 1.6,
                      }}
                    >
                      {entry.text}
                    </span>
                  </div>
                ))}
              </div>
            </div>
          );
        })}
        <div ref={bottomRef} />
      </div>
    </div>
  );
}
