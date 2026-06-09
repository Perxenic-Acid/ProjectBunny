#include "DXGIHooks.h"

#include <dxgi1_4.h>

#include "DX12Input.h"
#include "DX12Overlay.h"
#include "DX12State.h"

typedef HRESULT(STDMETHODCALLTYPE *PFN_CREATE_SWAP_CHAIN)(
	IDXGIFactory*, IUnknown*, DXGI_SWAP_CHAIN_DESC*, IDXGISwapChain**);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CREATE_SWAP_CHAIN_FOR_HWND)(
	IDXGIFactory2*, IUnknown*, HWND, const DXGI_SWAP_CHAIN_DESC1*,
	const DXGI_SWAP_CHAIN_FULLSCREEN_DESC*, IDXGIOutput*, IDXGISwapChain1**);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CREATE_SWAP_CHAIN_FOR_CORE_WINDOW)(
	IDXGIFactory2*, IUnknown*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*,
	IDXGIOutput*, IDXGISwapChain1**);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CREATE_SWAP_CHAIN_FOR_COMPOSITION)(
	IDXGIFactory2*, IUnknown*, const DXGI_SWAP_CHAIN_DESC1*, IDXGIOutput*, IDXGISwapChain1**);
typedef HRESULT(STDMETHODCALLTYPE *PFN_PRESENT)(IDXGISwapChain*, UINT, UINT);
typedef HRESULT(STDMETHODCALLTYPE *PFN_PRESENT1)(
	IDXGISwapChain1*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);
typedef HRESULT(WINAPI *PFN_CREATE_DXGI_FACTORY)(REFIID, void**);
typedef HRESULT(WINAPI *PFN_CREATE_DXGI_FACTORY2)(UINT, REFIID, void**);

static bool gDXGIHookAttempted = false;
static SRWLOCK gDXGIHookLock = SRWLOCK_INIT;
static PFN_CREATE_SWAP_CHAIN gOrigCreateSwapChain = nullptr;
static PFN_CREATE_SWAP_CHAIN_FOR_HWND gOrigCreateSwapChainForHwnd = nullptr;
static PFN_CREATE_SWAP_CHAIN_FOR_CORE_WINDOW gOrigCreateSwapChainForCoreWindow = nullptr;
static PFN_CREATE_SWAP_CHAIN_FOR_COMPOSITION gOrigCreateSwapChainForComposition = nullptr;
static PFN_PRESENT gOrigPresent = nullptr;
static PFN_PRESENT1 gOrigPresent1 = nullptr;
static PFN_CREATE_DXGI_FACTORY gOrigCreateDXGIFactory = nullptr;
static PFN_CREATE_DXGI_FACTORY gOrigCreateDXGIFactory1 = nullptr;
static PFN_CREATE_DXGI_FACTORY2 gOrigCreateDXGIFactory2 = nullptr;

static HRESULT WINAPI HookedCreateDXGIFactory(REFIID riid, void **factory);
static HRESULT WINAPI HookedCreateDXGIFactory1(REFIID riid, void **factory);
static HRESULT WINAPI HookedCreateDXGIFactory2(UINT flags, REFIID riid, void **factory);
static HRESULT STDMETHODCALLTYPE HookedCreateSwapChain(
	IDXGIFactory *factory, IUnknown *device, DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **swapChain);
static HRESULT STDMETHODCALLTYPE HookedCreateSwapChainForHwnd(
	IDXGIFactory2 *factory, IUnknown *device, HWND window, const DXGI_SWAP_CHAIN_DESC1 *desc,
	const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreenDesc, IDXGIOutput *output, IDXGISwapChain1 **swapChain);
static HRESULT STDMETHODCALLTYPE HookedCreateSwapChainForCoreWindow(
	IDXGIFactory2 *factory, IUnknown *device, IUnknown *window, const DXGI_SWAP_CHAIN_DESC1 *desc,
	IDXGIOutput *output, IDXGISwapChain1 **swapChain);
static HRESULT STDMETHODCALLTYPE HookedCreateSwapChainForComposition(
	IDXGIFactory2 *factory, IUnknown *device, const DXGI_SWAP_CHAIN_DESC1 *desc,
	IDXGIOutput *output, IDXGISwapChain1 **swapChain);
static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain *swapChain, UINT syncInterval, UINT flags);
static HRESULT STDMETHODCALLTYPE HookedPresent1(
	IDXGISwapChain1 *swapChain, UINT syncInterval, UINT flags, const DXGI_PRESENT_PARAMETERS *presentParameters);

static void HookSwapChain(IDXGISwapChain *swapChain)
{
	if (!swapChain)
		return;

	void **vtable = *reinterpret_cast<void***>(swapChain);
	DX12HookFunction(reinterpret_cast<void**>(&gOrigPresent),
		vtable[8], HookedPresent, "IDXGISwapChain::Present");

	IDXGISwapChain1 *swapChain1 = nullptr;
	if (SUCCEEDED(swapChain->QueryInterface(IID_PPV_ARGS(&swapChain1)))) {
		void **vtable1 = *reinterpret_cast<void***>(swapChain1);
		DX12HookFunction(reinterpret_cast<void**>(&gOrigPresent1),
			vtable1[22], HookedPresent1, "IDXGISwapChain1::Present1");
		swapChain1->Release();
	}
}

static void HookFactory(IUnknown *factory)
{
	if (!factory)
		return;

	IDXGIFactory *factory0 = nullptr;
	if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory0)))) {
		void **vtable = *reinterpret_cast<void***>(factory0);
		DX12HookFunction(reinterpret_cast<void**>(&gOrigCreateSwapChain),
			vtable[10], HookedCreateSwapChain, "IDXGIFactory::CreateSwapChain");
		factory0->Release();
	}

	IDXGIFactory2 *factory2 = nullptr;
	if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory2)))) {
		void **vtable = *reinterpret_cast<void***>(factory2);
		DX12HookFunction(reinterpret_cast<void**>(&gOrigCreateSwapChainForHwnd),
			vtable[15], HookedCreateSwapChainForHwnd, "IDXGIFactory2::CreateSwapChainForHwnd");
		DX12HookFunction(reinterpret_cast<void**>(&gOrigCreateSwapChainForCoreWindow),
			vtable[16], HookedCreateSwapChainForCoreWindow, "IDXGIFactory2::CreateSwapChainForCoreWindow");
		DX12HookFunction(reinterpret_cast<void**>(&gOrigCreateSwapChainForComposition),
			vtable[24], HookedCreateSwapChainForComposition, "IDXGIFactory2::CreateSwapChainForComposition");
		factory2->Release();
	}
}

static void HookDXGIFactoryVTables(HMODULE dxgi)
{
	auto createFactory = reinterpret_cast<PFN_CREATE_DXGI_FACTORY>(
		GetProcAddress(dxgi, "CreateDXGIFactory"));
	auto createFactory1 = reinterpret_cast<PFN_CREATE_DXGI_FACTORY>(
		GetProcAddress(dxgi, "CreateDXGIFactory1"));
	auto createFactory2 = reinterpret_cast<PFN_CREATE_DXGI_FACTORY2>(
		GetProcAddress(dxgi, "CreateDXGIFactory2"));

	IDXGIFactory *factory = nullptr;
	if (createFactory1 && SUCCEEDED(createFactory1(IID_PPV_ARGS(&factory)))) {
		HookFactory(factory);
		factory->Release();
		return;
	}

	if (createFactory && SUCCEEDED(createFactory(IID_PPV_ARGS(&factory)))) {
		HookFactory(factory);
		factory->Release();
	}

	IDXGIFactory2 *factory2 = nullptr;
	if (createFactory2 && SUCCEEDED(createFactory2(0, IID_PPV_ARGS(&factory2)))) {
		HookFactory(factory2);
		factory2->Release();
	}
}

static HRESULT WINAPI HookedCreateDXGIFactory(REFIID riid, void **factory)
{
	HRESULT hr = gOrigCreateDXGIFactory(riid, factory);
	DX12Log("CreateDXGIFactory riid=%p result=0x%lx factory=%p\n",
		&riid, hr, factory ? *factory : nullptr);
	if (SUCCEEDED(hr) && factory)
		HookFactory(static_cast<IUnknown*>(*factory));
	return hr;
}

static HRESULT WINAPI HookedCreateDXGIFactory1(REFIID riid, void **factory)
{
	HRESULT hr = gOrigCreateDXGIFactory1(riid, factory);
	DX12Log("CreateDXGIFactory1 riid=%p result=0x%lx factory=%p\n",
		&riid, hr, factory ? *factory : nullptr);
	if (SUCCEEDED(hr) && factory)
		HookFactory(static_cast<IUnknown*>(*factory));
	return hr;
}

static HRESULT WINAPI HookedCreateDXGIFactory2(UINT flags, REFIID riid, void **factory)
{
	HRESULT hr = gOrigCreateDXGIFactory2(flags, riid, factory);
	DX12Log("CreateDXGIFactory2 flags=0x%x riid=%p result=0x%lx factory=%p\n",
		flags, &riid, hr, factory ? *factory : nullptr);
	if (SUCCEEDED(hr) && factory)
		HookFactory(static_cast<IUnknown*>(*factory));
	return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreateSwapChain(
	IDXGIFactory *factory, IUnknown *device, DXGI_SWAP_CHAIN_DESC *desc, IDXGISwapChain **swapChain)
{
	HRESULT hr = gOrigCreateSwapChain(factory, device, desc, swapChain);
	DX12Log("CreateSwapChain device=%p result=0x%lx swapchain=%p\n",
		device, hr, swapChain ? *swapChain : nullptr);
	if (SUCCEEDED(hr) && swapChain)
		HookSwapChain(*swapChain);
	return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreateSwapChainForHwnd(
	IDXGIFactory2 *factory, IUnknown *device, HWND window, const DXGI_SWAP_CHAIN_DESC1 *desc,
	const DXGI_SWAP_CHAIN_FULLSCREEN_DESC *fullscreenDesc, IDXGIOutput *output, IDXGISwapChain1 **swapChain)
{
	HRESULT hr = gOrigCreateSwapChainForHwnd(factory, device, window, desc, fullscreenDesc, output, swapChain);
	DX12Log("CreateSwapChainForHwnd device=%p hwnd=%p result=0x%lx swapchain=%p\n",
		device, window, hr, swapChain ? *swapChain : nullptr);
	if (SUCCEEDED(hr) && swapChain)
		HookSwapChain(*swapChain);
	return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreateSwapChainForCoreWindow(
	IDXGIFactory2 *factory, IUnknown *device, IUnknown *window, const DXGI_SWAP_CHAIN_DESC1 *desc,
	IDXGIOutput *output, IDXGISwapChain1 **swapChain)
{
	HRESULT hr = gOrigCreateSwapChainForCoreWindow(factory, device, window, desc, output, swapChain);
	DX12Log("CreateSwapChainForCoreWindow device=%p result=0x%lx swapchain=%p\n",
		device, hr, swapChain ? *swapChain : nullptr);
	if (SUCCEEDED(hr) && swapChain)
		HookSwapChain(*swapChain);
	return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreateSwapChainForComposition(
	IDXGIFactory2 *factory, IUnknown *device, const DXGI_SWAP_CHAIN_DESC1 *desc,
	IDXGIOutput *output, IDXGISwapChain1 **swapChain)
{
	HRESULT hr = gOrigCreateSwapChainForComposition(factory, device, desc, output, swapChain);
	DX12Log("CreateSwapChainForComposition device=%p result=0x%lx swapchain=%p\n",
		device, hr, swapChain ? *swapChain : nullptr);
	if (SUCCEEDED(hr) && swapChain)
		HookSwapChain(*swapChain);
	return hr;
}

static HRESULT STDMETHODCALLTYPE HookedPresent(IDXGISwapChain *swapChain, UINT syncInterval, UINT flags)
{
	DX12PollInput();
	HRESULT hr = gOrigPresent(swapChain, syncInterval, flags);
	DX12IncrementPresentCount();
	DX12DrawSwapChainText(swapChain);
	return hr;
}

static HRESULT STDMETHODCALLTYPE HookedPresent1(
	IDXGISwapChain1 *swapChain, UINT syncInterval, UINT flags, const DXGI_PRESENT_PARAMETERS *presentParameters)
{
	DX12PollInput();
	HRESULT hr = gOrigPresent1(swapChain, syncInterval, flags, presentParameters);
	DX12IncrementPresentCount();
	DX12DrawSwapChainText(swapChain);
	return hr;
}

void DX12InstallDXGIHooks()
{
	AcquireSRWLockExclusive(&gDXGIHookLock);
	if (gDXGIHookAttempted) {
		ReleaseSRWLockExclusive(&gDXGIHookLock);
		return;
	}
	gDXGIHookAttempted = true;
	ReleaseSRWLockExclusive(&gDXGIHookLock);

	HMODULE dxgi = GetModuleHandleW(L"dxgi.dll");
	if (!dxgi) {
		DX12Log("dxgi.dll is not loaded yet; DXGI hook deferred\n");
		AcquireSRWLockExclusive(&gDXGIHookLock);
		gDXGIHookAttempted = false;
		ReleaseSRWLockExclusive(&gDXGIHookLock);
		return;
	}

	HookDXGIFactoryVTables(dxgi);
	DX12HookFunction(reinterpret_cast<void**>(&gOrigCreateDXGIFactory),
		GetProcAddress(dxgi, "CreateDXGIFactory"), HookedCreateDXGIFactory, "CreateDXGIFactory");
	DX12HookFunction(reinterpret_cast<void**>(&gOrigCreateDXGIFactory1),
		GetProcAddress(dxgi, "CreateDXGIFactory1"), HookedCreateDXGIFactory1, "CreateDXGIFactory1");
	DX12HookFunction(reinterpret_cast<void**>(&gOrigCreateDXGIFactory2),
		GetProcAddress(dxgi, "CreateDXGIFactory2"), HookedCreateDXGIFactory2, "CreateDXGIFactory2");
}
