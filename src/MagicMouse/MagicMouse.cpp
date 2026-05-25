#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef UNICODE
#define UNICODE
#endif
#ifndef _UNICODE
#define _UNICODE
#endif
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <setupapi.h>
#include <newdev.h>
#include <shellapi.h>
#include <shlobj.h>
#include <strsafe.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "newdev.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")

#define WM_TRAY        (WM_APP + 1)
#define WM_DEVICE_LOST (WM_APP + 2)

#define ID_TRAY            1
#define ID_M_QUIT          1001
#define ID_M_NATURAL       1002
#define ID_M_HORIZONTAL    1003
#define ID_M_RECONNECT     1004
#define ID_M_AUTOSTART     1005
#define ID_M_REINSTALL     1006
#define ID_M_STATUS        1007
#define ID_M_SPEED_BASE    2000

#define RES_DRV_INF  100
#define RES_DRV_SYS  101
#define RES_DRV_CAT  102

#define ARG_INSTALL_DRIVER L"--install-driver"

#define MAX_CANDIDATES 64

typedef struct {
    int  speed_x10;
    BOOL natural;
    BOOL horizontal;
    BOOL autostart;
} Settings;

typedef struct {
    wchar_t instanceId[256];
    wchar_t pdoName[260];
    wchar_t pdoPath[320];
    wchar_t service[128];
    wchar_t sampleHwid[256];
    BOOL    hasPdo;
    BOOL    isBound;
} MouseCandidate;

typedef enum {
    ST_CONNECTED = 0,
    ST_DRIVER_MISMATCH = 1,  // mouse present but not bound to MagicMouse filter/service
    ST_NOT_PAIRED = 2,
    ST_DRIVER_MISSING = 3
} AppState;

static const int kSpeedPresets[] = { 10, 20, 30, 50, 80, 100 };
static const wchar_t* kSpeedNames[] = {
    L"1.0x  (slow)", L"2.0x", L"3.0x  (default)",
    L"5.0x", L"8.0x", L"10.0x (fastest)"
};

// Hardware IDs from MagicMouse.inf.
static const wchar_t* kCanonicalHwids[] = {
    L"BTHENUM\\{00001124-0000-1000-8000-00805f9b34fb}_VID&000205ac_PID&030d",
    L"BTHENUM\\{00001124-0000-1000-8000-00805f9b34fb}_VID&000205ac_PID&0310",
    L"BTHENUM\\{00001124-0000-1000-8000-00805f9b34fb}_VID&0001004c_PID&0269",
    L"USB\\Vid_05ac&Pid_0269&MI_01",
    L"BTHENUM\\{00001124-0000-1000-8000-00805f9b34fb}_VID&0001004c_PID&0323",
    L"USB\\Vid_05ac&Pid_0323&MI_01",
    NULL
};

// Tokens used to detect Magic Mouse PIDs on this machine.
static const wchar_t* kPidTokens[] = {
    L"PID&0269", L"PID_0269",
    L"PID&0323", L"PID_0323",
    L"PID&030D", L"PID_030D",
    L"PID&0310", L"PID_0310",
    NULL
};

static Settings g_s = { 30, FALSE, TRUE, FALSE };
static HWND g_hwnd = NULL;
static NOTIFYICONDATAW g_nid = { 0 };
static HANDLE g_dev = NULL;
static HANDLE g_thread = NULL;
static volatile LONG g_running = 0;
static int g_accX = 0;
static int g_accY = 0;
static wchar_t g_lastOpenPath[MAX_PATH] = L"";

static BOOL IsServiceInstalled(const wchar_t* name);
static AppState QueryState(int* outCandidates, int* outBound);
static HANDLE TryOpenPath(const wchar_t* path);

static BOOL ContainsI(const wchar_t* hay, const wchar_t* needle)
{
    if (!hay || !needle || !*needle) return FALSE;
    size_t n = wcslen(needle);
    for (const wchar_t* p = hay; *p; p++) {
        if (_wcsnicmp(p, needle, n) == 0) return TRUE;
    }
    return FALSE;
}

static BOOL IsMagicMouseHwid(const wchar_t* hwid)
{
    if (!hwid || !*hwid) return FALSE;
    for (int i = 0; kPidTokens[i]; i++) {
        if (ContainsI(hwid, kPidTokens[i])) return TRUE;
    }
    return FALSE;
}

static BOOL MultiSzContainsI(const BYTE* data, DWORD bytes, const wchar_t* token)
{
    if (!data || !bytes || !token || !*token) return FALSE;
    const wchar_t* p = (const wchar_t*)data;
    DWORD remain = bytes / sizeof(wchar_t);
    while (remain > 0 && *p) {
        size_t len = wcslen(p);
        if (ContainsI(p, token)) return TRUE;
        if (len + 1 > remain) break;
        p += len + 1;
        remain -= (DWORD)(len + 1);
    }
    return FALSE;
}

static BOOL IsBoundByProps(HDEVINFO ds, SP_DEVINFO_DATA* d, wchar_t* outService, size_t cchService)
{
    BYTE buf[4096];
    DWORD got = 0;
    if (outService && cchService) outService[0] = 0;

    if (SetupDiGetDeviceRegistryPropertyW(ds, d, SPDRP_SERVICE, NULL,
            buf, sizeof(buf), &got) && got >= sizeof(wchar_t) * 2) {
        const wchar_t* s = (const wchar_t*)buf;
        if (outService && cchService) StringCchCopyW(outService, cchService, s);
        if (_wcsicmp(s, L"MagicMouse") == 0) return TRUE;
    }

    got = 0;
    if (SetupDiGetDeviceRegistryPropertyW(ds, d, SPDRP_LOWERFILTERS, NULL,
            buf, sizeof(buf), &got) && got >= sizeof(wchar_t) * 2) {
        if (MultiSzContainsI(buf, got, L"MagicMouse")) return TRUE;
    }
    return FALSE;
}

// Enumerate all present devices whose SPDRP_SERVICE is exactly "MagicMouse".
// This often discovers the real raw-report child node created by the driver,
// which may not carry the parent mouse PID hardware IDs.
static HANDLE OpenByMagicMouseService(int* outServiceCount)
{
    if (outServiceCount) *outServiceCount = 0;
    HDEVINFO ds = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (ds == INVALID_HANDLE_VALUE) return NULL;

    HANDLE opened = NULL;
    SP_DEVINFO_DATA d = { sizeof(d) };
    BYTE buf[4096];
    for (DWORD i = 0; SetupDiEnumDeviceInfo(ds, i, &d); i++) {
        DWORD got = 0;
        if (!SetupDiGetDeviceRegistryPropertyW(ds, &d, SPDRP_SERVICE, NULL,
                buf, sizeof(buf), &got) || got < sizeof(wchar_t) * 2) {
            continue;
        }
        const wchar_t* svc = (const wchar_t*)buf;
        if (_wcsicmp(svc, L"MagicMouse") != 0) continue;

        if (outServiceCount) (*outServiceCount)++;
        if (opened) continue;

        wchar_t pdo[260] = L"";
        if (!SetupDiGetDeviceRegistryPropertyW(ds, &d, SPDRP_PHYSICAL_DEVICE_OBJECT_NAME, NULL,
                (BYTE*)pdo, sizeof(pdo), NULL) || !pdo[0]) {
            continue;
        }
        wchar_t full[320];
        StringCchPrintfW(full, _countof(full), L"\\\\.\\GLOBALROOT%s", pdo);
        HANDLE h = TryOpenPath(full);
        if (h) {
            StringCchCopyW(g_lastOpenPath, _countof(g_lastOpenPath), full);
            opened = h;
        }
    }
    SetupDiDestroyDeviceInfoList(ds);
    return opened;
}

static int CollectCandidates(MouseCandidate* out, int cap)
{
    if (!out || cap <= 0) return 0;
    int count = 0;
    HDEVINFO ds = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (ds == INVALID_HANDLE_VALUE) return 0;

    SP_DEVINFO_DATA d = { sizeof(d) };
    BYTE hwbuf[4096];

    for (DWORD i = 0; SetupDiEnumDeviceInfo(ds, i, &d); i++) {
        DWORD got = 0;
        if (!SetupDiGetDeviceRegistryPropertyW(ds, &d, SPDRP_HARDWAREID, NULL,
                hwbuf, sizeof(hwbuf), &got) || got < sizeof(wchar_t) * 2)
            continue;

        const wchar_t* p = (const wchar_t*)hwbuf;
        DWORD remain = got / sizeof(wchar_t);
        wchar_t firstMatch[256] = L"";
        BOOL isMouse = FALSE;
        while (remain > 0 && *p) {
            size_t len = wcslen(p);
            if (IsMagicMouseHwid(p)) {
                isMouse = TRUE;
                if (!firstMatch[0]) StringCchCopyW(firstMatch, _countof(firstMatch), p);
            }
            if (len + 1 > remain) break;
            p += len + 1;
            remain -= (DWORD)(len + 1);
        }
        if (!isMouse) continue;
        if (count >= cap) break;

        MouseCandidate* c = &out[count];
        ZeroMemory(c, sizeof(*c));
        StringCchCopyW(c->sampleHwid, _countof(c->sampleHwid), firstMatch);
        c->isBound = IsBoundByProps(ds, &d, c->service, _countof(c->service));
        SetupDiGetDeviceInstanceIdW(ds, &d, c->instanceId, _countof(c->instanceId), NULL);

        if (SetupDiGetDeviceRegistryPropertyW(ds, &d, SPDRP_PHYSICAL_DEVICE_OBJECT_NAME,
                NULL, (BYTE*)c->pdoName, sizeof(c->pdoName), NULL)) {
            c->hasPdo = TRUE;
            StringCchPrintfW(c->pdoPath, _countof(c->pdoPath), L"\\\\.\\GLOBALROOT%s", c->pdoName);
        }
        count++;
    }

    SetupDiDestroyDeviceInfoList(ds);
    return count;
}

static HANDLE TryOpenPath(const wchar_t* path)
{
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        h = CreateFileW(path, GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, 0, NULL);
    }
    return (h == INVALID_HANDLE_VALUE) ? NULL : h;
}

static HANDLE OpenMagicMouse(int* outBoundCount, int* outCandidateCount)
{
    if (outBoundCount) *outBoundCount = 0;
    if (outCandidateCount) *outCandidateCount = 0;
    g_lastOpenPath[0] = 0;

    // Pass 0 (primary): direct service scan. This is the most reliable path.
    int svcCount = 0;
    HANDLE hSvc = OpenByMagicMouseService(&svcCount);
    if (outBoundCount) *outBoundCount = svcCount;
    if (hSvc) return hSvc;

    MouseCandidate arr[MAX_CANDIDATES];
    int n = CollectCandidates(arr, MAX_CANDIDATES);
    if (outCandidateCount) *outCandidateCount = n;
    if (n <= 0) return NULL;

    int boundCount = 0;
    for (int i = 0; i < n; i++) {
        if (arr[i].isBound) boundCount++;
    }
    if (outBoundCount && *outBoundCount == 0) *outBoundCount = boundCount;

    // Pass 1: prefer bound devices.
    for (int i = 0; i < n; i++) {
        if (!arr[i].isBound || !arr[i].hasPdo) continue;
        HANDLE h = TryOpenPath(arr[i].pdoPath);
        if (h) {
            StringCchCopyW(g_lastOpenPath, _countof(g_lastOpenPath), arr[i].pdoPath);
            return h;
        }
    }

    return NULL;
}

static void GetIniPath(wchar_t* out, size_t cch)
{
    PWSTR roaming = NULL;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, NULL, &roaming))) {
        StringCchPrintfW(out, cch, L"%s\\MagicMouse", roaming);
        CreateDirectoryW(out, NULL);
        StringCchCatW(out, cch, L"\\config.ini");
        CoTaskMemFree(roaming);
    } else {
        StringCchCopyW(out, cch, L".\\config.ini");
    }
}

static void LoadSettings(void)
{
    wchar_t p[MAX_PATH];
    GetIniPath(p, _countof(p));
    g_s.speed_x10  =       GetPrivateProfileIntW(L"main", L"speed_x10", 30, p);
    g_s.natural    = (BOOL)GetPrivateProfileIntW(L"main", L"natural", 0, p);
    g_s.horizontal = (BOOL)GetPrivateProfileIntW(L"main", L"horizontal", 1, p);
    g_s.autostart  = (BOOL)GetPrivateProfileIntW(L"main", L"autostart", 0, p);
    if (g_s.speed_x10 < 1) g_s.speed_x10 = 1;
    if (g_s.speed_x10 > 200) g_s.speed_x10 = 200;
}

static void SaveSettings(void)
{
    wchar_t p[MAX_PATH], v[16];
    GetIniPath(p, _countof(p));
    StringCchPrintfW(v, _countof(v), L"%d", g_s.speed_x10);
    WritePrivateProfileStringW(L"main", L"speed_x10", v, p);
    StringCchPrintfW(v, _countof(v), L"%d", g_s.natural);
    WritePrivateProfileStringW(L"main", L"natural", v, p);
    StringCchPrintfW(v, _countof(v), L"%d", g_s.horizontal);
    WritePrivateProfileStringW(L"main", L"horizontal", v, p);
    StringCchPrintfW(v, _countof(v), L"%d", g_s.autostart);
    WritePrivateProfileStringW(L"main", L"autostart", v, p);
}

static void ApplyAutostart(void)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (g_s.autostart) {
        wchar_t exe[MAX_PATH], quoted[MAX_PATH + 4];
        GetModuleFileNameW(NULL, exe, _countof(exe));
        StringCchPrintfW(quoted, _countof(quoted), L"\"%s\"", exe);
        RegSetValueExW(k, L"MagicMouse", 0, REG_SZ,
            (const BYTE*)quoted, (DWORD)((wcslen(quoted) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"MagicMouse");
    }
    RegCloseKey(k);
}

static void EmitWheel(int tickY, int tickX)
{
    INPUT in[2] = { 0 };
    int n = 0;
    if (tickY) {
        in[n].type = INPUT_MOUSE;
        in[n].mi.dwFlags = MOUSEEVENTF_WHEEL;
        in[n].mi.mouseData = (DWORD)(tickY * WHEEL_DELTA);
        n++;
    }
    if (tickX && g_s.horizontal) {
        in[n].type = INPUT_MOUSE;
        in[n].mi.dwFlags = MOUSEEVENTF_HWHEEL;
        in[n].mi.mouseData = (DWORD)(tickX * WHEEL_DELTA);
        n++;
    }
    if (n) SendInput(n, in, sizeof(INPUT));
}

static DWORD WINAPI ReadThread(LPVOID arg)
{
    (void)arg;
    BYTE buf[64];
    DWORD read = 0;

    while (g_running && g_dev) {
        if (!ReadFile(g_dev, buf, sizeof(buf), &read, NULL) || read < 3) {
            DWORD e = GetLastError();
            if (e == ERROR_INVALID_HANDLE || e == ERROR_DEVICE_NOT_CONNECTED ||
                e == ERROR_OPERATION_ABORTED || e == ERROR_NOT_READY) {
                PostMessageW(g_hwnd, WM_DEVICE_LOST, 0, 0);
                break;
            }
            Sleep(10);
            continue;
        }

        int dy = (signed char)buf[1];
        int dx = (signed char)buf[2];
        if (g_s.natural) { dy = -dy; dx = -dx; }

        g_accY += dy * g_s.speed_x10;
        g_accX += dx * g_s.speed_x10;

        const int step = WHEEL_DELTA * 10;
        int ty = g_accY / step;
        int tx = g_accX / step;
        if (ty) g_accY -= ty * step;
        if (tx) g_accX -= tx * step;

        if (ty || tx) EmitWheel(ty, tx);
    }
    return 0;
}

static void StopReader(void)
{
    g_running = 0;
    if (g_dev) {
        CancelIoEx(g_dev, NULL);
        CloseHandle(g_dev);
        g_dev = NULL;
    }
    if (g_thread) {
        WaitForSingleObject(g_thread, 1500);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    g_accX = g_accY = 0;
}

static BOOL StartReader(void)
{
    StopReader();
    g_dev = OpenMagicMouse(NULL, NULL);
    if (!g_dev) return FALSE;
    g_running = 1;
    g_thread = CreateThread(NULL, 0, ReadThread, NULL, 0, NULL);
    return (g_thread != NULL);
}

static BOOL ExtractResourceToFile(int id, const wchar_t* path)
{
    HRSRC r = FindResourceW(NULL, MAKEINTRESOURCEW(id), RT_RCDATA);
    if (!r) return FALSE;
    HGLOBAL h = LoadResource(NULL, r);
    if (!h) return FALSE;
    DWORD sz = SizeofResource(NULL, r);
    void* p = LockResource(h);
    if (!p || !sz) return FALSE;

    HANDLE f = CreateFileW(path, GENERIC_WRITE, 0, NULL, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (f == INVALID_HANDLE_VALUE) return FALSE;
    DWORD wr = 0;
    BOOL ok = WriteFile(f, p, sz, &wr, NULL) && wr == sz;
    CloseHandle(f);
    if (!ok) DeleteFileW(path);
    return ok;
}

static BOOL ExtractDriversToTemp(wchar_t* outDir, size_t cchDir, wchar_t* outInf, size_t cchInf)
{
    wchar_t tmp[MAX_PATH];
    if (!GetTempPathW(_countof(tmp), tmp)) return FALSE;
    StringCchPrintfW(outDir, cchDir, L"%sMagicMouseDrv-%lu", tmp, GetCurrentProcessId());
    if (!CreateDirectoryW(outDir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS) return FALSE;

    wchar_t inf[MAX_PATH], sys[MAX_PATH], cat[MAX_PATH];
    StringCchPrintfW(inf, _countof(inf), L"%s\\MagicMouse.inf", outDir);
    StringCchPrintfW(sys, _countof(sys), L"%s\\MagicMouse.sys", outDir);
    StringCchPrintfW(cat, _countof(cat), L"%s\\MagicMouse.cat", outDir);

    if (!ExtractResourceToFile(RES_DRV_INF, inf)) return FALSE;
    if (!ExtractResourceToFile(RES_DRV_SYS, sys)) return FALSE;
    if (!ExtractResourceToFile(RES_DRV_CAT, cat)) return FALSE;

    if (outInf && cchInf) StringCchCopyW(outInf, cchInf, inf);
    return TRUE;
}

static void RemoveTempDir(const wchar_t* dir)
{
    wchar_t pat[MAX_PATH];
    StringCchPrintfW(pat, _countof(pat), L"%s\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pat, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            wchar_t p[MAX_PATH];
            StringCchPrintfW(p, _countof(p), L"%s\\%s", dir, fd.cFileName);
            DeleteFileW(p);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

static DWORD RunWait(const wchar_t* cmdline)
{
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = { 0 };

    wchar_t mut[1024];
    StringCchCopyW(mut, _countof(mut), cmdline);
    if (!CreateProcessW(NULL, mut, NULL, NULL, FALSE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi))
        return (DWORD)-1;

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD rc = 0;
    GetExitCodeProcess(pi.hProcess, &rc);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return rc;
}

static BOOL IsServiceInstalled(const wchar_t* name)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return FALSE;
    SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_QUERY_STATUS);
    BOOL ok = (svc != NULL);
    if (svc) CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return ok;
}

static int RunDriverInstallElevated(void)
{
    wchar_t dir[MAX_PATH], infPath[MAX_PATH];
    if (!ExtractDriversToTemp(dir, _countof(dir), infPath, _countof(infPath))) {
        MessageBoxW(NULL, L"Failed to extract embedded driver files.", L"Magic Mouse", MB_OK | MB_ICONERROR);
        return 1;
    }

    wchar_t cmd[1024];
    StringCchPrintfW(cmd, _countof(cmd), L"pnputil.exe /add-driver \"%s\" /install", infPath);
    DWORD addRc = RunWait(cmd);

    // Update currently present REAL HWIDs first (most reliable), then canonical IDs.
    MouseCandidate arr[MAX_CANDIDATES];
    int n = CollectCandidates(arr, MAX_CANDIDATES);
    int tries = 0;
    int updated = 0;
    BOOL rebootNeeded = FALSE;

    for (int i = 0; i < n; i++) {
        if (!arr[i].sampleHwid[0]) continue;
        BOOL rb = FALSE;
        tries++;
        if (UpdateDriverForPlugAndPlayDevicesW(NULL, arr[i].sampleHwid, infPath,
                INSTALLFLAG_FORCE | INSTALLFLAG_NONINTERACTIVE, &rb)) {
            updated++;
            if (rb) rebootNeeded = TRUE;
        }
    }
    for (int i = 0; kCanonicalHwids[i]; i++) {
        BOOL rb = FALSE;
        tries++;
        if (UpdateDriverForPlugAndPlayDevicesW(NULL, kCanonicalHwids[i], infPath,
                INSTALLFLAG_FORCE | INSTALLFLAG_NONINTERACTIVE, &rb)) {
            updated++;
            if (rb) rebootNeeded = TRUE;
        }
    }

    Sleep(1200);
    int bound = 0, cands = 0;
    HANDLE h = OpenMagicMouse(&bound, &cands);
    if (h) CloseHandle(h);

    // If still not bound, do a targeted re-enumeration ONLY for detected
    // Magic Mouse instance IDs (safe for keyboard/headset). This is the
    // last resort that often fixes "driver in store but still Microsoft".
    if (bound == 0 && n > 0) {
        for (int i = 0; i < n; i++) {
            if (!arr[i].instanceId[0]) continue;
            StringCchPrintfW(cmd, _countof(cmd),
                L"pnputil.exe /remove-device \"%s\"", arr[i].instanceId);
            RunWait(cmd);
        }
        RunWait(L"pnputil.exe /scan-devices");
        Sleep(1500);
        h = OpenMagicMouse(&bound, &cands);
        if (h) CloseHandle(h);
    }
    BOOL svcOk = IsServiceInstalled(L"MagicMouse");
    RemoveTempDir(dir);

    if (bound > 0) {
        MessageBoxW(NULL,
            rebootNeeded
                ? L"Driver installed and bound.\r\nWindows requested a reboot to fully finalize."
                : L"Driver installed and bound.\r\nThe app will reconnect automatically.",
            L"Magic Mouse - OK", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    if (!svcOk) {
        wchar_t msg[512];
        StringCchPrintfW(msg, _countof(msg),
            L"Driver registration failed.\r\npnputil exit code: %lu\r\n\r\n"
            L"Make sure UAC was accepted and no conflicting package is locking the driver.",
            addRc);
        MessageBoxW(NULL, msg, L"Magic Mouse - Install error", MB_OK | MB_ICONERROR);
        return 2;
    }

    wchar_t sample[256] = L"(none)";
    if (n > 0 && arr[0].sampleHwid[0]) StringCchCopyW(sample, _countof(sample), arr[0].sampleHwid);

    wchar_t msg[1200];
    StringCchPrintfW(msg, _countof(msg),
        L"Driver is installed, but no device is currently bound.\r\n\r\n"
        L"Candidates found now: %d\r\n"
        L"Bound count: %d\r\n"
        L"Update calls succeeded: %d / %d\r\n"
        L"Sample HWID: %s\r\n\r\n"
        L"Try: Reconnect in tray, or remove/re-pair Magic Mouse in Bluetooth settings.",
        cands, bound, updated, tries, sample);
    MessageBoxW(NULL, msg, L"Magic Mouse - Not bound", MB_OK | MB_ICONINFORMATION);
    return 0;
}

static BOOL IsRunningAsAdmin(void)
{
    BOOL admin = FALSE;
    PSID sid = NULL;
    SID_IDENTIFIER_AUTHORITY auth = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&auth, 2, SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0, &sid)) {
        CheckTokenMembership(NULL, sid, &admin);
        FreeSid(sid);
    }
    return admin;
}

static void RelaunchInstallerElevated(void)
{
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(NULL, self, _countof(self));

    SHELLEXECUTEINFOW se = { sizeof(se) };
    se.fMask = SEE_MASK_NOCLOSEPROCESS;
    se.lpVerb = L"runas";
    se.lpFile = self;
    se.lpParameters = ARG_INSTALL_DRIVER;
    se.nShow = SW_SHOW;
    if (!ShellExecuteExW(&se)) {
        if (GetLastError() != ERROR_CANCELLED) {
            MessageBoxW(NULL, L"Failed to launch elevated installer.", L"Magic Mouse", MB_OK | MB_ICONERROR);
        }
        return;
    }

    if (se.hProcess) {
        for (;;) {
            DWORD r = MsgWaitForMultipleObjects(1, &se.hProcess, FALSE, INFINITE, QS_ALLINPUT);
            if (r == WAIT_OBJECT_0) break;
            MSG m;
            while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        }
        CloseHandle(se.hProcess);
    }
}

static AppState QueryState(int* outCandidates, int* outBound)
{
    if (g_dev) {
        int b = 0, c = 0;
        HANDLE h = OpenMagicMouse(&b, &c);
        if (h) CloseHandle(h);
        if (outCandidates) *outCandidates = c;
        if (outBound) *outBound = b;
        if (b > 0) return ST_CONNECTED;
        if (!IsServiceInstalled(L"MagicMouse")) return ST_DRIVER_MISSING;
        if (c > 0) return ST_DRIVER_MISMATCH;
        return ST_NOT_PAIRED;
    }
    int bound = 0, cands = 0;
    HANDLE h = OpenMagicMouse(&bound, &cands);
    if (h) {
        CloseHandle(h);
        if (outCandidates) *outCandidates = cands;
        if (outBound) *outBound = bound;
        if (bound > 0) return ST_CONNECTED;
        if (!IsServiceInstalled(L"MagicMouse")) return ST_DRIVER_MISSING;
        if (cands > 0) return ST_DRIVER_MISMATCH;
        return ST_NOT_PAIRED;
    }
    if (outCandidates) *outCandidates = cands;
    if (outBound) *outBound = bound;
    if (!IsServiceInstalled(L"MagicMouse")) return ST_DRIVER_MISSING;
    if (cands > 0) return ST_DRIVER_MISMATCH;
    return ST_NOT_PAIRED;
}

static void UpdateTrayTip(void)
{
    AppState st = QueryState(NULL, NULL);
    const wchar_t* tip = L"Magic Mouse - not connected";
    if (st == ST_CONNECTED) tip = L"Magic Mouse - connected";
    else if (st == ST_DRIVER_MISMATCH) tip = L"Magic Mouse - detected, driver not bound";
    else if (st == ST_NOT_PAIRED) tip = L"Magic Mouse - not paired";
    else if (st == ST_DRIVER_MISSING) tip = L"Magic Mouse - driver not installed";
    StringCchCopyW(g_nid.szTip, _countof(g_nid.szTip), tip);
    g_nid.uFlags |= NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void OfferDriverInstall(BOOL forceAsk)
{
    int cands = 0, bound = 0;
    AppState st = QueryState(&cands, &bound);
    const wchar_t* msg = NULL;

    if (st == ST_CONNECTED) {
        if (!forceAsk) return;
        msg = L"Driver is already working. Reinstall anyway?";
    } else if (st == ST_DRIVER_MISSING) {
        msg = L"Magic Mouse driver is not installed.\r\n\r\nInstall bundled driver now?";
    } else if (st == ST_NOT_PAIRED) {
        if (!forceAsk) return;
        msg = L"No Magic Mouse is currently paired/online.\r\n\r\nRun installer anyway?";
    } else {
        msg = L"Magic Mouse is detected, but driver is not bound for scrolling.\r\n\r\nTry to switch driver now?";
    }

    int r = MessageBoxW(NULL, msg, L"Magic Mouse - Driver setup",
        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1);
    if (r != IDYES) return;

    if (IsRunningAsAdmin()) RunDriverInstallElevated();
    else RelaunchInstallerElevated();

    StartReader();
    UpdateTrayTip();
}

static void ShowStatus(void)
{
    int cands = 0, bound = 0;
    AppState st = QueryState(&cands, &bound);
    const wchar_t* stateText = L"unknown";
    if (st == ST_CONNECTED) stateText = L"connected";
    else if (st == ST_DRIVER_MISMATCH) stateText = L"driver mismatch";
    else if (st == ST_NOT_PAIRED) stateText = L"not paired";
    else if (st == ST_DRIVER_MISSING) stateText = L"driver missing";

    wchar_t msg[1024];
    StringCchPrintfW(msg, _countof(msg),
        L"State: %s\r\nCandidates: %d\r\nBound count: %d\r\nLast open path: %s\r\n",
        stateText, cands, bound, g_lastOpenPath[0] ? g_lastOpenPath : L"(none)");
    MessageBoxW(NULL, msg, L"Magic Mouse - Status", MB_OK | MB_ICONINFORMATION);
}

static HICON CreateTrayIcon(void)
{
    BITMAPINFO bi = { 0 };
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = 16;
    bi.bmiHeader.biHeight = 16;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    UINT32* bits = NULL;
    HDC hdc = GetDC(NULL);
    HBITMAP color = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, (void**)&bits, NULL, 0);
    ReleaseDC(NULL, hdc);
    if (!color || !bits) return LoadIconW(NULL, IDI_APPLICATION);

    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            int dx = x - 8, dy = y - 8;
            int d2 = dx * dx + dy * dy;
            UINT32* px = &bits[(15 - y) * 16 + x];
            if (d2 <= 9) *px = 0xFF0A84FFu;
            else if (d2 <= 49) *px = 0xFFFFFFFFu;
            else *px = 0x00000000u;
        }
    }

    HBITMAP mask = CreateBitmap(16, 16, 1, 1, NULL);
    ICONINFO ii = { 0 };
    ii.fIcon = TRUE;
    ii.hbmMask = mask;
    ii.hbmColor = color;
    HICON ico = CreateIconIndirect(&ii);
    DeleteObject(mask);
    DeleteObject(color);
    return ico ? ico : LoadIconW(NULL, IDI_APPLICATION);
}

static void ShowTrayMenu(void)
{
    POINT pt;
    GetCursorPos(&pt);

    HMENU m = CreatePopupMenu();
    HMENU speed = CreatePopupMenu();

    for (int i = 0; i < (int)_countof(kSpeedPresets); i++) {
        UINT f = MF_STRING;
        if (g_s.speed_x10 == kSpeedPresets[i]) f |= MF_CHECKED;
        AppendMenuW(speed, f, ID_M_SPEED_BASE + kSpeedPresets[i], kSpeedNames[i]);
    }

    AppState st = QueryState(NULL, NULL);
    const wchar_t* title = L"Magic Mouse  [not connected]";
    if (st == ST_CONNECTED) title = L"Magic Mouse  [connected]";
    else if (st == ST_DRIVER_MISMATCH) title = L"Magic Mouse  [detected, driver not bound]";
    else if (st == ST_NOT_PAIRED) title = L"Magic Mouse  [not paired]";
    else if (st == ST_DRIVER_MISSING) title = L"Magic Mouse  [driver missing]";

    AppendMenuW(m, MF_STRING | MF_DISABLED, 0, title);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)speed, L"Scroll speed");
    AppendMenuW(m, MF_STRING | (g_s.natural ? MF_CHECKED : 0), ID_M_NATURAL, L"Natural scroll");
    AppendMenuW(m, MF_STRING | (g_s.horizontal ? MF_CHECKED : 0), ID_M_HORIZONTAL, L"Horizontal scroll");
    AppendMenuW(m, MF_STRING | (g_s.autostart ? MF_CHECKED : 0), ID_M_AUTOSTART, L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_M_RECONNECT, L"Reconnect");
    AppendMenuW(m, MF_STRING, ID_M_REINSTALL, L"Reinstall driver...");
    AppendMenuW(m, MF_STRING, ID_M_STATUS, L"Status...");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_M_QUIT, L"Quit");

    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(m, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, NULL);
    DestroyMenu(m);
}

static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
        case WM_TRAY:
            if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_RBUTTONUP) ShowTrayMenu();
            return 0;

        case WM_COMMAND: {
            WORD id = LOWORD(wp);
            if (id >= ID_M_SPEED_BASE && id < ID_M_SPEED_BASE + 1000) {
                g_s.speed_x10 = id - ID_M_SPEED_BASE;
                SaveSettings();
                return 0;
            }

            switch (id) {
                case ID_M_NATURAL:
                    g_s.natural = !g_s.natural;
                    SaveSettings();
                    break;
                case ID_M_HORIZONTAL:
                    g_s.horizontal = !g_s.horizontal;
                    SaveSettings();
                    break;
                case ID_M_AUTOSTART:
                    g_s.autostart = !g_s.autostart;
                    SaveSettings();
                    ApplyAutostart();
                    break;
                case ID_M_RECONNECT:
                    StartReader();
                    UpdateTrayTip();
                    if (!g_dev) SetTimer(hwnd, 1, 3000, NULL);
                    break;
                case ID_M_REINSTALL:
                    OfferDriverInstall(TRUE);
                    break;
                case ID_M_STATUS:
                    ShowStatus();
                    break;
                case ID_M_QUIT:
                    StopReader();
                    Shell_NotifyIconW(NIM_DELETE, &g_nid);
                    PostQuitMessage(0);
                    break;
            }
            return 0;
        }

        case WM_DEVICE_LOST:
            StopReader();
            UpdateTrayTip();
            SetTimer(hwnd, 1, 3000, NULL);
            return 0;

        case WM_TIMER:
            if (wp == 1) {
                if (StartReader()) KillTimer(hwnd, 1);
                UpdateTrayTip();
            }
            return 0;

        case WM_DESTROY:
            StopReader();
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE prev, LPWSTR cmdLine, int show)
{
    (void)prev;
    (void)show;

    if (cmdLine && wcsstr(cmdLine, ARG_INSTALL_DRIVER)) {
        return RunDriverInstallElevated();
    }

    HANDLE mtx = CreateMutexW(NULL, TRUE, L"Global\\MagicMouseSimple_SingleInstance");
    if (mtx == NULL || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mtx) CloseHandle(mtx);
        return 0;
    }

    LoadSettings();
    if (g_s.autostart) ApplyAutostart();

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = hInst;
    wc.lpszClassName = L"MagicMouseTrayWnd";
    RegisterClassW(&wc);

    g_hwnd = CreateWindowW(L"MagicMouseTrayWnd", L"MagicMouse", 0,
        0, 0, 0, 0, HWND_MESSAGE, NULL, hInst, NULL);

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = ID_TRAY;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = CreateTrayIcon();
    StringCchCopyW(g_nid.szTip, _countof(g_nid.szTip), L"Magic Mouse");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    if (!StartReader()) {
        if (!IsServiceInstalled(L"MagicMouse")) OfferDriverInstall(FALSE);
        SetTimer(g_hwnd, 1, 3000, NULL);
    }
    UpdateTrayTip();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (mtx) {
        ReleaseMutex(mtx);
        CloseHandle(mtx);
    }
    return 0;
}

