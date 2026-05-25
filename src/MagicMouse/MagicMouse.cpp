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

// Exact hardware IDs listed in MagicMouse.inf [MagicUtilities.NTamd64].
// We feed every entry to UpdateDriverForPlugAndPlayDevicesW, which finds
// matching devices and atomically switches their driver to MagicMouse.sys
// without disturbing any other Bluetooth/USB device.
static const wchar_t* kMouseHwids[] = {
    L"BTHENUM\\{00001124-0000-1000-8000-00805f9b34fb}_VID&000205ac_PID&030d", // Magic Mouse 2009 BT
    L"BTHENUM\\{00001124-0000-1000-8000-00805f9b34fb}_VID&000205ac_PID&0310", // Magic Mouse 2009 BT (alt)
    L"BTHENUM\\{00001124-0000-1000-8000-00805f9b34fb}_VID&0001004c_PID&0269", // Magic Mouse 2 (2015) BT
    L"USB\\Vid_05ac&Pid_0269&MI_01",                                          // Magic Mouse 2 (2015) USB
    L"BTHENUM\\{00001124-0000-1000-8000-00805f9b34fb}_VID&0001004c_PID&0323", // Magic Mouse 3 (2024) BT
    L"USB\\Vid_05ac&Pid_0323&MI_01",                                          // Magic Mouse 3 (2024) USB
    NULL
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

// Returns TRUE if at least one currently-present device matches a hardware
// ID listed in our INF (i.e. there is a Magic Mouse Windows might bind to).
// We consult the device's full HardwareIDs list and require an exact match,
// so e.g. a Magic Keyboard or AirPods will NEVER count as "a magic mouse".
static BOOL IsAnyMagicMousePresent(void)
{
    HDEVINFO ds = SetupDiGetClassDevsW(NULL, NULL, NULL,
        DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (ds == INVALID_HANDLE_VALUE) return FALSE;

    BOOL found = FALSE;
    SP_DEVINFO_DATA d = { sizeof(d) };
    BYTE buf[4096];
    for (DWORD i = 0; SetupDiEnumDeviceInfo(ds, i, &d) && !found; i++) {
        DWORD got = 0;
        if (!SetupDiGetDeviceRegistryPropertyW(ds, &d, SPDRP_HARDWAREID, NULL,
                buf, sizeof(buf), &got) || got < 4)
            continue;

        // SPDRP_HARDWAREID returns a REG_MULTI_SZ - walk each NUL-terminated
        // string and compare against our whitelist.
        const wchar_t* p = (const wchar_t*)buf;
        DWORD remain = got / sizeof(wchar_t);
        while (remain > 0 && *p) {
            size_t len = wcslen(p);
            for (int k = 0; kMouseHwids[k] && !found; k++) {
                if (_wcsicmp(p, kMouseHwids[k]) == 0) { found = TRUE; break; }
            }
            if (len + 1 > remain) break;
            p      += len + 1;
            remain -= (DWORD)(len + 1);
        }
    }
    SetupDiDestroyDeviceInfoList(ds);
    return found;
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

// ELEVATED entry point.
//
// Steps:
//   1. Extract the three embedded driver files to %TEMP%.
//   2. `pnputil /add-driver MagicMouse.inf /install` - registers the INF in
//      the driver store so future device arrivals find it.
//   3. For every hardware ID listed in the INF, ask Windows to update the
//      driver of any *currently present* matching device to MagicMouse.sys
//      via UpdateDriverForPlugAndPlayDevicesW. INSTALLFLAG_FORCE makes it
//      override Windows' built-in HID driver even when Windows considered
//      that driver "already good enough".
//   4. Verify and report.
//
// IMPORTANT: We deliberately do NOT call `pnputil /remove-device` on
// anything. The previous version of this code did, and it could blow away
// other Bluetooth devices (Apple Magic Keyboard, AirPods etc) because
// detection was VID-based. UpdateDriverForPlugAndPlayDevicesW is precise:
// it touches exactly the PIDs we list, nothing else.
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

    wchar_t infPath[MAX_PATH];
    StringCchPrintfW(infPath, MAX_PATH, L"%s\\MagicMouse.inf", dir);

    // Step 1: register INF in the driver store.
    wchar_t cmd[1024];
    StringCchPrintfW(cmd, 1024,
        L"pnputil.exe /add-driver \"%s\" /install", infPath);
    DWORD rcAdd = RunWait(cmd);

    // Step 2: per-hwid driver swap. Each call only touches devices whose
    // hardware-ID list contains an exact match, so e.g. an Apple Magic
    // Keyboard (different PID) will be left completely alone.
    int updated = 0;
    int triedHwid = 0;
    BOOL anyReboot = FALSE;
    for (int i = 0; kMouseHwids[i]; i++) {
        triedHwid++;
        BOOL reboot = FALSE;
        if (UpdateDriverForPlugAndPlayDevicesW(NULL, kMouseHwids[i], infPath,
                INSTALLFLAG_FORCE | INSTALLFLAG_NONINTERACTIVE, &reboot)) {
            updated++;
            if (reboot) anyReboot = TRUE;
        }
        // ERROR_NO_SUCH_DEVINST etc. just means "no matching device present
        // right now"; that's fine - we just continue.
    }

    Sleep(800);

    // Step 3: verify.
    int bound = 0;
    HANDLE check = OpenMagicMouse(&bound);
    if (check) CloseHandle(check);
    BOOL serviceOk = IsServiceInstalled(L"MagicMouse");

    DeleteFolderRecursive(dir);

    if (bound > 0) {
        MessageBoxW(NULL,
            anyReboot
              ? L"Driver installed and bound to your Magic Mouse.\r\n"
                L"Windows is asking for a reboot to finalize - reboot when\r\n"
                L"convenient. Scrolling will work after this dialog closes."
              : L"Driver installed and bound to your Magic Mouse.\r\n"
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
            L"Mouse driver (e.g. the official Magic Utilities) is installed.",
            rcAdd);
        MessageBoxW(NULL, msg, L"Magic Mouse - Install error",
            MB_OK | MB_ICONERROR);
        return 2;
    }

    // Driver is in the store but UpdateDriverForPlugAndPlayDevicesW didn't
    // match anything (or matched but PnP hasn't finished re-binding yet).
    wchar_t msg[1024];
    StringCchPrintfW(msg, _countof(msg),
        L"The Magic Mouse driver is registered, but no Magic Mouse is\r\n"
        L"currently bound to it (devices updated: %d / %d hardware IDs).\r\n\r\n"
        L"What to try:\r\n\r\n"
        L"  1. Make sure the mouse is paired and turned on, then click\r\n"
        L"     \"Reconnect\" in the tray menu.\r\n"
        L"  2. If that doesn't help, open Settings -> Bluetooth & devices,\r\n"
        L"     remove the Magic Mouse, then pair it again. The new driver\r\n"
        L"     will bind on first connection.\r\n"
        L"  3. As a last resort, reboot Windows.\r\n\r\n"
        L"Other Bluetooth devices (keyboard, headphones, ...) are NOT\r\n"
        L"affected - this installer only touches the Magic Mouse PIDs.",
        updated, triedHwid);
    MessageBoxW(NULL, msg,
        L"Magic Mouse - Driver registered, no mouse bound",
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

    BOOL serviceOk  = IsServiceInstalled(L"MagicMouse");
    BOOL mousePresent = IsAnyMagicMousePresent();

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
    } else if (!mousePresent) {
        // Driver installed but no compatible Magic Mouse is paired.
        if (!forceAsk) return;
        msg = L"The driver is already installed, but no Magic Mouse is\r\n"
              L"currently paired with this PC. Pair the mouse via\r\n"
              L"Settings -> Bluetooth & devices, then click \"Reconnect\".\r\n\r\n"
              L"Run the installer anyway?";
    } else {
        // The mouse IS visible to Windows, but bound to a different driver
        // (typically Microsoft's built-in HID stack).
        msg = L"Your Magic Mouse is detected, but it's currently using\r\n"
              L"Windows' default driver instead of MagicMouse.sys, so\r\n"
              L"two-finger scrolling doesn't work yet.\r\n\r\n"
              L"Switch to the bundled driver now?\r\n"
              L"Windows will ask for administrator approval once.\r\n\r\n"
              L"(Only your Magic Mouse will be touched - Bluetooth keyboard,\r\n"
              L"AirPods and other devices are not affected.)";
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
