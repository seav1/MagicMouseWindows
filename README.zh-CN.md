# Magic Mouse for Windows

> **语言：** [English](README.md) | [简体中文](README.zh-CN.md)

一个 ~6.5 MB 的托盘小程序，让 Apple 的 **Magic Mouse 1 / 2 / 3** 在 Windows
10 / 11 上原生支持两指滑动滚轮。Magic Utilities 内核驱动已经打包进 `.exe`，
首次运行时自动安装。

* 不依赖 .NET，无需手动装驱动，运行期不需要管理员权限——只有第一次安装驱动时弹一次 UAC。
* 一个 `MagicMouse.exe` 走遍全场，配置存在 `%APPDATA%\MagicMouse\config.ini`。
* 右键托盘图标即可调滚动速度、自然滚动、横向滚动、开机启动。

---

## 快速开始

1. 在 **设置 → 蓝牙和设备** 里把 Magic Mouse 配对上。
2. 从 [Releases 页面](https://github.com/seav1/MagicMouseWindows/releases) 下载
   `MagicMouse.exe`，双击运行。
3. 第一次运行会弹 UAC，点 **是** —— 程序自动安装内置驱动。完成后托盘图标会显示"已连接"。
4. 用一根手指在鼠标表面上下滑动即可滚动。完事。

不用额外下载驱动、不用进设备管理器、不用 PowerShell。

> 内置的驱动是 **Magic Utilities for Magic Mouse v3.1.5.3**
> （`MagicMouse.sys`，由 Magic Utilities Pty Ltd 签名）。

---

## 托盘菜单

* **Scroll speed**（滚动速度）—— 1.0× … 10.0×
* **Natural scroll**（自然滚动）—— 反向滚动（macOS 风格）
* **Horizontal scroll**（横向滚动）—— 启用 / 禁用左右滚动
* **Start with Windows**（开机启动）—— 写 / 删 `HKCU\…\Run` 注册表项
* **Reconnect**（重新连接）—— 休眠或蓝牙抽风后用
* **Reinstall driver…**（重装驱动）—— 状态不对时再来一次提权安装
* **Quit**（退出）

---

## 常见问题

| 现象 | 处理 |
|---|---|
| 托盘显示"未连接" | 鼠标是否已配对并打开？点 **Reconnect** |
| 首次 UAC 点了"否" | 右键托盘 → **Reinstall driver…** |
| 指针能动，就是滚不动 | 见下面"驱动装上但鼠标没绑定"那一节 |
| 昨天还行，今天 Windows 更新后挂了 | 跟上面一样，Windows 偶尔会把驱动切回自带 HID |
| 同时连了多个 Magic Mouse | 只会打开第一个绑定到 `MagicMouse` 服务的设备 |

如果你想直接用官方付费版，请卸载本程序后到
[Magic Utilities](https://magicutilities.net/) 下载安装。

### "驱动装上了，但鼠标没绑定"

程序会跑 `pnputil /add-driver /install`，再把已枚举的 Apple-VID 设备
逐个 `/remove-device` + `/scan-devices`。但对**已经配过对**的蓝牙鼠标，
Windows 不会从 `hidbth` 切到 `MagicMouse`，必须**手动取消配对再配对**：

1. **设置 → 蓝牙和设备**，点鼠标后面的 `…` 菜单 → **删除设备**
2. **添加设备 → 蓝牙**，重新配对鼠标
3. 右键托盘 → **Reconnect**，托盘提示应变为"connected"

USB 线连接的话，拔了再插也能达到同样效果。重启 Windows 也行。

---

## 自己编译

需要安装 **Visual Studio Build Tools** + **使用 C++ 的桌面开发** 工作负载
（或者完整的 Visual Studio）。

```cmd
cd src\MagicMouse
build.cmd
```

产物在 `src\MagicMouse\build\MagicMouse.exe`。`.github/workflows/build.yml`
的 CI 在每次打 tag 时执行同样的命令。

### 源码结构

```
src/MagicMouse/
├── MagicMouse.cpp     ← 单文件 Win32 程序（~800 行）
├── MagicMouse.rc      ← manifest、版本信息、嵌入的驱动资源
├── app.manifest       ← 声明 Win10/11 / PerMonitorV2 DPI / asInvoker
├── build.cmd          ← 一键 MSVC 编译
└── drivers/           ← MagicMouse.{sys,inf,cat}，作为 RCDATA 嵌入
```

---

## 许可与致谢

* 应用代码 —— MIT 协议，见 `LICENSE`。
* 驱动文件 (`drivers/MagicMouse.{sys,inf,cat}`) 版权归 Magic Utilities Pty Ltd
  所有，按其免费分发的驱动包条款再分发。如果你在商业环境中使用，请到
  <https://magicutilities.net/> 支持原作者。
* 最早的灵感和 .NET 版本：**mtorromeo/MagicMouseWindows**。
