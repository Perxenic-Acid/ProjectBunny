#include "D3D12Wrapper.h"

#include <Shlwapi.h>

#include "DX12DeviceHooks.h"
#include "DX12Overlay.h"
#include "DX12State.h"
#include "DXGIHooks.h"

static HMODULE gRealD3D12 = nullptr;
static bool gInitialized = false;

static HMODULE LoadRealD3D12();

typedef HRESULT(WINAPI *PFN_D3D12_CREATE_DEVICE_LOCAL)(
	IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);
typedef HRESULT(WINAPI *PFN_D3D12_GET_DEBUG_INTERFACE_LOCAL)(REFIID, void**);
typedef HRESULT(WINAPI *PFN_D3D12_SERIALIZE_ROOT_SIGNATURE_LOCAL)(
	const D3D12_ROOT_SIGNATURE_DESC*, D3D_ROOT_SIGNATURE_VERSION, ID3DBlob**, ID3DBlob**);
typedef HRESULT(WINAPI *PFN_D3D12_CREATE_ROOT_SIGNATURE_DESERIALIZER_LOCAL)(
	LPCVOID, SIZE_T, REFIID, void**);
typedef HRESULT(WINAPI *PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE_LOCAL)(
	const D3D12_VERSIONED_ROOT_SIGNATURE_DESC*, ID3DBlob**, ID3DBlob**);
typedef HRESULT(WINAPI *PFN_D3D12_CREATE_VERSIONED_ROOT_SIGNATURE_DESERIALIZER_LOCAL)(
	LPCVOID, SIZE_T, REFIID, void**);
typedef HRESULT(WINAPI *PFN_D3D12_ENABLE_EXPERIMENTAL_FEATURES_LOCAL)(
	UINT, const IID*, void*, UINT*);
typedef HRESULT(WINAPI *PFN_D3D12_GET_INTERFACE_LOCAL)(REFCLSID, REFIID, void**);
typedef UINT_PTR(WINAPI *PFN_D3D12_GENERIC_LOCAL)(
	UINT_PTR, UINT_PTR, UINT_PTR, UINT_PTR,
	UINT_PTR, UINT_PTR, UINT_PTR, UINT_PTR);

static PFN_D3D12_CREATE_DEVICE_LOCAL gOrigD3D12CreateDevice = nullptr;
static PFN_D3D12_GET_DEBUG_INTERFACE_LOCAL gOrigD3D12GetDebugInterface = nullptr;
static PFN_D3D12_SERIALIZE_ROOT_SIGNATURE_LOCAL gOrigD3D12SerializeRootSignature = nullptr;
static PFN_D3D12_CREATE_ROOT_SIGNATURE_DESERIALIZER_LOCAL gOrigD3D12CreateRootSignatureDeserializer = nullptr;
static PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE_LOCAL gOrigD3D12SerializeVersionedRootSignature = nullptr;
static PFN_D3D12_CREATE_VERSIONED_ROOT_SIGNATURE_DESERIALIZER_LOCAL gOrigD3D12CreateVersionedRootSignatureDeserializer = nullptr;
static PFN_D3D12_ENABLE_EXPERIMENTAL_FEATURES_LOCAL gOrigD3D12EnableExperimentalFeatures = nullptr;
static PFN_D3D12_GET_INTERFACE_LOCAL gOrigD3D12GetInterface = nullptr;

static FARPROC GetRealD3D12Proc(const char *name)
{
	HMODULE module = LoadRealD3D12();
	if (!module)
		return nullptr;

	FARPROC proc = GetProcAddress(module, name);
	if (!proc)
		DX12Log("Failed to resolve real d3d12 export %s, error=%lu\n", name, GetLastError());
	return proc;
}

static FARPROC GetRealD3D12Proc(WORD ordinal)
{
	HMODULE module = LoadRealD3D12();
	if (!module)
		return nullptr;

	FARPROC proc = GetProcAddress(module, MAKEINTRESOURCEA(ordinal));
	if (!proc)
		DX12Log("Failed to resolve real d3d12 ordinal %u, error=%lu\n", ordinal, GetLastError());
	return proc;
}

static UINT_PTR CallRealD3D12Proc(const char *name,
	UINT_PTR a1 = 0, UINT_PTR a2 = 0, UINT_PTR a3 = 0, UINT_PTR a4 = 0,
	UINT_PTR a5 = 0, UINT_PTR a6 = 0, UINT_PTR a7 = 0, UINT_PTR a8 = 0)
{
	FARPROC proc = GetRealD3D12Proc(name);
	if (!proc)
		return static_cast<UINT_PTR>(E_FAIL);

	return reinterpret_cast<PFN_D3D12_GENERIC_LOCAL>(proc)(a1, a2, a3, a4, a5, a6, a7, a8);
}

static UINT_PTR CallRealD3D12Proc(WORD ordinal,
	UINT_PTR a1 = 0, UINT_PTR a2 = 0, UINT_PTR a3 = 0, UINT_PTR a4 = 0,
	UINT_PTR a5 = 0, UINT_PTR a6 = 0, UINT_PTR a7 = 0, UINT_PTR a8 = 0)
{
	FARPROC proc = GetRealD3D12Proc(ordinal);
	if (!proc)
		return static_cast<UINT_PTR>(E_FAIL);

	return reinterpret_cast<PFN_D3D12_GENERIC_LOCAL>(proc)(a1, a2, a3, a4, a5, a6, a7, a8);
}

static HMODULE LoadRealD3D12()
{
	if (gRealD3D12)
		return gRealD3D12;

	wchar_t path[MAX_PATH];
	if (!GetSystemDirectoryW(path, MAX_PATH))
		return nullptr;
	PathAppendW(path, L"d3d12.dll");

	gRealD3D12 = LoadLibraryW(path);
	if (!gRealD3D12) {
		DX12Log("Failed to load real d3d12.dll from %S, error=%lu\n", path, GetLastError());
		return nullptr;
	}

	gOrigD3D12CreateDevice = reinterpret_cast<PFN_D3D12_CREATE_DEVICE_LOCAL>(
		GetProcAddress(gRealD3D12, "D3D12CreateDevice"));
	gOrigD3D12GetDebugInterface = reinterpret_cast<PFN_D3D12_GET_DEBUG_INTERFACE_LOCAL>(
		GetProcAddress(gRealD3D12, "D3D12GetDebugInterface"));
	gOrigD3D12SerializeRootSignature = reinterpret_cast<PFN_D3D12_SERIALIZE_ROOT_SIGNATURE_LOCAL>(
		GetProcAddress(gRealD3D12, "D3D12SerializeRootSignature"));
	gOrigD3D12CreateRootSignatureDeserializer =
		reinterpret_cast<PFN_D3D12_CREATE_ROOT_SIGNATURE_DESERIALIZER_LOCAL>(
			GetProcAddress(gRealD3D12, "D3D12CreateRootSignatureDeserializer"));
	gOrigD3D12SerializeVersionedRootSignature =
		reinterpret_cast<PFN_D3D12_SERIALIZE_VERSIONED_ROOT_SIGNATURE_LOCAL>(
			GetProcAddress(gRealD3D12, "D3D12SerializeVersionedRootSignature"));
	gOrigD3D12CreateVersionedRootSignatureDeserializer =
		reinterpret_cast<PFN_D3D12_CREATE_VERSIONED_ROOT_SIGNATURE_DESERIALIZER_LOCAL>(
			GetProcAddress(gRealD3D12, "D3D12CreateVersionedRootSignatureDeserializer"));
	gOrigD3D12EnableExperimentalFeatures =
		reinterpret_cast<PFN_D3D12_ENABLE_EXPERIMENTAL_FEATURES_LOCAL>(
			GetProcAddress(gRealD3D12, "D3D12EnableExperimentalFeatures"));
	gOrigD3D12GetInterface = reinterpret_cast<PFN_D3D12_GET_INTERFACE_LOCAL>(
		GetProcAddress(gRealD3D12, "D3D12GetInterface"));

	DX12Log("Loaded real d3d12.dll: %p\n", gRealD3D12);
	return gRealD3D12;
}

static DWORD WINAPI DX12WorkerThread(void*)
{
	DX12Log("DX12 worker thread started\n");
	LoadRealD3D12();

	for (int i = 0; i < 200; ++i) {
		DX12InstallDXGIHooks();
		Sleep(50);
	}

	return 0;
}

void DX12Initialize(HINSTANCE module)
{
	if (gInitialized)
		return;
	gInitialized = true;

	DX12SetModule(module);
	DX12OpenLogFile();
	DX12Log("\n*** 3DMigoto DX12 proxy initialized - frame analysis logging ready ***\n");

	HANDLE thread = CreateThread(nullptr, 0, DX12WorkerThread, nullptr, 0, nullptr);
	if (thread)
		CloseHandle(thread);

	HANDLE overlayThread = CreateThread(nullptr, 0, DX12OverlayThread, nullptr, 0, nullptr);
	if (overlayThread)
		CloseHandle(overlayThread);
}

void DX12Shutdown()
{
	DX12Log("*** 3DMigoto DX12 proxy shutting down ***\n");
	DX12CloseOverlayWindow();
	DX12UnhookAll();

	if (gRealD3D12) {
		FreeLibrary(gRealD3D12);
		gRealD3D12 = nullptr;
	}

	DX12CloseLogFile();
}

extern "C" HRESULT WINAPI D3D12CreateDevice(
	IUnknown *adapter, D3D_FEATURE_LEVEL minimumFeatureLevel, REFIID riid, void **device)
{
	LoadRealD3D12();
	DX12InstallDXGIHooks();
	DX12Log("D3D12CreateDevice adapter=%p minFeature=0x%x\n", adapter, minimumFeatureLevel);
	if (!gOrigD3D12CreateDevice)
		return E_FAIL;
	HRESULT hr = gOrigD3D12CreateDevice(adapter, minimumFeatureLevel, riid, device);
	DX12Log("D3D12CreateDevice result=0x%lx device=%p\n", hr, device ? *device : nullptr);
	if (SUCCEEDED(hr) && device && *device)
		DX12HookDevice(static_cast<IUnknown*>(*device));
	return hr;
}

extern "C" HRESULT WINAPI D3D12GetDebugInterface(REFIID riid, void **debug)
{
	LoadRealD3D12();
	if (!gOrigD3D12GetDebugInterface)
		return E_FAIL;
	return gOrigD3D12GetDebugInterface(riid, debug);
}

extern "C" HRESULT WINAPI D3D12SerializeRootSignature(
	const D3D12_ROOT_SIGNATURE_DESC *desc, D3D_ROOT_SIGNATURE_VERSION version,
	ID3DBlob **blob, ID3DBlob **errorBlob)
{
	LoadRealD3D12();
	if (!gOrigD3D12SerializeRootSignature)
		return E_FAIL;
	return gOrigD3D12SerializeRootSignature(desc, version, blob, errorBlob);
}

extern "C" HRESULT WINAPI D3D12CreateRootSignatureDeserializer(
	LPCVOID srcData, SIZE_T srcDataSize, REFIID riid, void **rootSignatureDeserializer)
{
	LoadRealD3D12();
	if (!gOrigD3D12CreateRootSignatureDeserializer)
		return E_FAIL;
	return gOrigD3D12CreateRootSignatureDeserializer(
		srcData, srcDataSize, riid, rootSignatureDeserializer);
}

extern "C" HRESULT WINAPI D3D12SerializeVersionedRootSignature(
	const D3D12_VERSIONED_ROOT_SIGNATURE_DESC *desc, ID3DBlob **blob, ID3DBlob **errorBlob)
{
	LoadRealD3D12();
	if (!gOrigD3D12SerializeVersionedRootSignature)
		return E_FAIL;
	return gOrigD3D12SerializeVersionedRootSignature(desc, blob, errorBlob);
}

extern "C" HRESULT WINAPI D3D12CreateVersionedRootSignatureDeserializer(
	LPCVOID srcData, SIZE_T srcDataSize, REFIID riid, void **rootSignatureDeserializer)
{
	LoadRealD3D12();
	if (!gOrigD3D12CreateVersionedRootSignatureDeserializer)
		return E_FAIL;
	return gOrigD3D12CreateVersionedRootSignatureDeserializer(
		srcData, srcDataSize, riid, rootSignatureDeserializer);
}

extern "C" HRESULT WINAPI D3D12EnableExperimentalFeatures(
	UINT numFeatures, const IID *featureIIDs, void *configurationStructs, UINT *configurationStructSizes)
{
	LoadRealD3D12();
	if (!gOrigD3D12EnableExperimentalFeatures)
		return E_FAIL;
	return gOrigD3D12EnableExperimentalFeatures(
		numFeatures, featureIIDs, configurationStructs, configurationStructSizes);
}

extern "C" HRESULT WINAPI D3D12GetInterface(REFCLSID clsid, REFIID riid, void **object)
{
	LoadRealD3D12();
	if (!gOrigD3D12GetInterface)
		return E_NOINTERFACE;
	HRESULT hr = gOrigD3D12GetInterface(clsid, riid, object);
	DX12Log("D3D12GetInterface clsid=%p riid=%p result=0x%lx object=%p\n",
		&clsid, &riid, hr, object ? *object : nullptr);
	if (SUCCEEDED(hr) && object && *object)
		DX12HookDeviceFactory(static_cast<IUnknown*>(*object));
	return hr;
}

extern "C" UINT_PTR WINAPI D3D12Ordinal99(
	UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4,
	UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8)
{
	return CallRealD3D12Proc(static_cast<WORD>(99), a1, a2, a3, a4, a5, a6, a7, a8);
}

extern "C" UINT_PTR WINAPI SetAppCompatStringPointer(
	UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4,
	UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8)
{
	return CallRealD3D12Proc("SetAppCompatStringPointer", a1, a2, a3, a4, a5, a6, a7, a8);
}

extern "C" UINT_PTR WINAPI D3D12CoreCreateLayeredDevice(
	UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4,
	UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8)
{
	return CallRealD3D12Proc("D3D12CoreCreateLayeredDevice", a1, a2, a3, a4, a5, a6, a7, a8);
}

extern "C" UINT_PTR WINAPI D3D12CoreGetLayeredDeviceSize(
	UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4,
	UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8)
{
	return CallRealD3D12Proc("D3D12CoreGetLayeredDeviceSize", a1, a2, a3, a4, a5, a6, a7, a8);
}

extern "C" UINT_PTR WINAPI D3D12CoreRegisterLayers(
	UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4,
	UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8)
{
	return CallRealD3D12Proc("D3D12CoreRegisterLayers", a1, a2, a3, a4, a5, a6, a7, a8);
}

extern "C" UINT_PTR WINAPI D3D12DeviceRemovedExtendedData(
	UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4,
	UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8)
{
	return CallRealD3D12Proc("D3D12DeviceRemovedExtendedData", a1, a2, a3, a4, a5, a6, a7, a8);
}

extern "C" UINT_PTR WINAPI D3D12PIXEventsReplaceBlock(
	UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4,
	UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8)
{
	return CallRealD3D12Proc("D3D12PIXEventsReplaceBlock", a1, a2, a3, a4, a5, a6, a7, a8);
}

extern "C" UINT_PTR WINAPI D3D12PIXGetThreadInfo(
	UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4,
	UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8)
{
	return CallRealD3D12Proc("D3D12PIXGetThreadInfo", a1, a2, a3, a4, a5, a6, a7, a8);
}

extern "C" UINT_PTR WINAPI D3D12PIXNotifyWakeFromFenceSignal(
	UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4,
	UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8)
{
	return CallRealD3D12Proc("D3D12PIXNotifyWakeFromFenceSignal", a1, a2, a3, a4, a5, a6, a7, a8);
}

extern "C" UINT_PTR WINAPI D3D12PIXReportCounter(
	UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4,
	UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8)
{
	return CallRealD3D12Proc("D3D12PIXReportCounter", a1, a2, a3, a4, a5, a6, a7, a8);
}

extern "C" UINT_PTR WINAPI GetBehaviorValue(
	UINT_PTR a1, UINT_PTR a2, UINT_PTR a3, UINT_PTR a4,
	UINT_PTR a5, UINT_PTR a6, UINT_PTR a7, UINT_PTR a8)
{
	return CallRealD3D12Proc("GetBehaviorValue", a1, a2, a3, a4, a5, a6, a7, a8);
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID)
{
	switch (reason) {
	case DLL_PROCESS_ATTACH:
		DisableThreadLibraryCalls(module);
		DX12Initialize(module);
		break;
	case DLL_PROCESS_DETACH:
		DX12Shutdown();
		break;
	}
	return TRUE;
}
