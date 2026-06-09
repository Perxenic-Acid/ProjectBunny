#include "DX12CommandListHooks.h"

#include <d3d12.h>

#include "DX12BindingTracker.h"
#include "DX12ResourceTracker.h"
#include "DX12State.h"

typedef HRESULT(STDMETHODCALLTYPE *PFN_CREATE_COMMAND_LIST)(
	ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*,
	ID3D12PipelineState*, REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CREATE_COMMAND_LIST1)(
	ID3D12Device4*, UINT, D3D12_COMMAND_LIST_TYPE, D3D12_COMMAND_LIST_FLAGS, REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE *PFN_RESET_COMMAND_LIST)(
	ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
typedef void(STDMETHODCALLTYPE *PFN_SET_PIPELINE_STATE)(
	ID3D12GraphicsCommandList*, ID3D12PipelineState*);
typedef void(STDMETHODCALLTYPE *PFN_RESOURCE_BARRIER)(
	ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
typedef void(STDMETHODCALLTYPE *PFN_SET_DESCRIPTOR_HEAPS)(
	ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap *const*);
typedef void(STDMETHODCALLTYPE *PFN_SET_ROOT_DESCRIPTOR_TABLE)(
	ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
typedef void(STDMETHODCALLTYPE *PFN_IA_SET_PRIMITIVE_TOPOLOGY)(
	ID3D12GraphicsCommandList*, D3D12_PRIMITIVE_TOPOLOGY);
typedef void(STDMETHODCALLTYPE *PFN_IA_SET_INDEX_BUFFER)(
	ID3D12GraphicsCommandList*, const D3D12_INDEX_BUFFER_VIEW*);
typedef void(STDMETHODCALLTYPE *PFN_IA_SET_VERTEX_BUFFERS)(
	ID3D12GraphicsCommandList*, UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW*);
typedef void(STDMETHODCALLTYPE *PFN_DRAW_INSTANCED)(
	ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
typedef void(STDMETHODCALLTYPE *PFN_DRAW_INDEXED_INSTANCED)(
	ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
typedef void(STDMETHODCALLTYPE *PFN_DISPATCH)(
	ID3D12GraphicsCommandList*, UINT, UINT, UINT);

static PFN_CREATE_COMMAND_LIST gOrigCreateCommandList = nullptr;
static PFN_CREATE_COMMAND_LIST1 gOrigCreateCommandList1 = nullptr;
static PFN_RESET_COMMAND_LIST gOrigResetCommandList = nullptr;
static PFN_SET_PIPELINE_STATE gOrigSetPipelineState = nullptr;
static PFN_RESOURCE_BARRIER gOrigResourceBarrier = nullptr;
static PFN_SET_DESCRIPTOR_HEAPS gOrigSetDescriptorHeaps = nullptr;
static PFN_SET_ROOT_DESCRIPTOR_TABLE gOrigSetComputeRootDescriptorTable = nullptr;
static PFN_SET_ROOT_DESCRIPTOR_TABLE gOrigSetGraphicsRootDescriptorTable = nullptr;
static PFN_IA_SET_PRIMITIVE_TOPOLOGY gOrigIASetPrimitiveTopology = nullptr;
static PFN_IA_SET_INDEX_BUFFER gOrigIASetIndexBuffer = nullptr;
static PFN_IA_SET_VERTEX_BUFFERS gOrigIASetVertexBuffers = nullptr;
static PFN_DRAW_INSTANCED gOrigDrawInstanced = nullptr;
static PFN_DRAW_INDEXED_INSTANCED gOrigDrawIndexedInstanced = nullptr;
static PFN_DISPATCH gOrigDispatch = nullptr;

static void RegisterCommandList(IUnknown *commandList, ID3D12PipelineState *initialState)
{
	if (!commandList)
		return;

	ID3D12GraphicsCommandList *baseList = nullptr;
	if (SUCCEEDED(commandList->QueryInterface(IID_PPV_ARGS(&baseList)))) {
		DX12HookCommandList(baseList);
		DX12BindingRegisterCommandList(baseList);
		DX12BindingResetCommandList(baseList, initialState);
		baseList->Release();
	}
}

static HRESULT STDMETHODCALLTYPE HookedCreateCommandList(
	ID3D12Device *device, UINT nodeMask, D3D12_COMMAND_LIST_TYPE type,
	ID3D12CommandAllocator *allocator, ID3D12PipelineState *initialState,
	REFIID riid, void **commandList)
{
	HRESULT hr = gOrigCreateCommandList(device, nodeMask, type, allocator, initialState, riid, commandList);
	if (SUCCEEDED(hr) && commandList && *commandList) {
		DX12Log("ID3D12Device::CreateCommandList type=%d result=0x%lx commandList=%p initialPso=%p\n",
			static_cast<int>(type), hr, *commandList, initialState);
		RegisterCommandList(static_cast<IUnknown*>(*commandList), initialState);
	}
	return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreateCommandList1(
	ID3D12Device4 *device, UINT nodeMask, D3D12_COMMAND_LIST_TYPE type,
	D3D12_COMMAND_LIST_FLAGS flags, REFIID riid, void **commandList)
{
	HRESULT hr = gOrigCreateCommandList1(device, nodeMask, type, flags, riid, commandList);
	if (SUCCEEDED(hr) && commandList && *commandList) {
		DX12Log("ID3D12Device4::CreateCommandList1 type=%d result=0x%lx commandList=%p\n",
			static_cast<int>(type), hr, *commandList);
		RegisterCommandList(static_cast<IUnknown*>(*commandList), nullptr);
	}
	return hr;
}

static HRESULT STDMETHODCALLTYPE HookedResetCommandList(
	ID3D12GraphicsCommandList *commandList, ID3D12CommandAllocator *allocator,
	ID3D12PipelineState *initialState)
{
	DX12BindingResetCommandList(commandList, initialState);
	return gOrigResetCommandList(commandList, allocator, initialState);
}

static void STDMETHODCALLTYPE HookedSetPipelineState(
	ID3D12GraphicsCommandList *commandList, ID3D12PipelineState *pipelineState)
{
	DX12BindingSetPipelineState(commandList, pipelineState);
	DX12BindingRecordStateEvent(commandList, "set_pso");
	gOrigSetPipelineState(commandList, pipelineState);
}

static void STDMETHODCALLTYPE HookedResourceBarrier(
	ID3D12GraphicsCommandList *commandList, UINT numBarriers,
	const D3D12_RESOURCE_BARRIER *barriers)
{
	DX12RecordResourceBarrier(numBarriers, barriers);
	gOrigResourceBarrier(commandList, numBarriers, barriers);
}

static void STDMETHODCALLTYPE HookedSetDescriptorHeaps(
	ID3D12GraphicsCommandList *commandList, UINT count,
	ID3D12DescriptorHeap *const *heaps)
{
	DX12BindingSetDescriptorHeaps(commandList, count, heaps);
	DX12BindingRecordStateEvent(commandList, "set_heaps");
	gOrigSetDescriptorHeaps(commandList, count, heaps);
}

static void STDMETHODCALLTYPE HookedSetComputeRootDescriptorTable(
	ID3D12GraphicsCommandList *commandList, UINT rootParameterIndex,
	D3D12_GPU_DESCRIPTOR_HANDLE baseDescriptor)
{
	DX12BindingSetComputeRootDescriptorTable(commandList, rootParameterIndex, baseDescriptor);
	DX12BindingRecordStateEvent(commandList, "set_compute_table");
	gOrigSetComputeRootDescriptorTable(commandList, rootParameterIndex, baseDescriptor);
}

static void STDMETHODCALLTYPE HookedSetGraphicsRootDescriptorTable(
	ID3D12GraphicsCommandList *commandList, UINT rootParameterIndex,
	D3D12_GPU_DESCRIPTOR_HANDLE baseDescriptor)
{
	DX12BindingSetGraphicsRootDescriptorTable(commandList, rootParameterIndex, baseDescriptor);
	DX12BindingRecordStateEvent(commandList, "set_graphics_table");
	gOrigSetGraphicsRootDescriptorTable(commandList, rootParameterIndex, baseDescriptor);
}

static void STDMETHODCALLTYPE HookedDrawInstanced(
	ID3D12GraphicsCommandList *commandList, UINT vertexCountPerInstance, UINT instanceCount,
	UINT startVertexLocation, UINT startInstanceLocation)
{
	DX12BindingRecordDrawInstanced(commandList, vertexCountPerInstance, instanceCount,
		startVertexLocation, startInstanceLocation);
	gOrigDrawInstanced(commandList, vertexCountPerInstance, instanceCount,
		startVertexLocation, startInstanceLocation);
}

static void STDMETHODCALLTYPE HookedDrawIndexedInstanced(
	ID3D12GraphicsCommandList *commandList, UINT indexCountPerInstance, UINT instanceCount,
	UINT startIndexLocation, INT baseVertexLocation, UINT startInstanceLocation)
{
	DX12BindingRecordDrawIndexedInstanced(commandList, indexCountPerInstance, instanceCount,
		startIndexLocation, baseVertexLocation, startInstanceLocation);
	gOrigDrawIndexedInstanced(commandList, indexCountPerInstance, instanceCount,
		startIndexLocation, baseVertexLocation, startInstanceLocation);
}

static void STDMETHODCALLTYPE HookedDispatch(
	ID3D12GraphicsCommandList *commandList, UINT threadGroupCountX,
	UINT threadGroupCountY, UINT threadGroupCountZ)
{
	DX12BindingRecordDispatch(commandList, threadGroupCountX, threadGroupCountY, threadGroupCountZ);
	gOrigDispatch(commandList, threadGroupCountX, threadGroupCountY, threadGroupCountZ);
}

static void STDMETHODCALLTYPE HookedIASetPrimitiveTopology(
	ID3D12GraphicsCommandList *commandList, D3D12_PRIMITIVE_TOPOLOGY topology)
{
	DX12BindingSetPrimitiveTopology(commandList, topology);
	gOrigIASetPrimitiveTopology(commandList, topology);
}

static void STDMETHODCALLTYPE HookedIASetIndexBuffer(
	ID3D12GraphicsCommandList *commandList, const D3D12_INDEX_BUFFER_VIEW *view)
{
	DX12BindingSetIndexBuffer(commandList, view);
	gOrigIASetIndexBuffer(commandList, view);
}

static void STDMETHODCALLTYPE HookedIASetVertexBuffers(
	ID3D12GraphicsCommandList *commandList, UINT startSlot, UINT count,
	const D3D12_VERTEX_BUFFER_VIEW *views)
{
	DX12BindingSetVertexBuffers(commandList, startSlot, count, views);
	gOrigIASetVertexBuffers(commandList, startSlot, count, views);
}

void DX12HookCommandList(IUnknown *commandList)
{
	if (!commandList)
		return;

	ID3D12GraphicsCommandList *baseList = nullptr;
	if (FAILED(commandList->QueryInterface(IID_PPV_ARGS(&baseList))))
		return;

	void **vtable = *reinterpret_cast<void***>(baseList);
	if (vtable) {
		DX12HookFunction(reinterpret_cast<void**>(&gOrigResetCommandList),
			vtable[10], HookedResetCommandList, "ID3D12GraphicsCommandList::Reset");
		DX12HookFunction(reinterpret_cast<void**>(&gOrigSetPipelineState),
			vtable[25], HookedSetPipelineState, "ID3D12GraphicsCommandList::SetPipelineState");
		DX12HookFunction(reinterpret_cast<void**>(&gOrigResourceBarrier),
			vtable[26], HookedResourceBarrier, "ID3D12GraphicsCommandList::ResourceBarrier");
		DX12HookFunction(reinterpret_cast<void**>(&gOrigSetDescriptorHeaps),
			vtable[28], HookedSetDescriptorHeaps, "ID3D12GraphicsCommandList::SetDescriptorHeaps");
		DX12HookFunction(reinterpret_cast<void**>(&gOrigSetComputeRootDescriptorTable),
			vtable[31], HookedSetComputeRootDescriptorTable,
			"ID3D12GraphicsCommandList::SetComputeRootDescriptorTable");
		DX12HookFunction(reinterpret_cast<void**>(&gOrigSetGraphicsRootDescriptorTable),
			vtable[32], HookedSetGraphicsRootDescriptorTable,
			"ID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable");
		DX12HookFunction(reinterpret_cast<void**>(&gOrigIASetPrimitiveTopology),
			vtable[20], HookedIASetPrimitiveTopology,
			"ID3D12GraphicsCommandList::IASetPrimitiveTopology");
		DX12HookFunction(reinterpret_cast<void**>(&gOrigIASetIndexBuffer),
			vtable[43], HookedIASetIndexBuffer,
			"ID3D12GraphicsCommandList::IASetIndexBuffer");
		DX12HookFunction(reinterpret_cast<void**>(&gOrigIASetVertexBuffers),
			vtable[44], HookedIASetVertexBuffers,
			"ID3D12GraphicsCommandList::IASetVertexBuffers");
		DX12HookFunction(reinterpret_cast<void**>(&gOrigDrawInstanced),
			vtable[12], HookedDrawInstanced,
			"ID3D12GraphicsCommandList::DrawInstanced");
		DX12HookFunction(reinterpret_cast<void**>(&gOrigDrawIndexedInstanced),
			vtable[13], HookedDrawIndexedInstanced,
			"ID3D12GraphicsCommandList::DrawIndexedInstanced");
		DX12HookFunction(reinterpret_cast<void**>(&gOrigDispatch),
			vtable[14], HookedDispatch,
			"ID3D12GraphicsCommandList::Dispatch");
	}
	baseList->Release();
}

void DX12HookCommandListCreation(IUnknown *device)
{
	if (!device)
		return;

	ID3D12Device *baseDevice = nullptr;
	if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&baseDevice)))) {
		void **vtable = *reinterpret_cast<void***>(baseDevice);
		if (vtable) {
			DX12HookFunction(reinterpret_cast<void**>(&gOrigCreateCommandList),
				vtable[12], HookedCreateCommandList, "ID3D12Device::CreateCommandList");
		}
		baseDevice->Release();
	}

	ID3D12Device4 *device4 = nullptr;
	if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device4)))) {
		void **vtable4 = *reinterpret_cast<void***>(device4);
		if (vtable4) {
			DX12HookFunction(reinterpret_cast<void**>(&gOrigCreateCommandList1),
				vtable4[52], HookedCreateCommandList1, "ID3D12Device4::CreateCommandList1");
		}
		device4->Release();
	}
}
