#include "CosmeticExt.h"
#include "Color/ColorRules.h"

#include <Phobos.h>
#include <Syringe.h>

HANDLE CosmeticExtDLL::hInstance = nullptr;

// No DEFINE_PATCH and no ExeRun seat. Every feature is a hook, except the
// ColorRules vtable wrap, which DllMain writes directly. Status lines are
// logged later, from the first soundmd.ini and rules reads (ExeRun-time
// logging is discarded -- debug.log isn't open yet).

bool __stdcall DllMain(HANDLE hInstance, DWORD dwReason, LPVOID)
{
	if (dwReason == DLL_PROCESS_ATTACH)
	{
		CosmeticExtDLL::hInstance = hInstance;
		Phobos::hInstance = hInstance;
		ColorRules::PatchVtables();
	}
	return true;
}

SYRINGE_HANDSHAKE(pInfo)
{
	pInfo->Message = const_cast<char*>("CosmeticExt");
	return S_OK;
}
