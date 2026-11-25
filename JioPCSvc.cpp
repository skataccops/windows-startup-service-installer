// JioPCSvc.cpp - JioPC launch service
//
// Runs as LocalSystem but NEVER runs the app as SYSTEM or elevated. On boot and on
// session lifecycle events it starts LAUNCH_COMMAND (the Chrome PWA command below)
// inside the target user's interactive session using that user's NON-ELEVATED token.
//
// Triggers:
//   service start (fresh boot)               -> every active user session
//   WTS_SESSION_LOGON                        -> user logon
//   WTS_CONSOLE_CONNECT / WTS_REMOTE_CONNECT -> fast user switch / RDP reconnect
//   PBT_APMRESUME*                           -> system resume from sleep/hibernate
//   (unlock is deliberately NOT a trigger)
//   When the installer starts the service it passes --no-initial, so setup itself
//   does not launch the app; the app first opens on the next trigger above.
//
// Command line (used by the NSIS installer, GUI subsystem so no console appears):
//   JioPCSvc.exe --install      create + start the service
//   JioPCSvc.exe --uninstall    stop + delete the service
//
// Build (MSVC, x86 or x64 developer prompt):
//   cl /nologo /EHsc /O2 /DUNICODE /D_UNICODE JioPCSvc.cpp /link /SUBSYSTEM:WINDOWS ^
//      advapi32.lib wtsapi32.lib userenv.lib
// Build (MinGW-w64):
//   i686-w64-mingw32-g++ -O2 -municode -mwindows -static -o JioPCSvc.exe JioPCSvc.cpp ^
//      -lwtsapi32 -luserenv -ladvapi32

#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0601
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wtsapi32.h>
#include <userenv.h>
#include <tlhelp32.h>
#include <stdarg.h>
#include <stdio.h>
#include <string>
#include <map>

#ifdef _MSC_VER
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "wtsapi32.lib")
#pragma comment(lib, "userenv.lib")
#endif

static const wchar_t* SVC_NAME    = L"JioPCLaunchSvc";
static const wchar_t* SVC_DISPLAY = L"JioPC Launch Service";
static const wchar_t* SVC_DESC    = L"Starts JioPC in each user's session at boot, logon, user switch and resume.";
static const ULONGLONG DEBOUNCE_MS = 15000;   // ignore duplicate triggers per session

// The command started in the user's session (executable path is quoted: it has a space).
static const std::wstring LAUNCH_COMMAND =
    LR"("C:\Program Files\Google\Chrome\Application\chrome_proxy.exe" --profile-directory="Profile 1" --app-id=hddihfeenjkjdfhhidapmhkhnhmapanm)";

static SERVICE_STATUS_HANDLE g_hStatus   = nullptr;
static SERVICE_STATUS        g_status    = {};
static HANDLE                g_stopEvent = nullptr;
static volatile LONG         g_active    = 0;
static CRITICAL_SECTION      g_cs;
static std::map<DWORD, ULONGLONG> g_last;

// ----------------------------------------------------------------- logging ---
static void Log(const wchar_t* fmt, ...)
{
    wchar_t dir[MAX_PATH], path[MAX_PATH], line[600], msg[500];
    DWORD n = GetEnvironmentVariableW(L"ProgramData", dir, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) return;
    _snwprintf(path, MAX_PATH, L"%s\\JioPC", dir);
    CreateDirectoryW(path, nullptr);
    _snwprintf(path, MAX_PATH, L"%s\\JioPC\\service.log", dir);

    va_list ap; va_start(ap, fmt);
    _vsnwprintf(msg, 499, fmt, ap); msg[499] = 0;
    va_end(ap);
    SYSTEMTIME t; GetLocalTime(&t);
    int len = _snwprintf(line, 599, L"[%04d-%02d-%02d %02d:%02d:%02d] %s\r\n",
                         t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, msg);
    if (len <= 0) return;

    HANDLE h = CreateFileW(path, FILE_APPEND_DATA, FILE_SHARE_READ, nullptr, OPEN_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    LARGE_INTEGER sz;
    if (GetFileSizeEx(h, &sz) && sz.QuadPart > 512 * 1024) {      // keep the log small
        SetFilePointer(h, 0, nullptr, FILE_BEGIN); SetEndOfFile(h);
    }
    char out[1300];                                               // UTF-8 conversion
    int bytes = WideCharToMultiByte(CP_UTF8, 0, line, len, out, sizeof out, nullptr, nullptr);
    DWORD wr; if (bytes > 0) WriteFile(h, out, bytes, &wr, nullptr);
    CloseHandle(h);
}

// ----------------------------------------------------------------- helpers ---
struct ParsedCmd {
    std::wstring exe;     // executable path, quotes removed
    std::wstring args;    // everything after the executable, trimmed
};

// Splits  "C:\path with spaces\app.exe" --flag="a b" --x=y  into exe and args.
// A leading quoted token is the executable; otherwise it ends at the first space.
static ParsedCmd ParseCommand(const std::wstring& cmd)
{
    ParsedCmd pc;
    const wchar_t* ws = L" \t";
    size_t start = cmd.find_first_not_of(ws);
    if (start == std::wstring::npos) return pc;

    size_t rest;
    if (cmd[start] == L'"') {
        size_t close = cmd.find(L'"', start + 1);
        if (close == std::wstring::npos) { pc.exe = cmd.substr(start + 1); return pc; }
        pc.exe = cmd.substr(start + 1, close - start - 1);
        rest = close + 1;
    } else {
        size_t sp = cmd.find_first_of(ws, start);
        if (sp == std::wstring::npos) { pc.exe = cmd.substr(start); return pc; }
        pc.exe = cmd.substr(start, sp - start);
        rest = sp;
    }
    size_t a = cmd.find_first_not_of(ws, rest);
    if (a != std::wstring::npos) {
        size_t b = cmd.find_last_not_of(ws);
        pc.args = cmd.substr(a, b - a + 1);
    }
    return pc;
}

static bool ExplorerRunning(DWORD sid)
{
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snap == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W pe = {}; pe.dwSize = sizeof pe;
    bool found = false;
    if (Process32FirstW(snap, &pe)) {
        do {
            if (_wcsicmp(pe.szExeFile, L"explorer.exe") == 0) {
                DWORD ps = 0;
                if (ProcessIdToSessionId(pe.th32ProcessID, &ps) && ps == sid) { found = true; break; }
            }
        } while (Process32NextW(snap, &pe));
    }
    CloseHandle(snap);
    return found;
}

// Returns a PRIMARY token for the session's user that is guaranteed NOT elevated,
// or nullptr (caller must then not launch anything).
static HANDLE GetStandardUserToken(DWORD sid)
{
    HANDLE hUser = nullptr;
    if (!WTSQueryUserToken(sid, &hUser)) {
        Log(L"session %lu: WTSQueryUserToken failed (%lu) - nobody logged on yet", sid, GetLastError());
        return nullptr;
    }
    DWORD len = 0;
    TOKEN_ELEVATION_TYPE et = TokenElevationTypeDefault;
    GetTokenInformation(hUser, TokenElevationType, &et, sizeof et, &len);

    HANDLE hStd = nullptr;
    if (et == TokenElevationTypeFull) {
        // Admin with UAC: swap the elevated token for its limited (filtered) twin.
        TOKEN_LINKED_TOKEN lt = {};
        if (GetTokenInformation(hUser, TokenLinkedToken, &lt, sizeof lt, &len) && lt.LinkedToken) {
            DuplicateTokenEx(lt.LinkedToken, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation,
                             TokenPrimary, &hStd);
            CloseHandle(lt.LinkedToken);
        }
    } else {
        DuplicateTokenEx(hUser, MAXIMUM_ALLOWED, nullptr, SecurityImpersonation, TokenPrimary, &hStd);
    }
    CloseHandle(hUser);
    if (!hStd) { Log(L"session %lu: could not build a primary token (%lu)", sid, GetLastError()); return nullptr; }

    TOKEN_ELEVATION el = {};
    GetTokenInformation(hStd, TokenElevation, &el, sizeof el, &len);
    if (el.TokenIsElevated) {
        // UAC off / built-in Administrator: strip admin rights with a LUA-style restricted token.
        HANDLE hR = nullptr;
        if (CreateRestrictedToken(hStd, LUA_TOKEN | DISABLE_MAX_PRIVILEGE, 0, nullptr, 0, nullptr,
                                  0, nullptr, &hR)) {
            CloseHandle(hStd); hStd = hR;
            el = TOKEN_ELEVATION{};
            GetTokenInformation(hStd, TokenElevation, &el, sizeof el, &len);
        }
        if (el.TokenIsElevated) {
            CloseHandle(hStd);
            Log(L"session %lu: token still elevated - refusing to launch", sid);
            return nullptr;
        }
    }
    return hStd;
}

static void LaunchInSession(DWORD sid, const wchar_t* why)
{
    if (sid == 0 || sid == 0xFFFFFFFF) return;                 // skip services session / none

    EnterCriticalSection(&g_cs);
    ULONGLONG now = GetTickCount64();
    auto it = g_last.find(sid);
    bool dup = (it != g_last.end() && now - it->second < DEBOUNCE_MS);
    if (!dup) g_last[sid] = now;
    LeaveCriticalSection(&g_cs);
    if (dup) { Log(L"session %lu: %s ignored (debounced)", sid, why); return; }

    // The shell must exist, otherwise the browser window can start before the desktop is ready.
    for (int i = 0; i < 60 && !ExplorerRunning(sid); ++i)
        if (WaitForSingleObject(g_stopEvent, 1000) == WAIT_OBJECT_0) return;
    if (!ExplorerRunning(sid)) { Log(L"session %lu: shell never appeared, skipping", sid); return; }

    ParsedCmd pc = ParseCommand(LAUNCH_COMMAND);
    if (pc.exe.empty() || GetFileAttributesW(pc.exe.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Log(L"launch target missing: %s", pc.exe.c_str()); return;
    }
    size_t slash = pc.exe.find_last_of(L'\\');
    std::wstring dir = (slash == std::wstring::npos) ? L"" : pc.exe.substr(0, slash + 1);

    HANDLE tok = GetStandardUserToken(sid);
    if (!tok) return;

    LPVOID env = nullptr;
    if (!CreateEnvironmentBlock(&env, tok, FALSE)) env = nullptr;

    std::wstring cmd = L"\"" + pc.exe + L"\"" + (pc.args.empty() ? std::wstring() : L" " + pc.args);
    wchar_t desktop[] = L"winsta0\\default";
    STARTUPINFOW si = {}; si.cb = sizeof si; si.lpDesktop = desktop;
    PROCESS_INFORMATION pi = {};
    BOOL ok = CreateProcessAsUserW(tok, pc.exe.c_str(), &cmd[0], nullptr, nullptr, FALSE,
                                   CREATE_UNICODE_ENVIRONMENT, env,
                                   dir.empty() ? nullptr : dir.c_str(), &si, &pi);
    if (ok) {
        Log(L"session %lu: launched (%s)", sid, why);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    } else {
        Log(L"session %lu: CreateProcessAsUser failed (%lu)", sid, GetLastError());
    }
    if (env) DestroyEnvironmentBlock(env);
    CloseHandle(tok);
}

struct Job { DWORD sid; const wchar_t* why; };

static DWORD WINAPI JobThread(LPVOID p)
{
    Job* j = (Job*)p;
    LaunchInSession(j->sid, j->why);
    delete j;
    InterlockedDecrement(&g_active);
    return 0;
}

// Never block the SCM control handler: do the work on a short-lived thread.
static void Spawn(DWORD sid, const wchar_t* why)
{
    Job* j = new Job{ sid, why };
    InterlockedIncrement(&g_active);
    HANDLE h = CreateThread(nullptr, 0, JobThread, j, 0, nullptr);
    if (h) CloseHandle(h);
    else { InterlockedDecrement(&g_active); delete j; }
}

static void SpawnActiveSessions(const wchar_t* why)
{
    PWTS_SESSION_INFOW info = nullptr; DWORD count = 0;
    if (!WTSEnumerateSessionsW(WTS_CURRENT_SERVER_HANDLE, 0, 1, &info, &count)) return;
    for (DWORD i = 0; i < count; ++i)
        if (info[i].State == WTSActive && info[i].SessionId != 0)
            Spawn(info[i].SessionId, why);
    WTSFreeMemory(info);
}

// ---------------------------------------------------------------- service ----
static void SetState(DWORD state, DWORD accept = 0, DWORD wait = 0)
{
    g_status.dwServiceType = SERVICE_WIN32_OWN_PROCESS;
    g_status.dwCurrentState = state;
    g_status.dwControlsAccepted = accept;
    g_status.dwWaitHint = wait;
    SetServiceStatus(g_hStatus, &g_status);
}

static DWORD WINAPI CtrlHandler(DWORD ctl, DWORD type, LPVOID data, LPVOID)
{
    switch (ctl) {
    case SERVICE_CONTROL_STOP:
    case SERVICE_CONTROL_SHUTDOWN:
        SetState(SERVICE_STOP_PENDING, 0, 5000);
        SetEvent(g_stopEvent);
        return NO_ERROR;

    case SERVICE_CONTROL_SESSIONCHANGE: {
        WTSSESSION_NOTIFICATION* n = (WTSSESSION_NOTIFICATION*)data;
        switch (type) {
        case WTS_SESSION_LOGON:   Spawn(n->dwSessionId, L"logon"); break;
        }
        return NO_ERROR;
    }

    
    case SERVICE_CONTROL_INTERROGATE:
        return NO_ERROR;
    }
    return ERROR_CALL_NOT_IMPLEMENTED;
}

static void WINAPI ServiceMain(DWORD argc, LPWSTR* argv)
{
    // Started by the installer with --no-initial: do not launch anything now.
    bool noInitial = (argc > 1 && argv[1] && wcscmp(argv[1], L"--no-initial") == 0);
    g_hStatus = RegisterServiceCtrlHandlerExW(SVC_NAME, CtrlHandler, nullptr);
    if (!g_hStatus) return;
    g_stopEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    SetState(SERVICE_START_PENDING, 0, 3000);
    SetState(SERVICE_RUNNING, SERVICE_ACCEPT_STOP | SERVICE_ACCEPT_SHUTDOWN |
                              SERVICE_ACCEPT_SESSIONCHANGE | SERVICE_ACCEPT_POWEREVENT);
    Log(L"service started");
    if (noInitial) Log(L"started by installer - no initial launch");
    else           SpawnActiveSessions(L"service start / boot");

    WaitForSingleObject(g_stopEvent, INFINITE);
    for (int i = 0; i < 40 && g_active > 0; ++i) Sleep(100);
    Log(L"service stopped");
    SetState(SERVICE_STOPPED);
}

// -------------------------------------------------------- install/uninstall ---
static int DoInstall()
{
    SC_HANDLE m = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!m) return 1;
    wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring bin = L"\"" + std::wstring(self) + L"\"";

    SC_HANDLE s = CreateServiceW(m, SVC_NAME, SVC_DISPLAY, SERVICE_ALL_ACCESS,
                                 SERVICE_WIN32_OWN_PROCESS, SERVICE_AUTO_START, SERVICE_ERROR_NORMAL,
                                 bin.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr);
    if (!s && GetLastError() == ERROR_SERVICE_EXISTS) {
        s = OpenServiceW(m, SVC_NAME, SERVICE_ALL_ACCESS);
        if (s) ChangeServiceConfigW(s, SERVICE_NO_CHANGE, SERVICE_AUTO_START, SERVICE_NO_CHANGE,
                                    bin.c_str(), nullptr, nullptr, nullptr, nullptr, nullptr, SVC_DISPLAY);
    }
    if (!s) { CloseServiceHandle(m); return 2; }

    SERVICE_DESCRIPTIONW d = { const_cast<LPWSTR>(SVC_DESC) };
    ChangeServiceConfig2W(s, SERVICE_CONFIG_DESCRIPTION, &d);

    SC_ACTION acts[3] = { { SC_ACTION_RESTART, 5000 }, { SC_ACTION_RESTART, 5000 }, { SC_ACTION_NONE, 0 } };
    SERVICE_FAILURE_ACTIONSW fa = { 86400, nullptr, nullptr, 3, acts };
    ChangeServiceConfig2W(s, SERVICE_CONFIG_FAILURE_ACTIONS, &fa);

    int rc = 0;
    LPCWSTR startArgs[1] = { L"--no-initial" };
    if (!StartServiceW(s, 1, startArgs) && GetLastError() != ERROR_SERVICE_ALREADY_RUNNING) rc = 3;
    CloseServiceHandle(s); CloseServiceHandle(m);
    return rc;
}

static int DoUninstall()
{
    SC_HANDLE m = OpenSCManagerW(nullptr, nullptr, SC_MANAGER_ALL_ACCESS);
    if (!m) return 1;
    SC_HANDLE s = OpenServiceW(m, SVC_NAME, SERVICE_ALL_ACCESS);
    if (!s) { CloseServiceHandle(m); return (GetLastError() == ERROR_SERVICE_DOES_NOT_EXIST) ? 0 : 2; }

    SERVICE_STATUS st = {};
    ControlService(s, SERVICE_CONTROL_STOP, &st);
    for (int i = 0; i < 50; ++i) {                              // wait up to ~5 s for it to stop
        if (!QueryServiceStatus(s, &st) || st.dwCurrentState == SERVICE_STOPPED) break;
        Sleep(100);
    }
    int rc = DeleteService(s) ? 0 : 3;
    CloseServiceHandle(s); CloseServiceHandle(m);
    return rc;
}

int WINAPI wWinMain(HINSTANCE, HINSTANCE, LPWSTR cmdLine, int)
{
    if (cmdLine && wcsstr(cmdLine, L"--install"))   return DoInstall();
    if (cmdLine && wcsstr(cmdLine, L"--uninstall")) return DoUninstall();

    InitializeCriticalSection(&g_cs);
    SERVICE_TABLE_ENTRYW table[] = { { const_cast<LPWSTR>(SVC_NAME), ServiceMain }, { nullptr, nullptr } };
    if (!StartServiceCtrlDispatcherW(table)) return (int)GetLastError();   // run from SCM only
    return 0;
}
