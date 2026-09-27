// ddnet-module-installer - core engine implementation
#include "core.h"

#include <windows.h>
#include <wininet.h>
#include <shlobj.h>
#include <shellapi.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <mutex>
#include <sstream>
#include <thread>

#pragma comment(lib, "wininet.lib")

// 多线程下载的"热"线程数（见 core.h）：UI 线程写，工作线程每次下载开始时读
std::atomic<int> g_DlThreadsHint{0};

// ============================================================ 小工具 =========
std::string WideToUtf8(const std::wstring &W)
{
	if(W.empty())
		return {};
	int N = WideCharToMultiByte(CP_UTF8, 0, W.c_str(), (int)W.size(), nullptr, 0, nullptr, nullptr);
	std::string S(N, 0);
	WideCharToMultiByte(CP_UTF8, 0, W.c_str(), (int)W.size(), &S[0], N, nullptr, nullptr);
	return S;
}

std::wstring Utf8ToWide(const std::string &S)
{
	if(S.empty())
		return {};
	int N = MultiByteToWideChar(CP_UTF8, 0, S.c_str(), (int)S.size(), nullptr, 0);
	std::wstring W(N, 0);
	MultiByteToWideChar(CP_UTF8, 0, S.c_str(), (int)S.size(), &W[0], N);
	return W;
}

std::string ExeDir()
{
	wchar_t Buf[MAX_PATH] = {};
	GetModuleFileNameW(nullptr, Buf, MAX_PATH);
	std::wstring P(Buf);
	size_t Pos = P.find_last_of(L"\\/");
	return WideToUtf8(Pos == std::wstring::npos ? P : P.substr(0, Pos));
}

std::string JoinPath(const std::string &A, const std::string &B)
{
	if(A.empty())
		return B;
	if(B.empty())
		return A;
	char Last = A.back();
	if(Last == '\\' || Last == '/')
		return A + B;
	return A + "\\" + B;
}

bool PathExists(const std::string &P)
{
	return GetFileAttributesW(Utf8ToWide(P).c_str()) != INVALID_FILE_ATTRIBUTES;
}

bool IsDir(const std::string &P)
{
	DWORD A = GetFileAttributesW(Utf8ToWide(P).c_str());
	return A != INVALID_FILE_ATTRIBUTES && (A & FILE_ATTRIBUTE_DIRECTORY);
}

bool MakeDirs(const std::string &P)
{
	if(P.empty())
		return false;
	std::wstring W = Utf8ToWide(P);
	for(size_t i = 0; i < W.size(); ++i)
	{
		if(W[i] == L'\\' || W[i] == L'/')
		{
			std::wstring Sub = W.substr(0, i);
			if(Sub.size() > 2)
				CreateDirectoryW(Sub.c_str(), nullptr);
		}
	}
	return CreateDirectoryW(W.c_str(), nullptr) != 0 || GetLastError() == ERROR_ALREADY_EXISTS;
}

bool RemoveTree(const std::string &P)
{
	std::wstring W = Utf8ToWide(P);
	if(W.empty() || !PathExists(P))
		return true;
	// 刚被杀掉的 git 子进程可能还握着 .git 里的文件句柄，
	// 所以删不掉要重试几次（SHFileOperation 会尽力删一部分），最后再用 rmdir 兜底。
	for(int Attempt = 0; Attempt < 5; ++Attempt)
	{
		std::wstring Double = W + L'\0';
		SHFILEOPSTRUCTW Op = {};
		Op.wFunc = FO_DELETE;
		Op.pFrom = Double.c_str();
		Op.fFlags = FOF_NO_UI | FOF_NOCONFIRMATION | FOF_SILENT;
		if(SHFileOperationW(&Op) == 0 && !PathExists(P))
			return true;
		Sleep(300 + 400 * Attempt);
	}
	{
		std::wstring Cmd = L"cmd.exe /c rmdir /s /q \"" + W + L"\"";
		std::wstring Mutable = Cmd;
		STARTUPINFOW Si = {sizeof(Si)};
		Si.dwFlags = STARTF_USESHOWWINDOW;
		Si.wShowWindow = SW_HIDE;
		PROCESS_INFORMATION Pi = {};
		if(CreateProcessW(nullptr, &Mutable[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &Si, &Pi))
		{
			WaitForSingleObject(Pi.hProcess, 15000);
			CloseHandle(Pi.hProcess);
			CloseHandle(Pi.hThread);
		}
	}
	return !PathExists(P);
}

// ------------------------------------------------ 完整性校验（用户要求）----
// 背景：TClient 在构建输出目录里本地编译 steam_api.dll，组装时漏拷导致
// DDNet.exe 启动报"找不到 steam_api.dll"而安装器却报"全部完成"。
// ①CollectRuntimeDlls：构建配置目录里的 *.dll 一律带上；②VerifyExeImports：
// 解析 PE 导入表逐个核对非系统 DLL 依赖，缺了就在"完成"之前拦下来。
bool CollectRuntimeDlls(const std::string &BuildDir, const std::string &Dist, int &Copied, std::string &Error)
{
	Copied = 0;
	WIN32_FIND_DATAW Fd;
	HANDLE H = FindFirstFileW(Utf8ToWide(JoinPath(BuildDir, "*.dll")).c_str(), &Fd);
	if(H == INVALID_HANDLE_VALUE)
		return true;   // 配置目录没有 DLL 也正常
	do {
		std::string Name = WideToUtf8(Fd.cFileName);
		if(Name == "." || Name == "..")
			continue;
		if(!CopyFileW(Utf8ToWide(JoinPath(BuildDir, Name)).c_str(),
			      Utf8ToWide(JoinPath(Dist, Name)).c_str(), FALSE))
		{
			Error = "复制运行时 DLL 失败: " + Name;
			FindClose(H);
			return false;
		}
		Copied++;
	} while(FindNextFileW(H, &Fd));
	FindClose(H);
	return true;
}

static bool ImportRvaToOff(const std::vector<IMAGE_SECTION_HEADER> &Secs, DWORD Rva, DWORD &Off)
{
	for(const auto &S : Secs)
	{
		DWORD Size = S.Misc.VirtualSize > S.SizeOfRawData ? S.Misc.VirtualSize : S.SizeOfRawData;
		if(Rva >= S.VirtualAddress && Rva < S.VirtualAddress + Size)
		{
			Off = S.PointerToRawData + (Rva - S.VirtualAddress);
			return true;
		}
	}
	return false;
}

bool VerifyExeImports(const std::string &ExePath, const std::string &Dir,
		      std::vector<std::string> &Missing, std::string &Error)
{
	Missing.clear();
	std::vector<char> Buf;
	{
		HANDLE F = CreateFileW(Utf8ToWide(ExePath).c_str(), GENERIC_READ, FILE_SHARE_READ,
				       nullptr, OPEN_EXISTING, 0, nullptr);
		if(F == INVALID_HANDLE_VALUE)
		{
			Error = "打不开可执行文件: " + ExePath;
			return false;
		}
		LARGE_INTEGER Sz;
		if(!GetFileSizeEx(F, &Sz) || Sz.QuadPart < 0x200)
		{
			CloseHandle(F);
			Error = "不是有效的可执行文件: " + ExePath;
			return false;
		}
		Buf.resize((size_t)Sz.QuadPart);
		DWORD Got = 0;
		BOOL Rd = ReadFile(F, Buf.data(), (DWORD)Buf.size(), &Got, nullptr);
		CloseHandle(F);
		if(!Rd || Got != Buf.size() || memcmp(Buf.data(), "MZ", 2) != 0)
		{
			Error = "不是有效的可执行文件: " + ExePath;
			return false;
		}
	}
	const IMAGE_DOS_HEADER *Dos = (const IMAGE_DOS_HEADER *)Buf.data();
	const IMAGE_NT_HEADERS *Nt = (const IMAGE_NT_HEADERS *)(Buf.data() + Dos->e_lfanew);
	if(Nt->Signature != IMAGE_NT_SIGNATURE)
	{
		Error = "PE 头无效: " + ExePath;
		return false;
	}
	const IMAGE_DATA_DIRECTORY &Imp = Nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
	std::vector<IMAGE_SECTION_HEADER> Secs(Nt->FileHeader.NumberOfSections);
	const IMAGE_SECTION_HEADER *S0 = (const IMAGE_SECTION_HEADER *)
		((const BYTE *)&Nt->OptionalHeader + Nt->FileHeader.SizeOfOptionalHeader);
	for(size_t i = 0; i < Secs.size(); ++i)
		Secs[i] = S0[i];
	auto NameAt = [&](DWORD Rva) -> std::string {
		DWORD Off = 0;
		if(!ImportRvaToOff(Secs, Rva, Off) || Off + 64 > Buf.size())
			return std::string();
		const char *P = Buf.data() + Off;
		size_t N = 0;
		while(N < 64 && P[N])
			++N;
		return std::string(P, N);
	};
	auto SysHas = [&](const std::string &Dll) -> bool {
		wchar_t Ws[MAX_PATH];
		UINT N = GetSystemDirectoryW(Ws, MAX_PATH);   // 64 位系统目录，不吃 WOW64 重定向
		if(N > 0 && N < MAX_PATH && PathExists(WideToUtf8(Ws) + "\\" + Dll))
			return true;
		return PathExists(std::string("C:\\Windows\\SysWOW64\\") + Dll);
	};
	for(DWORD O = Imp.VirtualAddress; O != 0; O += sizeof(IMAGE_IMPORT_DESCRIPTOR))
	{
		DWORD Off = 0;
		if(!ImportRvaToOff(Secs, O, Off) || Off + sizeof(IMAGE_IMPORT_DESCRIPTOR) > Buf.size())
			break;
		const IMAGE_IMPORT_DESCRIPTOR *D = (const IMAGE_IMPORT_DESCRIPTOR *)(Buf.data() + Off);
		if(D->Name == 0)
			break;
		std::string Dll = NameAt(D->Name);
		if(Dll.empty())
			continue;
		for(auto &c : Dll)
			c = (char)tolower((unsigned char)c);
		// Windows API Set（api-ms-win-* / ext-ms-*）由系统加载器虚拟解析，
		// 不是要随包分发的文件（libwinpthread 等会导入它们，误报会错杀）
		if(Dll.rfind("api-ms-win-", 0) == 0 || Dll.rfind("ext-ms-", 0) == 0)
			continue;
		if(PathExists(JoinPath(Dir, Dll)) || SysHas(Dll))
			continue;
		Missing.push_back(Dll);
	}
	return Missing.empty();
}

bool CopyTree(const std::string &From, const std::string &To, std::string &Error)
{
	// 单文件源必须走 CopyFileW：xcopy 对"单文件源 + 不存在的目标"会弹
	// "F = 文件 / D = 目录"交互提示，而本程序的子进程无窗口无 stdin，提示永远
	// 没人回答 → xcopy 以 0% CPU 永久挂起（实测卡死整个安装流水线）。
	DWORD Attr = GetFileAttributesW(Utf8ToWide(From).c_str());
	if(Attr != INVALID_FILE_ATTRIBUTES && !(Attr & FILE_ATTRIBUTE_DIRECTORY))
	{
		if(!CopyFileW(Utf8ToWide(From).c_str(), Utf8ToWide(To).c_str(), FALSE))
		{
			Error = "复制文件失败: " + From;
			return false;
		}
		return true;
	}
	// 目录：xcopy。目标末尾补反斜杠，强制按目录处理，同样杜绝 F/D 提示。
	std::wstring ToW = Utf8ToWide(To);
	if(!ToW.empty() && ToW.back() != L'\\')
		ToW += L'\\';
	std::wstring Cmd = L"cmd.exe /c xcopy \"" + Utf8ToWide(From) + L"\" \"" + ToW + L"\" /E /I /Y /Q >NUL";
	STARTUPINFOW Si = {sizeof(Si)};
	PROCESS_INFORMATION Pi = {};
	std::wstring Mutable = Cmd;
	if(!CreateProcessW(nullptr, &Mutable[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &Si, &Pi))
	{
		Error = "xcopy 启动失败";
		return false;
	}
	WaitForSingleObject(Pi.hProcess, INFINITE);
	DWORD Code = 1;
	GetExitCodeProcess(Pi.hProcess, &Code);
	CloseHandle(Pi.hProcess);
	CloseHandle(Pi.hThread);
	if(Code != 0)
	{
		Error = "复制目录失败: " + From;
		return false;
	}
	return true;
}

std::string NowStamp()
{
	SYSTEMTIME St;
	GetLocalTime(&St);
	char Buf[64];
	snprintf(Buf, sizeof(Buf), "%04d-%02d-%02d %02d:%02d:%02d",
		St.wYear, St.wMonth, St.wDay, St.wHour, St.wMinute, St.wSecond);
	return Buf;
}

// =============================================================== process =====
ProcessResult RunProcess(const std::string &Exe, const std::vector<std::string> &Args,
	const std::string &WorkDir, const std::function<void(const std::string &)> &OnLine,
	std::atomic<bool> *pCancel)
{
	ProcessResult R;
	std::wstring Cmd = L"\"" + Utf8ToWide(Exe) + L"\"";
	for(const auto &A : Args)
		Cmd += L" \"" + Utf8ToWide(A) + L"\"";
	std::wstring Mutable = Cmd;

	SECURITY_ATTRIBUTES Sa = {sizeof(Sa), nullptr, TRUE};
	HANDLE Rd = nullptr, Wr = nullptr;
	CreatePipe(&Rd, &Wr, &Sa, 0);
	SetHandleInformation(Rd, HANDLE_FLAG_INHERIT, 0);

	STARTUPINFOW Si = {sizeof(Si)};
	Si.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
	Si.wShowWindow = SW_HIDE;
	Si.hStdOutput = Wr;
	Si.hStdError = Wr;
	Si.hStdInput = nullptr;

	PROCESS_INFORMATION Pi = {};
	std::wstring Cwd = WorkDir.empty() ? L"" : Utf8ToWide(WorkDir);
	if(!CreateProcessW(nullptr, &Mutable[0], nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
		   nullptr, Cwd.empty() ? nullptr : Cwd.c_str(), &Si, &Pi))
	{
		CloseHandle(Rd);
		CloseHandle(Wr);
		R.ExitCode = -1;
		if(OnLine)
			OnLine("  [x] 无法启动进程: " + Exe);
		return R;
	}
	CloseHandle(Wr);

	// 用 Job Object 包住子进程：取消时连它的子进程（git 会派生 git-remote-https /
	// git-index-pack 等）一起收掉。否则爷爷进程被杀、孙子还活着握着 .git 里的句柄，
	// 清理就会报"删除失败（被占用）"。失败也不影响主流程（只是少一层保险）。
	HANDLE Job = CreateJobObjectW(nullptr, nullptr);
	if(Job)
	{
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION Jeli = {};
		Jeli.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
		SetInformationJobObject(Job, JobObjectExtendedLimitInformation, &Jeli, sizeof(Jeli));
		if(!AssignProcessToJobObject(Job, Pi.hProcess))
		{
			CloseHandle(Job);
			Job = nullptr;
		}
	}

	std::string Pending;
	char Buf[4096];
	DWORD Read = 0;
	bool Killed = false;
	// 最近一次收到子进程输出的时刻（用于"仍在进行"心跳）
	std::chrono::steady_clock::time_point LastOut = std::chrono::steady_clock::now();
	while(true)
	{
		if(pCancel && pCancel->load())
		{
			// 先按**进程树**杀：git 会派生 git-remote-https / index-pack 等孙子进程，
			// 只 TerminateProcess 直接子进程的话，孙子会继续活着并握着 .git 里的文件句柄，
			// 结果就是"取消后清理删不掉"。taskkill /T 会顺着父子关系整棵收掉。
			std::wstring Tk = L"taskkill /T /F /PID " + std::to_wstring((unsigned long)Pi.dwProcessId);
			std::wstring TkMutable = Tk;
			STARTUPINFOW Si2 = {sizeof(Si2)};
			Si2.dwFlags = STARTF_USESHOWWINDOW;
			Si2.wShowWindow = SW_HIDE;
			PROCESS_INFORMATION Pi2 = {};
			if(CreateProcessW(nullptr, &TkMutable[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &Si2, &Pi2))
			{
				WaitForSingleObject(Pi2.hProcess, 8000);
				CloseHandle(Pi2.hProcess);
				CloseHandle(Pi2.hThread);
			}
			TerminateProcess(Pi.hProcess, 1);
			Killed = true;
			if(OnLine)
				OnLine("  [i] 已取消，正在结束当前子进程…");
			break;
		}
				// 轮询读取：阻塞式 ReadFile 会在网络卡住时让"取消"检查永远等不到机会
		DWORD Avail = 0;
		if(!PeekNamedPipe(Rd, nullptr, 0, nullptr, &Avail, nullptr))
			break;   // 写端已关闭
		if(Avail == 0)
		{
			if(WaitForSingleObject(Pi.hProcess, 0) == WAIT_OBJECT_0)
			{
				if(!ReadFile(Rd, Buf, sizeof(Buf), &Read, nullptr) || Read == 0)
					break;   // 进程已退出且输出读干
			}
			else
			{
				Sleep(50);
				// 长时间没有任何输出时定期报一句"仍在进行"。
				// 用户实测：ddnet-libs 还在下载，但 git 一段时间不打任何进度 → 日志和界面
				// 看起来像卡死，人就以为出问题了。这里每 15 秒说明一次"进程还在跑"。
				const double IdleSec = std::chrono::duration<double>(std::chrono::steady_clock::now() - LastOut).count();
				if(IdleSec >= 15.0)
				{
					LastOut = std::chrono::steady_clock::now();
					if(OnLine)
					{
						char Hb[256];
						snprintf(Hb, sizeof(Hb), "  [i] 仍在进行：已 %.0f 秒没有新输出（进程仍在运行，没有卡死；网络慢时属正常）", IdleSec);
						OnLine(Hb);
					}
				}
				continue;
			}
		}
		else
		{
			const DWORD Want = Avail < (DWORD)sizeof(Buf) ? Avail : (DWORD)sizeof(Buf);
			if(!ReadFile(Rd, Buf, Want, &Read, nullptr) || Read == 0)
				break;
		}
		Pending.append(Buf, Read);
		LastOut = std::chrono::steady_clock::now();   // 有输出 → 重置心跳计时
		// git 的进度行是 \r 结尾（不是 \n）：两种都必须当行结束，
		// 否则进度全被攒在缓冲区里，界面上看起来就是"卡住不动"。
		for(;;)
		{
			size_t Pos = Pending.find_first_of("\r\n");
			if(Pos == std::string::npos)
				break;
			std::string Line = Pending.substr(0, Pos);
			size_t Skip = 1;
			if(Pending[Pos] == '\r' && Pos + 1 < Pending.size() && Pending[Pos + 1] == '\n')
				Skip = 2;   // CRLF 算一个换行
			Pending.erase(0, Pos + Skip);
			if(Line.empty())
				continue;
			R.Output += Line + "\n";
			if(OnLine)
				OnLine(Line);
		}
	}
	if(!Pending.empty())
	{
		R.Output += Pending + "\n";
		if(OnLine)
			OnLine(Pending);
	}
	WaitForSingleObject(Pi.hProcess, INFINITE);
	DWORD Code = 0;
	GetExitCodeProcess(Pi.hProcess, &Code);
	R.ExitCode = Killed ? -2 : (int)Code;
	CloseHandle(Pi.hProcess);
	CloseHandle(Pi.hThread);
	CloseHandle(Rd);
	if(Job)
		CloseHandle(Job);   // KILL_ON_JOB_CLOSE：顺手把还活着的孙子进程收掉
	return R;
}

// ================================================================== json =====
namespace
{
	struct Parser
	{
		const std::string &S;
		size_t I = 0;
		std::string Err;
		explicit Parser(const std::string &In) : S(In) {}

		void Skip()
		{
			while(I < S.size() && (S[I] == ' ' || S[I] == '\t' || S[I] == '\n' || S[I] == '\r'))
				++I;
		}
		bool Fail(const std::string &M)
		{
			if(Err.empty())
				Err = M + " @" + std::to_string(I);
			return false;
		}
		bool ParseValue(JsonValue &V)
		{
			Skip();
			if(I >= S.size())
				return Fail("意外结束");
			char C = S[I];
			if(C == '{')
				return ParseObject(V);
			if(C == '[')
				return ParseArray(V);
			if(C == '"')
			{
				V.T = JsonValue::Type::String;
				return ParseString(V.Str);
			}
			if(S.compare(I, 4, "true") == 0)
			{
				V.T = JsonValue::Type::Bool;
				V.Bool = true;
				I += 4;
				return true;
			}
			if(S.compare(I, 5, "false") == 0)
			{
				V.T = JsonValue::Type::Bool;
				V.Bool = false;
				I += 5;
				return true;
			}
			if(S.compare(I, 4, "null") == 0)
			{
				V.T = JsonValue::Type::Null;
				I += 4;
				return true;
			}
			// number
			size_t Start = I;
			while(I < S.size() && (isdigit((unsigned char)S[I]) || S[I] == '-' || S[I] == '+' || S[I] == '.' || S[I] == 'e' || S[I] == 'E'))
				++I;
			if(I == Start)
				return Fail("非法字符");
			V.T = JsonValue::Type::Number;
			V.Number = atof(S.substr(Start, I - Start).c_str());
			return true;
		}
		bool ParseString(std::string &Out)
		{
			if(S[I] != '"')
				return Fail("预期字符串");
			++I;
			Out.clear();
			while(I < S.size())
			{
				char C = S[I++];
				if(C == '"')
					return true;
				if(C != '\\')
				{
					Out += C;
					continue;
				}
				if(I >= S.size())
					return Fail("转义截断");
				char E = S[I++];
				switch(E)
				{
				case 'n': Out += '\n'; break;
				case 't': Out += '\t'; break;
				case 'r': Out += '\r'; break;
				case 'b': Out += '\b'; break;
				case 'f': Out += '\f'; break;
				case '/': Out += '/'; break;
				case '\\': Out += '\\'; break;
				case '"': Out += '"'; break;
				case 'u':
				{
					if(I + 4 > S.size())
						return Fail("\\u 截断");
					unsigned int Cp = (unsigned int)strtoul(S.substr(I, 4).c_str(), nullptr, 16);
					I += 4;
					// 代理对
					if(Cp >= 0xD800 && Cp <= 0xDBFF && S.compare(I, 2, "\\u") == 0 && I + 6 <= S.size())
					{
						unsigned int Lo = (unsigned int)strtoul(S.substr(I + 2, 4).c_str(), nullptr, 16);
						if(Lo >= 0xDC00 && Lo <= 0xDFFF)
						{
							Cp = 0x10000 + ((Cp - 0xD800) << 10) + (Lo - 0xDC00);
							I += 6;
						}
					}
					// UTF-8 编码
					if(Cp < 0x80)
						Out += (char)Cp;
					else if(Cp < 0x800)
					{
						Out += (char)(0xC0 | (Cp >> 6));
						Out += (char)(0x80 | (Cp & 0x3F));
					}
					else if(Cp < 0x10000)
					{
						Out += (char)(0xE0 | (Cp >> 12));
						Out += (char)(0x80 | ((Cp >> 6) & 0x3F));
						Out += (char)(0x80 | (Cp & 0x3F));
					}
					else
					{
						Out += (char)(0xF0 | (Cp >> 18));
						Out += (char)(0x80 | ((Cp >> 12) & 0x3F));
						Out += (char)(0x80 | ((Cp >> 6) & 0x3F));
						Out += (char)(0x80 | (Cp & 0x3F));
					}
					break;
				}
				default: Out += E; break;
				}
			}
			return Fail("字符串未闭合");
		}
		bool ParseArray(JsonValue &V)
		{
			V.T = JsonValue::Type::Array;
			++I;
			Skip();
			if(I < S.size() && S[I] == ']')
			{
				++I;
				return true;
			}
			while(true)
			{
				JsonValue Item;
				if(!ParseValue(Item))
					return false;
				V.Arr.push_back(std::move(Item));
				Skip();
				if(I < S.size() && S[I] == ',')
				{
					++I;
					continue;
				}
				if(I < S.size() && S[I] == ']')
				{
					++I;
					return true;
				}
				return Fail("数组格式错误");
			}
		}
		bool ParseObject(JsonValue &V)
		{
			V.T = JsonValue::Type::Object;
			++I;
			Skip();
			if(I < S.size() && S[I] == '}')
			{
				++I;
				return true;
			}
			while(true)
			{
				Skip();
				std::string Key;
				if(!ParseString(Key))
					return false;
				Skip();
				if(I >= S.size() || S[I] != ':')
					return Fail("缺少冒号");
				++I;
				JsonValue Val;
				if(!ParseValue(Val))
					return false;
				V.Obj.emplace_back(Key, std::move(Val));
				Skip();
				if(I < S.size() && S[I] == ',')
				{
					++I;
					continue;
				}
				if(I < S.size() && S[I] == '}')
				{
					++I;
					return true;
				}
				return Fail("对象格式错误");
			}
		}
	};
}

bool JsonParse(const std::string &Text, JsonValue &Out, std::string &Error)
{
	// 容忍 UTF-8 BOM：Windows 记事本/PowerShell Set-Content -Encoding UTF8 写出的
	// module.json 常带 BOM，直接解析会报"非法字符 @0"。
	size_t Start = 0;
	if(Text.size() >= 3 && (unsigned char)Text[0] == 0xEF && (unsigned char)Text[1] == 0xBB && (unsigned char)Text[2] == 0xBF)
		Start = 3;
	std::string Clean = Start ? Text.substr(Start) : Text;
	Parser P(Clean);
	if(!P.ParseValue(Out))
	{
		Error = P.Err;
		return false;
	}
	return true;
}

const JsonValue *JsonValue::Find(const std::string &Key) const
{
	for(const auto &KV : Obj)
		if(KV.first == Key)
			return &KV.second;
	return nullptr;
}

std::string JsonValue::GetString(const std::string &Key, const std::string &Default) const
{
	const JsonValue *V = Find(Key);
	return (V && V->T == Type::String) ? V->Str : Default;
}

double JsonValue::GetNumber(const std::string &Key, double Default) const
{
	const JsonValue *V = Find(Key);
	return (V && V->T == Type::Number) ? V->Number : Default;
}

bool JsonValue::GetBool(const std::string &Key, bool Default) const
{
	const JsonValue *V = Find(Key);
	return (V && V->T == Type::Bool) ? V->Bool : Default;
}

const JsonValue *JsonValue::GetArray(const std::string &Key) const
{
	const JsonValue *V = Find(Key);
	return (V && V->T == Type::Array) ? V : nullptr;
}

std::string JsonEscape(const std::string &In)
{
	std::string O;
	for(char C : In)
	{
		switch(C)
		{
		case '"': O += "\\\""; break;
		case '\\': O += "\\\\"; break;
		case '\n': O += "\\n"; break;
		case '\r': O += "\\r"; break;
		case '\t': O += "\\t"; break;
		default: O += C;
		}
	}
	return O;
}

// ================================================================== http =====
std::string Http::ApplyMirror(const std::string &Url) const
{
	if(MirrorPrefix.empty())
		return Url;
	if(Url.rfind("https://github.com/", 0) == 0 || Url.rfind("https://raw.githubusercontent.com/", 0) == 0 ||
		Url.rfind("https://api.github.com/", 0) == 0)
		return MirrorPrefix + Url;
	return Url;
}

namespace
{
	struct InetHandle
	{
		HINTERNET H = nullptr;
		~InetHandle()
		{
			if(H)
				InternetCloseHandle(H);
		}
	};

	// WinInet 需要宽字符 URL
	std::wstring WUrl(const std::string &U) { return Utf8ToWide(U); }

	// 打开 WinInet 会话：Direct = 绕过一切代理直连；Proxy 非空 = 显式走代理；
	// 两者都没有 = 沿用系统（IE）代理设置（ProxyEnable=1 时就是系统的那个代理）。
	HINTERNET OpenInet(const std::string &Proxy, bool Direct, const wchar_t *Agent)
	{
		if(Direct)
			return InternetOpenW(Agent, INTERNET_OPEN_TYPE_DIRECT, nullptr, nullptr, 0);
		if(Proxy.empty())
			return InternetOpenW(Agent, INTERNET_OPEN_TYPE_PRECONFIG, nullptr, nullptr, 0);
		std::wstring Px = Utf8ToWide(Proxy);
		std::wstring Bypass = L"<local>";
		return InternetOpenW(Agent, INTERNET_OPEN_TYPE_PROXY, Px.c_str(), Bypass.c_str(), 0);
	}
}

HttpResult Http::GetString(const std::string &Url, int TimeoutSec) const
{
	HttpResult R;
	std::string Final = ApplyMirror(Url);
	InetHandle Inet;
	Inet.H = OpenInet(Proxy, Direct, L"ddnet-module-installer");
	if(!Inet.H)
	{
		R.Error = "InternetOpen 失败";
		return R;
	}
	InternetSetOptionW(Inet.H, INTERNET_OPTION_CONNECT_TIMEOUT, (void *)&TimeoutSec, sizeof(TimeoutSec));
	if(!(Inet.H = Inet.H))
		return R;   // 上面的赋值只是为了复用 RAII，实际句柄未变
	HINTERNET Req = InternetOpenUrlW(Inet.H, WUrl(Final).c_str(), L"User-Agent: ddnet-module-installer\r\nAccept: application/json\r\n",
		(DWORD)-1L, INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_SECURE, 0);
	if(!Req)
	{
		R.Error = "连接失败: " + Final;
		return R;
	}
	DWORD Status = 0, Len = sizeof(Status);
	HttpQueryInfoW(Req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &Status, &Len, nullptr);
	R.Status = (int)Status;
	char Buf[8192];
	DWORD Read = 0;
	while(InternetReadFile(Req, Buf, sizeof(Buf), &Read) && Read > 0)
		R.Body.append(Buf, Read);
	InternetCloseHandle(Req);
	R.Ok = (R.Status >= 200 && R.Status < 300);
	if(!R.Ok && R.Error.empty())
		R.Error = "HTTP " + std::to_string(R.Status);
	return R;
}

HttpResult Http::DownloadFile(const std::string &Url, const std::string &DestPath,
	const std::function<void(int64_t, int64_t)> &Progress) const
{
	HttpResult R;
	std::string Final = ApplyMirror(Url);
	InetHandle Inet;
	Inet.H = OpenInet(Proxy, Direct, L"ddnet-module-installer");
	if(!Inet.H)
	{
		R.Error = "InternetOpen 失败";
		return R;
	}
	HINTERNET Req = InternetOpenUrlW(Inet.H, WUrl(Final).c_str(), L"User-Agent: ddnet-module-installer\r\n",
		(DWORD)-1L, INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_SECURE, 0);
	if(!Req)
	{
		R.Error = "下载连接失败: " + Final;
		return R;
	}
	DWORD Status = 0, Len = sizeof(Status);
	HttpQueryInfoW(Req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &Status, &Len, nullptr);
	R.Status = (int)Status;
	int64_t Total = -1;
	{
		char ClBuf[64] = {};
		DWORD ClLen = sizeof(ClBuf);
		if(HttpQueryInfoA(Req, HTTP_QUERY_CONTENT_LENGTH, ClBuf, &ClLen, nullptr))
			Total = _atoi64(ClBuf);
	}
	FILE *F = nullptr;
	if(fopen_s(&F, DestPath.c_str(), "wb") != 0 || !F)
	{
		InternetCloseHandle(Req);
		R.Error = "无法写入: " + DestPath;
		return R;
	}
	char Buf[65536];
	DWORD Read = 0;
	int64_t Got = 0;
	while(InternetReadFile(Req, Buf, sizeof(Buf), &Read) && Read > 0)
	{
		if(pCancel && pCancel->load())
		{
			fclose(F);
			InternetCloseHandle(Req);
			R.Ok = false;
			R.Error = "已取消";
			return R;
		}
		fwrite(Buf, 1, Read, F);
		Got += Read;
		if(Progress)
			Progress(Got, Total);
	}
	fclose(F);
	InternetCloseHandle(Req);
	// Total 已知时必须下满：连接中途断开会留下 Got<Total 的半截文件，
	// 不能当成"下载成功"交给下一步（解压/校验都会炸）。
	R.Ok = (R.Status >= 200 && R.Status < 300) && Got > 0 && (Total <= 0 || Got >= Total);
	if(!R.Ok)
	{
		if(Total > 0 && Got < Total && R.Error.empty())
			R.Error = "下载不完整：" + std::to_string((long long)Got) + "/" + std::to_string((long long)Total) + " 字节（连接中断）";
		else if(R.Error.empty())
			R.Error = "下载不完整 HTTP " + std::to_string(R.Status);
		DeleteFileW(Utf8ToWide(DestPath).c_str());   // 半截文件不留到下一次
	}
	return R;
}

// ------------------------------------------------ 多线程分段下载（NDM 式）-----
namespace
{
	// WinInet 的**自动重定向会丢掉自定义请求头**（包括 Range）：
	// github.com 的 release 下载会 302 到 release-assets.githubusercontent.com，
	// InternetOpenUrlW 自动跟随之后 Range 头就没了，分段请求只会拿到 200 全量，
	// 多线程永远走不通（每次都静默退回单连接）。所以分段前先手动把重定向链走完
	// （NO_AUTO_REDIRECT 逐跳跟随 Location），分段请求直接打"最终地址"，Range 才会生效。
	// 顺带在最终响应上读 Content-Length，省一次单独的预检请求。
	// 返回最终地址；TotalOut = 文件大小（拿不到 -1）；StatusOut = 最终 HTTP 状态码。
	std::wstring ResolveFinalUrl(const std::wstring &Url0, const std::string &Proxy, bool Direct,
		int64_t &TotalOut, int &StatusOut, std::string &Err)
	{
		std::wstring Cur = Url0;
		TotalOut = -1;
		StatusOut = 0;
		for(int Hop = 0; Hop < 6; ++Hop)
		{
			InetHandle Inet;
			Inet.H = OpenInet(Proxy, Direct, L"ddnet-module-installer");
			if(!Inet.H)
			{
				Err = "InternetOpen 失败";
				return Cur;
			}
			int Tmo = 30;
			InternetSetOptionW(Inet.H, INTERNET_OPTION_CONNECT_TIMEOUT, (void *)&Tmo, sizeof(Tmo));
			URL_COMPONENTSW Uc = {sizeof(Uc)};
			wchar_t Hst[256] = {}, Pth[2048] = {}, Ext[1024] = {};
			Uc.dwHostNameLength = 255; Uc.lpszHostName = Hst;
			Uc.dwUrlPathLength = 2047; Uc.lpszUrlPath = Pth;
			Uc.dwExtraInfoLength = 1023; Uc.lpszExtraInfo = Ext;
			if(!InternetCrackUrlW(Cur.c_str(), (DWORD)Cur.size(), 0, &Uc))
			{
				Err = "URL 解析失败";
				return Cur;
			}
			InetHandle Conn;
			Conn.H = InternetConnectW(Inet.H, Hst, Uc.nPort, nullptr, nullptr, INTERNET_SERVICE_HTTP, 0, 0);
			if(!Conn.H)
			{
				Err = "连接失败";
				return Cur;
			}
			std::wstring Path = Uc.dwUrlPathLength ? std::wstring(Pth, Uc.dwUrlPathLength) : std::wstring(L"/");
			if(Uc.dwExtraInfoLength)
				Path += std::wstring(Ext, Uc.dwExtraInfoLength);   // ?query 也属于请求路径
			DWORD Flags = INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_NO_AUTO_REDIRECT;
			if(Uc.nScheme == INTERNET_SCHEME_HTTPS)
				Flags |= INTERNET_FLAG_SECURE;
			InetHandle Req;
			Req.H = HttpOpenRequestW(Conn.H, L"GET", Path.c_str(), nullptr, nullptr, nullptr, Flags, 0);
			if(!Req.H)
			{
				Err = "构造请求失败";
				return Cur;
			}
			const wchar_t *Hd = L"User-Agent: ddnet-module-installer\r\nAccept: */*\r\n";
			if(!HttpSendRequestW(Req.H, Hd, (DWORD)-1L, nullptr, 0))
			{
				Err = "请求发送失败";
				return Cur;
			}
			DWORD Code = 0, Len = sizeof(Code);
			HttpQueryInfoW(Req.H, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &Code, &Len, nullptr);
			StatusOut = (int)Code;
			if(Code >= 300 && Code < 400)
			{
				wchar_t Loc[8192] = {};
				DWORD LL = sizeof(Loc) - 2;
				if(!HttpQueryInfoW(Req.H, HTTP_QUERY_LOCATION, Loc, &LL, nullptr))
				{
					Err = "重定向但拿不到 Location";
					return Cur;
				}
				// Location 可能是相对地址，合成绝对地址再进下一跳
				wchar_t Abs[8192] = {};
				DWORD AL = sizeof(Abs) / sizeof(wchar_t) - 1;
				Cur = InternetCombineUrlW(Cur.c_str(), Loc, Abs, &AL, 0) ? std::wstring(Abs) : std::wstring(Loc);
				continue;   // 只读头不收正文，句柄析构即断开
			}
			char Cl[64] = {};
			DWORD ClLen = sizeof(Cl);
			if(HttpQueryInfoA(Req.H, HTTP_QUERY_CONTENT_LENGTH, Cl, &ClLen, nullptr))
				TotalOut = _atoi64(Cl);
			return Cur;   // 非重定向：这就是最终地址
		}
		Err = "重定向次数过多";
		return Cur;
	}
}

HttpResult Http::DownloadFileMulti(const std::string &Url, const std::string &DestPath, int Threads,
	const std::function<void(int64_t, int64_t)> &Progress,
	const std::function<void(const std::string &)> &Note) const
{
	auto Say = [&](const std::string &S) {
		if(Note)
			Note(S);
	};
	HttpResult R;
	if(Threads < 2)
		return DownloadFile(Url, DestPath, Progress);   // 没开多线程

	const std::string Final = ApplyMirror(Url);
	// 先把重定向链手动走完（WinInet 自动重定向会丢 Range 头），拿到最终地址与文件大小
	int PreStatus = 0;
	std::string PreErr;
	int64_t Total = -1;
	const std::string FinalUrl = WideToUtf8(ResolveFinalUrl(WUrl(Final), Proxy, Direct, Total, PreStatus, PreErr));
	if(PreStatus != 200 && PreStatus != 206)
	{
		Say("  多线程下载：预检失败（HTTP " + std::to_string(PreStatus) + (PreErr.empty() ? "" : "，" + PreErr) + "），改用单连接");
		return DownloadFile(Url, DestPath, Progress);
	}
	if(Total <= 0)
	{
		Say("  多线程下载：拿不到文件大小（无 Content-Length），改用单连接");
		return DownloadFile(Url, DestPath, Progress);
	}

	// 分块：最多 16 段，且每段至少 1MB
	int N = Threads > 16 ? 16 : Threads;
	int64_t MaxChunks = Total / (1024 * 1024);
	if(MaxChunks < 1)
		MaxChunks = 1;
	if((int64_t)N > MaxChunks)
		N = (int)MaxChunks;
	if(N < 2)
	{
		Say("  多线程下载：文件较小（" + std::to_string((long long)Total) + " 字节），不值得分段，改用单连接");
		return DownloadFile(Url, DestPath, Progress);
	}
	Say("  多线程下载：开 " + std::to_string(N) + " 个连接分段拉取，共 " +
		std::to_string((long long)(Total / 1048576)) + " MB（每段独立断点续传）");

	// 预分配目标文件到最终大小，之后各线程直接按偏移写入自己的区间
	{
		HANDLE F = CreateFileW(Utf8ToWide(DestPath).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
			nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
		if(F == INVALID_HANDLE_VALUE)
		{
			R.Error = "无法写入: " + DestPath;
			return R;
		}
		LARGE_INTEGER Li;
		Li.QuadPart = Total;
		SetFilePointerEx(F, Li, nullptr, FILE_BEGIN);
		SetEndOfFile(F);
		CloseHandle(F);
	}

	std::vector<int64_t> Start(N), End(N);
	const int64_t Per = Total / N;
	for(int i = 0; i < N; ++i)
	{
		Start[i] = Per * i;
		End[i] = (i == N - 1) ? (Total - 1) : (Per * (i + 1) - 1);
	}
	std::atomic<int64_t> Done{0};
	std::atomic<int> Failed{0};
	std::mutex FailMx;
	std::string FailMsg;
	const std::string Fx = FinalUrl;   // 已解析完重定向的最终地址：分段请求打它，Range 才有效
	const std::string Dx = DestPath;
	const std::string Px = Proxy;
	const bool Dx2 = Direct;

	auto Worker = [&](int Idx) {
		int64_t Pos = Start[Idx];
		const int64_t EndPos = End[Idx];
		int Attempt = 0;
		while(Pos <= EndPos && Attempt < 4)
		{
			if(pCancel && pCancel->load())
				return;
			++Attempt;
			InetHandle Inet;
			Inet.H = OpenInet(Px, Dx2, L"ddnet-module-installer");
			if(!Inet.H)
			{
				Sleep(300);
				continue;
			}
			int Tmo = 60;
			InternetSetOptionW(Inet.H, INTERNET_OPTION_CONNECT_TIMEOUT, (void *)&Tmo, sizeof(Tmo));
			InternetSetOptionW(Inet.H, INTERNET_OPTION_RECEIVE_TIMEOUT, (void *)&Tmo, sizeof(Tmo));
			const std::wstring Hdr = L"User-Agent: ddnet-module-installer\r\nRange: bytes=" +
						 std::to_wstring(Pos) + L"-" + std::to_wstring(EndPos) + L"\r\n";
			HINTERNET Req = InternetOpenUrlW(Inet.H, WUrl(Fx).c_str(), Hdr.c_str(),
				(DWORD)-1L, INTERNET_FLAG_RELOAD | INTERNET_FLAG_NO_CACHE_WRITE | INTERNET_FLAG_SECURE, 0);
			if(!Req)
			{
				Sleep(300 * Attempt);
				continue;
			}
			DWORD Status = 0, Len = sizeof(Status);
			HttpQueryInfoW(Req, HTTP_QUERY_STATUS_CODE | HTTP_QUERY_FLAG_NUMBER, &Status, &Len, nullptr);
			if(Status != 206)
			{
				InternetCloseHandle(Req);
				std::lock_guard<std::mutex> Lk(FailMx);
				if(FailMsg.empty())
					FailMsg = "分段请求返回 HTTP " + std::to_string(Status) + "（不是 206）";
				Failed++;
				return;
			}
			HANDLE H = CreateFileW(Utf8ToWide(Dx).c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
				nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
			if(H == INVALID_HANDLE_VALUE)
			{
				InternetCloseHandle(Req);
				std::lock_guard<std::mutex> Lk(FailMx);
				if(FailMsg.empty())
					FailMsg = "无法打开目标文件写分段";
				Failed++;
				return;
			}
			LARGE_INTEGER Off;
			Off.QuadPart = Pos;
			SetFilePointerEx(H, Off, nullptr, FILE_BEGIN);
			char Buf[65536];
			bool Ok = true;
			for(;;)
			{
				if(pCancel && pCancel->load())
				{
					Ok = false;
					break;
				}
				DWORD Read = 0;
				if(!InternetReadFile(Req, Buf, sizeof(Buf), &Read) || Read == 0)
				{
					Ok = false;
					break;
				}
				DWORD Written = 0;
				if(!WriteFile(H, Buf, Read, &Written, nullptr) || Written != Read)
				{
					Ok = false;
					break;
				}
				Pos += Read;
				Done += Read;
				if(Pos > EndPos)
					break;
			}
			CloseHandle(H);
			InternetCloseHandle(Req);
			if(Ok && Pos > EndPos)
				return;   // 本段拉完
			if(pCancel && pCancel->load())
				return;
			Sleep(300 * Attempt);   // 断点续传：下一轮从 Pos 继续
		}
		if(Pos <= EndPos)
		{
			std::lock_guard<std::mutex> Lk(FailMx);
			if(FailMsg.empty())
				FailMsg = "分段未拉完（网络中断）";
			Failed++;
		}
	};

	std::vector<std::thread> Th;
	Th.reserve(N);
	for(int i = 0; i < N; ++i)
		Th.emplace_back(Worker, i);

	// 主线程只做一件事：每 0.4 秒报一次总进度（回调不在工作线程里调，避免多线程怼 UI）
	while(true)
	{
		if(pCancel && pCancel->load())
			break;
		if(Failed.load() > 0)
			break;
		const int64_t D = Done.load();
		if(Progress)
			Progress(D, Total);
		if(D >= Total)
			break;
		Sleep(400);
	}
	for(auto &T : Th)
		if(T.joinable())
			T.join();

	if(pCancel && pCancel->load())
	{
		DeleteFileW(Utf8ToWide(DestPath).c_str());
		R.Error = "已取消";
		return R;
	}
	if(Failed.load() > 0 || Done.load() < Total)
	{
		Say("  多线程下载失败（" + (FailMsg.empty() ? std::string("未完成") : FailMsg) + "），已改用单连接重试");
		DeleteFileW(Utf8ToWide(DestPath).c_str());
		return DownloadFile(Url, DestPath, Progress);
	}
	if(Progress)
		Progress(Total, Total);
	R.Ok = true;
	R.Status = 206;
	return R;
}

// ================================================================ index ======
namespace
{
	std::vector<std::string> JsonToStringList(const JsonValue *Arr)
	{
		std::vector<std::string> Out;
		if(Arr && Arr->T == JsonValue::Type::Array)
			for(const auto &V : Arr->Arr)
				if(V.T == JsonValue::Type::String)
					Out.push_back(V.Str);
		return Out;
	}
}

bool LoadIndexFromString(const std::string &Json, ModuleIndex &Out, std::string &Error)
{
	JsonValue Root;
	if(!JsonParse(Json, Root, Error))
		return false;
	Out = ModuleIndex{};
	Out.IndexVersion = (int)Root.GetNumber("index_version", 1);

	Out.RemoteIndexes = JsonToStringList(Root.GetArray("remote_indexes"));

	if(const JsonValue *Srcs = Root.GetArray("sources"))
	{
		for(const auto &S : Srcs->Arr)
		{
			GameSource G;
			G.Id = S.GetString("id");
			G.Name = S.GetString("name", G.Id);
			G.Repo = S.GetString("repo");
			if(!G.Id.empty() && !G.Repo.empty())
				Out.Sources.push_back(G);
		}
	}
	if(const JsonValue *Mods = Root.GetArray("modules"))
	{
		for(const auto &M : Mods->Arr)
		{
			ModuleInfo I;
			I.Id = M.GetString("id");
			I.Name = M.GetString("name", I.Id);
			I.Version = M.GetString("version");
			I.Description = M.GetString("description");
			I.Type = M.GetString("type", "source-patch");
			I.PatchUrl = M.GetString("patch_url");
			I.PatchLocal = M.GetString("patch_local");
			I.DocsUrl = M.GetString("docs_url");
			I.BaseHint = M.GetString("base_hint");
			I.TestedOn = JsonToStringList(M.GetArray("tested_on"));
			I.Requires = JsonToStringList(M.GetArray("requires"));
			if(const JsonValue *Ver = M.Find("verified"); Ver && Ver->T == JsonValue::Type::Object)
				for(const auto &KV : Ver->Obj)
					if(KV.second.T == JsonValue::Type::Bool)
						I.Verified[KV.first] = KV.second.Bool;
			I.VerifyPath = M.GetString("verify_path");
			if(!I.Id.empty())
			{
				I.Origin = "modules.json";
				Out.Modules.push_back(I);
			}
		}
	}
	if(Out.Sources.empty())
	{
		// 清单缺失时给一个可用的默认（但仍算成功，界面会有来源可选）
		Out.Sources.push_back({"ddnet", "DDNet 官方", "ddnet/ddnet"});
		Out.Sources.push_back({"tclient", "TClient", "TaterClient/TClient"});
	}
	return true;
}

bool LoadIndexFromFile(const std::string &Path, ModuleIndex &Out, std::string &Error)
{
	FILE *F = nullptr;
	if(fopen_s(&F, Path.c_str(), "rb") != 0 || !F)
	{
		Error = "无法打开清单: " + Path;
		return false;
	}
	std::string Text;
	char Buf[8192];
	size_t N;
	while((N = fread(Buf, 1, sizeof(Buf), F)) > 0)
		Text.append(Buf, N);
	fclose(F);
	return LoadIndexFromString(Text, Out, Error);
}

bool SaveIndexTemplate(const std::string &Path, std::string &Error)
{
	const char *Tpl =
		"{\n"
		"  \"index_version\": 1,\n"
		"  \"remote_indexes\": [],\n"
		"  \"sources\": [\n"
		"    {\"id\": \"ddnet\", \"name\": \"DDNet 官方\", \"repo\": \"ddnet/ddnet\"},\n"
		"    {\"id\": \"tclient\", \"name\": \"TClient\", \"repo\": \"TaterClient/TClient\"}\n"
		"  ],\n"
		"  \"modules\": [\n"
		"    {\n"
		"      \"id\": \"background\",\n"
		"      \"name\": \"自定义背景（图片 / 视频动态背景）\",\n"
		"      \"description\": \"主菜单与游戏内实体层的自定义背景，支持图片与视频、显示方式、Wallpaper Engine 等\",\n"
		"      \"type\": \"source-patch\",\n"
		"      \"patch_local\": \"modules\\\\background\\\\ddnet-background.patch\",\n"
		"      \"patch_url\": \"\",\n"
		"      \"docs_url\": \"\",\n"
		"      \"base_hint\": \"验证基线 TClient 10.9.0 (6b4118bf0)；其它版本用 3-way/reject 半自动\",\n"
		"      \"tested_on\": [\"TaterClient/TClient@6b4118bf0\"],\n"
		"      \"requires\": [\"git\", \"cmake\", \"msvc\", \"ffmpeg8.1\"],\n"
		"      \"verify_path\": \"src/game/client/components/custom_background.cpp\"\n"
		"    }\n"
		"  ]\n"
		"}\n";
	FILE *F = nullptr;
	if(fopen_s(&F, Path.c_str(), "wb") != 0 || !F)
	{
		Error = "无法写入清单模板: " + Path;
		return false;
	}
	fwrite(Tpl, 1, strlen(Tpl), F);
	fclose(F);
	return true;
}

// ------------------------------------------------------ 单文件 mod (.dmod) ---
std::string FileNameOf(const std::string &Path)
{
	size_t Pos = Path.find_last_of("\\/");
	return Pos == std::string::npos ? Path : Path.substr(Pos + 1);
}

bool IsDmodFile(const std::string &Path)
{
	std::string Name = FileNameOf(Path);
	if(Name.size() < 5)
		return false;
	std::string Ext = Name.substr(Name.size() - 5);
	for(char &C : Ext)
		C = (char)tolower((unsigned char)C);
	return Ext == ".dmod";
}

std::string ReadFileText(const std::string &Path, bool &Ok)
{
	Ok = false;
	FILE *F = nullptr;
	if(fopen_s(&F, Path.c_str(), "rb") != 0 || !F)
		return {};
	std::string Text;
	char Buf[8192];
	size_t N;
	while((N = fread(Buf, 1, sizeof(Buf), F)) > 0)
		Text.append(Buf, N);
	fclose(F);
	Ok = true;
	return Text;
}

bool WriteFileText(const std::string &Path, const std::string &Text)
{
	FILE *F = nullptr;
	if(fopen_s(&F, Path.c_str(), "wb") != 0 || !F)
		return false;
	size_t Written = Text.empty() ? 0 : fwrite(Text.data(), 1, Text.size(), F);
	fclose(F);
	return Written == Text.size();
}

// .dmod = 一个 zip 容器（内含 module.json + 补丁 + 可选 files/docs），
// 用系统自带 tar.exe 解开，因此不需要任何 zip 库。
bool LoadDmodFile(const std::string &DmodPath, const std::string &TempRoot, ModuleInfo &Out, std::string &Error)
{
	if(!PathExists(DmodPath))
	{
		Error = "mod 文件不存在: " + DmodPath;
		return false;
	}
	std::string Name = FileNameOf(DmodPath);
	std::string Base = Name;
	size_t Dot = Base.find_last_of('.');
	if(Dot != std::string::npos)
		Base = Base.substr(0, Dot);
	std::string Dir = JoinPath(TempRoot, Base);
	RemoveTree(Dir);
	MakeDirs(Dir);
	ProcessResult R = RunProcess("tar", {"-xf", DmodPath, "-C", Dir}, "", nullptr);
	if(!R.Ok())
	{
		Error = "无法解开 mod（不是有效的 .dmod？需要系统自带 tar.exe，Win10+ 均有）";
		return false;
	}
	std::string Mj = JoinPath(Dir, "module.json");
	if(!PathExists(Mj))
	{
		Error = "mod 内缺少 module.json —— 每个 mod 必须自描述（id/name/version/patch 等）";
		return false;
	}
	bool Ok = false;
	std::string Text = ReadFileText(Mj, Ok);
	JsonValue Root;
	std::string JErr;
	if(!Ok || !JsonParse(Text, Root, JErr))
	{
		Error = "module.json 解析失败: " + JErr;
		return false;
	}
	Out = ModuleInfo{};
	Out.Id = Root.GetString("id");
	Out.Name = Root.GetString("name", Out.Id);
	Out.Version = Root.GetString("version", "0.0.0");
	Out.Description = Root.GetString("description");
	Out.Type = Root.GetString("type", "source-patch");
	Out.BaseHint = Root.GetString("base_hint");
	Out.DocsUrl = Root.GetString("docs_url");
	Out.TestedOn = JsonToStringList(Root.GetArray("tested_on"));
	Out.SupportedVersions = JsonToStringList(Root.GetArray("supported_versions"));
	Out.Requires = JsonToStringList(Root.GetArray("requires"));
	if(const JsonValue *Ver = Root.Find("verified"); Ver && Ver->T == JsonValue::Type::Object)
		for(const auto &KV : Ver->Obj)
			if(KV.second.T == JsonValue::Type::Bool)
				Out.Verified[KV.first] = KV.second.Bool;
	Out.VerifyPath = Root.GetString("verify_path");
	std::string PatchRel = Root.GetString("patch", "patch/module.patch");
	// 多基线补丁：patches{"tclient": "...", "ddnet": "..."}——同一个模块给不同上游基线各带一份补丁，
	// 安装时按当前来源 id 选（没有对应项或没写 patches 就用默认 patch）。
	if(const JsonValue *Ps = Root.Find("patches"); Ps && Ps->T == JsonValue::Type::Object)
		for(const auto &KV : Ps->Obj)
			if(KV.second.T == JsonValue::Type::String)
				Out.Patches[KV.first] = JoinPath(Dir, KV.second.Str);   // 绝对路径，便于直接选用
	Out.ResolvedPatch = PatchRel;
	Out.PatchPath = JoinPath(Dir, PatchRel);
	Out.DmodPath = DmodPath;
	Out.Origin = "mods\\" + Name;
	if(Out.Id.empty())
	{
		Error = "module.json 缺少 id";
		return false;
	}
	if(Out.Type == "source-patch" && !PathExists(Out.PatchPath))
	{
		Error = "mod 内找不到补丁文件: " + PatchRel;
		return false;
	}
	return true;
}

// ================================================================ state ======
bool InstallState::Load(const std::string &Path, InstallState &Out)
{
	if(!PathExists(Path))
		return false;
	FILE *F = nullptr;
	if(fopen_s(&F, Path.c_str(), "rb") != 0 || !F)
		return false;
	std::string Text;
	char Buf[4096];
	size_t N;
	while((N = fread(Buf, 1, sizeof(Buf), F)) > 0)
		Text.append(Buf, N);
	fclose(F);
	JsonValue Root;
	std::string Err;
	if(!JsonParse(Text, Root, Err))
		return false;
	Out.SourceId = Root.GetString("source_id");
	Out.Version = Root.GetString("version");
	Out.WorkDir = Root.GetString("work_dir");
	Out.DistDir = Root.GetString("dist_dir");
	Out.Modules = JsonToStringList(Root.GetArray("modules"));
	Out.Timestamp = Root.GetString("timestamp");
	Out.Built = Root.GetBool("built", false);
	return true;
}

bool InstallState::Save(const std::string &Path, const InstallState &In)
{
	std::ostringstream O;
	O << "{\n";
	O << "  \"source_id\": \"" << JsonEscape(In.SourceId) << "\",\n";
	O << "  \"version\": \"" << JsonEscape(In.Version) << "\",\n";
	O << "  \"work_dir\": \"" << JsonEscape(In.WorkDir) << "\",\n";
	O << "  \"dist_dir\": \"" << JsonEscape(In.DistDir) << "\",\n";
	O << "  \"timestamp\": \"" << JsonEscape(In.Timestamp) << "\",\n";
	O << "  \"built\": " << (In.Built ? "true" : "false") << ",\n";
	O << "  \"modules\": [";
	for(size_t i = 0; i < In.Modules.size(); ++i)
		O << (i ? ", " : "") << "\"" << JsonEscape(In.Modules[i]) << "\"";
	O << "]\n}\n";
	FILE *F = nullptr;
	if(fopen_s(&F, Path.c_str(), "wb") != 0 || !F)
		return false;
	std::string S = O.str();
	fwrite(S.data(), 1, S.size(), F);
	fclose(F);
	return true;
}

// =============================================================== github ======
// ---------------------------------------------------------------- 版本号规范化 --
// 用户硬要求：**无论走哪条数据源（git / releases API / tags API），拉到的版本列表必须一样，
// 且最新版排在最上面**。所以所有来源都必须经过同一套"过滤 + 排序"。
namespace
{
	struct VerKeyT
	{
		std::vector<int> Parts;   // 20.1 -> {20,1}
		bool Prefixed = false;    // 标签带 v/V 前缀（fork 自己的发布常用这种写法）
		bool IsRc = false;
		int Rc = 0;
		bool Ok = false;          // false = 不是版本号样式，应从列表剔除
	};

	// 认识这些：20.1 / 19.9 / 0.6.0 / V10.9.0 / v10.9.0 / 20.1-rc2
	// 其余（pr-xxx、xxx-headless、languageadd_german1、0.6.0-release…）一律不算版本号。
	VerKeyT ParseVerKey(const std::string &Tag)
	{
		VerKeyT K;
		std::string S = Tag;
		if(!S.empty() && (S[0] == 'v' || S[0] == 'V'))
		{
			K.Prefixed = true;
			S = S.substr(1);
		}
		const size_t Dash = S.find('-');
		if(Dash != std::string::npos)
		{
			const std::string Suf = S.substr(Dash + 1);
			if(Suf.rfind("rc", 0) != 0 || Suf.size() < 3)
				return K;
			for(size_t i = 2; i < Suf.size(); ++i)
				if(!isdigit((unsigned char)Suf[i]))
					return K;
			K.IsRc = true;
			K.Rc = atoi(Suf.substr(2).c_str());
			S = S.substr(0, Dash);
		}
		if(S.empty())
			return K;
		size_t i = 0;
		while(i < S.size())
		{
			const size_t B = i;
			while(i < S.size() && isdigit((unsigned char)S[i]))
				++i;
			if(i == B)
				return K;
			K.Parts.push_back(atoi(S.substr(B, i - B).c_str()));
			if(i < S.size())
			{
				if(S[i] != '.')
					return K;
				++i;
				if(i >= S.size())
					return K;   // 结尾是点
			}
		}
		if(K.Parts.size() < 2)
			return K;               // 至少要 x.y
		K.Ok = true;
		return K;
	}

	// 从新到旧：先比版本号数值；同数值时正式版在 rc 之前；都是 rc 则 rc 号大的在前。
	bool VerNewer(const std::string &A, const std::string &B)
	{
		const VerKeyT Ka = ParseVerKey(A), Kb = ParseVerKey(B);
		if(Ka.Parts != Kb.Parts)
			return Ka.Parts > Kb.Parts;
		if(Ka.IsRc != Kb.IsRc)
			return !Ka.IsRc;
		if(Ka.IsRc && Ka.Rc != Kb.Rc)
			return Ka.Rc > Kb.Rc;
		return false;
	}

	// 统一规范化：剔除不是版本号的 → （按开关）剔除测试版 → 去重 → 按版本号从新到旧
	void NormalizeVersions(std::vector<GameVersion> &V, bool IncludePrerelease)
	{
		std::vector<GameVersion> Keep;
		for(const auto &X : V)
		{
			const VerKeyT K = ParseVerKey(X.Tag);
			if(!K.Ok)
				continue;                                   // 不是版本号样式（pr-*、*-headless 等）
			if(!IncludePrerelease && (K.IsRc || X.Prerelease))
				continue;                                   // 关闭"测试版"时只留正式版
			Keep.push_back(X);
		}
		std::stable_sort(Keep.begin(), Keep.end(), [](const GameVersion &A, const GameVersion &B) {
			const VerKeyT Ka = ParseVerKey(A.Tag), Kb = ParseVerKey(B.Tag);
			// 同一个仓库里若"带 v/V 前缀"和"不带前缀"两套编号都有，通常前者是它自己的发布、
			// 后者是从上游继承来的标签（TClient 就是：V10.9.0 是它的正式版，0.x~16.x 是继承的）。
			// 让前缀那组排在前面，否则它自己的最新版会被埋到很下面。
			// 只有一套编号的仓库（如 DDNet 全是不带前缀的）此条不产生任何影响。
			if(Ka.Prefixed != Kb.Prefixed)
				return Ka.Prefixed;
			if(VerNewer(A.Tag, B.Tag))
				return true;
			if(VerNewer(B.Tag, A.Tag))
				return false;
			return false;
		});
		std::vector<std::string> Seen;
		std::vector<GameVersion> Uniq;
		for(const auto &X : Keep)
		{
			bool Dup = false;
			for(const auto &S : Seen)
				if(S == X.Tag) { Dup = true; break; }
			if(!Dup)
			{
				Seen.push_back(X.Tag);
				Uniq.push_back(X);
			}
		}
		V.swap(Uniq);
	}
}

bool Installer::FetchVersions(const GameSource &Src, std::vector<GameVersion> &Out, std::string &Error)
{
	Out.clear();
	// ① 首选 git：一次拿全所有标签、不消耗 API 配额，而且与"镜像/代理哪条路通"无关 ——
	//    这是让"无论怎么做列表都一样"成立的根基。API 只在 git 拿不到时才用。
	auto TryGitTags = [&]() {
		std::vector<std::string> GitArgs = {"-c", "http.lowSpeedLimit=1000", "-c", "http.lowSpeedTime=45"};
		if(!Net.MirrorPrefix.empty())
			GitArgs.insert(GitArgs.end(), {"-c", "url." + Net.MirrorPrefix + "https://github.com/.insteadOf=https://github.com/"});
		if(!Net.Proxy.empty() && !Net.Direct)
			GitArgs.insert(GitArgs.end(), {"-c", "http.proxy=http://" + Net.Proxy, "-c", "https.proxy=http://" + Net.Proxy});
		GitArgs.push_back("ls-remote");
		GitArgs.push_back("--tags");
		GitArgs.push_back("--refs");
		GitArgs.push_back("https://github.com/" + Src.Repo + ".git");
		if(Log)
			Log(LogLevel::Info, "  用 git 读取全部标签（不消耗 API 配额）…");
		ProcessResult G = RunProcess("git", GitArgs, "", nullptr);
		if(!G.Ok())
		{
			if(Log)
				Log(LogLevel::Warn, "  git 读取标签失败（退出码 " + std::to_string(G.ExitCode) + "），改用 API");
			return;
		}
		// 每行形如：<sha>\trefs/tags/<tag>
		std::istringstream Is(G.Output);
		std::string Line;
		while(std::getline(Is, Line))
		{
			while(!Line.empty() && (Line.back() == '\r' || Line.back() == ' ' || Line.back() == '\t'))
				Line.pop_back();
			const size_t Tab = Line.find('\t');
			if(Tab == std::string::npos)
				continue;
			const std::string RefName = Line.substr(Tab + 1);
			const std::string Prefix = "refs/tags/";
			if(RefName.rfind(Prefix, 0) != 0)
				continue;
			const std::string Tag = RefName.substr(Prefix.size());
			if(Tag.empty() || Tag.find("^{}") != std::string::npos)
				continue;
			GameVersion V;
			V.Tag = Tag;
			V.Name = Tag;
			V.Ref = Tag;
			Out.push_back(V);
		}
		if(Log)
			Log(LogLevel::Info, "  git 读到 " + std::to_string(Out.size()) + " 个标签");
	};
	TryGitTags();
	// 拿着接口一路退到"真正绕过代理的直连"：
	//  ① 按界面设置（可能同时带镜像/代理；两者都没勾 = 跟随 Windows 系统代理）
	//  ② 镜像不支持 api.github.com（ghproxy.net 会 403）→ 去掉镜像再试
	//  ③ 还是失败（常见：系统代理开着、出口 IP 被 GitHub 限流 403）→ 用 INTERNET_OPEN_TYPE_DIRECT
	//     **绕过系统代理**再试一次。注意：只清空 Proxy 是没用的，PRECONFIG 仍会跟随系统代理。
	// 镜像对 api.github.com 的支持情况只需判定一次：一旦发现不支持，本次拉取就不再走它，
	// 也避免每次刷新都刷两条"镜像 403"警告（用户会误以为自己的网络/代理出问题了）。
	bool MirrorBad = false;
	auto ApiGet = [&](const std::string &Url) {
		HttpResult R = Net.GetString(Url);
		if(!R.Ok && !Net.MirrorPrefix.empty() && !MirrorBad)
		{
			MirrorBad = true;
			if(Log)
				Log(LogLevel::Info, "  这个镜像不代理 api.github.com（" + R.Error + "）——本次改为不走镜像；换用支持 api 的镜像可避免这一步");
			const std::string Saved = Net.MirrorPrefix;
			Net.MirrorPrefix.clear();
			R = Net.GetString(Url);
			Net.MirrorPrefix = Saved;
		}
		if(!R.Ok && !Net.Direct)
		{
			if(Log)
			{
				// 403 / 429 是 GitHub 接口限流（未登录 60 次/小时）——**与代理无关**，
				// 以前这里一律说"可能被系统代理拦了/请确认代理软件在运行"，把用户带偏。
				if(R.Status == 403 || R.Status == 429)
					Log(LogLevel::Warn, "  GitHub 接口返回 HTTP " + std::to_string(R.Status) + "（未登录每小时 60 次的上限，或被出口 IP 限流）——这不是代理的问题；本次请求改为直连，若仍失败会自动改用 git 取版本");
				else if(Net.Proxy.empty())
					Log(LogLevel::Warn, "  仍失败（" + R.Error + "）——可能被 Windows 系统代理拦了（当前系统代理开着就会走它）；本次请求改为绕过所有代理直连重试");
				else
					Log(LogLevel::Warn, "  经代理 " + Net.Proxy + " 失败（" + R.Error + "）——本次请求改为直连重试（不改动你勾选的代理设置；若直连也失败再确认代理软件是否在运行）");
			}
			// 回退只作用于"这一次请求"：临时直连，试完立即把用户设置还回去。
			// 以前这里把 Proxy 清空、Direct 永久置 true，导致后续请求全部忽略用户勾的代理。
			const std::string SavedProxy = Net.Proxy;
			Net.Proxy.clear();
			Net.Direct = true;
			R = Net.GetString(Url);
			Net.Proxy = SavedProxy;
			Net.Direct = false;
		}
		return R;
	};
	// ② git 没拿到才用 API（releases 含 tag 与时间）；失败再退到 tags。
	//    per_page 提到 100：尽量取全，结果才能与 git 那份保持一致。
	std::string Api = "https://api.github.com/repos/" + Src.Repo + "/releases?per_page=100";
	HttpResult R;   // 若 git 已经拿到标签就不打 API：省配额，也避免列表被 API 的部分结果覆盖
	if(Out.empty())
		R = ApiGet(Api);
	if(R.Ok)
	{
		JsonValue Root;
		std::string Err;
		if(JsonParse(R.Body, Root, Err) && Root.T == JsonValue::Type::Array)
		{
			for(const auto &Rel : Root.Arr)
			{
				GameVersion V;
				V.Tag = Rel.GetString("tag_name");
				V.Name = Rel.GetString("name", V.Tag);
				V.PublishedAt = Rel.GetString("published_at");
				V.Prerelease = Rel.GetBool("prerelease");
				V.Ref = V.Tag;
				if(!V.Tag.empty())
					Out.push_back(V);
			}
		}
		if(JsonParse(R.Body, Root, Err) && Root.T == JsonValue::Type::Object && !Out.empty())
			Error = "未知响应";
	}
	if(Out.empty())
	{
		// 退路：tags API。若 releases 已经明确是限流（403/429），tags 必然同样被限，
		// 再发一次请求只会白白消耗配额并多刷一条警告 —— 直接跳到下面的 git 兜底。
		const bool ReleasesRateLimited = (R.Status == 403 || R.Status == 429);
		if(ReleasesRateLimited)
		{
			if(Log)
				Log(LogLevel::Info, "  releases 已被限流（HTTP " + std::to_string(R.Status) + "），跳过 tags 接口以免继续消耗配额");
			Error = "GitHub 接口限流（HTTP " + std::to_string(R.Status) + "，未登录每小时 60 次；稍后再试即可，与代理无关）";
		}
		else
		{
		std::string TagsApi = "https://api.github.com/repos/" + Src.Repo + "/tags?per_page=100";
		HttpResult T = ApiGet(TagsApi);
		if(!T.Ok)
		{
			// 403 / 429 = GitHub 接口限流（未登录只有 60 次/小时），**不是代理坏了**。
			// 以前这里只报"tags 获取失败"，界面又提示"请确认代理软件在运行"，把用户引偏。
			const int St = (T.Status == 403 || T.Status == 429) ? T.Status : R.Status;
			if(St == 403 || St == 429)
				Error = "GitHub 接口限流（HTTP " + std::to_string(St) + "，未登录每小时 60 次；稍后再试即可，与代理无关）";
			else
				Error = R.Ok ? "releases 为空且 tags 获取失败" : ("无法获取版本信息: " + R.Error);
			// 注意：这里不直接失败 —— 下面还有 git 兜底
		}
		else
		{
			JsonValue Root;
			std::string Err;
			if(!JsonParse(T.Body, Root, Err) || Root.T != JsonValue::Type::Array)
			{
				Error = "版本 JSON 解析失败: " + Err;
				return false;
			}
			for(const auto &Tag : Root.Arr)
			{
				GameVersion V;
				V.Tag = Tag.GetString("name");
				V.Name = V.Tag;
				V.Ref = V.Tag;
				if(!V.Tag.empty())
					Out.push_back(V);
			}
		}
		}   // 结束"tags API"分支
	}
	if(Out.empty())
	{
		// 兜底：改用 git 协议直接读标签。GitHub API 未登录只有 60 次/小时，
		// 被限流（HTTP 403）时这条路**不消耗 API 配额**，用户照样能选版本。
		std::vector<std::string> GitArgs = {"-c", "http.lowSpeedLimit=1000", "-c", "http.lowSpeedTime=45"};
		if(!Net.MirrorPrefix.empty())
			GitArgs.insert(GitArgs.end(), {"-c", "url." + Net.MirrorPrefix + "https://github.com/.insteadOf=https://github.com/"});
		if(!Net.Proxy.empty() && !Net.Direct)
			GitArgs.insert(GitArgs.end(), {"-c", "http.proxy=http://" + Net.Proxy, "-c", "https.proxy=http://" + Net.Proxy});
		GitArgs.push_back("ls-remote");
		GitArgs.push_back("--tags");
		GitArgs.push_back("--refs");
		GitArgs.push_back("https://github.com/" + Src.Repo + ".git");
		if(Log)
			Log(LogLevel::Info, "  API 没拿到版本 —— 改用 git 读取标签（不消耗 API 配额）…");
		ProcessResult G = RunProcess("git", GitArgs, "", nullptr);
		if(G.Ok())
		{
			// 每行形如：<sha>\trefs/tags/<tag>
			std::istringstream Is(G.Output);
			std::string Line;
			while(std::getline(Is, Line))
			{
				while(!Line.empty() && (Line.back() == '\r' || Line.back() == ' ' || Line.back() == '\t'))
					Line.pop_back();
				const size_t Tab = Line.find('\t');
				if(Tab == std::string::npos)
					continue;
				const std::string RefName = Line.substr(Tab + 1);
				const std::string Prefix = "refs/tags/";
				if(RefName.rfind(Prefix, 0) != 0)
					continue;
				std::string Tag = RefName.substr(Prefix.size());
				if(Tag.empty() || Tag.find("^{}") != std::string::npos)
					continue;
				GameVersion V;
				V.Tag = Tag;
				V.Name = Tag;
				V.Ref = Tag;
				Out.push_back(V);
			}
			if(!Out.empty() && Log)
				Log(LogLevel::Info, "  已用 git 读取到 " + std::to_string(Out.size()) + " 个标签");
		}
		else if(Log)
			Log(LogLevel::Warn, "  git 读取标签也失败（" + std::to_string(G.ExitCode) + "）");
	}
	// ③ 统一规范化：无论上面走的是 git 还是 API，最终列表都按同一规则过滤 + 排序，
	//    保证"无论怎么做，拉取到的版本列表一样，且最新版在最上面"。
	NormalizeVersions(Out, IncludePrerelease);
	// 透明说明：这个来源的标签里若混着两套编号（自己的发布 + 继承上游的），
	// 就在日志里说明为什么这么排，免得用户以为列表乱了。
	{
		int PrefN = 0, PlainN = 0;
		for(const auto &X : Out)
			(ParseVerKey(X.Tag).Prefixed ? PrefN : PlainN)++;
		if(PrefN > 0 && PlainN > 0 && Log)
			Log(LogLevel::Info, "  共 " + std::to_string(PrefN) + " 个带 v/V 前缀的版本（该来源自己的发布，已排在前面）与 " +
						 std::to_string(PlainN) + " 个从上游继承来的版本");
	}
	if(Out.empty())
	{
		if(Error.empty())
			Error = "没有拿到任何版本（仓库里没有版本号样式的标签，或网络被拦）";
		return false;
	}
	return true;
}
