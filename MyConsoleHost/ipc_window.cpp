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
	CreateConsoleRequestInfo req{}; SIZE_T readed{};
	if (!ReadProcessMemory(hProcess, (PVOID)ev.lParam, &req, sizeof(req), &readed) || readed != sizeof(req)) {
		CloseHandle(hProcess);
		ev.returnValue(STATUS_ACCESS_VIOLATION);
		return;
	}
	CloseHandle(hProcess);
	auto pWindow = shared_ptr<ui::ConsoleWindow>(new ui::ConsoleWindow(req.x, req.y, req.w, req.h));
	pWindow->create();
	if (pWindow->SpawnApplication(req.lpApplication, req.lpCommand) == false) {
		ev.returnValue(GetLastError());
		DestroyWindow(*pWindow);
		return;
	}
	pWindow->show(req.nCmdShow);
	pWindow->focus();
	app::windows.push_back(std::move(pWindow));
}