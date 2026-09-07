#include "targetver.h"
#include <ConsoleApi.h>
#include "../../w32oop/w32use.hpp"
#include <vector>
#include <mutex>
#include <string>
#include <thread>
#include <cstdint>
#include <format>
#include "vt_parser.hpp"

namespace app::ui {
	enum CellFlag : uint8_t {
		CELL_NORMAL = 0,
		CELL_WIDE_LEAD = 1,
		CELL_WIDE_CONT = 2,
	};

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
		static constexpr int HistoryMax = 5000;
		int totalRows = 0;
		int historyUsed = 0;
		int scrollBack = 0;

		COLORREF get_window_background_color() override {
			return RgbColor("#000");
		}

		void onCreated() override;
		void onDestroy() override;

		void onClose(EventData& ev);
		void onEraseBkgnd(EventData& ev);
		void doPaint(EventData& ev);
		void onSize(EventData& ev);
		void onKeyDown(EventData& ev);
		void onChar(EventData& ev);
		void onGetMinMaxInfo(EventData& ev);
		void onVScroll(EventData& ev);
		void onMouseWheel(EventData& ev);
		void onLButtonDown(EventData& ev);
		void onLButtonUp(EventData& ev);
		void onMouseMove(EventData& ev);
		void onRButtonUp(EventData& ev);
		void onRButtonDown(EventData& ev);
		void onMButtonDown(EventData& ev);
		void onMButtonUp(EventData& ev);
		void sendMouseEvent(int button, bool isRelease, bool isDrag, bool isWheel, int col, int row);
		void onContextMenu(EventData& ev);
		void onMenuCommand(EventData& ev);

		void onImeSetContext(EventData& ev);
		void onImeStartComposition(EventData& ev);
		void onImeComposition(EventData& ev);
		void onImeEndComposition(EventData& ev);
		void positionImeWindow();
		void drawImeComposition(HDC hdc, int cellW, int cellH);

		void onOutputNotify(EventData& ev);

		void setup_event_handlers() override {
			WINDOW_add_handler(WM_CLOSE, onClose);
			WINDOW_add_handler(WM_ERASEBKGND, onEraseBkgnd);
			WINDOW_add_handler(WM_PAINT, doPaint);
			WINDOW_add_handler(WM_SIZE, onSize);
			WINDOW_add_handler(WM_KEYDOWN, onKeyDown);
			WINDOW_add_handler(WM_CHAR, onChar);
			WINDOW_add_handler(WM_GETMINMAXINFO, onGetMinMaxInfo);
			WINDOW_add_handler(WM_VSCROLL, onVScroll);
			WINDOW_add_handler(WM_MOUSEWHEEL, onMouseWheel);
			WINDOW_add_handler(WM_LBUTTONDOWN, onLButtonDown);
			WINDOW_add_handler(WM_LBUTTONUP, onLButtonUp);
			WINDOW_add_handler(WM_MOUSEMOVE, onMouseMove);
			WINDOW_add_handler(WM_RBUTTONUP, onRButtonUp);
			WINDOW_add_handler(WM_RBUTTONDOWN, onRButtonDown);
			WINDOW_add_handler(WM_MBUTTONDOWN, onMButtonDown);
			WINDOW_add_handler(WM_MBUTTONUP, onMButtonUp);
			WINDOW_add_handler(WM_CONTEXTMENU, onContextMenu);
			WINDOW_add_handler(WM_MENU_CHECKED, onMenuCommand);
			WINDOW_add_handler(WM_IME_SETCONTEXT, onImeSetContext);
			WINDOW_add_handler(WM_IME_STARTCOMPOSITION, onImeStartComposition);
			WINDOW_add_handler(WM_IME_COMPOSITION, onImeComposition);
			WINDOW_add_handler(WM_IME_ENDCOMPOSITION, onImeEndComposition);
			WINDOW_add_handler(WMU_OUTPUT, onOutputNotify);
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
		bool   shuttingDown = false;
		HPCON hConsole{};
		w32FileHandle outputReadSide, inputWriteSide;
		HFONT myFont{};

		void worker();
		std::thread workerThread;
		void pumpJoinWorker();

		std::vector<wchar_t> buffer;
		std::vector<uint8_t> cellFlags;
		std::vector<COLORREF> fgColors;
		std::vector<COLORREF> bgColors;
		mutable std::mutex bufMutex;

		int cursorX = 0, cursorY = 0;
		bool cursorVisible = true;
		int savedCursorX = 0, savedCursorY = 0;

		bool inAltBuffer = false;
		std::vector<wchar_t> altBuffer;
		std::vector<uint8_t> altCellFlags;
		std::vector<COLORREF> altFgColors;
		std::vector<COLORREF> altBgColors;
		std::vector<wchar_t> savedViewport;
		std::vector<uint8_t> savedViewportFlags;
		std::vector<COLORREF> savedViewportFg;
		std::vector<COLORREF> savedViewportBg;
		int savedViewportCursorX = 0, savedViewportCursorY = 0;
		void enterAltBuffer(bool saveCursor);
		void exitAltBuffer(bool restoreCursor);

		int fgIndex = 7, bgIndex = 0;
		COLORREF fgRgb = 0, bgRgb = 0;
		bool fgIsRgb = false, bgIsRgb = false;
		bool bold = false, underline = false, reverse = false;

		vt::Parser vtParser;
		std::wstring pendingTitle;

		int utf8Remaining = 0;
		unsigned int utf8CodePoint = 0;

		bool selecting = false;
		bool hasSel = false;
		bool blockSelection = false;
		bool wordSelecting = false;
		int selAX = 0, selAY = 0, selBX = 0, selBY = 0;
		ULONGLONG lastClickTick = 0;
		int lastClickCol = -1, lastClickRow = -1;

		bool imeComposing = false;
		std::wstring imeComp;

		int wheelRemainder = 0;
		int mouseTrackingMode = 0;
		bool sgrMouseMode = false;
		int pressedMouseButton = -1;

		void processOutput(const char* data, DWORD len);
		void processChar(wchar_t ch);
		void putPrintable(wchar_t ch);
		void onVtPrint(wchar_t ch);
		void onVtCtrl(wchar_t ch);
		void onVtCsi(wchar_t finalByte, const std::vector<int>& params,
			wchar_t privateMarker, const std::wstring& intermediates);
		void onVtOsc(int command, const std::wstring& data);
		void applySgr(const std::vector<int>& params);
		void resetAttr();
		void eraseDisplay(int mode);
		void eraseLine(int mode);
		void scrollUp(int rows);
		void resizeBuffer(int nwc, int nhc);
		void clearRow(int screenY);
		void clearViewport();
		void setCell(int x, int screenY, wchar_t ch);
		void clearCell(int x, int screenY);
		void appendSystemMessage(const std::wstring& msg);
		bool writeInputBytes(const char* data, DWORD len);
		void sendInputText(const wchar_t* text, int len);
		void sendInputString(const std::wstring& s) { sendInputText(s.c_str(), (int)s.size()); }

		void jumpToBottom();
		void updateScrollbar();
		void scrollViewport(int newScrollBack);
		void requestUiRefresh();

		POINT measureCellPx();
		POINT caretPixelPos();
		POINT cellFromPoint(LPARAM lParam);
		bool cellSelected(int bufX, int bufY) const;
		void wordBoundsAt(int bufX, int bufY, int& x1, int& x2) const;
		void clearSelection();
		void selectAll();
		void copySelection();
		void pasteClipboard();
		void showContextMenu(int sx, int sy);

		COLORREF effFg() const;
		COLORREF effBg() const;
		static COLORREF color256(int index);
		static std::vector<int> parseCsiParams(const std::wstring& s);
		static bool isWideChar(wchar_t ch);

		static constexpr COLORREF Palette16[16] = {
			RGB(0,   0,   0), RGB(128,   0,   0), RGB(0, 128,   0), RGB(128, 128,   0),
			RGB(0,   0, 128), RGB(128,   0, 128), RGB(0, 128, 128), RGB(192, 192, 192),
			RGB(128, 128, 128), RGB(255,   0,   0), RGB(0, 255,   0), RGB(255, 255,   0),
			RGB(0,   0, 255), RGB(255,   0, 255), RGB(0, 255, 255), RGB(255, 255, 255),
		};
		static constexpr COLORREF DefaultForeground = Palette16[7];
		static constexpr COLORREF DefaultBackground = Palette16[0];

		enum { IDM_COPY = 40001, IDM_PASTE, IDM_SELECTALL, IDM_CLOSE };
		enum { WMU_OUTPUT = WM_APP + 1 };

	public:
		bool SpawnApplication(_In_opt_ PCWSTR app, _In_opt_ PCWSTR cmd, _In_opt_ PCWSTR cd);
	};
}