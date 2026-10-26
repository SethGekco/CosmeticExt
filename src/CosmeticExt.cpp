#include "CosmeticExt.h"

#include <Phobos.h>
#include <Syringe.h>

HANDLE CosmeticExtDLL::hInstance = nullptr;

// No DEFINE_PATCH and no ExeRun seat: every feature is a hook, so nothing has
// to run before WinMain. The load banner is logged from the first soundmd.ini
// read instead (ExeRun-time logging is discarded -- debug.log isn't open yet).

bool __stdcall DllMain(HANDLE hInstance, DWORD dwReason, LPVOID)
{
	if (dwReason == DLL_PROCESS_ATTACH)
	{
		CosmeticExtDLL::hInstance = hInstance;
		Phobos::hInstance = hInstance;
	}
	return true;
}

SYRINGE_HANDSHAKE(pInfo)
{
	pInfo->Message = const_cast<char*>("CosmeticExt");
	return S_OK;
}
