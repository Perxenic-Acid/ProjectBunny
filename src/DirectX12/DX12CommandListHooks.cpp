#include "DX12CommandListHooks.h"

#include <d3d12.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "DX12BindingTracker.h"
#include "DX12FrameAnalysis.h"
#include "DX12HookManager.h"
#include "DX12ResourceTracker.h"
#include "DX12ShaderDump.h"
#include "DX12State.h"

typedef HRESULT(STDMETHODCALLTYPE *PFN_CREATE_COMMAND_QUEUE)(
	ID3D12Device*, const D3D12_COMMAND_QUEUE_DESC*, REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CREATE_COMMAND_ALLOCATOR)(
	ID3D12Device*, D3D12_COMMAND_LIST_TYPE, REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CREATE_COMMAND_LIST)(
	ID3D12Device*, UINT, D3D12_COMMAND_LIST_TYPE, ID3D12CommandAllocator*,
	ID3D12PipelineState*, REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CREATE_COMMAND_LIST1)(
	ID3D12Device4*, UINT, D3D12_COMMAND_LIST_TYPE, D3D12_COMMAND_LIST_FLAGS, REFIID, void**);

typedef void(STDMETHODCALLTYPE *PFN_QUEUE_UPDATE_TILE_MAPPINGS)(
	ID3D12CommandQueue*, ID3D12Resource*, UINT, const D3D12_TILED_RESOURCE_COORDINATE*,
	const D3D12_TILE_REGION_SIZE*, ID3D12Heap*, UINT, const D3D12_TILE_RANGE_FLAGS*,
	const UINT*, const UINT*, D3D12_TILE_MAPPING_FLAGS);
typedef void(STDMETHODCALLTYPE *PFN_QUEUE_COPY_TILE_MAPPINGS)(
	ID3D12CommandQueue*, ID3D12Resource*, const D3D12_TILED_RESOURCE_COORDINATE*,
	ID3D12Resource*, const D3D12_TILED_RESOURCE_COORDINATE*, const D3D12_TILE_REGION_SIZE*,
	D3D12_TILE_MAPPING_FLAGS);
typedef void(STDMETHODCALLTYPE *PFN_QUEUE_EXECUTE_COMMAND_LISTS)(
	ID3D12CommandQueue*, UINT, ID3D12CommandList *const*);
typedef void(STDMETHODCALLTYPE *PFN_QUEUE_SET_MARKER)(
	ID3D12CommandQueue*, UINT, const void*, UINT);
typedef void(STDMETHODCALLTYPE *PFN_QUEUE_BEGIN_EVENT)(
	ID3D12CommandQueue*, UINT, const void*, UINT);
typedef void(STDMETHODCALLTYPE *PFN_QUEUE_END_EVENT)(ID3D12CommandQueue*);
typedef HRESULT(STDMETHODCALLTYPE *PFN_QUEUE_SIGNAL)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);
typedef HRESULT(STDMETHODCALLTYPE *PFN_QUEUE_WAIT)(ID3D12CommandQueue*, ID3D12Fence*, UINT64);
typedef HRESULT(STDMETHODCALLTYPE *PFN_QUEUE_GET_TIMESTAMP_FREQUENCY)(ID3D12CommandQueue*, UINT64*);
typedef HRESULT(STDMETHODCALLTYPE *PFN_QUEUE_GET_CLOCK_CALIBRATION)(ID3D12CommandQueue*, UINT64*, UINT64*);

typedef HRESULT(STDMETHODCALLTYPE *PFN_CLOSE_COMMAND_LIST)(ID3D12GraphicsCommandList*);
typedef HRESULT(STDMETHODCALLTYPE *PFN_RESET_COMMAND_LIST)(
	ID3D12GraphicsCommandList*, ID3D12CommandAllocator*, ID3D12PipelineState*);
typedef void(STDMETHODCALLTYPE *PFN_CLEAR_STATE)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
typedef void(STDMETHODCALLTYPE *PFN_DRAW_INSTANCED)(
	ID3D12GraphicsCommandList*, UINT, UINT, UINT, UINT);
typedef void(STDMETHODCALLTYPE *PFN_DRAW_INDEXED_INSTANCED)(
	ID3D12GraphicsCommandList*, UINT, UINT, UINT, INT, UINT);
typedef void(STDMETHODCALLTYPE *PFN_DISPATCH)(ID3D12GraphicsCommandList*, UINT, UINT, UINT);
typedef void(STDMETHODCALLTYPE *PFN_COPY_BUFFER_REGION)(
	ID3D12GraphicsCommandList*, ID3D12Resource*, UINT64, ID3D12Resource*, UINT64, UINT64);
typedef void(STDMETHODCALLTYPE *PFN_COPY_TEXTURE_REGION)(
	ID3D12GraphicsCommandList*, const D3D12_TEXTURE_COPY_LOCATION*, UINT, UINT, UINT,
	const D3D12_TEXTURE_COPY_LOCATION*, const D3D12_BOX*);
typedef void(STDMETHODCALLTYPE *PFN_COPY_RESOURCE)(
	ID3D12GraphicsCommandList*, ID3D12Resource*, ID3D12Resource*);
typedef void(STDMETHODCALLTYPE *PFN_COPY_TILES)(
	ID3D12GraphicsCommandList*, ID3D12Resource*, const D3D12_TILED_RESOURCE_COORDINATE*,
	const D3D12_TILE_REGION_SIZE*, ID3D12Resource*, UINT64, D3D12_TILE_COPY_FLAGS);
typedef void(STDMETHODCALLTYPE *PFN_RESOLVE_SUBRESOURCE)(
	ID3D12GraphicsCommandList*, ID3D12Resource*, UINT, ID3D12Resource*, UINT, DXGI_FORMAT);
typedef void(STDMETHODCALLTYPE *PFN_IA_SET_PRIMITIVE_TOPOLOGY)(
	ID3D12GraphicsCommandList*, D3D12_PRIMITIVE_TOPOLOGY);
typedef void(STDMETHODCALLTYPE *PFN_RS_SET_VIEWPORTS)(
	ID3D12GraphicsCommandList*, UINT, const D3D12_VIEWPORT*);
typedef void(STDMETHODCALLTYPE *PFN_RS_SET_SCISSOR_RECTS)(
	ID3D12GraphicsCommandList*, UINT, const D3D12_RECT*);
typedef void(STDMETHODCALLTYPE *PFN_OM_SET_BLEND_FACTOR)(ID3D12GraphicsCommandList*, const FLOAT[4]);
typedef void(STDMETHODCALLTYPE *PFN_OM_SET_STENCIL_REF)(ID3D12GraphicsCommandList*, UINT);
typedef void(STDMETHODCALLTYPE *PFN_SET_PIPELINE_STATE)(ID3D12GraphicsCommandList*, ID3D12PipelineState*);
typedef void(STDMETHODCALLTYPE *PFN_RESOURCE_BARRIER)(
	ID3D12GraphicsCommandList*, UINT, const D3D12_RESOURCE_BARRIER*);
typedef void(STDMETHODCALLTYPE *PFN_EXECUTE_BUNDLE)(ID3D12GraphicsCommandList*, ID3D12GraphicsCommandList*);
typedef void(STDMETHODCALLTYPE *PFN_SET_DESCRIPTOR_HEAPS)(
	ID3D12GraphicsCommandList*, UINT, ID3D12DescriptorHeap *const*);
typedef void(STDMETHODCALLTYPE *PFN_SET_ROOT_SIGNATURE)(
	ID3D12GraphicsCommandList*, ID3D12RootSignature*);
typedef void(STDMETHODCALLTYPE *PFN_SET_ROOT_DESCRIPTOR_TABLE)(
	ID3D12GraphicsCommandList*, UINT, D3D12_GPU_DESCRIPTOR_HANDLE);
typedef void(STDMETHODCALLTYPE *PFN_SET_ROOT_32BIT_CONSTANT)(
	ID3D12GraphicsCommandList*, UINT, UINT, UINT);
typedef void(STDMETHODCALLTYPE *PFN_SET_ROOT_32BIT_CONSTANTS)(
	ID3D12GraphicsCommandList*, UINT, UINT, const void*, UINT);
typedef void(STDMETHODCALLTYPE *PFN_SET_ROOT_GPU_VA)(
	ID3D12GraphicsCommandList*, UINT, D3D12_GPU_VIRTUAL_ADDRESS);
typedef void(STDMETHODCALLTYPE *PFN_IA_SET_INDEX_BUFFER)(
	ID3D12GraphicsCommandList*, const D3D12_INDEX_BUFFER_VIEW*);
typedef void(STDMETHODCALLTYPE *PFN_IA_SET_VERTEX_BUFFERS)(
	ID3D12GraphicsCommandList*, UINT, UINT, const D3D12_VERTEX_BUFFER_VIEW*);
typedef void(STDMETHODCALLTYPE *PFN_SO_SET_TARGETS)(
	ID3D12GraphicsCommandList*, UINT, UINT, const D3D12_STREAM_OUTPUT_BUFFER_VIEW*);
typedef void(STDMETHODCALLTYPE *PFN_OM_SET_RENDER_TARGETS)(
	ID3D12GraphicsCommandList*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, BOOL,
	const D3D12_CPU_DESCRIPTOR_HANDLE*);
typedef void(STDMETHODCALLTYPE *PFN_CLEAR_DEPTH_STENCIL_VIEW)(
	ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CLEAR_FLAGS, FLOAT, UINT8,
	UINT, const D3D12_RECT*);
typedef void(STDMETHODCALLTYPE *PFN_CLEAR_RENDER_TARGET_VIEW)(
	ID3D12GraphicsCommandList*, D3D12_CPU_DESCRIPTOR_HANDLE, const FLOAT[4], UINT, const D3D12_RECT*);
typedef void(STDMETHODCALLTYPE *PFN_CLEAR_UAV_UINT)(
	ID3D12GraphicsCommandList*, D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE,
	ID3D12Resource*, const UINT[4], UINT, const D3D12_RECT*);
typedef void(STDMETHODCALLTYPE *PFN_CLEAR_UAV_FLOAT)(
	ID3D12GraphicsCommandList*, D3D12_GPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE,
	ID3D12Resource*, const FLOAT[4], UINT, const D3D12_RECT*);
typedef void(STDMETHODCALLTYPE *PFN_DISCARD_RESOURCE)(
	ID3D12GraphicsCommandList*, ID3D12Resource*, const D3D12_DISCARD_REGION*);
typedef void(STDMETHODCALLTYPE *PFN_BEGIN_QUERY)(
	ID3D12GraphicsCommandList*, ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT);
typedef void(STDMETHODCALLTYPE *PFN_END_QUERY)(
	ID3D12GraphicsCommandList*, ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT);
typedef void(STDMETHODCALLTYPE *PFN_RESOLVE_QUERY_DATA)(
	ID3D12GraphicsCommandList*, ID3D12QueryHeap*, D3D12_QUERY_TYPE, UINT, UINT,
	ID3D12Resource*, UINT64);
typedef void(STDMETHODCALLTYPE *PFN_SET_PREDICATION)(
	ID3D12GraphicsCommandList*, ID3D12Resource*, UINT64, D3D12_PREDICATION_OP);
typedef void(STDMETHODCALLTYPE *PFN_COMMAND_LIST_SET_MARKER)(
	ID3D12GraphicsCommandList*, UINT, const void*, UINT);
typedef void(STDMETHODCALLTYPE *PFN_COMMAND_LIST_BEGIN_EVENT)(
	ID3D12GraphicsCommandList*, UINT, const void*, UINT);
typedef void(STDMETHODCALLTYPE *PFN_COMMAND_LIST_END_EVENT)(ID3D12GraphicsCommandList*);
typedef void(STDMETHODCALLTYPE *PFN_EXECUTE_INDIRECT)(
	ID3D12GraphicsCommandList*, ID3D12CommandSignature*, UINT, ID3D12Resource*, UINT64,
	ID3D12Resource*, UINT64);

static PFN_CREATE_COMMAND_QUEUE gOrigCreateCommandQueue = nullptr;
static PFN_CREATE_COMMAND_ALLOCATOR gOrigCreateCommandAllocator = nullptr;
static PFN_CREATE_COMMAND_LIST gOrigCreateCommandList = nullptr;
static PFN_CREATE_COMMAND_LIST1 gOrigCreateCommandList1 = nullptr;

static PFN_QUEUE_UPDATE_TILE_MAPPINGS gOrigQueueUpdateTileMappings = nullptr;
static PFN_QUEUE_COPY_TILE_MAPPINGS gOrigQueueCopyTileMappings = nullptr;
static PFN_QUEUE_EXECUTE_COMMAND_LISTS gOrigQueueExecuteCommandLists = nullptr;
static PFN_QUEUE_SET_MARKER gOrigQueueSetMarker = nullptr;
static PFN_QUEUE_BEGIN_EVENT gOrigQueueBeginEvent = nullptr;
static PFN_QUEUE_END_EVENT gOrigQueueEndEvent = nullptr;
static PFN_QUEUE_SIGNAL gOrigQueueSignal = nullptr;
static PFN_QUEUE_WAIT gOrigQueueWait = nullptr;
static PFN_QUEUE_GET_TIMESTAMP_FREQUENCY gOrigQueueGetTimestampFrequency = nullptr;
static PFN_QUEUE_GET_CLOCK_CALIBRATION gOrigQueueGetClockCalibration = nullptr;

static PFN_CLOSE_COMMAND_LIST gOrigCloseCommandList = nullptr;
static PFN_RESET_COMMAND_LIST gOrigResetCommandList = nullptr;
static PFN_CLEAR_STATE gOrigClearState = nullptr;
static PFN_DRAW_INSTANCED gOrigDrawInstanced = nullptr;
static PFN_DRAW_INDEXED_INSTANCED gOrigDrawIndexedInstanced = nullptr;
static PFN_DISPATCH gOrigDispatch = nullptr;
static PFN_COPY_BUFFER_REGION gOrigCopyBufferRegion = nullptr;
static PFN_COPY_TEXTURE_REGION gOrigCopyTextureRegion = nullptr;
static PFN_COPY_RESOURCE gOrigCopyResource = nullptr;
static PFN_COPY_TILES gOrigCopyTiles = nullptr;
static PFN_RESOLVE_SUBRESOURCE gOrigResolveSubresource = nullptr;
static PFN_IA_SET_PRIMITIVE_TOPOLOGY gOrigIASetPrimitiveTopology = nullptr;
static PFN_RS_SET_VIEWPORTS gOrigRSSetViewports = nullptr;
static PFN_RS_SET_SCISSOR_RECTS gOrigRSSetScissorRects = nullptr;
static PFN_OM_SET_BLEND_FACTOR gOrigOMSetBlendFactor = nullptr;
static PFN_OM_SET_STENCIL_REF gOrigOMSetStencilRef = nullptr;
static PFN_SET_PIPELINE_STATE gOrigSetPipelineState = nullptr;
static PFN_RESOURCE_BARRIER gOrigResourceBarrier = nullptr;
static PFN_EXECUTE_BUNDLE gOrigExecuteBundle = nullptr;
static PFN_SET_DESCRIPTOR_HEAPS gOrigSetDescriptorHeaps = nullptr;
static PFN_SET_ROOT_SIGNATURE gOrigSetComputeRootSignature = nullptr;
static PFN_SET_ROOT_SIGNATURE gOrigSetGraphicsRootSignature = nullptr;
static PFN_SET_ROOT_DESCRIPTOR_TABLE gOrigSetComputeRootDescriptorTable = nullptr;
static PFN_SET_ROOT_DESCRIPTOR_TABLE gOrigSetGraphicsRootDescriptorTable = nullptr;
static PFN_SET_ROOT_32BIT_CONSTANT gOrigSetComputeRoot32BitConstant = nullptr;
static PFN_SET_ROOT_32BIT_CONSTANT gOrigSetGraphicsRoot32BitConstant = nullptr;
static PFN_SET_ROOT_32BIT_CONSTANTS gOrigSetComputeRoot32BitConstants = nullptr;
static PFN_SET_ROOT_32BIT_CONSTANTS gOrigSetGraphicsRoot32BitConstants = nullptr;
static PFN_SET_ROOT_GPU_VA gOrigSetComputeRootConstantBufferView = nullptr;
static PFN_SET_ROOT_GPU_VA gOrigSetGraphicsRootConstantBufferView = nullptr;
static PFN_SET_ROOT_GPU_VA gOrigSetComputeRootShaderResourceView = nullptr;
static PFN_SET_ROOT_GPU_VA gOrigSetGraphicsRootShaderResourceView = nullptr;
static PFN_SET_ROOT_GPU_VA gOrigSetComputeRootUnorderedAccessView = nullptr;
static PFN_SET_ROOT_GPU_VA gOrigSetGraphicsRootUnorderedAccessView = nullptr;
static PFN_IA_SET_INDEX_BUFFER gOrigIASetIndexBuffer = nullptr;
static PFN_IA_SET_VERTEX_BUFFERS gOrigIASetVertexBuffers = nullptr;
static PFN_SO_SET_TARGETS gOrigSOSetTargets = nullptr;
static PFN_OM_SET_RENDER_TARGETS gOrigOMSetRenderTargets = nullptr;
static PFN_CLEAR_DEPTH_STENCIL_VIEW gOrigClearDepthStencilView = nullptr;
static PFN_CLEAR_RENDER_TARGET_VIEW gOrigClearRenderTargetView = nullptr;
static PFN_CLEAR_UAV_UINT gOrigClearUnorderedAccessViewUint = nullptr;
static PFN_CLEAR_UAV_FLOAT gOrigClearUnorderedAccessViewFloat = nullptr;
static PFN_DISCARD_RESOURCE gOrigDiscardResource = nullptr;
static PFN_BEGIN_QUERY gOrigBeginQuery = nullptr;
static PFN_END_QUERY gOrigEndQuery = nullptr;
static PFN_RESOLVE_QUERY_DATA gOrigResolveQueryData = nullptr;
static PFN_SET_PREDICATION gOrigSetPredication = nullptr;
static PFN_COMMAND_LIST_SET_MARKER gOrigCommandListSetMarker = nullptr;
static PFN_COMMAND_LIST_BEGIN_EVENT gOrigCommandListBeginEvent = nullptr;
static PFN_COMMAND_LIST_END_EVENT gOrigCommandListEndEvent = nullptr;
static PFN_EXECUTE_INDIRECT gOrigExecuteIndirect = nullptr;

static void LogDX12Call(const char *name, const void *object, const char *fmt = nullptr, ...)
{
	if (!DX12FrameAnalysisIsCapturing())
		return;
	if (!name)
		return;

	const char *shortName = strrchr(name, ':');
	shortName = shortName ? shortName + 1 : name;
	if (shortName[0] == ':')
		shortName++;

	if (strcmp(shortName, "SetPipelineState") &&
		strcmp(shortName, "SetGraphicsRootSignature") &&
		strcmp(shortName, "SetComputeRootSignature") &&
		strcmp(shortName, "SetDescriptorHeaps") &&
		strcmp(shortName, "SetGraphicsRootDescriptorTable") &&
		strcmp(shortName, "SetComputeRootDescriptorTable") &&
		strcmp(shortName, "SetGraphicsRootConstantBufferView") &&
		strcmp(shortName, "SetComputeRootConstantBufferView") &&
		strcmp(shortName, "SetGraphicsRootShaderResourceView") &&
		strcmp(shortName, "SetComputeRootShaderResourceView") &&
		strcmp(shortName, "SetGraphicsRootUnorderedAccessView") &&
		strcmp(shortName, "SetComputeRootUnorderedAccessView") &&
		strcmp(shortName, "IASetPrimitiveTopology") &&
		strcmp(shortName, "IASetIndexBuffer") &&
		strcmp(shortName, "IASetVertexBuffers") &&
		strcmp(shortName, "DrawInstanced") &&
		strcmp(shortName, "DrawIndexedInstanced") &&
		strcmp(shortName, "Dispatch") &&
		strcmp(shortName, "ExecuteIndirect"))
		return;

	char extra[512] = {};
	if (fmt) {
		va_list args;
		va_start(args, fmt);
		vsnprintf(extra, sizeof(extra), fmt, args);
		va_end(args);
	}

	DX12FrameAnalysisLogEvent("%s list=%p%s\n", shortName, object, extra);
}

static bool ShouldTrackBindings()
{
	return DX12FrameAnalysisIsCapturing() || DX12ShaderDumpIsCapturingFrame();
}

static void FormatVertexBufferViews(
	char *text, size_t textCount, UINT startSlot, UINT count,
	const D3D12_VERTEX_BUFFER_VIEW *views)
{
	if (!text || textCount == 0)
		return;
	text[0] = '\0';
	if (!views || count == 0)
		return;

	const UINT maxLogged = count > 8 ? 8 : count;
	size_t used = 0;
	for (UINT i = 0; i < maxLogged && used < textCount; ++i) {
		int written = sprintf_s(text + used, textCount - used,
			"%s%u:gpu=0x%llx,size=%u,stride=%u",
			i ? ";" : "",
			startSlot + i,
			static_cast<unsigned long long>(views[i].BufferLocation),
			views[i].SizeInBytes,
			views[i].StrideInBytes);
		if (written <= 0)
			break;
		used += static_cast<size_t>(written);
	}
	if (count > maxLogged && used < textCount)
		sprintf_s(text + used, textCount - used, ";...");
}

struct ThreadLocalCommandListOriginal
{
	void *target = nullptr;
	void *original = nullptr;
};

static thread_local ThreadLocalCommandListOriginal tCommandListOriginals[64];

template <typename T>
static T GetCommandListOriginal(
	ID3D12GraphicsCommandList *commandList, UINT slot, T fallback, const char *name)
{
	if (commandList) {
		void **vtable = *reinterpret_cast<void***>(commandList);
		if (vtable) {
			void *target = vtable[slot];
			if (slot < ARRAYSIZE(tCommandListOriginals) &&
				tCommandListOriginals[slot].target == target &&
				tCommandListOriginals[slot].original)
				return reinterpret_cast<T>(tCommandListOriginals[slot].original);

			void *original = DX12GetOriginalFunction(target);
			if (slot < ARRAYSIZE(tCommandListOriginals) && original) {
				tCommandListOriginals[slot].target = target;
				tCommandListOriginals[slot].original = original;
			}
			if (original)
				return reinterpret_cast<T>(original);
		}
	}

	if (fallback)
		return fallback;

	DX12Log("DX12CallMissingOriginal %s this=%p slot=%u\n",
		name ? name : "unknown", commandList, slot);
	return nullptr;
}

#define DX12_CL_ORIG(commandList, slot, type, name) \
	GetCommandListOriginal<type>(commandList, slot, gOrig##name, "ID3D12GraphicsCommandList::" #name)

static void RegisterCommandList(IUnknown *commandList, ID3D12PipelineState *initialState)
{
	if (!commandList)
		return;

	ID3D12GraphicsCommandList *baseList = nullptr;
	if (SUCCEEDED(commandList->QueryInterface(IID_PPV_ARGS(&baseList)))) {
		DX12HookCommandList(baseList);
		if (ShouldTrackBindings()) {
			DX12BindingRegisterCommandList(baseList);
			DX12BindingResetCommandList(baseList, initialState);
		}
		baseList->Release();
	}
}

static void RegisterCommandQueue(IUnknown *commandQueue)
{
	DX12HookCommandQueue(commandQueue);
}

static HRESULT STDMETHODCALLTYPE HookedCreateCommandQueue(
	ID3D12Device *device, const D3D12_COMMAND_QUEUE_DESC *desc, REFIID riid, void **commandQueue)
{
	LogDX12Call("ID3D12Device::CreateCommandQueue", device, " type=%d flags=0x%x",
		desc ? static_cast<int>(desc->Type) : -1,
		desc ? static_cast<UINT>(desc->Flags) : 0);
	HRESULT hr = gOrigCreateCommandQueue(device, desc, riid, commandQueue);
	DX12FrameAnalysisLogInfo("DX12CallResult ID3D12Device::CreateCommandQueue hr=0x%lx queue=%p\n",
		hr, commandQueue ? *commandQueue : nullptr);
	if (SUCCEEDED(hr) && commandQueue && *commandQueue)
		RegisterCommandQueue(static_cast<IUnknown*>(*commandQueue));
	return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreateCommandAllocator(
	ID3D12Device *device, D3D12_COMMAND_LIST_TYPE type, REFIID riid, void **allocator)
{
	LogDX12Call("ID3D12Device::CreateCommandAllocator", device, " type=%d",
		static_cast<int>(type));
	HRESULT hr = gOrigCreateCommandAllocator(device, type, riid, allocator);
	DX12FrameAnalysisLogInfo("DX12CallResult ID3D12Device::CreateCommandAllocator hr=0x%lx allocator=%p\n",
		hr, allocator ? *allocator : nullptr);
	return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreateCommandList(
	ID3D12Device *device, UINT nodeMask, D3D12_COMMAND_LIST_TYPE type,
	ID3D12CommandAllocator *allocator, ID3D12PipelineState *initialState,
	REFIID riid, void **commandList)
{
	LogDX12Call("ID3D12Device::CreateCommandList", device,
		" nodeMask=%u type=%d allocator=%p initialPso=%p",
		nodeMask, static_cast<int>(type), allocator, initialState);
	HRESULT hr = gOrigCreateCommandList(device, nodeMask, type, allocator, initialState, riid, commandList);
	DX12FrameAnalysisLogInfo("DX12CallResult ID3D12Device::CreateCommandList hr=0x%lx commandList=%p\n",
		hr, commandList ? *commandList : nullptr);
	if (SUCCEEDED(hr) && commandList && *commandList)
		RegisterCommandList(static_cast<IUnknown*>(*commandList), initialState);
	return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreateCommandList1(
	ID3D12Device4 *device, UINT nodeMask, D3D12_COMMAND_LIST_TYPE type,
	D3D12_COMMAND_LIST_FLAGS flags, REFIID riid, void **commandList)
{
	LogDX12Call("ID3D12Device4::CreateCommandList1", device,
		" nodeMask=%u type=%d flags=0x%x",
		nodeMask, static_cast<int>(type), static_cast<UINT>(flags));
	HRESULT hr = gOrigCreateCommandList1(device, nodeMask, type, flags, riid, commandList);
	DX12FrameAnalysisLogInfo("DX12CallResult ID3D12Device4::CreateCommandList1 hr=0x%lx commandList=%p\n",
		hr, commandList ? *commandList : nullptr);
	if (SUCCEEDED(hr) && commandList && *commandList)
		RegisterCommandList(static_cast<IUnknown*>(*commandList), nullptr);
	return hr;
}

static void STDMETHODCALLTYPE HookedQueueUpdateTileMappings(
	ID3D12CommandQueue *queue, ID3D12Resource *resource, UINT numResourceRegions,
	const D3D12_TILED_RESOURCE_COORDINATE *resourceRegionStartCoordinates,
	const D3D12_TILE_REGION_SIZE *resourceRegionSizes, ID3D12Heap *heap, UINT numRanges,
	const D3D12_TILE_RANGE_FLAGS *rangeFlags, const UINT *heapRangeStartOffsets,
	const UINT *rangeTileCounts, D3D12_TILE_MAPPING_FLAGS flags)
{
	LogDX12Call("ID3D12CommandQueue::UpdateTileMappings", queue,
		" resource=%p regions=%u heap=%p ranges=%u flags=0x%x",
		resource, numResourceRegions, heap, numRanges, static_cast<UINT>(flags));
	gOrigQueueUpdateTileMappings(queue, resource, numResourceRegions,
		resourceRegionStartCoordinates, resourceRegionSizes, heap, numRanges,
		rangeFlags, heapRangeStartOffsets, rangeTileCounts, flags);
}

static void STDMETHODCALLTYPE HookedQueueCopyTileMappings(
	ID3D12CommandQueue *queue, ID3D12Resource *dstResource,
	const D3D12_TILED_RESOURCE_COORDINATE *dstRegionStartCoordinate,
	ID3D12Resource *srcResource, const D3D12_TILED_RESOURCE_COORDINATE *srcRegionStartCoordinate,
	const D3D12_TILE_REGION_SIZE *regionSize, D3D12_TILE_MAPPING_FLAGS flags)
{
	LogDX12Call("ID3D12CommandQueue::CopyTileMappings", queue,
		" dst=%p src=%p flags=0x%x", dstResource, srcResource, static_cast<UINT>(flags));
	gOrigQueueCopyTileMappings(queue, dstResource, dstRegionStartCoordinate,
		srcResource, srcRegionStartCoordinate, regionSize, flags);
}

static void STDMETHODCALLTYPE HookedQueueExecuteCommandLists(
	ID3D12CommandQueue *queue, UINT numCommandLists, ID3D12CommandList *const *commandLists)
{
	LogDX12Call("ID3D12CommandQueue::ExecuteCommandLists", queue, " count=%u", numCommandLists);
	for (UINT i = 0; commandLists && i < numCommandLists; ++i)
		RegisterCommandList(commandLists[i], nullptr);
	gOrigQueueExecuteCommandLists(queue, numCommandLists, commandLists);
}

static void STDMETHODCALLTYPE HookedQueueSetMarker(
	ID3D12CommandQueue *queue, UINT metadata, const void *data, UINT size)
{
	LogDX12Call("ID3D12CommandQueue::SetMarker", queue, " metadata=%u size=%u", metadata, size);
	gOrigQueueSetMarker(queue, metadata, data, size);
}

static void STDMETHODCALLTYPE HookedQueueBeginEvent(
	ID3D12CommandQueue *queue, UINT metadata, const void *data, UINT size)
{
	LogDX12Call("ID3D12CommandQueue::BeginEvent", queue, " metadata=%u size=%u", metadata, size);
	gOrigQueueBeginEvent(queue, metadata, data, size);
}

static void STDMETHODCALLTYPE HookedQueueEndEvent(ID3D12CommandQueue *queue)
{
	LogDX12Call("ID3D12CommandQueue::EndEvent", queue);
	gOrigQueueEndEvent(queue);
}

static HRESULT STDMETHODCALLTYPE HookedQueueSignal(
	ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 value)
{
	LogDX12Call("ID3D12CommandQueue::Signal", queue, " fence=%p value=%llu",
		fence, static_cast<unsigned long long>(value));
	return gOrigQueueSignal(queue, fence, value);
}

static HRESULT STDMETHODCALLTYPE HookedQueueWait(
	ID3D12CommandQueue *queue, ID3D12Fence *fence, UINT64 value)
{
	LogDX12Call("ID3D12CommandQueue::Wait", queue, " fence=%p value=%llu",
		fence, static_cast<unsigned long long>(value));
	return gOrigQueueWait(queue, fence, value);
}

static HRESULT STDMETHODCALLTYPE HookedQueueGetTimestampFrequency(
	ID3D12CommandQueue *queue, UINT64 *frequency)
{
	LogDX12Call("ID3D12CommandQueue::GetTimestampFrequency", queue);
	return gOrigQueueGetTimestampFrequency(queue, frequency);
}

static HRESULT STDMETHODCALLTYPE HookedQueueGetClockCalibration(
	ID3D12CommandQueue *queue, UINT64 *gpuTimestamp, UINT64 *cpuTimestamp)
{
	LogDX12Call("ID3D12CommandQueue::GetClockCalibration", queue);
	return gOrigQueueGetClockCalibration(queue, gpuTimestamp, cpuTimestamp);
}

static HRESULT STDMETHODCALLTYPE HookedCloseCommandList(ID3D12GraphicsCommandList *commandList)
{
	LogDX12Call("ID3D12GraphicsCommandList::Close", commandList);
	PFN_CLOSE_COMMAND_LIST original = DX12_CL_ORIG(commandList, 9, PFN_CLOSE_COMMAND_LIST, CloseCommandList);
	return original ? original(commandList) : E_FAIL;
}

static HRESULT STDMETHODCALLTYPE HookedResetCommandList(
	ID3D12GraphicsCommandList *commandList, ID3D12CommandAllocator *allocator,
	ID3D12PipelineState *initialState)
{
	LogDX12Call("ID3D12GraphicsCommandList::Reset", commandList,
		" allocator=%p initialPso=%p", allocator, initialState);
	if (ShouldTrackBindings())
		DX12BindingResetCommandList(commandList, initialState);
	PFN_RESET_COMMAND_LIST original = DX12_CL_ORIG(commandList, 10, PFN_RESET_COMMAND_LIST, ResetCommandList);
	return original ? original(commandList, allocator, initialState) : E_FAIL;
}

static void STDMETHODCALLTYPE HookedClearState(
	ID3D12GraphicsCommandList *commandList, ID3D12PipelineState *pipelineState)
{
	LogDX12Call("ID3D12GraphicsCommandList::ClearState", commandList, " pso=%p", pipelineState);
	if (ShouldTrackBindings())
		DX12BindingRecordStateEvent(commandList, "clear_state");
	PFN_CLEAR_STATE original = DX12_CL_ORIG(commandList, 11, PFN_CLEAR_STATE, ClearState);
	if (original)
		original(commandList, pipelineState);
}

static void STDMETHODCALLTYPE HookedDrawInstanced(
	ID3D12GraphicsCommandList *commandList, UINT vertexCountPerInstance, UINT instanceCount,
	UINT startVertexLocation, UINT startInstanceLocation)
{
	LogDX12Call("ID3D12GraphicsCommandList::DrawInstanced", commandList,
		" vertices=%u instances=%u startVertex=%u startInstance=%u",
		vertexCountPerInstance, instanceCount, startVertexLocation, startInstanceLocation);
	if (ShouldTrackBindings())
		DX12BindingRecordDrawInstanced(commandList, vertexCountPerInstance, instanceCount,
			startVertexLocation, startInstanceLocation);
	PFN_DRAW_INSTANCED original = DX12_CL_ORIG(commandList, 12, PFN_DRAW_INSTANCED, DrawInstanced);
	if (original)
		original(commandList, vertexCountPerInstance, instanceCount,
			startVertexLocation, startInstanceLocation);
}

static void STDMETHODCALLTYPE HookedDrawIndexedInstanced(
	ID3D12GraphicsCommandList *commandList, UINT indexCountPerInstance, UINT instanceCount,
	UINT startIndexLocation, INT baseVertexLocation, UINT startInstanceLocation)
{
	LogDX12Call("ID3D12GraphicsCommandList::DrawIndexedInstanced", commandList,
		" indices=%u instances=%u startIndex=%u baseVertex=%d startInstance=%u",
		indexCountPerInstance, instanceCount, startIndexLocation, baseVertexLocation,
		startInstanceLocation);
	if (ShouldTrackBindings())
		DX12BindingRecordDrawIndexedInstanced(commandList, indexCountPerInstance, instanceCount,
			startIndexLocation, baseVertexLocation, startInstanceLocation);
	PFN_DRAW_INDEXED_INSTANCED original =
		DX12_CL_ORIG(commandList, 13, PFN_DRAW_INDEXED_INSTANCED, DrawIndexedInstanced);
	if (original)
		original(commandList, indexCountPerInstance, instanceCount,
			startIndexLocation, baseVertexLocation, startInstanceLocation);
}

static void STDMETHODCALLTYPE HookedDispatch(
	ID3D12GraphicsCommandList *commandList, UINT threadGroupCountX,
	UINT threadGroupCountY, UINT threadGroupCountZ)
{
	LogDX12Call("ID3D12GraphicsCommandList::Dispatch", commandList,
		" groups=%u,%u,%u", threadGroupCountX, threadGroupCountY, threadGroupCountZ);
	if (ShouldTrackBindings())
		DX12BindingRecordDispatch(commandList, threadGroupCountX, threadGroupCountY, threadGroupCountZ);
	PFN_DISPATCH original = DX12_CL_ORIG(commandList, 14, PFN_DISPATCH, Dispatch);
	if (original)
		original(commandList, threadGroupCountX, threadGroupCountY, threadGroupCountZ);
}

static void STDMETHODCALLTYPE HookedCopyBufferRegion(
	ID3D12GraphicsCommandList *commandList, ID3D12Resource *dstBuffer, UINT64 dstOffset,
	ID3D12Resource *srcBuffer, UINT64 srcOffset, UINT64 numBytes)
{
	LogDX12Call("ID3D12GraphicsCommandList::CopyBufferRegion", commandList,
		" dst=%p dstOffset=%llu src=%p srcOffset=%llu bytes=%llu",
		dstBuffer, static_cast<unsigned long long>(dstOffset), srcBuffer,
		static_cast<unsigned long long>(srcOffset), static_cast<unsigned long long>(numBytes));
	gOrigCopyBufferRegion(commandList, dstBuffer, dstOffset, srcBuffer, srcOffset, numBytes);
}

static void STDMETHODCALLTYPE HookedCopyTextureRegion(
	ID3D12GraphicsCommandList *commandList, const D3D12_TEXTURE_COPY_LOCATION *dst,
	UINT dstX, UINT dstY, UINT dstZ, const D3D12_TEXTURE_COPY_LOCATION *src, const D3D12_BOX *srcBox)
{
	LogDX12Call("ID3D12GraphicsCommandList::CopyTextureRegion", commandList,
		" dst=%p dstXYZ=%u,%u,%u src=%p srcBox=%p", dst, dstX, dstY, dstZ, src, srcBox);
	gOrigCopyTextureRegion(commandList, dst, dstX, dstY, dstZ, src, srcBox);
}

static void STDMETHODCALLTYPE HookedCopyResource(
	ID3D12GraphicsCommandList *commandList, ID3D12Resource *dstResource, ID3D12Resource *srcResource)
{
	LogDX12Call("ID3D12GraphicsCommandList::CopyResource", commandList,
		" dst=%p src=%p", dstResource, srcResource);
	gOrigCopyResource(commandList, dstResource, srcResource);
}

static void STDMETHODCALLTYPE HookedCopyTiles(
	ID3D12GraphicsCommandList *commandList, ID3D12Resource *tiledResource,
	const D3D12_TILED_RESOURCE_COORDINATE *tileRegionStartCoordinate,
	const D3D12_TILE_REGION_SIZE *tileRegionSize, ID3D12Resource *buffer,
	UINT64 bufferStartOffsetInBytes, D3D12_TILE_COPY_FLAGS flags)
{
	LogDX12Call("ID3D12GraphicsCommandList::CopyTiles", commandList,
		" tiled=%p buffer=%p offset=%llu flags=0x%x", tiledResource, buffer,
		static_cast<unsigned long long>(bufferStartOffsetInBytes), static_cast<UINT>(flags));
	gOrigCopyTiles(commandList, tiledResource, tileRegionStartCoordinate,
		tileRegionSize, buffer, bufferStartOffsetInBytes, flags);
}

static void STDMETHODCALLTYPE HookedResolveSubresource(
	ID3D12GraphicsCommandList *commandList, ID3D12Resource *dstResource, UINT dstSubresource,
	ID3D12Resource *srcResource, UINT srcSubresource, DXGI_FORMAT format)
{
	LogDX12Call("ID3D12GraphicsCommandList::ResolveSubresource", commandList,
		" dst=%p dstSub=%u src=%p srcSub=%u format=%u",
		dstResource, dstSubresource, srcResource, srcSubresource, static_cast<UINT>(format));
	gOrigResolveSubresource(commandList, dstResource, dstSubresource, srcResource, srcSubresource, format);
}

static void STDMETHODCALLTYPE HookedIASetPrimitiveTopology(
	ID3D12GraphicsCommandList *commandList, D3D12_PRIMITIVE_TOPOLOGY topology)
{
	LogDX12Call("ID3D12GraphicsCommandList::IASetPrimitiveTopology", commandList,
		" topology=%u", static_cast<UINT>(topology));
	if (ShouldTrackBindings())
		DX12BindingSetPrimitiveTopology(commandList, topology);
	PFN_IA_SET_PRIMITIVE_TOPOLOGY original =
		DX12_CL_ORIG(commandList, 20, PFN_IA_SET_PRIMITIVE_TOPOLOGY, IASetPrimitiveTopology);
	if (original)
		original(commandList, topology);
}

static void STDMETHODCALLTYPE HookedRSSetViewports(
	ID3D12GraphicsCommandList *commandList, UINT count, const D3D12_VIEWPORT *viewports)
{
	LogDX12Call("ID3D12GraphicsCommandList::RSSetViewports", commandList, " count=%u", count);
	gOrigRSSetViewports(commandList, count, viewports);
}

static void STDMETHODCALLTYPE HookedRSSetScissorRects(
	ID3D12GraphicsCommandList *commandList, UINT count, const D3D12_RECT *rects)
{
	LogDX12Call("ID3D12GraphicsCommandList::RSSetScissorRects", commandList, " count=%u", count);
	gOrigRSSetScissorRects(commandList, count, rects);
}

static void STDMETHODCALLTYPE HookedOMSetBlendFactor(
	ID3D12GraphicsCommandList *commandList, const FLOAT blendFactor[4])
{
	LogDX12Call("ID3D12GraphicsCommandList::OMSetBlendFactor", commandList);
	gOrigOMSetBlendFactor(commandList, blendFactor);
}

static void STDMETHODCALLTYPE HookedOMSetStencilRef(ID3D12GraphicsCommandList *commandList, UINT stencilRef)
{
	LogDX12Call("ID3D12GraphicsCommandList::OMSetStencilRef", commandList, " stencilRef=%u", stencilRef);
	gOrigOMSetStencilRef(commandList, stencilRef);
}

static void STDMETHODCALLTYPE HookedSetPipelineState(
	ID3D12GraphicsCommandList *commandList, ID3D12PipelineState *pipelineState)
{
	LogDX12Call("ID3D12GraphicsCommandList::SetPipelineState", commandList, " pso=%p", pipelineState);
	if (ShouldTrackBindings()) {
		DX12BindingSetPipelineState(commandList, pipelineState);
		DX12BindingRecordStateEvent(commandList, "set_pso");
	}
	PFN_SET_PIPELINE_STATE original = DX12_CL_ORIG(commandList, 25, PFN_SET_PIPELINE_STATE, SetPipelineState);
	if (original)
		original(commandList, pipelineState);
}

static void STDMETHODCALLTYPE HookedResourceBarrier(
	ID3D12GraphicsCommandList *commandList, UINT numBarriers,
	const D3D12_RESOURCE_BARRIER *barriers)
{
	LogDX12Call("ID3D12GraphicsCommandList::ResourceBarrier", commandList, " count=%u", numBarriers);
	if (ShouldTrackBindings())
		DX12RecordResourceBarrier(numBarriers, barriers);
	PFN_RESOURCE_BARRIER original = DX12_CL_ORIG(commandList, 26, PFN_RESOURCE_BARRIER, ResourceBarrier);
	if (original)
		original(commandList, numBarriers, barriers);
}

static void STDMETHODCALLTYPE HookedExecuteBundle(
	ID3D12GraphicsCommandList *commandList, ID3D12GraphicsCommandList *bundle)
{
	LogDX12Call("ID3D12GraphicsCommandList::ExecuteBundle", commandList, " bundle=%p", bundle);
	RegisterCommandList(bundle, nullptr);
	PFN_EXECUTE_BUNDLE original = DX12_CL_ORIG(commandList, 27, PFN_EXECUTE_BUNDLE, ExecuteBundle);
	if (original)
		original(commandList, bundle);
}

static void STDMETHODCALLTYPE HookedSetDescriptorHeaps(
	ID3D12GraphicsCommandList *commandList, UINT count,
	ID3D12DescriptorHeap *const *heaps)
{
	LogDX12Call("ID3D12GraphicsCommandList::SetDescriptorHeaps", commandList, " count=%u", count);
	if (ShouldTrackBindings()) {
		DX12BindingSetDescriptorHeaps(commandList, count, heaps);
		DX12BindingRecordStateEvent(commandList, "set_heaps");
	}
	PFN_SET_DESCRIPTOR_HEAPS original =
		DX12_CL_ORIG(commandList, 28, PFN_SET_DESCRIPTOR_HEAPS, SetDescriptorHeaps);
	if (original)
		original(commandList, count, heaps);
}

static void STDMETHODCALLTYPE HookedSetComputeRootSignature(
	ID3D12GraphicsCommandList *commandList, ID3D12RootSignature *rootSignature)
{
	LogDX12Call("ID3D12GraphicsCommandList::SetComputeRootSignature", commandList,
		" rootSignature=%p", rootSignature);
	if (ShouldTrackBindings()) {
		DX12BindingSetComputeRootSignature(commandList, rootSignature);
		DX12BindingRecordStateEvent(commandList, "set_compute_root_signature");
	}
	PFN_SET_ROOT_SIGNATURE original =
		DX12_CL_ORIG(commandList, 29, PFN_SET_ROOT_SIGNATURE, SetComputeRootSignature);
	if (original)
		original(commandList, rootSignature);
}

static void STDMETHODCALLTYPE HookedSetGraphicsRootSignature(
	ID3D12GraphicsCommandList *commandList, ID3D12RootSignature *rootSignature)
{
	LogDX12Call("ID3D12GraphicsCommandList::SetGraphicsRootSignature", commandList,
		" rootSignature=%p", rootSignature);
	if (ShouldTrackBindings()) {
		DX12BindingSetGraphicsRootSignature(commandList, rootSignature);
		DX12BindingRecordStateEvent(commandList, "set_graphics_root_signature");
	}
	PFN_SET_ROOT_SIGNATURE original =
		DX12_CL_ORIG(commandList, 30, PFN_SET_ROOT_SIGNATURE, SetGraphicsRootSignature);
	if (original)
		original(commandList, rootSignature);
}

static void STDMETHODCALLTYPE HookedSetComputeRootDescriptorTable(
	ID3D12GraphicsCommandList *commandList, UINT rootParameterIndex,
	D3D12_GPU_DESCRIPTOR_HANDLE baseDescriptor)
{
	LogDX12Call("ID3D12GraphicsCommandList::SetComputeRootDescriptorTable", commandList,
		" root=%u gpu=0x%llx", rootParameterIndex,
		static_cast<unsigned long long>(baseDescriptor.ptr));
	if (ShouldTrackBindings()) {
		DX12BindingSetComputeRootDescriptorTable(commandList, rootParameterIndex, baseDescriptor);
		DX12BindingRecordStateEvent(commandList, "set_compute_table");
	}
	PFN_SET_ROOT_DESCRIPTOR_TABLE original =
		DX12_CL_ORIG(commandList, 31, PFN_SET_ROOT_DESCRIPTOR_TABLE, SetComputeRootDescriptorTable);
	if (original)
		original(commandList, rootParameterIndex, baseDescriptor);
}

static void STDMETHODCALLTYPE HookedSetGraphicsRootDescriptorTable(
	ID3D12GraphicsCommandList *commandList, UINT rootParameterIndex,
	D3D12_GPU_DESCRIPTOR_HANDLE baseDescriptor)
{
	LogDX12Call("ID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable", commandList,
		" root=%u gpu=0x%llx", rootParameterIndex,
		static_cast<unsigned long long>(baseDescriptor.ptr));
	if (ShouldTrackBindings()) {
		DX12BindingSetGraphicsRootDescriptorTable(commandList, rootParameterIndex, baseDescriptor);
		DX12BindingRecordStateEvent(commandList, "set_graphics_table");
	}
	PFN_SET_ROOT_DESCRIPTOR_TABLE original =
		DX12_CL_ORIG(commandList, 32, PFN_SET_ROOT_DESCRIPTOR_TABLE, SetGraphicsRootDescriptorTable);
	if (original)
		original(commandList, rootParameterIndex, baseDescriptor);
}

static void STDMETHODCALLTYPE HookedSetComputeRoot32BitConstant(
	ID3D12GraphicsCommandList *commandList, UINT rootParameterIndex, UINT srcData, UINT destOffset)
{
	LogDX12Call("ID3D12GraphicsCommandList::SetComputeRoot32BitConstant", commandList,
		" root=%u destOffset=%u", rootParameterIndex, destOffset);
	PFN_SET_ROOT_32BIT_CONSTANT original =
		DX12_CL_ORIG(commandList, 33, PFN_SET_ROOT_32BIT_CONSTANT, SetComputeRoot32BitConstant);
	if (original)
		original(commandList, rootParameterIndex, srcData, destOffset);
}

static void STDMETHODCALLTYPE HookedSetGraphicsRoot32BitConstant(
	ID3D12GraphicsCommandList *commandList, UINT rootParameterIndex, UINT srcData, UINT destOffset)
{
	LogDX12Call("ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstant", commandList,
		" root=%u destOffset=%u", rootParameterIndex, destOffset);
	PFN_SET_ROOT_32BIT_CONSTANT original =
		DX12_CL_ORIG(commandList, 34, PFN_SET_ROOT_32BIT_CONSTANT, SetGraphicsRoot32BitConstant);
	if (original)
		original(commandList, rootParameterIndex, srcData, destOffset);
}

static void STDMETHODCALLTYPE HookedSetComputeRoot32BitConstants(
	ID3D12GraphicsCommandList *commandList, UINT rootParameterIndex, UINT num32BitValuesToSet,
	const void *srcData, UINT destOffset)
{
	LogDX12Call("ID3D12GraphicsCommandList::SetComputeRoot32BitConstants", commandList,
		" root=%u values=%u destOffset=%u", rootParameterIndex, num32BitValuesToSet, destOffset);
	PFN_SET_ROOT_32BIT_CONSTANTS original =
		DX12_CL_ORIG(commandList, 35, PFN_SET_ROOT_32BIT_CONSTANTS, SetComputeRoot32BitConstants);
	if (original)
		original(commandList, rootParameterIndex, num32BitValuesToSet, srcData, destOffset);
}

static void STDMETHODCALLTYPE HookedSetGraphicsRoot32BitConstants(
	ID3D12GraphicsCommandList *commandList, UINT rootParameterIndex, UINT num32BitValuesToSet,
	const void *srcData, UINT destOffset)
{
	LogDX12Call("ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstants", commandList,
		" root=%u values=%u destOffset=%u", rootParameterIndex, num32BitValuesToSet, destOffset);
	PFN_SET_ROOT_32BIT_CONSTANTS original =
		DX12_CL_ORIG(commandList, 36, PFN_SET_ROOT_32BIT_CONSTANTS, SetGraphicsRoot32BitConstants);
	if (original)
		original(commandList, rootParameterIndex, num32BitValuesToSet, srcData, destOffset);
}

static void STDMETHODCALLTYPE HookedSetComputeRootConstantBufferView(
	ID3D12GraphicsCommandList *commandList, UINT rootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS address)
{
	LogDX12Call("ID3D12GraphicsCommandList::SetComputeRootConstantBufferView", commandList,
		" root=%u gpu=0x%llx", rootParameterIndex, static_cast<unsigned long long>(address));
	if (ShouldTrackBindings()) {
		DX12BindingSetComputeRootDescriptor(
			commandList, rootParameterIndex, D3D12_ROOT_PARAMETER_TYPE_CBV, address);
		DX12BindingRecordStateEvent(commandList, "set_compute_root_cbv");
	}
	PFN_SET_ROOT_GPU_VA original =
		DX12_CL_ORIG(commandList, 37, PFN_SET_ROOT_GPU_VA, SetComputeRootConstantBufferView);
	if (original)
		original(commandList, rootParameterIndex, address);
}

static void STDMETHODCALLTYPE HookedSetGraphicsRootConstantBufferView(
	ID3D12GraphicsCommandList *commandList, UINT rootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS address)
{
	LogDX12Call("ID3D12GraphicsCommandList::SetGraphicsRootConstantBufferView", commandList,
		" root=%u gpu=0x%llx", rootParameterIndex, static_cast<unsigned long long>(address));
	if (ShouldTrackBindings()) {
		DX12BindingSetGraphicsRootDescriptor(
			commandList, rootParameterIndex, D3D12_ROOT_PARAMETER_TYPE_CBV, address);
		DX12BindingRecordStateEvent(commandList, "set_graphics_root_cbv");
	}
	PFN_SET_ROOT_GPU_VA original =
		DX12_CL_ORIG(commandList, 38, PFN_SET_ROOT_GPU_VA, SetGraphicsRootConstantBufferView);
	if (original)
		original(commandList, rootParameterIndex, address);
}

static void STDMETHODCALLTYPE HookedSetComputeRootShaderResourceView(
	ID3D12GraphicsCommandList *commandList, UINT rootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS address)
{
	LogDX12Call("ID3D12GraphicsCommandList::SetComputeRootShaderResourceView", commandList,
		" root=%u gpu=0x%llx", rootParameterIndex, static_cast<unsigned long long>(address));
	if (ShouldTrackBindings()) {
		DX12BindingSetComputeRootDescriptor(
			commandList, rootParameterIndex, D3D12_ROOT_PARAMETER_TYPE_SRV, address);
		DX12BindingRecordStateEvent(commandList, "set_compute_root_srv");
	}
	PFN_SET_ROOT_GPU_VA original =
		DX12_CL_ORIG(commandList, 39, PFN_SET_ROOT_GPU_VA, SetComputeRootShaderResourceView);
	if (original)
		original(commandList, rootParameterIndex, address);
}

static void STDMETHODCALLTYPE HookedSetGraphicsRootShaderResourceView(
	ID3D12GraphicsCommandList *commandList, UINT rootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS address)
{
	LogDX12Call("ID3D12GraphicsCommandList::SetGraphicsRootShaderResourceView", commandList,
		" root=%u gpu=0x%llx", rootParameterIndex, static_cast<unsigned long long>(address));
	if (ShouldTrackBindings()) {
		DX12BindingSetGraphicsRootDescriptor(
			commandList, rootParameterIndex, D3D12_ROOT_PARAMETER_TYPE_SRV, address);
		DX12BindingRecordStateEvent(commandList, "set_graphics_root_srv");
	}
	PFN_SET_ROOT_GPU_VA original =
		DX12_CL_ORIG(commandList, 40, PFN_SET_ROOT_GPU_VA, SetGraphicsRootShaderResourceView);
	if (original)
		original(commandList, rootParameterIndex, address);
}

static void STDMETHODCALLTYPE HookedSetComputeRootUnorderedAccessView(
	ID3D12GraphicsCommandList *commandList, UINT rootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS address)
{
	LogDX12Call("ID3D12GraphicsCommandList::SetComputeRootUnorderedAccessView", commandList,
		" root=%u gpu=0x%llx", rootParameterIndex, static_cast<unsigned long long>(address));
	if (ShouldTrackBindings()) {
		DX12BindingSetComputeRootDescriptor(
			commandList, rootParameterIndex, D3D12_ROOT_PARAMETER_TYPE_UAV, address);
		DX12BindingRecordStateEvent(commandList, "set_compute_root_uav");
	}
	PFN_SET_ROOT_GPU_VA original =
		DX12_CL_ORIG(commandList, 41, PFN_SET_ROOT_GPU_VA, SetComputeRootUnorderedAccessView);
	if (original)
		original(commandList, rootParameterIndex, address);
}

static void STDMETHODCALLTYPE HookedSetGraphicsRootUnorderedAccessView(
	ID3D12GraphicsCommandList *commandList, UINT rootParameterIndex, D3D12_GPU_VIRTUAL_ADDRESS address)
{
	LogDX12Call("ID3D12GraphicsCommandList::SetGraphicsRootUnorderedAccessView", commandList,
		" root=%u gpu=0x%llx", rootParameterIndex, static_cast<unsigned long long>(address));
	if (ShouldTrackBindings()) {
		DX12BindingSetGraphicsRootDescriptor(
			commandList, rootParameterIndex, D3D12_ROOT_PARAMETER_TYPE_UAV, address);
		DX12BindingRecordStateEvent(commandList, "set_graphics_root_uav");
	}
	PFN_SET_ROOT_GPU_VA original =
		DX12_CL_ORIG(commandList, 42, PFN_SET_ROOT_GPU_VA, SetGraphicsRootUnorderedAccessView);
	if (original)
		original(commandList, rootParameterIndex, address);
}

static void STDMETHODCALLTYPE HookedIASetIndexBuffer(
	ID3D12GraphicsCommandList *commandList, const D3D12_INDEX_BUFFER_VIEW *view)
{
	LogDX12Call("ID3D12GraphicsCommandList::IASetIndexBuffer", commandList,
		" gpu=0x%llx size=%u format=%u",
		view ? static_cast<unsigned long long>(view->BufferLocation) : 0ull,
		view ? view->SizeInBytes : 0,
		view ? static_cast<UINT>(view->Format) : 0);
	if (ShouldTrackBindings())
		DX12BindingSetIndexBuffer(commandList, view);
	PFN_IA_SET_INDEX_BUFFER original = DX12_CL_ORIG(commandList, 43, PFN_IA_SET_INDEX_BUFFER, IASetIndexBuffer);
	if (original)
		original(commandList, view);
}

static void STDMETHODCALLTYPE HookedIASetVertexBuffers(
	ID3D12GraphicsCommandList *commandList, UINT startSlot, UINT count,
	const D3D12_VERTEX_BUFFER_VIEW *views)
{
	char viewsText[512];
	FormatVertexBufferViews(viewsText, sizeof(viewsText), startSlot, count, views);
	LogDX12Call("ID3D12GraphicsCommandList::IASetVertexBuffers", commandList,
		" start=%u count=%u views=%s", startSlot, count, viewsText);
	if (ShouldTrackBindings())
		DX12BindingSetVertexBuffers(commandList, startSlot, count, views);
	PFN_IA_SET_VERTEX_BUFFERS original =
		DX12_CL_ORIG(commandList, 44, PFN_IA_SET_VERTEX_BUFFERS, IASetVertexBuffers);
	if (original)
		original(commandList, startSlot, count, views);
}

static void STDMETHODCALLTYPE HookedSOSetTargets(
	ID3D12GraphicsCommandList *commandList, UINT startSlot, UINT count,
	const D3D12_STREAM_OUTPUT_BUFFER_VIEW *views)
{
	LogDX12Call("ID3D12GraphicsCommandList::SOSetTargets", commandList,
		" start=%u count=%u", startSlot, count);
	gOrigSOSetTargets(commandList, startSlot, count, views);
}

static void STDMETHODCALLTYPE HookedOMSetRenderTargets(
	ID3D12GraphicsCommandList *commandList, UINT numRenderTargetDescriptors,
	const D3D12_CPU_DESCRIPTOR_HANDLE *renderTargetDescriptors,
	BOOL singleHandleToDescriptorRange, const D3D12_CPU_DESCRIPTOR_HANDLE *depthStencilDescriptor)
{
	LogDX12Call("ID3D12GraphicsCommandList::OMSetRenderTargets", commandList,
		" rtCount=%u singleRange=%u dsv=%p", numRenderTargetDescriptors,
		singleHandleToDescriptorRange, depthStencilDescriptor);
	gOrigOMSetRenderTargets(commandList, numRenderTargetDescriptors,
		renderTargetDescriptors, singleHandleToDescriptorRange, depthStencilDescriptor);
}

static void STDMETHODCALLTYPE HookedClearDepthStencilView(
	ID3D12GraphicsCommandList *commandList, D3D12_CPU_DESCRIPTOR_HANDLE depthStencilView,
	D3D12_CLEAR_FLAGS clearFlags, FLOAT depth, UINT8 stencil, UINT numRects, const D3D12_RECT *rects)
{
	LogDX12Call("ID3D12GraphicsCommandList::ClearDepthStencilView", commandList,
		" dsv=0x%llx flags=0x%x rects=%u", static_cast<unsigned long long>(depthStencilView.ptr),
		static_cast<UINT>(clearFlags), numRects);
	gOrigClearDepthStencilView(commandList, depthStencilView, clearFlags, depth, stencil, numRects, rects);
}

static void STDMETHODCALLTYPE HookedClearRenderTargetView(
	ID3D12GraphicsCommandList *commandList, D3D12_CPU_DESCRIPTOR_HANDLE renderTargetView,
	const FLOAT colorRGBA[4], UINT numRects, const D3D12_RECT *rects)
{
	LogDX12Call("ID3D12GraphicsCommandList::ClearRenderTargetView", commandList,
		" rtv=0x%llx rects=%u", static_cast<unsigned long long>(renderTargetView.ptr), numRects);
	gOrigClearRenderTargetView(commandList, renderTargetView, colorRGBA, numRects, rects);
}

static void STDMETHODCALLTYPE HookedClearUnorderedAccessViewUint(
	ID3D12GraphicsCommandList *commandList, D3D12_GPU_DESCRIPTOR_HANDLE viewGpuHandleInCurrentHeap,
	D3D12_CPU_DESCRIPTOR_HANDLE viewCpuHandle, ID3D12Resource *resource, const UINT values[4],
	UINT numRects, const D3D12_RECT *rects)
{
	LogDX12Call("ID3D12GraphicsCommandList::ClearUnorderedAccessViewUint", commandList,
		" resource=%p gpu=0x%llx cpu=0x%llx rects=%u", resource,
		static_cast<unsigned long long>(viewGpuHandleInCurrentHeap.ptr),
		static_cast<unsigned long long>(viewCpuHandle.ptr), numRects);
	gOrigClearUnorderedAccessViewUint(commandList, viewGpuHandleInCurrentHeap, viewCpuHandle,
		resource, values, numRects, rects);
}

static void STDMETHODCALLTYPE HookedClearUnorderedAccessViewFloat(
	ID3D12GraphicsCommandList *commandList, D3D12_GPU_DESCRIPTOR_HANDLE viewGpuHandleInCurrentHeap,
	D3D12_CPU_DESCRIPTOR_HANDLE viewCpuHandle, ID3D12Resource *resource, const FLOAT values[4],
	UINT numRects, const D3D12_RECT *rects)
{
	LogDX12Call("ID3D12GraphicsCommandList::ClearUnorderedAccessViewFloat", commandList,
		" resource=%p gpu=0x%llx cpu=0x%llx rects=%u", resource,
		static_cast<unsigned long long>(viewGpuHandleInCurrentHeap.ptr),
		static_cast<unsigned long long>(viewCpuHandle.ptr), numRects);
	gOrigClearUnorderedAccessViewFloat(commandList, viewGpuHandleInCurrentHeap, viewCpuHandle,
		resource, values, numRects, rects);
}

static void STDMETHODCALLTYPE HookedDiscardResource(
	ID3D12GraphicsCommandList *commandList, ID3D12Resource *resource, const D3D12_DISCARD_REGION *region)
{
	LogDX12Call("ID3D12GraphicsCommandList::DiscardResource", commandList, " resource=%p region=%p", resource, region);
	gOrigDiscardResource(commandList, resource, region);
}

static void STDMETHODCALLTYPE HookedBeginQuery(
	ID3D12GraphicsCommandList *commandList, ID3D12QueryHeap *queryHeap, D3D12_QUERY_TYPE type, UINT index)
{
	LogDX12Call("ID3D12GraphicsCommandList::BeginQuery", commandList,
		" heap=%p type=%u index=%u", queryHeap, static_cast<UINT>(type), index);
	gOrigBeginQuery(commandList, queryHeap, type, index);
}

static void STDMETHODCALLTYPE HookedEndQuery(
	ID3D12GraphicsCommandList *commandList, ID3D12QueryHeap *queryHeap, D3D12_QUERY_TYPE type, UINT index)
{
	LogDX12Call("ID3D12GraphicsCommandList::EndQuery", commandList,
		" heap=%p type=%u index=%u", queryHeap, static_cast<UINT>(type), index);
	gOrigEndQuery(commandList, queryHeap, type, index);
}

static void STDMETHODCALLTYPE HookedResolveQueryData(
	ID3D12GraphicsCommandList *commandList, ID3D12QueryHeap *queryHeap, D3D12_QUERY_TYPE type,
	UINT startIndex, UINT numQueries, ID3D12Resource *destinationBuffer, UINT64 alignedDestinationBufferOffset)
{
	LogDX12Call("ID3D12GraphicsCommandList::ResolveQueryData", commandList,
		" heap=%p type=%u start=%u count=%u dst=%p offset=%llu",
		queryHeap, static_cast<UINT>(type), startIndex, numQueries, destinationBuffer,
		static_cast<unsigned long long>(alignedDestinationBufferOffset));
	gOrigResolveQueryData(commandList, queryHeap, type, startIndex, numQueries,
		destinationBuffer, alignedDestinationBufferOffset);
}

static void STDMETHODCALLTYPE HookedSetPredication(
	ID3D12GraphicsCommandList *commandList, ID3D12Resource *buffer,
	UINT64 alignedBufferOffset, D3D12_PREDICATION_OP operation)
{
	LogDX12Call("ID3D12GraphicsCommandList::SetPredication", commandList,
		" buffer=%p offset=%llu op=%u", buffer,
		static_cast<unsigned long long>(alignedBufferOffset), static_cast<UINT>(operation));
	gOrigSetPredication(commandList, buffer, alignedBufferOffset, operation);
}

static void STDMETHODCALLTYPE HookedCommandListSetMarker(
	ID3D12GraphicsCommandList *commandList, UINT metadata, const void *data, UINT size)
{
	LogDX12Call("ID3D12GraphicsCommandList::SetMarker", commandList, " metadata=%u size=%u", metadata, size);
	gOrigCommandListSetMarker(commandList, metadata, data, size);
}

static void STDMETHODCALLTYPE HookedCommandListBeginEvent(
	ID3D12GraphicsCommandList *commandList, UINT metadata, const void *data, UINT size)
{
	LogDX12Call("ID3D12GraphicsCommandList::BeginEvent", commandList, " metadata=%u size=%u", metadata, size);
	gOrigCommandListBeginEvent(commandList, metadata, data, size);
}

static void STDMETHODCALLTYPE HookedCommandListEndEvent(ID3D12GraphicsCommandList *commandList)
{
	LogDX12Call("ID3D12GraphicsCommandList::EndEvent", commandList);
	gOrigCommandListEndEvent(commandList);
}

static void STDMETHODCALLTYPE HookedExecuteIndirect(
	ID3D12GraphicsCommandList *commandList, ID3D12CommandSignature *commandSignature,
	UINT maxCommandCount, ID3D12Resource *argumentBuffer, UINT64 argumentBufferOffset,
	ID3D12Resource *countBuffer, UINT64 countBufferOffset)
{
	LogDX12Call("ID3D12GraphicsCommandList::ExecuteIndirect", commandList,
		" signature=%p max=%u args=%p argOffset=%llu count=%p countOffset=%llu",
		commandSignature, maxCommandCount, argumentBuffer,
		static_cast<unsigned long long>(argumentBufferOffset), countBuffer,
		static_cast<unsigned long long>(countBufferOffset));
	gOrigExecuteIndirect(commandList, commandSignature, maxCommandCount, argumentBuffer,
		argumentBufferOffset, countBuffer, countBufferOffset);
}

void DX12HookCommandQueue(IUnknown *commandQueue)
{
	if (!commandQueue)
		return;

	ID3D12CommandQueue *queue = nullptr;
	if (FAILED(commandQueue->QueryInterface(IID_PPV_ARGS(&queue))))
		return;

	DX12SetCommandQueue(queue);
	DX12VTableHook queueHooks[] = {
		{8, reinterpret_cast<void**>(&gOrigQueueUpdateTileMappings),
			HookedQueueUpdateTileMappings, "ID3D12CommandQueue::UpdateTileMappings"},
		{9, reinterpret_cast<void**>(&gOrigQueueCopyTileMappings),
			HookedQueueCopyTileMappings, "ID3D12CommandQueue::CopyTileMappings"},
		{10, reinterpret_cast<void**>(&gOrigQueueExecuteCommandLists),
			HookedQueueExecuteCommandLists, "ID3D12CommandQueue::ExecuteCommandLists"},
		{11, reinterpret_cast<void**>(&gOrigQueueSetMarker),
			HookedQueueSetMarker, "ID3D12CommandQueue::SetMarker"},
		{12, reinterpret_cast<void**>(&gOrigQueueBeginEvent),
			HookedQueueBeginEvent, "ID3D12CommandQueue::BeginEvent"},
		{13, reinterpret_cast<void**>(&gOrigQueueEndEvent),
			HookedQueueEndEvent, "ID3D12CommandQueue::EndEvent"},
		{14, reinterpret_cast<void**>(&gOrigQueueSignal),
			HookedQueueSignal, "ID3D12CommandQueue::Signal"},
		{15, reinterpret_cast<void**>(&gOrigQueueWait),
			HookedQueueWait, "ID3D12CommandQueue::Wait"},
		{16, reinterpret_cast<void**>(&gOrigQueueGetTimestampFrequency),
			HookedQueueGetTimestampFrequency, "ID3D12CommandQueue::GetTimestampFrequency"},
		{17, reinterpret_cast<void**>(&gOrigQueueGetClockCalibration),
			HookedQueueGetClockCalibration, "ID3D12CommandQueue::GetClockCalibration"},
	};
	DX12InstallVTableHooks(queue, queueHooks);
	queue->Release();
}

void DX12HookCommandList(IUnknown *commandList)
{
	if (!commandList)
		return;

	ID3D12GraphicsCommandList *baseList = nullptr;
	if (FAILED(commandList->QueryInterface(IID_PPV_ARGS(&baseList))))
		return;

	DX12VTableHook commandListHooks[] = {
		{9, reinterpret_cast<void**>(&gOrigCloseCommandList),
			HookedCloseCommandList, "ID3D12GraphicsCommandList::Close"},
		{10, reinterpret_cast<void**>(&gOrigResetCommandList),
			HookedResetCommandList, "ID3D12GraphicsCommandList::Reset"},
		{11, reinterpret_cast<void**>(&gOrigClearState),
			HookedClearState, "ID3D12GraphicsCommandList::ClearState"},
		{12, reinterpret_cast<void**>(&gOrigDrawInstanced),
			HookedDrawInstanced, "ID3D12GraphicsCommandList::DrawInstanced"},
		{13, reinterpret_cast<void**>(&gOrigDrawIndexedInstanced),
			HookedDrawIndexedInstanced, "ID3D12GraphicsCommandList::DrawIndexedInstanced"},
		{14, reinterpret_cast<void**>(&gOrigDispatch),
			HookedDispatch, "ID3D12GraphicsCommandList::Dispatch"},
		{20, reinterpret_cast<void**>(&gOrigIASetPrimitiveTopology),
			HookedIASetPrimitiveTopology, "ID3D12GraphicsCommandList::IASetPrimitiveTopology"},
		{25, reinterpret_cast<void**>(&gOrigSetPipelineState),
			HookedSetPipelineState, "ID3D12GraphicsCommandList::SetPipelineState"},
		{26, reinterpret_cast<void**>(&gOrigResourceBarrier),
			HookedResourceBarrier, "ID3D12GraphicsCommandList::ResourceBarrier"},
		{27, reinterpret_cast<void**>(&gOrigExecuteBundle),
			HookedExecuteBundle, "ID3D12GraphicsCommandList::ExecuteBundle"},
		{28, reinterpret_cast<void**>(&gOrigSetDescriptorHeaps),
			HookedSetDescriptorHeaps, "ID3D12GraphicsCommandList::SetDescriptorHeaps"},
		{29, reinterpret_cast<void**>(&gOrigSetComputeRootSignature),
			HookedSetComputeRootSignature, "ID3D12GraphicsCommandList::SetComputeRootSignature"},
		{30, reinterpret_cast<void**>(&gOrigSetGraphicsRootSignature),
			HookedSetGraphicsRootSignature, "ID3D12GraphicsCommandList::SetGraphicsRootSignature"},
		{31, reinterpret_cast<void**>(&gOrigSetComputeRootDescriptorTable),
			HookedSetComputeRootDescriptorTable, "ID3D12GraphicsCommandList::SetComputeRootDescriptorTable"},
		{32, reinterpret_cast<void**>(&gOrigSetGraphicsRootDescriptorTable),
			HookedSetGraphicsRootDescriptorTable, "ID3D12GraphicsCommandList::SetGraphicsRootDescriptorTable"},
		{33, reinterpret_cast<void**>(&gOrigSetComputeRoot32BitConstant),
			HookedSetComputeRoot32BitConstant, "ID3D12GraphicsCommandList::SetComputeRoot32BitConstant"},
		{34, reinterpret_cast<void**>(&gOrigSetGraphicsRoot32BitConstant),
			HookedSetGraphicsRoot32BitConstant, "ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstant"},
		{35, reinterpret_cast<void**>(&gOrigSetComputeRoot32BitConstants),
			HookedSetComputeRoot32BitConstants, "ID3D12GraphicsCommandList::SetComputeRoot32BitConstants"},
		{36, reinterpret_cast<void**>(&gOrigSetGraphicsRoot32BitConstants),
			HookedSetGraphicsRoot32BitConstants, "ID3D12GraphicsCommandList::SetGraphicsRoot32BitConstants"},
		{37, reinterpret_cast<void**>(&gOrigSetComputeRootConstantBufferView),
			HookedSetComputeRootConstantBufferView, "ID3D12GraphicsCommandList::SetComputeRootConstantBufferView"},
		{38, reinterpret_cast<void**>(&gOrigSetGraphicsRootConstantBufferView),
			HookedSetGraphicsRootConstantBufferView, "ID3D12GraphicsCommandList::SetGraphicsRootConstantBufferView"},
		{39, reinterpret_cast<void**>(&gOrigSetComputeRootShaderResourceView),
			HookedSetComputeRootShaderResourceView, "ID3D12GraphicsCommandList::SetComputeRootShaderResourceView"},
		{40, reinterpret_cast<void**>(&gOrigSetGraphicsRootShaderResourceView),
			HookedSetGraphicsRootShaderResourceView, "ID3D12GraphicsCommandList::SetGraphicsRootShaderResourceView"},
		{41, reinterpret_cast<void**>(&gOrigSetComputeRootUnorderedAccessView),
			HookedSetComputeRootUnorderedAccessView, "ID3D12GraphicsCommandList::SetComputeRootUnorderedAccessView"},
		{42, reinterpret_cast<void**>(&gOrigSetGraphicsRootUnorderedAccessView),
			HookedSetGraphicsRootUnorderedAccessView, "ID3D12GraphicsCommandList::SetGraphicsRootUnorderedAccessView"},
		{43, reinterpret_cast<void**>(&gOrigIASetIndexBuffer),
			HookedIASetIndexBuffer, "ID3D12GraphicsCommandList::IASetIndexBuffer"},
		{44, reinterpret_cast<void**>(&gOrigIASetVertexBuffers),
			HookedIASetVertexBuffers, "ID3D12GraphicsCommandList::IASetVertexBuffers"},
	};
	DX12InstallVTableHooks(baseList, commandListHooks);
	baseList->Release();
}

void DX12HookCommandListCreation(IUnknown *device)
{
	if (!device)
		return;

	ID3D12Device *baseDevice = nullptr;
	if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&baseDevice)))) {
		DX12VTableHook deviceHooks[] = {
			{8, reinterpret_cast<void**>(&gOrigCreateCommandQueue),
				HookedCreateCommandQueue, "ID3D12Device::CreateCommandQueue"},
			{9, reinterpret_cast<void**>(&gOrigCreateCommandAllocator),
				HookedCreateCommandAllocator, "ID3D12Device::CreateCommandAllocator"},
			{12, reinterpret_cast<void**>(&gOrigCreateCommandList),
				HookedCreateCommandList, "ID3D12Device::CreateCommandList"},
		};
		DX12InstallVTableHooks(baseDevice, deviceHooks);
		baseDevice->Release();
	}

	ID3D12Device4 *device4 = nullptr;
	if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device4)))) {
		DX12VTableHook device4Hooks[] = {
			{52, reinterpret_cast<void**>(&gOrigCreateCommandList1),
				HookedCreateCommandList1, "ID3D12Device4::CreateCommandList1"},
		};
		DX12InstallVTableHooks(device4, device4Hooks);
		device4->Release();
	}
}

