#pragma once

#include <Windows.h>

void DX12HookCommandListCreation(IUnknown *device);
void DX12HookCommandList(IUnknown *commandList);
void DX12HookCommandQueue(IUnknown *commandQueue);
