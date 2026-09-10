#include "targetver.h"
#include <ConsoleApi.h>
#include "../../w32oop/w32use.hpp"
#include <vector>
#include <mutex>
#include <string>
#include <thread>
#include <cstdint>
#include <format>

namespace app::ui {
	class ConsoleWindow : public Window {
	public:
		ConsoleWindow(int x, int y, int wc, int hc) :
			fontSize(defaultFontSize),
			wc(wc), hc(hc),
			Window(L"Console", (defaultFontSize / 2)* wc, defaultFontSize* hc, x, y,
				WS_OVERLAPPEDWINDOW | WS_VSCROLL, WS_EX_LAYERED) {
			font_name = L"Consolas";
		}
		~ConsoleWindow() {
			if (hHostedProcess) CloseHandle(hHostedProcess);
			if (hConsole) ClosePseudoConsole(hConsole);
		}

		const std::wstring get_class_name() const override {
			using w32oop::util::str::converts::str_wstr;
			return std::format(L"[MyConsoleHost Console Window] HashCode:{} Class:{}",
				typeid(*this).hash_code(), str_wstr(typeid(*this).raw_name()));
		}

	protected:
		static int defaultFontSize;
		int fontSize = 14;
		std::wstring font_name;
		int wc, hc;

		COLORREF get_window_background_color() override {
			return RgbColor("#000");
		}

		void onCreated() override;
		void onDestroy() override;
		void onNcDestroy(EventData&);

		void onClose(EventData& ev);
		void onSize(EventData& ev);
		void onMenuCommand(EventData& ev);
		void onSysMenu(EventData& ev);

		void onImeSetContext(EventData& ev);
		void onImeStartComposition(EventData& ev);
		void onImeComposition(EventData& ev);
		void onImeEndComposition(EventData& ev);
		void positionImeWindow();
		void drawImeComposition(HDC hdc, int cellW, int cellH);

		void setup_event_handlers() override {
			WINDOW_add_handler(WM_CLOSE, onClose);
			WINDOW_add_handler(WM_NCDESTROY, onNcDestroy);
			WINDOW_add_handler(WM_SIZING, onSize);
			WINDOW_add_handler(WM_SIZE, onSize);
			WINDOW_add_handler(WM_MENU_CHECKED, onMenuCommand);
			WINDOW_add_handler(WM_SYSCOMMAND, onSysMenu);
			WINDOW_add_handler(WM_IME_SETCONTEXT, onImeSetContext);
			WINDOW_add_handler(WM_IME_STARTCOMPOSITION, onImeStartComposition);
			WINDOW_add_handler(WM_IME_COMPOSITION, onImeComposition);
			WINDOW_add_handler(WM_IME_ENDCOMPOSITION, onImeEndComposition);
		}

	public:
		int getFontSize() const { return fontSize; };
		void setFontSize(int _) { fontSize = _; };
		static int getDefaultFontSize() { return defaultFontSize; }

	protected:
		bool _hosted = false;
		HANDLE hHostedProcess{};
		DWORD  hostedExitCode = 0;
		bool   hostedExited = false;
		w32ProcessHandle hRendererProcess{};
		HPCON hConsole{};
		w32FileHandle outputReadSide, inputWriteSide;
		HFONT myFont{};

		void worker();
		std::thread workerThread;

	public:
		bool SpawnApplication(_In_opt_ PCWSTR app, _In_opt_ PCWSTR cmd, _In_opt_ PCWSTR cd);

	protected:
		bool CreateRenderer();
	};
}