#include "ipc_window.hpp"
#include "console_window.hpp"
#include "basedef.hpp"
#include <ShlObj.h>
using namespace std;

void app::ipc::IPCWindow::onCreated() {
	text(getUserIdentifier(L""));
}

void app::ipc::IPCWindow::onClose(EventData& ev) {
	for (auto& i : app::windows) i->post(WM_CLOSE);
	SendMessageW(hwnd, WM_NULL, 0, 0);
}

template<size_t N> inline void ensureNullStr(WCHAR(&a)[N]) {
	a[std::size(a) - 1] = 0;
}

void app::ipc::IPCWindow::requestCreateConsole(EventData& ev) {
	HANDLE hProcess = OpenProcess(PROCESS_VM_READ, false, (DWORD)ev.wParam);
	if (!hProcess) {
		ev.returnValue(ERROR_INVALID_PARAMETER);
		return;
	}
	auto req = make_unique<CreateConsoleRequestInfo>(); SIZE_T readed{};
	if (!ReadProcessMemory(hProcess, (PVOID)ev.lParam, req.get(), sizeof(CreateConsoleRequestInfo), &readed)
		|| readed != sizeof(CreateConsoleRequestInfo)) {
		CloseHandle(hProcess);
		ev.returnValue(STATUS_ACCESS_VIOLATION);
		return;
	}
	CloseHandle(hProcess);
	ensureNullStr(req->lpApplication);
	ensureNullStr(req->lpCommand);
	ensureNullStr(req->lpCurrentDirectory);
	auto pWindow = shared_ptr<ui::ConsoleWindow>(new ui::ConsoleWindow(req->x, req->y, req->w, req->h));
	pWindow->create();
	if (pWindow->SpawnApplication(
		req->lpApplication[0] ? req->lpApplication : NULL,
		req->lpCommand[0] ? req->lpCommand : NULL,
		req->lpCurrentDirectory
	) == false) {
		ev.returnValue(GetLastError());
		DestroyWindow(*pWindow);
		return;
	}
	pWindow->show(req->nCmdShow);
	if (req->nCmdShow) pWindow->focus();
	app::windows.push_back(std::move(pWindow));
}

std::wstring app::ipc::IPCWindow::getUserIdentifier(std::wstring userId) {
	WCHAR username[256]{};
	DWORD size = 256;
	GetUserNameW(username, &size);
	BOOL IsAdmin = IsUserAnAdmin();
	return format(L"IPC Window: User:[{}];Admin?:[{}];UserId:[{}]", username, IsAdmin, userId);
}
