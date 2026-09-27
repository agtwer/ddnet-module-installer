// ddnet-module-installer - core engine (no external dependencies)
//
// 设计目标：任意版本的 DDNet 系客户端 + 任意数量的模块（源补丁 / 文件投放），
// 全部通过"清单（JSON）"驱动，所以增加模块或新增游戏来源都不需要重编译本程序。
#pragma once

#include <string>
#include <vector>
#include <map>
#include <functional>
#include <cstdint>
#include <atomic>

// ---------------------------------------------------------------- logging ---
enum class LogLevel { Info, Step, Warn, Error, Raw };
using LogFn = std::function<void(LogLevel, const std::string &)>;

// 程序版本号（安装器自身；模块各自有自己的 tested_on/verified 信息）
inline const char *kInstallerVersion = "1.2";

// ------------------------------------------------------------------- http ---
struct HttpResult
{
	bool Ok = false;
	int Status = 0;
	std::string Body;      // for GetString
	std::string Error;
};

struct Http
{
	// 国内镜像前缀（可为空）：形如 "https://ghproxy.net/"，
	// 会把 https://github.com/... 或 raw/api 地址变成 <prefix><原地址>
	std::string MirrorPrefix;
	std::atomic<bool> *pCancel = nullptr;   // 非空时：置 true 即中断下载
	// HTTP(S) 代理（可为空）：形如 "127.0.0.1:7890"；空 = 沿用系统设置
	std::string Proxy;
	// true = 绕过一切代理（含 Windows 系统代理）直连。回退重试时用：
	// 只清空 Proxy 是不够的——空 Proxy 走 PRECONFIG，仍然会跟随系统代理。
	bool Direct = false;
	std::string ApplyMirror(const std::string &Url) const;
	HttpResult GetString(const std::string &Url, int TimeoutSec = 30) const;
	// 下载到文件，带进度回调（已下载字节，总字节；总字节可能为 -1）
	HttpResult DownloadFile(const std::string &Url, const std::string &DestPath,
		const std::function<void(int64_t, int64_t)> &Progress) const;
	// 多线程分段下载（NDM 式）：Threads>=2 且服务端支持 HTTP Range 时开多个连接并行取分段，
	// 每段都能断点续传；不支持分段或分段失败会自动退回单连接。Note 用来说明实际发生了什么。
	HttpResult DownloadFileMulti(const std::string &Url, const std::string &DestPath, int Threads,
		const std::function<void(int64_t, int64_t)> &Progress,
		const std::function<void(const std::string &)> &Note) const;
};

// ------------------------------------------------------------------- json ---
// 极简 JSON（对象/数组/字符串/数字/真假/null），够用即可。
struct JsonValue
{
	enum class Type { Null, Bool, Number, String, Array, Object } T = Type::Null;
	bool Bool = false;
	double Number = 0;
	std::string Str;
	std::vector<JsonValue> Arr;
	std::vector<std::pair<std::string, JsonValue>> Obj;   // 保持顺序

	const JsonValue *Find(const std::string &Key) const;
	std::string GetString(const std::string &Key, const std::string &Default = "") const;
	double GetNumber(const std::string &Key, double Default = 0) const;
	bool GetBool(const std::string &Key, bool Default = false) const;
	const JsonValue *GetArray(const std::string &Key) const;
};

bool JsonParse(const std::string &Text, JsonValue &Out, std::string &Error);
std::string JsonEscape(const std::string &In);

// ---------------------------------------------------------------- process ---
struct ProcessResult
{
	int ExitCode = -1;
	std::string Output;    // stdout + stderr 合并（按行回调同时收集）
	bool Ok() const { return ExitCode == 0; }
};

// 运行子进程；每输出一行调用 OnLine（可为空）；pCancel 置 true 会终止子进程
// （返回 ExitCode = -2 表示被取消）
ProcessResult RunProcess(const std::string &Exe, const std::vector<std::string> &Args,
	const std::string &WorkDir, const std::function<void(const std::string &)> &OnLine,
	std::atomic<bool> *pCancel = nullptr);

// ------------------------------------------------------------------ model ---
struct GameSource
{
	std::string Id;          // "ddnet" / "tclient" / 自定义
	std::string Name;
	std::string Repo;        // "ddnet/ddnet"
};

struct GameVersion
{
	std::string Tag;         // 原始 tag，例如 "18.0.3" 或 "10.9.0"
	std::string Name;
	std::string PublishedAt;
	bool Prerelease = false;
	std::string Ref;         // 可用于 git clone / 下载的 ref（tag 或 commit）
};

struct ModuleInfo
{
	std::string Id;
	std::string Name;
	std::string Version;      // mod 自己的版本号（单文件 mod 里自带）
	std::string Description;
	std::string Type;            // "source-patch" | "file-drop"
	std::string PatchUrl;        // 远端补丁
	std::string PatchLocal;      // 相对程序目录的本地补丁（可空）
	std::string PatchPath;       // 已解析出的补丁绝对路径（来自 .dmod 时自动填）
	std::map<std::string, std::string> Patches;   // 按来源 id 的补丁: {"tclient":"patch/module.patch","ddnet":"patch/ddnet-background.patch"}
	std::string ResolvedPatch;   // 本次选用（已按来源 id 解析）的补丁相对路径
	std::string DmodPath;        // 来源 .dmod 文件（可空）
	std::string Origin;          // 人类可读来源：mods/xxx.dmod 或 modules/modules.json
	std::string DocsUrl;
	std::string BaseHint;        // 基线说明（给人看）
	std::vector<std::string> TestedOn;   // 例如 "TaterClient/TClient@6b4118bf0"
	// 适配版本：该模块在哪些"来源@版本"上做过适配（module.json 的 supported_versions）。
	// 形如 {"ddnet@19.9", "tclient@10.9.0"}；只写版本号或来源 id 也接受（宽松匹配）。
	std::vector<std::string> SupportedVersions;
	std::vector<std::string> Requires;   // "ffmpeg8.1" / "git" / "cmake" / "msvc"
	std::map<std::string, bool> Verified; // 按来源 id 的验证状态: {"tclient":true,"ddnet":false}
	std::string VerifyPath;      // 安装后应存在的相对路径（用于校验）
	bool Selected = false;
};

// 安装器读取的清单：来源 + 模块 + 远端索引
struct ModuleIndex
{
	int IndexVersion = 1;
	std::vector<GameSource> Sources;
	std::vector<ModuleInfo> Modules;
	std::vector<std::string> RemoteIndexes;   // 可再拉取的清单地址（扩展用）
};

bool LoadIndexFromString(const std::string &Json, ModuleIndex &Out, std::string &Error);
bool LoadIndexFromFile(const std::string &Path, ModuleIndex &Out, std::string &Error);
bool SaveIndexTemplate(const std::string &Path, std::string &Error);

// 单文件 mod（.dmod = 内含 module.json 与补丁的 zip 容器）
// 解到 <TempRoot>/<文件名> 下，并把补丁绝对路径填进 Out.PatchPath
bool LoadDmodFile(const std::string &DmodPath, const std::string &TempRoot, ModuleInfo &Out, std::string &Error);
bool IsDmodFile(const std::string &Path);
std::string FileNameOf(const std::string &Path);
std::string ReadFileText(const std::string &Path, bool &Ok);
bool WriteFileText(const std::string &Path, const std::string &Text);   // 用于源码树里的"已打补丁"标记

// ------------------------------------------------------------------- state ---
struct InstallState
{
	std::string SourceId;
	std::string Version;
	std::string WorkDir;
	std::string DistDir;
	std::vector<std::string> Modules;
	std::string Timestamp;
	bool Built = false;

	static bool Load(const std::string &Path, InstallState &Out);
	static bool Save(const std::string &Path, const InstallState &In);
};

// ----------------------------------------------------------------- install ---
struct InstallOptions
{
	std::string WorkDir;          // 源码与构建所在目录（默认 <根>\_work）
	std::string ClientDir;        // 最终可运行客户端目录（默认 <根>\Client，照 DDNet 默认结构）
	GameSource Source;
	GameVersion Version;
	std::vector<ModuleInfo> Modules;
	// 网络：镜像前缀走 Net.MirrorPrefix；这里只放代理（git 与 HTTP 都走它）
	std::string Proxy;            // 空 = 不用代理；否则形如 "127.0.0.1:7890"
	int DownloadThreads = 1;      // HTTP 下载线程数（1 = 单连接；>1 走 Range 分段多连接）
	bool SkipSubmodules = false;
	bool InstallFfmpeg = true;
	std::string FfmpegZip;        // 空则自动下载
	bool Build = true;
	// 调试安装：把游戏存档（settings.cfg/截图/背景等）留在游戏目录的 save\ 里，
	// 而不是系统 AppData。实现方式 = 组装时把 storage.cfg 的第一条（决定存档位置）
	// 从 $USERDIR 改成 save。
	bool DebugSaveInGameDir = false;
	std::string CmakePath = "cmake";
	std::string GitPath = "git";
	std::string Config = "Release";
};

struct Installer
{
	Http Net;
	LogFn Log;
	// 进度回调：百分比(0..100) + 一行状态（含正在下载的地址与速度）
	std::function<void(int, const std::string &)> Progress;
	std::atomic<bool> *pCancel = nullptr;   // 置 true 会尽快中断（步骤边界 / 下载中 / 子进程）
	std::string AppDir;           // 程序所在目录（放模块与清单）

	bool FetchVersions(const GameSource &Src, std::vector<GameVersion> &Out, std::string &Error);
	// 整条流水线；失败返回 false，Error 给原因（日志里已有细节）
	bool Run(const InstallOptions &Opt, InstallState &OutState, std::string &Error);
};

// ---------------------------------------------------------------- 小工具 -----
std::string NowStamp();
std::string JoinPath(const std::string &A, const std::string &B);
bool PathExists(const std::string &P);
bool IsDir(const std::string &P);
bool MakeDirs(const std::string &P);
bool CopyTree(const std::string &From, const std::string &To, std::string &Error);
bool RemoveTree(const std::string &P);
// 完整性校验：把构建输出目录（如 build\Release）里编出来的运行时 DLL 全部带上
//（TClient 本地构建的 steam_api.dll 就在那里，不带会导致启动报"找不到 steam_api.dll"）
bool CollectRuntimeDlls(const std::string &BuildDir, const std::string &Dist, int &Copied, std::string &Error);
// 完整性校验：解析 exe 的 PE 导入表，核对每个非系统 DLL 依赖是否在 Dir 里。
// 返回 true=全部就绪；false=内部错误（Error 说明）或有缺失（Missing 列出 DLL 名）。
bool VerifyExeImports(const std::string &ExePath, const std::string &Dir,
		      std::vector<std::string> &Missing, std::string &Error);
// 多线程下载的"热"线程数：安装进行中改 UI 输入框立即写这里，每次下载开始时读一次，
// 对下一个要下载的文件立即生效（已经在分的段落不变）。0 = 没有提示值，用本次安装的快照。
extern std::atomic<int> g_DlThreadsHint;
std::string ExeDir();
std::string WideToUtf8(const std::wstring &W);
std::wstring Utf8ToWide(const std::string &S);
