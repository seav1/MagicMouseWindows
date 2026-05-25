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

#define ID_TRAY            1
#define ID_M_QUIT          1001
#define ID_M_NATURAL       1002
#define ID_M_HORIZONTAL    1003
#define ID_M_RECONNECT     1004
#define ID_M_AUTOSTART     1005
#define ID_M_STATUS        1007
#define ID_M_SPEED_BASE    2000

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
                    if (idleLoops > 32) { // ~8s no data on this node
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

        // Adaptive fallback if fixed parser seems dead.
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
    g_reports = 0;
    g_emits = 0;
    g_timeouts = 0;
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
        wchar_t ln[420];
        DWORD err = 0;
        HANDLE h = TryOpenPath(g_candidates.items[i].path, &err);
        if (h) CloseHandle(h);
        StringCchPrintfW(ln, _countof(ln),
            L"[%d] score=%d svc=%s err=%lu hwid=%s\r\n",
            i, g_candidates.items[i].score,
            g_candidates.items[i].service[0] ? g_candidates.items[i].service : L"(none)",
            err,
            g_candidates.items[i].hwid[0] ? g_candidates.items[i].hwid : L"(none)");
        StringCchCatW(top, _countof(top), ln);
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
        if (LOWORD(lp) == WM_LBUTTONUP || LOWORD(lp) == WM_RBUTTONUP) ShowTrayMenu();
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
static wchar_t g_lastOpenDetail[4096] = L"";
static wchar_t g_excludedPath[MAX_PATH] = L"";
static volatile LONG g_reportCount = 0;
static volatile LONG g_emitCount = 0;
static volatile LONG g_idleTimeouts = 0;
static volatile LONG g_readFailCount = 0;
static volatile LONG g_lastReadErr = 0;
static int g_parserMode = 0; // 0=fixed [1,2], 1=adaptive delta
static int g_parserY = 1;
static int g_parserX = 2;
static wchar_t g_lastReportHex[128] = L"";

static BOOL IsServiceInstalled(const wchar_t* name);
static AppState QueryState(int* outCandidates, int* outBound);
static HANDLE TryOpenPath(const wchar_t* path, DWORD* outErr);

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
        DWORD err = 0;
        HANDLE h = TryOpenPath(full, &err);
        if (h) {
            StringCchCopyW(g_lastOpenPath, _countof(g_lastOpenPath), full);
            StringCchPrintfW(g_lastOpenDetail, _countof(g_lastOpenDetail),
                L"service open ok: %s", full);
            opened = h;
        } else if (!g_lastOpenDetail[0]) {
            StringCchPrintfW(g_lastOpenDetail, _countof(g_lastOpenDetail),
                L"service open failed: %s (err=%lu)", full, err);
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

static HANDLE TryOpenPath(const wchar_t* path, DWORD* outErr)
{
    if (outErr) *outErr = ERROR_SUCCESS;
    HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
        FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD e1 = GetLastError();
        h = CreateFileW(path, GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, NULL, OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
        if (h == INVALID_HANDLE_VALUE && outErr) {
            DWORD e2 = GetLastError();
            *outErr = e2 ? e2 : e1;
        }
    }
    if (h != INVALID_HANDLE_VALUE && outErr) *outErr = ERROR_SUCCESS;
    return (h == INVALID_HANDLE_VALUE) ? NULL : h;
}

static HANDLE OpenMagicMouse(int* outBoundCount, int* outCandidateCount)
{
    if (outBoundCount) *outBoundCount = 0;
    if (outCandidateCount) *outCandidateCount = 0;
    g_lastOpenPath[0] = 0;
    g_lastOpenDetail[0] = 0;

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
        if (g_excludedPath[0] && _wcsicmp(arr[i].pdoPath, g_excludedPath) == 0) continue;
        DWORD err = 0;
        HANDLE h = TryOpenPath(arr[i].pdoPath, &err);
        if (h) {
            StringCchCopyW(g_lastOpenPath, _countof(g_lastOpenPath), arr[i].pdoPath);
            StringCchPrintfW(g_lastOpenDetail, _countof(g_lastOpenDetail),
                L"candidate[%d] open ok: %s", i, arr[i].pdoPath);
            return h;
        } else if (!g_lastOpenDetail[0]) {
            StringCchPrintfW(g_lastOpenDetail, _countof(g_lastOpenDetail),
                L"candidate[%d] open failed: %s (err=%lu)", i, arr[i].pdoPath, err);
        }
    }

    // Pass 2: some systems expose the usable report stream on a HID collection
    // node (often Col03) that is not marked "bound" by our service/filter check.
    // Pick the best openable candidate by score instead of giving up.
    int bestIdx = -1;
    int bestScore = -1;
    HANDLE bestHandle = NULL;
    DWORD bestErr = ERROR_GEN_FAILURE;
    for (int i = 0; i < n; i++) {
        if (!arr[i].hasPdo) continue;
        if (g_excludedPath[0] && _wcsicmp(arr[i].pdoPath, g_excludedPath) == 0) continue;

        DWORD err = 0;
        HANDLE h = TryOpenPath(arr[i].pdoPath, &err);
        if (!h) {
            bestErr = err;
            continue;
        }

        int score = 0;
        if (arr[i].isBound) score += 20;
        if (ContainsI(arr[i].sampleHwid, L"COL03")) score += 100;  // most likely touch collection
        if (ContainsI(arr[i].sampleHwid, L"COL02")) score += 40;
        if (ContainsI(arr[i].sampleHwid, L"HID\\")) score += 10;
        if (ContainsI(arr[i].service, L"mouhid")) score -= 10;

        if (score > bestScore) {
            if (bestHandle) CloseHandle(bestHandle);
            bestHandle = h;
            bestScore = score;
            bestIdx = i;
        } else {
            CloseHandle(h);
        }
    }
    if (bestHandle) {
        StringCchCopyW(g_lastOpenPath, _countof(g_lastOpenPath), arr[bestIdx].pdoPath);
        StringCchPrintfW(g_lastOpenDetail, _countof(g_lastOpenDetail),
            L"fallback open ok: candidate[%d], score=%d, hwid=%s",
            bestIdx, bestScore,
            arr[bestIdx].sampleHwid[0] ? arr[bestIdx].sampleHwid : L"(none)");
        return bestHandle;
    }
    if (!g_lastOpenDetail[0]) {
        StringCchPrintfW(g_lastOpenDetail, _countof(g_lastOpenDetail),
            L"all candidate opens failed (last err=%lu)", bestErr);
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
    BYTE prevRaw[64] = { 0 };
    BYTE prevDelta[64] = { 0 };
    BOOL havePrevDelta = FALSE;
    int score[16] = { 0 };
    int zeroFixedStreak = 0;

    OVERLAPPED ov = { 0 };
    ov.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (!ov.hEvent) return 0;

    while (g_running && g_dev) {
        ResetEvent(ov.hEvent);
        read = 0;
        BOOL ok = ReadFile(g_dev, buf, sizeof(buf), &read, &ov);
        if (!ok) {
            DWORD e = GetLastError();
            if (e == ERROR_IO_PENDING) {
                DWORD wr = WaitForSingleObject(ov.hEvent, 350);
                if (wr == WAIT_TIMEOUT) {
                    InterlockedIncrement(&g_idleTimeouts);
                    CancelIoEx(g_dev, &ov);
                    // If this opened node never yields data, exclude it and reconnect.
                    if (g_reportCount == 0 && g_idleTimeouts > 25 && g_lastOpenPath[0]) {
                        StringCchCopyW(g_excludedPath, _countof(g_excludedPath), g_lastOpenPath);
                        StringCchPrintfW(g_lastOpenDetail, _countof(g_lastOpenDetail),
                            L"excluded stalled path: %s", g_excludedPath);
                        PostMessageW(g_hwnd, WM_DEVICE_LOST, 0, 0);
                        break;
                    }
                    continue;
                }
                if (wr != WAIT_OBJECT_0 || !GetOverlappedResult(g_dev, &ov, &read, FALSE)) {
                    e = GetLastError();
                    InterlockedExchange(&g_lastReadErr, (LONG)e);
                    InterlockedIncrement(&g_readFailCount);
                    if (e == ERROR_INVALID_HANDLE || e == ERROR_DEVICE_NOT_CONNECTED ||
                        e == ERROR_OPERATION_ABORTED || e == ERROR_NOT_READY) {
                        PostMessageW(g_hwnd, WM_DEVICE_LOST, 0, 0);
                        break;
                    }
                    continue;
                }
                ok = TRUE;
            } else {
                InterlockedExchange(&g_lastReadErr, (LONG)e);
                InterlockedIncrement(&g_readFailCount);
                if (e == ERROR_INVALID_HANDLE || e == ERROR_DEVICE_NOT_CONNECTED ||
                    e == ERROR_OPERATION_ABORTED || e == ERROR_NOT_READY) {
                    PostMessageW(g_hwnd, WM_DEVICE_LOST, 0, 0);
                    break;
                }
                Sleep(10);
                continue;
            }
        }
        if (!ok || read < 3) continue;
        g_idleTimeouts = 0;

        InterlockedIncrement(&g_reportCount);

        // Keep a short hex snapshot in Status... for debugging.
        int lim = (read > 8) ? 8 : (int)read;
        wchar_t line[128] = L"";
        for (int i = 0; i < lim; i++) {
            wchar_t b[8];
            StringCchPrintfW(b, _countof(b), L"%02X ", (unsigned)buf[i]);
            StringCchCatW(line, _countof(line), b);
        }
        StringCchCopyW(g_lastReportHex, _countof(g_lastReportHex), line);

        // Fixed parser (legacy layout): [1]=dy, [2]=dx.
        int dyFixed = (signed char)buf[1];
        int dxFixed = (signed char)buf[2];
        int dy = dyFixed;
        int dx = dxFixed;

        // Learn adaptive indices if fixed parser keeps producing zeros.
        int n = (read > 16) ? 16 : (int)read;
        for (int i = 1; i < n; i++) {
            int d = (int)(signed char)buf[i] - (int)(signed char)prevRaw[i];
            if (d < 0) d = -d;
            score[i] = score[i] - (score[i] / 16) + d; // leaky integrator
            prevRaw[i] = buf[i];
        }

        if (dyFixed == 0 && dxFixed == 0) zeroFixedStreak++;
        else zeroFixedStreak = 0;

        if (zeroFixedStreak > 120 && n > 4) {
            int best1 = -1, best2 = -1;
            for (int i = 1; i < n; i++) {
                if (best1 < 0 || score[i] > score[best1]) {
                    best2 = best1;
                    best1 = i;
                } else if (best2 < 0 || score[i] > score[best2]) {
                    best2 = i;
                }
            }
            if (best1 > 0 && best2 > 0 && best1 != best2 && score[best1] > 20) {
                g_parserMode = 1;
                g_parserY = best1;
                g_parserX = best2;
                havePrevDelta = FALSE;
            }
        }

        if (g_parserMode == 1) {
            if (!havePrevDelta) {
                for (int i = 0; i < 64; i++) prevDelta[i] = buf[i];
                havePrevDelta = TRUE;
                dy = 0;
                dx = 0;
            } else {
                int iy = g_parserY, ix = g_parserX;
                if (iy >= (int)read) iy = 1;
                if (ix >= (int)read) ix = 2;
                dy = (int)(signed char)buf[iy] - (int)(signed char)prevDelta[iy];
                dx = (int)(signed char)buf[ix] - (int)(signed char)prevDelta[ix];
                prevDelta[iy] = buf[iy];
                prevDelta[ix] = buf[ix];
            }
        } else {
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
            InterlockedIncrement(&g_emitCount);
        }
    }
    CloseHandle(ov.hEvent);
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
    g_reportCount = 0;
    g_emitCount = 0;
    g_idleTimeouts = 0;
    g_readFailCount = 0;
    g_lastReadErr = 0;
    g_parserMode = 0;
    g_parserY = 1;
    g_parserX = 2;
    g_lastReportHex[0] = 0;
}

static BOOL StartReader(void)
{
    StopReader();
    g_dev = OpenMagicMouse(NULL, NULL);
    if (!g_dev && g_excludedPath[0]) {
        // Safety valve: if exclusion leaves no usable node, retry once with
        // full candidate set.
        g_excludedPath[0] = 0;
        g_dev = OpenMagicMouse(NULL, NULL);
    }
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

    MouseCandidate arr[MAX_CANDIDATES];
    int n = CollectCandidates(arr, MAX_CANDIDATES);
    int lim = n > 6 ? 6 : n;
    wchar_t top[2800] = L"";
    for (int i = 0; i < lim; i++) {
        wchar_t line[480];
        if (arr[i].hasPdo) {
            DWORD err = 0;
            HANDLE h = TryOpenPath(arr[i].pdoPath, &err);
            if (h) CloseHandle(h);
            StringCchPrintfW(line, _countof(line),
                L"[%d] b=%d pdo=1 svc=%s err=%lu hwid=%s\r\n",
                i, arr[i].isBound ? 1 : 0,
                arr[i].service[0] ? arr[i].service : L"(none)",
                err,
                arr[i].sampleHwid[0] ? arr[i].sampleHwid : L"(none)");
        } else {
            StringCchPrintfW(line, _countof(line),
                L"[%d] b=%d pdo=0 svc=%s hwid=%s\r\n",
                i, arr[i].isBound ? 1 : 0,
                arr[i].service[0] ? arr[i].service : L"(none)",
                arr[i].sampleHwid[0] ? arr[i].sampleHwid : L"(none)");
        }
        StringCchCatW(top, _countof(top), line);
    }

    wchar_t msg[4096];
    StringCchPrintfW(msg, _countof(msg),
        L"State: %s\r\nCandidates: %d\r\nBound count: %d\r\nLast open path: %s\r\n"
        L"Open detail: %s\r\n"
        L"Excluded path: %s\r\n"
        L"Parser: %s (y=%d, x=%d)\r\n"
        L"Reports: %ld  Emits: %ld  IdleTimeouts: %ld\r\n"
        L"ReadFail: %ld  LastReadErr: %ld\r\n"
        L"Last report[0..7]: %s\r\n\r\nTop candidates:\r\n%s",
        stateText, cands, bound,
        g_lastOpenPath[0] ? g_lastOpenPath : L"(none)",
        g_lastOpenDetail[0] ? g_lastOpenDetail : L"(none)",
        g_excludedPath[0] ? g_excludedPath : L"(none)",
        g_parserMode == 0 ? L"fixed" : L"adaptive",
        g_parserY, g_parserX,
        g_reportCount, g_emitCount, g_idleTimeouts,
        g_readFailCount, g_lastReadErr,
        g_lastReportHex[0] ? g_lastReportHex : L"(none)",
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

