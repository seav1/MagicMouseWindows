// ============================================================================
// MagicMouse.cpp - single-file Win32 implementation (no .NET, no MFC, no STL)
//
// What it does
//   1. On first run (or when the driver isn't bound to your mouse) it offers
//      to extract the embedded MagicMouse.sys / .inf / .cat to %TEMP%, relaunch
//      itself elevated, run pnputil to install + rebind, and resume.
//   2. Walks the PnP tree to find the device whose driver service is
//      "MagicMouse" (Magic Utilities' kernel driver), opens its raw PDO via
//      "\\.\GLOBALROOT\Device\<pdo-name>", and reads touch reports.
//   3. Translates X/Y deltas into standard mouse-wheel events using SendInput.
//   4. Lives in the system tray with a right-click menu.
//
// Build:
//   cl /O2 /MT /EHsc /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 \
//      MagicMouse.cpp MagicMouse.res \
//      /link /SUBSYSTEM:WINDOWS /OUT:MagicMouse.exe \
//      setupapi.lib user32.lib shell32.lib advapi32.lib gdi32.lib ole32.lib
//
// Resulting exe is ~6.5 MB (driver embedded), runs on Windows 8.1 / 10 / 11.
// ============================================================================

#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#ifndef _WIN32_WINNT
#define _WIN32_WINNT 0x0A00
#endif

#include <windows.h>
#include <setupapi.h>
#include <shellapi.h>
#include <shlobj.h>
#include <strsafe.h>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "ole32.lib")

// ---------------------------------------------------------------------------
// Constants
// ---------------------------------------------------------------------------
#define WM_TRAY        (WM_APP + 1)
#define WM_DEVICE_LOST (WM_APP + 2)

#define ID_TRAY            1
#define ID_M_QUIT          1001
#define ID_M_NATURAL       1002
#define ID_M_HORIZONTAL    1003
#define ID_M_RECONNECT     1004
#define ID_M_AUTOSTART     1005
#define ID_M_REINSTALL     1006
#define ID_M_SPEED_BASE    2000  // menu id = 2000 + speed_x10

// Embedded driver resource IDs (must match MagicMouse.rc)
#define RES_DRV_INF  100
#define RES_DRV_SYS  101
#define RES_DRV_CAT  102

// Hidden command-line flag used when the app relaunches itself elevated.
#define ARG_INSTALL_DRIVER L"--install-driver"

static const int     kSpeedPresets[] = { 10, 20, 30, 50, 80, 100 };
static const wchar_t* kSpeedNames[]  = {
    L"1.0x  (slow)", L"2.0x", L"3.0x  (default)",
    L"5.0x", L"8.0x", L"10.0x (fastest)"
};

// ---------------------------------------------------------------------------
// Settings (persisted to %APPDATA%\MagicMouse\config.ini)
// ---------------------------------------------------------------------------
typedef struct {
    int  speed_x10;
    BOOL natural;
    BOOL horizontal;
    BOOL autostart;
} Settings;

static Settings g_s = { 30, FALSE, TRUE, FALSE };

// ---------------------------------------------------------------------------
// Runtime state
// ---------------------------------------------------------------------------
static HWND             g_hwnd   = NULL;
static NOTIFYICONDATAW  g_nid    = { 0 };
static HANDLE           g_dev    = NULL;
static HANDLE           g_thread = NULL;
static volatile LONG    g_running = 0;
static int              g_accX = 0, g_accY = 0;
static wchar_t          g_lastOpenPath[MAX_PATH] = L"";
static BOOL             g_offeredInstall = FALSE;

// ===========================================================================
// Settings persistence
// ===========================================================================
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
    GetIniPath(p, MAX_PATH);
    g_s.speed_x10  =       GetPrivateProfileIntW(L"main", L"speed_x10",  30, p);
    g_s.natural    = (BOOL)GetPrivateProfileIntW(L"main", L"natural",     0, p);
    g_s.horizontal = (BOOL)GetPrivateProfileIntW(L"main", L"horizontal",  1, p);
    g_s.autostart  = (BOOL)GetPrivateProfileIntW(L"main", L"autostart",   0, p);
    if (g_s.speed_x10 < 1)   g_s.speed_x10 = 1;
    if (g_s.speed_x10 > 200) g_s.speed_x10 = 200;
}

static void SaveSettings(void)
{
    wchar_t p[MAX_PATH], v[16];
    GetIniPath(p, MAX_PATH);
    StringCchPrintfW(v, 16, L"%d", g_s.speed_x10);  WritePrivateProfileStringW(L"main", L"speed_x10",  v, p);
    StringCchPrintfW(v, 16, L"%d", g_s.natural);    WritePrivateProfileStringW(L"main", L"natural",    v, p);
    StringCchPrintfW(v, 16, L"%d", g_s.horizontal); WritePrivateProfileStringW(L"main", L"horizontal", v, p);
    StringCchPrintfW(v, 16, L"%d", g_s.autostart);  WritePrivateProfileStringW(L"main", L"autostart",  v, p);
}

static void ApplyAutostart(void)
{
    HKEY k;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
            L"Software\\Microsoft\\Windows\\CurrentVersion\\Run",
            0, KEY_SET_VALUE, &k) != ERROR_SUCCESS) return;
    if (g_s.autostart) {
        wchar_t exe[MAX_PATH], val[MAX_PATH + 4];
        GetModuleFileNameW(NULL, exe, MAX_PATH);
        StringCchPrintfW(val, MAX_PATH + 4, L"\"%s\"", exe);
        RegSetValueExW(k, L"MagicMouse", 0, REG_SZ, (const BYTE*)val,
                       (DWORD)((wcslen(val) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"MagicMouse");
    }
    RegCloseKey(k);
}

// ===========================================================================
// Device discovery (open by service name -> physical PDO path)
// ===========================================================================
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

// Returns first openable PDO for a device whose service is "MagicMouse",
// or NULL. Also fills outBound with the count of MagicMouse-bound devices.
static HANDLE OpenMagicMouse(int* outBound)
{
    if (outBound) *outBound = 0;
    HANDLE result = NULL;
    HDEVINFO ds = SetupDiGetClassDevsW(NULL, NULL, NULL,
        DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (ds == INVALID_HANDLE_VALUE) return NULL;

    SP_DEVINFO_DATA d = { sizeof(d) };
    for (DWORD i = 0; SetupDiEnumDeviceInfo(ds, i, &d); i++) {
        wchar_t svc[64] = { 0 };
        if (!SetupDiGetDeviceRegistryPropertyW(ds, &d, SPDRP_SERVICE, NULL,
                (BYTE*)svc, sizeof(svc), NULL))
            continue;
        if (_wcsicmp(svc, L"MagicMouse") != 0) continue;

        if (outBound) (*outBound)++;
        if (result) continue;  // keep counting, but don't open more

        wchar_t pdo[260] = { 0 };
        if (!SetupDiGetDeviceRegistryPropertyW(ds, &d,
                SPDRP_PHYSICAL_DEVICE_OBJECT_NAME, NULL,
                (BYTE*)pdo, sizeof(pdo), NULL))
            continue;

        wchar_t full[300];
        StringCchPrintfW(full, 300, L"\\\\.\\GLOBALROOT%s", pdo);
        result = TryOpenPath(full);
        if (result) StringCchCopyW(g_lastOpenPath, MAX_PATH, full);
    }
    SetupDiDestroyDeviceInfoList(ds);
    return result;
}

// Tests whether a wide-char hardware ID buffer contains "05ac" or "004c"
// (USB-IF Apple VID, Bluetooth-SIG Apple VID). Case-insensitive.
static BOOL HwidLooksApple(const wchar_t* p, DWORD n)
{
    for (DWORD k = 0; k + 3 < n; k++) {
        wchar_t c2 = p[k+2] | 0x20;
        wchar_t c3 = p[k+3] | 0x20;
        if (p[k] == L'0' && p[k+1] == L'5' && c2 == L'a' && c3 == L'c') return TRUE;
        if (p[k] == L'0' && p[k+1] == L'0' && p[k+2] == L'4' && c3 == L'c') return TRUE;
    }
    return FALSE;
}

// Counts devices whose hardware ID contains an Apple VID. Used to decide
// whether to offer driver install (vs the mouse simply not being paired).
static int CountAppleDevices(void)
{
    HDEVINFO ds = SetupDiGetClassDevsW(NULL, NULL, NULL,
        DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (ds == INVALID_HANDLE_VALUE) return 0;

    SP_DEVINFO_DATA d = { sizeof(d) };
    BYTE buf[2048];
    int count = 0;
    for (DWORD i = 0; SetupDiEnumDeviceInfo(ds, i, &d); i++) {
        DWORD got = 0;
        if (!SetupDiGetDeviceRegistryPropertyW(ds, &d, SPDRP_HARDWAREID, NULL,
                buf, sizeof(buf), &got) || got < 4)
            continue;
        if (HwidLooksApple((wchar_t*)buf, got / sizeof(wchar_t))) count++;
    }
    SetupDiDestroyDeviceInfoList(ds);
    return count;
}

// ===========================================================================
// Read loop and SendInput
// ===========================================================================
static void EmitWheel(int tickY, int tickX)
{
    INPUT in[2] = { 0 };
    int n = 0;
    if (tickY != 0) {
        in[n].type         = INPUT_MOUSE;
        in[n].mi.dwFlags   = MOUSEEVENTF_WHEEL;
        in[n].mi.mouseData = (DWORD)(tickY * WHEEL_DELTA);
        n++;
    }
    if (tickX != 0 && g_s.horizontal) {
        in[n].type         = INPUT_MOUSE;
        in[n].mi.dwFlags   = MOUSEEVENTF_HWHEEL;
        in[n].mi.mouseData = (DWORD)(tickX * WHEEL_DELTA);
        n++;
    }
    if (n) SendInput(n, in, sizeof(INPUT));
}

static DWORD WINAPI ReadThread(LPVOID arg)
{
    (void)arg;
    BYTE buf[64];
    DWORD bytes;
    while (g_running && g_dev) {
        if (!ReadFile(g_dev, buf, sizeof(buf), &bytes, NULL) || bytes < 3) {
            DWORD e = GetLastError();
            if (e == ERROR_INVALID_HANDLE      ||
                e == ERROR_DEVICE_NOT_CONNECTED ||
                e == ERROR_OPERATION_ABORTED   ||
                e == ERROR_NOT_READY)
            {
                PostMessageW(g_hwnd, WM_DEVICE_LOST, 0, 0);
                break;
            }
            Sleep(10);
            continue;
        }
        // Magic Utilities raw report layout: [0]=reportId, [1]=dY, [2]=dX (signed bytes).
        int dy = (signed char)buf[1];
        int dx = (signed char)buf[2];
        if (g_s.natural) { dy = -dy; dx = -dx; }

        g_accY += dy * g_s.speed_x10;
        g_accX += dx * g_s.speed_x10;

        const int kT = WHEEL_DELTA * 10;  // = 1200 (speed stored as tenths)
        int ty = g_accY / kT;
        int tx = g_accX / kT;
        if (ty) g_accY -= ty * kT;
        if (tx) g_accX -= tx * kT;

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
    g_lastOpenPath[0] = 0;
}

static BOOL StartReader(void)
{
    StopReader();
    g_dev = OpenMagicMouse(NULL);
    if (!g_dev) return FALSE;
    g_running = 1;
    g_thread = CreateThread(NULL, 0, ReadThread, NULL, 0, NULL);
    return TRUE;
}

static void UpdateTrayTip(void)
{
    StringCchCopyW(g_nid.szTip, _countof(g_nid.szTip),
        g_dev ? L"Magic Mouse - connected" : L"Magic Mouse - not connected");
    g_nid.uFlags |= NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

// ===========================================================================
// Embedded driver extraction + auto-install
// ===========================================================================
static BOOL ExtractResource(int rsrcId, const wchar_t* destPath)
{
    HRSRC res = FindResourceW(NULL, MAKEINTRESOURCEW(rsrcId), RT_RCDATA);
    if (!res) return FALSE;
    HGLOBAL data = LoadResource(NULL, res);
    if (!data) return FALSE;
    DWORD size = SizeofResource(NULL, res);
    void* p = LockResource(data);
    if (!p || !size) return FALSE;

    HANDLE h = CreateFileW(destPath, GENERIC_WRITE, 0, NULL,
        CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) return FALSE;
    DWORD written = 0;
    BOOL ok = WriteFile(h, p, size, &written, NULL) && written == size;
    CloseHandle(h);
    if (!ok) DeleteFileW(destPath);
    return ok;
}

static BOOL ExtractDriversToTemp(wchar_t* outDir, size_t cchDir)
{
    wchar_t tempBase[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tempBase)) return FALSE;
    StringCchPrintfW(outDir, cchDir, L"%sMagicMouseDrv-%lu", tempBase, GetCurrentProcessId());
    if (!CreateDirectoryW(outDir, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        return FALSE;

    wchar_t inf[MAX_PATH], sys[MAX_PATH], cat[MAX_PATH];
    StringCchPrintfW(inf, MAX_PATH, L"%s\\MagicMouse.inf", outDir);
    StringCchPrintfW(sys, MAX_PATH, L"%s\\MagicMouse.sys", outDir);
    StringCchPrintfW(cat, MAX_PATH, L"%s\\MagicMouse.cat", outDir);

    return ExtractResource(RES_DRV_INF, inf)
        && ExtractResource(RES_DRV_SYS, sys)
        && ExtractResource(RES_DRV_CAT, cat);
}

static void DeleteFolderRecursive(const wchar_t* dir)
{
    wchar_t pattern[MAX_PATH];
    StringCchPrintfW(pattern, MAX_PATH, L"%s\\*", dir);
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(pattern, &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
            wchar_t full[MAX_PATH];
            StringCchPrintfW(full, MAX_PATH, L"%s\\%s", dir, fd.cFileName);
            DeleteFileW(full);
        } while (FindNextFileW(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryW(dir);
}

// Run a console process synchronously, return exit code; capture combined output.
static DWORD RunCapture(const wchar_t* cmdline, wchar_t* outBuf, size_t outCch)
{
    if (outBuf && outCch) outBuf[0] = 0;

    SECURITY_ATTRIBUTES sa = { sizeof(sa) };
    sa.bInheritHandle = TRUE;
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 0)) return (DWORD)-1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);

    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags    = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    si.hStdOutput = wr;
    si.hStdError  = wr;
    si.hStdInput  = NULL;
    PROCESS_INFORMATION pi = { 0 };

    wchar_t mut[1024];
    StringCchCopyW(mut, 1024, cmdline);
    if (!CreateProcessW(NULL, mut, NULL, NULL, TRUE,
            CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        CloseHandle(rd); CloseHandle(wr);
        return (DWORD)-1;
    }
    CloseHandle(wr);

    char  rbuf[4096];
    DWORD got;
    size_t outLen = outBuf ? wcslen(outBuf) : 0;
    while (ReadFile(rd, rbuf, sizeof(rbuf) - 1, &got, NULL) && got) {
        rbuf[got] = 0;
        if (outBuf && outCch) {
            wchar_t wbuf[4096];
            int wn = MultiByteToWideChar(CP_ACP, 0, rbuf, (int)got, wbuf, _countof(wbuf) - 1);
            if (wn > 0) {
                wbuf[wn] = 0;
                if (outLen + wn + 1 < outCch) {
                    StringCchCatW(outBuf, outCch, wbuf);
                    outLen += wn;
                }
            }
        }
    }
    CloseHandle(rd);

    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return exitCode;
}

// ELEVATED entry point: extract -> add-driver -> remove Apple devices ->
// scan-devices. Run from RunAsAdminAndInstall via "--install-driver" flag.
static int RunDriverInstallElevated(void)
{
    wchar_t log[16384];
    StringCchCopyW(log, _countof(log),
        L"=== Magic Mouse driver install log ===\r\n\r\n");

    wchar_t dir[MAX_PATH];
    if (!ExtractDriversToTemp(dir, MAX_PATH)) {
        MessageBoxW(NULL,
            L"Failed to extract embedded driver files.",
            L"Magic Mouse - Install error", MB_OK | MB_ICONERROR);
        return 1;
    }

    StringCchCatW(log, _countof(log), L"Extracted to: ");
    StringCchCatW(log, _countof(log), dir);
    StringCchCatW(log, _countof(log), L"\r\n\r\n");

    // 1) pnputil /add-driver <inf> /install
    wchar_t cmd[1024];
    StringCchPrintfW(cmd, 1024,
        L"pnputil.exe /add-driver \"%s\\MagicMouse.inf\" /install", dir);
    StringCchCatW(log, _countof(log), L"[1/3] Adding driver to driver store...\r\n");
    StringCchCatW(log, _countof(log), cmd);
    StringCchCatW(log, _countof(log), L"\r\n");
    DWORD rc = RunCapture(cmd, log, _countof(log));
    wchar_t line[64];
    StringCchPrintfW(line, 64, L"[exit %lu]\r\n\r\n", rc);
    StringCchCatW(log, _countof(log), line);

    // 2) Find every Apple device + remove its instance so PnP re-evaluates
    StringCchCatW(log, _countof(log), L"[2/3] Re-binding existing Apple devices...\r\n");

    HDEVINFO ds = SetupDiGetClassDevsW(NULL, NULL, NULL,
        DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (ds != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA d = { sizeof(d) };
        BYTE hwbuf[2048];
        for (DWORD i = 0; SetupDiEnumDeviceInfo(ds, i, &d); i++) {
            DWORD got = 0;
            if (!SetupDiGetDeviceRegistryPropertyW(ds, &d, SPDRP_HARDWAREID, NULL,
                    hwbuf, sizeof(hwbuf), &got) || got < 4)
                continue;
            if (!HwidLooksApple((wchar_t*)hwbuf, got / sizeof(wchar_t)))
                continue;

            wchar_t instId[256] = { 0 };
            if (!SetupDiGetDeviceInstanceIdW(ds, &d, instId, _countof(instId), NULL))
                continue;

            StringCchPrintfW(cmd, 1024, L"pnputil.exe /remove-device \"%s\"", instId);
            StringCchCatW(log, _countof(log), L"  removing ");
            StringCchCatW(log, _countof(log), instId);
            StringCchCatW(log, _countof(log), L"\r\n");
            RunCapture(cmd, log, _countof(log));
        }
        SetupDiDestroyDeviceInfoList(ds);
    }
    StringCchCatW(log, _countof(log), L"\r\n");

    // 3) Trigger bus rescan so PnP picks our newly-added INF
    StringCchCatW(log, _countof(log), L"[3/3] Rescanning bus...\r\n");
    StringCchCopyW(cmd, 1024, L"pnputil.exe /scan-devices");
    rc = RunCapture(cmd, log, _countof(log));
    StringCchPrintfW(line, 64, L"[exit %lu]\r\n", rc);
    StringCchCatW(log, _countof(log), line);

    Sleep(1500);

    // Check final state
    int bound = 0;
    HANDLE check = OpenMagicMouse(&bound);
    if (check) CloseHandle(check);

    StringCchCatW(log, _countof(log), L"\r\n----\r\n");
    StringCchPrintfW(line, 64, L"Devices now bound to MagicMouse: %d\r\n", bound);
    StringCchCatW(log, _countof(log), line);

    DeleteFolderRecursive(dir);

    if (bound > 0) {
        MessageBoxW(NULL,
            L"Driver installed and bound to your Magic Mouse.\r\n"
            L"You can close this dialog; the app will resume automatically.",
            L"Magic Mouse - Install OK", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    StringCchCatW(log, _countof(log),
        L"\r\nNo Apple device is bound to MagicMouse yet.\r\n"
        L"Possible reasons:\r\n"
        L"  - Mouse isn't paired/turned on - pair it via Settings -> Bluetooth\r\n"
        L"    and run this installer again from the tray menu.\r\n"
        L"  - Driver INF doesn't list your specific HardwareID (rare).\r\n");
    MessageBoxW(NULL, log, L"Magic Mouse - Install result",
        MB_OK | MB_ICONWARNING);
    return 2;
}

// ===========================================================================
// UAC relaunch helper
// ===========================================================================
static BOOL IsRunningAsAdmin(void)
{
    BOOL admin = FALSE;
    PSID admGroup = NULL;
    SID_IDENTIFIER_AUTHORITY auth = SECURITY_NT_AUTHORITY;
    if (AllocateAndInitializeSid(&auth, 2,
            SECURITY_BUILTIN_DOMAIN_RID, DOMAIN_ALIAS_RID_ADMINS,
            0, 0, 0, 0, 0, 0, &admGroup)) {
        CheckTokenMembership(NULL, admGroup, &admin);
        FreeSid(admGroup);
    }
    return admin;
}

static void RelaunchInstallerElevated(void)
{
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(NULL, self, MAX_PATH);

    SHELLEXECUTEINFOW sei = { sizeof(sei) };
    sei.fMask  = SEE_MASK_NOCLOSEPROCESS;
    sei.lpVerb = L"runas";
    sei.lpFile = self;
    sei.lpParameters = ARG_INSTALL_DRIVER;
    sei.nShow  = SW_SHOW;

    if (!ShellExecuteExW(&sei)) {
        DWORD e = GetLastError();
        if (e == ERROR_CANCELLED) return;  // user clicked No on UAC
        MessageBoxW(NULL, L"Failed to launch elevated installer.",
            L"Magic Mouse", MB_OK | MB_ICONERROR);
        return;
    }
    if (sei.hProcess) {
        // Wait for the elevated installer to finish but keep the tray
        // responsive by pumping messages.
        for (;;) {
            DWORD r = MsgWaitForMultipleObjects(1, &sei.hProcess, FALSE,
                INFINITE, QS_ALLINPUT);
            if (r == WAIT_OBJECT_0) break;
            MSG m;
            while (PeekMessageW(&m, NULL, 0, 0, PM_REMOVE)) {
                TranslateMessage(&m);
                DispatchMessageW(&m);
            }
        }
        CloseHandle(sei.hProcess);
    }
}

static void OfferDriverInstall(BOOL forceAsk)
{
    int boundCount = 0;
    HANDLE h = OpenMagicMouse(&boundCount);
    if (h) { CloseHandle(h); }
    int appleCount = CountAppleDevices();

    // Decide whether to prompt
    const wchar_t* msg = NULL;
    if (boundCount > 0) {
        if (!forceAsk) return;  // already bound, nothing to do unless user asked
        msg = L"The driver appears to be installed and bound. Reinstall it anyway?";
    } else if (appleCount == 0) {
        if (!forceAsk) return;  // mouse simply isn't paired/on
        msg = L"No Apple device detected in PnP yet.\r\n"
              L"Make sure the mouse is paired via Bluetooth, then proceed?";
    } else {
        msg = L"The Magic Mouse driver isn't bound to your mouse.\r\n\r\n"
              L"Install/rebind the embedded driver now?\r\n"
              L"(Windows will ask for administrator approval.)";
    }

    int r = MessageBoxW(NULL, msg, L"Magic Mouse - Driver setup",
        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1);
    if (r != IDYES) return;

    if (IsRunningAsAdmin()) {
        RunDriverInstallElevated();
    } else {
        RelaunchInstallerElevated();
    }

    // Try to reconnect after install
    StartReader();
    UpdateTrayTip();
}

// ===========================================================================
// Tray icon (programmatic 16x16 white circle + blue dot)
// ===========================================================================
static HICON CreateTrayIcon(void)
{
    BITMAPINFO bi = { 0 };
    bi.bmiHeader.biSize        = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth       = 16;
    bi.bmiHeader.biHeight      = 16;
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    UINT32* bits = NULL;
    HDC hdc = GetDC(NULL);
    HBITMAP hbm = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, (void**)&bits, NULL, 0);
    ReleaseDC(NULL, hdc);
    if (!hbm || !bits) return LoadIconW(NULL, IDI_APPLICATION);

    for (int y = 0; y < 16; y++) {
        for (int x = 0; x < 16; x++) {
            int dx = x - 8, dy = y - 8;
            int d2 = dx * dx + dy * dy;
            UINT32* px = &bits[(15 - y) * 16 + x];
            if      (d2 <= 9)  *px = 0xFF0A84FFu;
            else if (d2 <= 49) *px = 0xFFFFFFFFu;
            else               *px = 0x00000000u;
        }
    }

    HBITMAP mask = CreateBitmap(16, 16, 1, 1, NULL);
    ICONINFO ii = { 0 };
    ii.fIcon = TRUE; ii.hbmMask = mask; ii.hbmColor = hbm;
    HICON icon = CreateIconIndirect(&ii);
    DeleteObject(hbm);
    DeleteObject(mask);
    return icon ? icon : LoadIconW(NULL, IDI_APPLICATION);
}

// ===========================================================================
// Tray menu
// ===========================================================================
static void ShowTrayMenu(void)
{
    POINT pt;
    GetCursorPos(&pt);

    HMENU menu = CreatePopupMenu();
    HMENU spd  = CreatePopupMenu();

    for (int i = 0; i < (int)_countof(kSpeedPresets); i++) {
        UINT f = MF_STRING;
        if (g_s.speed_x10 == kSpeedPresets[i]) f |= MF_CHECKED;
        AppendMenuW(spd, f, ID_M_SPEED_BASE + kSpeedPresets[i], kSpeedNames[i]);
    }

    AppendMenuW(menu, MF_STRING | MF_DISABLED, 0,
        g_dev ? L"Magic Mouse  [connected]" : L"Magic Mouse  [not connected]");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_POPUP, (UINT_PTR)spd, L"Scroll speed");
    AppendMenuW(menu, MF_STRING | (g_s.natural    ? MF_CHECKED : 0), ID_M_NATURAL,    L"Natural scroll");
    AppendMenuW(menu, MF_STRING | (g_s.horizontal ? MF_CHECKED : 0), ID_M_HORIZONTAL, L"Horizontal scroll");
    AppendMenuW(menu, MF_STRING | (g_s.autostart  ? MF_CHECKED : 0), ID_M_AUTOSTART,  L"Start with Windows");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, ID_M_RECONNECT, L"Reconnect");
    AppendMenuW(menu, MF_STRING, ID_M_REINSTALL, L"Reinstall driver...");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, ID_M_QUIT, L"Quit");

    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, NULL);
    DestroyMenu(menu);
}

// ===========================================================================
// Window procedure
// ===========================================================================
static LRESULT CALLBACK WndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    switch (msg) {
        case WM_TRAY:
            if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP)
                ShowTrayMenu();
            return 0;

        case WM_COMMAND: {
            WORD id = LOWORD(wp);
            if (id >= ID_M_SPEED_BASE && id < ID_M_SPEED_BASE + 1000) {
                g_s.speed_x10 = id - ID_M_SPEED_BASE;
                SaveSettings();
                return 0;
            }
            switch (id) {
                case ID_M_NATURAL:    g_s.natural    = !g_s.natural;    SaveSettings(); break;
                case ID_M_HORIZONTAL: g_s.horizontal = !g_s.horizontal; SaveSettings(); break;
                case ID_M_AUTOSTART:  g_s.autostart  = !g_s.autostart;  SaveSettings(); ApplyAutostart(); break;
                case ID_M_RECONNECT:
                    StartReader();
                    UpdateTrayTip();
                    if (!g_dev) SetTimer(hwnd, 1, 3000, NULL);
                    break;
                case ID_M_REINSTALL:
                    OfferDriverInstall(TRUE);
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
                if (!g_dev && !g_offeredInstall && CountAppleDevices() > 0) {
                    g_offeredInstall = TRUE;
                    OfferDriverInstall(FALSE);
                }
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

// ===========================================================================
// Entry point
// ===========================================================================
int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE prev, LPWSTR cmdLine, int show)
{
    (void)prev; (void)show;

    // Hidden mode: this process was relaunched with --install-driver as admin.
    if (cmdLine && wcsstr(cmdLine, ARG_INSTALL_DRIVER)) {
        return RunDriverInstallElevated();
    }

    // Single-instance guard for normal mode
    HANDLE mtx = CreateMutexW(NULL, TRUE, L"Global\\MagicMouseSimple_SingleInstance");
    if (mtx == NULL || GetLastError() == ERROR_ALREADY_EXISTS) {
        if (mtx) CloseHandle(mtx);
        return 0;
    }

    LoadSettings();
    if (g_s.autostart) ApplyAutostart();

    WNDCLASSW wc = { 0 };
    wc.lpfnWndProc   = WndProc;
    wc.hInstance     = hInst;
    wc.lpszClassName = L"MagicMouseTrayWnd";
    RegisterClassW(&wc);

    g_hwnd = CreateWindowW(L"MagicMouseTrayWnd", L"MagicMouse",
        0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInst, NULL);

    g_nid.cbSize           = sizeof(g_nid);
    g_nid.hWnd             = g_hwnd;
    g_nid.uID              = ID_TRAY;
    g_nid.uFlags           = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon            = CreateTrayIcon();
    StringCchCopyW(g_nid.szTip, _countof(g_nid.szTip), L"Magic Mouse");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    // Try to connect immediately. If it fails and an Apple device is present,
    // offer to install the embedded driver.
    if (!StartReader()) {
        if (CountAppleDevices() > 0) {
            g_offeredInstall = TRUE;
            OfferDriverInstall(FALSE);
        }
        SetTimer(g_hwnd, 1, 3000, NULL);
    }
    UpdateTrayTip();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (mtx) { ReleaseMutex(mtx); CloseHandle(mtx); }
    return 0;
}
