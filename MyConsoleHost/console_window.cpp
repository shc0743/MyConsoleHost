#include "console_window.hpp"
#include "ipc_window.hpp"
#include "basedef.hpp"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#pragma comment(lib, "imm32.lib")
using namespace std;
#pragma optimize("",off)
int app::ui::ConsoleWindow::defaultFontSize = 14;

void app::ui::ConsoleWindow::onCreated() {
	SetLayeredWindowAttributes(hwnd, 0, (BYTE)0xF7, LWA_ALPHA);

	float factor = get_dpi_scale_factor();
	myFont = CreateFontW(
		(int)(-(float(fontSize)) * factor - 0.5f), (int)(-(float(fontSize / 2)) * factor - 0.5f),
		0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
		OUT_CHARACTER_PRECIS, CLIP_CHARACTER_PRECIS, CLEARTYPE_QUALITY, FF_DONTCARE,
		font_name.c_str());

	RECT rc{};
	GetClientRect(hwnd, &rc);
	SIZE cell{};
	HDC hdc = GetDC(hwnd);
	if (hdc) {
		if (myFont) {
			HFONT old = (HFONT)SelectObject(hdc, myFont);
			GetTextExtentPoint32W(hdc, L"W", 1, &cell);
			SelectObject(hdc, old);
		}
		ReleaseDC(hwnd, hdc);
	}
	int cw = (std::max)(1, (int)(cell.cx > 0 ? (rc.right - rc.left) / cell.cx : 1));
	int chh = (std::max)(1, (int)(cell.cy > 0 ? (rc.bottom - rc.top) / cell.cy : 1));
	{
		std::lock_guard<std::mutex> lock(bufMutex);
		resizeBuffer(cw, chh);
	}
	if (hConsole) ResizePseudoConsole(hConsole, COORD{ (SHORT)wc, (SHORT)hc });

	vt::Parser::Callbacks vtcb;
	vtcb.onPrint = [this](wchar_t ch) { onVtPrint(ch); };
	vtcb.onCtrl = [this](wchar_t ch) { onVtCtrl(ch); };
	vtcb.onCsi = [this](wchar_t f, const std::vector<int>& p, wchar_t pm, const std::wstring& im) {
		onVtCsi(f, p, pm, im);
	};
	vtcb.onOsc = [this](int cmd, const std::wstring& d) { onVtOsc(cmd, d); };
	vtParser.setCallbacks(std::move(vtcb));
}


void app::ui::ConsoleWindow::onDestroy() {
	shuttingDown = true;
	inputWriteSide.close();
	if (hConsole) {
		ClosePseudoConsole(hConsole);
		hConsole = nullptr;
	}
	pumpJoinWorker();

	if (myFont) {
		DeleteFont(myFont);
		myFont = nullptr;
	}

	for (auto& w : app::windows) {
		if (w && w.get() != this && w->is_alive()) return;
	}
	DestroyWindow(*ipcWindow);
}


bool app::ui::ConsoleWindow::SpawnApplication(_In_opt_ PCWSTR app, _In_opt_ PCWSTR cmd) {
	if (_hosted) throw runtime_error("This console window already hosted an application.");

	HANDLE inputReadSide{}, outputWriteSide{};
	HANDLE outputReadSide{}, inputWriteSide{};

	if (!CreatePipe(&inputReadSide, &inputWriteSide, NULL, 0)) return false;
	if (!CreatePipe(&outputReadSide, &outputWriteSide, NULL, 0)) {
		CloseHandle(inputReadSide); CloseHandle(inputWriteSide);
		return false;
	}

	if (FAILED(CreatePseudoConsole(COORD{ .X = (SHORT)wc, .Y = (SHORT)hc }, inputReadSide, outputWriteSide, 0, &hConsole))) {
		CloseHandle(inputReadSide); CloseHandle(outputWriteSide);
		CloseHandle(outputReadSide); CloseHandle(inputWriteSide);
		return false;
	}

	STARTUPINFOEXW si{ sizeof(si) };
	PROCESS_INFORMATION pi{};
	std::unique_ptr<uint8_t[]> attributeList;
	DWORD flags = CREATE_SUSPENDED | EXTENDED_STARTUPINFO_PRESENT;
	SIZE_T need{};
	bool ok = false;
	InitializeProcThreadAttributeList(0, 1, 0, &need);
	if (need && need < 32768) {
		attributeList = make_unique<uint8_t[]>(need);
		if (InitializeProcThreadAttributeList((PPROC_THREAD_ATTRIBUTE_LIST)attributeList.get(),
			1, 0, &need)) {
			if (UpdateProcThreadAttribute(
				(PPROC_THREAD_ATTRIBUTE_LIST)attributeList.get(), 0,
				PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
				hConsole,
				sizeof(hConsole),
				NULL, NULL
			)) {
				ok = true;
			}
		}
	}
	if (!ok) {
		CloseHandle(inputReadSide); CloseHandle(outputWriteSide);
		CloseHandle(outputReadSide); CloseHandle(inputWriteSide);
		return false;
	}
	si.StartupInfo.cb = sizeof(STARTUPINFOEXW);
	si.StartupInfo.dwFlags = STARTF_USESHOWWINDOW;
	si.StartupInfo.wShowWindow = SW_SHOWNORMAL;
	si.lpAttributeList = PPROC_THREAD_ATTRIBUTE_LIST(attributeList ? attributeList.get() : nullptr);
	wstring c = cmd ? cmd : L"";
	WCHAR COMSPEC[260]{};
	GetEnvironmentVariableW(L"COMSPEC", COMSPEC, 260);
	if (!CreateProcessW((app || cmd) ? ((app && app[0]) ? app : NULL) : COMSPEC, cmd ? c.data() : COMSPEC,
		NULL, NULL, FALSE, flags, NULL, NULL, (LPSTARTUPINFOW)&si, &pi)) {
		if (attributeList) DeleteProcThreadAttributeList((PPROC_THREAD_ATTRIBUTE_LIST)attributeList.get());
		CloseHandle(inputReadSide); CloseHandle(outputWriteSide);
		CloseHandle(outputReadSide); CloseHandle(inputWriteSide);
		return false;
	}
	if (attributeList) DeleteProcThreadAttributeList((PPROC_THREAD_ATTRIBUTE_LIST)attributeList.get());
	ResumeThread(pi.hThread);

	this->_hosted = true;
	this->hHostedProcess = pi.hProcess;
	CloseHandle(pi.hThread);

	CloseHandle(inputReadSide);
	CloseHandle(outputWriteSide);
	this->outputReadSide = outputReadSide;
	this->inputWriteSide = inputWriteSide;

	workerThread = std::thread(std::bind(&app::ui::ConsoleWindow::worker, this));

	return true;
}


void app::ui::ConsoleWindow::requestUiRefresh() {
	if (!shuttingDown && hwnd) PostMessageW(hwnd, WMU_OUTPUT, 0, 0);
}


void app::ui::ConsoleWindow::worker() {
	char buf[8192];
	HANDLE hOut = outputReadSide.get();
	for (;;) {
		if (hHostedProcess && WaitForSingleObject(hHostedProcess, 0) == WAIT_OBJECT_0) {
			for (;;) {
				DWORD avail = 0;
				if (!PeekNamedPipe(hOut, nullptr, 0, nullptr, &avail, nullptr) || avail == 0) break;
				DWORD toRead = (std::min<DWORD>)(avail, (DWORD)sizeof(buf));
				DWORD read = 0;
				if (!ReadFile(hOut, buf, toRead, &read, nullptr) || read == 0) break;
				processOutput(buf, read);
			}
			break;
		}
		if (hHostedProcess) {
			DWORD w = WaitForSingleObject(hHostedProcess, 10);
			if (w == WAIT_OBJECT_0) continue;
		} else {
			Sleep(10);
		}
		DWORD avail = 0;
		if (!PeekNamedPipe(hOut, nullptr, 0, nullptr, &avail, nullptr)) {
			break;
		}
		if (avail == 0) continue;
		DWORD toRead = (std::min<DWORD>)(avail, (DWORD)sizeof(buf));
		DWORD read = 0;
		if (!ReadFile(hOut, buf, toRead, &read, nullptr) || read == 0) {
			break;
		}
		processOutput(buf, read);
		requestUiRefresh();
	}
	DWORD exitCode = DWORD(-1);
	if (hHostedProcess) GetExitCodeProcess(hHostedProcess, &exitCode);
	//FUCK YOU MOTHERFUCKER DOUBAO WRITE RUBBISH CODES
	//wchar_t line[256]{};
	//swprintf(line, 256, L"\r\n[Process exited with code %lu. Press any key to close this window.]", exitCode);
	wstring line = format(L"\r\n[Process exited with code {} (0x{:08x})]\r\nPress any key to close...", exitCode, exitCode);
	// TODO: allow user to choose whether wait
	{
		std::lock_guard<std::mutex> lock(bufMutex);
		hostedExited = true;
		hostedExitCode = exitCode;
		scrollBack = 0;
		appendSystemMessage(line);
	}
	requestUiRefresh();
	outputReadSide.close();
	post(WM_CLOSE);
}

void app::ui::ConsoleWindow::pumpJoinWorker() {
	if (!workerThread.joinable()) return;
	//fuck you sb doubao write rubbish code
	//HANDLE h = workerThread.native_handle();
	//for (;;) {
	//	DWORD wr = MsgWaitForMultipleObjects(1, &h, FALSE, INFINITE, QS_ALLINPUT);
	//	if (wr == WAIT_OBJECT_0) break;
	//	MSG m;
	//	// FIXME: may dead lock here
	//	while (PeekMessageW(&m, nullptr, 0, 0, PM_REMOVE)) {
	//		if (m.message == WM_QUIT) {
	//			PostThreadMessageW(GetCurrentThreadId(), m.message, m.wParam, m.lParam);
	//			break;
	//		}
	//		TranslateMessage(&m);
	//		DispatchMessageW(&m);
	//	}
	//}
	if (workerThread.joinable()) workerThread.join();
}

void app::ui::ConsoleWindow::onOutputNotify(EventData& ev) {
	ev.preventDefault();
	if (shuttingDown) return;
	std::wstring title;
	{
		std::lock_guard<std::mutex> lock(bufMutex);
		if (!pendingTitle.empty()) { title = pendingTitle; pendingTitle.clear(); }
	}
	if (!title.empty()) SetWindowTextW(hwnd, title.c_str());
	updateScrollbar();
	positionImeWindow();
	InvalidateRect(hwnd, nullptr, FALSE);
}
void app::ui::ConsoleWindow::processOutput(const char* data, DWORD len) {
	std::lock_guard<std::mutex> lock(bufMutex);
	for (DWORD i = 0; i < len; ++i) {
		unsigned char c = (unsigned char)data[i];
		for (;;) {
			if (utf8Remaining > 0) {
				if ((c & 0xC0) == 0x80) {
					utf8CodePoint = (utf8CodePoint << 6) | (c & 0x3F);
					if (--utf8Remaining == 0) {
						processChar(utf8CodePoint <= 0xFFFF ? (wchar_t)utf8CodePoint : L'?');
					}
				} else {
					utf8Remaining = 0;
					utf8CodePoint = 0;
					continue;
				}
				break;
			}
			if (c < 0x80) {
				processChar((wchar_t)c);
			} else if ((c & 0xE0) == 0xC0) {
				utf8CodePoint = c & 0x1F;
				utf8Remaining = 1;
			} else if ((c & 0xF0) == 0xE0) {
				utf8CodePoint = c & 0x0F;
				utf8Remaining = 2;
			} else if ((c & 0xF8) == 0xF0) {
				utf8CodePoint = c & 0x07;
				utf8Remaining = 3;
			}
			break;
		}
	}
}

void app::ui::ConsoleWindow::processChar(wchar_t ch) {
	vtParser.feed(ch);
}


void app::ui::ConsoleWindow::onVtPrint(wchar_t ch) {
	putPrintable(ch);
}

void app::ui::ConsoleWindow::onVtCtrl(wchar_t ch) {
	switch (ch) {
	case L'\r':
		cursorX = 0;
		break;
	case L'\n':
	case 0x0B:
	case 0x0C:
		++cursorY;
		if (cursorY >= hc) {
			scrollUp(cursorY - hc + 1);
			cursorY = hc - 1;
		}
		break;
	case L'\b':
		if (cursorX > 0) {
			if (cursorX >= 2 && cellFlags[(size_t)(HistoryMax + cursorY) * wc + cursorX - 1] == CELL_WIDE_CONT)
				cursorX -= 2;
			else
				--cursorX;
		}
		break;
	case L'\t': {
		int next = ((cursorX / 8) + 1) * 8;
		if (next >= wc) {
			cursorX = 0;
			++cursorY;
			if (cursorY >= hc) { scrollUp(1); cursorY = hc - 1; }
		} else {
			cursorX = next;
		}
		break;
	}
	case L'\a':
		MessageBeep(MB_OK);
		break;
	default:
		break;
	}
}

void app::ui::ConsoleWindow::onVtOsc(int command, const std::wstring& data) {
	if (command == 0 || command == 2) {
		pendingTitle = data;
	}
}

void app::ui::ConsoleWindow::putPrintable(wchar_t ch) {
	bool wide = isWideChar(ch);
	if (cursorX >= wc || (wide && cursorX == wc - 1)) {
		if (cursorX == wc - 1 && wide) setCell(cursorX, cursorY, L' ');
		cursorX = 0;
		++cursorY;
		if (cursorY >= hc) {
			scrollUp(1);
			cursorY = hc - 1;
		}
	}
	setCell(cursorX, cursorY, ch);
	cursorX += wide ? 2 : 1;
	if (cursorX > wc) cursorX = wc;
}

void app::ui::ConsoleWindow::onVtCsi(wchar_t finalByte, const std::vector<int>& params,
									   wchar_t privateMarker, const std::wstring& intermediates) {
	auto p = [&](size_t i, int def) -> int {
		return (i < params.size() && params[i] > 0) ? params[i] : def;
	};
	auto moveX = [&](int x) {
		cursorX = (std::clamp)(x, 0, wc - 1);
	};
	switch (finalByte) {
	case L'A': cursorY = (std::max)(0, cursorY - p(0, 1)); if (cursorX >= wc) cursorX = wc - 1; break;
	case L'B': cursorY = (std::min)(hc - 1, cursorY + p(0, 1)); if (cursorX >= wc) cursorX = wc - 1; break;
	case L'C': moveX(cursorX + p(0, 1)); break;
	case L'D': moveX(cursorX - p(0, 1)); break;
	case L'E': cursorY = (std::min)(hc - 1, cursorY + p(0, 1)); cursorX = 0; break;
	case L'F': cursorY = (std::max)(0, cursorY - p(0, 1)); cursorX = 0; break;
	case L'G': moveX(p(0, 1) - 1); break;
	case L'H':
	case L'f':
		cursorY = (std::clamp)(p(0, 1) - 1, 0, hc - 1);
		cursorX = (std::clamp)(p(1, 1) - 1, 0, wc - 1);
		break;
	case L'J': eraseDisplay(p(0, 0)); break;
	case L'K': eraseLine(p(0, 0)); break;
	case L'X': // ECH：从光标处擦除 N 个字符（写空格，不移动光标）
		for (int i = 0; i < p(0, 1) && cursorX + i < wc; ++i) clearCell(cursorX + i, cursorY);
		break;
	case L'P': // DCH：从光标处删除 N 个字符（后续字符左移，尾部填空格）
		{
			int dn = p(0, 1);
			for (int x = cursorX; x < wc; ++x) {
				int src = x + dn;
				if (src < wc) {
					auto& dbuf = inAltBuffer ? altBuffer : buffer;
					auto& dflags = inAltBuffer ? altCellFlags : cellFlags;
					auto& dfg = inAltBuffer ? altFgColors : fgColors;
					auto& dbg2 = inAltBuffer ? altBgColors : bgColors;
					size_t di = inAltBuffer ? (size_t)cursorY * wc + x : (size_t)(HistoryMax + cursorY) * wc + x;
					size_t si = inAltBuffer ? (size_t)cursorY * wc + src : (size_t)(HistoryMax + cursorY) * wc + src;
					dbuf[di] = dbuf[si]; dflags[di] = dflags[si]; dfg[di] = dfg[si]; dbg2[di] = dbg2[si];
				} else {
					clearCell(x, cursorY);
				}
			}
		}
		break;
	case L'm': applySgr(params); break;
	case L'n':
		if (p(0, 0) == 6) {
			wchar_t reply[32]{};
			swprintf(reply, 32, L"\x1b[%d;%dR", cursorY + 1, cursorX + 1);
			sendInputText(reply, (int)wcslen(reply));
		}
		break;
	case L'h':
		if (privateMarker == L'?') {
			for (int v : params) {
				if (v == 25) cursorVisible = true;
				else if (v == 1047) enterAltBuffer(false);
				else if (v == 1048) { savedCursorX = cursorX; savedCursorY = cursorY; }
				else if (v == 1049) enterAltBuffer(true);
			}
		}
		break;
	case L'l':
		if (privateMarker == L'?') {
			for (int v : params) {
				if (v == 25) cursorVisible = false;
				else if (v == 1047) exitAltBuffer(false);
				else if (v == 1048) { cursorX = savedCursorX; cursorY = savedCursorY; }
				else if (v == 1049) exitAltBuffer(true);
			}
		}
		break;
	case L's': savedCursorX = cursorX; savedCursorY = cursorY; break;
	case L'u':
		cursorX = (std::clamp)(savedCursorX, 0, wc - 1);
		cursorY = (std::clamp)(savedCursorY, 0, hc - 1);
		break;
	default:
		break;
	}
}

void app::ui::ConsoleWindow::applySgr(const std::vector<int>& params) {
	if (params.empty()) {
		resetAttr();
		return;
	}
	size_t i = 0;
	while (i < params.size()) {
		int p = params[i];
		switch (p) {
		case 0: resetAttr(); break;
		case 1: bold = true; break;
		case 22: bold = false; break;
		case 4: underline = true; break;
		case 24: underline = false; break;
		case 7: reverse = true; break;
		case 27: reverse = false; break;
		case 39: fgIndex = 7; fgIsRgb = false; break;
		case 49: bgIndex = 0; bgIsRgb = false; break;
		case 30: case 31: case 32: case 33: case 34: case 35: case 36: case 37:
			fgIndex = p - 30; fgIsRgb = false; break;
		case 40: case 41: case 42: case 43: case 44: case 45: case 46: case 47:
			bgIndex = p - 40; bgIsRgb = false; break;
		case 90: case 91: case 92: case 93: case 94: case 95: case 96: case 97:
			fgIndex = p - 90 + 8; fgIsRgb = false; break;
		case 100: case 101: case 102: case 103: case 104: case 105: case 106: case 107:
			bgIndex = p - 100 + 8; bgIsRgb = false; break;
		case 38:
		case 48: {
			if (i + 2 < params.size() && params[i + 1] == 5) {
				if (p == 38) { fgRgb = color256(params[i + 2]); fgIsRgb = true; }
				else { bgRgb = color256(params[i + 2]); bgIsRgb = true; }
				i += 2;
			} else if (i + 4 < params.size() && params[i + 1] == 2) {
				int r = (std::clamp)(params[i + 2], 0, 255);
				int g = (std::clamp)(params[i + 3], 0, 255);
				int b = (std::clamp)(params[i + 4], 0, 255);
				if (p == 38) { fgRgb = RGB(r, g, b); fgIsRgb = true; }
				else { bgRgb = RGB(r, g, b); bgIsRgb = true; }
				i += 4;
			}
			break;
		}
		default:
			break;
		}
		++i;
	}
}

void app::ui::ConsoleWindow::resetAttr() {
	fgIndex = 7;
	bgIndex = 0;
	fgIsRgb = bgIsRgb = false;
	bold = underline = reverse = false;
}

void app::ui::ConsoleWindow::eraseDisplay(int mode) {
	switch (mode) {
	case 0: // 从光标处擦除到视口末尾
		if (cursorX >= wc) { clearRow(cursorY); }
		else { for (int x = cursorX; x < wc; ++x) clearCell(x, cursorY); }
		for (int y = cursorY + 1; y < hc; ++y) clearRow(y);
		break;
	case 1: // 从视口开头擦除到光标处
		for (int y = 0; y < cursorY; ++y) clearRow(y);
		for (int x = 0; x <= cursorX; ++x) clearCell(x, cursorY);
		break;
	case 2: // 清空整个视口（不影响历史）
		clearViewport();
		break;
	default: // 3：清空视口与全部历史
		clearViewport();
		std::fill(buffer.begin(), buffer.end(), L' ');
		std::fill(cellFlags.begin(), cellFlags.end(), CELL_NORMAL);
		std::fill(fgColors.begin(), fgColors.end(), DefaultForeground);
		std::fill(bgColors.begin(), bgColors.end(), DefaultBackground);
		historyUsed = 0;
		scrollBack = 0;
		break;
	}
}

void app::ui::ConsoleWindow::eraseLine(int mode) {
	switch (mode) {
	case 0:
		if (cursorX >= wc) { clearRow(cursorY); }
		else { for (int x = cursorX; x < wc; ++x) clearCell(x, cursorY); }
		break;
	case 1:
		for (int x = 0; x <= cursorX && x < wc; ++x) clearCell(x, cursorY);
		break;
	default:
		clearRow(cursorY);
		break;
	}
}

void app::ui::ConsoleWindow::scrollUp(int rows) {
	if (rows <= 0) return;
	size_t rowCells = (size_t)wc;
	if (inAltBuffer) {
		int n = (std::min)(rows, hc);
		size_t keep = (size_t)(hc - n);
		std::memmove(altBuffer.data(), altBuffer.data() + rowCells * n, rowCells * keep * sizeof(wchar_t));
		std::memmove(altCellFlags.data(), altCellFlags.data() + rowCells * n, rowCells * keep * sizeof(uint8_t));
		std::memmove(altFgColors.data(), altFgColors.data() + rowCells * n, rowCells * keep * sizeof(COLORREF));
		std::memmove(altBgColors.data(), altBgColors.data() + rowCells * n, rowCells * keep * sizeof(COLORREF));
		for (int y = hc - n; y < hc; ++y)
			for (int x = 0; x < wc; ++x) {
				size_t idx = (size_t)y * wc + x;
				altBuffer[idx] = L' '; altCellFlags[idx] = CELL_NORMAL;
				altFgColors[idx] = DefaultForeground; altBgColors[idx] = DefaultBackground;
			}
		return;
	}
	if (rows >= totalRows) {
		std::fill(buffer.begin(), buffer.end(), L' ');
		std::fill(cellFlags.begin(), cellFlags.end(), CELL_NORMAL);
		std::fill(fgColors.begin(), fgColors.end(), DefaultForeground);
		std::fill(bgColors.begin(), bgColors.end(), DefaultBackground);
	} else {
		size_t keepRows = (size_t)(totalRows - rows);
		std::memmove(buffer.data(), buffer.data() + rowCells * rows, rowCells * keepRows * sizeof(wchar_t));
		std::memmove(cellFlags.data(), cellFlags.data() + rowCells * rows, rowCells * keepRows * sizeof(uint8_t));
		std::memmove(fgColors.data(), fgColors.data() + rowCells * rows, rowCells * keepRows * sizeof(COLORREF));
		std::memmove(bgColors.data(), bgColors.data() + rowCells * rows, rowCells * keepRows * sizeof(COLORREF));
		for (int y = totalRows - rows; y < totalRows; ++y) {
			for (int x = 0; x < wc; ++x) {
				size_t idx = (size_t)y * wc + x;
				buffer[idx] = L' '; cellFlags[idx] = CELL_NORMAL;
				fgColors[idx] = DefaultForeground; bgColors[idx] = DefaultBackground;
			}
		}
	}
	historyUsed = (std::min)(HistoryMax, historyUsed + rows);
	if (scrollBack > 0) scrollBack = (std::min)(historyUsed, scrollBack + rows);
}

void app::ui::ConsoleWindow::enterAltBuffer(bool saveCursor) {
	if (inAltBuffer) return;
	size_t cells = (size_t)wc * hc;
	savedViewport.assign(cells, L' ');
	savedViewportFlags.assign(cells, CELL_NORMAL);
	savedViewportFg.assign(cells, DefaultForeground);
	savedViewportBg.assign(cells, DefaultBackground);
	for (int y = 0; y < hc; ++y) {
		size_t src = (size_t)(HistoryMax + y) * wc;
		size_t dst = (size_t)y * wc;
		std::memcpy(savedViewport.data() + dst, buffer.data() + src, wc * sizeof(wchar_t));
		std::memcpy(savedViewportFlags.data() + dst, cellFlags.data() + src, wc * sizeof(uint8_t));
		std::memcpy(savedViewportFg.data() + dst, fgColors.data() + src, wc * sizeof(COLORREF));
		std::memcpy(savedViewportBg.data() + dst, bgColors.data() + src, wc * sizeof(COLORREF));
	}
	if (saveCursor) { savedViewportCursorX = cursorX; savedViewportCursorY = cursorY; }
	altBuffer.assign(cells, L' ');
	altCellFlags.assign(cells, CELL_NORMAL);
	altFgColors.assign(cells, DefaultForeground);
	altBgColors.assign(cells, DefaultBackground);
	inAltBuffer = true;
	cursorX = 0; cursorY = 0;
	scrollBack = 0;
}

void app::ui::ConsoleWindow::exitAltBuffer(bool restoreCursor) {
	if (!inAltBuffer) return;
	inAltBuffer = false;
	for (int y = 0; y < hc; ++y) {
		size_t src = (size_t)y * wc;
		size_t dst = (size_t)(HistoryMax + y) * wc;
		std::memcpy(buffer.data() + dst, savedViewport.data() + src, wc * sizeof(wchar_t));
		std::memcpy(cellFlags.data() + dst, savedViewportFlags.data() + src, wc * sizeof(uint8_t));
		std::memcpy(fgColors.data() + dst, savedViewportFg.data() + src, wc * sizeof(COLORREF));
		std::memcpy(bgColors.data() + dst, savedViewportBg.data() + src, wc * sizeof(COLORREF));
	}
	if (restoreCursor) { cursorX = savedViewportCursorX; cursorY = savedViewportCursorY; }
	scrollBack = 0;
}

void app::ui::ConsoleWindow::resizeBuffer(int nwc, int nhc) {
	if (nwc < 1) nwc = 1;
	if (nhc < 1) nhc = 1;
	int nTotal = HistoryMax + nhc;
	if (nwc == wc && nhc == hc && (int)buffer.size() == wc * totalRows) return;

	std::vector<wchar_t> nb((size_t)nwc * nTotal, L' ');
	std::vector<uint8_t> nf0((size_t)nwc * nTotal, CELL_NORMAL);
	std::vector<COLORREF> nf((size_t)nwc * nTotal, DefaultForeground);
	std::vector<COLORREF> nbk((size_t)nwc * nTotal, DefaultBackground);

	if ((int)buffer.size() == wc * totalRows) {
		int copyRows = (std::min)(totalRows, nTotal);
		int copyCols = (std::min)(wc, nwc);
		int srcStart = totalRows - copyRows;
		int dstStart = nTotal - copyRows;
		for (int y = 0; y < copyRows; ++y) {
			for (int x = 0; x < copyCols; ++x) {
				size_t src = (size_t)(srcStart + y) * wc + x;
				size_t dst = (size_t)(dstStart + y) * nwc + x;
				nb[dst] = buffer[src];
				nf0[dst] = cellFlags[src];
				nf[dst] = fgColors[src];
				nbk[dst] = bgColors[src];
			}
		}
	}
	buffer.swap(nb);
	cellFlags.swap(nf0);
	fgColors.swap(nf);
	bgColors.swap(nbk);
	wc = nwc;
	hc = nhc;
	totalRows = nTotal;
	cursorX = (std::clamp)(cursorX, 0, wc - 1);
	cursorY = (std::clamp)(cursorY, 0, hc - 1);
	scrollBack = (std::clamp)(scrollBack, 0, (std::max)(0, historyUsed));
}

void app::ui::ConsoleWindow::clearRow(int screenY) {
	for (int x = 0; x < wc; ++x) clearCell(x, screenY);
}

void app::ui::ConsoleWindow::clearViewport() {
	for (int y = 0; y < hc; ++y) clearRow(y);
}

void app::ui::ConsoleWindow::setCell(int x, int screenY, wchar_t ch) {
	if (x < 0 || x >= wc || screenY < 0 || screenY >= hc) return;
	auto& buf = inAltBuffer ? altBuffer : buffer;
	auto& flags = inAltBuffer ? altCellFlags : cellFlags;
	auto& fg = inAltBuffer ? altFgColors : fgColors;
	auto& bg = inAltBuffer ? altBgColors : bgColors;
	size_t idx = inAltBuffer ? (size_t)screenY * wc + x : (size_t)(HistoryMax + screenY) * wc + x;
	if (x > 0 && flags[idx - 1] == CELL_WIDE_LEAD) { buf[idx - 1] = L' '; flags[idx - 1] = CELL_NORMAL; }
	bool wide = isWideChar(ch);
	buf[idx] = ch; flags[idx] = wide ? CELL_WIDE_LEAD : CELL_NORMAL;
	fg[idx] = effFg(); bg[idx] = effBg();
	if (!wide && x + 1 < wc && flags[idx + 1] == CELL_WIDE_CONT) {
		buf[idx + 1] = L' '; flags[idx + 1] = CELL_NORMAL;
		fg[idx + 1] = effFg(); bg[idx + 1] = effBg();
	}
	if (wide && x + 1 < wc) {
		size_t j = idx + 1;
		if (j + 1 < buf.size() && flags[j + 1] == CELL_WIDE_CONT) { buf[j + 1] = L' '; flags[j + 1] = CELL_NORMAL; }
		buf[j] = L' '; flags[j] = CELL_WIDE_CONT; fg[j] = effFg(); bg[j] = effBg();
	}
}

void app::ui::ConsoleWindow::clearCell(int x, int screenY) {
	if (x < 0 || x >= wc || screenY < 0 || screenY >= hc) return;
	auto& buf = inAltBuffer ? altBuffer : buffer;
	auto& flags = inAltBuffer ? altCellFlags : cellFlags;
	auto& fg = inAltBuffer ? altFgColors : fgColors;
	auto& bg = inAltBuffer ? altBgColors : bgColors;
	size_t idx = inAltBuffer ? (size_t)screenY * wc + x : (size_t)(HistoryMax + screenY) * wc + x;
	if (flags[idx] == CELL_WIDE_LEAD && x + 1 < wc) { buf[idx + 1] = L' '; flags[idx + 1] = CELL_NORMAL; fg[idx + 1] = effFg(); bg[idx + 1] = effBg(); }
	if (flags[idx] == CELL_WIDE_CONT && x > 0) { buf[idx - 1] = L' '; flags[idx - 1] = CELL_NORMAL; fg[idx - 1] = effFg(); bg[idx - 1] = effBg(); }
	buf[idx] = L' '; flags[idx] = CELL_NORMAL; fg[idx] = effFg(); bg[idx] = effBg();
}

void app::ui::ConsoleWindow::appendSystemMessage(const std::wstring& msg) {
	resetAttr();
	for (wchar_t ch : msg) {
		if (ch == L'\r') { cursorX = 0; continue; }
		if (ch == L'\n') {
			++cursorY;
			if (cursorY >= hc) { scrollUp(1); cursorY = hc - 1; }
			continue;
		}
		if (ch < 0x20) continue;
		putPrintable(ch);
	}
}

bool app::ui::ConsoleWindow::writeInputBytes(const char* data, DWORD len) {
	if (hostedExited || shuttingDown) return false;
	if (!inputWriteSide.is_valid()) return false;
	DWORD written = 0;
	return WriteFile((HANDLE)inputWriteSide, data, len, &written, nullptr) == TRUE;
}

void app::ui::ConsoleWindow::sendInputText(const wchar_t* text, int len) {
	if (len <= 0) return;
	char buf[2048];
	int n = WideCharToMultiByte(CP_UTF8, 0, text, len, buf, (int)sizeof(buf), nullptr, nullptr);
	if (n <= 0) return;
	DWORD off = 0;
	while (off < (DWORD)n) {
		DWORD piece = (std::min<DWORD>)((DWORD)n - off, 1024);
		if (!writeInputBytes(buf + off, piece)) break;
		off += piece;
	}
}

void app::ui::ConsoleWindow::onKeyDown(EventData& ev) {
	ev.preventDefault();
	if (hostedExited) {
		DestroyWindow(hwnd);
		return;
	}

	UINT vk = (UINT)ev.wParam;
	bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
	bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;
	bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;

	if (vk == VK_ESCAPE && !ctrl && !alt && hasSel) {
		clearSelection();
		return;
	}

	// TODO: allow user choose Ctrl or Ctrl+Shift
	if (ctrl && shift && !alt) {
		if (vk == 'C') { copySelection(); return; }
		if (vk == 'V') { pasteClipboard(); return; }
		if (vk == 'A') { selectAll(); return; }
	}
	if (ctrl && !shift && !alt) {
		if (vk == 'V') { pasteClipboard(); return; }
	}

	if (is_compositioning()) return;

	wchar_t seq[16]{};
	int len = 0;
	switch (vk) {
	case VK_RETURN: if (!alt) seq[len++] = L'\r'; break;
	case VK_BACK:
		if (!alt) seq[len++] = ctrl ? L'\x08' : L'\x7f';
		break;
	case VK_TAB: if (!alt) seq[len++] = L'\t'; break;
	case VK_ESCAPE: seq[len++] = 0x1B; break;
	case VK_UP: wcscpy_s(seq, 16, L"\x1b[A"); len = 3; break;
	case VK_DOWN: wcscpy_s(seq, 16, L"\x1b[B"); len = 3; break;
	case VK_RIGHT: wcscpy_s(seq, 16, L"\x1b[C"); len = 3; break;
	case VK_LEFT: wcscpy_s(seq, 16, L"\x1b[D"); len = 3; break;
	case VK_HOME: wcscpy_s(seq, 16, shift ? L"\x1b[1;2H" : L"\x1b[H"); len = shift ? 6 : 3; break;
	case VK_END: wcscpy_s(seq, 16, shift ? L"\x1b[1;2F" : L"\x1b[F"); len = shift ? 6 : 3; break;
	case VK_PRIOR: wcscpy_s(seq, 16, L"\x1b[5~"); len = 4; break;
	case VK_NEXT: wcscpy_s(seq, 16, L"\x1b[6~"); len = 4; break;
	case VK_INSERT:
		if (shift && !ctrl && !alt) { pasteClipboard(); return; }
		wcscpy_s(seq, 16, L"\x1b[2~"); len = 4; break;
	case VK_DELETE: wcscpy_s(seq, 16, L"\x1b[3~"); len = 4; break;
	case VK_F1: wcscpy_s(seq, 16, L"\x1bOP"); len = 3; break;
	case VK_F2: wcscpy_s(seq, 16, L"\x1bOQ"); len = 3; break;
	case VK_F3: wcscpy_s(seq, 16, L"\x1bOR"); len = 3; break;
	case VK_F4: wcscpy_s(seq, 16, L"\x1bOS"); len = 3; break;
	case VK_F5: wcscpy_s(seq, 16, L"\x1b[15~"); len = 5; break;
	case VK_F6: wcscpy_s(seq, 16, L"\x1b[17~"); len = 5; break;
	case VK_F7: wcscpy_s(seq, 16, L"\x1b[18~"); len = 5; break;
	case VK_F8: wcscpy_s(seq, 16, L"\x1b[19~"); len = 5; break;
	case VK_F9: wcscpy_s(seq, 16, L"\x1b[20~"); len = 5; break;
	case VK_F10: wcscpy_s(seq, 16, L"\x1b[21~"); len = 5; break;
	case VK_F11: wcscpy_s(seq, 16, L"\x1b[23~"); len = 5; break;
	case VK_F12: wcscpy_s(seq, 16, L"\x1b[24~"); len = 5; break;
	default: break;
	}
	if (len > 0) {
		clearSelection();
		sendInputText(seq, len);
		jumpToBottom();
		return;
	}

	if (ctrl && !alt) {
		wchar_t cc = 0;
		if (vk >= 'A' && vk <= 'Z') cc = (wchar_t)(vk - 'A' + 1);
		else if (vk == '[' || vk == '{') cc = 0x1B;
		else if (vk == '\\' || vk == '|') cc = 0x1C;
		else if (vk == ']' || vk == '}') cc = 0x1D;
		if (cc) {
			clearSelection();
			sendInputText(&cc, 1);
			jumpToBottom();
			return;
		}
	}

}

void app::ui::ConsoleWindow::onChar(EventData& ev) {
	ev.preventDefault();
	if (hostedExited) {
		DestroyWindow(hwnd);
		return;
	}
	wchar_t ch = (wchar_t)ev.wParam;
	if (ch < 0x20 || ch == 0x7F) return;
	clearSelection();
	sendInputText(&ch, 1);
	jumpToBottom();
}

void app::ui::ConsoleWindow::onImeSetContext(EventData& ev) {
	WPARAM masked = ev.wParam & ~ISC_SHOWUICOMPOSITIONWINDOW;
	ev.returnValue(DefWindowProcW(hwnd, WM_IME_SETCONTEXT, masked, ev.lParam));
}

void app::ui::ConsoleWindow::onImeStartComposition(EventData& ev) {
	imeComposing = true;
	imeComp.clear();
	positionImeWindow();
	InvalidateRect(hwnd, nullptr, FALSE);
}

void app::ui::ConsoleWindow::onImeComposition(EventData& ev) {
	HIMC hIMC = (HIMC)ev.wParam;
	LPARAM flags = ev.lParam;
	if (hIMC && (flags & GCS_COMPSTR)) {
		LONG bytes = ImmGetCompositionStringW(hIMC, GCS_COMPSTR, nullptr, 0);
		if (bytes > 0) {
			imeComp.resize(bytes / sizeof(wchar_t));
			ImmGetCompositionStringW(hIMC, GCS_COMPSTR, &imeComp[0], bytes);
		} else {
			imeComp.clear();
		}
		positionImeWindow();
		InvalidateRect(hwnd, nullptr, FALSE);
	}
}

void app::ui::ConsoleWindow::onImeEndComposition(EventData& ev) {
	imeComposing = false;
	imeComp.clear();
	InvalidateRect(hwnd, nullptr, FALSE);
}

POINT app::ui::ConsoleWindow::caretPixelPos() {
	POINT cell = measureCellPx();
	return POINT{ cursorX * cell.x, cursorY * cell.y };
}

void app::ui::ConsoleWindow::positionImeWindow() {
	if (shuttingDown) return;
	HIMC hIMC = ImmGetContext(hwnd);
	if (!hIMC) return;
	POINT caret = caretPixelPos();
	POINT cell = measureCellPx();

	COMPOSITIONFORM cf{};
	cf.dwStyle = CFS_POINT;
	cf.ptCurrentPos = caret;
	ImmSetCompositionWindow(hIMC, &cf);

	CANDIDATEFORM cand{};
	cand.dwIndex = 0;
	cand.dwStyle = CFS_CANDIDATEPOS;
	cand.ptCurrentPos = POINT{ caret.x, caret.y + cell.y };
	ImmSetCandidateWindow(hIMC, &cand);

	ImmReleaseContext(hwnd, hIMC);
}

void app::ui::ConsoleWindow::drawImeComposition(HDC hdc, int cellW, int cellH) {
	if (!imeComposing || imeComp.empty() || scrollBack != 0) return;
	int px = cursorX * cellW;
	int py = cursorY * cellH;
	SIZE sz{};
	GetTextExtentPoint32W(hdc, imeComp.c_str(), (int)imeComp.size(), &sz);
	RECT bg{ px, py, px + (std::max)((int)sz.cx, cellW), py + cellH };
	HBRUSH br = CreateSolidBrush(DefaultBackground);
	FillRect(hdc, &bg, br);
	DeleteObject(br);
	int oldBk = SetBkMode(hdc, TRANSPARENT);
	COLORREF oldFg = SetTextColor(hdc, RGB(255, 255, 255));
	ExtTextOutW(hdc, px, py, 0, nullptr, imeComp.c_str(), (int)imeComp.size(), nullptr);
	HPEN pen = CreatePen(PS_SOLID, 1, RGB(255, 255, 255));
	HGDIOBJ oldPen = SelectObject(hdc, pen);
	MoveToEx(hdc, px, py + cellH - 2, nullptr);
	LineTo(hdc, px + (std::max)((int)sz.cx, cellW), py + cellH - 2);
	SelectObject(hdc, oldPen);
	DeleteObject(pen);
	SetTextColor(hdc, oldFg);
	SetBkMode(hdc, oldBk);
}


POINT app::ui::ConsoleWindow::measureCellPx() {
	POINT result{ 8, 16 };
	HDC hdc = GetDC(hwnd);
	if (hdc) {
		if (myFont) {
			HFONT old = (HFONT)SelectObject(hdc, myFont);
			SIZE cell{};
			GetTextExtentPoint32W(hdc, L"W", 1, &cell);
			SelectObject(hdc, old);
			if (cell.cx > 0 && cell.cy > 0) result = POINT{ cell.cx, cell.cy };
		}
		ReleaseDC(hwnd, hdc);
	}
	return result;
}

void app::ui::ConsoleWindow::onSize(EventData& ev) {
	ev.preventDefault();
	int cw = LOWORD(ev.lParam);
	int chh = HIWORD(ev.lParam);
	if (cw <= 0 || chh <= 0) return;

	POINT cell = measureCellPx();
	int nwc = (std::max)(1, cw / (int)cell.x);
	int nhc = (std::max)(1, chh / (int)cell.y);

	bool sizeChanged = false;
	{
		std::lock_guard<std::mutex> lock(bufMutex);
		sizeChanged = (nwc != wc || nhc != hc);
		if (sizeChanged) resizeBuffer(nwc, nhc);
	}
	if (sizeChanged && hConsole) ResizePseudoConsole(hConsole, COORD{ (SHORT)nwc, (SHORT)nhc });
	updateScrollbar();
	InvalidateRect(hwnd, nullptr, FALSE);
}

void app::ui::ConsoleWindow::onEraseBkgnd(EventData& ev) {
	ev.preventDefault();
	ev.returnValue(1);
}

void app::ui::ConsoleWindow::doPaint(EventData& ev) {
	PAINTSTRUCT ps;
	HDC hdc = BeginPaint(hwnd, &ps);
	if (!hdc) return;
	if (!myFont) {
		EndPaint(hwnd, &ps);
		return;
	}

	RECT clientRc{};
	GetClientRect(hwnd, &clientRc);
	int cw = clientRc.right - clientRc.left;
	int chh = clientRc.bottom - clientRc.top;
	if (cw <= 0 || chh <= 0) { EndPaint(hwnd, &ps); return; }

	HDC memDC = CreateCompatibleDC(hdc);
	HBITMAP memBmp = CreateCompatibleBitmap(hdc, cw, chh);
	HGDIOBJ oldBmp = SelectObject(memDC, memBmp);

	HBRUSH bgBrush = CreateSolidBrush(DefaultBackground);
	RECT fullRc{ 0, 0, cw, chh };
	FillRect(memDC, &fullRc, bgBrush);
	DeleteObject(bgBrush);

	HFONT oldFont = (HFONT)SelectObject(memDC, myFont);
	SetBkMode(memDC, OPAQUE);

	SIZE cell{};
	GetTextExtentPoint32W(memDC, L"W", 1, &cell);
	int cellW = (std::max)(1, (int)cell.cx);
	int cellH = (std::max)(1, (int)cell.cy);

	std::lock_guard<std::mutex> lock(bufMutex);

	if (!buffer.empty() && wc > 0 && hc > 0) {
		int viewTop = HistoryMax - scrollBack;
		int firstRow = (std::max)(0, (int)(ps.rcPaint.top / cellH));
		int lastRow = (std::min)(hc - 1, (int)(ps.rcPaint.bottom / cellH));

		for (int y = firstRow; y <= lastRow; ++y) {
			int absY = viewTop + y;
			int py = y * cellH;
			int firstCol = (std::max)(0, (int)(ps.rcPaint.left / cellW));
			for (int x = firstCol; x < wc; ++x) {
				int px = x * cellW;
				if (px > ps.rcPaint.right) break;
				size_t idx = inAltBuffer ? (size_t)y * wc + x : (size_t)absY * wc + x;
				auto& pb = inAltBuffer ? altBuffer : buffer;
				auto& pf = inAltBuffer ? altCellFlags : cellFlags;
				auto& pfg = inAltBuffer ? altFgColors : fgColors;
				auto& pbg = inAltBuffer ? altBgColors : bgColors;
				uint8_t flag = pf[idx];
				if (flag == CELL_WIDE_CONT) continue;

				bool wide = (flag == CELL_WIDE_LEAD);
				int span = wide ? 2 : 1;
				RECT rc{ px, py, px + cellW * span, py + cellH };

				wchar_t glyph = pb[idx] ? pb[idx] : L' ';
				COLORREF fg = pfg[idx];
				COLORREF bg = pbg[idx];
				if (cellSelected(x, absY) || (wide && x + 1 < wc && cellSelected(x + 1, absY))) {
					std::swap(fg, bg);
				}
				if (scrollBack == 0 && cursorVisible && !imeComposing && x == cursorX && y == cursorY) {
					std::swap(fg, bg);
				}
				SetTextColor(memDC, fg);
				SetBkColor(memDC, bg);
				ExtTextOutW(memDC, px, py, ETO_OPAQUE | ETO_CLIPPED, &rc, &glyph, 1, nullptr);
			}
		}

		drawImeComposition(memDC, cellW, cellH);
	}

	BitBlt(hdc, 0, 0, cw, chh, memDC, 0, 0, SRCCOPY);

	SelectObject(memDC, oldFont);
	SelectObject(memDC, oldBmp);
	DeleteObject(memBmp);
	DeleteDC(memDC);
	EndPaint(hwnd, &ps);
}


void app::ui::ConsoleWindow::updateScrollbar() {
	if (!hwnd) return;
	int snapHc, snapUsed, snapBack;
	{
		std::lock_guard<std::mutex> lock(bufMutex);
		snapHc = hc; snapUsed = historyUsed; snapBack = scrollBack;
	}
	SCROLLINFO si{ sizeof(si) };
	si.fMask = SIF_RANGE | SIF_PAGE | SIF_POS;
	si.nMin = 0;
	si.nMax = (std::max)(snapHc - 1, snapUsed + snapHc - 1);
	si.nPage = snapHc;
	si.nPos = snapUsed - snapBack;
	SetScrollInfo(hwnd, SB_VERT, &si, TRUE);
}

void app::ui::ConsoleWindow::scrollViewport(int newScrollBack) {
	{
		std::lock_guard<std::mutex> lock(bufMutex);
		scrollBack = (std::clamp)(newScrollBack, 0, (std::max)(0, historyUsed));
	}
	updateScrollbar();
	InvalidateRect(hwnd, nullptr, FALSE);
}

void app::ui::ConsoleWindow::jumpToBottom() {
	bool changed;
	{
		std::lock_guard<std::mutex> lock(bufMutex);
		changed = (scrollBack != 0);
		if (changed) scrollBack = 0;
	}
	if (changed) {
		updateScrollbar();
		InvalidateRect(hwnd, nullptr, FALSE);
	}
}

void app::ui::ConsoleWindow::onVScroll(EventData& ev) {
	ev.preventDefault();
	int action = LOWORD((DWORD)ev.wParam);
	SCROLLINFO si{ sizeof(si) };
	si.fMask = SIF_ALL;
	GetScrollInfo(hwnd, SB_VERT, &si);
	int pos = si.nPos;
	switch (action) {
	case SB_LINEUP: --pos; break;
	case SB_LINEDOWN: ++pos; break;
	case SB_PAGEUP: pos -= hc; break;
	case SB_PAGEDOWN: pos += hc; break;
	case SB_THUMBTRACK:
	case SB_THUMBPOSITION: pos = si.nTrackPos; break;
	case SB_TOP: pos = 0; break;
	case SB_BOTTOM: pos = historyUsed; break;
	default: return;
	}
	scrollViewport(historyUsed - pos);
}

void app::ui::ConsoleWindow::onMouseWheel(EventData& ev) {
	ev.preventDefault();
	short delta = GET_WHEEL_DELTA_WPARAM((WPARAM)ev.wParam);
	UINT linesPerNotch = 3;
	SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &linesPerNotch, 0);
	if (linesPerNotch == 0) return;
	int step;
	if (linesPerNotch == WHEEL_PAGESCROLL) {
		step = (delta > 0 ? -hc : hc);
		wheelRemainder = 0;
	} else {
		wheelRemainder += delta;
		int actual = (std::max)(1, WHEEL_DELTA / (int)linesPerNotch);
		step = wheelRemainder / actual;
		wheelRemainder %= actual;
	}
	if (step != 0) scrollViewport(scrollBack + step);
}


void app::ui::ConsoleWindow::onGetMinMaxInfo(EventData& ev) {
	ev.preventDefault();
	MINMAXINFO* mmi = (MINMAXINFO*)ev.lParam;
	if (!mmi) return;
	POINT cell = measureCellPx();
	int minClientW = cell.x * 2 + GetSystemMetrics(SM_CXVSCROLL);
	int minClientH = cell.y * 2;
	RECT rc{ 0, 0, minClientW, minClientH };
	DWORD style = (DWORD)GetWindowLongPtrW(hwnd, GWL_STYLE);
	DWORD exStyle = (DWORD)GetWindowLongPtrW(hwnd, GWL_EXSTYLE);
	AdjustWindowRectEx(&rc, style, FALSE, exStyle);
	mmi->ptMinTrackSize.x = rc.right - rc.left;
	mmi->ptMinTrackSize.y = rc.bottom - rc.top;
	ev.returnValue(0);
}

POINT app::ui::ConsoleWindow::cellFromPoint(LPARAM lp) {
	int mx = (short)LOWORD((DWORD_PTR)lp);
	int my = (short)HIWORD((DWORD_PTR)lp);
	POINT cell = measureCellPx();
	int col = (std::clamp)(mx / (cell.x > 0 ? (int)cell.x : 1), 0, (int)(wc - 1));
	int screenRow = (std::clamp)(my / (cell.y > 0 ? (int)cell.y : 1), 0, (int)(hc - 1));
	int absY = HistoryMax - scrollBack + screenRow;
	return POINT{ col, absY };
}

bool app::ui::ConsoleWindow::cellSelected(int bufX, int bufY) const {
	if (!hasSel) return false;
	int r1 = (std::min)(selAY, selBY), r2 = (std::max)(selAY, selBY);
	if (bufY < r1 || bufY > r2) return false;
	if (blockSelection) {
		int c1 = (std::min)(selAX, selBX), c2 = (std::max)(selAX, selBX);
		return bufX >= c1 && bufX <= c2;
	}
	long long lo = (long long)r1 * wc + (r1 == r2 ? (std::min)(selAX, selBX) : (selAY <= selBY ? selAX : selBX));
	long long hi = (long long)r2 * wc + (r1 == r2 ? (std::max)(selAX, selBX) : (selAY <= selBY ? selBX : selAX));
	long long cur = (long long)bufY * wc + bufX;
	return cur >= lo && cur <= hi;
}

static bool isWordSeparator(wchar_t ch) {
	if (iswspace(ch)) return true;
	static const wchar_t* punct = L" \t,.;:!?\"'()[]{}<>/\\|`~!@#$%^&*-=+";
	for (const wchar_t* p = punct; *p; ++p) if (*p == ch) return true;
	return false;
}

void app::ui::ConsoleWindow::wordBoundsAt(int bufX, int bufY, int& x1, int& x2) const {
	x1 = (std::clamp)(bufX, 0, wc - 1);
	x2 = x1;
	size_t base = (size_t)bufY * wc;
	if (bufX >= 0 && bufX < wc && isWordSeparator(buffer[base + bufX])) return;
	while (x1 > 0 && !isWordSeparator(buffer[base + x1 - 1])) --x1;
	while (x2 + 1 < wc && !isWordSeparator(buffer[base + x2 + 1])) ++x2;
}

void app::ui::ConsoleWindow::clearSelection() {
	bool changed = hasSel || selecting;
	hasSel = false;
	selecting = false;
	wordSelecting = false;
	if (changed) InvalidateRect(hwnd, nullptr, FALSE);
}

void app::ui::ConsoleWindow::selectAll() {
	std::lock_guard<std::mutex> lock(bufMutex);
	blockSelection = false;
	selAX = 0; selAY = 0;
	selBX = wc - 1; selBY = totalRows - 1;
	hasSel = true;
	InvalidateRect(hwnd, nullptr, FALSE);
}

void app::ui::ConsoleWindow::copySelection() {
	std::wstring text;
	{
		std::lock_guard<std::mutex> lock(bufMutex);
		if (!hasSel) return;
		int r1 = (std::min)(selAY, selBY), r2 = (std::max)(selAY, selBY);
		int c1 = (std::min)(selAX, selBX), c2 = (std::max)(selAX, selBX);
		for (int y = r1; y <= r2; ++y) {
			int from, to;
			if (blockSelection) {
				from = c1; to = c2;
			} else {
				from = (y == r1) ? c1 : 0;
				to = (y == r2) ? c2 : wc - 1;
			}
			std::wstring line;
			for (int x = from; x <= to; ++x) {
				size_t idx = (size_t)y * wc + x;
				if (cellFlags[idx] == CELL_WIDE_CONT) {
					if (!(x > 0 && x - 1 >= from && cellFlags[idx - 1] == CELL_WIDE_LEAD)) line += L' ';
					continue;
				}
				line += buffer[idx] ? buffer[idx] : L' ';
			}
			while (!line.empty() && line.back() == L' ') line.pop_back();
			if (y != r1) text += L"\r\n";
			text += line;
		}
	}
	if (text.empty()) return;
	if (!OpenClipboard(hwnd)) return;
	EmptyClipboard();
	size_t bytes = (text.size() + 1) * sizeof(wchar_t);
	HGLOBAL hMem = GlobalAlloc(GMEM_MOVEABLE, bytes);
	if (hMem) {
		void* p = GlobalLock(hMem);
		if (p) {
			memcpy(p, text.c_str(), bytes);
			GlobalUnlock(hMem);
			SetClipboardData(CF_UNICODETEXT, hMem);
		}
	}
	CloseClipboard();
}

void app::ui::ConsoleWindow::pasteClipboard() {
	if (hostedExited) return;
	if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return;
	if (!OpenClipboard(hwnd)) return;
	HANDLE hData = GetClipboardData(CF_UNICODETEXT);
	std::wstring text;
	if (hData) {
		const wchar_t* p = (const wchar_t*)GlobalLock(hData);
		if (p) { text = p; GlobalUnlock(hData); }
	}
	CloseClipboard();
	if (text.empty()) return;
	std::wstring normalized;
	normalized.reserve(text.size());
	for (size_t i = 0; i < text.size(); ++i) {
		if (text[i] == L'\r') {
			normalized += L'\r';
			if (i + 1 < text.size() && text[i + 1] == L'\n') ++i;
		} else if (text[i] == L'\n') {
			normalized += L'\r';
		} else if (text[i] == L'\t' || text[i] >= L' ') {
			normalized += text[i];
		}
	}
	clearSelection();
	jumpToBottom();
	sendInputString(normalized);
}

void app::ui::ConsoleWindow::onLButtonDown(EventData& ev) {
	ev.preventDefault();
	if (hostedExited) return;
	SetFocus(hwnd);
	std::lock_guard<std::mutex> lock(bufMutex);
	POINT c = cellFromPoint(ev.lParam);
	bool alt = (GetKeyState(VK_MENU) & 0x8000) != 0;

	ULONGLONG now = GetTickCount64();
	bool isDouble = (now - lastClickTick) <= (ULONGLONG)GetDoubleClickTime()
		&& lastClickCol == c.x && lastClickRow == c.y;
	lastClickTick = now;
	lastClickCol = c.x; lastClickRow = c.y;

	blockSelection = alt;
	wordSelecting = isDouble;
	if (isDouble) {
		int x1, x2;
		wordBoundsAt(c.x, c.y, x1, x2);
		selAX = x1; selAY = c.y;
		selBX = x2; selBY = c.y;
	} else {
		selAX = selBX = c.x;
		selAY = selBY = c.y;
	}
	hasSel = true;
	selecting = true;
	SetCapture(hwnd);
	InvalidateRect(hwnd, nullptr, FALSE);
}

void app::ui::ConsoleWindow::onMouseMove(EventData& ev) {
	if (!selecting) return;
	ev.preventDefault();
	std::lock_guard<std::mutex> lock(bufMutex);
	POINT c = cellFromPoint(ev.lParam);
	int nx = c.x, ny = c.y;
	if (wordSelecting) {
		int x1, x2;
		wordBoundsAt(c.x, c.y, x1, x2);
		long long anchor = (long long)selAY * wc + selAX;
		long long cur = (long long)ny * wc + nx;
		if (cur >= anchor) { nx = x2; } else { nx = x1; }
	}
	if (nx != selBX || ny != selBY) {
		selBX = nx;
		selBY = ny;
		InvalidateRect(hwnd, nullptr, FALSE);
	}
}

void app::ui::ConsoleWindow::onLButtonUp(EventData& ev) {
	if (!selecting) return;
	ev.preventDefault();
	selecting = false;
	ReleaseCapture();
}

void app::ui::ConsoleWindow::onRButtonUp(EventData& ev) {
	ev.preventDefault();
	bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
	if (shift) return;
	if (hasSel) {
		copySelection();
		clearSelection();
	} else {
		pasteClipboard();
	}
}

void app::ui::ConsoleWindow::showContextMenu(int sx, int sy) {
	// FIXME: remove the fucking code created by doubao
	HMENU hMenu = CreatePopupMenu();
	if (!hMenu) return;
	AppendMenuW(hMenu, hasSel ? MF_STRING : MF_STRING | MF_GRAYED, IDM_COPY, L"复制\tCtrl+Shift+C");
	AppendMenuW(hMenu, MF_STRING, IDM_PASTE, L"粘贴\tCtrl+Shift+V");
	AppendMenuW(hMenu, MF_STRING, IDM_SELECTALL, L"全选\tCtrl+Shift+A");
	AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
	AppendMenuW(hMenu, MF_STRING, IDM_CLOSE, L"关闭");
	TPMPARAMS tpm{ sizeof(tpm) };
	RECT rc{};
	GetWindowRect(hwnd, &rc);
	tpm.rcExclude = rc;
	TrackPopupMenuEx(hMenu, TPM_RIGHTBUTTON | TPM_LEFTALIGN | TPM_TOPALIGN,
		sx, sy, hwnd, &tpm);
	DestroyMenu(hMenu);
}

void app::ui::ConsoleWindow::onContextMenu(EventData& ev) {
	// hittest first
	// WM_CONTEXTMENU lParam is same as WM_NCHITTEST
	if (send(WM_NCHITTEST, 0, ev.lParam) != HTCLIENT) return;

	ev.preventDefault();
	int sx = (short)LOWORD((DWORD_PTR)ev.lParam);
	int sy = (short)HIWORD((DWORD_PTR)ev.lParam);
	bool fromKeyboard = (sx == -1 && sy == -1);
	bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
	if (!fromKeyboard && !shift) return;
	if (fromKeyboard) {
		POINT pt{};
		GetCursorPos(&pt);
		sx = pt.x; sy = pt.y;
	}
	showContextMenu(sx, sy);
}

void app::ui::ConsoleWindow::onMenuCommand(EventData& ev) {
	ev.preventDefault();
	switch ((int)ev.wParam) {
	case IDM_COPY: copySelection(); break;
	case IDM_PASTE: pasteClipboard(); break;
	case IDM_SELECTALL: selectAll(); break;
	case IDM_CLOSE: PostMessageW(hwnd, WM_CLOSE, 0, 0); break;
	default: break;
	}
}

COLORREF app::ui::ConsoleWindow::effFg() const {
	if (fgIsRgb) return reverse ? (bgIsRgb ? bgRgb : Palette16[bgIndex]) : fgRgb;
	COLORREF f = Palette16[bold && fgIndex < 8 ? fgIndex + 8 : fgIndex];
	return reverse ? (bgIsRgb ? bgRgb : Palette16[bgIndex]) : f;
}

COLORREF app::ui::ConsoleWindow::effBg() const {
	if (bgIsRgb) return reverse ? (fgIsRgb ? fgRgb : Palette16[bold && fgIndex < 8 ? fgIndex + 8 : fgIndex]) : bgRgb;
	COLORREF b = Palette16[bgIndex];
	return reverse ? (fgIsRgb ? fgRgb : Palette16[bold && fgIndex < 8 ? fgIndex + 8 : fgIndex]) : b;
}

COLORREF app::ui::ConsoleWindow::color256(int index) {
	index &= 0xFF;
	if (index < 16) return Palette16[index];
	if (index < 232) {
		int v = index - 16;
		int r = v / 36, g = (v / 6) % 6, b = v % 6;
		static const int levels[6] = { 0, 95, 135, 175, 215, 255 };
		return RGB(levels[r], levels[g], levels[b]);
	}
	int v = 8 + (index - 232) * 10;
	return RGB(v, v, v);
}

std::vector<int> app::ui::ConsoleWindow::parseCsiParams(const std::wstring& s) {
	std::vector<int> result;
	size_t start = 0;
	while (start <= s.size()) {
		size_t end = s.find(L';', start);
		std::wstring tok = s.substr(start, (end == std::wstring::npos) ? std::wstring::npos : (end - start));
		result.push_back(tok.empty() ? 0 : _wtoi(tok.c_str()));
		if (end == std::wstring::npos) break;
		start = end + 1;
	}
	return result;
}

bool app::ui::ConsoleWindow::isWideChar(wchar_t ch) {
	return (ch >= 0x1100 && ch <= 0x115F) || // 谚文 Jamo
		(ch >= 0x2329 && ch <= 0x232A) ||
		(ch >= 0x2E80 && ch <= 0x303F) ||    // CJK 部首/康熙/符号
		(ch >= 0x3041 && ch <= 0x33FF) ||    // 平假名、片假名、CJK 符号
		(ch >= 0x3400 && ch <= 0x4DBF) ||    // CJK 扩展 A
		(ch >= 0x4E00 && ch <= 0x9FFF) ||    // CJK 统一表意文字
		(ch >= 0xA000 && ch <= 0xA4CF) ||    // 彝文
		(ch >= 0xA960 && ch <= 0xA97F) ||
		(ch >= 0xAC00 && ch <= 0xD7A3) ||    // 谚文音节
		(ch >= 0xD800 && ch <= 0xDBFF) ||    // 高代理项（emoji 等）
		(ch >= 0xF900 && ch <= 0xFAFF) ||    // CJK 兼容表意文字
		(ch >= 0xFE10 && ch <= 0xFE19) ||    // 竖排符号
		(ch >= 0xFE30 && ch <= 0xFE6F) ||    // CJK 兼容形式/小字号全角
		(ch >= 0xFF00 && ch <= 0xFF60) ||    // 全角 ASCII
		(ch >= 0xFFE0 && ch <= 0xFFE6);
}
