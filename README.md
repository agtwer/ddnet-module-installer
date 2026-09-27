# ddnet-module-installer

这是一个 **DDNet 源码拉取编译器**，支持在编译前插入模块以实现自定义功能：选来源与版本 → 自动拉源码 → 套模块补丁 → 编译 → 组装出可直接双击的客户端。
C++ / Win32 单文件程序。
**开发模块**：见 **[模块开发文档](MODULE-DEV.md)**。

## 它做什么

| 能力 | 说明 |
|---|---|
| **拉取版本信息** | 从 GitHub `releases`（失败退回 `tags`）拉取所选来源的版本列表（版本号 + 日期 + 是否预发布）；**网络可配**：国内镜像可选，本地代理可选（默认 `127.0.0.1:7890`），两项都同时作用于 HTTP 请求与 git 子进程。镜像大多**不代 `api.github.com`**，所以版本接口走镜像失败时会**自动退回直连**再试一次并在日志里说明 |
| **选择安装哪个版本** | 列表里点选 |
| **模块选择安装** | **单文件 mod（`.dmod`）驱动**，可多选、可拖入 |
| **一键装完可用** | 拉源码 → 子模块 → 套模块补丁（3-way，失败自动 `--reject` 并留 `.rej`）→ 装 FFmpeg 8.1（模块声明 `requires` 时才装，视频背景必需）→ CMake 配置 → 编译 → 组装出 `client\<种类>-<版本>\`（双击即玩） |
| **更新能力** | 记录 `install-state.json`（来源/版本/模块/目录/时间）；`检查更新` 会比对最新版本并给出「已是最新 / 可更新 X → Y」 |
| **日志可追溯** | 界面实时日志 + 落地 `installer.log`（便于自动化核对与排错） |
| **进度与速度** | 进度条走**整体进度**（2% 准备 → 5~15% 下载源码 → 18~30% 子模块 → 30~55% 模块 → 60~78% FFmpeg → 82% 配置 → 85~94% 编译 → 100%）。下面那行状态实时显示**当前下载的百分比 + 速度**，例如 `下载源码：15% · 340.00 KiB/s · 整体 6%`（源码、子模块、FFmpeg 三个阶段都有；下载地址显示在每一步的开头行）。**编译阶段没有可靠的百分比**，所以进度条切成滚动动画，状态行显示 `已编译 N 个文件 · 已用 m:ss` |

## 构建

```bat
build.bat            :: 自动找 VS 自带 cmake，配置 + 编译
:: 产物：build\Release\ddnet-module-installer.exe
:: 只产出 exe 单程序（mods\src\client 由首次运行自检创建）
```

## 使用

1. 双击 `ddnet-module-installer.exe`（单文件即可，首次运行会自动创建 mods\src\client）。
2. 选**游戏来源** → 点 `刷新版本` 拉取可用版本（网络在下方设置：见步骤 5）。
3. 在版本列表里选一个版本（默认选最新；想复现验证过的组合就选 TClient `10.9.0` 对应的 tag）。
4. 在**模块**列表里勾选要装的模块（默认勾第一个）。列表为 `模块 / 版本 / 说明 / DDNet / TClient`，后两列用 **✔ / ✖** 表示该模块在对应来源上是否验证过。
5. 调**网络**那一行：`国内镜像` + `使用代理`；`多线程下载` 与 `自动编译` 同排第二行。**方向只有一个**：一旦勾 `使用代理`，`国内镜像` 会被**自动取消并变灰**；取消代理时镜像恢复默认勾选。`使用代理` 这个勾选框**永远可点**。**安装位置不可选**：强制在本程序目录下的 `src\` 与 `client\<种类>-<版本>\`（成品，按客户端与版本分目录，互不覆盖）。
6. 点 `开始安装`，看日志跑完；完成后 `打开安装目录` 直接到该次安装的 `client\<种类>-<版本>\`，双击 `DDNet.exe`。

> **引入 NDM 的多线程下载功能**（<https://github.com/wr-sky/NDM>）：模块补丁、FFmpeg 这类大文件下载按设定的线程数并行拉取——每个线程用 HTTP `Range` 请求各自拉一段、失败自动从断点重试，最后按偏移拼回一个文件。实测同一个 GitHub release 包：单连接 ~13 MB/s，4 连接 ~25-38 MB/s。服务端不支持 `Range`（分段请求返回 200 而不是 206）或拿不到文件大小时会**自动退回单连接**，日志会写明。实现上要先手动把 GitHub 的 302 重定向走完再分段（WinInet 自动重定向会丢自定义请求头，Range 就失效了）。

## 单文件 mod（`.dmod`）
安装器只认 `<exe>\mods\*.dmod`（**消费者**）；`.dmod` 由模块项目产出（**生产者**，例如 `ddnet-background-module\pack-dmod.ps1`）。
两个项目**不共享源码目录**，接口就是 `.dmod` 文件本身。
- 把 `.dmod` **拖进窗口**即添加（复制到 `mods\`，同 `id` 自动替换旧版）；启动时自动扫描该目录。
- `.dmod` 是 zip 容器，内含自描述的 `module.json`（id / name / **version** / type / tested_on / verified / requires / verify_path / patch）+ `patch/module.patch` + 可选的 `files/`（模块源码副本，供手工安装参考）。
- 模块声明的依赖（例如 `ffmpeg8.1`）**由安装器自动满足**，不再是全局开关。
- 拖入时同 `id` 的旧版本会被替换；启动与拖入都会重新扫描 `mods\`。

**要自己写模块？** 完整字段说明、补丁生成方式、多基线（DDNet / TClient 各一份补丁）做法与发布前检查清单见 **[模块开发文档](MODULE-DEV.md)**。

## 已知边界

- 平台：**Windows x64**；GUI 用 Win32，无外部 UI 依赖。
- 网络设置不落盘：每次启动都是默认值（镜像开、代理关）。取消勾选镜像 = **本程序不再额外加镜像**；如果你机器上的 git 全局配置里已经配了镜像，git 子进程仍会按全局配置走——那不是本程序加的。
- 代理只作用于本程序的 HTTP 请求与它启动的 git 子进程（通过 `-c http.proxy=` / `-c https.proxy=`），不改系统或 git 的全局代理设置。
- **不勾代理时按 Windows 系统设置走**：本程序用 WinInet 的 PRECONFIG，所以你的系统代理开着（`设置 → 网络和 Internet → 代理`，Clash/FlClash 这类会写 `127.0.0.1:7890`）时，**即使不勾"使用代理"也会经它**。
- **失败会自动一路退回"真正绕过所有代理的直连"**：① 按界面设置（含系统代理）→ ② 镜像不支持该接口（`api.github.com` 不代）就去掉镜像 → ③ 还失败就 `INTERNET_OPEN_TYPE_DIRECT` 绕过系统代理再试一次。版本接口、模块补丁、FFmpeg 下载、git 克隆都适用；每一步都在日志里写明原因（版本接口那次回退只对本次运行生效，下一轮会重新读界面设置）。
- 模块类型目前实现了 `source-patch`；`file-drop`（纯文件投放，不需要 git）**尚未实现**。
- **取消会按进程树整棵终止**（`taskkill /T /F`）：git 会派生 `git-remote-https`/`index-pack` 等子进程，只杀直接子进程会留下它们握着 `.git` 里的句柄，导致清理删不掉；删除本身也会重试 5 次、最后用 `rmdir /s /q` 兜底。万一仍没删净，**下次安装会检测到"源码目录不完整"并自动删掉重新克隆**（判据：`.git` 存在 且 `HEAD` 可解析 且 `rev-parse --show-toplevel` 等于该目录本身——只看 `.git` 存在不够：下载中断留下的树 `HEAD=refs/heads/.invalid`，会让 `git apply --3way` 报一堆 `does not exist in index` **却返回退出码 0**、补丁静默不落地，最后只会报成"基线不匹配"）。
- 「更新」= 用新版本重装一次（旧的 `src-*` 目录保留，便于回退）；不做增量补丁。
- 自动编译需要本机有 MSVC + CMake + Rust；缺什么会在日志里报出来。`cmake` 不在 PATH 时（本机只有 VS 自带的），会自动按 `where` → 常见 VS/CMake 安装位置探测，日志写明实际用的路径。
- **中断残留的坏 zip 不会污染下一次安装**：多线程下载把文件预分配到最终大小再并行写分段，进程被杀/崩溃会留下"尺寸正常、内容半截"的文件——安装器复用 zip 前**先解压验证**，失败就删掉重下一次；单连接下载中断留下的半截文件也会被识别（`Got<Total`）并删除。
- **单文件复制不走 xcopy**：xcopy 对"单文件源 + 不存在的目标"会弹 `F/D` 交互提示，在无窗口子进程里没人回答就永久挂起（0% CPU 卡死整个流水线）——单文件一律 `CopyFileW`。
- **开发/自检**：`ddnet-module-installer.exe --download <url> <保存路径> [线程数] [限时秒] [--proxy 地址]` 跑一次纯下载并输出速度（结果同时写入 `<保存路径>.dl-result.txt`，本程序是 GUI 子系统没有 stdout，自动化读该文件）。

## 目录结构

```
ddnet-module-installer/
├─ src/core.{h,cpp}           HTTP(WinInet) / 极简 JSON / 进程 / 清单 / 状态 / 版本拉取
├─ src/installer.cpp          安装流水线（8 步，日志逐步输出）
├─ src/gui.cpp                Win32 界面 + 工作线程
└─ build.bat                  一键构建
```

---

# English

A **DDNet source fetch-and-build tool**: it pulls a chosen upstream source tree, applies selected modules before building, compiles, and assembles a ready-to-run client folder. C++ / Win32, single self-contained exe — no Qt, no .NET.

## What it does

- **Version list** from GitHub `releases` (falls back to `tags`). Mirrors and a local proxy are optional; most mirrors do not proxy `api.github.com`, so the version API retries directly and says so in the log.
- **Module driven**: modules are single-file `.dmod` packages (multi-select, drag-and-drop). Adding a module never requires rebuilding this program.
- **One click, ready to play**: fetch source → submodules → apply module patch → install FFmpeg 8.1 when a module requires it → CMake configure → build → assemble `client\<kind>-<version>\` (double-click to run).
- **Update check** via `install-state.json`; live log plus a persisted `installer.log`.

## Usage

1. Run `ddnet-module-installer.exe` (single file; `mods\src\client` are created on first run).
2. Pick the **game source**, click refresh, choose a version, tick the modules you want.
3. Adjust the **network** row if needed (mirror / proxy / multi-thread downloads). Multi-thread downloads (NDM-style segmented connections, see <https://github.com/wr-sky/NDM>) are on by default; the thread count can be changed mid-install and applies to the next file. Git clone and submodules do not use it — they are git's own connections.
4. Click install and watch the log; when it finishes, open the install directory and run `DDNet.exe`.

Install layout is fixed next to the exe: `mods\` (modules), `src\` (source + build cache), `client\<kind>-<version>\` (finished clients, one folder per kind and version).

## Known boundaries

- Windows x64 only; Win32 GUI, no external UI dependency.
- Network settings are not persisted: every launch starts from defaults. With no proxy ticked the program uses the Windows system proxy (WinInet PRECONFIG), so a running Clash/FlClash on `127.0.0.1:7890` is used even when the proxy box is unticked.
- Failures fall back step by step: the settings in the UI → drop the mirror → `INTERNET_OPEN_TYPE_DIRECT` (bypass everything), each step explained in the log.
- Only the `source-patch` module type is implemented; `file-drop` is not.
- Cancelling kills the whole process tree; leftovers are detected and re-cloned on the next run.
- Building requires MSVC + CMake + Rust on this machine; `cmake` is auto-detected when not on PATH.
