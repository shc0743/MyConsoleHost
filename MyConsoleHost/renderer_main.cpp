#include "renderer_main.hpp"


int RendererMain(RendererMainData data) {
	HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
	if (!k32) __fastfail(FAST_FAIL_STACK_COOKIE_CHECK_FAILURE);
	auto GetProcAddress = reinterpret_cast<decltype(&::GetProcAddress)>(::GetProcAddress(k32, "GetProcAddress"));
	if (!GetProcAddress) __fastfail(FAST_FAIL_STACK_COOKIE_CHECK_FAILURE);
	PROCESS_MITIGATION_CHILD_PROCESS_POLICY f{};
	f.NoChildProcessCreation = 1;
	auto SetProcessMitigationPolicy = reinterpret_cast<BOOL(WINAPI*)(PROCESS_MITIGATION_POLICY, PVOID, SIZE_T)>
		(GetProcAddress(k32, "SetProcessMitigationPolicy"));
	if (SetProcessMitigationPolicy) SetProcessMitigationPolicy(ProcessChildProcessPolicy, &f, sizeof f);
	
	RevertToSelf();

	// stub!
	Sleep(10000);

	return 0;
}
