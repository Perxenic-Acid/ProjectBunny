#include "DX12Input.h"

#include <Windows.h>

#include "DX12ShaderDump.h"
#include "DX12State.h"

static bool gF8WasDown = false;

void DX12PollInput()
{
	const bool f8Down = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
	if (f8Down && !gF8WasDown) {
		DX12Log("F8 pressed; shader dump requested\n");
		DX12DumpCachedShaders();
	}
	gF8WasDown = f8Down;
}
