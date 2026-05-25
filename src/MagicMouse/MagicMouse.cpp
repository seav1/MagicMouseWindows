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

// Run a console process synchronously, hidden, return exit code only.
// We deliberately do NOT capture stdout: modern pnputil emits UTF-16 LE
// with a BOM on some locales while reverting to OEM/ACP on others, which
// makes pretty-printing a moving target. The exit code is enough.
static DWORD RunWait(const wchar_t* cmdline)
{
    STARTUPINFOW si = { sizeof(si) };
    si.dwFlags     = STARTF_USESHOWWINDOW;
    si.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION pi = { 0 };

    wchar_t mut[1024];
    StringCchCopyW(mut, 1024, cmdline);
    if (!CreateProcessW(NULL, mut, NULL, NULL, FALSE,
            CREATE_NO_WINDOW, NULL, NULL, &si, &pi)) {
        return (DWORD)-1;
    }
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return exitCode;
}

// Returns TRUE if the named kernel service is registered with the SCM.
// We use this to distinguish "INF was never installed" (->offer install)
// from "INF is in driver store but mouse isn't connected" (->just wait).
static BOOL IsServiceInstalled(const wchar_t* name)
{
    SC_HANDLE scm = OpenSCManagerW(NULL, NULL, SC_MANAGER_CONNECT);
    if (!scm) return FALSE;
    SC_HANDLE svc = OpenServiceW(scm, name, SERVICE_QUERY_STATUS);
    BOOL found = (svc != NULL);
    if (svc) CloseServiceHandle(svc);
    CloseServiceHandle(scm);
    return found;
}

// ELEVATED entry point: extract embedded driver, add to store, force PnP
// to re-evaluate any Apple devices already enumerated, then verify.
static int RunDriverInstallElevated(void)
{
    wchar_t dir[MAX_PATH];
    if (!ExtractDriversToTemp(dir, MAX_PATH)) {
        MessageBoxW(NULL,
            L"Failed to extract the embedded driver files to %TEMP%.\r\n"
            L"Free some disk space and try again.",
            L"Magic Mouse - Install error", MB_OK | MB_ICONERROR);
        return 1;
    }

    // 1) pnputil /add-driver <inf> /install  - registers the INF and binds
    //    it to any matching device that is currently UN-driven.
    wchar_t cmd[1024];
    StringCchPrintfW(cmd, 1024,
        L"pnputil.exe /add-driver \"%s\\MagicMouse.inf\" /install", dir);
    DWORD rcAdd = RunWait(cmd);

    // 2) Force-rebind: for every Apple-VID device currently bound to a
    //    different driver, remove the instance so PnP re-picks one.
    int removed = 0;
    HDEVINFO ds = SetupDiGetClassDevsW(NULL, NULL, NULL,
        DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (ds != INVALID_HANDLE_VALUE) {
        SP_DEVINFO_DATA d = { sizeof(d) };
        BYTE hwbuf[2048];
        for (DWORD i = 0; SetupDiEnumDeviceInfo(ds, i, &d); i++) {
            DWORD got = 0;
            if (!SetupDiGetDeviceRegistryPropertyW(ds, &d, SPDRP_HARDWAREID,
                    NULL, hwbuf, sizeof(hwbuf), &got) || got < 4)
                continue;
            if (!HwidLooksApple((wchar_t*)hwbuf, got / sizeof(wchar_t)))
                continue;

            wchar_t instId[256] = { 0 };
            if (!SetupDiGetDeviceInstanceIdW(ds, &d, instId, _countof(instId), NULL))
                continue;

            StringCchPrintfW(cmd, 1024,
                L"pnputil.exe /remove-device \"%s\"", instId);
            if (RunWait(cmd) == 0) removed++;
        }
        SetupDiDestroyDeviceInfoList(ds);
    }

    // 3) Bus rescan: resurrect the just-removed instances under the new INF.
    RunWait(L"pnputil.exe /scan-devices");
    Sleep(2000);

    // Verify
    int bound = 0;
    HANDLE check = OpenMagicMouse(&bound);
    if (check) CloseHandle(check);
    BOOL serviceOk = IsServiceInstalled(L"MagicMouse");

    DeleteFolderRecursive(dir);

    if (bound > 0) {
        MessageBoxW(NULL,
            L"Driver installed and bound to your Magic Mouse.\r\n"
            L"Close this dialog; the tray app will resume automatically.",
            L"Magic Mouse - Install OK", MB_OK | MB_ICONINFORMATION);
        return 0;
    }

    if (!serviceOk) {
        wchar_t msg[512];
        StringCchPrintfW(msg, _countof(msg),
            L"Failed to register the driver with Windows.\r\n\r\n"
            L"pnputil exit code: %lu\r\n\r\n"
            L"Make sure you accepted the UAC prompt and that no other Magic\r\n"
            L"Mouse driver (e.g. Magic Utilities) is currently installed.",
            rcAdd);
        MessageBoxW(NULL, msg, L"Magic Mouse - Install error",
            MB_OK | MB_ICONERROR);
        return 2;
    }

    // Driver is in the store but no device is currently bound to it.
    // The most common cause is a paired-but-bound-to-Microsoft-HID Bluetooth
    // mouse: PnP won't auto-switch drivers for an already-known BT device
    // until the device is unpaired and re-paired.
    wchar_t msg[1024];
    StringCchPrintfW(msg, _countof(msg),
        L"The Magic Mouse driver is now installed in Windows, but no\r\n"
        L"Magic Mouse is bound to it yet (instances re-scanned: %d).\r\n\r\n"
        L"How to finish:\r\n\r\n"
        L"   1.  Open  Settings -> Bluetooth & devices.\r\n"
        L"   2.  Find your Magic Mouse, click the [...] menu, choose\r\n"
        L"       \"Remove device\" / \"Forget\".\r\n"
        L"   3.  Click \"Add device\" -> \"Bluetooth\" and pair the mouse\r\n"
        L"       again.\r\n"
        L"   4.  Click \"Reconnect\" in the tray menu.\r\n\r\n"
        L"(If the mouse is plugged in via USB / USB-C cable, just unplug\r\n"
        L"and re-plug it instead. A reboot also works.)",
        removed);
    MessageBoxW(NULL, msg,
        L"Magic Mouse - Driver added, please re-pair the mouse",
        MB_OK | MB_ICONINFORMATION);
    return 0;
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
    if (h) CloseHandle(h);
    int  appleCount = CountAppleDevices();
    BOOL serviceOk  = IsServiceInstalled(L"MagicMouse");

    const wchar_t* msg = NULL;
    if (boundCount > 0) {
        if (!forceAsk) return;
        msg = L"The driver is installed and your Magic Mouse is bound to it.\r\n"
              L"Reinstall it anyway?";
    } else if (!serviceOk) {
        // Real first run - INF was never registered.
        msg = L"The Magic Mouse driver isn't installed on this PC yet.\r\n\r\n"
              L"Install the bundled driver now?\r\n"
              L"Windows will ask for administrator approval once.";
    } else if (appleCount == 0) {
        // Driver is in the store but no Apple device is paired/connected.
        // Don't nag on every launch - only ask if the user explicitly chose
        // \"Reinstall driver...\".
        if (!forceAsk) return;
        msg = L"The driver is already installed, but no Magic Mouse is\r\n"
              L"currently paired/connected. Pair the mouse first, then click\r\n"
              L"\"Reconnect\" in the tray menu.\r\n\r\n"
              L"Run the installer anyway?";
    } else {
        // Driver in store, Apple device present, but not bound. Common case
        // when Windows preferred its built-in HID stack.
        msg = L"The driver is installed but not bound to your Magic Mouse.\r\n\r\n"
              L"Try to fix it now?\r\n"
              L"Windows will ask for administrator approval once.";
    }

    int r = MessageBoxW(NULL, msg, L"Magic Mouse - Driver setup",
        MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON1);
    if (r != IDYES) return;

    if (IsRunningAsAdmin()) RunDriverInstallElevated();
    else                    RelaunchInstallerElevated();

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
                // Don't pop install prompts from the timer - if the user
                // dismissed it once we won't keep asking. Use the
                // "Reinstall driver..." menu item to retry on demand.
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

    // Try to connect immediately. If we can't, only offer the install
    // dialog when the kernel service genuinely isn't registered (= true
    // first run). Otherwise just spin on the reconnect timer; the user
    // can still kick off "Reinstall driver..." manually from the tray.
    if (!StartReader()) {
        if (!IsServiceInstalled(L"MagicMouse")) {
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
