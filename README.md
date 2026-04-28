# 🖱 MagicMouseWindows

**Free scroll & touch support for Apple Magic Mouse USB-C on Windows 11**

> Works with Apple Magic Mouse (USB-C / A3204, 2024 model) — the newest model that no free driver previously supported.

![Windows 11](https://img.shields.io/badge/Windows-11-0078D4?logo=windows)
![.NET 6](https://img.shields.io/badge/.NET-6.0-512BD4?logo=dotnet)
![License](https://img.shields.io/badge/License-MIT-green)
![Free](https://img.shields.io/badge/Price-Free-brightgreen)

---

## ✨ Features

- ✅ Vertical scroll
- ✅ Horizontal scroll
- ✅ Adjustable scroll speed
- ✅ Natural scroll (macOS style) toggle
- ✅ Acceleration control
- ✅ Runs in system tray
- ✅ Auto-reconnect on disconnect
- ✅ Start with Windows option
- ✅ Settings saved automatically

---

## 📋 Requirements

- Windows 11 (x64)
- Apple Magic Mouse USB-C (A3204 / 2024 model)
- Magic Utilities driver (free to extract — see setup below)

---

## 🚀 Quick Start

### Step 1 — Get the driver

The app requires `MagicMouse.sys` from Magic Utilities. You only need the free trial — you don't need to pay.

1. Download **Magic Utilities** trial: https://magicutilities.net
2. Install it (it installs the driver automatically)
3. Copy the driver files to a safe location:

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

### Step 2 — Download MagicMouseWindows

Download the latest release from the [Releases page](https://github.com/kanishkkmalik/MagicMouseWindows/releases).

Download `MagicMouseApp.exe` — no install needed, just run it.

### Step 3 — Run

Double-click `MagicMouseApp.exe`. It appears in your system tray.

Double-click the tray icon to open settings.

---

## ⚙️ Settings

| Setting | Description |
|---|---|
| Scroll Speed | How fast the page scrolls (1x - 10x) |
| Acceleration | Momentum multiplier |
| Natural Scroll | Reverses scroll direction (macOS style) |
| Horizontal Scroll | Enable/disable horizontal scrolling |
| Start with Windows | Launch automatically on startup |

---

## 🔧 Build from Source

Requirements:
- [.NET 6 SDK](https://dotnet.microsoft.com/download/dotnet/6.0)

```bash
git clone https://github.com/kanishkkmalik/MagicMouseWindows
cd MagicMouseWindows/src/MagicMouseApp
dotnet publish -c Release -r win-x64 --self-contained true
```

The `.exe` will be in `bin/Release/net6.0-windows/win-x64/publish/`

---

## 🐛 Troubleshooting

**Scroll not working after launch**
- Make sure the Magic Mouse is paired and connected via Bluetooth
- Check that `MagicMouse.sys` is installed (run `sc query MagicMouse` in PowerShell)
- Try closing and reopening the app

**Mouse disconnects**
- This was a known issue with manual driver injection — the current version uses the proper Magic Utilities driver which is stable

**Scroll direction is wrong**
- Open Settings → toggle **Natural Scroll**

---

## 📝 How it works

1. Magic Utilities' `MagicMouse.sys` driver is installed at the Bluetooth enumerator level
2. It exposes a raw touch interface (`MAGICMOUSERAWPDO`)
3. Our app opens this interface and reads raw touch reports
4. Touch deltas are converted to standard Windows scroll wheel events via `SendInput`
5. Works with any app that supports mouse wheel scrolling

---

## 🙏 Credits

- Driver: [Magic Utilities](https://magicutilities.net) — their `MagicMouse.sys` kernel driver makes this possible
- App: Built with C# / .NET 6 Windows Forms
- Reverse engineered with help from Boot Camp driver analysis and HID report capture

---

## 📄 License

MIT License — free to use, modify, and distribute.

Note: `MagicMouse.sys` is proprietary to Magic Utilities and is not included in this repository. You must obtain it separately as described in the setup instructions.
