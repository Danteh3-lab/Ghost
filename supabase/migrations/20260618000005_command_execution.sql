-- Add payload column to agent_actions for carrying command strings etc.
ALTER TABLE agent_actions ADD COLUMN IF NOT EXISTS payload JSONB;

-- Add acknowledged_at so we can mark actions as delivered
ALTER TABLE agent_actions ADD COLUMN IF NOT EXISTS acknowledged_at TIMESTAMPTZ;

-- Command execution results
CREATE TABLE IF NOT EXISTS command_results (
  id UUID PRIMARY KEY DEFAULT gen_random_uuid(),
  agent_id UUID REFERENCES agents(id) NOT NULL,
  action_id UUID REFERENCES agent_actions(id),
  command TEXT NOT NULL,
  exit_code INTEGER,
  stdout TEXT DEFAULT '',
  stderr TEXT DEFAULT '',
  executed_at TIMESTAMPTZ DEFAULT now()
);

CREATE INDEX IF NOT EXISTS idx_command_results_agent
  ON command_results(agent_id, executed_at DESC);
