// ============================================================================
// MagicMouse.cpp - single-file Win32 implementation (no .NET, no MFC, no STL)
//
// What it does
//   1. Walks the PnP tree to find the device whose driver service is
//      "MagicMouse" (Magic Utilities' kernel driver), opens its raw PDO
//      via "\\.\GLOBALROOT\Device\<pdo-name>", and reads touch reports.
//   2. Translates X/Y deltas into standard mouse-wheel events using SendInput.
//   3. Lives in the system tray with a right-click menu for speed presets,
//      natural scroll, horizontal scroll, autostart and diagnostics.
//
// Build (from a Developer Command Prompt or via build.cmd):
//   cl /O2 /MT /EHsc /DUNICODE /D_UNICODE /D_WIN32_WINNT=0x0A00 \
//      MagicMouse.cpp MagicMouse.res \
//      /link /SUBSYSTEM:WINDOWS /OUT:MagicMouse.exe \
//      setupapi.lib user32.lib shell32.lib advapi32.lib gdi32.lib ole32.lib
//
// Resulting exe is ~150 KB, runs on Windows 8.1 / 10 / 11 (x64).
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
// IDs and messages
// ---------------------------------------------------------------------------
#define WM_TRAY        (WM_APP + 1)
#define WM_DEVICE_LOST (WM_APP + 2)

#define ID_TRAY            1
#define ID_M_QUIT          1001
#define ID_M_NATURAL       1002
#define ID_M_HORIZONTAL    1003
#define ID_M_RECONNECT     1004
#define ID_M_DIAG          1005
#define ID_M_AUTOSTART     1006
#define ID_M_DEVMGR        1007
#define ID_M_HELP_REBIND   1008
#define ID_M_SPEED_BASE    2000   // menu id = 2000 + speed_x10

static const int    kSpeedPresets[] = { 10, 20, 30, 50, 80, 100 };
static const wchar_t* kSpeedNames[] = {
    L"1.0x  (slow)",
    L"2.0x",
    L"3.0x  (default)",
    L"5.0x",
    L"8.0x",
    L"10.0x (fastest)"
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
static HINSTANCE        g_inst   = NULL;
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
// Device discovery
//
// The Magic Utilities driver creates a non-HID raw PDO. PnP assigns it a
// kernel name like "\Device\00000416", with the exact number being random
// per machine. We have to look it up by service name rather than hard-code.
// ===========================================================================
static const wchar_t* kServices[] = {
    L"MagicMouse",
    L"MagicMouseUSB",
    L"MagicMouseHID",
    NULL
};

static const wchar_t* kFallbackPaths[] = {
    L"\\\\.\\MagicMouse",
    L"\\\\.\\MagicMouseRawPDO",
    L"\\\\.\\MagicMouseUSB",
    NULL
};

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

// Helper: read a single SetupAPI property string.
static BOOL ReadProp(HDEVINFO ds, SP_DEVINFO_DATA* d, DWORD prop,
                    wchar_t* out, DWORD cb)
{
    out[0] = 0;
    return SetupDiGetDeviceRegistryPropertyW(ds, d, prop, NULL,
        (BYTE*)out, cb, NULL);
}

// Walks the PnP tree, finds devices whose driver service starts with
// "MagicMouse*", and (optionally) opens the first viable raw PDO.
//
// outBoundCount receives the number of MagicMouse-service devices found.
static HANDLE FindMagicMouseService(BOOL openIt,
                                    wchar_t* outDiag, size_t cchDiag,
                                    int* outBoundCount)
{
    if (outBoundCount) *outBoundCount = 0;

    HANDLE result = NULL;
    HDEVINFO ds = SetupDiGetClassDevsW(NULL, NULL, NULL,
        DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (ds == INVALID_HANDLE_VALUE) {
        if (outDiag) StringCchCatW(outDiag, cchDiag,
            L"  ! SetupDiGetClassDevs failed\r\n");
        return NULL;
    }

    SP_DEVINFO_DATA d = { sizeof(d) };
    int matches = 0;

    for (DWORD i = 0; SetupDiEnumDeviceInfo(ds, i, &d); i++) {
        wchar_t svc[64] = { 0 };
        if (!ReadProp(ds, &d, SPDRP_SERVICE, svc, sizeof(svc))) continue;

        BOOL isMagic = FALSE;
        for (int k = 0; kServices[k]; k++)
            if (_wcsicmp(svc, kServices[k]) == 0) { isMagic = TRUE; break; }
        if (!isMagic) continue;

        matches++;

        wchar_t pdo[260] = { 0 };
        BOOL hasPdo = ReadProp(ds, &d, SPDRP_PHYSICAL_DEVICE_OBJECT_NAME,
                                pdo, sizeof(pdo));

        if (outDiag) {
            wchar_t desc[200] = { 0 };
            if (!ReadProp(ds, &d, SPDRP_FRIENDLYNAME, desc, sizeof(desc)))
                ReadProp(ds, &d, SPDRP_DEVICEDESC, desc, sizeof(desc));

            wchar_t line[600];
            StringCchPrintfW(line, 600,
                L"  [%d] %s\r\n      service=%s\r\n      pdo=%s\r\n",
                matches,
                desc[0] ? desc : L"(no description)",
                svc,
                hasPdo ? pdo : L"(none)");
            StringCchCatW(outDiag, cchDiag, line);
        }

        if (!hasPdo) continue;

        wchar_t full[300];
        StringCchPrintfW(full, 300, L"\\\\.\\GLOBALROOT%s", pdo);

        if (openIt && !result) {
            result = TryOpenPath(full);
            if (outDiag) {
                StringCchCatW(outDiag, cchDiag,
                    result ? L"      open -> OK\r\n" : L"      open -> FAILED\r\n");
            }
            if (result)
                StringCchCopyW(g_lastOpenPath, MAX_PATH, full);
        }
    }
    SetupDiDestroyDeviceInfoList(ds);

    if (outBoundCount) *outBoundCount = matches;

    if (outDiag && matches == 0)
        StringCchCatW(outDiag, cchDiag,
            L"  (none found - the MagicMouse driver isn't bound to any device)\r\n");

    if (openIt && !result) {
        for (int i = 0; kFallbackPaths[i]; i++) {
            HANDLE h = TryOpenPath(kFallbackPaths[i]);
            if (h) {
                result = h;
                StringCchCopyW(g_lastOpenPath, MAX_PATH, kFallbackPaths[i]);
                if (outDiag) {
                    StringCchCatW(outDiag, cchDiag, L"  Fallback opened: ");
                    StringCchCatW(outDiag, cchDiag, kFallbackPaths[i]);
                    StringCchCatW(outDiag, cchDiag, L"\r\n");
                }
                break;
            }
        }
    }

    return result;
}

// Walks the PnP tree looking for ANY device whose hardware ID contains
// "VID_05AC" (Apple, Inc.). For each, prints description + currently
// bound driver service and manufacturer. This makes it obvious when
// MagicMouse.sys is installed but Windows still uses its built-in HID
// driver (e.g. service=HidUsb / mouhid) instead of MagicMouse.
//
// Returns: total Apple devices found via outFoundCount,
//          how many of them are bound to MagicMouse via outBoundCount.
static void EnumerateAppleDevices(wchar_t* outDiag, size_t cchDiag,
                                  int* outFoundCount, int* outBoundCount)
{
    if (outFoundCount) *outFoundCount = 0;
    if (outBoundCount) *outBoundCount = 0;

    HDEVINFO ds = SetupDiGetClassDevsW(NULL, NULL, NULL,
        DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (ds == INVALID_HANDLE_VALUE) return;

    SP_DEVINFO_DATA d = { sizeof(d) };
    int matches = 0;

    // SPDRP_HARDWAREID returns REG_MULTI_SZ - just check the buffer for the substring.
    BYTE  hwidBuf[2048];

    for (DWORD i = 0; SetupDiEnumDeviceInfo(ds, i, &d); i++) {
        DWORD got = 0;
        if (!SetupDiGetDeviceRegistryPropertyW(ds, &d, SPDRP_HARDWAREID, NULL,
                hwidBuf, sizeof(hwidBuf), &got) || got < 4)
            continue;

        // Treat as one wide string and look for "VID_05AC" anywhere.
        wchar_t* p = (wchar_t*)hwidBuf;
        DWORD count = got / sizeof(wchar_t);
        BOOL apple = FALSE;
        for (DWORD k = 0; k + 8 < count; k++) {
            if ((p[k]   == L'V' || p[k]   == L'v') &&
                (p[k+1] == L'I' || p[k+1] == L'i') &&
                (p[k+2] == L'D' || p[k+2] == L'd') &&
                 p[k+3] == L'_' &&
                 p[k+4] == L'0' && p[k+5] == L'5' &&
                (p[k+6] == L'A' || p[k+6] == L'a') &&
                 p[k+7] == L'C')
            { apple = TRUE; break; }
        }
        if (!apple) continue;

        matches++;

        // Hardware ID first string for display
        wchar_t firstHwid[256] = { 0 };
        StringCchCopyW(firstHwid, 256, (wchar_t*)hwidBuf);

        wchar_t desc[200] = { 0 };
        if (!ReadProp(ds, &d, SPDRP_FRIENDLYNAME, desc, sizeof(desc)))
            ReadProp(ds, &d, SPDRP_DEVICEDESC, desc, sizeof(desc));

        wchar_t svc[64] = L"(none)";
        ReadProp(ds, &d, SPDRP_SERVICE, svc, sizeof(svc));

        wchar_t mfg[128] = L"(unknown)";
        ReadProp(ds, &d, SPDRP_MFG, mfg, sizeof(mfg));

        BOOL boundToMagic = FALSE;
        for (int k = 0; kServices[k]; k++)
            if (_wcsicmp(svc, kServices[k]) == 0) { boundToMagic = TRUE; break; }
        if (boundToMagic && outBoundCount) (*outBoundCount)++;

        if (outDiag) {
            wchar_t line[800];
            // Truncate hardware ID for readability
            if (wcslen(firstHwid) > 80) firstHwid[80] = 0;
            StringCchPrintfW(line, 800,
                L"  [%d] %s%s\r\n"
                L"      hwid=%s%s\r\n"
                L"      service=%s%s\r\n"
                L"      manufacturer=%s\r\n",
                matches,
                desc[0] ? desc : L"(no description)",
                boundToMagic ? L"  [bound to MagicMouse]" : L"  [NOT bound to MagicMouse]",
                firstHwid, wcslen((wchar_t*)hwidBuf) > 80 ? L"..." : L"",
                svc,
                boundToMagic ? L"  <-- OK" : L"  <-- should be 'MagicMouse'!",
                mfg);
            StringCchCatW(outDiag, cchDiag, line);
        }
    }

    SetupDiDestroyDeviceInfoList(ds);

    if (outFoundCount) *outFoundCount = matches;

    if (outDiag && matches == 0)
        StringCchCatW(outDiag, cchDiag,
            L"  (no Apple-VID devices present - is the mouse paired and on?)\r\n");
}

// Backwards-compatible wrapper used by the runtime open path.
static HANDLE FindAndOpen(BOOL openIt, wchar_t* outDiag, size_t cchDiag)
{
    if (outDiag && cchDiag)
        StringCchCopyW(outDiag, cchDiag,
            L"=== Magic Mouse device discovery ===\r\n");
    int n = 0;
    return FindMagicMouseService(openIt, outDiag, cchDiag, &n);
}

// ===========================================================================
// Read loop and SendInput
// ===========================================================================
static void EmitWheel(int tickY, int tickX)
{
    INPUT in[2] = { 0 };
    int n = 0;
    if (tickY != 0) {
        in[n].type            = INPUT_MOUSE;
        in[n].mi.dwFlags      = MOUSEEVENTF_WHEEL;
        in[n].mi.mouseData    = (DWORD)(tickY * WHEEL_DELTA);
        n++;
    }
    if (tickX != 0 && g_s.horizontal) {
        in[n].type            = INPUT_MOUSE;
        in[n].mi.dwFlags      = MOUSEEVENTF_HWHEEL;
        in[n].mi.mouseData    = (DWORD)(tickX * WHEEL_DELTA);
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

        // speed_x10 is "tenths" (30 = 3.0x). Each WHEEL_DELTA (120) = one notch,
        // so threshold becomes 120 * 10 = 1200 when accumulating in tenths.
        g_accY += dy * g_s.speed_x10;
        g_accX += dx * g_s.speed_x10;

        const int kT = WHEEL_DELTA * 10;
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
    g_dev = FindAndOpen(TRUE, NULL, 0);
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
// Tray icon (drawn programmatically: white circle, blue dot)
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
            UINT32* px = &bits[(15 - y) * 16 + x];   // bottom-up DIB
            if (d2 <= 9)         *px = 0xFF0A84FFu;  // ARGB blue dot
            else if (d2 <= 49)   *px = 0xFFFFFFFFu;  // white body
            else                 *px = 0x00000000u;  // transparent
        }
    }

    HBITMAP mask = CreateBitmap(16, 16, 1, 1, NULL);

    ICONINFO ii = { 0 };
    ii.fIcon    = TRUE;
    ii.hbmMask  = mask;
    ii.hbmColor = hbm;
    HICON icon  = CreateIconIndirect(&ii);

    DeleteObject(hbm);
    DeleteObject(mask);
    return icon ? icon : LoadIconW(NULL, IDI_APPLICATION);
}

// ===========================================================================
// Tray context menu
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
    AppendMenuW(menu, MF_STRING, ID_M_RECONNECT,    L"Reconnect");
    AppendMenuW(menu, MF_STRING, ID_M_DIAG,         L"Diagnostics...");
    AppendMenuW(menu, MF_STRING, ID_M_DEVMGR,       L"Open Device Manager");
    AppendMenuW(menu, MF_STRING, ID_M_HELP_REBIND,  L"How to switch driver...");
    AppendMenuW(menu, MF_SEPARATOR, 0, NULL);
    AppendMenuW(menu, MF_STRING, ID_M_QUIT, L"Quit");

    SetForegroundWindow(g_hwnd);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON, pt.x, pt.y, 0, g_hwnd, NULL);
    DestroyMenu(menu);
}

static void ShowDiagnostics(void)
{
    static wchar_t diag[32768];
    StringCchCopyW(diag, _countof(diag),
        L"=== Magic Mouse - Diagnostics ===\r\n\r\n"
        L"[1] PnP devices using the MagicMouse driver service:\r\n");

    int boundCount = 0;
    FindMagicMouseService(FALSE, diag, _countof(diag), &boundCount);

    StringCchCatW(diag, _countof(diag),
        L"\r\n[2] PnP devices with Apple VID (0x05AC):\r\n");

    int appleFound = 0, appleBound = 0;
    EnumerateAppleDevices(diag, _countof(diag), &appleFound, &appleBound);

    wchar_t tail[1024];
    StringCchPrintfW(tail, 1024,
        L"\r\n[3] Current state: %s\r\n"
        L"    Opened path:   %s\r\n"
        L"    Speed: %d.%dx   Natural: %s   Horizontal: %s\r\n",
        g_dev ? L"CONNECTED" : L"NOT CONNECTED",
        g_lastOpenPath[0] ? g_lastOpenPath : L"(none)",
        g_s.speed_x10 / 10, g_s.speed_x10 % 10,
        g_s.natural    ? L"on" : L"off",
        g_s.horizontal ? L"on" : L"off");
    StringCchCatW(diag, _countof(diag), tail);

    // Smart conclusion
    StringCchCatW(diag, _countof(diag), L"\r\n--- DIAGNOSIS ---\r\n");
    if (g_dev) {
        StringCchCatW(diag, _countof(diag),
            L"App is connected to the MagicMouse driver. If scroll still doesn't\r\n"
            L"work, the driver may need a wake-up: try the Reconnect menu item, or\r\n"
            L"unpair and re-pair the mouse via Bluetooth.\r\n");
    }
    else if (boundCount == 0 && appleFound == 0) {
        StringCchCatW(diag, _countof(diag),
            L"No Apple device is currently present in PnP. Make sure the mouse\r\n"
            L"is powered ON and PAIRED via Bluetooth, then click 'Reconnect'.\r\n");
    }
    else if (boundCount == 0 && appleFound > 0) {
        StringCchCatW(diag, _countof(diag),
            L"** The MagicMouse driver is installed but NOT bound to your mouse. **\r\n"
            L"Windows is using its built-in HID driver (e.g. HidUsb / mouhid).\r\n"
            L"You must manually switch the driver:\r\n"
            L"\r\n"
            L"  1. Click 'Open Device Manager' in this app's tray menu.\r\n"
            L"  2. Expand 'Mice and other pointing devices' (and 'Bluetooth' if present).\r\n"
            L"  3. Right-click your Magic Mouse -> Update driver.\r\n"
            L"  4. 'Browse my computer for drivers'\r\n"
            L"  5. 'Let me pick from a list of available drivers on my computer'.\r\n"
            L"  6. UNCHECK 'Show compatible hardware'.\r\n"
            L"  7. Manufacturer: 'Magic Utilities' or similar - select 'MagicMouse'.\r\n"
            L"     If it isn't listed, click 'Have Disk...' and point to MagicMouse.inf.\r\n"
            L"  8. Click Next, accept any unsigned-driver warning.\r\n"
            L"  9. Come back here and click 'Reconnect'.\r\n");
    }
    else if (boundCount > 0 && !g_dev) {
        StringCchCatW(diag, _countof(diag),
            L"MagicMouse driver IS bound, but the raw PDO couldn't be opened.\r\n"
            L"Try clicking 'Reconnect'. If that still fails, restart the\r\n"
            L"MagicMouse service:\r\n"
            L"  PowerShell (Admin):  Restart-Service MagicMouse\r\n");
    }

    StringCchCatW(diag, _countof(diag),
        L"\r\nTip: press Ctrl+C inside this dialog to copy the full report.\r\n");

    MessageBoxW(NULL, diag, L"Magic Mouse - Diagnostics",
        MB_OK | MB_ICONINFORMATION);
}

static void OpenDeviceManager(void)
{
    // devmgmt.msc is an MMC console; ShellExecute lets the system handle it.
    ShellExecuteW(NULL, L"open", L"devmgmt.msc", NULL, NULL, SW_SHOWNORMAL);
}

static void ShowRebindHelp(void)
{
    MessageBoxW(NULL,
        L"How to bind the MagicMouse driver to your mouse:\r\n"
        L"\r\n"
        L"1. Open Device Manager (devmgmt.msc).\r\n"
        L"2. Expand 'Mice and other pointing devices'.\r\n"
        L"   (If your mouse appears under 'Bluetooth' or 'Human Interface\r\n"
        L"   Devices' instead, look there.)\r\n"
        L"3. Right-click your Magic Mouse -> Update driver.\r\n"
        L"4. Choose 'Browse my computer for drivers'.\r\n"
        L"5. Choose 'Let me pick from a list of available drivers on my\r\n"
        L"   computer'.\r\n"
        L"6. Untick 'Show compatible hardware' so all drivers are listed.\r\n"
        L"7. In the Manufacturer column find 'Magic Utilities' (or similar);\r\n"
        L"   in the Model column select 'MagicMouse'. If it isn't there,\r\n"
        L"   click 'Have Disk...' and browse to your MagicMouse.inf file.\r\n"
        L"8. Confirm Next, accept the unsigned-driver / digital-signature\r\n"
        L"   warning if Windows shows one.\r\n"
        L"9. The mouse may briefly disconnect; that's expected.\r\n"
        L"10. Back in this app's tray menu, click 'Reconnect'.\r\n"
        L"\r\n"
        L"You can also re-run the driver installer from PowerShell:\r\n"
        L"  pnputil /add-driver \"C:\\MagicMouseDriver\\MagicMouse.inf\" /install\r\n"
        L"...and then unpair + re-pair the mouse via Bluetooth so PnP\r\n"
        L"re-evaluates which driver to use.",
        L"Magic Mouse - How to switch driver",
        MB_OK | MB_ICONINFORMATION);
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
                case ID_M_DIAG:
                    ShowDiagnostics();
                    break;
                case ID_M_DEVMGR:
                    OpenDeviceManager();
                    break;
                case ID_M_HELP_REBIND:
                    ShowRebindHelp();
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
            SetTimer(hwnd, 1, 3000, NULL);   // try every 3s
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

// ===========================================================================
// Entry point
// ===========================================================================
int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE prev, LPWSTR cmdLine, int show)
{
    (void)prev; (void)cmdLine; (void)show;
    g_inst = hInst;

    // Single-instance guard
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
    StringCchCopyW(g_nid.szTip, _countof(g_nid.szTip), L"Magic Mouse - starting");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    if (!StartReader())
        SetTimer(g_hwnd, 1, 3000, NULL);
    UpdateTrayTip();

    MSG msg;
    while (GetMessageW(&msg, NULL, 0, 0)) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (mtx) { ReleaseMutex(mtx); CloseHandle(mtx); }
    return 0;
}
