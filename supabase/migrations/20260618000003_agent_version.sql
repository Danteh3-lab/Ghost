-- Add version column to agents table for auto-update tracking
ALTER TABLE agents ADD COLUMN IF NOT EXISTS version TEXT DEFAULT '0.0.0';
