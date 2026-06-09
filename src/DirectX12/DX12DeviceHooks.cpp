#include "DX12DeviceHooks.h"

#include <d3d12.h>

#include "DX12ShaderDump.h"
#include "DX12State.h"

typedef HRESULT(STDMETHODCALLTYPE *PFN_CREATE_GRAPHICS_PIPELINE_STATE)(
	ID3D12Device*, const D3D12_GRAPHICS_PIPELINE_STATE_DESC*, REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CREATE_COMPUTE_PIPELINE_STATE)(
	ID3D12Device*, const D3D12_COMPUTE_PIPELINE_STATE_DESC*, REFIID, void**);

static PFN_CREATE_GRAPHICS_PIPELINE_STATE gOrigCreateGraphicsPipelineState = nullptr;
static PFN_CREATE_COMPUTE_PIPELINE_STATE gOrigCreateComputePipelineState = nullptr;

static HRESULT STDMETHODCALLTYPE HookedCreateGraphicsPipelineState(
	ID3D12Device *device, const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc,
	REFIID riid, void **pipelineState)
{
	HRESULT hr = gOrigCreateGraphicsPipelineState(device, desc, riid, pipelineState);
	if (SUCCEEDED(hr) && desc)
		DX12RecordGraphicsPipelineState(desc);
	return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreateComputePipelineState(
	ID3D12Device *device, const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc,
	REFIID riid, void **pipelineState)
{
	HRESULT hr = gOrigCreateComputePipelineState(device, desc, riid, pipelineState);
	if (SUCCEEDED(hr) && desc)
		DX12RecordComputePipelineState(desc);
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

	baseDevice->Release();
}
