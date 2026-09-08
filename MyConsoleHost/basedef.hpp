#include "targetver.h"
#include "../../w32oop/w32use.hpp"
#include <vector>
#include <memory>

namespace app {
	// stub
	namespace ui {
		class ConsoleWindow;
	}
	namespace ipc {
		class IPCWindow;
	}

	// extern
	extern std::vector<std::shared_ptr<Window>> windows;
	extern std::unique_ptr<ipc::IPCWindow> ipcWindow;

	// func
	bool CreateConsoleWindow(HWND pIPC, LPCWSTR lpApplication, LPCWSTR lpCommand, LPSTARTUPINFOW lpStartupInfo);
}