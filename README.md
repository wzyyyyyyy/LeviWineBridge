# LeviWineBridge

这是一个面向 Wine 中 Minecraft 基岩版的 LeviLamina 客户端配套插件，用于在原生宿主（如 macOS 上的 LeviLauncher）与 Wine 游戏进程之间提供系统桥接和兼容性修复：

- **Xbox 登录桥接**：游戏生成设备码后，插件通过本机回环把提示和登录结果交给兼容的原生启动器，由启动器打开微软验证页并显示登录进度；启动器不可用时，游戏继续显示原登录提示。身份验证与凭据存储仍由游戏的 XGameRuntime 处理。
- **原生终端控制台桥接**：启用实例控制台时，插件在进程内接管标准流与 Win32 Console API（支持 ANSI 色彩、UTF-8 中文输出与交互输入），通过本机回环桥接到宿主默认终端应用（如 Ghostty、iTerm2、Terminal.app 等）。
- **Wine 输入体验修复**：仅在 Wine 下激活（原生 Windows 静默），包含重获焦点首帧防视角甩头、切窗自动释放粘连修饰/移动键，以及游戏内 `Cmd+C/V/X/A` 快捷键映射。
- **PreLoader 97% 堆锁兼容修复**：与 PreLoader 一起在 Wine 中运行时，避免挂起持有进程堆锁的线程导致的启动卡死。

## 安装

lip 包 ID 为 `github.com/wzyyyyyyy/LeviWineBridge`，适用于 `client` / `win-x64` ，依赖 LeviLamina 26.60.8 的客户端版本。通过 LeviLauncher 的模组目录安装时，包管理器会自动解析该依赖。安装位置沿用 `mods/LeviClient/`，与早期版本共用该目录。

手动导入时，先在目标实例安装 LeviLamina 26.60.8 的客户端版本，再从 [GitHub Releases](https://github.com/wzyyyyyyy/LeviWineBridge/releases) 下载 `LeviWineBridge-client-windows-x64.zip`，通过 LeviLauncher 的「管理模组」导入。

## 构建

需要 Python 3 和 MinGW-w64，或者 Zig。

```sh
python3 build.py
# 或者使用 Zig
MINGW_CC='zig cc -target x86_64-windows-gnu' python3 build.py
```

构建产物为 `dist/LeviWineBridge.dll`。LeviLauncher 启动游戏时会按需提供以下本机回环环境变量（未配置时插件保持静默，普通 Windows 启动无副作用）：

- 登录桥：`LEVI_GAME_AUTH_URL`、`LEVI_GAME_AUTH_TOKEN`
- 控制台桥：`LEVI_CONSOLE_ADDR`、`LEVI_CONSOLE_TOKEN`

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

Wine 集成测试使用模拟设备码与本机回环服务，检查登录转发、失败回退、未配置静默、内存锁兼容以及控制台双向 UTF-8 / ANSI 交互。

源码最初从 [LeviLauncher 的 macOS 适配分支](https://github.com/wzyyyyyyy/LeviLauncher) 中的 `native/leviclient` 拆出。许可证为 GPL-3.0-only，见 [COPYING](COPYING)。
