// ddnet-module-installer - core engine implementation
#include "core.h"

#include <windows.h>
#include <wininet.h>
#include <shlobj.h>
#include <shellapi.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <sstream>

#pragma comment(lib, "wininet.lib")

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

bool CopyTree(const std::string &From, const std::string &To, std::string &Error)
{
	std::wstring Cmd = L"cmd.exe /c xcopy \"" + Utf8ToWide(From) + L"\" \"" + Utf8ToWide(To) + L"\" /E /I /Y /Q >NUL";
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
	R.Ok = (R.Status >= 200 && R.Status < 300) && Got > 0;
	if(!R.Ok)
		R.Error = R.Error.empty() ? ("下载不完整 HTTP " + std::to_string(R.Status)) : R.Error;
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
	Out.Requires = JsonToStringList(Root.GetArray("requires"));
	if(const JsonValue *Ver = Root.Find("verified"); Ver && Ver->T == JsonValue::Type::Object)
		for(const auto &KV : Ver->Obj)
			if(KV.second.T == JsonValue::Type::Bool)
				Out.Verified[KV.first] = KV.second.Bool;
	Out.VerifyPath = Root.GetString("verify_path");
	std::string PatchRel = Root.GetString("patch", "patch/module.patch");
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
bool Installer::FetchVersions(const GameSource &Src, std::vector<GameVersion> &Out, std::string &Error)
{
	Out.clear();
	// 拿着接口一路退到"真正绕过代理的直连"：
	//  ① 按界面设置（可能同时带镜像/代理；两者都没勾 = 跟随 Windows 系统代理）
	//  ② 镜像不支持 api.github.com（ghproxy.net 会 403）→ 去掉镜像再试
	//  ③ 还是失败（常见：系统代理开着、出口 IP 被 GitHub 限流 403）→ 用 INTERNET_OPEN_TYPE_DIRECT
	//     **绕过系统代理**再试一次。注意：只清空 Proxy 是没用的，PRECONFIG 仍会跟随系统代理。
	auto ApiGet = [&](const std::string &Url) {
		HttpResult R = Net.GetString(Url);
		if(!R.Ok && !Net.MirrorPrefix.empty())
		{
			if(Log)
				Log(LogLevel::Warn, "  镜像 " + Net.MirrorPrefix + " 不支持该接口（" + R.Error + "），去掉镜像重试");
			const std::string Saved = Net.MirrorPrefix;
			Net.MirrorPrefix.clear();
			R = Net.GetString(Url);
			Net.MirrorPrefix = Saved;
		}
		if(!R.Ok && !Net.Direct)
		{
			if(Log)
			{
				if(Net.Proxy.empty())
					Log(LogLevel::Warn, "  仍失败（" + R.Error + "）——可能被 Windows 系统代理拦了（当前系统代理开着就会走它）；" + "本次改为绕过所有代理直连重试");
				else
					Log(LogLevel::Warn, "  代理 " + Net.Proxy + " 不可用（" + R.Error + "）——请确认代理软件在运行、出口没被限流；本次改为绕过所有代理直连重试");
			}
			Net.Proxy.clear();
			Net.Direct = true;   // 本次运行后续请求也一律直连
			R = Net.GetString(Url);
		}
		return R;
	};
	// 先试 GitHub releases API（含 tag 与时间）；失败再退到 tags
	std::string Api = "https://api.github.com/repos/" + Src.Repo + "/releases?per_page=30";
	HttpResult R = ApiGet(Api);
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
		// 退路：tags API
		std::string TagsApi = "https://api.github.com/repos/" + Src.Repo + "/tags?per_page=30";
		HttpResult T = ApiGet(TagsApi);
		if(!T.Ok)
		{
			Error = R.Ok ? "releases 为空且 tags 获取失败" : ("无法获取版本信息: " + R.Error);
			return false;
		}
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
	if(Out.empty())
	{
		Error = "没有拿到任何版本（仓库可能没有 release/tag，或网络被拦）";
		return false;
	}
	return true;
}
