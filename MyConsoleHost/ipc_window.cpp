#include "ipc_window.hpp"
#include "console_window.hpp"
#include "basedef.hpp"
using namespace std;

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
	auto pWindow = shared_ptr<ui::ConsoleWindow>(new ui::ConsoleWindow(req->x, req->y, req->w, req->h));
	pWindow->create();
	if (pWindow->SpawnApplication(
		req->lpApplication[0] ? req->lpApplication : NULL,
		req->lpCommand[0] ? req->lpCommand : NULL
	) == false) {
		ev.returnValue(GetLastError());
		DestroyWindow(*pWindow);
		return;
	}
	pWindow->show(req->nCmdShow);
	pWindow->focus();
	app::windows.push_back(std::move(pWindow));
}