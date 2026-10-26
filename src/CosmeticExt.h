#pragma once

#include <Windows.h>

// CosmeticExt: presentation-only engine extensions (sound, later visuals).
// Nothing in this DLL writes simulation state, so it is multiplayer-neutral
// by construction; keep it that way.
class CosmeticExtDLL
{
public:
	static HANDLE hInstance;
};
