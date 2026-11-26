# LeviWineBridge

[English](README.md) | 简体中文

这是一个面向 Wine 中 Minecraft 基岩版的 LeviLamina 客户端配套插件，用于在原生宿主（如 macOS 上的 LeviLauncher）与 Wine 游戏进程之间提供系统桥接和兼容性修复：

- **Xbox 登录桥接**：游戏生成设备码后，插件通过本机回环把提示和登录结果交给兼容的原生启动器，由启动器打开微软验证页并显示登录进度；启动器不可用时，游戏继续显示原登录提示。身份验证与凭据存储仍由游戏的 XGameRuntime 处理。
- **原生终端控制台桥接**：启用实例控制台时，插件在进程内接管标准流与 Win32 Console API（支持 ANSI 色彩、UTF-8 中文输出与交互输入），通过本机回环桥接到宿主默认终端应用（如 Ghostty、iTerm2、Terminal.app 等）。
- **原生 Finder 与文件选择对话框桥接**：仅在 Wine 下激活（可通过 `LEVI_WINE_SHELL_FIX=0` 关闭）。接管 `ShellExecuteW/A`、`ShellExecuteExW/A`、`SHOpenFolderAndSelectItems` 以及 `CreateProcessW/A` / `WinExec`（`explorer.exe` / `cmd /c start`）的打开目录与定位文件调用，通过本机回环交由 macOS 原生 Finder 打开（无启动器时自动回退到 `winebrowser.exe`）；在启动器桥接可用时同时接管 `GetOpenFileNameW/A`、`GetSaveFileNameW/A` 与 `SHBrowseForFolderW/A`，弹出 macOS 原生打开/保存文件与选目录面板。
- **Wine 输入体验修复**：仅在 Wine 下激活，包含重获焦点首帧防视角甩头、切窗自动释放粘连修饰/移动键，以及游戏内 `Cmd+C/V/X/A` 快捷键映射。
- **虚拟窗口与坐标缩放（MetalFX）**：仅在 Wine 且设置 `LEVI_WINE_RENDER_SCALE`（如 `1.50` / `2.00`）时激活，按倍率缩放游戏读取的客户区尺寸与客户区鼠标坐标，配合 macOS 宿主侧原生 `MTLFXSpatialScaler` 实现 D3DMetal 空间超分。
- **Arke 中文字体修复**：在 Wine 下若 `C:\Windows\Fonts\msyh.ttc` 缺失，自动从 macOS 系统自带字体（优先 `Hiragino Sans GB.ttc`）复制到容器字体目录，修复 Arke 等模组中文显示为 `?` 的问题。
- **PreLoader 97% 堆锁兼容修复**：与 PreLoader 一起在 Wine 中运行时，避免挂起持有进程堆锁的线程导致的启动卡死。

## 安装

lip 包 ID 为 `github.com/wzyyyyyyy/LeviWineBridge`，适用于 `client` / `win-x64`，兼容全版本 Minecraft 与 LeviLamina（`>=0.0.0`）。通过 LeviLauncher 的模组目录安装时，包管理器会自动解析该依赖。

手动导入时，先在目标实例安装对应版本的 LeviLamina 客户端，再从 [GitHub Releases](https://github.com/wzyyyyyyy/LeviWineBridge/releases) 下载 `LeviWineBridge-client-windows-x64.zip`，通过 LeviLauncher 的「管理模组」导入。

## 构建

需要 Python 3 和 MinGW-w64，或者 Zig。

```sh
python3 build.py
# 或者使用 Zig
MINGW_CC='zig cc -target x86_64-windows-gnu' python3 build.py
```

构建产物为 `dist/LeviWineBridge.dll`。LeviLauncher 启动游戏时会按需提供以下本机回环 / 运行时环境变量：

- 登录桥：`LEVI_GAME_AUTH_URL`、`LEVI_GAME_AUTH_TOKEN`
- 控制台桥：`LEVI_CONSOLE_ADDR`、`LEVI_CONSOLE_TOKEN`
- Shell / 对话框桥：`LEVI_SHELL_BRIDGE_URL`、`LEVI_SHELL_BRIDGE_TOKEN`（设 `LEVI_WINE_SHELL_FIX=0` 可关闭）
- 虚拟渲染缩放：`LEVI_WINE_RENDER_SCALE`（`> 1.01` 且 `<= 4.0` 时启用，例如 `1.33`、`1.50`、`2.00`）

发布时，将 DLL、`manifest.json` 和 `COPYING` 放入 ZIP 内的 `LeviClient/`，以 `LeviWineBridge-client-windows-x64.zip` 上传。Git 标签、`lip.toml` 和 `manifest.json` 使用同一版本号。

## 验证

```sh
cc -Wall -Wextra -Werror protocol_test.c -o /tmp/levi-wine-bridge-protocol
/tmp/levi-wine-bridge-protocol

python3 tests/smoke.py \
  --compiler 'zig cc -target x86_64-windows-gnu' \
  --launcher '/path/Levi Wine.app/Contents/MacOS/wineskinlauncher' \
  --output /tmp/levi-wine-bridge-smoke
```

Wine 集成测试使用模拟设备码与本机回环服务，检查登录转发、失败回退、未配置静默、内存锁兼容、控制台双向 UTF-8 / ANSI 交互、输入修复以及目录打开 / 原生文件选择对话框桥接。

## 许可证

源码最初从 [LeviLauncher 的 macOS 适配分支](https://github.com/wzyyyyyyy/LeviLauncher) 中的 `native/leviclient` 拆出。许可证为 GPL-3.0-only，见 [COPYING](COPYING)。
