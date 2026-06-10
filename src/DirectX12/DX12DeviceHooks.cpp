#include "DX12DeviceHooks.h"

#include <d3d12.h>

#include "DX12CommandListHooks.h"
#include "DX12ResourceTracker.h"
#include "DX12ShaderDump.h"
#include "DX12State.h"

typedef HRESULT(STDMETHODCALLTYPE *PFN_CREATE_GRAPHICS_PIPELINE_STATE)(
	ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CREATE_COMPUTE_PIPELINE_STATE)(
	ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CREATE_PIPELINE_STATE)(
	ID3D12Device2*, const D3D12_PIPELINE_STATE_STREAM_DESC*, REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE *PFN_DEVICE_FACTORY_CREATE_DEVICE)(
	ID3D12DeviceFactory*, IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

static PFN_CREATE_GRAPHICS_PIPELINE_STATE gOrigCreateGraphicsPipelineState = nullptr;
static PFN_CREATE_COMPUTE_PIPELINE_STATE gOrigCreateComputePipelineState = nullptr;
static PFN_CREATE_PIPELINE_STATE gOrigCreatePipelineState = nullptr;
static PFN_DEVICE_FACTORY_CREATE_DEVICE gOrigDeviceFactoryCreateDevice = nullptr;

static HRESULT STDMETHODCALLTYPE HookedCreateGraphicsPipelineState(
	ID3D12Device *device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
	REFIID riid, void **pipelineState)
{
	HRESULT hr = gOrigCreateGraphicsPipelineState(device, desc, riid, pipelineState);
	if (SUCCEEDED(hr) && desc)
		DX12RecordGraphicsPipelineState(
			pipelineState ? static_cast<ID3D12PipelineState*>(*pipelineState) : nullptr, desc);
	return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreateComputePipelineState(
	ID3D12Device *device, const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
	REFIID riid, void **pipelineState)
{
	HRESULT hr = gOrigCreateComputePipelineState(device, desc, riid, pipelineState);
	if (SUCCEEDED(hr) && desc)
		DX12RecordComputePipelineState(
			pipelineState ? static_cast<ID3D12PipelineState*>(*pipelineState) : nullptr, desc);
	return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreatePipelineState(
	ID3D12Device2 *device, const D3D12_PIPELINE_STATE_STREAM_DESC *desc,
	REFIID riid, void **pipelineState)
{
	HRESULT hr = gOrigCreatePipelineState(device, desc, riid, pipelineState);
	if (SUCCEEDED(hr) && desc)
		DX12RecordPipelineStateStream(
			pipelineState ? static_cast<ID3D12PipelineState*>(*pipelineState) : nullptr, desc);
	return hr;
}

static HRESULT STDMETHODCALLTYPE HookedDeviceFactoryCreateDevice(
	ID3D12DeviceFactory *factory, IUnknown *adapter, D3D_FEATURE_LEVEL featureLevel,
	REFIID riid, void **device)
{
	HRESULT hr = gOrigDeviceFactoryCreateDevice(factory, adapter, featureLevel, riid, device);
	DX12Log("ID3D12DeviceFactory::CreateDevice result=0x%lx device=%p\n",
		hr, device ? *device : nullptr);
	if (SUCCEEDED(hr) && device && *device)
		DX12HookDevice(static_cast<IUnknown*>(*device));
	return hr;
}

void DX12HookDevice(IUnknown *device)
{
	if (!device)
		return;

	ID3D12Device *baseDevice = nullptr;
	if (FAILED(device->QueryInterface(IID_PPV_ARGS(&baseDevice))))
		return;

	void **vtable = *reinterpret_cast<void***>(baseDevice);
	if (!vtable)
	{
		baseDevice->Release();
		return;
	}

	// IUnknown(0-2) + ID3D12Object(3-6) + ID3D12Device methods:
	// GetNodeCount(7), CreateCommandQueue(8), CreateCommandAllocator(9).
	constexpr size_t CreateGraphicsPipelineStateIndex = 10;
	constexpr size_t CreateComputePipelineStateIndex = 11;

	DX12HookFunction(reinterpret_cast<void**>(&gOrigCreateGraphicsPipelineState),
		vtable[CreateGraphicsPipelineStateIndex], HookedCreateGraphicsPipelineState,
		"ID3D12Device::CreateGraphicsPipelineState");
	DX12HookFunction(reinterpret_cast<void**>(&gOrigCreateComputePipelineState),
		vtable[CreateComputePipelineStateIndex], HookedCreateComputePipelineState,
		"ID3D12Device::CreateComputePipelineState");
	DX12HookResourceMetadata(baseDevice);
	DX12Log("Skipping command-list hooks for UE/DX12 stability\n");

	baseDevice->Release();

	ID3D12Device2 *device2 = nullptr;
	if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device2)))) {
		void **vtable2 = *reinterpret_cast<void***>(device2);
		if (vtable2) {
			// IUnknown + ID3D12Object + ID3D12Device(37 methods) + ID3D12Device1(3 methods).
			constexpr size_t CreatePipelineStateIndex = 47;
			DX12HookFunction(reinterpret_cast<void**>(&gOrigCreatePipelineState),
				vtable2[CreatePipelineStateIndex], HookedCreatePipelineState,
				"ID3D12Device2::CreatePipelineState");
		}
		device2->Release();
	}
	// Command-list hooks only record binding metadata; resource readback stays disabled.
}

void DX12HookDeviceFactory(IUnknown *factory)
{
	if (!factory)
		return;

	ID3D12DeviceFactory *deviceFactory = nullptr;
	if (FAILED(factory->QueryInterface(IID_PPV_ARGS(&deviceFactory))))
		return;

	void **vtable = *reinterpret_cast<void***>(deviceFactory);
	if (vtable) {
		constexpr size_t CreateDeviceIndex = 9;
		DX12HookFunction(reinterpret_cast<void**>(&gOrigDeviceFactoryCreateDevice),
			vtable[CreateDeviceIndex], HookedDeviceFactoryCreateDevice,
			"ID3D12DeviceFactory::CreateDevice");
	}
	deviceFactory->Release();
}

void DX12HookDeviceFromCommandQueue(IUnknown *commandQueue)
{
	if (!commandQueue)
		return;

	ID3D12CommandQueue *queue = nullptr;
	if (FAILED(commandQueue->QueryInterface(IID_PPV_ARGS(&queue))))
		return;

	DX12SetCommandQueue(queue);

	ID3D12Device *device = nullptr;
	HRESULT hr = queue->GetDevice(IID_PPV_ARGS(&device));
	DX12Log("ID3D12CommandQueue::GetDevice result=0x%lx device=%p\n", hr, device);
	if (SUCCEEDED(hr) && device) {
		DX12HookDevice(device);
		device->Release();
	}
	queue->Release();
}
