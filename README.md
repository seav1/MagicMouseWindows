# MagicMouseWindows

**Free scroll & touch support for Apple Magic Mouse on Windows 10 and Windows 11**

> Works with Apple Magic Mouse (USB-C / A3204, 2024 model) and older models.
> Single ~150 KB native exe, no .NET runtime required.

![Windows 10/11](https://img.shields.io/badge/Windows-10%20%7C%2011-0078D4?logo=windows)
![Native](https://img.shields.io/badge/Build-Win32%20native-orange)
![License](https://img.shields.io/badge/License-MIT-green)
![Free](https://img.shields.io/badge/Price-Free-brightgreen)

---

## Features

- Vertical + horizontal scroll
- Adjustable scroll speed (presets in tray menu)
- Natural scroll (macOS style) toggle
- Auto-reconnect on disconnect
- Start with Windows option
- Settings saved to `%APPDATA%\MagicMouse\config.ini`
- Tray-only UI - no settings dialog clutter

---

## Requirements

- Windows 10 (1607 / build 14393 or later) **or** Windows 11, x64
- Apple Magic Mouse (USB-C A3204, Magic Mouse 2, original Magic Mouse all supported)
- Magic Utilities driver `MagicMouse.sys` (free trial - see setup below)

---

## Quick Start

### Step 1 - Get the driver

This app needs `MagicMouse.sys` from Magic Utilities. You only need their free trial - **the driver itself keeps working forever** even after you uninstall their app.

1. Download Magic Utilities trial: https://magicutilities.net
2. Install it (this installs the driver automatically)
3. Save a copy of the driver files:

```powershell
New-Item -Path "C:\MagicMouseDriver" -ItemType Directory -Force
Copy-Item "C:\Program Files\MagicUtilities\DriverMouse\MagicMouse.sys" "C:\MagicMouseDriver\"
Copy-Item "C:\Program Files\MagicUtilities\DriverMouse\MagicMouse.inf" "C:\MagicMouseDriver\"
Copy-Item "C:\Program Files\MagicUtilities\DriverMouse\MagicMouse.cat" "C:\MagicMouseDriver\"
```

4. Uninstall Magic Utilities (keep the driver files you copied)
5. Reinstall just the driver:

```powershell
pnputil /add-driver "C:\MagicMouseDriver\MagicMouse.inf" /install
```

6. Verify the driver service is running:

```powershell
sc query MagicMouse
```

It must show `STATE: 4 RUNNING`.

### Step 2 - Download

Download `MagicMouse.exe` from the [Releases page](https://github.com/seav1/MagicMouseWindows/releases).

It's a single ~150 KB exe - no installer.

### Step 3 - Run

Double-click `MagicMouse.exe`. A small white-and-blue dot icon appears in the system tray.

Right-click the tray icon to open the menu:

```
Magic Mouse  [connected]
─────────────────────────
Scroll speed              ▶  (1x / 2x / 3x / 5x / 8x / 10x)
  Natural scroll
  Horizontal scroll  ✓
  Start with Windows
─────────────────────────
Reconnect
Diagnostics...
─────────────────────────
Quit
```

---

## Build from source

### Native version (recommended) - `src/MagicMouse/`

Requirements: Visual Studio Build Tools (free) with the **Desktop development with C++** workload.

```powershell
cd src\MagicMouse
.\build.cmd
```

Output: `src\MagicMouse\build\MagicMouse.exe` (~150 KB).

The `build.cmd` script auto-detects MSVC via `vswhere.exe`. If you're already inside a Developer Command Prompt it just uses that.

### Legacy .NET version - `src/MagicMouseApp/`

The original .NET 6 / WinForms implementation is kept for reference. To build it:

```powershell
cd src\MagicMouseApp
dotnet publish -c Release -r win-x64 --self-contained true -p:PublishSingleFile=true
```

Output: ~60 MB self-contained exe under `bin\Release\net6.0-windows\win-x64\publish\`.

---

## Troubleshooting

**Scroll doesn't work**

Right-click the tray icon → **Diagnostics...**. The dialog shows every PnP device whose driver service starts with `MagicMouse`, plus the resolved kernel PDO path.

- If the list is **empty**, the driver isn't bound to your mouse:
  - Run `sc query MagicMouse` in PowerShell - state must be `RUNNING`
  - Re-run `pnputil /add-driver "C:\MagicMouseDriver\MagicMouse.inf" /install`
  - Re-pair the mouse via Bluetooth → click **Reconnect**
- If the list **shows a device** but scroll still doesn't work, click **Reconnect**. Some Bluetooth stacks need the handle reopened after sleep/wake.
- Press `Ctrl+C` inside the dialog to copy the entire report to clipboard, then attach to a GitHub issue.

**Important**: while Magic Utilities was installed, **their userland app** was generating scroll events. After you uninstall it, this app takes over - but Magic Utilities' driver service (`MagicMouse`) must still be running.

**Scroll direction wrong**

Right-click tray → **Natural scroll** to toggle.

**App seems to disappear**

It's only in the tray. Right-click the tray icon to access it. To prevent Windows from auto-hiding the icon, drag it from the overflow flyout into the always-visible area.

---

## How it works

1. `MagicMouse.sys` from Magic Utilities is bound to the Bluetooth Magic Mouse at the enumerator level.
2. It exposes a non-HID raw touch PDO whose kernel name (e.g. `\Device\00000416`) is assigned by PnP at install time. This number is **different on every machine**.
3. The app calls `SetupDiGetClassDevs(NULL, NULL, NULL, DIGCF_PRESENT | DIGCF_ALLCLASSES)`, walks the PnP tree, finds devices whose `SPDRP_SERVICE` is `MagicMouse`, then reads `SPDRP_PHYSICAL_DEVICE_OBJECT_NAME` to get the dynamic PDO name.
4. Opens it via `\\.\GLOBALROOT\Device\<pdo-name>` and runs a `ReadFile` loop on a worker thread.
5. Each report's signed delta-X / delta-Y bytes are accumulated and emitted as `SendInput(MOUSEEVENTF_WHEEL / _HWHEEL)` events.

---

## Credits

- Driver: [Magic Utilities](https://magicutilities.net) — their `MagicMouse.sys` kernel driver makes this possible.
- Native rewrite: single-file C++ Win32 (no .NET, no MFC, no STL).
- Reverse engineered via Boot Camp driver analysis and HID report capture.

---

## License

MIT License — free to use, modify, and distribute.

Note: `MagicMouse.sys` is proprietary to Magic Utilities and is **not** included in this repository. Obtain it separately via their free trial as described above.
