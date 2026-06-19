import { useState } from "react";
import { ChevronLeft, ChevronRight, Activity } from "lucide-react";
import type { Agent, AgentStatus } from "../types";

interface AgentSidebarProps {
  agents: Agent[];
  selectedAgentId: string | null;
  onSelectAgent: (id: string) => void;
  latestVersion: string | null;
}

function countryFlag(code: string | undefined): string {
  if (!code || code.length !== 2) return "";
  /* Convert ISO 3166-1 alpha-2 to regional indicator symbols */
  const cp = code.toUpperCase();
  return String.fromCodePoint(cp.charCodeAt(0) + 0x1F1E6 - 65, cp.charCodeAt(1) + 0x1F1E6 - 65);
}

function VersionBadge({ version, latestVersion }: { version: string; latestVersion: string | null }) {
  const isLatest = latestVersion && version === latestVersion;
  const color = isLatest ? "#3fb950" : latestVersion ? "#d29922" : "#30363d";
  return (
    <span
      className="px-1 py-0.5 rounded"
      style={{
        backgroundColor: `${color}1a`,
        color,
        fontFamily: "JetBrains Mono, monospace",
        fontSize: 8,
        fontWeight: 600,
      }}
    >
      v{version}
    </span>
  );
}

const STATUS_COLOR: Record<AgentStatus, string> = {
  active: "#3fb950",
  idle: "#d29922",
  offline: "#f85149",
};

const STATUS_LABEL: Record<AgentStatus, string> = {
  active: "ACTIVE",
  idle: "IDLE",
  offline: "OFFLINE",
};

function MiniSparkbar({ data }: { data: number[] }) {
  const max = Math.max(...data, 1);
  return (
    <div className="flex items-end gap-px h-4">
      {data.map((v, i) => (
        <div
          key={i}
          className="w-[3px] rounded-sm"
          style={{
            height: `${Math.max(2, (v / max) * 16)}px`,
            backgroundColor: v > 0 ? "#58a6ff" : "#30363d",
            opacity: v > 0 ? 0.8 : 0.4,
          }}
        />
      ))}
    </div>
  );
}

export function AgentSidebar({ agents, selectedAgentId, onSelectAgent, latestVersion }: AgentSidebarProps) {
  const [collapsed, setCollapsed] = useState(false);

  return (
    <div
      className="flex flex-col border-r transition-all duration-300 flex-shrink-0"
      style={{
        width: collapsed ? 48 : 240,
        backgroundColor: "#0d1117",
        borderColor: "#30363d",
      }}
    >
      {/* Header */}
      <div
        className="flex items-center justify-between px-3 py-3 border-b"
        style={{ borderColor: "#30363d" }}
      >
        {!collapsed && (
          <div className="flex items-center gap-2">
            <Activity size={12} style={{ color: "#58a6ff" }} />
            <span
              className="tracking-widest"
              style={{ color: "#8b949e", fontFamily: "JetBrains Mono, monospace", fontSize: 10 }}
            >
              AGENTS ({agents.length})
            </span>
          </div>
        )}
        <button
          onClick={() => setCollapsed(!collapsed)}
          className="ml-auto p-1 rounded transition-colors hover:bg-[#21262d]"
          style={{ color: "#8b949e" }}
        >
          {collapsed ? <ChevronRight size={14} /> : <ChevronLeft size={14} />}
        </button>
      </div>

      {/* Agent List */}
      <div className="flex-1 overflow-y-auto py-1" style={{ scrollbarWidth: "none" }}>
        {agents.map((agent) => (
          <button
            key={agent.id}
            onClick={() => onSelectAgent(agent.id)}
            className="w-full text-left transition-colors"
            style={{
              backgroundColor: selectedAgentId === agent.id ? "#21262d" : "transparent",
              borderLeft: selectedAgentId === agent.id ? "2px solid #58a6ff" : "2px solid transparent",
            }}
          >
            {collapsed ? (
              <div className="flex justify-center py-3">
                <div
                  className="w-2 h-2 rounded-full"
                  style={{ backgroundColor: STATUS_COLOR[agent.status] }}
                />
              </div>
            ) : (
              <div className="px-3 py-2.5 hover:bg-[#21262d]">
                {/* Top row */}
                <div className="flex items-center gap-2 mb-1">
                  <div
                    className="w-1.5 h-1.5 rounded-full flex-shrink-0"
                    style={{
                      backgroundColor: STATUS_COLOR[agent.status],
                      boxShadow: `0 0 4px ${STATUS_COLOR[agent.status]}`,
                    }}
                  />
                  <span
                    className="truncate"
                    style={{
                      color: "#e6edf3",
                      fontFamily: "JetBrains Mono, monospace",
                      fontSize: 11,
                      fontWeight: 500,
                    }}
                  >
                    {agent.hostname}
                  </span>
                  <span
                    className="ml-auto flex-shrink-0"
                    style={{
                      color: STATUS_COLOR[agent.status],
                      fontFamily: "JetBrains Mono, monospace",
                      fontSize: 8,
                      letterSpacing: "0.05em",
                    }}
                  >
                    {STATUS_LABEL[agent.status]}
                  </span>
                </div>

                {/* IP */}
                <div
                  className="mb-1.5"
                  style={{
                    color: "#8b949e",
                    fontFamily: "JetBrains Mono, monospace",
                    fontSize: 10,
                  }}
                >
                  {agent.ip}
                </div>

                {/* Bottom row */}
                <div className="flex items-center justify-between">
                  <div className="flex items-center gap-2">
                    <span
                      style={{
                        color: "#58a6ff",
                        fontFamily: "JetBrains Mono, monospace",
                        fontSize: 10,
                      }}
                    >
                      {agent.keystrokeCount.toLocaleString()} keys
                    </span>
                    <VersionBadge version={agent.version} latestVersion={latestVersion} />
                  </div>
                  <MiniSparkbar data={agent.activityHistory} />
                </div>
              </div>
            )}
          </button>
        ))}
      </div>
    </div>
  );
}
