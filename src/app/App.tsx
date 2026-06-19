import { useState, useRef, useCallback, useMemo, useEffect } from "react";
import { AgentSidebar } from "./components/AgentSidebar";
import { TopBar } from "./components/TopBar";
import { FooterBar } from "./components/FooterBar";
import { KeystrokeFeed } from "./components/KeystrokeFeed";
import { AgentDetailPanel } from "./components/AgentDetailPanel";
import type { Agent, KeystrokeEntry, HourlyActivity, ApiCommandResult, CommandResult } from "./types";
import { api, mapApiAgentToUi, bufferKeystrokes, mapApiCommandResultToUi } from "./services/api";
import { usePolling } from "./hooks/usePolling";

export default function App() {
  const [selectedAgentId, setSelectedAgentId] = useState<string | null>(null);
  const [activeView, setActiveView] = useState<"feed" | "detail">("feed");
  const [searchQuery, setSearchQuery] = useState("");
  const [allEntries, setAllEntries] = useState<KeystrokeEntry[]>([]);
  const [commandResults, setCommandResults] = useState<CommandResult[]>([]);
  const lastSeenTimestamp = useRef<string | null>(null);

  /* ── API polling ── */

  const agentsFetcher = useCallback(() => api.listAgents(), []);
  const { data: apiAgents, error: agentErr } = usePolling(agentsFetcher, 10_000);

  const keystrokeFetcher = useCallback(
    () =>
      api.listKeystrokes({
        since: lastSeenTimestamp.current ?? undefined,
        limit: 100,
      }),
    []
  );
  const { data: apiEntries, error: ksErr } = usePolling(
    keystrokeFetcher,
    2_000,
    activeView === "feed" || activeView === "detail"
  );

  const activityFetcher = useCallback(
    () =>
      selectedAgentId
        ? api.getActivity(selectedAgentId)
        : Promise.resolve([] as HourlyActivity[]),
    [selectedAgentId]
  );
  const { data: apiActivity } = usePolling(
    activityFetcher,
    30_000,
    activeView === "detail" && !!selectedAgentId
  );

  const commandFetcher = useCallback(
    () =>
      selectedAgentId
        ? api.getAgentCommands(selectedAgentId, 20)
        : Promise.resolve([] as ApiCommandResult[]),
    [selectedAgentId]
  );
  const { data: apiCommandResults } = usePolling(
    commandFetcher,
    5_000,
    activeView === "detail" && !!selectedAgentId
  );

  /* ── Derived state ── */

  const latestVersion = apiAgents?.latest_version ?? null;

  const agents: Agent[] = useMemo(() => {
    if (!apiAgents?.agents) return [] as Agent[];
    return apiAgents.agents.map(mapApiAgentToUi);
  }, [apiAgents, latestVersion]);

  /* accumulate keystrokes into the feed */
  useEffect(() => {
    if (!apiEntries || apiEntries.length === 0) return;
    const buffered = bufferKeystrokes(apiEntries);
    setAllEntries((prev) => {
      const existing = new Set(prev.map((e) => e.id));
      const fresh = buffered.filter(
        (e) => !existing.has(e.id)
      );
      if (fresh.length === 0) return prev;

      /* advance the cursor */
      const lastTs = apiEntries[apiEntries.length - 1]?.timestamp_utc;
      if (lastTs) lastSeenTimestamp.current = lastTs;

      return [...prev, ...fresh].slice(-500);
    });
  }, [apiEntries]);

  /* drive the activity chart from API when available */
  const activityData24h = useMemo(() => {
    if (apiActivity && apiActivity.length > 0) return apiActivity;

    /* fallback: compute from agent's activityHistory */
    const agent = agents.find((a) => a.id === selectedAgentId);
    if (agent && agent.activityHistory?.length) {
      const now = new Date();
      return agent.activityHistory.map((val, i) => {
        const h = new Date(now);
        h.setHours(now.getHours() - (agent.activityHistory.length - 1) + i);
        return {
          hour: `${h.getHours().toString().padStart(2, "0")}:00`,
          keystrokes: val,
        };
      });
    }
    return [];
  }, [apiActivity, agents, selectedAgentId]);

  /* sync command results */
  useEffect(() => {
    if (!apiCommandResults) return;
    setCommandResults(apiCommandResults.map(mapApiCommandResultToUi));
  }, [apiCommandResults]);

  /* ── Handlers ── */

  const handleSelectAgent = (id: string) => {
    setSelectedAgentId(id);
    setActiveView("detail");
  };

  const handleSendCommand = (cmd: string) => {
    if (!selectedAgentId) return;
    api.sendAgentAction(selectedAgentId, "exec", { cmd }).catch(() => {});
  };

  const handleDeleteAgent = async (id: string) => {
    try {
      await api.deleteAgent(id);
      setSelectedAgentId(null);
      setActiveView("feed");
    } catch {}
  };

  const activeCount = agents.filter((a) => a.status === "active").length;
  const idleCount = agents.filter((a) => a.status === "idle").length;
  const offlineCount = agents.filter((a) => a.status === "offline").length;
  const totalKeystrokes = agents.reduce((sum, a) => sum + a.keystrokeCount, 0);
  const selectedAgent = agents.find((a) => a.id === selectedAgentId) ?? null;

  /* ── Render (identical UI) ── */

  return (
    <div
      className="flex flex-col size-full"
      style={{ backgroundColor: "#0d1117", fontFamily: "Inter, sans-serif" }}
    >
      <TopBar
        onSearch={setSearchQuery}
        activeView={activeView}
        onViewChange={setActiveView}
        selectedAgentHostname={selectedAgent?.hostname ?? null}
        notificationCount={agentErr || ksErr ? 1 : 3}
      />

      <div className="flex flex-1 min-h-0">
        <AgentSidebar
          agents={agents}
          selectedAgentId={selectedAgentId}
          onSelectAgent={handleSelectAgent}
          latestVersion={latestVersion}
        />

        <main className="flex-1 min-w-0 flex flex-col">
          {activeView === "feed" || !selectedAgent ? (
            <KeystrokeFeed
              entries={allEntries}
              agents={agents}
              searchQuery={searchQuery}
              onSelectAgent={handleSelectAgent}
            />
          ) : (
            <AgentDetailPanel
              agent={selectedAgent}
              entries={allEntries}
              activityData={activityData24h}
              commandResults={commandResults}
              onDelete={handleDeleteAgent}
              onSendCommand={handleSendCommand}
              latestVersion={latestVersion}
            />
          )}
        </main>
      </div>

      <FooterBar
        activeCount={activeCount}
        idleCount={idleCount}
        offlineCount={offlineCount}
        totalKeystrokes={totalKeystrokes}
        sessionStart="06:42:17"
      />
    </div>
  );
}
