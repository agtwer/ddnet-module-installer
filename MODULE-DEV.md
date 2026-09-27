# 模块开发文档（`.dmod`）

本文写给要为 **ddnet-module-installer** 开发模块的人。读完你应该能：做一个合法的 `.dmod`、让安装器正确识别它、并在目标上游上干净地打好补丁。

> 安装器本身见 <https://github.com/agtwer/ddnet-module-installer>。一个完整的真实样例（双基线、含新源文件与 FFmpeg 依赖）见 <https://github.com/agtwer/ddnet-background-module>。

---

## 1. 模块是什么

模块 = **一个 `.dmod` 文件**。安装器只认这个文件本身，**不与模块项目共享任何目录**：你把文件放进安装器的 `mods\`，或直接拖进窗口即可。

`.dmod` 是一个 **zip 容器**（用系统自带 `tar.exe` 解开，Win10+ 均有），内部结构：

```
module.json            必需，模块自描述（唯一事实来源）
patch/module.patch     默认补丁（source-patch 类型必需）
patch/<其它>.patch     可选的、给别的上游基线用的补丁
files/                 可选：新增源文件的副本（供人工安装参考）
```

打包方式随便（zip 后改名 `.dmod` 即可）；参考实现见模块项目的 `pack-dmod.ps1`。
**注意**：`Compress-Archive` 只接受 `.zip` 后缀，先压 `.zip` 再改名。

---

## 2. `module.json` 字段

安装器解析的字段如下（左侧为键名，取自 `src/core.cpp` 的实际读取逻辑）：

| 键 | 必需 | 类型 | 说明 |
|---|---|---|---|
| `id` | **是** | string | 模块唯一标识。为空则加载失败。同 `id` 的新版本会替换旧版 |
| `name` | 否 | string | 显示名，缺省用 `id` |
| `version` | 否 | string | 模块自身版本号，缺省 `"0.0.0"`。**改版本号会让安装器在复用的源码树上重打补丁**（见 §5） |
| `type` | 否 | string | `source-patch`（默认，已实现）或 `file-drop`（**尚未实现**） |
| `description` | 否 | string | 一句话说明，显示在模块列表的「说明」列 |
| `patch` | 否 | string | 默认补丁在容器内的相对路径，缺省 `patch/module.patch`。`source-patch` 类型下该文件必须存在 |
| `patches` | 否 | object | **多基线补丁**：`{"<来源id>": "<容器内路径>"}`，见 §4 |
| `patches` → 来源 id | — | string | 目前可用 `tclient`、`ddnet`（`core.cpp` 内置的两个来源 id） |
| `verify_path` | 否 | string | 打完补丁后**应当存在**的、相对源码树根的路径。用于校验补丁是否真的落地（强烈建议填） |
| `verified` | 否 | object | `{"<来源id>": true/false}`，在模块列表的对应来源列显示 ✔ / ✖ |
| `tested_on` | 否 | array | 例如 `["TaterClient/TClient@6b4118bf0"]`，纯展示用 |
| `requires` | 否 | array | 安装器据此自动满足依赖。当前识别 `ffmpeg8.1`（或 `ffmpeg`）→ 自动安装 FFmpeg 8.1；`git`/`cmake`/`msvc` 为提示性声明 |
| `base_hint` | 否 | string | 给人看的基线说明 |
| `docs_url` | 否 | string | 模块文档地址（安装器读取但仅作信息保留） |

其余字段（`format`、`format_version`、`upstream`、`diffstat`、`config_variables` 等）安装器**不解析**，你可以自由添加作为元信息。

### 最小可用示例

```json
{
  "id": "my-mod",
  "name": "我的模块",
  "version": "1.0",
  "type": "source-patch",
  "description": "一句话说明",
  "patch": "patch/module.patch",
  "verify_path": "src/game/client/components/my_component.cpp",
  "verified": { "ddnet": false, "tclient": false }
}
```

---

## 3. 补丁怎么被套用（决定你该怎么生成补丁）

安装器对源码树执行（`src/installer.cpp`）：

1. `git apply --3way --whitespace=nowarn <patch>`
2. 若失败（**在镜像/浅克隆下很常见**，因为 `--3way` 需要 blob 索引），退回 `git apply --reject --whitespace=nowarn <patch>`
3. 然后**统计实际留下的 `.rej` 文件数**判断是否套全 —— 不看退出码
4. 最后检查 `verify_path` 是否存在；不在就判定该模块安装失败

由此得出两条实践要求：

- **补丁要基于目标基线自身的提交生成**（用 `git diff` 出来的、带正确 `index` 行的补丁）；从别的分支拼出来的补丁在 `--3way` 下会整片失败。
- **`verify_path` 要指向补丁一定会创建或修改的文件**，这是防止"补丁静默没落地"的关键闸门。

生成方式（在目标源码树里改完后）：

```powershell
git add -A
git diff --cached --binary --output=patch/module.patch
```

`--binary` 让二进制文件（图片、字体）也能进补丁。

---

## 4. 多基线：一个模块同时支持 DDNet 官方与 TClient

不同上游的结构差异（源文件列表、设置页枚举、配置变量所在文件）往往大到无法用一个补丁通吃。做法是**每个基线各带一份补丁**：

```json
{
  "patch": "patch/module.patch",
  "patches": {
    "tclient": "patch/module.patch",
    "ddnet": "patch/ddnet.patch"
  }
}
```

- 安装器按当前选择的**来源 id** 取对应那份，日志会写明「使用 ddnet 专用补丁」；
- 找不到对应项时回退到默认 `patch`，并**在日志里告警**「可能套不上」；
- 为兼容起见，`patch`（单数）始终保留为回退项。

`patches` 的键要匹配安装器内置的来源 id。当前内置两个：`ddnet`（DDNet 官方）、`tclient`（TClient）。

---

## 5. 复用它已装好的源码树（重要）

安装器会复用 `src\` 下已存在的源码树，因此**同一棵树可能被多次安装**。为了避免补丁被重复叠加（表现为 `函数已有主体` 之类的重复定义编译错误），安装器会在源码树根写标记文件：

```
.dmod-applied-<id>.txt     内容 = 模块 version
```

行为：

- 标记存在且**版本号相同** → 跳过打补丁（复用）；
- 标记存在但**版本号不同** → 先反向套用旧补丁，再打新的；
- 标记在但 `verify_path` 不存在 → 视为上次没打全，删标记重打。

**给模块作者的含义**：改动模块内容后**务必递增 `version`**，否则安装器会认为"已经打过了"而跳过。

---

## 6. 其它需要知道的运行时行为

- **模块来源**：安装器只扫描 `<exe>\mods\*.dmod`。拖入即复制到该目录，同 `id` 替换旧版。提供「刷新模块」按钮重新扫描（无需重启）。
- **前台可见信息**：模块列表列为 `模块 / 版本 / 说明 / DDNet / TClient`，后两列由 `verified` 决定显示 ✔ 还是 ✖。
- **依赖自动满足**：`requires` 里声明 `ffmpeg8.1` 时，安装器会自动下载并安装 FFmpeg 8.1 到源码树的 `ddnet-libs/ffmpeg/windows/lib64`（DDNet 19 系约定）；若该树里已经装好（`lib64` 与 `include/libavcodec` 都在），会整步跳过。
- **产物目录**：成品组装到 `client\<来源id>-<版本>\`，每个来源与版本独立目录、互不覆盖。
- **取消与清理**：取消只删半成品（克隆到一半的源码树、解压临时目录、补丁临时文件），已下载完整的依赖包保留供下次复用。

---

## 7. 检查清单（发布前逐条过）

- [ ] `module.json` 有 `id`，且同 `id` 的旧版会被本版替换
- [ ] `version` 比上一版大（否则复用源码树时会跳过补丁）
- [ ] `patch`（或 `patches` 中对应来源的项）指向容器内真实存在的文件
- [ ] 补丁基于**目标基线自身的提交**用 `git diff` 生成
- [ ] 填了 `verify_path`，且该路径确实由补丁产生
- [ ] `verified` 如实填写（**没在某个来源上实测过就填 `false`**）
- [ ] 用了 FFmpeg 就在 `requires` 里声明 `ffmpeg8.1`
- [ ] 在**干净的目标源码树**上实测过一次完整安装（含编译）
