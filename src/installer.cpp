// ddnet-module-installer - install pipeline
#include "core.h"

#include <windows.h>

#include <chrono>
#include <cctype>
#include <cstring>
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
	// 子进程输出：先给"进度识别器"过一遍（git 的进度行 / 编译文件名行会被转成状态行），
	// 认领了就不要再原样刷日志（git 每 0.2s 刷一次，日志会爆）。
	std::function<bool(const std::string &)> HandleProcLine;
	auto Do = [&](const std::string &Exe, const std::vector<std::string> &Args, const std::string &Dir) {
		if(Log)
		{
			std::string Cmd = "  [cmd] " + Exe;
			for(const auto &A : Args)
				Cmd += (A.find(' ') == std::string::npos ? " " + A : " \"" + A + "\"");
			Log(LogLevel::Raw, Cmd);
		}
		return RunProcess(Exe, Args, Dir, [&](const std::string &Line) {
			if(HandleProcLine && HandleProcLine(Line))
				return;
			if(Log)
				Log(LogLevel::Raw, "  " + Line);
		}, pCancel);
	};
	// 网络：非空时给 git 命令加 -c 重写与代理（clone / fetch / submodule 都会走它）。
	// GitProxy 会在"代理不可用"时被自动关掉（见下面克隆失败的重试），避免代理没开就整装失败。
	bool GitProxy = !Opt.Proxy.empty();
	auto WithNet = [&](std::vector<std::string> Args) {
		// 低速保护：代理路由不稳时传输会长期停在几 KiB/s（实测两次都卡死在克隆 31%），
		// git 默认永不超时 → 挂死。低于 1KiB/s 持续 45 秒就让它失败，交给上层重试换新连接。
		Args.insert(Args.begin(), {"-c", "http.lowSpeedLimit=1000", "-c", "http.lowSpeedTime=45"});
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
	// 下载（模块补丁 / FFmpeg）：线程数 >1 时走 HTTP Range 分段多连接（NDM 式），
	// 不支持分段或分段失败会自动退回单连接；再失败则绕过所有代理直连重试一次。
	auto Download = [&](const std::string &Url, const std::string &Dest, const std::function<void(int64_t, int64_t)> &Cb) {
		auto Note = [&](const std::string &S) { LogAt(LogLevel::Info, S); };
		// 热线程数：安装进行中在输入框改数字，对下一个开始下载的文件立即生效；
		// 没有提示值（CLI 自检等场景）就用本次安装的快照
		int Threads = g_DlThreadsHint.load();
		if(Threads < 1)
			Threads = Opt.DownloadThreads;
		if(Threads < 1)
			Threads = 1;
		auto Once = [&]() {
			if(Threads > 1)
				return Net.DownloadFileMulti(Url, Dest, Threads, Cb, Note);
			return Net.DownloadFile(Url, Dest, Cb);
		};
		HttpResult R = Once();
		if(!R.Ok && !Net.Direct)
		{
			LogAt(LogLevel::Warn, "  下载失败（" + R.Error + "；" + (Net.Proxy.empty() ? "当前按系统代理设置走" : "当前代理 " + Net.Proxy) +
						      "）——绕过所有代理直连重试一次");
			Net.Proxy.clear();
			Net.Direct = true;
			GitProxy = false;
			R = Once();
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
	// 进度：百分比 + 一行状态（状态里带地址/速度）；Pct<0 表示"百分比未知"→ 进度条改滚动
	auto Report = [&](int Pct, const std::string &S) {
		if(Progress)
			Progress(Pct < 0 ? -1 : (Pct > 100 ? 100 : Pct), S);
		if(Log)
			Log(LogLevel::Raw, "  " + S);
	};

	// ---- 把 git 的实时进度映射到进度条 --------------------------------------
	// git 在管道下默认不打进度，所以调用处都要加 --progress；输出形如：
	//   Receiving objects:  45% (1234/2743), 12.34 MiB | 5.67 MiB/s
	//   Resolving deltas: 100% (200/200), done.
	struct GitProgT
	{
		int Base = 0, Span = 0;          // 映射到 [Base, Base+Span]
		std::string What;                // "下载源码" / "初始化子模块"
		std::chrono::steady_clock::time_point LastUi = std::chrono::steady_clock::now();
		bool Active() const { return Span > 0; }
	} Gp;
	int Compiled = 0;
	bool InCompile = false;
	std::chrono::steady_clock::time_point CompileT0 = std::chrono::steady_clock::now();

	HandleProcLine = [&](const std::string &Line) -> bool {
		if(Gp.Active())
		{
			const size_t PctPos = Line.find('%');
			if(PctPos != std::string::npos && PctPos > 0 && isdigit((unsigned char)Line[PctPos - 1]))
			{
				int P = 0;
				{
					size_t A = PctPos;
					while(A > 0 && isdigit((unsigned char)Line[A - 1]))
						--A;
					P = atoi(Line.substr(A, PctPos - A).c_str());
					if(P < 0)
						P = 0;
					if(P > 100)
						P = 100;
				}
				std::string Speed;
				const size_t Bar = Line.find('|');
				if(Bar != std::string::npos)
				{
					const size_t S = Line.find_first_not_of(" \t", Bar + 1);
					if(S != std::string::npos)
					{
						const size_t E = Line.find_first_of(",;", S);
						Speed = Line.substr(S, E == std::string::npos ? std::string::npos : E - S);
						// git 会把速度字段补空格对齐，去掉首尾空白
						const size_t A2 = Speed.find_first_not_of(" \t");
						const size_t E2 = Speed.find_last_not_of(" \t");
						Speed = (A2 == std::string::npos) ? "" : Speed.substr(A2, E2 - A2 + 1);
					}
				}
				auto Now = std::chrono::steady_clock::now();
				if(std::chrono::duration<double>(Now - Gp.LastUi).count() < 0.4)
					return true;   // 认领这行，但节流到 ~2.5 次/秒再刷新界面
				Gp.LastUi = Now;
				const int Overall = Gp.Base + Gp.Span * P / 100;
				char Buf[256];
				if(!Speed.empty())
					snprintf(Buf, sizeof(Buf), "%s：%d%% · %s · 整体 %d%%", Gp.What.c_str(), P, Speed.c_str(), Overall);
				else
					snprintf(Buf, sizeof(Buf), "%s：%d%% · 整体 %d%%", Gp.What.c_str(), P, Overall);
				Report(Overall, Buf);
				return true;
			}
			return false;
		}
		if(InCompile)
		{
			// MSBuild 每编译一个文件会单独打一行文件名（如 "  custom_background.cpp"）：
			// 编译没有可靠的百分比，就用"已编译 N 个文件 + 已用时间"+滚动的进度条，避免看起来卡死。
			const size_t Dot = Line.find_last_of('.');
			if(Dot == std::string::npos || Line.find(' ') != std::string::npos || Line.find('\t') != std::string::npos)
				return false;
			const std::string Ext = Line.substr(Dot);
			if(Ext != ".cpp" && Ext != ".c" && Ext != ".cc" && Ext != ".cxx")
				return false;
			++Compiled;
			auto Now = std::chrono::steady_clock::now();
			if(std::chrono::duration<double>(Now - Gp.LastUi).count() < 0.5)
				return true;
			Gp.LastUi = Now;
			const int Sec = (int)std::chrono::duration<double>(Now - CompileT0).count();
			char Buf[256];
			snprintf(Buf, sizeof(Buf), "编译 game-client：已编译 %d 个文件 · 已用 %d:%02d（编译没有百分比，进度条滚动＝在跑）",
				Compiled, Sec / 60, Sec % 60);
			Report(-1, Buf);
			return true;
		}
		return false;
	};

	// 源码目录是否"完整可用"：只看 .git 存在是不够的。
	// 下载中途被取消会留下 HEAD=refs/heads/.invalid、没有 index 的残缺树，
	// 之后 `git apply --3way` 会对每个文件打印 "does not exist in index"，
	// **却仍然返回退出码 0**、什么都不写 —— 补丁会静默不落地，最后报成"基线不匹配"。
	// 另外：若 exe 所在目录被别的 git 仓库包住，git 会往上找仓库，这里一并挡掉。
	auto TreeUsable = [&](const std::string &Dir) {
		if(!PathExists(JoinPath(Dir, ".git")))
			return false;
		if(!RunProcess(Opt.GitPath, {"rev-parse", "--verify", "--quiet", "HEAD"}, Dir, nullptr, nullptr).Ok())
			return false;
		ProcessResult T = RunProcess(Opt.GitPath, {"rev-parse", "--show-toplevel"}, Dir, nullptr, nullptr);
		if(!T.Ok())
			return false;
		std::string Top = T.Output, Want = Dir;
		while(!Top.empty() && (Top.back() == '\n' || Top.back() == '\r' || Top.back() == ' '))
			Top.pop_back();
		for(char &C : Top)
			if(C == '\\')
				C = '/';
		for(char &C : Want)
			if(C == '\\')
				C = '/';
		while(!Want.empty() && Want.back() == '/')
			Want.pop_back();
		return _stricmp(Top.c_str(), Want.c_str()) == 0;
	};

	if(Opt.Version.Ref.empty())
	{
		Error = "未选择版本";
		return false;
	}
	if(Log)
		Log(LogLevel::Info, std::string("  网络：") + (Net.MirrorPrefix.empty() ? "不用镜像" : "镜像 " + Net.MirrorPrefix) +
					 (Opt.Proxy.empty() ? "；不用代理（跟随 Windows 系统设置）" : "；代理 http://" + Opt.Proxy));
	if(Log)
		Log(LogLevel::Info, std::string("  下载线程：") + (Opt.DownloadThreads > 1
									  ? std::to_string(Opt.DownloadThreads) + "（HTTP Range 分段多连接）"
									  : "1（单连接）"));
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
	bool HaveTree = TreeUsable(Tree);
	if(HaveTree)
	{
		LogAt(LogLevel::Info, "  已存在完整源码目录，跳过克隆（如需换版本请删除该目录）");
	}
	else
	{
		if(PathExists(Tree))
		{
			LogAt(LogLevel::Warn, "  上次中断留下的源码目录不完整（HEAD 无效或不是独立的 git 仓库），已删除并重新克隆");
			RemoveTree(Tree);
		}
		Gp.Base = 5;
		Gp.Span = 10;
		Gp.What = "下载源码";
		Gp.LastUi = std::chrono::steady_clock::now();
		ProcessResult R = Do(Opt.GitPath, WithNet({"clone", "--progress", "--depth", "1", "--branch", Opt.Version.Ref, CloneUrl, Tree}), "");
		if(!R.Ok() && Cancelled())
		{
			CleanupPartial();
			Error = "用户已取消（本次下载已自动清理）";
			return false;
		}
		if(!R.Ok() && !Cancelled())
		{
			// 低速保护触发/传输被掐断：同样的设置重试一次，换新连接往往就过了
			// （代理软件在运行时直连反而更慢，所以先别降级）。
			LogAt(LogLevel::Warn, "  克隆中断（多半是代理/网络传输被掐断）——原设置重试一次（换新连接）");
			RemoveTree(Tree);
			R = Do(Opt.GitPath, WithNet({"clone", "--progress", "--depth", "1", "--branch", Opt.Version.Ref, CloneUrl, Tree}), "");
		}
		if(!R.Ok() && GitProxy)
		{
			// 多半是代理软件没在运行：关掉代理（镜像仍保留）再试一次，别让整个安装白白失败
			LogAt(LogLevel::Warn, "  克隆仍失败，而本次勾了代理 " + Opt.Proxy + " —— 关掉代理直连重试一次");
			GitProxy = false;
			RemoveTree(Tree);
			R = Do(Opt.GitPath, WithNet({"clone", "--progress", "--depth", "1", "--branch", Opt.Version.Ref, CloneUrl, Tree}), "");
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
				!Do(Opt.GitPath, WithNet({"fetch", "--progress", "--depth", "1", "origin", Opt.Version.Ref}), Tree).Ok() ||
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
	Gp.Span = 0;   // 源码阶段结束
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
		Gp.Base = 18;
		Gp.Span = 12;
		Gp.What = "下载子模块 ddnet-libs";
		Gp.LastUi = std::chrono::steady_clock::now();
		if(CheckCancel())
			{
				CleanupPartial();
				return false;
			}
		ProcessResult R = Do(Opt.GitPath, WithNet({"submodule", "update", "--progress", "--init", "--recursive"}), Tree);
		if(!R.Ok() && !Cancelled())
		{
			// 同克隆：传输被掐断时原设置重试一次（换新连接），别直接放弃
			LogAt(LogLevel::Warn, "  子模块初始化中断——原设置重试一次（换新连接）");
			R = Do(Opt.GitPath, WithNet({"submodule", "update", "--progress", "--init", "--recursive"}), Tree);
		}
		if(!R.Ok())
		{
			CleanupPartial();
			Error = Cancelled() ? "用户已取消（本次下载已自动清理）" : "子模块初始化失败";
			return false;
		}
		Gp.Span = 0;   // 子模块阶段结束
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
			if(!TreeUsable(Tree))
			{
				LogAt(LogLevel::Warn, "  源码目录不是完整可用的 git 检出（下载可能被中断）——已删除，再点一次「开始安装」会重新克隆");
				RemoveTree(Tree);
				Error = "源码目录不完整，已清理；请再点一次「开始安装」重新克隆";
				return false;
			}
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
				Error = "模块 " + M.Id + " 校验失败：补丁没落地（源码目录不完整，或模块与这个游戏版本的基线不匹配）";
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
		// 解压即验证：多线程下载把文件**预分配到最终大小**再并行写分段，进程被杀/崩溃
		// 会留下"尺寸正常、内容半截"的坏 zip（tar -t 只查目录测不出分段缺失），直接复用
		// 必然在解压时炸。所以"解压"本身就是验证：失败就删掉重下一次（用户要求中断不残留）。
		std::string Ex = JoinPath(WorkDir0, "ffmpeg-extract");
		for(int Attempt = 0; Attempt < 2 && !FfmpegInstalled; ++Attempt)
		{
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
					const std::string Th = Opt.DownloadThreads > 1 ? ("，" + std::to_string(Opt.DownloadThreads) + " 线程") : "";
					if(Total > 0)
						snprintf(Buf, sizeof(Buf), "下载 FFmpeg 8.1：%d%%（%.2f MB/s，已 %.1f/%.1f MB%s）%s",
							(int)(Got * 100 / Total), Speed, Got / 1048576.0, Total / 1048576.0, Th.c_str(), ShowUrl.c_str());
					else
						snprintf(Buf, sizeof(Buf), "下载 FFmpeg 8.1：已 %.1f MB（%.2f MB/s%s）%s", Got / 1048576.0, Speed, Th.c_str(), ShowUrl.c_str());
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
					break;
				}
			}
			RemoveTree(Ex);
			MakeDirs(Ex);
			ProcessResult T = Do("tar", {"-xf", Zip, "-C", Ex}, "");
			if(T.Ok())
			{
				FfmpegInstalled = true;
				break;
			}
			LogAt(LogLevel::Warn, Attempt == 0
				? "  ffmpeg zip 解压失败——多半是上次下载被中断留下的半截文件，删除后重新下载再试"
				: "  重新下载后仍解压失败，放弃 FFmpeg（不影响图片背景，视频会不可用）");
			DeleteFileW(Utf8ToWide(Zip).c_str());
		}
		if(FfmpegInstalled)
		{
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
			// DDNet 19 系（TClient 同）的约定：x64 的导入库与运行时 DLL 都放在
			// windows/lib64 —— FindFFMPEG.cmake 在这里 find_library，并把同目录的
			// avcodec-62.dll 等 5 个 DLL 列进 FFMPEG_COPY_FILES（configure 时 file(COPY)
			// 到输出目录）。装错位置（如 windows/lib）会在 configure 阶段报
			// "file COPY cannot find .../lib64/avcodec-62.dll"。
			std::string Lib64 = JoinPath(Libs, "windows\\lib64");
			MakeDirs(Lib64);
			CopyTree(JoinPath(Root, "lib"), Lib64, Error);
			MakeDirs(ThirdPartyDll);
			for(const char *Dll : {"avcodec-62.dll", "avformat-62.dll", "avutil-60.dll", "swresample-6.dll", "swscale-9.dll"})
			{
				std::string S = JoinPath(JoinPath(Root, "bin"), Dll);
				if(PathExists(S))
				{
					CopyTree(S, JoinPath(Lib64, Dll), Error);        // 给 configure 的 file(COPY) 用
					CopyTree(S, JoinPath(ThirdPartyDll, Dll), Error); // 给成品 client\ 目录兜底
				}
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
	// client\ 按「客户端种类-版本号」分子目录存放（用户要求）：tclient 与 ddnet、
	// 不同版本互不覆盖，如 client\tclient-V10.9.0 和 client\ddnet-19.1
	auto SanitizeRef = [](std::string S) {
		for(char &c : S)
			if(c == '/' || c == '\\' || c == ':' || c == '*' || c == '?' || c == '"' || c == '<' || c == '>' || c == '|')
				c = '-';
		return S;
	};
	std::string Dist = ClientDir0.empty() ? JoinPath(WorkDir0, "Client")
					  : JoinPath(ClientDir0, SanitizeRef(Opt.Source.Id + "-" + Opt.Version.Ref));
	bool Built = false;
	if(Opt.Build)
	{
		// cmake 可能不在 PATH（本机只有 VS 自带的 cmake）：
		// 默认值 "cmake" 时先 where 探测，失败再按常见 VS/CMake 安装位置找。
		std::string Cmake = Opt.CmakePath;
		if(Cmake == "cmake")
		{
			std::string Found;
			ProcessResult W = RunProcess("where", {"cmake"}, "", nullptr, nullptr);
			if(W.Ok() && !W.Output.empty())
			{
				Found = W.Output;
				size_t Pos = Found.find_first_of("\r\n");
				if(Pos != std::string::npos)
					Found = Found.substr(0, Pos);
				while(!Found.empty() && (Found.back() == ' ' || Found.back() == '\r' || Found.back() == '\n'))
					Found.pop_back();
			}
			if(Found.empty())
			{
				for(const char *Cand : {
					"C:\\Program Files\\Microsoft Visual Studio\\18\\Community\\Common7\\IDE\\CommonExtensions\\Microsoft\\CMake\\CMake\\bin\\cmake.exe",
					"C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\Common7\\IDE\\CommonExtensions\\Microsoft\\CMake\\CMake\\bin\\cmake.exe",
					"C:\\Program Files\\Microsoft Visual Studio\\2022\\Professional\\Common7\\IDE\\CommonExtensions\\Microsoft\\CMake\\CMake\\bin\\cmake.exe",
					"C:\\Program Files\\Microsoft Visual Studio\\2022\\Enterprise\\Common7\\IDE\\CommonExtensions\\Microsoft\\CMake\\CMake\\bin\\cmake.exe",
					"C:\\Program Files\\CMake\\bin\\cmake.exe"})
				{
					if(PathExists(Cand))
					{
						Found = Cand;
						break;
					}
				}
			}
			if(!Found.empty())
			{
				LogAt(LogLevel::Info, "  使用 cmake：" + Found);
				Cmake = Found;
			}
		}
		if(CheckCancel())
			{
				CleanupPartial();
				return false;
			}
		Step(StepNo++, "配置 CMake");
		Report(82, "配置 CMake…");
		ProcessResult C = Do(Cmake, {"-S", ".", "-B", "build", "-A", "x64", "-DVULKAN=OFF", "-DDOWNLOAD_GTEST=OFF"}, Tree);
		if(!C.Ok())
		{
			// 失败的 configure 会留下毒化的 CMakeCache.txt（半套变量），
			// 直接重跑大概率还是错。清掉 build 目录重试一次。
			LogAt(LogLevel::Warn, "  CMake 配置失败，清空 build 缓存后重试一次");
			RemoveTree(JoinPath(Tree, "build"));
			C = Do(Cmake, {"-S", ".", "-B", "build", "-A", "x64", "-DVULKAN=OFF", "-DDOWNLOAD_GTEST=OFF"}, Tree);
			if(!C.Ok())
			{
				Error = "CMake 配置失败（重试后仍失败；检查 MSVC/CMake 是否可用，日志里有详细错误）";
				return false;
			}
		}
		if(CheckCancel())
			{
				CleanupPartial();
				return false;
			}
		Step(StepNo++, "编译 game-client（耗时较长，请耐心）");
		Compiled = 0;
		CompileT0 = std::chrono::steady_clock::now();
		Gp.LastUi = CompileT0;
		InCompile = true;
		Report(-1, "编译 game-client：开始编译（编译没有百分比 → 进度条滚动表示在跑；按「取消」会终止编译器）");
		ProcessResult B = Do(Cmake, {"--build", "build", "--config", Opt.Config, "--target", "game-client", "--parallel", "4"}, Tree);
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
		InCompile = false;
		Built = true;
		Report(94, "编译完成，组装客户端…");

		// ---------------------------------------------------- 7. 组装 ------
		Step(StepNo++, "组装可直接运行目录 " + Dist);
		RemoveTree(Dist);
		MakeDirs(Dist);
		CopyTree(JoinPath(Tree, "build\\" + Opt.Config + "\\DDNet.exe"), JoinPath(Dist, "DDNet.exe"), Error);
		// 构建配置目录里编出来的运行时 DLL 一并带上（TClient 本地构建的 steam_api.dll
		// 就在这里；漏拷会启动报"找不到 steam_api.dll"）
		{
			int Copied = 0;
			std::string CErr;
			if(CollectRuntimeDlls(JoinPath(Tree, "build\\" + Opt.Config), Dist, Copied, CErr) && Copied > 0)
				LogAt(LogLevel::Info, "  从构建输出目录带上 " + std::to_string(Copied) + " 个运行时 DLL");
		}
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
		// 完整性校验（强校验，不过就不许报"完成"）：解析 DDNet.exe 的 PE 导入表，
		// 每个非系统 DLL 依赖都必须在 client\ 里——缺了就报安装中断并点名缺谁。
		{
			std::vector<std::string> Missing;
			std::string VErr;
			if(!VerifyExeImports(JoinPath(Dist, "DDNet.exe"), Dist, Missing, VErr))
			{
				if(!VErr.empty())
				{
					LogAt(LogLevel::Error, "  完整性校验无法进行：" + VErr);
					Error = "完整性校验失败：" + VErr;
				}
				else
				{
					std::string Miss;
					for(size_t i = 0; i < Missing.size() && i < 8; ++i)
						Miss += (i ? "、" : "") + Missing[i];
					LogAt(LogLevel::Error, "  完整性校验失败：client\\ 缺少运行时 DLL：" + Miss);
					Error = "完整性校验失败：缺少运行时 DLL：" + Miss;
				}
				return false;
			}
			LogAt(LogLevel::Info, "  完整性校验通过：DDNet.exe 的 DLL 依赖全部就绪");
		}
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
