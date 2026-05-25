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

#define WM_TRAY      (WM_APP + 1)
#define WM_RECONNECT (WM_APP + 2)

#define ID_TRAY         1
#define ID_M_QUIT       1001
#define ID_M_NATURAL    1002
#define ID_M_HORIZONTAL 1003
#define ID_M_RECONNECT  1004
#define ID_M_AUTOSTART  1005
#define ID_M_STATUS     1007
#define ID_M_SPEED_BASE 2000

#define MAX_CANDIDATES 64

typedef struct {
    int speed_x10;
    BOOL natural;
    BOOL horizontal;
    BOOL autostart;
} Settings;

typedef struct {
    wchar_t path[320];
    wchar_t hwid[256];
    wchar_t service[128];
    int score;
} Candidate;

typedef struct {
    Candidate items[MAX_CANDIDATES];
    int count;
} CandidateList;

static const int kSpeedPresets[] = { 10, 20, 30, 50, 80, 100 };
static const wchar_t* kSpeedNames[] = {
    L"1.0x  (slow)", L"2.0x", L"3.0x  (default)", L"5.0x", L"8.0x", L"10.0x (fastest)"
};
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
static HANDLE g_thread = NULL;
static HANDLE g_dev = NULL;
static volatile LONG g_running = 0;
static int g_curCandidate = -1;
static CandidateList g_candidates;

static int g_accX = 0;
static int g_accY = 0;
static volatile LONG g_reports = 0;
static volatile LONG g_emits = 0;
static volatile LONG g_timeouts = 0;
static volatile LONG g_lastErr = 0;
static wchar_t g_lastPath[320] = L"";
static wchar_t g_lastReport[128] = L"";
static int g_parserMode = 0; // 0=fixed, 1=adaptive
static int g_parserY = 1;
static int g_parserX = 2;

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
    g_s.speed_x10 = GetPrivateProfileIntW(L"main", L"speed_x10", 30, p);
    g_s.natural = (BOOL)GetPrivateProfileIntW(L"main", L"natural", 0, p);
    g_s.horizontal = (BOOL)GetPrivateProfileIntW(L"main", L"horizontal", 1, p);
    g_s.autostart = (BOOL)GetPrivateProfileIntW(L"main", L"autostart", 0, p);
    if (g_s.speed_x10 < 1) g_s.speed_x10 = 1;
    if (g_s.speed_x10 > 200) g_s.speed_x10 = 200;
}

static void SaveSettings(void)
{
    wchar_t p[MAX_PATH], v[16];
    GetIniPath(p, _countof(p));
    StringCchPrintfW(v, _countof(v), L"%d", g_s.speed_x10); WritePrivateProfileStringW(L"main", L"speed_x10", v, p);
    StringCchPrintfW(v, _countof(v), L"%d", g_s.natural); WritePrivateProfileStringW(L"main", L"natural", v, p);
    StringCchPrintfW(v, _countof(v), L"%d", g_s.horizontal); WritePrivateProfileStringW(L"main", L"horizontal", v, p);
    StringCchPrintfW(v, _countof(v), L"%d", g_s.autostart); WritePrivateProfileStringW(L"main", L"autostart", v, p);
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
        RegSetValueExW(k, L"MagicMouse", 0, REG_SZ, (const BYTE*)quoted,
            (DWORD)((wcslen(quoted) + 1) * sizeof(wchar_t)));
    } else {
        RegDeleteValueW(k, L"MagicMouse");
    }
    RegCloseKey(k);
}

static void CandidateListAdd(CandidateList* list, const wchar_t* path, const wchar_t* hwid, const wchar_t* service, int score)
{
    if (!list || !path || !path[0]) return;
    for (int i = 0; i < list->count; i++) {
        if (_wcsicmp(list->items[i].path, path) == 0) {
            if (score > list->items[i].score) list->items[i].score = score;
            if (hwid && hwid[0] && !list->items[i].hwid[0]) StringCchCopyW(list->items[i].hwid, _countof(list->items[i].hwid), hwid);
            if (service && service[0] && !list->items[i].service[0]) StringCchCopyW(list->items[i].service, _countof(list->items[i].service), service);
            return;
        }
    }
    if (list->count >= MAX_CANDIDATES) return;
    Candidate* c = &list->items[list->count++];
    ZeroMemory(c, sizeof(*c));
    StringCchCopyW(c->path, _countof(c->path), path);
    if (hwid && hwid[0]) StringCchCopyW(c->hwid, _countof(c->hwid), hwid);
    if (service && service[0]) StringCchCopyW(c->service, _countof(c->service), service);
    c->score = score;
}

static void CandidateListSort(CandidateList* list)
{
    for (int i = 0; i < list->count; i++) {
        for (int j = i + 1; j < list->count; j++) {
            if (list->items[j].score > list->items[i].score) {
                Candidate t = list->items[i];
                list->items[i] = list->items[j];
                list->items[j] = t;
            }
        }
    }
}

static void RebuildCandidates(CandidateList* out)
{
    ZeroMemory(out, sizeof(*out));
    HDEVINFO ds = SetupDiGetClassDevsW(NULL, NULL, NULL, DIGCF_PRESENT | DIGCF_ALLCLASSES);
    if (ds == INVALID_HANDLE_VALUE) return;

    SP_DEVINFO_DATA d = { sizeof(d) };
    BYTE hwbuf[4096];
    BYTE svcbuf[512];
    for (DWORD i = 0; SetupDiEnumDeviceInfo(ds, i, &d); i++) {
        wchar_t pdo[260] = L"";
        if (!SetupDiGetDeviceRegistryPropertyW(ds, &d, SPDRP_PHYSICAL_DEVICE_OBJECT_NAME, NULL,
                (BYTE*)pdo, sizeof(pdo), NULL) || !pdo[0]) {
            continue;
        }
        wchar_t path[320];
        StringCchPrintfW(path, _countof(path), L"\\\\.\\GLOBALROOT%s", pdo);

        wchar_t svc[128] = L"";
        DWORD got = 0;
        if (SetupDiGetDeviceRegistryPropertyW(ds, &d, SPDRP_SERVICE, NULL, svcbuf, sizeof(svcbuf), &got) &&
                got >= sizeof(wchar_t) * 2) {
            StringCchCopyW(svc, _countof(svc), (const wchar_t*)svcbuf);
        }

        int base = 0;
        if (_wcsicmp(svc, L"MagicMouse") == 0) base += 1000;
        if (ContainsI(svc, L"HidBth")) base += 60;
        if (ContainsI(svc, L"mouhid")) base -= 30;

        got = 0;
        if (SetupDiGetDeviceRegistryPropertyW(ds, &d, SPDRP_HARDWAREID, NULL, hwbuf, sizeof(hwbuf), &got) &&
                got >= sizeof(wchar_t) * 2) {
            const wchar_t* p = (const wchar_t*)hwbuf;
            DWORD remain = got / sizeof(wchar_t);
            while (remain > 0 && *p) {
                size_t len = wcslen(p);
                if (IsMagicMouseHwid(p)) {
                    int score = base + 200;
                    if (ContainsI(p, L"COL03")) score += 300;
                    else if (ContainsI(p, L"COL02")) score += 120;
                    else if (ContainsI(p, L"COL01")) score += 20;
                    CandidateListAdd(out, path, p, svc, score);
                }
                if (len + 1 > remain) break;
                p += len + 1;
                remain -= (DWORD)(len + 1);
            }
        } else if (_wcsicmp(svc, L"MagicMouse") == 0) {
            CandidateListAdd(out, path, L"", svc, base + 500);
        }
    }

    SetupDiDestroyDeviceInfoList(ds);
    CandidateListSort(out);
}

static HANDLE TryOpenPath(const wchar_t* path, DWORD* outErr)
{
    if (outErr) *outErr = ERROR_SUCCESS;
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
        NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e1 = GetLastError();
        h = CreateFileW(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE,
            NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (h == INVALID_HANDLE_VALUE && outErr) {
            DWORD e2 = GetLastError();
            *outErr = e2 ? e2 : e1;
        }
    }
    return (h == INVALID_HANDLE_VALUE) ? NULL : h;
}

static BOOL OpenNextCandidate(int startIndex)
{
    if (g_dev) { CloseHandle(g_dev); g_dev = NULL; }
    if (g_candidates.count <= 0) return FALSE;
    if (startIndex < 0 || startIndex >= g_candidates.count) startIndex = 0;

    for (int off = 0; off < g_candidates.count; off++) {
        int i = (startIndex + off) % g_candidates.count;
        DWORD err = 0;
        HANDLE h = TryOpenPath(g_candidates.items[i].path, &err);
        if (h) {
            g_dev = h;
            g_curCandidate = i;
            StringCchCopyW(g_lastPath, _countof(g_lastPath), g_candidates.items[i].path);
            return TRUE;
        }
        InterlockedExchange(&g_lastErr, (LONG)err);
    }
    g_curCandidate = -1;
    g_lastPath[0] = 0;
    return FALSE;
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

static void UpdateReportPreview(const BYTE* buf, DWORD read)
{
    wchar_t line[128] = L"";
    int lim = read > 8 ? 8 : (int)read;
    for (int i = 0; i < lim; i++) {
        wchar_t b[8];
        StringCchPrintfW(b, _countof(b), L"%02X ", (unsigned)buf[i]);
        StringCchCatW(line, _countof(line), b);
    }
    StringCchCopyW(g_lastReport, _countof(g_lastReport), line);
}

static DWORD WINAPI ReadThread(LPVOID arg)
{
    (void)arg;
    BYTE buf[64], prev[64] = { 0 };
    BOOL hasPrev = FALSE;
    OVERLAPPED ov = { 0 };
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return 0;

    int idleLoops = 0;
    while (g_running) {
        if (!g_dev) {
            if (!OpenNextCandidate(g_curCandidate + 1)) {
                Sleep(500);
                continue;
            }
            idleLoops = 0;
            hasPrev = FALSE;
        }

        ResetEvent(ov.hEvent);
        DWORD read = 0;
        BOOL ok = ReadFile(g_dev, buf, sizeof(buf), &read, &ov);
        if (!ok) {
            DWORD e = GetLastError();
            if (e == ERROR_IO_PENDING) {
                DWORD wr = WaitForSingleObject(ov.hEvent, 250);
                if (wr == WAIT_TIMEOUT) {
                    InterlockedIncrement(&g_timeouts);
                    CancelIoEx(g_dev, &ov);
                    idleLoops++;
                    if (idleLoops > 32) { // ~8s no data
                        CloseHandle(g_dev);
                        g_dev = NULL;
                    }
                    continue;
                }
                if (wr != WAIT_OBJECT_0 || !GetOverlappedResult(g_dev, &ov, &read, FALSE)) {
                    InterlockedExchange(&g_lastErr, (LONG)GetLastError());
                    CloseHandle(g_dev);
                    g_dev = NULL;
                    continue;
                }
            } else {
                InterlockedExchange(&g_lastErr, (LONG)e);
                CloseHandle(g_dev);
                g_dev = NULL;
                continue;
            }
        }
        if (read < 3) continue;

        idleLoops = 0;
        InterlockedIncrement(&g_reports);
        UpdateReportPreview(buf, read);

        int dy = (signed char)buf[1];
        int dx = (signed char)buf[2];

        // Adaptive parser when fixed parser is quiet.
        if (dy == 0 && dx == 0) {
            if (!hasPrev) {
                for (int i = 0; i < 64; i++) prev[i] = buf[i];
                hasPrev = TRUE;
                continue;
            }
            int bestA = -1, bestB = -1, magA = 0, magB = 0;
            int lim = read > 16 ? 16 : (int)read;
            for (int i = 1; i < lim; i++) {
                int d = (int)(signed char)buf[i] - (int)(signed char)prev[i];
                int ad = d < 0 ? -d : d;
                if (ad > magA) { magB = magA; bestB = bestA; magA = ad; bestA = i; }
                else if (ad > magB) { magB = ad; bestB = i; }
                prev[i] = buf[i];
            }
            if (bestA > 0 && bestB > 0) {
                g_parserMode = 1;
                g_parserY = bestA;
                g_parserX = bestB;
                dy = (int)(signed char)buf[bestA] - (int)(signed char)prev[bestA];
                dx = (int)(signed char)buf[bestB] - (int)(signed char)prev[bestB];
            } else {
                g_parserMode = 0;
                g_parserY = 1;
                g_parserX = 2;
            }
        } else {
            g_parserMode = 0;
            g_parserY = 1;
            g_parserX = 2;
        }

        if (g_s.natural) { dy = -dy; dx = -dx; }
        g_accY += dy * g_s.speed_x10;
        g_accX += dx * g_s.speed_x10;

        const int step = WHEEL_DELTA * 10;
        int ty = g_accY / step;
        int tx = g_accX / step;
        if (ty) g_accY -= ty * step;
        if (tx) g_accX -= tx * step;
        if (ty || tx) {
            EmitWheel(ty, tx);
            InterlockedIncrement(&g_emits);
        }
    }

    if (g_dev) { CloseHandle(g_dev); g_dev = NULL; }
    CloseHandle(ov.hEvent);
    return 0;
}

static void StopReader(void)
{
    g_running = 0;
    if (g_thread) {
        WaitForSingleObject(g_thread, 2000);
        CloseHandle(g_thread);
        g_thread = NULL;
    }
    if (g_dev) {
        CloseHandle(g_dev);
        g_dev = NULL;
    }
    g_curCandidate = -1;
    g_accX = g_accY = 0;
    g_reports = g_emits = g_timeouts = 0;
    g_lastErr = 0;
    g_lastPath[0] = 0;
    g_lastReport[0] = 0;
    g_parserMode = 0;
    g_parserY = 1;
    g_parserX = 2;
}

static BOOL StartReader(void)
{
    StopReader();
    RebuildCandidates(&g_candidates);
    g_running = 1;
    g_thread = CreateThread(NULL, 0, ReadThread, NULL, 0, NULL);
    return (g_thread != NULL);
}

static void UpdateTrayTip(void)
{
    const wchar_t* tip = L"Magic Mouse - not found";
    if (g_dev) tip = L"Magic Mouse - connected";
    else if (g_candidates.count > 0) tip = L"Magic Mouse - detected, switching nodes";
    StringCchCopyW(g_nid.szTip, _countof(g_nid.szTip), tip);
    g_nid.uFlags |= NIF_TIP;
    Shell_NotifyIconW(NIM_MODIFY, &g_nid);
}

static void ShowStatus(void)
{
    RebuildCandidates(&g_candidates);
    wchar_t top[2600] = L"";
    int lim = g_candidates.count > 6 ? 6 : g_candidates.count;
    for (int i = 0; i < lim; i++) {
        wchar_t line[420];
        DWORD err = 0;
        HANDLE h = TryOpenPath(g_candidates.items[i].path, &err);
        if (h) CloseHandle(h);
        StringCchPrintfW(line, _countof(line),
            L"[%d] score=%d svc=%s err=%lu hwid=%s\r\n",
            i, g_candidates.items[i].score,
            g_candidates.items[i].service[0] ? g_candidates.items[i].service : L"(none)",
            err,
            g_candidates.items[i].hwid[0] ? g_candidates.items[i].hwid : L"(none)");
        StringCchCatW(top, _countof(top), line);
    }

    wchar_t msg[4096];
    StringCchPrintfW(msg, _countof(msg),
        L"Candidates: %d\r\nCurrent index: %d\r\nCurrent path: %s\r\n"
        L"Parser: %s (y=%d, x=%d)\r\n\r\n"
        L"Reports: %ld  Emits: %ld  Timeouts: %ld\r\n"
        L"LastErr: %ld\r\nLastReport[0..7]: %s\r\n\r\n"
        L"Top candidates:\r\n%s",
        g_candidates.count, g_curCandidate,
        g_lastPath[0] ? g_lastPath : L"(none)",
        g_parserMode ? L"adaptive" : L"fixed", g_parserY, g_parserX,
        g_reports, g_emits, g_timeouts, g_lastErr,
        g_lastReport[0] ? g_lastReport : L"(none)",
        top[0] ? top : L"(none)");
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

    const wchar_t* title = g_dev ? L"Magic Mouse [connected]" :
        (g_candidates.count > 0 ? L"Magic Mouse [detected]" : L"Magic Mouse [not found]");
    AppendMenuW(m, MF_STRING | MF_DISABLED, 0, title);
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_POPUP, (UINT_PTR)speed, L"Scroll speed");
    AppendMenuW(m, MF_STRING | (g_s.natural ? MF_CHECKED : 0), ID_M_NATURAL, L"Natural scroll");
    AppendMenuW(m, MF_STRING | (g_s.horizontal ? MF_CHECKED : 0), ID_M_HORIZONTAL, L"Horizontal scroll");
    AppendMenuW(m, MF_STRING | (g_s.autostart ? MF_CHECKED : 0), ID_M_AUTOSTART, L"Start with Windows");
    AppendMenuW(m, MF_SEPARATOR, 0, NULL);
    AppendMenuW(m, MF_STRING, ID_M_RECONNECT, L"Reconnect");
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
        if (LOWORD(lp) == WM_RBUTTONUP || LOWORD(lp) == WM_LBUTTONUP) ShowTrayMenu();
        return 0;
    case WM_RECONNECT:
        StartReader();
        UpdateTrayTip();
        return 0;
    case WM_COMMAND: {
        WORD id = LOWORD(wp);
        if (id >= ID_M_SPEED_BASE && id < ID_M_SPEED_BASE + 1000) {
            g_s.speed_x10 = id - ID_M_SPEED_BASE;
            SaveSettings();
            return 0;
        }
        switch (id) {
        case ID_M_NATURAL: g_s.natural = !g_s.natural; SaveSettings(); break;
        case ID_M_HORIZONTAL: g_s.horizontal = !g_s.horizontal; SaveSettings(); break;
        case ID_M_AUTOSTART: g_s.autostart = !g_s.autostart; SaveSettings(); ApplyAutostart(); break;
        case ID_M_RECONNECT: PostMessageW(hwnd, WM_RECONNECT, 0, 0); break;
        case ID_M_STATUS: ShowStatus(); break;
        case ID_M_QUIT:
            StopReader();
            Shell_NotifyIconW(NIM_DELETE, &g_nid);
            PostQuitMessage(0);
            break;
        }
        UpdateTrayTip();
        return 0;
    }
    case WM_DESTROY:
        StopReader();
        Shell_NotifyIconW(NIM_DELETE, &g_nid);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE prev, LPWSTR cmd, int show)
{
    (void)prev;
    (void)cmd;
    (void)show;

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

    g_hwnd = CreateWindowW(L"MagicMouseTrayWnd", L"MagicMouse",
        0, 0, 0, 0, 0, HWND_MESSAGE, NULL, hInst, NULL);

    g_nid.cbSize = sizeof(g_nid);
    g_nid.hWnd = g_hwnd;
    g_nid.uID = ID_TRAY;
    g_nid.uFlags = NIF_ICON | NIF_MESSAGE | NIF_TIP;
    g_nid.uCallbackMessage = WM_TRAY;
    g_nid.hIcon = CreateTrayIcon();
    StringCchCopyW(g_nid.szTip, _countof(g_nid.szTip), L"Magic Mouse");
    Shell_NotifyIconW(NIM_ADD, &g_nid);

    StartReader();
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

