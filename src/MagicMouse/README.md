# MagicMouse — native Win32 build

A single-file C++ Win32 reimplementation of the .NET tray app, with the same
functionality but **no .NET runtime** and **no third-party dependencies**.

| | .NET version (`src/MagicMouseApp`) | Native version (this folder) |
|---|---|---|
| Source files | 8 .cs + .csproj + manifest | 1 .cpp + .rc + .manifest |
| Lines of code | ~700 | ~470 |
| Output size | ~60 MB single-file (self-contained .NET 6) | ~150 KB |
| Cold-start RAM | ~80 MB | ~3 MB |
| Runtime requirements | none (self-contained) | none (uses Win32 APIs only) |
| Min OS | Windows 10 1607 | Windows 8.1 |

## Files

- `MagicMouse.cpp` — entire program (settings, device discovery, read loop, tray UI)
- `MagicMouse.rc` — embeds `app.manifest` + version block
- `app.manifest` — Win10/Win11 supportedOS GUIDs + per-monitor V2 DPI awareness
- `build.cmd` — one-click local build (auto-locates MSVC via `vswhere`)

## Build locally

```powershell
cd src\MagicMouse
.\build.cmd
```

Output: `build\MagicMouse.exe`.

You need *Visual Studio Build Tools* (the free package) with the **Desktop
development with C++** workload installed. The script auto-detects it via
`vswhere.exe`. If you're already inside a Developer Command Prompt the script
just uses that.

## Build via GitHub Actions

Push a `v*` tag (or use `Run workflow`) — see `.github/workflows/build.yml`. The
runner uses MSVC out of the box; the resulting `MagicMouse.exe` is uploaded as
an artifact and attached to the GitHub Release.

## What it does

1. `SetupDiGetClassDevs(NULL, NULL, NULL, DIGCF_PRESENT | DIGCF_ALLCLASSES)`
   walks every present PnP device.
2. For each, read `SPDRP_SERVICE`. If it equals `MagicMouse` (or one of the
   variants in `kServices`), read `SPDRP_PHYSICAL_DEVICE_OBJECT_NAME` to get
   the kernel PDO name (e.g. `\Device\00000416`).
3. `CreateFileW(L"\\\\.\\GLOBALROOT\\Device\\00000416", ...)` opens the raw
   touch interface that Magic Utilities' driver exposes.
4. A worker thread does `ReadFile` in a loop, byte 1 = signed Y delta,
   byte 2 = signed X delta. Apply user's speed/natural/horizontal settings,
   accumulate, and emit `SendInput(MOUSEEVENTF_WHEEL / _HWHEEL)`.
5. The tray menu lives on a hidden `HWND_MESSAGE` window. No setting dialog,
   no XAML, no WinForms — just `TrackPopupMenu` for the menu and `MessageBox`
   for the diagnostics view.

## Settings

Stored in `%APPDATA%\MagicMouse\config.ini` via `WritePrivateProfileStringW`.

```ini
[main]
speed_x10=30
natural=0
horizontal=1
autostart=0
```

## Why no setting dialog?

The whole UI fits in a single tray right-click menu:

```
Magic Mouse  [connected]
─────────────────────────
Scroll speed              ▶  ✓ 3.0x  (default)
✓ Horizontal scroll
  Natural scroll
  Start with Windows
─────────────────────────
Reconnect
Diagnostics...
─────────────────────────
Quit
```

## Notes on the `Diagnostics...` view

Lists every device whose driver service starts with `MagicMouse`, including
the dynamically resolved PDO path. Press `Ctrl+C` inside the dialog to copy
the entire report to the clipboard.
