// ddnet-module-installer - Win32 GUI (no external UI dependencies)
#include "core.h"

#include <windows.h>
#include <commctrl.h>
#include <shellapi.h>
#include <shlobj.h>

#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "comctl32.lib")

namespace
{
	const int IDC_SOURCE = 1001, IDC_REFRESH = 1002, IDC_VERSIONS = 1003, IDC_MODULES = 1004;
	const int IDC_WORKDIR = 1005, IDC_BROWSE = 1006, IDC_MIRROR = 1007;
	const int IDC_BUILD = 1009, IDC_INSTALL = 1010, IDC_CANCEL = 1011, IDC_OPENDIR = 1012;
	const int IDC_CLEAN = 1023, IDC_REFMODS = 1024;   // 一键清理 src\ / 刷新 mods\ 模块列表
	const int IDC_PROGRESS = 1013, IDC_LOG = 1014, IDC_STATUS = 1015, IDC_UPDATE = 1016;
	const int IDC_SRCLIST = 1017;
	const int IDC_MIRRORHOST = 1018, IDC_PROXY = 1019, IDC_PROXYADDR = 1020;
	const int IDC_MT = 1021, IDC_MTCOUNT = 1022;   // 多线程下载（NDM 式分段多连接）

	const UINT WM_APP_LOG = WM_APP + 1;
	const UINT WM_APP_DONE = WM_APP + 2;
	const UINT WM_APP_PROGRESS = WM_APP + 3;
	const UINT WM_APP_BUSY = WM_APP + 4;

	std::atomic<bool> g_CancelFlag{false};
	HWND g_hProgText;

	ModuleIndex g_Index;
	std::vector<GameVersion> g_Versions;
	Installer g_Installer;
	InstallState g_State;
	bool g_HasState = false;
	std::thread g_Worker;
	bool g_Busy = false;
	bool g_Cancel = false;
	std::string g_LogFile;

	HWND g_hMain, g_hSource, g_hRefresh, g_hVersions, g_hModules, g_hWorkDir, g_hBrowse, g_hMirror;
	HWND g_hMirrorHost, g_hProxy, g_hProxyAddr;
	HWND g_hMt, g_hMtCount;   // 多线程下载
	HWND g_hBuild, g_hInstall, g_hCancel, g_hOpenDir, g_hProgress, g_hLog, g_hStatus, g_hUpdate;
	HWND g_hClean, g_hRefMods;
	HWND g_hLabel1, g_hLabel2, g_hLabel3;

	const wchar_t *CHECK = L"\u2714";   // ✔
	const wchar_t *CROSS = L"\u2716";   // ✖

	void SetStatus(const std::string &S) { SetWindowTextW(g_hStatus, Utf8ToWide(S).c_str()); }
	void WriteHostNote(const std::string &S) { if(g_hStatus) SetStatus(S); }

	void AppendLog(const std::string &Line)
	{
		if(!g_LogFile.empty())
		{
			FILE *F = nullptr;
			if(fopen_s(&F, g_LogFile.c_str(), "ab") == 0 && F)
			{
				std::string L = Line + "\r\n";
				fwrite(L.data(), 1, L.size(), F);
				fclose(F);
			}
		}
		int Len = GetWindowTextLengthW(g_hLog);
		SendMessageW(g_hLog, EM_SETSEL, Len, Len);
		std::string S = Line + "\r\n";
		SendMessageW(g_hLog, EM_REPLACESEL, FALSE, (LPARAM)Utf8ToWide(S).c_str());
	}

	void LogBridge(LogLevel Lv, const std::string &S)
	{
		std::string *P = new std::string(S);
		PostMessageW(g_hMain, WM_APP_LOG, (WPARAM)Lv, (LPARAM)P);
	}

	// 进度桥：百分比 + 状态行（含正在下载的地址与速度）
	void ProgressBridge(int Pct, const std::string &S)
	{
		std::string *P = new std::string(S);
		PostMessageW(g_hMain, WM_APP_PROGRESS, (WPARAM)Pct, (LPARAM)P);
	}

	std::string GetText(HWND H)
	{
		int Len = GetWindowTextLengthW(H);
		std::wstring W(Len + 1, 0);
		GetWindowTextW(H, &W[0], Len + 1);
		W.resize(Len);
		return WideToUtf8(W);
	}

	void SetBusy(bool Busy)
	{
		g_Busy = Busy;
		EnableWindow(g_hInstall, !Busy);
		EnableWindow(g_hRefresh, !Busy);
		EnableWindow(g_hUpdate, !Busy);
		EnableWindow(g_hClean, !Busy);     // 安装进行中不能删 src\（正在用）
		EnableWindow(g_hRefMods, !Busy);
		EnableWindow(g_hCancel, Busy);
		SendMessageW(g_hProgress, PBM_SETMARQUEE, FALSE, 0);   // 每轮都从"确定式"开始
		if(Busy)
		{
			g_CancelFlag = false;
			SendMessageW(g_hProgress, PBM_SETRANGE32, 0, 100);
			SendMessageW(g_hProgress, PBM_SETMARQUEE, FALSE, 0);   // 默认确定式（有 PBS_MARQUEE 风格才能切）
			SendMessageW(g_hProgress, PBM_SETPOS, 0, 0);
			SetWindowTextW(g_hProgText, L"");
		}
		EnableWindow(g_hProgText, TRUE);
	}

	// --------------------------------------------------------------- 填充 ----
	// 扫描 mods\*.dmod 并合并进模块列表（替换同 id 旧版）。启动与「刷新模块」共用。
	int ScanModsFolder()
	{
		std::string ModsDir = JoinPath(ExeDir(), "mods");
		MakeDirs(ModsDir);
		std::string CacheDir = JoinPath(ExeDir(), "_mods");
		int LoadedMods = 0;
		WIN32_FIND_DATAW Fd;
		HANDLE Hf = FindFirstFileW(Utf8ToWide(JoinPath(ModsDir, "*.dmod")).c_str(), &Fd);
		if(Hf != INVALID_HANDLE_VALUE)
		{
			do {
				std::string Name = WideToUtf8(Fd.cFileName);
				if(Name == "." || Name == "..")
					continue;
				ModuleInfo M;
				std::string Err;
				if(LoadDmodFile(JoinPath(ModsDir, Name), CacheDir, M, Err))
				{
					bool Replaced = false;
					for(auto &X : g_Index.Modules)
						if(X.Id == M.Id)
						{
							X = M;
							Replaced = true;
							break;
						}
					if(!Replaced)
						g_Index.Modules.push_back(M);
					LoadedMods++;
				}
				else
					AppendLog("[x] 单文件 mod 加载失败 " + Name + "：" + Err);
			} while(FindNextFileW(Hf, &Fd));
			FindClose(Hf);
		}
		return LoadedMods;
	}

	// 递归统计目录大小（给"清理源码"报告释放了多少空间）
	uint64_t DirSizeBytes(const std::string &Dir)
	{
		uint64_t Total = 0;
		WIN32_FIND_DATAW Fd;
		HANDLE H = FindFirstFileW(Utf8ToWide(JoinPath(Dir, "*")).c_str(), &Fd);
		if(H == INVALID_HANDLE_VALUE)
			return 0;
		do {
			std::string Name = WideToUtf8(Fd.cFileName);
			if(Name == "." || Name == "..")
				continue;
			if(Fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)
				Total += DirSizeBytes(JoinPath(Dir, Name));
			else
				Total += ((uint64_t)Fd.nFileSizeHigh << 32) | Fd.nFileSizeLow;
		} while(FindNextFileW(H, &Fd));
		FindClose(H);
		return Total;
	}

	void WorkerCleanSrc()
	{
		SetBusy(true);
		std::string SrcDir = JoinPath(g_Installer.AppDir, "src");
		if(!PathExists(SrcDir))
		{
			LogBridge(LogLevel::Info, "src\\ 不存在，没有可清理的内容");
			PostMessageW(g_hMain, WM_APP_DONE, 3, 0);
			return;
		}
		LogBridge(LogLevel::Info, "正在清理源码与构建产物（文件很多，可能要几秒到几十秒）…");
		uint64_t Bytes = DirSizeBytes(SrcDir);
		char Buf[64];
		snprintf(Buf, sizeof(Buf), "%.2f GB", Bytes / 1073741824.0);
		bool Ok = RemoveTree(SrcDir);
		MakeDirs(SrcDir);   // 目录保留（固定布局：src\ 由安装流程继续使用）
		if(Ok)
			LogBridge(LogLevel::Info, std::string("已清理 src\\（源码 + 构建，释放约 ") + Buf + "）；下次安装会重新克隆");
		else
			LogBridge(LogLevel::Error, "清理 src\\ 没删干净（可能有文件被占用，可关闭占用程序后再点一次）");
		PostMessageW(g_hMain, WM_APP_DONE, 3, 0);
	}

	void FillSources()
	{
		SendMessageW(g_hSource, CB_RESETCONTENT, 0, 0);
		for(const auto &S : g_Index.Sources)
			SendMessageW(g_hSource, CB_ADDSTRING, 0, (LPARAM)Utf8ToWide(S.Name).c_str());
		SendMessageW(g_hSource, CB_SETCURSEL, 0, 0);
	}

	void FillVersions()
	{
		SendMessageW(g_hVersions, LB_RESETCONTENT, 0, 0);
		for(const auto &V : g_Versions)
		{
			std::string Line = V.Tag;
			if(!V.PublishedAt.empty())
				Line += "   (" + V.PublishedAt.substr(0, 10) + ")";
			if(V.Prerelease)
				Line += "   [pre]";
			SendMessageW(g_hVersions, LB_ADDSTRING, 0, (LPARAM)Utf8ToWide(Line).c_str());
		}
		if(!g_Versions.empty())
			SendMessageW(g_hVersions, LB_SETCURSEL, 0, 0);
	}

	void FillModules()
	{
		ListView_DeleteAllItems(g_hModules);
		for(size_t i = 0; i < g_Index.Modules.size(); ++i)
		{
			const auto &M = g_Index.Modules[i];
			std::wstring Name = Utf8ToWide(M.Name);
			LVITEMW It = {};
			It.mask = LVIF_TEXT;
			It.iItem = (int)i;
			It.pszText = &Name[0];
			ListView_InsertItem(g_hModules, &It);

			std::wstring Ver = Utf8ToWide(M.Version.empty() ? "?" : M.Version);
			ListView_SetItemText(g_hModules, (int)i, 1, &Ver[0]);

			std::string DescS = M.Description;
			if(!M.Origin.empty())
				DescS += "   [" + M.Origin + "]";
			std::wstring Desc = Utf8ToWide(DescS);
			ListView_SetItemText(g_hModules, (int)i, 2, &Desc[0]);

			auto Cell = [&](const char *SrcId) {
				auto It2 = M.Verified.find(SrcId);
				bool VerOk = It2 != M.Verified.end() && It2->second;
				return Utf8ToWide(std::string(VerOk ? "✔" : "✖"));
			};
			std::wstring Dd = Cell("ddnet");
			std::wstring Tc = Cell("tclient");
			ListView_SetItemText(g_hModules, (int)i, 3, &Dd[0]);
			ListView_SetItemText(g_hModules, (int)i, 4, &Tc[0]);

			ListView_SetCheckState(g_hModules, (int)i, i == 0 ? TRUE : FALSE);
		}
	}

	GameSource SelectedSource()
	{
		int Sel = (int)SendMessageW(g_hSource, CB_GETCURSEL, 0, 0);
		if(Sel < 0 || Sel >= (int)g_Index.Sources.size())
			return g_Index.Sources.empty() ? GameSource{"ddnet", "DDNet", "ddnet/ddnet"} : g_Index.Sources[0];
		return g_Index.Sources[Sel];
	}

	std::vector<ModuleInfo> SelectedModules()
	{
		std::vector<ModuleInfo> Out;
		for(int i = 0; i < (int)g_Index.Modules.size(); ++i)
			if(ListView_GetCheckState(g_hModules, i))
				Out.push_back(g_Index.Modules[i]);
		return Out;
	}

	bool ModulesNeedFfmpeg(const std::vector<ModuleInfo> &Mods)
	{
		for(const auto &M : Mods)
			for(const auto &R : M.Requires)
				if(R == "ffmpeg8.1" || R == "ffmpeg")
					return true;
		return false;
	}

	// ------------------------------------------------------------ 网络选项 --
	std::string CtlText(HWND H)
	{
		wchar_t Buf[512] = {};
		if(H)
			GetWindowTextW(H, Buf, 512);
		return WideToUtf8(Buf);
	}

	std::string TrimStr(const std::string &S)
	{
		size_t A = S.find_first_not_of(" \t\r\n");
		if(A == std::string::npos)
			return "";
		size_t B = S.find_last_not_of(" \t\r\n");
		return S.substr(A, B - A + 1);
	}

	// 镜像主机名：勾选了才用；允许用户在下拉框里改或自填（会去掉 http(s):// 与结尾斜杠）
	std::string SelectedMirrorHost()
	{
		if(SendMessageW(g_hMirror, BM_GETCHECK, 0, 0) != BST_CHECKED)
			return "";
		std::string H = TrimStr(CtlText(g_hMirrorHost));
		if(H.rfind("https://", 0) == 0)
			H = H.substr(8);
		else if(H.rfind("http://", 0) == 0)
			H = H.substr(7);
		while(!H.empty() && H.back() == '/')
			H.pop_back();
		return H;
	}

	// 代理：勾选了才用；默认 127.0.0.1:7890
	std::string SelectedProxy()
	{
		if(SendMessageW(g_hProxy, BM_GETCHECK, 0, 0) != BST_CHECKED)
			return "";
		std::string P = TrimStr(CtlText(g_hProxyAddr));
		if(P.rfind("https://", 0) == 0)
			P = P.substr(8);
		else if(P.rfind("http://", 0) == 0)
			P = P.substr(7);
		while(!P.empty() && P.back() == '/')
			P.pop_back();
		return P;
	}

	// 把界面上的网络选项写进引擎（所有网络动作开工前都要调一次）
	void ApplyNetSettings()
	{
		const std::string Host = SelectedMirrorHost();
		g_Installer.Net.MirrorPrefix = Host.empty() ? "" : ("https://" + Host + "/");
		g_Installer.Net.Proxy = SelectedProxy();
	}

	bool CtlChecked(HWND H) { return SendMessageW(H, BM_GETCHECK, 0, 0) == BST_CHECKED; }

	// 用户要求：一旦开始用代理，就**强制关掉并变灰**"国内镜像"；关掉代理时镜像恢复默认开。
	// 注意方向只有一个：镜像勾选框会被代理灰掉，但**代理勾选框永远可点**
	//（否则默认镜像开着时用户根本勾不到代理）。ProxyClicked = 这次是代理勾选框被点。
	void SyncNetControls(bool ProxyClicked)
	{
		if(ProxyClicked && CtlChecked(g_hProxy))
			SendMessageW(g_hMirror, BM_SETCHECK, BST_UNCHECKED, 0);   // 用代理 → 强制关镜像
		else if(ProxyClicked && !CtlChecked(g_hProxy))
			SendMessageW(g_hMirror, BM_SETCHECK, BST_CHECKED, 0);     // 关掉代理 → 镜像回到默认开

		const bool Px = CtlChecked(g_hProxy);
		const bool Mi = CtlChecked(g_hMirror);
		EnableWindow(g_hMirror, Px ? FALSE : TRUE);
		EnableWindow(g_hMirrorHost, (Mi && !Px) ? TRUE : FALSE);
		EnableWindow(g_hProxy, TRUE);
		EnableWindow(g_hProxyAddr, Px ? TRUE : FALSE);
	}

	// 多线程下载热生效（用户要求"能热改线程数"）：勾选状态或输入框一变就写全局提示值，
	// 安装工作线程每次开始下载一个文件时读它——改完对下一个文件立即生效。
	// 每次实际变化都写日志（用户要求：切换线程的操作要记录在案）。
	void UpdateHotThreads()
	{
		int T = 1;
		if(SendMessageW(g_hMt, BM_GETCHECK, 0, 0) == BST_CHECKED)
		{
			T = atoi(CtlText(g_hMtCount).c_str());
			if(T < 2)
				T = 2;
			if(T > 16)
				T = 16;
		}
		int Old = g_DlThreadsHint.load();
		if(T == Old)
			return;
		g_DlThreadsHint.store(T);
		char Buf[160];
		if(T > 1)
			snprintf(Buf, sizeof(Buf), "[i] 下载线程数切换：%d → %d（热生效：下一个开始下载的文件用 %d 线程）", Old, T, T);
		else
			snprintf(Buf, sizeof(Buf), "[i] 下载线程数切换：%d → 单连接（热生效：下一个开始下载的文件不用多线程）", Old);
		AppendLog(Buf);
	}

	// 安装进行中改动镜像/代理：写日志存证（本次运行仍用开始时的快照，下一轮才生效）
	void LogNetToggleDuringInstall()
	{
		if(!g_Busy)
			return;
		AppendLog("[i] 安装进行中切换了镜像/代理（已记录在案）——本次运行继续用安装开始时的设置（见上方「开始安装」行），改动对下一轮安装生效");
	}

	std::string NetSummary()
	{
		const std::string Host = SelectedMirrorHost();
		const std::string Px = SelectedProxy();
		return std::string(Host.empty() ? "不用镜像" : "镜像 " + Host) + (Px.empty() ? "；不用代理（跟随系统设置）" : "；代理 " + Px);
	}

	// -------------------------------------------------------------- 工作线程 --
	void WorkerRefresh()
	{
		SetBusy(true);
		g_Versions.clear();
		ApplyNetSettings();
		GameSource S = SelectedSource();
		LogBridge(LogLevel::Step, "拉取版本信息：" + S.Name + "（" + S.Repo + "；" + NetSummary() + "）");
		std::string Err;
		bool Ok = g_Installer.FetchVersions(S, g_Versions, Err);
		if(!Ok)
			LogBridge(LogLevel::Error, "失败：" + Err + "（可换镜像、或勾上/取消代理后重试；勾了代理请先确认代理软件在运行）");
		else
			LogBridge(LogLevel::Info, "共 " + std::to_string(g_Versions.size()) + " 个版本");
		PostMessageW(g_hMain, WM_APP_DONE, 1, 0);
	}

	void WorkerUpdateCheck()
	{
		SetBusy(true);
		ApplyNetSettings();
		if(!g_HasState)
			LogBridge(LogLevel::Warn, "本机还没有安装记录（install-state.json），先安装一次再看更新");
		else
		{
			LogBridge(LogLevel::Step, "检查更新：本机已装 " + g_State.SourceId + " @" + g_State.Version + "（" + NetSummary() + "）");
			GameSource S;
			for(const auto &X : g_Index.Sources)
				if(X.Id == g_State.SourceId)
					S = X;
			if(S.Repo.empty())
				LogBridge(LogLevel::Warn, "找不到对应来源，无法比对");
			else
			{
				std::vector<GameVersion> Vs;
				std::string Err;
				if(g_Installer.FetchVersions(S, Vs, Err))
				{
					std::string Latest = Vs.empty() ? "" : Vs[0].Tag;
					if(Latest == g_State.Version)
						LogBridge(LogLevel::Info, "已是最新：" + Latest);
					else
						LogBridge(LogLevel::Info, "可更新：" + g_State.Version + " → " + Latest + "（在上方列表选新版本后重新安装；模块会一并重装）");
				}
				else
					LogBridge(LogLevel::Error, "检查更新失败：" + Err);
			}
		}
		PostMessageW(g_hMain, WM_APP_DONE, 2, 0);
	}

	void WorkerInstall()
	{
		SetBusy(true);
		ApplyNetSettings();
		LogBridge(LogLevel::Info, "网络：" + NetSummary());
		InstallOptions Opt;
		// 强制固定布局（用户不可选）：全部在本程序目录下
		Opt.WorkDir = JoinPath(g_Installer.AppDir, "src");      // 拉取的源码 + 构建
		Opt.ClientDir = JoinPath(g_Installer.AppDir, "client"); // 构建好的客户端
		Opt.Source = SelectedSource();
		int V = (int)SendMessageW(g_hVersions, LB_GETCURSEL, 0, 0);
		if(V < 0 || V >= (int)g_Versions.size())
		{
			LogBridge(LogLevel::Error, "请先在右侧/下方列表里选择一个版本");
			PostMessageW(g_hMain, WM_APP_DONE, 3, 0);
			return;
		}
		Opt.Version = g_Versions[V];
		Opt.Build = SendMessageW(g_hBuild, BM_GETCHECK, 0, 0) == BST_CHECKED;
		Opt.Modules = SelectedModules();
		// FFmpeg 不再是独立开关：由所选模块的 requires 决定（background 需要 ffmpeg8.1）
		Opt.InstallFfmpeg = ModulesNeedFfmpeg(Opt.Modules);
		// 代理也要带给 git（clone / fetch / submodule 都是子进程）
		Opt.Proxy = g_Installer.Net.Proxy;
		// 多线程下载：勾选才用；线程数取下拉框里的值（2~16，填别的会被夹到范围内）
		Opt.DownloadThreads = 1;
		if(SendMessageW(g_hMt, BM_GETCHECK, 0, 0) == BST_CHECKED)
		{
			Opt.DownloadThreads = atoi(CtlText(g_hMtCount).c_str());
			if(Opt.DownloadThreads < 2)
				Opt.DownloadThreads = 2;
			if(Opt.DownloadThreads > 16)
				Opt.DownloadThreads = 16;
		}
		g_DlThreadsHint.store(Opt.DownloadThreads);   // 热线程数以本次安装开始时的值为起点

		LogBridge(LogLevel::Step, "开始安装：" + Opt.Source.Name + " @" + Opt.Version.Ref +
					      "，模块 " + std::to_string(Opt.Modules.size()) + " 个；网络：" + NetSummary() +
					      "；下载线程：" + std::to_string(Opt.DownloadThreads));
		if(Opt.Modules.empty())
			LogBridge(LogLevel::Warn, "没有勾选任何模块：只会装一份干净的客户端源码");
		if(Opt.InstallFfmpeg)
			LogBridge(LogLevel::Info, "所选模块需要 FFmpeg 8.1（视频背景），安装流程会自动装上");
		LogBridge(LogLevel::Info, "客户端将装到 " + Opt.ClientDir + " ；源码与构建在 " + Opt.WorkDir);

		std::string Err;
		bool Ok = g_Installer.Run(Opt, g_State, Err);
		if(Ok)
		{
			g_HasState = true;
			LogBridge(LogLevel::Info, "全部完成 ✔");
		}
		else if(g_CancelFlag.load())
			LogBridge(LogLevel::Warn, "已取消：本次下载与构建产物已自动清理（下次安装需重新下载）");
		else
			LogBridge(LogLevel::Error, "安装中断：" + Err);
		PostMessageW(g_hMain, WM_APP_DONE, 3, 0);
	}

	void StartWorker(void (*Fn)())
	{
		if(g_Busy)
			return;
		if(g_Worker.joinable())
			g_Worker.join();
		g_Worker = std::thread(Fn);
		g_Worker.detach();
	}

	// ------------------------------------------------------------------ 布局 --
	// 两行设置区，严格不重叠；所有坐标由宽度算出
	void Layout(HWND H, int W, int Hh)
	{
		const int M = 12;
		const int ColW = (W - M * 3) / 2;          // 左列宽度
		const int LeftX = M;
		const int RightX = M * 2 + ColW;

		int Y = M;
		MoveWindow(g_hLabel1, LeftX, Y, ColW, 18, TRUE);
		MoveWindow(g_hSource, LeftX, Y + 20, ColW - 92, 200, TRUE);
		MoveWindow(g_hRefresh, LeftX + ColW - 88, Y + 20, 88, 24, TRUE);
		MoveWindow(g_hVersions, LeftX, Y + 50, ColW, 226, TRUE);

		MoveWindow(g_hLabel2, RightX, Y, ColW, 18, TRUE);
		MoveWindow(g_hModules, RightX, Y + 20, ColW, 256, TRUE);

		// 设置区：第一行 = 镜像（勾选 + 地址可选）/ 代理（勾选 + 地址）；第二行 = 自动编译
		int Y2 = Y + 292;
		MoveWindow(g_hLabel3, LeftX, Y2, W - M * 2, 18, TRUE);
		MoveWindow(g_hMirror, LeftX, Y2 + 22, 100, 24, TRUE);
		MoveWindow(g_hMirrorHost, LeftX + 104, Y2 + 22, 200, 200, TRUE);
		MoveWindow(g_hProxy, LeftX + 316, Y2 + 22, 90, 24, TRUE);
		MoveWindow(g_hProxyAddr, LeftX + 410, Y2 + 23, 170, 22, TRUE);
		MoveWindow(g_hBuild, LeftX, Y2 + 54, 110, 24, TRUE);
		MoveWindow(g_hMt, LeftX + 122, Y2 + 54, 110, 24, TRUE);
		MoveWindow(g_hMtCount, LeftX + 240, Y2 + 54, 80, 200, TRUE);   // 可编辑下拉：2/4/8/16 或自填

		// 按钮 + 进度
		int Y3 = Y2 + 94;
		MoveWindow(g_hInstall, LeftX, Y3, 110, 28, TRUE);
		MoveWindow(g_hCancel, LeftX + 118, Y3, 80, 28, TRUE);
		MoveWindow(g_hUpdate, LeftX + 206, Y3, 90, 28, TRUE);
		MoveWindow(g_hOpenDir, LeftX + 304, Y3, 110, 28, TRUE);
		MoveWindow(g_hClean, LeftX + 422, Y3, 92, 28, TRUE);
		MoveWindow(g_hRefMods, LeftX + 522, Y3, 92, 28, TRUE);
		MoveWindow(g_hProgress, LeftX + 622, Y3 + 2, W - M * 2 - 622, 18, TRUE);
		MoveWindow(g_hProgText, LeftX + 622, Y3 + 22, W - M * 2 - 622, 16, TRUE);

		int Y4 = Y3 + 44;
		int LogH = Hh - Y4 - 52;
		if(LogH < 60)
			LogH = 60;
		MoveWindow(g_hLog, LeftX, Y4, W - M * 2, LogH, TRUE);
		MoveWindow(g_hStatus, LeftX, Hh - 36, W - M * 2, 22, TRUE);
	}
}

LRESULT CALLBACK WndProc(HWND H, UINT Msg, WPARAM W, LPARAM L)
{
	switch(Msg)
	{
	case WM_CREATE: {
		HFONT Font = (HFONT)GetStockObject(DEFAULT_GUI_FONT);

		g_hLabel1 = CreateWindowW(L"STATIC", L"游戏来源与版本（点「刷新版本」拉取最新）", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, H, nullptr, nullptr, nullptr);
		g_hSource = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWNLIST | WS_VSCROLL, 0, 0, 0, 0, H, (HMENU)(INT_PTR)IDC_SOURCE, nullptr, nullptr);
		g_hRefresh = CreateWindowW(L"BUTTON", L"刷新版本", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, H, (HMENU)(INT_PTR)IDC_REFRESH, nullptr, nullptr);
		g_hVersions = CreateWindowW(L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | LBS_NOTIFY, 0, 0, 0, 0, H, (HMENU)(INT_PTR)IDC_VERSIONS, nullptr, nullptr);

		g_hLabel2 = CreateWindowW(L"STATIC", L"模块（勾选要装的，可多选；✔ 已验证该基线 / ✖ 未验证）", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, H, nullptr, nullptr, nullptr);
		g_hModules = CreateWindowExW(0, WC_LISTVIEWW, L"", WS_CHILD | WS_VISIBLE | WS_BORDER | LVS_REPORT, 0, 0, 0, 0, H, (HMENU)(INT_PTR)IDC_MODULES, nullptr, nullptr);
		ListView_SetExtendedListViewStyle(g_hModules, LVS_EX_CHECKBOXES | LVS_EX_FULLROWSELECT | LVS_EX_GRIDLINES);
		{
			LVCOLUMNW C = {};
			C.mask = LVCF_TEXT | LVCF_WIDTH;
			C.pszText = (LPWSTR)L"模块";
			C.cx = 150;
			ListView_InsertColumn(g_hModules, 0, &C);
			C.pszText = (LPWSTR)L"版本";
			C.cx = 60;
			ListView_InsertColumn(g_hModules, 1, &C);
			C.pszText = (LPWSTR)L"说明";
			C.cx = 230;
			ListView_InsertColumn(g_hModules, 2, &C);
			C.pszText = (LPWSTR)L"DDNet";
			C.cx = 50;
			ListView_InsertColumn(g_hModules, 3, &C);
			C.pszText = (LPWSTR)L"TClient";
			C.cx = 50;
			ListView_InsertColumn(g_hModules, 4, &C);
		}

		g_hLabel3 = CreateWindowW(L"STATIC", L"安装布局（强制、固定在本程序目录下）：mods\\ 放 mod，src\\ 放拉取的源码与构建，client\\<种类>-<版本>\\ 放构建好的客户端（按客户端与版本分目录，互不覆盖）；FFmpeg 由模块依赖自动决定", WS_CHILD | WS_VISIBLE, 0, 0, 0, 0, H, nullptr, nullptr, nullptr);
		g_hMirror = CreateWindowW(L"BUTTON", L"国内镜像", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 0, 0, H, (HMENU)(INT_PTR)IDC_MIRROR, nullptr, nullptr);
		// 镜像地址可选：下拉里给两个实测能用的，也可以直接改/自填（可编辑下拉框）
		g_hMirrorHost = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWN | WS_VSCROLL | CBS_AUTOHSCROLL, 0, 0, 0, 0, H, (HMENU)(INT_PTR)IDC_MIRRORHOST, nullptr, nullptr);
		SendMessageW(g_hMirrorHost, CB_ADDSTRING, 0, (LPARAM)L"gh.meali.top");
		SendMessageW(g_hMirrorHost, CB_ADDSTRING, 0, (LPARAM)L"ghproxy.net");
		SendMessageW(g_hMirrorHost, CB_ADDSTRING, 0, (LPARAM)L"hk.gh-proxy.org");
		SetWindowTextW(g_hMirrorHost, L"gh.meali.top");
		g_hProxy = CreateWindowW(L"BUTTON", L"使用代理", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 0, 0, H, (HMENU)(INT_PTR)IDC_PROXY, nullptr, nullptr);
		g_hProxyAddr = CreateWindowW(L"EDIT", L"127.0.0.1:7890", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL, 0, 0, 0, 0, H, (HMENU)(INT_PTR)IDC_PROXYADDR, nullptr, nullptr);
		g_hBuild = CreateWindowW(L"BUTTON", L"自动编译", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 0, 0, H, (HMENU)(INT_PTR)IDC_BUILD, nullptr, nullptr);
		// 多线程下载（NDM 式）：勾选后用 HTTP Range 分段多连接拉取大文件（FFmpeg 等）
		g_hMt = CreateWindowW(L"BUTTON", L"多线程下载", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX, 0, 0, 0, 0, H, (HMENU)(INT_PTR)IDC_MT, nullptr, nullptr);
		g_hMtCount = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | CBS_DROPDOWN | WS_VSCROLL | CBS_AUTOHSCROLL, 0, 0, 0, 0, H, (HMENU)(INT_PTR)IDC_MTCOUNT, nullptr, nullptr);
		for(const wchar_t *N : {L"2", L"4", L"8", L"16"})
			SendMessageW(g_hMtCount, CB_ADDSTRING, 0, (LPARAM)N);
		SetWindowTextW(g_hMtCount, L"4");

		g_hInstall = CreateWindowW(L"BUTTON", L"开始安装", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON, 0, 0, 0, 0, H, (HMENU)(INT_PTR)IDC_INSTALL, nullptr, nullptr);
		g_hCancel = CreateWindowW(L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, H, (HMENU)(INT_PTR)IDC_CANCEL, nullptr, nullptr);
		g_hUpdate = CreateWindowW(L"BUTTON", L"检查更新", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, H, (HMENU)(INT_PTR)IDC_UPDATE, nullptr, nullptr);
		g_hOpenDir = CreateWindowW(L"BUTTON", L"打开安装目录", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, H, (HMENU)(INT_PTR)IDC_OPENDIR, nullptr, nullptr);
		g_hClean = CreateWindowW(L"BUTTON", L"清理源码", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, H, (HMENU)(INT_PTR)IDC_CLEAN, nullptr, nullptr);
		g_hRefMods = CreateWindowW(L"BUTTON", L"刷新模块", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON, 0, 0, 0, 0, H, (HMENU)(INT_PTR)IDC_REFMODS, nullptr, nullptr);
		g_hProgress = CreateWindowExW(0, PROGRESS_CLASSW, L"", WS_CHILD | WS_VISIBLE | PBS_MARQUEE, 0, 0, 0, 0, H, (HMENU)(INT_PTR)IDC_PROGRESS, nullptr, nullptr);
		g_hProgText = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS, 0, 0, 0, 0, H, nullptr, nullptr, nullptr);
		g_hLog = CreateWindowW(L"EDIT", L"", WS_CHILD | WS_VISIBLE | WS_BORDER | WS_VSCROLL | ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL, 0, 0, 0, 0, H, (HMENU)(INT_PTR)IDC_LOG, nullptr, nullptr);
		g_hStatus = CreateWindowW(L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT, 0, 0, 0, 0, H, (HMENU)(INT_PTR)IDC_STATUS, nullptr, nullptr);

		for(HWND Ctl : {g_hSource, g_hRefresh, g_hVersions, g_hModules, g_hMirror, g_hMirrorHost, g_hProxy, g_hProxyAddr, g_hBuild, g_hMt, g_hMtCount,
			     g_hInstall, g_hCancel, g_hUpdate, g_hOpenDir, g_hClean, g_hRefMods, g_hLog, g_hStatus, g_hLabel1, g_hLabel2, g_hLabel3, g_hProgText})
			SendMessageW(Ctl, WM_SETFONT, (WPARAM)Font, TRUE);

		CheckDlgButton(H, IDC_MIRROR, BST_CHECKED);
		CheckDlgButton(H, IDC_BUILD, BST_CHECKED);
		CheckDlgButton(H, IDC_MT, BST_CHECKED);
		EnableWindow(g_hMtCount, TRUE);
		AppendLog("[i] 多线程下载：默认开（4 线程，可改 2/4/8/16 或自己填）；服务端不支持 HTTP Range 会自动退回单连接");
		SyncNetControls(false);   // 初始：镜像开、代理关（代理勾选框始终可点）
		AppendLog("[i] 网络选项：镜像 gh.meali.top（下拉可选 ghproxy.net / hk.gh-proxy.org，也可改成别的）与代理 127.0.0.1:7890 是两条替代路线——**勾了代理会自动关掉并变灰镜像**，关掉代理时镜像恢复默认开");
		AppendLog("[i] 注意：不勾代理时按 Windows 系统设置走——系统代理开着（如 Clash 的 127.0.0.1:7890）就会经它；被限流/不可用时会自动绕过所有代理直连重试");
		DragAcceptFiles(H, TRUE);   // 支持把 .dmod 拖进窗口添加
		SetStatus(std::string("v") + kInstallerVersion + " 就绪");
		AppendLog(std::string("[i] ddnet-module-installer v") + kInstallerVersion);

		std::string Idx1 = JoinPath(ExeDir(), "modules\\modules.json");
		std::string Idx2 = JoinPath(ExeDir(), "modules.json");
		std::string Err;
		bool Loaded = false;
		if(PathExists(Idx1))
			Loaded = LoadIndexFromFile(Idx1, g_Index, Err);
		if(!Loaded && PathExists(Idx2))
			Loaded = LoadIndexFromFile(Idx2, g_Index, Err);
		if(!Loaded)
		{
			AppendLog("[i] 未发现旧式模块清单（不需要；mod 请用单文件 .dmod 拖入）");
		// 内置来源：即使没有任何清单文件，DDNet / TClient 也必须永远可选
		if(g_Index.Sources.empty())
		{
			g_Index.Sources.push_back({"tclient", "TClient", "TaterClient/TClient"});
			g_Index.Sources.push_back({"ddnet", "DDNet 官方", "ddnet/ddnet"});
			AppendLog("[i] 使用内置来源：TClient / DDNet 官方");
		}
		}
		else
			AppendLog("[i] 已加载清单：来源 " + std::to_string(g_Index.Sources.size()) + " 个，模块 " + std::to_string(g_Index.Modules.size()) + " 个");

		// ---- 启动自检：强制布局（只出一个 exe，三个目录首次运行时自动创建）----
		{
			AppendLog("[i] 自检：程序 " + ExeDir());
			const char *apDirs[] = {"mods", "src", "client"};
			const char *apWhy[] = {"存放 .dmod 模块", "存放拉取的源码与构建", "存放构建好的客户端"};
			for(int i = 0; i < 3; ++i)
			{
				std::string P = JoinPath(ExeDir(), apDirs[i]);
				bool Existed = IsDir(P);
				if(!Existed)
					MakeDirs(P);
				AppendLog(std::string("      ") + apDirs[i] + "\\ " + (Existed ? "已存在" : "已创建") + " —— " + apWhy[i] + "  " + P);
			}
			std::string ExePath = JoinPath(ExeDir(), "ddnet-module-installer.exe");
			AppendLog(std::string("[i] 主程序：") + (PathExists(ExePath) ? "就位" : "文件名与预期不同（不影响使用）"));
		}

		// 单文件 mod：<exe>\mods\*.dmod（也可以直接把 .dmod 拖进本窗口添加）
		{
			int LoadedMods = ScanModsFolder();
			if(LoadedMods > 0)
				AppendLog("[i] 已加载单文件 mod " + std::to_string(LoadedMods) + " 个（" + JoinPath(ExeDir(), "mods") + "\\*.dmod）");
			else
				AppendLog("[i] 还没有单文件 mod —— 把 .dmod 文件直接拖进本窗口即可添加");
		}

		FillSources();
		FillModules();
		StartWorker(WorkerRefresh);

		g_HasState = InstallState::Load(JoinPath(ExeDir(), "install-state.json"), g_State);
		if(g_HasState)
		{
			AppendLog("[i] 检测到已安装记录：" + g_State.SourceId + " @" + g_State.Version + "（" + g_State.Timestamp + "）");
			if(!g_State.WorkDir.empty())
				SetWindowTextW(g_hWorkDir, Utf8ToWide(g_State.WorkDir).c_str());
		}
		return 0;
	}
	case WM_SIZE:
		Layout(H, LOWORD(L), HIWORD(L));
		return 0;
	case WM_APP_LOG: {
		std::unique_ptr<std::string> P((std::string *)L);
		LogLevel Lv = (LogLevel)W;
		std::string Prefix = Lv == LogLevel::Error ? "[x] " : (Lv == LogLevel::Warn ? "[!] " : (Lv == LogLevel::Step ? "" : "    "));
		AppendLog(Lv == LogLevel::Step ? P->c_str() : (Prefix + *P).c_str());
		if(Lv == LogLevel::Step)
			SetStatus(*P);
		return 0;
	}
	case WM_APP_BUSY:
		SetBusy(W != 0);
		return 0;
	case WM_APP_PROGRESS: {
		std::unique_ptr<std::string> P((std::string *)L);
		if((int)W < 0)
		{
			// 百分比未知的长步骤（如编译）：用滚动条明确表示"在干活"，而不是让进度条僵住
			SendMessageW(g_hProgress, PBM_SETMARQUEE, TRUE, 0);
		}
		else
		{
			SendMessageW(g_hProgress, PBM_SETMARQUEE, FALSE, 0);
			SendMessageW(g_hProgress, PBM_SETPOS, (WPARAM)W, 0);
		}
		SetWindowTextW(g_hProgText, Utf8ToWide(*P).c_str());
		return 0;
	}
	case WM_APP_DONE: {
		SetBusy(false);
		if(W == 1)
			FillVersions();
		SetStatus("就绪");
		return 0;
	}
	case WM_COMMAND: {
		int Id = LOWORD(W);
		if(Id == IDC_INSTALL || Id == IDC_REFRESH) WriteHostNote(Id == IDC_INSTALL ? "收到点击：开始安装" : "收到点击：刷新版本");
		if(Id == IDC_REFRESH)
			StartWorker(WorkerRefresh);
		else if(Id == IDC_CLEAN)
			StartWorker(WorkerCleanSrc);
		else if(Id == IDC_REFMODS)
		{
			// 手动刷新 mods\：清掉来自 mods\ 的条目再重扫（modules.json 来的不受影响）
			for(size_t i = g_Index.Modules.size(); i-- > 0;)
				if(g_Index.Modules[i].Origin.rfind("mods\\", 0) == 0)
					g_Index.Modules.erase(g_Index.Modules.begin() + i);
			int Loaded = ScanModsFolder();
			FillModules();
			if(Loaded > 0)
				AppendLog("[i] 模块列表已刷新：从 mods\\ 加载 " + std::to_string(Loaded) + " 个 .dmod");
			else
				AppendLog("[i] 模块列表已刷新：mods\\ 里当前没有 .dmod");
		}
		else if(Id == IDC_UPDATE)
			StartWorker(WorkerUpdateCheck);
		else if(Id == IDC_INSTALL)
			StartWorker(WorkerInstall);
		else if(Id == IDC_CANCEL)
		{
			g_CancelFlag = true;   // 真正的取消：安装流程会在步骤边界/下载中/子进程处检查
			EnableWindow(g_hCancel, FALSE);
			AppendLog("[i] 已请求取消：正在结束当前下载/子进程，本次已下载内容将自动清理");
			SetStatus("正在取消…");
		}
		else if(Id == IDC_MIRROR && HIWORD(W) == BN_CLICKED)
		{
			SyncNetControls(false);
			LogNetToggleDuringInstall();
		}
		else if(Id == IDC_PROXY && HIWORD(W) == BN_CLICKED)
		{
			SyncNetControls(true);
			LogNetToggleDuringInstall();
		}
		else if(Id == IDC_MIRRORHOST && (HIWORD(W) == CBN_EDITCHANGE || HIWORD(W) == CBN_SELCHANGE))
			LogNetToggleDuringInstall();   // 改镜像地址（如 gh.meali.top ↔ ghproxy.net）
		else if(Id == IDC_PROXYADDR && HIWORD(W) == EN_CHANGE)
			LogNetToggleDuringInstall();   // 改代理地址
		else if(Id == IDC_MT && HIWORD(W) == BN_CLICKED)
		{
			EnableWindow(g_hMtCount, SendMessageW(g_hMt, BM_GETCHECK, 0, 0) == BST_CHECKED);
			UpdateHotThreads();   // 热生效：勾/取消多线程立即影响下一个下载
		}
		else if(Id == IDC_MTCOUNT && (HIWORD(W) == CBN_EDITCHANGE || HIWORD(W) == CBN_SELCHANGE))
			UpdateHotThreads();   // 热生效：安装进行中改线程数（可编辑下拉发 CBN_EDITCHANGE，不是 EN_CHANGE）
		else if(Id == IDC_BROWSE)
		{
			BROWSEINFOW Bi = {};
			Bi.hwndOwner = H;
			Bi.lpszTitle = L"选择安装/工作目录";
			Bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_USENEWUI;
			LPITEMIDLIST Pidl = SHBrowseForFolderW(&Bi);
			if(Pidl)
			{
				wchar_t Path[MAX_PATH] = {};
				SHGetPathFromIDListW(Pidl, Path);
				SetWindowTextW(g_hWorkDir, Path);
			}
		}
		else if(Id == IDC_OPENDIR)
		{
			std::string D = g_HasState && !g_State.DistDir.empty() ? g_State.DistDir : JoinPath(g_Installer.AppDir, "client");
			ShellExecuteW(nullptr, L"open", Utf8ToWide(D).c_str(), nullptr, nullptr, SW_SHOWNORMAL);
		}
		else if(Id == IDC_SOURCE && HIWORD(W) == CBN_SELCHANGE)
			StartWorker(WorkerRefresh);
		return 0;
	}
	case WM_DROPFILES: {
		HDROP Drop = (HDROP)W;
		UINT Count = DragQueryFileW(Drop, 0xFFFFFFFF, nullptr, 0);
		int Added = 0;
		std::string ModsDir = JoinPath(g_Installer.AppDir, "mods");
		std::string CacheDir = JoinPath(g_Installer.AppDir, "_mods");
		MakeDirs(ModsDir);
		for(UINT i = 0; i < Count; ++i)
		{
			wchar_t Path[MAX_PATH] = {};
			DragQueryFileW(Drop, i, Path, MAX_PATH);
			std::string P = WideToUtf8(Path);
			if(!IsDmodFile(P))
			{
				AppendLog("[i] 忽略（不是 .dmod）：" + FileNameOf(P) + " —— mod 请用单文件 .dmod 格式");
				continue;
			}
			std::string Dst = JoinPath(ModsDir, FileNameOf(P));
			if(_stricmp(P.c_str(), Dst.c_str()) != 0)
			{
				std::string CErr;
				CopyTree(P, Dst, CErr);
			}
			ModuleInfo M;
			std::string Err;
			if(!LoadDmodFile(Dst, CacheDir, M, Err))
			{
				AppendLog("[x] 这个 mod 用不了：" + FileNameOf(P) + " —— " + Err);
				continue;
			}
			bool Replaced = false;
			for(auto &X : g_Index.Modules)
				if(X.Id == M.Id)
				{
					X = M;
					Replaced = true;
					break;
				}
			if(!Replaced)
				g_Index.Modules.push_back(M);
			AppendLog("[+] 已加入 mod：" + M.Name + "  v" + M.Version + "（" + FileNameOf(Dst) + "，" + (Replaced ? "替换同 id 旧版" : "新增") + "）");
			Added++;
		}
		DragFinish(Drop);
		if(Added > 0)
		{
			FillModules();
			FillSources();
			SetStatus("已添加 " + std::to_string(Added) + " 个 mod");
		}
		return 0;
	}
	case WM_DESTROY:
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(H, Msg, W, L);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, LPWSTR, int)
{
	// 开发/自检用（不进 GUI，方便核对多线程下载的字节数与哈希）：
	//   ddnet-module-installer.exe --download <url> <保存路径> [线程数] [限时秒] [--proxy 地址]
	// 注意本程序是 WIN32(GUI) 子系统，没有 stdout，printf 的结果看不见；
	// 所以结果同时写入 <保存路径>.dl-result.txt（自动化自检读这个文件）。
	{
		int Argc = 0;
		LPWSTR *Argv = CommandLineToArgvW(GetCommandLineW(), &Argc);
		if(Argv && Argc >= 4 && _wcsicmp(Argv[1], L"--download") == 0)
		{
			const std::string Url = WideToUtf8(Argv[2]);
			const std::string Dest = WideToUtf8(Argv[3]);
			const int Th = (Argc >= 5) ? _wtoi(Argv[4]) : 1;
			const int LimitSec = (Argc >= 6) ? _wtoi(Argv[5]) : 0;   // 可选：跑 N 秒就停（测速用）
			std::string Proxy;
			for(int i = 6; i < Argc; ++i)   // 可选开关：--proxy <host:port>
				if(_wcsicmp(Argv[i], L"--proxy") == 0 && i + 1 < Argc)
					Proxy = WideToUtf8(Argv[++i]);
			LocalFree(Argv);
			Http H;
			H.Proxy = Proxy;   // 空 = 按 Windows 系统代理设置走（PRECONFIG）
			std::atomic<bool> Stop{false};
			if(LimitSec > 0)
				H.pCancel = &Stop;
			const auto T0 = std::chrono::steady_clock::now();
			int64_t LastShown = 0;
			int64_t LastGot = 0;
			auto Cb = [&](int64_t Got, int64_t Total) {
				LastGot = Got;
				if(LimitSec > 0 && std::chrono::duration<double>(std::chrono::steady_clock::now() - T0).count() >= LimitSec)
					Stop = true;
				if(Got - LastShown >= 4 * 1024 * 1024 || Got == Total)
				{
					LastShown = Got;
					printf("  %lld / %lld bytes\n", (long long)Got, (long long)Total);
				}
			};
			HttpResult R;
			if(Th > 1)
				R = H.DownloadFileMulti(Url, Dest, Th, Cb, [](const std::string &S) { printf("%s\n", S.c_str()); });
			else
				R = H.DownloadFile(Url, Dest, Cb);
			const double Sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - T0).count();
			char Line[640];
			snprintf(Line, sizeof(Line), "result: ok=%d status=%d err=%s seconds=%.2f bytes=%lld rate=%.2f MB/s threads=%d proxy=%s",
				R.Ok ? 1 : 0, R.Status, R.Error.empty() ? "-" : R.Error.c_str(), Sec, (long long)LastGot,
				Sec > 0 ? LastGot / 1048576.0 / Sec : 0.0, Th, Proxy.empty() ? "(system)" : Proxy.c_str());
			printf("%s\n", Line);
			{
				std::string Rf = Dest + ".dl-result.txt";
				FILE *F = nullptr;
				if(fopen_s(&F, Rf.c_str(), "wb") == 0 && F)
				{
					fwrite(Line, 1, strlen(Line), F);
					fprintf(F, "\n");
					fclose(F);
				}
			}
			return R.Ok ? 0 : 1;
		}
		if(Argv)
			LocalFree(Argv);
	}

	INITCOMMONCONTROLSEX Icc = {sizeof(Icc), ICC_LISTVIEW_CLASSES | ICC_PROGRESS_CLASS | ICC_STANDARD_CLASSES | ICC_BAR_CLASSES};
	InitCommonControlsEx(&Icc);

	g_Installer.Log = LogBridge;
	g_Installer.Progress = ProgressBridge;
	g_Installer.pCancel = &g_CancelFlag;
	g_Installer.Net.pCancel = &g_CancelFlag;
	g_Installer.AppDir = ExeDir();
	g_LogFile = JoinPath(g_Installer.AppDir, "installer.log");
	{
		FILE *F = nullptr;
		if(fopen_s(&F, g_LogFile.c_str(), "wb") == 0 && F)
			fclose(F);
	}

	WNDCLASSEXW Wc = {sizeof(Wc)};
	Wc.lpfnWndProc = WndProc;
	Wc.hInstance = hInst;
	Wc.lpszClassName = L"DDNetModuleInstaller";
	Wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
	Wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
	Wc.hIcon = LoadIcon(nullptr, IDI_APPLICATION);
	RegisterClassExW(&Wc);

	g_hMain = CreateWindowExW(0, Wc.lpszClassName, Utf8ToWide(std::string("DDNet / TClient 模块化安装器  v") + kInstallerVersion).c_str(),
		WS_OVERLAPPEDWINDOW | WS_VISIBLE, CW_USEDEFAULT, CW_USEDEFAULT, 1180, 860,
		nullptr, nullptr, hInst, nullptr);
	if(!g_hMain)
		return 1;

	MSG Msg;
	while(GetMessageW(&Msg, nullptr, 0, 0) > 0)
	{
		TranslateMessage(&Msg);
		DispatchMessageW(&Msg);
	}
	return 0;
}
