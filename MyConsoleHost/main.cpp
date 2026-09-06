#include "targetver.h"
#include "basedef.hpp"
#include "ipc_window.hpp"
#include "console_window.hpp"
#include "../lib/CLI11.hpp"
using namespace std;

#pragma comment(linker,"\"/manifestdependency:type='win32' \
name='Microsoft.Windows.Common-Controls' version='6.0.0.0' \
processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")
#pragma comment(lib, "comctl32.lib")

namespace app {
	vector<shared_ptr<ui::ConsoleWindow>> windows;
	unique_ptr<ipc::IPCWindow> ipcWindow;
}
HINSTANCE hInst;

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
	bool hidden = false;
	bool noUI = false;
	bool newInstance = false;
	app.add_flag("--hidden", hidden);
	app.add_flag("--no-ui", noUI);
	app.add_flag("--new-instance", newInstance);
	try { app.parse(utf16_utf8(GetCommandLineW()), true); }
	catch (...) {}

	auto consoleWinStart = [&app, &noUI](HWND hWnd) -> int {
		STARTUPINFOW si{};
		GetStartupInfoW(&si);
		auto extra = app.remaining(true);
		if (!app::CreateConsoleWindow(hWnd, 
			(extra.size() > 1) ? utf8_utf16(extra[1]).c_str() : NULL,
			(extra.size() > 0) ? utf8_utf16(extra[0]).c_str() : NULL,
			&si
		)) {
			DWORD e = GetLastError();
			if (!e) e = -1;
			if (!noUI) thread([](DWORD e) {
				MessageBoxW(NULL, ErrorChecker(e).message().c_str(), L"Console", MB_ICONERROR);
			}, e).join();
			return e;
		}
		return 0;
	};

	if (!newInstance && !hidden) {
		// find whether already has a IPCWindow
		auto c = app::ipc::IPCWindow().get_class_name();
		HWND h = FindWindowW(c.c_str(), NULL);
		if (h) {
			return consoleWinStart(h);
		}
	}

	Window::set_global_option(Window::Option_QuitWhenWindowAllClosed, true);
	Window::set_global_option(Window::Option_DisableDialogWindowHandling, true);

	app::ipcWindow = unique_ptr<app::ipc::IPCWindow>(new app::ipc::IPCWindow());
	app::ipcWindow->create();

	if (!hidden) {
		if (int r = consoleWinStart(*app::ipcWindow)) {
			if (IsWindow(*app::ipcWindow)) app::ipcWindow->close();
			return r;
		}
	}

	if (app::windows.size() == 0) {
		if (IsWindow(*app::ipcWindow)) app::ipcWindow->close();
		return ERROR_NO_DATA;
	}
	return Window::run();
}


bool app::CreateConsoleWindow(HWND pIPC, LPCWSTR lpApplication, LPCWSTR lpCommand, LPSTARTUPINFOW lpStartupInfo) {
	ipc::CreateConsoleRequestInfo ccri{};
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
	ccri.lpApplication = lpApplication;
	ccri.lpCommand = lpCommand;
	ccri.nCmdShow = (lpStartupInfo && (lpStartupInfo->dwFlags & STARTF_USESHOWWINDOW)) ?
		(lpStartupInfo->wShowWindow) : SW_NORMAL;
	
	DWORD_PTR result = 0;
	if (!SendMessageTimeoutW(pIPC, ipc::IPC_RequestCreateConsole, GetCurrentProcessId(), (LPARAM)&ccri,
		SMTO_ABORTIFHUNG | SMTO_BLOCK | SMTO_ERRORONEXIT, 5000, &result)) result = GetLastError();
	SetLastError((DWORD)result);
	return 0 == result;
}

