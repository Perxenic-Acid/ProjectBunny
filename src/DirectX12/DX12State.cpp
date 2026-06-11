#include "DX12State.h"

#include <Shlwapi.h>
#include <stdio.h>
#include <unordered_map>
#include <unordered_set>

#include "Nektra/NktHookLib.h"

static HINSTANCE gModule = nullptr;
static FILE *gLog = nullptr;
static volatile LONG gPresentCount = 0;
static HWND gOverlayWindow = nullptr;
static wchar_t gOverlayStatus[512] = L"3DMigoto DX12 hook alive";
static ID3D12CommandQueue *gCommandQueue = nullptr;
static SRWLOCK gStateLock = SRWLOCK_INIT;
static CNktHookLib gHookMgr;
static std::unordered_set<void*> gHookedFunctions;
static std::unordered_map<void*, void*> gOriginalFunctions;

void DX12SetModule(HINSTANCE module)
{
	gModule = module;
}

HINSTANCE DX12GetModule()
{
	return gModule;
}

bool DX12OpenLogFile()
{
	wchar_t path[MAX_PATH];
	if (!GetModuleFileNameW(gModule, path, MAX_PATH))
		return false;

	PathRemoveFileSpecW(path);
	PathAppendW(path, L"d3d12_log.txt");
	gLog = _wfsopen(path, L"w", _SH_DENYNO);
	return gLog != nullptr;
}

void DX12CloseLogFile()
{
	if (gLog) {
		fclose(gLog);
		gLog = nullptr;
	}
}

void DX12Log(const char *fmt, ...)
{
	if (!gLog)
		return;

	va_list args;
	va_start(args, fmt);
	vfprintf(gLog, fmt, args);
	va_end(args);
	fflush(gLog);
}

static bool RememberHook(void *target)
{
	AcquireSRWLockExclusive(&gStateLock);
	bool inserted = gHookedFunctions.insert(target).second;
	ReleaseSRWLockExclusive(&gStateLock);
	return inserted;
}

static void RememberOriginal(void *target, void *original)
{
	if (!target || !original)
		return;

	AcquireSRWLockExclusive(&gStateLock);
	gOriginalFunctions[target] = original;
	ReleaseSRWLockExclusive(&gStateLock);
}

DWORD DX12HookFunction(void **original, void *target, void *hook, const char *name)
{
	if (!target || !hook)
		return ERROR_PROC_NOT_FOUND;
	if (!RememberHook(target))
		return ERROR_SUCCESS;

	SIZE_T hookId = 0;
	DWORD result = gHookMgr.Hook(&hookId, original, target, hook);
	if (result == ERROR_SUCCESS && original)
		RememberOriginal(target, *original);
	DX12Log("%s hook %s target=%p original=%p result=0x%lx\n",
		result == ERROR_SUCCESS ? "Installed" : "Failed",
		name, target, original ? *original : nullptr, result);
	return result;
}

void *DX12GetOriginalFunction(void *target)
{
	if (!target)
		return nullptr;

	AcquireSRWLockShared(&gStateLock);
	auto it = gOriginalFunctions.find(target);
	void *original = it != gOriginalFunctions.end() ? it->second : nullptr;
	ReleaseSRWLockShared(&gStateLock);
	return original;
}

void DX12UnhookAll()
{
	gHookMgr.UnhookAll();
}

LONG DX12IncrementPresentCount()
{
	return InterlockedIncrement(&gPresentCount);
}

LONG DX12GetPresentCount()
{
	return gPresentCount;
}

void DX12SetOverlayWindow(HWND hwnd)
{
	gOverlayWindow = hwnd;
}

HWND DX12GetOverlayWindow()
{
	return gOverlayWindow;
}

void DX12SetOverlayStatus(const wchar_t *status)
{
	AcquireSRWLockExclusive(&gStateLock);
	if (status && status[0])
		wcsncpy_s(gOverlayStatus, status, _TRUNCATE);
	else
		wcsncpy_s(gOverlayStatus, L"3DMigoto DX12 hook alive", _TRUNCATE);
	ReleaseSRWLockExclusive(&gStateLock);

	HWND hwnd = DX12GetOverlayWindow();
	if (hwnd)
		InvalidateRect(hwnd, nullptr, FALSE);
}

void DX12GetOverlayStatus(wchar_t *status, size_t statusCount)
{
	if (!status || statusCount == 0)
		return;

	AcquireSRWLockShared(&gStateLock);
	wcsncpy_s(status, statusCount, gOverlayStatus, _TRUNCATE);
	ReleaseSRWLockShared(&gStateLock);
}

void DX12SetCommandQueue(ID3D12CommandQueue *queue)
{
	if (queue)
		queue->AddRef();

	AcquireSRWLockExclusive(&gStateLock);
	ID3D12CommandQueue *oldQueue = gCommandQueue;
	gCommandQueue = queue;
	ReleaseSRWLockExclusive(&gStateLock);

	if (oldQueue)
		oldQueue->Release();
}

ID3D12CommandQueue *DX12AcquireCommandQueue()
{
	AcquireSRWLockShared(&gStateLock);
	ID3D12CommandQueue *queue = gCommandQueue;
	if (queue)
		queue->AddRef();
	ReleaseSRWLockShared(&gStateLock);
	return queue;
}
