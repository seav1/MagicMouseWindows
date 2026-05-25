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
| Pointer moves but no scrolling | **Reinstall driver…**, then in Windows **Settings → Bluetooth & devices** click the mouse's `…` menu → **Remove device**, then pair it again. Windows only switches drivers for an already-paired BT device after a re-pair. |
| Worked yesterday, dead today after Windows Update | Same as above — Windows sometimes promotes its built-in HID driver after updates. |
| Multiple Magic Mice connected | Only the first device whose service is `MagicMouse` is opened. |

If you'd rather use the official paid app, uninstall this and grab
[Magic Utilities](https://magicutilities.net/).

### "Driver added, but no Magic Mouse is bound"

The bundled installer runs `pnputil /add-driver /install` and then
removes + re-scans every Apple-VID device it sees. For a Bluetooth mouse,
Windows still won't switch from `hidbth` to `MagicMouse` until the device
is **unpaired and re-paired**. Steps:

1. **Settings → Bluetooth & devices**, click the `…` next to your Magic
   Mouse and choose **Remove device**.
2. **Add device → Bluetooth**, pair the mouse again.
3. Right-click the tray icon → **Reconnect**. Tray should switch to
   *"connected"*.

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
