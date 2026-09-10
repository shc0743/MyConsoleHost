#include "targetver.h"
#include "basedef.hpp"
#include <sddl.h>
#pragma comment(lib, "advapi32.lib")
using namespace std;

int SandboxContainerStartup(bool noError) {
#if 0
	if (!app::hSandboxJob) {
		HANDLE hJob = CreateJobObjectW(NULL, NULL);
		if (!hJob) {
			DWORD e = GetLastError();
			if (!e) e = -1;
			if (!noError) MessageBoxW(NULL, ErrorChecker(e).message().c_str(), L"Console", MB_ICONERROR);
			return e;
		}
		JOBOBJECT_EXTENDED_LIMIT_INFORMATION jeli = {};
		jeli.BasicLimitInformation.LimitFlags =
			JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE |
			JOB_OBJECT_LIMIT_ACTIVE_PROCESS;
		jeli.BasicLimitInformation.ActiveProcessLimit = 1;
		SetInformationJobObject(hJob, JobObjectExtendedLimitInformation, &jeli, sizeof(jeli));
		app::hSandboxJob = hJob;
	}
#endif
	HANDLE hCurrent = OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, TRUE, GetCurrentProcessId());
	if (!hCurrent) return GetLastError();
	wstring cmd = format(L"- --type=sandbox --internal --client-id={}", (ULONGLONG)(PVOID)hCurrent);
	STARTUPINFOW si{}; PROCESS_INFORMATION pi{};
	si.cb = sizeof(si);
	try {
		auto app = make_unique<WCHAR[]>(32768);
		GetModuleFileNameW(NULL, app.get(), 32768);
		HANDLE hToken{}, hTokenStart{};
		{
			w32ProcessHandle hProcess = OpenProcess(
				PROCESS_QUERY_INFORMATION | PROCESS_QUERY_LIMITED_INFORMATION,
				FALSE, GetCurrentProcessId());
			HANDLE hImpToken{};
			OpenProcessToken(hProcess, TOKEN_DUPLICATE | TOKEN_QUERY, &hImpToken);
			if (!hImpToken) throw runtime_error("");
			DuplicateTokenEx(hImpToken, TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenPrimary, &hToken);
			DuplicateTokenEx(hImpToken, TOKEN_ALL_ACCESS, nullptr, SecurityImpersonation, TokenImpersonation, &hTokenStart);
			CloseHandle(hImpToken);
			if (!hTokenStart) {
				if (hToken) CloseHandle(hToken);
				throw runtime_error("");
			}
			if (!hToken) {
				if (hTokenStart) CloseHandle(hTokenStart);
				throw runtime_error("");
			}
			PSID pIntegritySid = nullptr;
			SID_IDENTIFIER_AUTHORITY SID_MANDATORY_AUTHORITY = { 0,0,0,0,0,16 };
			if (!AllocateAndInitializeSid(&SID_MANDATORY_AUTHORITY,
				1, 4096, 0, 0, 0, 0, 0, 0, 0, &pIntegritySid
			)) {
				CloseHandle(hToken);
				CloseHandle(hTokenStart);
				throw runtime_error("");
			}
			TOKEN_MANDATORY_LABEL tml = {};
			tml.Label.Attributes = SE_GROUP_INTEGRITY;
			tml.Label.Sid = pIntegritySid;

			if (!SetTokenInformation(hToken, TokenIntegrityLevel,
				&tml, sizeof(tml) + GetLengthSid(pIntegritySid))) {
				LocalFree(pIntegritySid);
				CloseHandle(hToken);
				CloseHandle(hTokenStart);
				throw runtime_error("");
			}
			LocalFree(pIntegritySid);

			pIntegritySid = nullptr;
			SID_MANDATORY_AUTHORITY = { 0,0,0,0,0,16 };
			if (!AllocateAndInitializeSid(&SID_MANDATORY_AUTHORITY,
				1, 4096, 0, 0, 0, 0, 0, 0, 0, &pIntegritySid
			)) {
				CloseHandle(hToken);
				CloseHandle(hTokenStart);
				throw runtime_error("");
			}
			tml.Label.Attributes = SE_GROUP_INTEGRITY;
			tml.Label.Sid = pIntegritySid;

			if (!SetTokenInformation(hTokenStart, TokenIntegrityLevel,
				&tml, sizeof(tml) + GetLengthSid(pIntegritySid))) {
				LocalFree(pIntegritySid);
				CloseHandle(hToken);
				CloseHandle(hTokenStart);
				throw runtime_error("");
			}
			LocalFree(pIntegritySid);

			// TODO: CreateRestrictedToken
		}
		STARTUPINFOEXW siex{};
		std::unique_ptr<uint8_t[]> attributeList;
		SIZE_T need{};
		bool ok = false;
		HANDLE hList[] = { hCurrent };
		InitializeProcThreadAttributeList(0, 1, 0, &need);
		if (need && need < 32768) {
			attributeList = make_unique<uint8_t[]>(need);
			if (InitializeProcThreadAttributeList((PPROC_THREAD_ATTRIBUTE_LIST)attributeList.get(),
				1, 0, &need)) {
				if (UpdateProcThreadAttribute(
					(PPROC_THREAD_ATTRIBUTE_LIST)attributeList.get(), 0,
					PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
					&hList, // only 1 handle
					sizeof(HANDLE),
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
			CloseHandle(hToken);
			CloseHandle(hTokenStart);
			throw runtime_error("");
		}
		siex.StartupInfo = si;
		siex.StartupInfo.cb = sizeof(siex);
		siex.lpAttributeList = PPROC_THREAD_ATTRIBUTE_LIST(attributeList ? attributeList.get() : nullptr);
#pragma warning(push)
#pragma warning(disable: 6335)
		BOOL r = CreateProcessAsUserW(hToken, app.get(), cmd.data(), NULL, NULL, TRUE,
			CREATE_NEW_PROCESS_GROUP | CREATE_SUSPENDED | CREATE_DEFAULT_ERROR_MODE | EXTENDED_STARTUPINFO_PRESENT,
			NULL, NULL, (LPSTARTUPINFOW)&siex, &pi);
		DWORD e = GetLastError();
		if (attributeList) DeleteProcThreadAttributeList((PPROC_THREAD_ATTRIBUTE_LIST)attributeList.get());
		CloseHandle(hToken);
		//AssignProcessToJobObject(app::hSandboxJob, pi.hProcess);
		HANDLE hThread2 = OpenThread(THREAD_SET_THREAD_TOKEN | THREAD_QUERY_INFORMATION,
			FALSE, pi.dwThreadId);
		if (!hThread2 || !SetThreadToken(&hThread2, hTokenStart)) {
			TerminateProcess(pi.hProcess, 1);
			CloseHandle(pi.hThread);
			CloseHandle(pi.hProcess);
			r = false;
		}
		if (hThread2) CloseHandle(hThread2);
		CloseHandle(hTokenStart);
		CloseHandle(hCurrent);
		hCurrent = NULL;
		SetLastError(e);
		if (!r) {
			throw runtime_error("");
		}
#pragma warning(pop)
	}
	catch (...) {
		if (hCurrent) CloseHandle(hCurrent);
		DWORD e = GetLastError();
		if (!e) e = -1;
		if (!noError) MessageBoxW(NULL, ErrorChecker(e).message().c_str(), L"Console", MB_ICONERROR);
		return e;
	}
	if (hCurrent) CloseHandle(hCurrent);
	ResumeThread(pi.hThread);
	CloseHandle(pi.hThread);
	app::hSandboxProcess = pi.hProcess;
	return 0;
}

int SandboxContainerMain(HANDLE hProcess) {
	if (!hProcess) return ERROR_INVALID_PARAMETER;
	DWORD code{};
	WaitForSingleObject(hProcess, INFINITE);
	GetExitCodeProcess(hProcess, &code);
	CloseHandle(hProcess);
	return 0;
}
