#include "console_window.hpp"
#include "ipc_window.hpp"
#include "WindowAlphaEditor.h"
#include "basedef.hpp"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#pragma comment(lib, "imm32.lib")
using namespace std;

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

	HMENU sys = sysmenu();
	AppendMenuW(sys, MF_SEPARATOR, 0, NULL);
	AppendMenuW(sys, MF_STRING, 1001, L"Set Opacity...");
}


void app::ui::ConsoleWindow::onDestroy() {
	inputWriteSide.close();
	if (hConsole) {
		ClosePseudoConsole(hConsole);
		hConsole = nullptr;
	}
	if (workerThread.joinable()) workerThread.join();

	if (myFont) {
		DeleteFont(myFont);
		myFont = nullptr;
	}

	for (auto& w : app::windows) {
		if (w && w.get() != this && IsWindow(*w)) return;
	}
	DestroyWindow(*ipcWindow);
}


void app::ui::ConsoleWindow::onNcDestroy(EventData&) {
	invokeLater([](Window* w, EventData&) {
		app::windows.erase(remove_if(app::windows.begin(), app::windows.end(), [w](decltype(app::windows)::value_type v) {
			return v.get() == w;
		}), app::windows.end());
	});
}


bool app::ui::ConsoleWindow::SpawnApplication(_In_opt_ PCWSTR app, _In_opt_ PCWSTR cmd, _In_opt_ PCWSTR cd) {
	if (_hosted) throw runtime_error("This console window already hosted an application.");
	if (!CreateRenderer()) return false;

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
			else {
				DeleteProcThreadAttributeList((PPROC_THREAD_ATTRIBUTE_LIST)attributeList.get());
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
		NULL, NULL, FALSE, flags, NULL, cd, (LPSTARTUPINFOW)&si, &pi)) {
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

bool app::ui::ConsoleWindow::CreateRenderer() {
	wstring newCmd = format(L"- --type=renderer --internal");
	STARTUPINFOW si{}; PROCESS_INFORMATION pi{};
	si.cb = sizeof(si);
	try {
		auto app = make_unique<WCHAR[]>(32768);
		GetModuleFileNameW(NULL, app.get(), 32768);
#pragma warning(push)
#pragma warning(disable: 6335)
		STARTUPINFOEXW siex{};
		DWORD64 mitigationPolicy = PROCESS_CREATION_MITIGATION_POLICY_PROHIBIT_DYNAMIC_CODE_ALWAYS_ON;
		DWORD cpp = PROCESS_CREATION_CHILD_PROCESS_RESTRICTED;
		std::unique_ptr<uint8_t[]> attributeList;
		SIZE_T need{};
		bool ok = false;
		InitializeProcThreadAttributeList(0, 1, 0, &need);
		if (need && need < 32768) {
			attributeList = make_unique<uint8_t[]>(need);
			if (InitializeProcThreadAttributeList((PPROC_THREAD_ATTRIBUTE_LIST)attributeList.get(),
				1, 0, &need)) {
				if (UpdateProcThreadAttribute(
					(PPROC_THREAD_ATTRIBUTE_LIST)attributeList.get(), 0,
					PROC_THREAD_ATTRIBUTE_PARENT_PROCESS,
					&app::hSandboxProcess,
					sizeof(HANDLE),
					NULL, NULL
				)/* && UpdateProcThreadAttribute(
					(PPROC_THREAD_ATTRIBUTE_LIST)attributeList.get(), 0,
					PROC_THREAD_ATTRIBUTE_MITIGATION_POLICY,
					&mitigationPolicy,
					sizeof(DWORD64),
					NULL, NULL
				) && UpdateProcThreadAttribute(
					(PPROC_THREAD_ATTRIBUTE_LIST)attributeList.get(), 0,
					PROC_THREAD_ATTRIBUTE_CHILD_PROCESS_POLICY,
					&cpp,
					sizeof(DWORD),
					NULL, NULL
				)*/) {
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
		BOOL r = CreateProcessW(app.get(), newCmd.data(), NULL, NULL, FALSE,
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
		return false;
	}
	ResumeThread(pi.hThread);
	CloseHandle(pi.hThread);
	hRendererProcess = pi.hProcess;
	return true;
}


void app::ui::ConsoleWindow::worker() {

}

void app::ui::ConsoleWindow::onImeSetContext(EventData& ev) {
	WPARAM masked = ev.wParam & ~ISC_SHOWUICOMPOSITIONWINDOW;
	ev.returnValue(DefWindowProcW(hwnd, WM_IME_SETCONTEXT, masked, ev.lParam));
}

void app::ui::ConsoleWindow::onImeStartComposition(EventData& ev) {
	//imeComposing = true;
	//imeComp.clear();
	positionImeWindow();
	InvalidateRect(hwnd, nullptr, FALSE);
}

void app::ui::ConsoleWindow::onImeComposition(EventData& ev) {
	HIMC hIMC = (HIMC)ev.wParam;
	LPARAM flags = ev.lParam;
	if (hIMC && (flags & GCS_COMPSTR)) {
		LONG bytes = ImmGetCompositionStringW(hIMC, GCS_COMPSTR, nullptr, 0);
		if (bytes > 0) {
			//imeComp.resize(bytes / sizeof(wchar_t));
			//ImmGetCompositionStringW(hIMC, GCS_COMPSTR, &imeComp[0], bytes);
		}
		else {
			//imeComp.clear();
		}
		positionImeWindow();
		InvalidateRect(hwnd, nullptr, FALSE);
	}
}

void app::ui::ConsoleWindow::onImeEndComposition(EventData& ev) {
	//imeComposing = false;
	//imeComp.clear();
	InvalidateRect(hwnd, nullptr, FALSE);
}

void app::ui::ConsoleWindow::positionImeWindow() {
	/*if (shuttingDown) return;
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

	ImmReleaseContext(hwnd, hIMC);*/
}

void app::ui::ConsoleWindow::drawImeComposition(HDC hdc, int cellW, int cellH) {
	/*if (!imeComposing || imeComp.empty() || scrollBack != 0) return;
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
	SetBkMode(hdc, oldBk);*/
}


void app::ui::ConsoleWindow::onSize(EventData& ev) {
	ev.preventDefault();

}

void app::ui::ConsoleWindow::onClose(EventData& ev) {
	remove_style_ex(WS_EX_LAYERED);
}

void app::ui::ConsoleWindow::onMenuCommand(EventData& ev) {
	ev.preventDefault();

}

void app::ui::ConsoleWindow::onSysMenu(EventData& ev) {
	switch (ev.wParam) {
	case 1001:
		AllowSetForegroundWindow(ASFW_ANY);
		std::thread([](HWND hwnd) {
			ui::WindowAlphaEditor w;
			w.create();
			w.setTarget(hwnd);
			w.center(hwnd);
			w.show();
			w.focus();
			return w.run();
		}, hwnd).detach();
		break;
	default:
		return;
	}
	ev.preventDefault();
}


