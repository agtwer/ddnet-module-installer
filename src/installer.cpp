// ddnet-module-installer - install pipeline
#include "core.h"

#include <windows.h>

#include <chrono>
#include <sstream>

namespace
{
	std::string StripQuotes(std::string S)
	{
		if(S.size() >= 2 && S.front() == '"' && S.back() == '"')
			return S.substr(1, S.size() - 2);
		return S;
	}
}

bool Installer::Run(const InstallOptions &Opt, InstallState &OutState, std::string &Error)
{
	auto LogAt = [&](LogLevel Lv, const std::string &S) {
		if(Log)
			Log(Lv, S);
	};
	auto Step = [&](int N, const std::string &S) {
		if(Log)
			Log(LogLevel::Step, "[" + std::to_string(N) + "] " + S);
	};
	auto Do = [&](const std::string &Exe, const std::vector<std::string> &Args, const std::string &Dir) {
		if(Log)
		{
			std::string Cmd = "  [cmd] " + Exe;
			for(const auto &A : Args)
				Cmd += (A.find(' ') == std::string::npos ? " " + A : " \"" + A + "\"");
			Log(LogLevel::Raw, Cmd);
		}
		return RunProcess(Exe, Args, Dir, [&](const std::string &Line) {
			if(Log)
				Log(LogLevel::Raw, "  " + Line);
		}, pCancel);
	};
	// 网络：非空时给 git 命令加 -c 重写与代理（clone / fetch / submodule 都会走它）。
	// GitProxy 会在"代理不可用"时被自动关掉（见下面克隆失败的重试），避免代理没开就整装失败。
	bool GitProxy = !Opt.Proxy.empty();
	auto WithNet = [&](std::vector<std::string> Args) {
		if(!Net.MirrorPrefix.empty())
			Args.insert(Args.begin(), {"-c", "url." + Net.MirrorPrefix + "https://github.com/.insteadOf=https://github.com/"});
		if(GitProxy)
		{
			const std::string P = "http://" + Opt.Proxy;
			Args.insert(Args.begin(), {"-c", "https.proxy=" + P});
			Args.insert(Args.begin(), {"-c", "http.proxy=" + P});
		}
		return Args;
	};
	// 下载（模块补丁 / FFmpeg）：失败时先怀疑代理不可用/被限流——绕过所有代理直连重试一次
	auto Download = [&](const std::string &Url, const std::string &Dest, const std::function<void(int64_t, int64_t)> &Cb) {
		HttpResult R = Net.DownloadFile(Url, Dest, Cb);
		if(!R.Ok && !Net.Direct)
		{
			LogAt(LogLevel::Warn, "  下载失败（" + R.Error + "；" + (Net.Proxy.empty() ? "当前按系统代理设置走" : "当前代理 " + Net.Proxy) +
						      "）——绕过所有代理直连重试一次");
			Net.Proxy.clear();
			Net.Direct = true;
			GitProxy = false;
			R = Net.DownloadFile(Url, Dest, Cb);
		}
		return R;
	};
	// 取消：步骤边界、下载中、子进程 三处都会检查
	auto Cancelled = [&]() { return pCancel && pCancel->load(); };
	auto CheckCancel = [&]() {
		if(Cancelled())
		{
			Error = "用户已取消（本次下载已自动清理）";
			return true;
		}
		return false;
	};
	// 进度：百分比 + 一行状态（状态里带地址/速度）
	auto Report = [&](int Pct, const std::string &S) {
		if(Progress)
			Progress(Pct < 0 ? 0 : (Pct > 100 ? 100 : Pct), S);
		if(Log)
			Log(LogLevel::Raw, "  " + S);
	};

	if(Opt.Version.Ref.empty())
	{
		Error = "未选择版本";
		return false;
	}
	if(Log)
		Log(LogLevel::Info, std::string("  网络：") + (Net.MirrorPrefix.empty() ? "不用镜像" : "镜像 " + Net.MirrorPrefix) +
					 (Opt.Proxy.empty() ? "；不用代理（跟随 Windows 系统设置）" : "；代理 http://" + Opt.Proxy));
	const std::string WorkDir0 = Opt.WorkDir.empty() ? JoinPath(AppDir, "src") : Opt.WorkDir;
	const std::string ClientDir0 = Opt.ClientDir.empty() ? JoinPath(AppDir, "client") : Opt.ClientDir;
	if(WorkDir0.empty())
	{
		Error = "未设置安装/工作目录";
		return false;
	}

	// ---------------------------------------------------------- 1. 目录 -----
	Step(1, "准备目录 " + WorkDir0);
	Report(2, "准备安装目录：" + WorkDir0);
	if(!MakeDirs(WorkDir0))
	{
		Error = "无法创建目录";
		return false;
	}
	const std::string Tree = JoinPath(WorkDir0, "src-" + Opt.Source.Id + "-" + Opt.Version.Ref);
	const std::string ThirdPartyDll = JoinPath(Tree, "thirdparty-dll");

	// 取消时：把「本次」产生的东西全部清掉（下载的源码/子模块、FFmpeg 包与解包目录、
	// 模块补丁临时文件），让取消之后回到干净状态——下一次安装会重新下载。
	auto CleanupPartial = [&]() {
		std::vector<std::string> Killed;
		std::vector<std::string> Failed;
		auto Kill = [&](const std::string &P) {
			if(P.empty() || !PathExists(P))
				return;
			if(RemoveTree(P) && !PathExists(P))
				Killed.push_back(P);
			else
				Failed.push_back(P);
		};
		Kill(Tree);
		Kill(JoinPath(WorkDir0, "ffmpeg-8.1.zip"));
		Kill(JoinPath(WorkDir0, "ffmpeg-extract"));
		for(const auto &M : Opt.Modules)
			Kill(JoinPath(WorkDir0, M.Id + ".patch"));
		// 若 _work 已空就一起收掉，别留空目录
		{
			WIN32_FIND_DATAW Fd;
			HANDLE H = FindFirstFileW(Utf8ToWide(JoinPath(WorkDir0, "*")).c_str(), &Fd);
			bool Empty = true;
			if(H != INVALID_HANDLE_VALUE)
			{
				do {
					std::string N = WideToUtf8(Fd.cFileName);
					if(N != "." && N != "..")
					{
						Empty = false;
						break;
					}
				} while(FindNextFileW(H, &Fd));
				FindClose(H);
			}
			if(Empty)
			{
				RemoveTree(WorkDir0);
				Killed.push_back(WorkDir0);
			}
		}
		if(Log)
		{
			Log(LogLevel::Info, "  已取消：自动清理本次下载与构建产物");
			for(const auto &K : Killed)
				Log(LogLevel::Info, "    已删除 " + K);
			if(Killed.empty())
				Log(LogLevel::Info, "    （本次没有产生可清理的文件）");
			for(const auto &P : Failed)
				Log(LogLevel::Warn, "    清理失败（可能被占用，可稍后手动删除）：" + P);
		}
	};

	// ---------------------------------------------------------- 2. 源码 -----
	Step(2, "获取源码 " + Opt.Source.Repo + " @ " + Opt.Version.Ref);
	Report(5, "获取源码：https://github.com/" + Opt.Source.Repo + " @ " + Opt.Version.Ref);
	if(CheckCancel())
		{
			CleanupPartial();
			return false;
		}
	std::string RepoUrl = "https://github.com/" + Opt.Source.Repo + ".git";
	std::string CloneUrl = Net.ApplyMirror(RepoUrl);
	bool HaveTree = PathExists(JoinPath(Tree, ".git"));
	if(HaveTree)
	{
		LogAt(LogLevel::Info, "  已存在源码目录，跳过克隆（如需换版本请删除该目录）");
	}
	else
	{
		ProcessResult R = Do(Opt.GitPath, WithNet({"clone", "--depth", "1", "--branch", Opt.Version.Ref, CloneUrl, Tree}), "");
		if(!R.Ok() && Cancelled())
		{
			CleanupPartial();
			Error = "用户已取消（本次下载已自动清理）";
			return false;
		}
		if(!R.Ok() && GitProxy)
		{
			// 多半是代理软件没在运行：关掉代理（镜像仍保留）再试一次，别让整个安装白白失败
			LogAt(LogLevel::Warn, "  克隆失败，而本次勾了代理 " + Opt.Proxy + " —— 若代理软件没开请先启动；现在先关掉代理重试一次");
			GitProxy = false;
			RemoveTree(Tree);
			R = Do(Opt.GitPath, WithNet({"clone", "--depth", "1", "--branch", Opt.Version.Ref, CloneUrl, Tree}), "");
		}
		if(!R.Ok())
		{
			LogAt(LogLevel::Warn, "  按 tag/分支浅克隆失败，尝试按提交号抓取…");
			RemoveTree(Tree);
			if(!MakeDirs(Tree))
			{
				Error = "无法创建源码目录";
				return false;
			}
			if(!Do(Opt.GitPath, {"init", "-q", "."}, Tree).Ok() || !Do(Opt.GitPath, {"remote", "add", "origin", CloneUrl}, Tree).Ok() ||
				!Do(Opt.GitPath, WithNet({"fetch", "--depth", "1", "origin", Opt.Version.Ref}), Tree).Ok() ||
				!Do(Opt.GitPath, {"checkout", "-q", "-B", "installer", "FETCH_HEAD"}, Tree).Ok())
			{
				CleanupPartial();
				Error = Cancelled() ? "用户已取消（本次下载已自动清理）" :
					(Opt.Proxy.empty() ? "克隆/抓取源码失败（网络或版本号无效）" :
							     "克隆/抓取源码失败（代理与直连都试过了：请确认代理软件在运行，或取消勾选代理）");
				return false;
			}
		}
	}
	LogAt(LogLevel::Info, "  源码: " + Tree);
	Report(15, "源码就绪：" + Tree);
	if(CheckCancel())
		{
			CleanupPartial();
			return false;
		}

	// ------------------------------------------------------ 3. 子模块 -------
	if(!Opt.SkipSubmodules)
	{
		Step(3, "初始化子模块（ddnet-libs，约 580MB，走镜像会快一些）");
		Report(18, "初始化子模块 ddnet-libs（约 580MB，可能较慢）");
		if(CheckCancel())
			{
				CleanupPartial();
				return false;
			}
		ProcessResult R = Do(Opt.GitPath, WithNet({"submodule", "update", "--init", "--recursive"}), Tree);
		if(!R.Ok())
		{
			CleanupPartial();
			Error = Cancelled() ? "用户已取消（本次下载已自动清理）" : "子模块初始化失败";
			return false;
		}
		Report(30, "子模块就绪");
	}
	else
		Step(3, "跳过子模块");

	// -------------------------------------------------------- 4. 模块 ------
	int StepNo = 4;
	size_t ModIdx = 0;
	for(const auto &M : Opt.Modules)
	{
		Step(StepNo++, "安装模块：" + M.Name + "  v" + (M.Version.empty() ? "?" : M.Version) + "（" + M.Id + "，来自 " + M.Origin + "）");
		Report(35 + (int)(20.0 * ModIdx / (double)(Opt.Modules.empty() ? 1 : Opt.Modules.size())), "应用模块 " + M.Name + " v" + M.Version);
		ModIdx++;
		if(CheckCancel())
			{
				CleanupPartial();
				return false;
			}
		if(M.Type == "source-patch")
		{
			std::string PatchPath = M.PatchPath;   // .dmod 已解开时是绝对路径
			if(PatchPath.empty() && !M.PatchLocal.empty())
			{
				std::string Local = JoinPath(AppDir, M.PatchLocal);
				if(PathExists(Local))
					PatchPath = Local;
			}
			if(PatchPath.empty() && !M.PatchUrl.empty())
			{
				std::string Tmp = JoinPath(WorkDir0, M.Id + ".patch");
				Report(56, "下载模块补丁：" + Net.ApplyMirror(M.PatchUrl));
				HttpResult D = Download(M.PatchUrl, Tmp, nullptr);
				if(!D.Ok)
				{
					if(Cancelled())
					{
						CleanupPartial();
						Error = "用户已取消（本次下载已自动清理）";
						return false;
					}
					Error = "补丁下载失败: " + D.Error;
					return false;
				}
				PatchPath = Tmp;
			}
			if(PatchPath.empty())
			{
				Error = "模块 " + M.Id + " 既没有本地补丁也下载不到远端补丁";
				return false;
			}
			ProcessResult A = Do(Opt.GitPath, {"apply", "--3way", "--whitespace=nowarn", PatchPath}, Tree);
			if(!A.Ok())
			{
				LogAt(LogLevel::Warn, "  3-way 套用失败，改用 --reject 半自动（能套的套上，其余留 .rej）");
				ProcessResult A2 = Do(Opt.GitPath, {"apply", "--reject", "--whitespace=nowarn", PatchPath}, Tree);
				if(!A2.Ok() && !A2.Output.empty())
					LogAt(LogLevel::Warn, "  仍有未套用的 hunk，请按模块文档的锚点表手工处理 .rej 文件");
				if(!A2.Ok() && PathExists(JoinPath(Tree, "CMakeLists.txt")) == false)
				{
					Error = "补丁套用失败且源码树异常";
					return false;
				}
			}
			if(!M.VerifyPath.empty() && !PathExists(JoinPath(Tree, M.VerifyPath)))
			{
				LogAt(LogLevel::Warn, "  校验失败：补丁声称会产生 " + M.VerifyPath + "，但文件不存在");
				Error = "模块 " + M.Id + " 校验失败（基线不匹配？）";
				return false;
			}
			LogAt(LogLevel::Info, "  模块已应用并校验通过");
		}
		else if(M.Type == "file-drop")
		{
			LogAt(LogLevel::Warn, "  file-drop 类型模块暂未实现（请改用 source-patch）");
		}
		else
		{
			LogAt(LogLevel::Warn, "  未知模块类型: " + M.Type);
		}
	}

	// ------------------------------------------------------- 5. FFmpeg -----
	bool FfmpegInstalled = false;
	if(Opt.InstallFfmpeg)
	{
		Step(StepNo++, "安装 FFmpeg 8.1（视频背景必需）");
		std::string Zip = Opt.FfmpegZip;
		if(Zip.empty())
			Zip = JoinPath(WorkDir0, "ffmpeg-8.1.zip");
		if(!PathExists(Zip))
		{
			std::string Url = "https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-n8.1-latest-win64-gpl-shared-8.1.zip";
			std::string ShowUrl = Net.ApplyMirror(Url);
			if(CheckCancel())
				{
					CleanupPartial();
					return false;
				}
			Report(60, "下载 FFmpeg 8.1：" + ShowUrl);
			int64_t LastBytes = 0;
			auto LastT = std::chrono::steady_clock::now();
			HttpResult D = Download(Url, Zip, [&](int64_t Got, int64_t Total) {
				auto Now = std::chrono::steady_clock::now();
				double Dt = std::chrono::duration<double>(Now - LastT).count();
				if(Dt < 0.5)
					return;
				double Speed = (double)(Got - LastBytes) / 1048576.0 / Dt;   // MB/s
				LastBytes = Got;
				LastT = Now;
				char Buf[512];
				if(Total > 0)
					snprintf(Buf, sizeof(Buf), "下载 FFmpeg 8.1：%d%%（%.2f MB/s，已 %.1f/%.1f MB）%s",
						(int)(Got * 100 / Total), Speed, Got / 1048576.0, Total / 1048576.0, ShowUrl.c_str());
				else
					snprintf(Buf, sizeof(Buf), "下载 FFmpeg 8.1：已 %.1f MB（%.2f MB/s）%s", Got / 1048576.0, Speed, ShowUrl.c_str());
				Report(60 + (int)(18.0 * (Total > 0 ? (double)Got / (double)Total : 0.0)), Buf);
			});
			if(!D.Ok)
			{
				if(Cancelled())
				{
					CleanupPartial();
					Error = "用户已取消（本次下载已自动清理）";
					return false;
				}
				LogAt(LogLevel::Warn, "  FFmpeg 下载失败：" + D.Error + "（不影响图片背景，视频会不可用；可从 BtbN/FFmpeg-Builds 手动下载后重试）");
			}
		}
		if(PathExists(Zip))
		{
			std::string Ex = JoinPath(WorkDir0, "ffmpeg-extract");
			RemoveTree(Ex);
			MakeDirs(Ex);
			ProcessResult T = Do("tar", {"-xf", Zip, "-C", Ex}, "");
			if(!T.Ok())
			{
				LogAt(LogLevel::Warn, "  tar 解压失败，改用 PowerShell Expand-Archive");
				Do("powershell", {"-NoProfile", "-Command", "Expand-Archive -LiteralPath '" + Zip + "' -DestinationPath '" + Ex + "' -Force"}, "");
			}
			// bsdtar 解压后通常有一层目录
			std::string Root = Ex;
			{
				WIN32_FIND_DATAW Fd;
				std::string Pattern = JoinPath(Ex, "*");
				HANDLE H = FindFirstFileW(Utf8ToWide(Pattern).c_str(), &Fd);
				int Dirs = 0, Files = 0;
				std::string FirstDir;
				if(H != INVALID_HANDLE_VALUE)
				{
					do {
						std::string Name = WideToUtf8(Fd.cFileName);
						if(Name == "." || Name == "..")
							continue;
						if(Fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
						{
							Dirs++;
							if(FirstDir.empty())
								FirstDir = Name;
						}
						else
							Files++;
					} while(FindNextFileW(H, &Fd));
					FindClose(H);
				}
				if(Dirs == 1 && Files == 0)
					Root = JoinPath(Ex, FirstDir);
			}
			const std::string Libs = JoinPath(Tree, "ddnet-libs\\ffmpeg");
			CopyTree(JoinPath(Root, "include"), JoinPath(Libs, "include"), Error);
			MakeDirs(JoinPath(Libs, "windows\\lib"));
			CopyTree(JoinPath(Root, "lib"), JoinPath(Libs, "windows\\lib"), Error);
			MakeDirs(ThirdPartyDll);
			for(const char *Dll : {"avcodec-62.dll", "avformat-62.dll", "avutil-60.dll", "swresample-6.dll", "swscale-9.dll"})
			{
				std::string S = JoinPath(JoinPath(Root, "bin"), Dll);
				if(PathExists(S))
					CopyTree(S, JoinPath(ThirdPartyDll, Dll), Error);
			}
			// touch 头文件：避免 CMake 认为无需重编而导致 ABI 错位崩溃（0xc0000409）
			{
				WIN32_FIND_DATAW Fd;
				std::string Pattern = JoinPath(JoinPath(Libs, "include"), "*");
				HANDLE H = FindFirstFileW(Utf8ToWide(Pattern).c_str(), &Fd);
				SYSTEMTIME St;
				GetLocalTime(&St);
				FILETIME Ft;
				SystemTimeToFileTime(&St, &Ft);
				if(H != INVALID_HANDLE_VALUE)
				{
					do {
						std::string Name = WideToUtf8(Fd.cFileName);
						if(Name == "." || Name == "..")
							continue;
						std::string P = JoinPath(JoinPath(Libs, "include"), Name);
						HANDLE Fh = CreateFileW(Utf8ToWide(P).c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
							nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
						if(Fh != INVALID_HANDLE_VALUE)
						{
							SetFileTime(Fh, nullptr, nullptr, &Ft);
							CloseHandle(Fh);
						}
					} while(FindNextFileW(H, &Fd));
					FindClose(H);
				}
			}
			FfmpegInstalled = true;
			LogAt(LogLevel::Info, "  FFmpeg 8.1 已就位（头文件时间戳已刷新，保证重编）");
		}
	}
	else
		Step(StepNo++, "跳过 FFmpeg（仅图片背景可用）");

	// -------------------------------------------------------- 6. 构建 ------
	std::string Dist = ClientDir0.empty() ? JoinPath(WorkDir0, "Client") : ClientDir0;
	bool Built = false;
	if(Opt.Build)
	{
		if(CheckCancel())
			{
				CleanupPartial();
				return false;
			}
		Step(StepNo++, "配置 CMake");
		Report(82, "配置 CMake…");
		ProcessResult C = Do(Opt.CmakePath, {"-S", ".", "-B", "build", "-A", "x64", "-DVULKAN=OFF", "-DDOWNLOAD_GTEST=OFF"}, Tree);
		if(!C.Ok())
		{
			Error = "CMake 配置失败（检查 MSVC/CMake 是否可用）";
			return false;
		}
		if(CheckCancel())
			{
				CleanupPartial();
				return false;
			}
		Step(StepNo++, "编译 game-client（耗时较长，请耐心）");
		Report(85, "编译 game-client（耗时较长；按「取消」会终止编译器）");
		ProcessResult B = Do(Opt.CmakePath, {"--build", "build", "--config", Opt.Config, "--target", "game-client", "--parallel", "4"}, Tree);
		if(!B.Ok())
		{
			if(Cancelled())
			{
				CleanupPartial();
				Error = "用户已取消（本次下载已自动清理）";
				return false;
			}
			Error = "编译失败（看日志最后几行；常见：缺 Rust/CMake 版本过低）";
			return false;
		}
		Built = true;
		Report(94, "编译完成，组装客户端…");

		// ---------------------------------------------------- 7. 组装 ------
		Step(StepNo++, "组装可直接运行目录 " + Dist);
		RemoveTree(Dist);
		MakeDirs(Dist);
		CopyTree(JoinPath(Tree, "build\\" + Opt.Config + "\\DDNet.exe"), JoinPath(Dist, "DDNet.exe"), Error);
		{
			WIN32_FIND_DATAW Fd;
			std::string Pattern = JoinPath(JoinPath(Tree, "build"), "*.dll");
			HANDLE H = FindFirstFileW(Utf8ToWide(Pattern).c_str(), &Fd);
			if(H != INVALID_HANDLE_VALUE)
			{
				do {
					std::string Name = WideToUtf8(Fd.cFileName);
					if(Name == "." || Name == "..")
						continue;
					CopyTree(JoinPath(JoinPath(Tree, "build"), Name), JoinPath(Dist, Name), Error);
				} while(FindNextFileW(H, &Fd));
				FindClose(H);
			}
		}
		if(PathExists(ThirdPartyDll))
			CopyTree(ThirdPartyDll, Dist, Error);
		CopyTree(JoinPath(Tree, "data"), JoinPath(Dist, "data"), Error);
		if(PathExists(JoinPath(Tree, "storage.cfg")))
			CopyTree(JoinPath(Tree, "storage.cfg"), JoinPath(Dist, "storage.cfg"), Error);
		LogAt(LogLevel::Info, "  完成：双击 " + JoinPath(Dist, "DDNet.exe"));
	}
	else
		Step(StepNo++, "跳过构建（源码已就绪，可自行 cmake 构建）");

	// -------------------------------------------------------- 8. 状态 ------
	OutState.SourceId = Opt.Source.Id;
	OutState.Version = Opt.Version.Ref;
	OutState.WorkDir = WorkDir0;
	OutState.DistDir = Dist;
	OutState.Built = Built;
	OutState.Timestamp = NowStamp();
	OutState.Modules.clear();
	for(const auto &M : Opt.Modules)
		OutState.Modules.push_back(M.Id + "@" + (M.Version.empty() ? "?" : M.Version));
	InstallState::Save(JoinPath(AppDir, "install-state.json"), OutState);
	Step(StepNo++, std::string("完成") + (FfmpegInstalled ? "（含视频支持）" : "（未装 FFmpeg：仅图片）"));
	Report(100, std::string("完成") + (FfmpegInstalled ? "（含视频支持）" : "（未装 FFmpeg：仅图片）") + " → " + Dist);
	return true;
}
