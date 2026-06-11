#pragma once

#include <Windows.h>
#include <d3d12.h>

void DX12SetModule(HINSTANCE module);
HINSTANCE DX12GetModule();

bool DX12OpenLogFile();
void DX12CloseLogFile();
void DX12Log(const char *fmt, ...);

DWORD DX12HookFunction(void **original, void *target, void *hook, const char *name);
void *DX12GetOriginalFunction(void *target);
void DX12UnhookAll();

LONG DX12IncrementPresentCount();
LONG DX12GetPresentCount();

void DX12SetOverlayWindow(HWND hwnd);
HWND DX12GetOverlayWindow();

void DX12SetOverlayStatus(const wchar_t *status);
void DX12GetOverlayStatus(wchar_t *status, size_t statusCount);

void DX12SetCommandQueue(ID3D12CommandQueue *queue);
ID3D12CommandQueue *DX12AcquireCommandQueue();
