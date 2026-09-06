#pragma once

#include <functional>
#include <string>
#include <vector>
#include <cstdint>

namespace app {
namespace vt {

class Parser {
public:
	struct Callbacks {
		std::function<void(wchar_t)> onPrint;
		std::function<void(wchar_t)> onCtrl;
		std::function<void(wchar_t finalByte,
		                   const std::vector<int>& params,
		                   wchar_t privateMarker,
		                   const std::wstring& intermediates)> onCsi;
		std::function<void(int command, const std::wstring& data)> onOsc;
		std::function<void(wchar_t finalByte)> onEsc;
	};

	void setCallbacks(Callbacks cb) { cb_ = std::move(cb); }

	void reset() {
		state_ = State::Ground;
		oscString_.clear();
		csiParams_.clear();
		intermediates_.clear();
		privateMarker_ = 0;
	}

	void feed(wchar_t ch) {
		switch (state_) {
		case State::Ground:
			if (ch == 0x1B) { state_ = State::Escape; }
			else if (ch < 0x20) { if (cb_.onCtrl) cb_.onCtrl(ch); }
			else if (ch == 0x7F) { /* DEL 忽略 */ }
			else { if (cb_.onPrint) cb_.onPrint(ch); }
			break;

		case State::Escape:
			if (ch == L'[') {
				state_ = State::CsiEntry;
				csiParams_.clear(); intermediates_.clear(); privateMarker_ = 0;
			} else if (ch == L']') {
				state_ = State::OscString;
				oscString_.clear();
			} else if (ch == L'P' || ch == L'_' || ch == L'^' || ch == L'k' || ch == L'X') {
				state_ = State::IgnoreUntilSt;
			} else if (ch >= 0x20 && ch <= 0x2F) {
				intermediates_.clear();
				intermediates_ += ch;
				state_ = State::EscIntermediate;
			} else {
				if (cb_.onEsc) cb_.onEsc(ch);
				state_ = State::Ground;
			}
			break;

		case State::EscIntermediate:
			if (ch >= 0x20 && ch <= 0x2F) {
				intermediates_ += ch;
			} else {
				if (cb_.onEsc) cb_.onEsc(ch);
				state_ = State::Ground;
			}
			break;

		case State::CsiEntry:
			if (ch == L'?' || ch == L'>' || ch == L'=' || ch == L'<') {
				privateMarker_ = ch;
				state_ = State::CsiParam;
			} else if ((ch >= L'0' && ch <= L'9') || ch == L';') {
				csiParams_ += ch;
				state_ = State::CsiParam;
			} else if (ch >= 0x20 && ch <= 0x2F) {
				intermediates_ += ch;
				state_ = State::CsiIntermediate;
			} else if (ch >= L'@' && ch <= L'~') {
				dispatchCsi(ch);
				state_ = State::Ground;
			} else if (ch == 0x1B) {
				state_ = State::Escape;
			} else {
				state_ = State::Ground;
			}
			break;

		case State::CsiParam:
			if ((ch >= L'0' && ch <= L'9') || ch == L';') {
				csiParams_ += ch;
			} else if (ch >= 0x20 && ch <= 0x2F) {
				intermediates_ += ch;
				state_ = State::CsiIntermediate;
			} else if (ch >= L'@' && ch <= L'~') {
				dispatchCsi(ch);
				state_ = State::Ground;
			} else if (ch == 0x1B) {
				state_ = State::Escape;
			} else {
				state_ = State::Ground;
			}
			break;

		case State::CsiIntermediate:
			if (ch >= 0x20 && ch <= 0x2F) {
				intermediates_ += ch;
			} else if (ch >= L'@' && ch <= L'~') {
				dispatchCsi(ch);
				state_ = State::Ground;
			} else if (ch == 0x1B) {
				state_ = State::Escape;
			} else {
				state_ = State::Ground;
			}
			break;

		case State::OscString:
			if (ch == 0x07) {
				dispatchOsc();
				state_ = State::Ground;
			} else if (ch == 0x1B) {
				state_ = State::OscStWait;
			} else {
				oscString_ += ch;
			}
			break;

		case State::OscStWait:
			if (ch == L'\\') {
				dispatchOsc();
				state_ = State::Ground;
			} else if (ch == 0x1B) {
			} else {
				dispatchOsc();
				state_ = State::Ground;
				if (ch < 0x20) { if (cb_.onCtrl) cb_.onCtrl(ch); }
				else if (ch != 0x7F) { if (cb_.onPrint) cb_.onPrint(ch); }
			}
			break;

		case State::IgnoreUntilSt:
			if (ch == 0x1B) state_ = State::IgnoreStWait;
			break;

		case State::IgnoreStWait:
			state_ = (ch == L'\\') ? State::Ground : State::IgnoreUntilSt;
			break;
		}
	}

private:
	enum class State {
		Ground, Escape, EscIntermediate,
		CsiEntry, CsiParam, CsiIntermediate,
		OscString, OscStWait,
		IgnoreUntilSt, IgnoreStWait,
	};

	State state_ = State::Ground;
	std::wstring oscString_;
	std::wstring csiParams_;
	std::wstring intermediates_;
	wchar_t privateMarker_ = 0;
	Callbacks cb_;

	static std::vector<int> parseParams(const std::wstring& s) {
		std::vector<int> result;
		size_t start = 0;
		while (start <= s.size()) {
			size_t end = s.find(L';', start);
			std::wstring tok = (end == std::wstring::npos)
				? s.substr(start)
				: s.substr(start, end - start);
			result.push_back(tok.empty() ? 0 : _wtoi(tok.c_str()));
			if (end == std::wstring::npos) break;
			start = end + 1;
		}
		return result;
	}

	void dispatchCsi(wchar_t finalByte) {
		if (!cb_.onCsi) return;
		cb_.onCsi(finalByte, parseParams(csiParams_), privateMarker_, intermediates_);
	}

	void dispatchOsc() {
		if (!cb_.onOsc) return;
		int command = 0;
		std::wstring data = oscString_;
		size_t semi = oscString_.find(L';');
		if (semi != std::wstring::npos) {
			std::wstring cmdStr = oscString_.substr(0, semi);
			command = cmdStr.empty() ? 0 : _wtoi(cmdStr.c_str());
			data = oscString_.substr(semi + 1);
		}
		cb_.onOsc(command, data);
	}
};

} // namespace vt
} // namespace app
