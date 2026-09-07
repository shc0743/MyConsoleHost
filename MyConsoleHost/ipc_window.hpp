#include "targetver.h"
#include "../../w32oop/w32use.hpp"
#include <format>

namespace app::ipc {
	constexpr UINT IPC_RequestCreateConsole = WM_USER + 0x11;

	class IPCWindow : public Window {
	public:
		IPCWindow() :Window(L"IPC Window", 1, 1, 0, 0, WS_OVERLAPPED, 0) {}

		const std::wstring get_class_name() const override {
			using w32oop::util::str::converts::str_wstr;
			return std::format(L"[MyConsoleHost IPC Window] HashCode:{} Class:{}",
				typeid(*this).hash_code(), str_wstr(typeid(*this).raw_name()));
		}

	protected:
		void onCreated() override;

		void requestCreateConsole(EventData& ev);

		void setup_event_handlers() override {
			WINDOW_add_handler(IPC_RequestCreateConsole, requestCreateConsole);
		}

	public:
		std::wstring getUserIdentifier();
	};

	struct CreateConsoleRequestInfo {
		WCHAR lpApplication[32768];
		WCHAR lpCommand[32768];
		WCHAR lpCurrentDirectory[32768];
		int x, y, w, h;
		int nCmdShow;
	};
}
