# Magic Mouse for Windows

> **Languages:** [English](README.md) | [简体中文](README.zh-CN.md)

A small (~6.5 MB) tray app that gives Apple's **Magic Mouse 1 / 2 / 3** native
two-finger scrolling on Windows 10 and 11. The Magic Utilities kernel driver is
embedded inside the `.exe` and auto-installed on first run.

* No .NET, no installer, no admin needed at runtime — only one UAC prompt the
  first time, to register the driver.
* Single `MagicMouse.exe`. Move it anywhere; settings live in
  `%APPDATA%\MagicMouse\config.ini`.
* Right-click the tray icon for scroll speed, natural / horizontal scroll, and
  start-with-Windows.

---

## Quick start

1. Pair your Magic Mouse via **Settings → Bluetooth & devices**.
2. Download `MagicMouse.exe` from the [Releases page](https://github.com/seav1/MagicMouseWindows/releases) and run it.
3. The first time, you'll see a UAC prompt — click **Yes** to install the
   bundled driver. The app then resumes and the tray icon goes "connected".
4. Scroll with one finger on the mouse surface. Done.

That's it. No external driver download, no Device Manager, no PowerShell.

> The driver is **Magic Utilities** for Magic Mouse v3.1.5.3
> (`MagicMouse.sys`, signed by Magic Utilities Pty Ltd).

---

## Tray menu

* **Scroll speed** — 1.0× … 10.0×
* **Natural scroll** — invert direction (macOS-style)
* **Horizontal scroll** — enable/disable side-to-side scrolling
* **Start with Windows** — toggles `HKCU\…\Run`
* **Reconnect** — re-open the device (use after sleep / Bluetooth glitches)
* **Reinstall driver…** — re-runs the elevated installer if something is off
* **Quit**

---

## Troubleshooting

| Symptom | Fix |
|---|---|
| Tray says "not connected" | Mouse paired and powered on? Click **Reconnect**. |
| First-run UAC dialog rejected | Right-click tray → **Reinstall driver…** |
| Pointer moves but no scrolling | **Reinstall driver…** — the installer only switches the driver of the Magic Mouse PIDs (0269 / 0323 / 030d / 0310). It will **not** touch your Bluetooth keyboard, AirPods, etc. |
| Worked yesterday, dead today after Windows Update | Same as above — Windows occasionally restores its built-in HID driver after updates. |
| Multiple Magic Mice connected | Only the first device whose driver service is `MagicMouse` is opened. |

If everything else fails: open **Settings → Bluetooth & devices**, remove
the Magic Mouse, then pair it again. The newly-arrived device will pick up
the registered driver automatically.

If you'd rather use the official paid app, uninstall this one and grab
[Magic Utilities](https://magicutilities.net/).

### How the installer works

The installer **does not** delete or re-scan unrelated devices. It does
exactly two things:

1. `pnputil /add-driver MagicMouse.inf /install` — registers the INF in the
   Windows driver store.
2. `UpdateDriverForPlugAndPlayDevicesW(...)` once per known Magic Mouse
   hardware ID — Windows' precise per-PID driver-swap API. Other Apple
   devices (Magic Keyboard, AirPods, …) are not affected because their
   PIDs are not in our list.

---

## Build from source

You need **Visual Studio Build Tools** with the *Desktop development with C++*
workload (or full Visual Studio).

```cmd
cd src\MagicMouse
build.cmd
```

The output is `src\MagicMouse\build\MagicMouse.exe`. CI in
`.github/workflows/build.yml` does the same on every tag push.

### Source layout

```
src/MagicMouse/
├── MagicMouse.cpp     ← single-file Win32 app (~800 lines)
├── MagicMouse.rc      ← manifest + version + embedded driver resources
├── app.manifest       ← Win10/11 supportedOS, PerMonitorV2 DPI, asInvoker
├── build.cmd          ← one-click MSVC build
└── drivers/           ← MagicMouse.{sys,inf,cat} embedded as RCDATA
```

---

## License & credits

* App code — MIT (see `LICENSE`).
* Driver (`drivers/MagicMouse.{sys,inf,cat}`) © Magic Utilities Pty Ltd,
  redistributed under the terms of their freely-available driver package.
  If you use this commercially, please support them at
  <https://magicutilities.net/>.
* Original idea / earlier .NET version: **mtorromeo/MagicMouseWindows**.
