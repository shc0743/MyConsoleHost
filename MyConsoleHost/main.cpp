#include "targetver.h"
#include "basedef.hpp"
#include "ipc_window.hpp"
#include "console_window.hpp"
#include "renderer_main.hpp"
#include "../lib/CLI11.hpp"
using namespace std;

#pragma comment(linker,"\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#pragma comment(lib, "comctl32.lib")

namespace app {
	vector<shared_ptr<Window>> windows;
	unique_ptr<ipc::IPCWindow> ipcWindow;
	HANDLE hSandboxProcess;
	//HANDLE hSandboxJob;
}
HINSTANCE hInst;

int SandboxContainerStartup(bool noError);
int SandboxContainerMain(HANDLE hProcess);

int APIENTRY wWinMain(
	_In_ HINSTANCE hInstance,
	_In_opt_ HINSTANCE hPrevInstance,
	_In_ LPWSTR lpCmdLine,
	_In_ int nShowCmd
) {
	::hInst = hInstance;
	if (FAILED(CoInitializeEx(NULL, COINIT_APARTMENTTHREADED))) __fastfail(FAST_FAIL_FATAL_APP_EXIT);
	w32oop::util::RAIIHelper comUninit([] { CoUninitialize(); });

	using namespace w32oop::util::str::encodings;
	CLI::App app;
	app.allow_extras();
	string type;
	string userId;
	bool internal = false;
	bool hidden = false;
	bool noUI = false;
	bool newInstance = false;
	bool standalone = false;
	bool wait = false;
	bool forkServer = false;
	DWORD serverOutProcess = 0;
	ULONGLONG serverOutAddress = 0;
	ULONGLONG serverOutEvent = 0;
	ULONGLONG clientId = 0;
	app.add_flag("--type", type);
	app.add_flag("--user-id", userId);
	app.add_flag("--internal", internal);
	app.add_flag("--hidden", hidden);
	app.add_flag("--no-ui", noUI);
	app.add_flag("--new-instance", newInstance);
	app.add_flag("--standalone", standalone);
	app.add_flag("--wait", wait);
	app.add_flag("--fork-server", forkServer);
	app.add_option("--server-out-process", serverOutProcess);
	app.add_option("--server-out-address", serverOutAddress);
	app.add_option("--server-out-event", serverOutEvent);
	app.add_option("--client-id", clientId);
	try { app.parse(utf16_utf8(GetCommandLineW()), true); }
	catch (exception& exc) {
		fputs("0\r\n", stdout);
		fputs(exc.what(), stderr);
		// this is only debug purpose; do not depend in production!
		if (wstring(lpCmdLine).find(L"--no-ui") == wstring::npos) MessageBoxW(NULL, format(L"{}: {}",
			utf8_utf16(typeid(exc).name()), utf8_utf16(exc.what())).c_str(), L"Console", 0x10);
		return ERROR_INVALID_PARAMETER;
	}

	if (internal && type == "sandbox") {
		return SandboxContainerMain((HANDLE)clientId);
	}

	if (internal && type == "renderer") {
		RendererMainData data;
		return RendererMain(data);
	}

	if (!forkServer) {
		// if stdio is redirected, communicate with cmd and other apps might broken
		// so fork a new process detached to avoid io redirect
		volatile HWND outHwnd{};
		SECURITY_ATTRIBUTES sa{ .nLength = sizeof(sa), .lpSecurityDescriptor = 0, .bInheritHandle = true };
		HANDLE hEvent = CreateEventW(&sa, FALSE, FALSE, NULL);
		if (!hEvent) return GetLastError();
		wstring newCmd = format(L"- --type=main --fork-server --user-id=\"{}\" --server-out-process={} "
			"--server-out-address={} --server-out-event={} ", utf8_utf16(userId), GetCurrentProcessId(),
			(ULONGLONG)&outHwnd, (ULONGLONG)hEvent);
		newCmd += lpCmdLine;
		STARTUPINFOW si{}; PROCESS_INFORMATION pi{};
		GetStartupInfoW(&si);
		si.dwFlags &= ~STARTF_USESTDHANDLES;
		si.hStdInput = si.hStdOutput = si.hStdError = 0;
		try {
			auto app = make_unique<WCHAR[]>(32768);
			GetModuleFileNameW(NULL, app.get(), 32768);
			STARTUPINFOEXW siex{};
			std::unique_ptr<uint8_t[]> attributeList;
			SIZE_T need{};
			bool ok = false;
			HANDLE hList[] = { hEvent };
			InitializeProcThreadAttributeList(0, 1, 0, &need);
			if (need && need < 32768) {
				attributeList = make_unique<uint8_t[]>(need);
				if (InitializeProcThreadAttributeList((PPROC_THREAD_ATTRIBUTE_LIST)attributeList.get(),
					1, 0, &need)) {
					if (UpdateProcThreadAttribute(
						(PPROC_THREAD_ATTRIBUTE_LIST)attributeList.get(), 0,
						PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
						&hList, // only 1 handle
						sizeof(HANDLE),
						NULL, NULL
					)) {
						ok = true;
					}
					else {
						DeleteProcThreadAttributeList((PPROC_THREAD_ATTRIBUTE_LIST)attributeList.get());
					}
				}
			}
			if (!ok) {
				throw runtime_error("");
			}
			siex.StartupInfo = si;
			siex.StartupInfo.cb = sizeof(siex);
			siex.lpAttributeList = PPROC_THREAD_ATTRIBUTE_LIST(attributeList ? attributeList.get() : nullptr);
#pragma warning(push)
#pragma warning(disable: 6335)
			BOOL r = CreateProcessW(app.get(), newCmd.data(), NULL, NULL, TRUE,
				CREATE_NEW_PROCESS_GROUP | CREATE_SUSPENDED | CREATE_DEFAULT_ERROR_MODE | EXTENDED_STARTUPINFO_PRESENT,
				NULL, NULL, (LPSTARTUPINFOW)&siex, &pi);
			DWORD e = GetLastError();
			if (attributeList) DeleteProcThreadAttributeList((PPROC_THREAD_ATTRIBUTE_LIST)attributeList.get());
			SetLastError(e);
			if (!r) {
				throw runtime_error("");
			}
#pragma warning(pop)
		}
		catch (...) {
			DWORD e = GetLastError();
			if (!e) e = -1;
			if (!noUI) MessageBoxW(NULL, ErrorChecker(e).message().c_str(), L"Console", MB_ICONERROR);
			return e;
		}
		DWORD code = 0;bool c1 = false;
		ResumeThread(pi.hThread);
		CloseHandle(pi.hThread);
		if (wait) {
			WaitForSingleObject(pi.hProcess, INFINITE);
			GetExitCodeProcess(pi.hProcess, &code);
			c1 = true;
		}
		else {
			HANDLE waits[]{ pi.hProcess, hEvent };
			if (WAIT_OBJECT_0 == WaitForMultipleObjects(2, waits, false, INFINITE)) {
				GetExitCodeProcess(pi.hProcess, &code);
				c1 = true;
			}
		}
		CloseHandle(pi.hProcess);
		CloseHandle(hEvent);
		cout << to_string((ULONGLONG)outHwnd) << endl;
		if (!c1) return (outHwnd ? 0 : -1);
		return code;
	}

	auto consoleWinStart = [&app, &noUI, &hidden](HWND hWnd, bool noErr) -> int {
		STARTUPINFOW si{};
		GetStartupInfoW(&si);
		auto extra = app.remaining(true);
		if (hidden) {
			si.dwFlags |= STARTF_USESHOWWINDOW;
			si.wShowWindow = SW_HIDE;
		}
		if (!app::CreateConsoleWindow(hWnd, 
			(extra.size() > 1) ? utf8_utf16(extra[1]).c_str() : NULL,
			(extra.size() > 0) ? utf8_utf16(extra[0]).c_str() : NULL,
			&si
		)) {
			DWORD e = GetLastError();
			if (!e) e = -1;
			if (!noUI && ! noErr) thread([](DWORD e) {
				MessageBoxW(NULL, ErrorChecker(e).message().c_str(), L"Console", MB_ICONERROR);
			}, e).join();
			return e;
		}
		return 0;
	};
	auto outputResult = [&serverOutProcess, &serverOutAddress, &serverOutEvent](HWND value) {
		cout << w32oop::util::str::converts::wstr_str(to_wstring((ULONG_PTR)HWND(value))) << endl;
		if (serverOutProcess && serverOutAddress) {
			HANDLE hProcess = OpenProcess(PROCESS_VM_WRITE | PROCESS_VM_OPERATION, FALSE, serverOutProcess);
			if (hProcess) {
				SIZE_T written{};
				(void)WriteProcessMemory(hProcess, (LPVOID)serverOutAddress, &value, sizeof(value), &written);
				CloseHandle(hProcess);
			}
		}
		if (serverOutEvent) {
			SetEvent((HANDLE)serverOutEvent);
		}
	};

	if (!standalone && !newInstance && !hidden) {
		// find whether already has a IPCWindow
		app::ipc::IPCWindow tmp;
		HWND h = FindWindowW(tmp.get_class_name().c_str(), tmp.getUserIdentifier(utf8_utf16(userId)).c_str());
		if (h) do {
			int ret = consoleWinStart(h, true);
			if (ret != 0) break;
			if (ret == 0) outputResult(h);
			else outputResult(0);
			return ret;
		} while (0);
	}

	Window::set_global_option(Window::Option_QuitWhenWindowAllClosed, true);
	Window::set_global_option(Window::Option_DisableDialogWindowHandling, true);

	// start sandbox container
	if (int r = SandboxContainerStartup(noUI)) return r;
	w32oop::util::RAIIHelper _sbxProc([] {
		if (app::hSandboxProcess) CloseHandle(app::hSandboxProcess);
		//if (app::hSandboxJob) CloseHandle(app::hSandboxJob);
	});

	app::ipcWindow = unique_ptr<app::ipc::IPCWindow>(new app::ipc::IPCWindow());
	app::ipcWindow->create();
	app::ipcWindow->set_main_window();
	if (standalone) app::ipcWindow->text(L"Standalone Mode");
	else app::ipcWindow->text(app::ipcWindow->getUserIdentifier(utf8_utf16(userId)));

	if (int r = consoleWinStart(*app::ipcWindow, false)) {
		if (IsWindow(*app::ipcWindow)) app::ipcWindow->dest();
		return r;
	}

	if (!hidden && app::windows.size() == 0) {
		if (IsWindow(*app::ipcWindow)) app::ipcWindow->dest();
		return ERROR_NO_DATA;
	}

	outputResult(*app::ipcWindow);
	return Window::run();
}


bool app::CreateConsoleWindow(HWND pIPC, LPCWSTR lpApplication, LPCWSTR lpCommand, LPSTARTUPINFOW lpStartupInfo) {
	auto lpCCRI = make_unique<ipc::CreateConsoleRequestInfo>();
	ipc::CreateConsoleRequestInfo& ccri = *lpCCRI.get();
	ccri.x = ccri.y = CW_USEDEFAULT;
	ccri.w = 120;
	ccri.h = 30;
	int fontSize = app::ui::ConsoleWindow::getDefaultFontSize();
	if (lpStartupInfo) {
		if (lpStartupInfo->dwFlags & STARTF_USEPOSITION) {
			ccri.x = lpStartupInfo->dwX;
			ccri.y = lpStartupInfo->dwY;
		}
		if (lpStartupInfo->dwFlags & STARTF_USESIZE) {
			ccri.w = lpStartupInfo->dwXSize / (fontSize / 2);
			ccri.h = lpStartupInfo->dwYSize / fontSize;
		}
	}
	if (lpApplication) wcscpy_s(ccri.lpApplication, lpApplication);
	if (lpCommand) wcscpy_s(ccri.lpCommand, lpCommand);
	GetCurrentDirectoryW(sizeof(ccri.lpCurrentDirectory) / sizeof(decltype(ccri.lpCurrentDirectory[0])), ccri.lpCurrentDirectory);
	ccri.nCmdShow = (lpStartupInfo && (lpStartupInfo->dwFlags & STARTF_USESHOWWINDOW)) ?
		(lpStartupInfo->wShowWindow) : SW_NORMAL;
	
	DWORD_PTR result = 0;
	if (!SendMessageTimeoutW(pIPC, ipc::IPC_RequestCreateConsole, GetCurrentProcessId(), (LPARAM)lpCCRI.get(),
		SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT, 5000, &result)) result = GetLastError();
	SetLastError((DWORD)result);
	return 0 == result;
}

