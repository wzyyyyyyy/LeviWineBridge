# LeviWineBridge

English | [简体中文](README.zh.md)

A LeviLamina client companion plugin for Minecraft: Bedrock Edition running under Wine, providing host integration bridges and compatibility fixes between a native host launcher (such as LeviLauncher on macOS) and the Wine game process:

- **Xbox Sign-In Bridge**: When the game generates a device code, the plugin forwards the prompt and sign-in result over loopback to a compatible native launcher, which opens the Microsoft verification page and tracks progress. If the launcher bridge is unavailable, the game falls back to its original sign-in prompt. Authentication and credential storage remain handled by XGameRuntime.
- **Native Terminal Console Bridge**: When the instance console is enabled, the plugin intercepts standard streams and Win32 Console APIs in-process (with ANSI color, UTF-8 text output, and interactive input) and bridges them over loopback to the host's default terminal emulator (e.g., Ghostty, iTerm2, Terminal.app).
- **Native Finder & File/Folder Dialog Bridge**: Active only under Wine (disable with `LEVI_WINE_SHELL_FIX=0`). Intercepts folder-opening and file-revealing Win32 calls (`ShellExecuteW/A`, `ShellExecuteExW/A`, `SHOpenFolderAndSelectItems`, and `explorer.exe` / `cmd /c start` via `CreateProcessW/A` & `WinExec`) and opens them in macOS Finder via the host bridge (automatically falling back to `winebrowser.exe` without a launcher). When the host bridge is available, also intercepts `GetOpenFileNameW/A`, `GetSaveFileNameW/A`, and `SHBrowseForFolderW/A` to show native macOS open/save file and folder pickers.
- **Wine Input Fixes**: Active only under Wine. Prevents first-frame camera snap spikes after regaining focus, releases stuck modifier/movement keys on window switch, and maps in-game `Cmd+C/V/X/A` shortcuts.
- **Virtual Window & Input Scaling (MetalFX)**: Active only under Wine when `LEVI_WINE_RENDER_SCALE` is set (e.g., `1.50` / `2.00`). Scales the client area dimensions and mouse coordinates seen by the game to pair with native macOS `MTLFXSpatialScaler` upscaling on D3DMetal.
- **CJK Fallback Font Fix (Arke, etc.)**: Under Wine, if `C:\Windows\Fonts\msyh.ttc` is missing, automatically copies a macOS system CJK font (preferring `Hiragino Sans GB.ttc`) into the prefix's Fonts directory so Chinese characters render properly instead of `?`.
- **PreLoader 97% Heap-Lock Fix**: Prevents startup deadlocks when running alongside PreLoader under Wine by avoiding suspending threads while they hold the process heap lock.

## Installation

The lip package ID is `github.com/wzyyyyyyy/LeviWineBridge` (`client` / `win-x64`), compatible with all Minecraft and LeviLamina versions (`>=0.0.0`). Installing it from LeviLauncher's mod catalog resolves dependencies automatically.

For manual installation, first install LeviLamina for your instance, then download `LeviWineBridge-client-windows-x64.zip` from [GitHub Releases](https://github.com/wzyyyyyyy/LeviWineBridge/releases) and import it via LeviLauncher's mod manager.

## Building

Requires Python 3 and MinGW-w64 (or Zig):

```sh
python3 build.py
# Or using Zig
MINGW_CC='zig cc -target x86_64-windows-gnu' python3 build.py
```

The build output is `dist/LeviWineBridge.dll`. LeviLauncher injects the following loopback / runtime environment variables as needed:

- Sign-in bridge: `LEVI_GAME_AUTH_URL`, `LEVI_GAME_AUTH_TOKEN`
- Console bridge: `LEVI_CONSOLE_ADDR`, `LEVI_CONSOLE_TOKEN`
- Shell & dialog bridge: `LEVI_SHELL_BRIDGE_URL`, `LEVI_SHELL_BRIDGE_TOKEN` (set `LEVI_WINE_SHELL_FIX=0` to disable)
- Virtual render scaling: `LEVI_WINE_RENDER_SCALE` (active when `> 1.01` and `<= 4.0`, e.g., `1.33`, `1.50`, `2.00`)

When publishing a release, package the DLL, `manifest.json`, and `COPYING` inside `LeviClient/` in `LeviWineBridge-client-windows-x64.zip`. Keep the Git tag, `lip.toml`, and `manifest.json` on the same version.

## Verification

```sh
cc -Wall -Wextra -Werror protocol_test.c -o /tmp/levi-wine-bridge-protocol
/tmp/levi-wine-bridge-protocol

python3 tests/smoke.py \
  --compiler 'zig cc -target x86_64-windows-gnu' \
  --launcher '/path/Levi Wine.app/Contents/MacOS/wineskinlauncher' \
  --output /tmp/levi-wine-bridge-smoke
```

The Wine integration suite uses a mock device-code prompt and local loopback servers to verify sign-in forwarding, fallback, inert mode, PreLoader heap-lock guard, bidirectional UTF-8 / ANSI console I/O, input fixes, and folder-open / file-dialog bridging.

## License

Originally split out from `native/leviclient` in [LeviLauncher's macOS adaptation branch](https://github.com/wzyyyyyyy/LeviLauncher). Licensed under GPL-3.0-only; see [COPYING](COPYING).
