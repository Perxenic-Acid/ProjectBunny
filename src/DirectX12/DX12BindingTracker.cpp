#include "DX12BindingTracker.h"

#include <stdio.h>
#include <string.h>

#include <unordered_set>
#include <string>
#include <unordered_map>
#include <vector>

#include "DX12FrameAnalysis.h"
#include "DX12FrameAnalysisManifest.h"
#include "DX12ResourceTracker.h"
#include "DX12ShaderDump.h"
#include "DX12State.h"

static constexpr UINT MaxRootParameters = 64;
static constexpr UINT MaxVertexBufferSlots = 32;
static constexpr UINT MaxTrackedEvents = 20000;
static constexpr UINT MaxDescriptorsPerRangeDump = 256;

static void GetSummaryDirectory(const wchar_t *dir, wchar_t *path, size_t pathCount)
{
	if (!dir || !path || pathCount == 0)
		return;
	swprintf_s(path, pathCount, L"%s\\summary", dir);
	CreateDirectoryW(path, nullptr);
}

struct RootTableState
{
	bool valid = false;
	UINT rootParameterIndex = 0;
	D3D12_GPU_DESCRIPTOR_HANDLE baseDescriptor = {};
};

struct RootDescriptorState
{
	bool valid = false;
	UINT rootParameterIndex = 0;
	D3D12_ROOT_PARAMETER_TYPE type = D3D12_ROOT_PARAMETER_TYPE_CBV;
	D3D12_GPU_VIRTUAL_ADDRESS address = 0;
};

struct CommandListBindingState
{
	ID3D12PipelineState *pipelineState = nullptr;
	ID3D12RootSignature *graphicsRootSignature = nullptr;
	ID3D12RootSignature *computeRootSignature = nullptr;
	ID3D12DescriptorHeap *cbvSrvUavHeap = nullptr;
	ID3D12DescriptorHeap *samplerHeap = nullptr;
	RootTableState graphicsTables[MaxRootParameters] = {};
	RootTableState computeTables[MaxRootParameters] = {};
	RootDescriptorState graphicsRootDescriptors[MaxRootParameters] = {};
	RootDescriptorState computeRootDescriptors[MaxRootParameters] = {};
	D3D12_VERTEX_BUFFER_VIEW vertexBuffers[MaxVertexBufferSlots] = {};
	bool vertexBufferValid[MaxVertexBufferSlots] = {};
	D3D12_INDEX_BUFFER_VIEW indexBuffer = {};
	bool indexBufferValid = false;
	D3D12_PRIMITIVE_TOPOLOGY primitiveTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
	UINT64 drawSerial = 0;
	UINT64 dispatchSerial = 0;
};

struct BindingEvent
{
	UINT64 serial = 0;
	std::string kind;
	UINT64 drawId = 0;
	UINT64 dispatchId = 0;
	ID3D12GraphicsCommandList *commandList = nullptr;
	ID3D12PipelineState *pipelineState = nullptr;
	ID3D12RootSignature *graphicsRootSignature = nullptr;
	ID3D12RootSignature *computeRootSignature = nullptr;
	DX12PsoShaderInfo shaderInfo = {};
	ID3D12DescriptorHeap *cbvSrvUavHeap = nullptr;
	ID3D12DescriptorHeap *samplerHeap = nullptr;
	RootTableState graphicsTables[MaxRootParameters] = {};
	RootTableState computeTables[MaxRootParameters] = {};
	RootDescriptorState graphicsRootDescriptors[MaxRootParameters] = {};
	RootDescriptorState computeRootDescriptors[MaxRootParameters] = {};
	D3D12_VERTEX_BUFFER_VIEW vertexBuffers[MaxVertexBufferSlots] = {};
	bool vertexBufferValid[MaxVertexBufferSlots] = {};
	D3D12_INDEX_BUFFER_VIEW indexBuffer = {};
	bool indexBufferValid = false;
	D3D12_PRIMITIVE_TOPOLOGY primitiveTopology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
	UINT vertexCountPerInstance = 0;
	UINT indexCountPerInstance = 0;
	UINT instanceCount = 0;
	UINT startVertexLocation = 0;
	UINT startIndexLocation = 0;
	INT baseVertexLocation = 0;
	UINT startInstanceLocation = 0;
	UINT threadGroupCountX = 0;
	UINT threadGroupCountY = 0;
	UINT threadGroupCountZ = 0;
};

static SRWLOCK gBindingLock = SRWLOCK_INIT;
static std::unordered_map<ID3D12GraphicsCommandList*, CommandListBindingState> gCommandLists;
static std::vector<BindingEvent> gEvents;
static UINT64 gEventSerial = 0;
static UINT64 gGlobalDrawSerial = 0;
static UINT64 gGlobalDispatchSerial = 0;
static UINT64 gDroppedEvents = 0;

static void StoreEventLocked(
	const char *kind, ID3D12GraphicsCommandList *commandList,
	const CommandListBindingState &state)
{
	if (gEvents.size() >= MaxTrackedEvents) {
		gDroppedEvents++;
		return;
	}

	BindingEvent event;
	event.serial = ++gEventSerial;
	event.kind = kind ? kind : "";
	const bool isDrawKind = kind && (!strcmp(kind, "draw") || !strcmp(kind, "draw_indexed"));
	const bool isDispatchKind = kind && !strcmp(kind, "dispatch");
	event.drawId = isDrawKind ? state.drawSerial : 0;
	event.dispatchId = isDispatchKind ? state.dispatchSerial : 0;
	event.commandList = commandList;
	event.pipelineState = state.pipelineState;
	event.graphicsRootSignature = state.graphicsRootSignature;
	event.computeRootSignature = state.computeRootSignature;
	event.cbvSrvUavHeap = state.cbvSrvUavHeap;
	event.samplerHeap = state.samplerHeap;
	memcpy(event.graphicsTables, state.graphicsTables, sizeof(event.graphicsTables));
	memcpy(event.computeTables, state.computeTables, sizeof(event.computeTables));
	memcpy(event.graphicsRootDescriptors, state.graphicsRootDescriptors, sizeof(event.graphicsRootDescriptors));
	memcpy(event.computeRootDescriptors, state.computeRootDescriptors, sizeof(event.computeRootDescriptors));
	memcpy(event.vertexBuffers, state.vertexBuffers, sizeof(event.vertexBuffers));
	memcpy(event.vertexBufferValid, state.vertexBufferValid, sizeof(event.vertexBufferValid));
	event.indexBuffer = state.indexBuffer;
	event.indexBufferValid = state.indexBufferValid;
	event.primitiveTopology = state.primitiveTopology;
	if (state.pipelineState) {
		DX12GetPipelineStateShaderInfo(state.pipelineState, &event.shaderInfo);
		ID3D12RootSignature *psoRootSignature = nullptr;
		if (DX12GetPsoRootSignature(event.shaderInfo.psoIndex, &psoRootSignature)) {
			if (!event.graphicsRootSignature)
				event.graphicsRootSignature = psoRootSignature;
			if (!event.computeRootSignature)
				event.computeRootSignature = psoRootSignature;
		}
	}
	gEvents.push_back(event);
}

void DX12BindingRegisterCommandList(ID3D12GraphicsCommandList *commandList)
{
	if (!commandList)
		return;

	AcquireSRWLockExclusive(&gBindingLock);
	gCommandLists.try_emplace(commandList);
	ReleaseSRWLockExclusive(&gBindingLock);
}

void DX12BindingResetCommandList(
	ID3D12GraphicsCommandList *commandList, ID3D12PipelineState *initialState)
{
	if (!commandList)
		return;

	AcquireSRWLockExclusive(&gBindingLock);
	CommandListBindingState &state = gCommandLists[commandList];
	state = CommandListBindingState();
	state.pipelineState = initialState;
	ReleaseSRWLockExclusive(&gBindingLock);
}

void DX12BindingSetPipelineState(
	ID3D12GraphicsCommandList *commandList, ID3D12PipelineState *pipelineState)
{
	if (!commandList)
		return;

	AcquireSRWLockExclusive(&gBindingLock);
	gCommandLists[commandList].pipelineState = pipelineState;
	ReleaseSRWLockExclusive(&gBindingLock);
}

void DX12BindingSetGraphicsRootSignature(
	ID3D12GraphicsCommandList *commandList, ID3D12RootSignature *rootSignature)
{
	if (!commandList)
		return;

	AcquireSRWLockExclusive(&gBindingLock);
	gCommandLists[commandList].graphicsRootSignature = rootSignature;
	ReleaseSRWLockExclusive(&gBindingLock);
}

void DX12BindingSetComputeRootSignature(
	ID3D12GraphicsCommandList *commandList, ID3D12RootSignature *rootSignature)
{
	if (!commandList)
		return;

	AcquireSRWLockExclusive(&gBindingLock);
	gCommandLists[commandList].computeRootSignature = rootSignature;
	ReleaseSRWLockExclusive(&gBindingLock);
}

void DX12BindingRecordStateEvent(ID3D12GraphicsCommandList *commandList, const char *kind)
{
	if (!commandList)
		return;

	AcquireSRWLockExclusive(&gBindingLock);
	CommandListBindingState &state = gCommandLists[commandList];
	StoreEventLocked(kind ? kind : "state", commandList, state);
	ReleaseSRWLockExclusive(&gBindingLock);
}

void DX12BindingSetDescriptorHeaps(
	ID3D12GraphicsCommandList *commandList, UINT count,
	ID3D12DescriptorHeap *const *heaps)
{
	if (!commandList)
		return;

	AcquireSRWLockExclusive(&gBindingLock);
	CommandListBindingState &state = gCommandLists[commandList];
	state.cbvSrvUavHeap = nullptr;
	state.samplerHeap = nullptr;
	for (UINT i = 0; i < count && heaps; ++i) {
		ID3D12DescriptorHeap *heap = heaps[i];
		if (!heap)
			continue;
		D3D12_DESCRIPTOR_HEAP_DESC desc = heap->GetDesc();
		if (desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV)
			state.cbvSrvUavHeap = heap;
		else if (desc.Type == D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER)
			state.samplerHeap = heap;
	}
	ReleaseSRWLockExclusive(&gBindingLock);
}

void DX12BindingSetGraphicsRootDescriptorTable(
	ID3D12GraphicsCommandList *commandList, UINT rootParameterIndex,
	D3D12_GPU_DESCRIPTOR_HANDLE baseDescriptor)
{
	if (!commandList || rootParameterIndex >= MaxRootParameters)
		return;

	AcquireSRWLockExclusive(&gBindingLock);
	RootTableState &table = gCommandLists[commandList].graphicsTables[rootParameterIndex];
	table.valid = true;
	table.rootParameterIndex = rootParameterIndex;
	table.baseDescriptor = baseDescriptor;
	ReleaseSRWLockExclusive(&gBindingLock);
}

void DX12BindingSetComputeRootDescriptorTable(
	ID3D12GraphicsCommandList *commandList, UINT rootParameterIndex,
	D3D12_GPU_DESCRIPTOR_HANDLE baseDescriptor)
{
	if (!commandList || rootParameterIndex >= MaxRootParameters)
		return;

	AcquireSRWLockExclusive(&gBindingLock);
	RootTableState &table = gCommandLists[commandList].computeTables[rootParameterIndex];
	table.valid = true;
	table.rootParameterIndex = rootParameterIndex;
	table.baseDescriptor = baseDescriptor;
	ReleaseSRWLockExclusive(&gBindingLock);
}

void DX12BindingSetPrimitiveTopology(
	ID3D12GraphicsCommandList *commandList, D3D12_PRIMITIVE_TOPOLOGY topology)
{
	if (!commandList)
		return;

	AcquireSRWLockExclusive(&gBindingLock);
	gCommandLists[commandList].primitiveTopology = topology;
	ReleaseSRWLockExclusive(&gBindingLock);
}

void DX12BindingSetIndexBuffer(
	ID3D12GraphicsCommandList *commandList, const D3D12_INDEX_BUFFER_VIEW *view)
{
	if (!commandList)
		return;

	AcquireSRWLockExclusive(&gBindingLock);
	CommandListBindingState &state = gCommandLists[commandList];
	if (view) {
		state.indexBuffer = *view;
		state.indexBufferValid = true;
	} else {
		state.indexBuffer = {};
		state.indexBufferValid = false;
	}
	ReleaseSRWLockExclusive(&gBindingLock);
}

void DX12BindingSetVertexBuffers(
	ID3D12GraphicsCommandList *commandList, UINT startSlot, UINT count,
	const D3D12_VERTEX_BUFFER_VIEW *views)
{
	if (!commandList || startSlot >= MaxVertexBufferSlots)
		return;

	AcquireSRWLockExclusive(&gBindingLock);
	CommandListBindingState &state = gCommandLists[commandList];
	for (UINT i = 0; i < count && startSlot + i < MaxVertexBufferSlots; ++i) {
		const UINT slot = startSlot + i;
		if (views) {
			state.vertexBuffers[slot] = views[i];
			state.vertexBufferValid[slot] = views[i].BufferLocation != 0 && views[i].SizeInBytes != 0;
		} else {
			state.vertexBuffers[slot] = {};
			state.vertexBufferValid[slot] = false;
		}
	}
	ReleaseSRWLockExclusive(&gBindingLock);
}

void DX12BindingRecordDrawInstanced(
	ID3D12GraphicsCommandList *commandList, UINT vertexCountPerInstance,
	UINT instanceCount, UINT startVertexLocation, UINT startInstanceLocation)
{
	if (!commandList)
		return;

	AcquireSRWLockExclusive(&gBindingLock);
	CommandListBindingState &state = gCommandLists[commandList];
	state.drawSerial = ++gGlobalDrawSerial;
	const size_t before = gEvents.size();
	StoreEventLocked("draw", commandList, state);
	if (gEvents.size() > before) {
		BindingEvent &event = gEvents.back();
		event.vertexCountPerInstance = vertexCountPerInstance;
		event.instanceCount = instanceCount;
		event.startVertexLocation = startVertexLocation;
		event.startInstanceLocation = startInstanceLocation;
	}
	ReleaseSRWLockExclusive(&gBindingLock);
}

void DX12BindingRecordDrawIndexedInstanced(
	ID3D12GraphicsCommandList *commandList, UINT indexCountPerInstance,
	UINT instanceCount, UINT startIndexLocation, INT baseVertexLocation,
	UINT startInstanceLocation)
{
	if (!commandList)
		return;

	AcquireSRWLockExclusive(&gBindingLock);
	CommandListBindingState &state = gCommandLists[commandList];
	state.drawSerial = ++gGlobalDrawSerial;
	const size_t before = gEvents.size();
	StoreEventLocked("draw_indexed", commandList, state);
	if (gEvents.size() > before) {
		BindingEvent &event = gEvents.back();
		event.indexCountPerInstance = indexCountPerInstance;
		event.instanceCount = instanceCount;
		event.startIndexLocation = startIndexLocation;
		event.baseVertexLocation = baseVertexLocation;
		event.startInstanceLocation = startInstanceLocation;
	}
	ReleaseSRWLockExclusive(&gBindingLock);
}

void DX12BindingRecordDispatch(
	ID3D12GraphicsCommandList *commandList, UINT threadGroupCountX,
	UINT threadGroupCountY, UINT threadGroupCountZ)
{
	if (!commandList)
		return;

	AcquireSRWLockExclusive(&gBindingLock);
	CommandListBindingState &state = gCommandLists[commandList];
	state.dispatchSerial = ++gGlobalDispatchSerial;
	const size_t before = gEvents.size();
	StoreEventLocked("dispatch", commandList, state);
	if (gEvents.size() > before) {
		BindingEvent &event = gEvents.back();
		event.threadGroupCountX = threadGroupCountX;
		event.threadGroupCountY = threadGroupCountY;
		event.threadGroupCountZ = threadGroupCountZ;
	}
	ReleaseSRWLockExclusive(&gBindingLock);
}

void DX12BindingBeginFrame()
{
	AcquireSRWLockExclusive(&gBindingLock);
	gEvents.clear();
	gEventSerial = 0;
	gGlobalDrawSerial = 0;
	gGlobalDispatchSerial = 0;
	gDroppedEvents = 0;
	ReleaseSRWLockExclusive(&gBindingLock);
}

void DX12BindingSetGraphicsRootDescriptor(
	ID3D12GraphicsCommandList *commandList, UINT rootParameterIndex,
	D3D12_ROOT_PARAMETER_TYPE type, D3D12_GPU_VIRTUAL_ADDRESS address)
{
	if (!commandList || rootParameterIndex >= MaxRootParameters)
		return;

	AcquireSRWLockExclusive(&gBindingLock);
	RootDescriptorState &descriptor = gCommandLists[commandList].graphicsRootDescriptors[rootParameterIndex];
	descriptor.valid = address != 0;
	descriptor.rootParameterIndex = rootParameterIndex;
	descriptor.type = type;
	descriptor.address = address;
	ReleaseSRWLockExclusive(&gBindingLock);
}

void DX12BindingSetComputeRootDescriptor(
	ID3D12GraphicsCommandList *commandList, UINT rootParameterIndex,
	D3D12_ROOT_PARAMETER_TYPE type, D3D12_GPU_VIRTUAL_ADDRESS address)
{
	if (!commandList || rootParameterIndex >= MaxRootParameters)
		return;

	AcquireSRWLockExclusive(&gBindingLock);
	RootDescriptorState &descriptor = gCommandLists[commandList].computeRootDescriptors[rootParameterIndex];
	descriptor.valid = address != 0;
	descriptor.rootParameterIndex = rootParameterIndex;
	descriptor.type = type;
	descriptor.address = address;
	ReleaseSRWLockExclusive(&gBindingLock);
}

void DX12GetCurrentFrameShaderInfos(std::vector<DX12PsoShaderInfo> *shaderInfos)
{
	if (!shaderInfos)
		return;

	shaderInfos->clear();
	std::unordered_set<UINT64> seen;
	AcquireSRWLockShared(&gBindingLock);
	for (const BindingEvent &event : gEvents) {
		if (!event.shaderInfo.psoIndex)
			continue;
		if (!seen.insert(event.shaderInfo.psoIndex).second)
			continue;
		shaderInfos->push_back(event.shaderInfo);
	}
	ReleaseSRWLockShared(&gBindingLock);
}

static void WriteShaderInfo(FILE *file, const DX12PsoShaderInfo &info)
{
	if (!file)
		return;

	fprintf(file, "%llu,",
		static_cast<unsigned long long>(info.psoIndex));
	if (info.hasVS)
		fprintf(file, "%016llx", static_cast<unsigned long long>(info.vs));
	fprintf(file, ",");
	if (info.hasPS)
		fprintf(file, "%016llx", static_cast<unsigned long long>(info.ps));
	fprintf(file, ",");
	if (info.hasCS)
		fprintf(file, "%016llx", static_cast<unsigned long long>(info.cs));
}

static void FormatOptionalHashText(bool hasHash, UINT64 hash, char *text, size_t textCount)
{
	if (!text || textCount == 0)
		return;
	if (hasHash)
		sprintf_s(text, textCount, "%016llx", static_cast<unsigned long long>(hash));
	else
		sprintf_s(text, textCount, "-");
}

static void AppendShaderHashNamePart(
	wchar_t *text, size_t textCount, const char *stage, bool hasHash, UINT64 hash)
{
	if (!text || textCount == 0 || !stage || !hasHash)
		return;
	const size_t used = wcslen(text);
	if (used >= textCount)
		return;
	swprintf_s(text + used, textCount - used, L"-%S%016llx",
		stage, static_cast<unsigned long long>(hash));
}

static void BuildShaderHashNamePart(
	const DX12PsoShaderInfo &info, wchar_t *text, size_t textCount)
{
	if (!text || textCount == 0)
		return;
	text[0] = L'\0';
	AppendShaderHashNamePart(text, textCount, "vs", info.hasVS, info.vs);
	AppendShaderHashNamePart(text, textCount, "ps", info.hasPS, info.ps);
	AppendShaderHashNamePart(text, textCount, "cs", info.hasCS, info.cs);
}

static void WriteRootTables(FILE *file, const RootTableState *tables)
{
	bool first = true;
	for (UINT i = 0; i < MaxRootParameters; ++i) {
		if (!tables[i].valid)
			continue;
		if (!first)
			fprintf(file, ";");
		fprintf(file, "%u=0x%llx",
			tables[i].rootParameterIndex,
			static_cast<unsigned long long>(tables[i].baseDescriptor.ptr));
		first = false;
	}
}

static void WriteVertexBufferSlots(FILE *file, const BindingEvent &event)
{
	bool first = true;
	for (UINT slot = 0; slot < MaxVertexBufferSlots; ++slot) {
		if (!event.vertexBufferValid[slot])
			continue;
		const D3D12_VERTEX_BUFFER_VIEW &view = event.vertexBuffers[slot];
		if (!first)
			fprintf(file, ";");
		fprintf(file, "%u:0x%llx:%u:%u",
			slot,
			static_cast<unsigned long long>(view.BufferLocation),
			view.SizeInBytes,
			view.StrideInBytes);
		first = false;
	}
}

static const DX12DescriptorHeapSummary *FindHeapForGpuHandle(
	const std::vector<DX12DescriptorHeapSummary> &heaps,
	const RootTableState &table,
	UINT requiredType);
static const DX12DescriptorSummary *FindDescriptorByCpuHandle(
	const std::unordered_map<SIZE_T, const DX12DescriptorSummary*> &descriptorsByCpuHandle,
	SIZE_T cpuHandle);
static void BuildDescriptorLookup(
	const std::vector<DX12DescriptorSummary> &descriptors,
	std::unordered_map<SIZE_T, const DX12DescriptorSummary*> *descriptorsByCpuHandle);

static std::string ResourceRefsForEvent(
	const BindingEvent &event,
	const std::vector<DX12DescriptorHeapSummary> &heaps,
	const std::unordered_map<SIZE_T, const DX12DescriptorSummary*> &descriptorsByCpuHandle)
{
	std::string refs;
	std::unordered_set<std::string> seen;
	for (UINT i = 0; i < MaxRootParameters; ++i) {
		struct TableRef {
			const char *space;
			const RootTableState &table;
			UINT heapType;
		};
		const TableRef tables[] = {
			{ "g", event.graphicsTables[i], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV },
			{ "gs", event.graphicsTables[i], D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER },
			{ "c", event.computeTables[i], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV },
			{ "cs", event.computeTables[i], D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER },
		};
		for (const TableRef &entry : tables) {
			if (!entry.table.valid)
				continue;
			const DX12DescriptorHeapSummary *heap =
				FindHeapForGpuHandle(heaps, entry.table, entry.heapType);
			if (!heap)
				continue;
			const UINT64 descriptorIndex = (entry.table.baseDescriptor.ptr - heap->gpuStart) / heap->increment;
			const SIZE_T cpuHandle = heap->cpuStart + static_cast<SIZE_T>(descriptorIndex) * heap->increment;
			const DX12DescriptorSummary *descriptor = FindDescriptorByCpuHandle(descriptorsByCpuHandle, cpuHandle);
			char key[128];
			sprintf_s(key, "%s%u:%llu:%p",
				entry.space,
				entry.table.rootParameterIndex,
				static_cast<unsigned long long>(descriptorIndex),
				descriptor ? descriptor->resource : nullptr);
			if (!seen.insert(key).second)
				continue;
			if (!refs.empty())
				refs += ";";
			refs += key;
		}
	}
	return refs;
}

static const char *HeapTypeName(UINT type)
{
	switch (type) {
	case D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV:
		return "CBV_SRV_UAV";
	case D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER:
		return "SAMPLER";
	case D3D12_DESCRIPTOR_HEAP_TYPE_RTV:
		return "RTV";
	case D3D12_DESCRIPTOR_HEAP_TYPE_DSV:
		return "DSV";
	default:
		return "UNKNOWN";
	}
}

static const char *ResourceDimensionName(D3D12_RESOURCE_DIMENSION dimension)
{
	switch (dimension) {
	case D3D12_RESOURCE_DIMENSION_BUFFER:
		return "BUFFER";
	case D3D12_RESOURCE_DIMENSION_TEXTURE1D:
		return "TEXTURE1D";
	case D3D12_RESOURCE_DIMENSION_TEXTURE2D:
		return "TEXTURE2D";
	case D3D12_RESOURCE_DIMENSION_TEXTURE3D:
		return "TEXTURE3D";
	default:
		return "UNKNOWN";
	}
}

static const char *TopologyName(D3D12_PRIMITIVE_TOPOLOGY topology)
{
	switch (topology) {
	case D3D_PRIMITIVE_TOPOLOGY_POINTLIST:
		return "POINTLIST";
	case D3D_PRIMITIVE_TOPOLOGY_LINELIST:
		return "LINELIST";
	case D3D_PRIMITIVE_TOPOLOGY_LINESTRIP:
		return "LINESTRIP";
	case D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST:
		return "TRIANGLELIST";
	case D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP:
		return "TRIANGLESTRIP";
	default:
		return "UNKNOWN";
	}
}

static bool IsDrawEvent(const BindingEvent &event)
{
	return event.kind == "draw" || event.kind == "draw_indexed";
}

static bool IsDispatchEvent(const BindingEvent &event)
{
	return event.kind == "dispatch";
}

struct FlatBufferRow
{
	std::string id;
	std::string role;
	std::string skinningClass;
	UINT64 eventSerial = 0;
	UINT64 drawId = 0;
	UINT64 dispatchId = 0;
	UINT64 psoIndex = 0;
	DX12PsoShaderInfo shaderInfo = {};
	UINT64 producerEventSerial = 0;
	UINT64 producerDrawId = 0;
	UINT64 producerDispatchId = 0;
	UINT64 producerPsoIndex = 0;
	DX12PsoShaderInfo producerShaderInfo = {};
	std::string producerBindSpace;
	UINT producerRootParameterIndex = 0;
	UINT producerShaderRegister = UINT_MAX;
	UINT64 gpuVa = 0;
	UINT64 size = 0;
	UINT stride = 0;
	UINT slot = 0;
	UINT format = 0;
	bool resolved = false;
	DX12BufferResourceSummary resource = {};
};

struct BufferProducer
{
	UINT64 eventSerial = 0;
	UINT64 drawId = 0;
	UINT64 dispatchId = 0;
	UINT64 psoIndex = 0;
	DX12PsoShaderInfo shaderInfo = {};
	std::string bindSpace;
	UINT rootParameterIndex = 0;
	UINT shaderRegister = UINT_MAX;
	ID3D12Resource *resource = nullptr;
	UINT64 begin = 0;
	UINT64 end = 0;
};

static std::string MakeBufferKey(
	const char *role, UINT64 gpuVa, UINT64 size, UINT stride, UINT slot, UINT format)
{
	char key[128];
	sprintf_s(key, "%s|%llx|%llu|%u|%u|%u",
		role ? role : "",
		static_cast<unsigned long long>(gpuVa),
		static_cast<unsigned long long>(size),
		stride,
		slot,
		format);
	return key;
}

static std::string NextBufferId(const char *prefix, UINT *nextId)
{
	char id[32];
	const UINT value = nextId ? ++(*nextId) : 0;
	sprintf_s(id, "%s_%u", prefix ? prefix : "buf", value);
	return id;
}

static const FlatBufferRow *GetOrAddBufferRow(
	std::vector<FlatBufferRow> *rows, std::unordered_map<std::string, size_t> *rowByKey,
	UINT *nextVbId, UINT *nextIbId, const BindingEvent &event, const char *role,
	UINT64 gpuVa, UINT64 size, UINT stride, UINT slot, UINT format)
{
	if (!rows || !rowByKey || gpuVa == 0 || size == 0)
		return nullptr;

	std::string key = MakeBufferKey(role, gpuVa, size, stride, slot, format);
	auto found = rowByKey->find(key);
	if (found != rowByKey->end() && found->second < rows->size())
		return &(*rows)[found->second];

	FlatBufferRow row;
	row.id = NextBufferId(role && !strcmp(role, "IB") ? "ib" : "vb",
		role && !strcmp(role, "IB") ? nextIbId : nextVbId);
	row.role = role ? role : "";
	row.eventSerial = event.serial;
	row.drawId = event.drawId;
	row.dispatchId = event.dispatchId;
	row.psoIndex = event.shaderInfo.psoIndex;
	row.shaderInfo = event.shaderInfo;
	row.gpuVa = gpuVa;
	row.size = size;
	row.stride = stride;
	row.slot = slot;
	row.format = format;
	row.resolved = DX12ResolveBufferResourceByGpuVa(gpuVa, size, &row.resource);
	(*rowByKey)[key] = rows->size();
	rows->push_back(row);
	return &rows->back();
}

static std::string BufferIdsForEvent(
	const BindingEvent &event, std::vector<FlatBufferRow> *rows,
	std::unordered_map<std::string, size_t> *rowByKey, UINT *nextVbId, UINT *nextIbId)
{
	std::string ids;
	for (UINT slot = 0; slot < MaxVertexBufferSlots; ++slot) {
		if (!event.vertexBufferValid[slot])
			continue;
		const D3D12_VERTEX_BUFFER_VIEW &view = event.vertexBuffers[slot];
		const FlatBufferRow *row = GetOrAddBufferRow(rows, rowByKey, nextVbId, nextIbId,
			event, "VB", view.BufferLocation, view.SizeInBytes, view.StrideInBytes, slot, 0);
		if (!row)
			continue;
		if (!ids.empty())
			ids += ";";
		char item[64];
		sprintf_s(item, "%u:%s", slot, row->id.c_str());
		ids += item;
	}
	return ids;
}

static std::string IndexBufferIdForEvent(
	const BindingEvent &event, std::vector<FlatBufferRow> *rows,
	std::unordered_map<std::string, size_t> *rowByKey, UINT *nextVbId, UINT *nextIbId)
{
	if (!event.indexBufferValid || event.indexBuffer.BufferLocation == 0 ||
		event.indexBuffer.SizeInBytes == 0)
		return "";

	const FlatBufferRow *row = GetOrAddBufferRow(rows, rowByKey, nextVbId, nextIbId,
		event, "IB", event.indexBuffer.BufferLocation, event.indexBuffer.SizeInBytes, 0, 0,
		static_cast<UINT>(event.indexBuffer.Format));
	return row ? row->id : "";
}

static void CollectFlatBuffersForEvents(
	const std::vector<BindingEvent> &events, std::vector<FlatBufferRow> *buffers)
{
	if (!buffers)
		return;

	buffers->clear();
	std::unordered_map<std::string, size_t> bufferByKey;
	UINT nextVbId = 0;
	UINT nextIbId = 0;
	for (const BindingEvent &event : events) {
		if (!IsDrawEvent(event) && !IsDispatchEvent(event))
			continue;
		BufferIdsForEvent(event, buffers, &bufferByKey, &nextVbId, &nextIbId);
		IndexBufferIdForEvent(event, buffers, &bufferByKey, &nextVbId, &nextIbId);
	}
}

static void AddFrameIaBufferRow(
	std::vector<FlatBufferRow> *rows, UINT *nextVbId, UINT *nextIbId,
	const BindingEvent &event, const char *role, UINT64 gpuVa, UINT64 size,
	UINT stride, UINT slot, UINT format)
{
	if (!rows || gpuVa == 0 || size == 0)
		return;

	FlatBufferRow row;
	row.id = NextBufferId(role && !strcmp(role, "IB") ? "ib" : "vb",
		role && !strcmp(role, "IB") ? nextIbId : nextVbId);
	row.role = role ? role : "";
	row.eventSerial = event.serial;
	row.drawId = event.drawId;
	row.dispatchId = event.dispatchId;
	row.psoIndex = event.shaderInfo.psoIndex;
	row.shaderInfo = event.shaderInfo;
	row.gpuVa = gpuVa;
	row.size = size;
	row.stride = stride;
	row.slot = slot;
	row.format = format;
	row.resolved = DX12ResolveBufferResourceByGpuVa(gpuVa, size, &row.resource);
	rows->push_back(row);
}

static void CollectFrameIaBuffersForEvents(
	const std::vector<BindingEvent> &events, std::vector<FlatBufferRow> *buffers)
{
	if (!buffers)
		return;

	buffers->clear();
	UINT nextVbId = 0;
	UINT nextIbId = 0;
	for (const BindingEvent &event : events) {
		if (!IsDrawEvent(event))
			continue;
		for (UINT slot = 0; slot < MaxVertexBufferSlots; ++slot) {
			if (!event.vertexBufferValid[slot])
				continue;
			const D3D12_VERTEX_BUFFER_VIEW &view = event.vertexBuffers[slot];
			AddFrameIaBufferRow(buffers, &nextVbId, &nextIbId, event, "VB",
				view.BufferLocation, view.SizeInBytes, view.StrideInBytes, slot, 0);
		}
		if (event.indexBufferValid) {
			AddFrameIaBufferRow(buffers, &nextVbId, &nextIbId, event, "IB",
				event.indexBuffer.BufferLocation, event.indexBuffer.SizeInBytes, 0, 0,
				static_cast<UINT>(event.indexBuffer.Format));
		}
	}
}

static bool RangesOverlap(UINT64 aBegin, UINT64 aSize, UINT64 bBegin, UINT64 bSize)
{
	if (aSize == 0 || bSize == 0)
		return false;
	const UINT64 aEnd = aBegin + aSize;
	const UINT64 bEnd = bBegin + bSize;
	if (aEnd < aBegin || bEnd < bBegin)
		return false;
	return aBegin < bEnd && bBegin < aEnd;
}

static bool BindingIsUavBufferProducer(const DX12FrameResourceBinding &binding)
{
	if (binding.rootDescriptor)
		return binding.rangeType == D3D12_ROOT_PARAMETER_TYPE_UAV &&
			binding.gpuVirtualAddress != 0;
	if (!binding.hasDescriptor || binding.descriptor.kind != "UAV")
		return false;
	if (!binding.descriptor.resource)
		return false;
	if (!binding.descriptor.hasResourceDesc ||
		binding.descriptor.resourceDesc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER)
		return false;
	return true;
}

static BufferProducer ProducerFromBinding(const DX12FrameResourceBinding &binding)
{
	BufferProducer producer;
	producer.eventSerial = binding.eventSerial;
	producer.drawId = binding.drawId;
	producer.dispatchId = binding.dispatchId;
	producer.psoIndex = binding.psoIndex;
	producer.shaderInfo = binding.shaderInfo;
	producer.bindSpace = binding.bindSpace;
	producer.rootParameterIndex = binding.rootParameterIndex;
	producer.shaderRegister = binding.shaderRegister;
	if (binding.rootDescriptor) {
		DX12BufferResourceSummary resource;
		if (DX12ResolveBufferResourceByGpuVa(binding.gpuVirtualAddress, 1, &resource)) {
			producer.resource = resource.resource;
			producer.begin = binding.gpuVirtualAddress;
			const UINT64 viewBytes = resource.viewSize ? resource.viewSize :
				resource.hasResourceDesc ? resource.resourceDesc.Width : 1;
			producer.end = producer.begin + viewBytes;
			if (producer.end < producer.begin)
				producer.end = producer.begin;
		}
		return producer;
	}
	producer.resource = binding.descriptor.resource;
	producer.begin = binding.descriptor.gpuVirtualAddress + binding.descriptor.resourceOffset;
	const UINT64 viewBytes = binding.descriptor.viewSize ?
		binding.descriptor.viewSize : binding.descriptor.resourceDesc.Width;
	producer.end = producer.begin + viewBytes;
	if (producer.end < producer.begin)
		producer.end = producer.begin;
	return producer;
}

static const BufferProducer *FindLatestProducerForBuffer(
	const FlatBufferRow &row, const std::vector<BufferProducer> &producers)
{
	if (row.role != "VB" || !row.resolved || !row.resource.resource)
		return nullptr;

	const BufferProducer *best = nullptr;
	for (const BufferProducer &producer : producers) {
		if (producer.eventSerial >= row.eventSerial)
			continue;
		if (producer.resource != row.resource.resource)
			continue;
		if (!RangesOverlap(row.gpuVa, row.size, producer.begin, producer.end - producer.begin))
			continue;
		if (!best || producer.eventSerial > best->eventSerial)
			best = &producer;
	}
	return best;
}

static void ClassifyIaBufferSkinning(
	std::vector<FlatBufferRow> *buffers,
	const std::vector<DX12FrameResourceBinding> &resourceBindings)
{
	if (!buffers)
		return;

	std::vector<BufferProducer> producers;
	for (const DX12FrameResourceBinding &binding : resourceBindings) {
		if (!BindingIsUavBufferProducer(binding))
			continue;
		BufferProducer producer = ProducerFromBinding(binding);
		if (producer.begin == producer.end)
			continue;
		producers.push_back(std::move(producer));
	}

	for (FlatBufferRow &row : *buffers) {
		if (row.role != "VB") {
			row.skinningClass = "not_applicable";
			continue;
		}

		const BufferProducer *producer = FindLatestProducerForBuffer(row, producers);
		if (producer) {
			row.skinningClass = "gpu_preskinning";
			row.producerEventSerial = producer->eventSerial;
			row.producerDrawId = producer->drawId;
			row.producerDispatchId = producer->dispatchId;
			row.producerPsoIndex = producer->psoIndex;
			row.producerShaderInfo = producer->shaderInfo;
			row.producerBindSpace = producer->bindSpace;
			row.producerRootParameterIndex = producer->rootParameterIndex;
			row.producerShaderRegister = producer->shaderRegister;
		} else if (row.resolved) {
			row.skinningClass = "cpu_preskinning";
		} else {
			row.skinningClass = "unknown";
		}
	}
}

static DX12FrameIaBufferBinding FrameIaBufferFromFlatRow(const FlatBufferRow &row)
{
	DX12FrameIaBufferBinding buffer;
	buffer.bufferId = row.id;
	buffer.role = row.role;
	buffer.skinningClass = row.skinningClass;
	buffer.eventSerial = row.eventSerial;
	buffer.drawId = row.drawId;
	buffer.dispatchId = row.dispatchId;
	buffer.psoIndex = row.psoIndex;
	buffer.shaderInfo = row.shaderInfo;
	buffer.producerEventSerial = row.producerEventSerial;
	buffer.producerDrawId = row.producerDrawId;
	buffer.producerDispatchId = row.producerDispatchId;
	buffer.producerPsoIndex = row.producerPsoIndex;
	buffer.producerShaderInfo = row.producerShaderInfo;
	buffer.producerBindSpace = row.producerBindSpace;
	buffer.producerRootParameterIndex = row.producerRootParameterIndex;
	buffer.producerShaderRegister = row.producerShaderRegister;
	buffer.gpuVa = row.gpuVa;
	buffer.size = row.size;
	buffer.stride = row.stride;
	buffer.slot = row.slot;
	buffer.format = row.format;
	buffer.resolved = row.resolved;
	buffer.resource = row.resource;
	return buffer;
}

void DX12BuildIaBufferFileName(
	const DX12FrameIaBufferBinding &buffer, wchar_t *fileName, size_t fileNameCount)
{
	if (!fileName || fileNameCount == 0)
		return;

	fileName[0] = L'\0';
	const char *role = buffer.role.empty() ? "BUF" : buffer.role.c_str();
	const wchar_t eventKind = buffer.dispatchId ? L'c' : L'd';
	const UINT64 eventId = buffer.dispatchId ? buffer.dispatchId : buffer.drawId;
	wchar_t shaderPart[96];
	BuildShaderHashNamePart(buffer.shaderInfo, shaderPart, ARRAYSIZE(shaderPart));
	swprintf_s(fileName, fileNameCount,
		L"%c%06llu-pso%llu%s-ia_%S_slot%u_va%016llx_size%llu_stride%u_fmt%u.buf",
		eventKind,
		static_cast<unsigned long long>(eventId),
		static_cast<unsigned long long>(buffer.psoIndex),
		shaderPart,
		role,
		buffer.slot,
		static_cast<unsigned long long>(buffer.gpuVa),
		static_cast<unsigned long long>(buffer.size),
		buffer.stride,
		buffer.format);
}

void DX12GetCurrentFrameIaBuffers(std::vector<DX12FrameIaBufferBinding> *buffers)
{
	if (!buffers)
		return;

	std::vector<BindingEvent> events;
	AcquireSRWLockShared(&gBindingLock);
	events = gEvents;
	ReleaseSRWLockShared(&gBindingLock);

	std::vector<FlatBufferRow> rows;
	CollectFrameIaBuffersForEvents(events, &rows);
	std::vector<DX12FrameResourceBinding> resourceBindings;
	DX12GetCurrentFrameResourceBindings(&resourceBindings);
	ClassifyIaBufferSkinning(&rows, resourceBindings);

	buffers->clear();
	buffers->reserve(rows.size());
	for (const FlatBufferRow &row : rows)
		buffers->push_back(FrameIaBufferFromFlatRow(row));

	DX12FrameAnalysisLogInfo("IA buffer stage: events=%zu buffers=%zu\n",
		events.size(), buffers->size());
}

static void WriteOptionalHash(FILE *file, bool hasHash, UINT64 hash)
{
	if (hasHash)
		fprintf(file, "%016llx", static_cast<unsigned long long>(hash));
}

static void WriteFlatFrameAnalysisFiles(
	const wchar_t *dir, const std::vector<BindingEvent> &events, UINT64 droppedEvents)
{
	if (!dir)
		return;

	std::vector<DX12DescriptorSummary> descriptors;
	std::vector<DX12DescriptorHeapSummary> heaps;
	DX12GetResourceMetadataSnapshot(nullptr, &descriptors, nullptr, &heaps);
	std::unordered_map<SIZE_T, const DX12DescriptorSummary*> descriptorsByCpuHandle;
	BuildDescriptorLookup(descriptors, &descriptorsByCpuHandle);

	std::vector<FlatBufferRow> buffers;
	std::unordered_map<std::string, size_t> bufferByKey;
	UINT nextVbId = 0;
	UINT nextIbId = 0;
	size_t drawRows = 0;
	size_t dispatchRows = 0;

	wchar_t summaryDir[MAX_PATH];
	GetSummaryDirectory(dir, summaryDir, ARRAYSIZE(summaryDir));

	wchar_t drawPath[MAX_PATH];
	swprintf_s(drawPath, L"%s\\DrawCallsDX12.csv", summaryDir);
	FILE *drawFile = _wfsopen(drawPath, L"w", _SH_DENYNO);
	if (drawFile) {
		fprintf(drawFile,
			"draw_id,dispatch_id,type,serial,command_list,pipeline_state,pso,vs,ps,cs,topology,"
			"vertex_count,index_count,start_vertex,start_index,base_vertex,instance_count,start_instance,"
			"groups_x,groups_y,groups_z,vb_slots,ib_resource,resource_refs\n");
		for (const BindingEvent &event : events) {
			if (!IsDrawEvent(event) && !IsDispatchEvent(event))
				continue;

			std::string vbSlots = BufferIdsForEvent(
				event, &buffers, &bufferByKey, &nextVbId, &nextIbId);
			std::string ibResource = IndexBufferIdForEvent(
				event, &buffers, &bufferByKey, &nextVbId, &nextIbId);
			std::string resourceRefs = ResourceRefsForEvent(event, heaps, descriptorsByCpuHandle);

			if (IsDrawEvent(event))
				drawRows++;
			else
				dispatchRows++;

			fprintf(drawFile, "%llu,%llu,%s,%llu,%p,%p,%llu,",
				static_cast<unsigned long long>(event.drawId),
				static_cast<unsigned long long>(event.dispatchId),
				event.kind.c_str(),
				static_cast<unsigned long long>(event.serial),
				event.commandList,
				event.pipelineState,
				static_cast<unsigned long long>(event.shaderInfo.psoIndex));
			WriteOptionalHash(drawFile, event.shaderInfo.hasVS, event.shaderInfo.vs);
			fprintf(drawFile, ",");
			WriteOptionalHash(drawFile, event.shaderInfo.hasPS, event.shaderInfo.ps);
			fprintf(drawFile, ",");
			WriteOptionalHash(drawFile, event.shaderInfo.hasCS, event.shaderInfo.cs);
			fprintf(drawFile,
				",%s,%u,%u,%u,%u,%d,%u,%u,%u,%u,%u,%s,%s,%s\n",
				TopologyName(event.primitiveTopology),
				event.vertexCountPerInstance,
				event.indexCountPerInstance,
				event.startVertexLocation,
				event.startIndexLocation,
				event.baseVertexLocation,
				event.instanceCount,
				event.startInstanceLocation,
				event.threadGroupCountX,
				event.threadGroupCountY,
				event.threadGroupCountZ,
				vbSlots.c_str(),
				ibResource.c_str(),
				resourceRefs.c_str());
		}
		fclose(drawFile);
		DX12FrameAnalysisLogInfo("Draw call CSV written: %S draws=%zu dispatches=%zu buffers=%zu\n",
			drawPath, drawRows, dispatchRows, buffers.size());
	}

	wchar_t bufferPath[MAX_PATH];
	swprintf_s(bufferPath, L"%s\\BuffersDX12.csv", summaryDir);
	FILE *bufferFile = _wfsopen(bufferPath, L"w", _SH_DENYNO);
	if (bufferFile) {
		std::vector<DX12FrameResourceBinding> resourceBindings;
		DX12GetCurrentFrameResourceBindings(&resourceBindings);
		ClassifyIaBufferSkinning(&buffers, resourceBindings);
		fprintf(bufferFile,
			"buffer_id,role,resource,file,gpu_va,resource_gpu_va,offset,size,stride,slot,format,"
			"resolved,current_state,has_current_state,heap_type,resource_size,skin_source,"
			"producer_event,producer_draw,producer_dispatch,producer_pso,producer_bind,"
			"producer_root,producer_reg\n");
		for (const FlatBufferRow &row : buffers) {
			UINT64 resourceSize = row.resource.hasResourceDesc ? row.resource.resourceDesc.Width : 0;
			DX12FrameIaBufferBinding iaBuffer = FrameIaBufferFromFlatRow(row);
			wchar_t fileName[MAX_PATH];
			DX12BuildIaBufferFileName(iaBuffer, fileName, ARRAYSIZE(fileName));
			fprintf(bufferFile,
				"%s,%s,%p,%S,0x%llx,0x%llx,%llu,%llu,%u,%u,%u,%u,0x%x,%u,%u,%llu,%s,%llu,%llu,%llu,%llu,%s,%u,%u\n",
				row.id.c_str(),
				row.role.c_str(),
				row.resolved ? row.resource.resource : nullptr,
				fileName,
				static_cast<unsigned long long>(row.gpuVa),
				static_cast<unsigned long long>(row.resolved ? row.resource.gpuVirtualAddress : 0),
				static_cast<unsigned long long>(row.resolved ? row.resource.resourceOffset : 0),
				static_cast<unsigned long long>(row.size),
				row.stride,
				row.slot,
				row.format,
				row.resolved ? 1 : 0,
				row.resolved ? row.resource.currentState : 0,
				row.resolved && row.resource.hasCurrentState ? 1 : 0,
				row.resolved && row.resource.hasResourceHeapType ? row.resource.resourceHeapType : 0,
				static_cast<unsigned long long>(resourceSize),
				row.skinningClass.empty() ? "unknown" : row.skinningClass.c_str(),
				static_cast<unsigned long long>(row.producerEventSerial),
				static_cast<unsigned long long>(row.producerDrawId),
				static_cast<unsigned long long>(row.producerDispatchId),
				static_cast<unsigned long long>(row.producerPsoIndex),
				row.producerBindSpace.empty() ? "-" : row.producerBindSpace.c_str(),
				row.producerRootParameterIndex,
				row.producerShaderRegister);
		}
		fclose(bufferFile);
		DX12FrameAnalysisLogInfo("Buffer CSV written: %S rows=%zu\n", bufferPath, buffers.size());
	}

	wchar_t framePath[MAX_PATH];
	swprintf_s(framePath, L"%s\\FrameAnalysisDX12.csv", summaryDir);
	FILE *frameFile = _wfsopen(framePath, L"w", _SH_DENYNO);
	if (frameFile) {
		fprintf(frameFile,
			"events,dropped,max_events,draw_rows,dispatch_rows,descriptor_heaps,descriptors,buffers,"
			"draw_calls_csv,buffers_csv,current_frame_resources,current_frame_resource_files\n");
		fprintf(frameFile, "%zu,%llu,%u,%zu,%zu,%zu,%zu,%zu,summary\\DrawCallsDX12.csv,summary\\BuffersDX12.csv,summary\\CurrentFrameResourcesDX12.txt,summary\\CurrentFrameResourceFilesDX12.txt\n",
			events.size(),
			static_cast<unsigned long long>(droppedEvents),
			MaxTrackedEvents,
			drawRows,
			dispatchRows,
			heaps.size(),
			descriptors.size(),
			buffers.size());
		fclose(frameFile);
		DX12FrameAnalysisLogInfo("Frame analysis CSV written: %S events=%zu buffers=%zu\n",
			framePath, events.size(), buffers.size());
	}
}

static const DX12DescriptorHeapSummary *FindHeapForGpuHandle(
	const std::vector<DX12DescriptorHeapSummary> &heaps,
	const RootTableState &table,
	UINT requiredType)
{
	for (const DX12DescriptorHeapSummary &heap : heaps) {
		if (heap.type != requiredType || heap.increment == 0 || heap.gpuStart == 0)
			continue;
		const UINT64 begin = heap.gpuStart;
		const UINT64 end = begin + static_cast<UINT64>(heap.numDescriptors) * heap.increment;
		if (table.baseDescriptor.ptr >= begin && table.baseDescriptor.ptr < end)
			return &heap;
	}
	return nullptr;
}

static const DX12DescriptorSummary *FindDescriptorByCpuHandle(
	const std::unordered_map<SIZE_T, const DX12DescriptorSummary*> &descriptorsByCpuHandle,
	SIZE_T cpuHandle)
{
	auto it = descriptorsByCpuHandle.find(cpuHandle);
	return it != descriptorsByCpuHandle.end() ? it->second : nullptr;
}

static void BuildDescriptorLookup(
	const std::vector<DX12DescriptorSummary> &descriptors,
	std::unordered_map<SIZE_T, const DX12DescriptorSummary*> *descriptorsByCpuHandle)
{
	if (!descriptorsByCpuHandle)
		return;

	descriptorsByCpuHandle->clear();
	descriptorsByCpuHandle->reserve(descriptors.size());
	for (const DX12DescriptorSummary &descriptor : descriptors)
		(*descriptorsByCpuHandle)[descriptor.cpuHandle] = &descriptor;
}

static UINT HeapTypeForDescriptorRange(UINT rangeType)
{
	if (rangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER)
		return D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER;
	return D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
}

static bool TableMatchesHeap(const RootTableState &table, const DX12DescriptorHeapSummary &heap)
{
	if (!table.valid || heap.increment == 0 || heap.gpuStart == 0)
		return false;
	const UINT64 begin = heap.gpuStart;
	const UINT64 end = begin + static_cast<UINT64>(heap.numDescriptors) * heap.increment;
	return table.baseDescriptor.ptr >= begin && table.baseDescriptor.ptr < end;
}

static const DX12DescriptorHeapSummary *FindHeapForTableRange(
	const std::vector<DX12DescriptorHeapSummary> &heaps,
	const RootTableState &table,
	const DX12RootDescriptorRangeSummary *range,
	UINT fallbackHeapType)
{
	const UINT requiredType = range ? HeapTypeForDescriptorRange(range->rangeType) : fallbackHeapType;
	for (const DX12DescriptorHeapSummary &heap : heaps) {
		if (heap.type != requiredType || !TableMatchesHeap(table, heap))
			continue;
		return &heap;
	}
	return nullptr;
}

static bool InsertBindingSeen(
	std::unordered_set<std::string> *seen, const BindingEvent &event,
	const char *bindSpace, UINT rootParameterIndex, UINT rangeIndex,
	ID3D12DescriptorHeap *heap, UINT64 descriptorIndex, bool rootDescriptor)
{
	if (!seen)
		return false;

	char key[320];
	sprintf_s(key, "%llu|%s|%u|%u|%p|%llu|%u|%llu",
		static_cast<unsigned long long>(event.shaderInfo.psoIndex),
		bindSpace ? bindSpace : "",
		rootParameterIndex,
		rangeIndex,
		heap,
		static_cast<unsigned long long>(descriptorIndex),
		rootDescriptor ? 1 : 0,
		static_cast<unsigned long long>(event.serial));
	return seen->insert(key).second;
}

static void WriteDescriptorResourceRow(
	FILE *file, const BindingEvent &event, const char *bindSpace,
	UINT rootParameterIndex, UINT rangeIndex, UINT rangeType,
	UINT shaderRegister, UINT registerSpace, UINT descriptorOffset,
	const DX12DescriptorHeapSummary *heap, UINT64 descriptorIndex,
	UINT64 gpuHandle, SIZE_T cpuHandle,
	const DX12DescriptorSummary *descriptor)
{
	if (!file || !heap)
		return;

	fprintf(file, "%llu,%llu,%s,%u,%u,%u,%u,%u,%u,%p,%s,%llu,0x%llx,0x%llx,0x%llx,",
		static_cast<unsigned long long>(event.serial),
		static_cast<unsigned long long>(event.shaderInfo.psoIndex),
		bindSpace ? bindSpace : "",
		rootParameterIndex,
		rangeIndex,
		rangeType,
		shaderRegister,
		registerSpace,
		descriptorOffset,
		heap->heap,
		HeapTypeName(heap->type),
		static_cast<unsigned long long>(descriptorIndex),
		static_cast<unsigned long long>(gpuHandle),
		static_cast<unsigned long long>(cpuHandle),
		static_cast<unsigned long long>(heap->gpuStart));

	if (!descriptor) {
		fprintf(file, "untracked,0,0,0,UNKNOWN,0,0,0,0,0,0,0,0\n");
		return;
	}

	fprintf(file, "%s,%p,%p,%u,",
		descriptor->kind.c_str(),
		descriptor->resource,
		descriptor->counterResource,
		descriptor->hasDesc ? 1 : 0);

	if (descriptor->hasResourceDesc) {
		const D3D12_RESOURCE_DESC &desc = descriptor->resourceDesc;
		fprintf(file, "%s,%llu,%u,%u,%u,%u,0x%llx,0x%llx,%u\n",
			ResourceDimensionName(desc.Dimension),
			static_cast<unsigned long long>(desc.Width),
			desc.Height,
			desc.DepthOrArraySize,
			desc.MipLevels,
			static_cast<UINT>(desc.Format),
			static_cast<unsigned long long>(desc.Flags),
			static_cast<unsigned long long>(descriptor->gpuVirtualAddress),
			descriptor->viewDimension);
	} else {
		fprintf(file, "NONE,0,0,0,0,%u,0x0,0x%llx,%u\n",
			descriptor->viewFormat,
			static_cast<unsigned long long>(descriptor->gpuVirtualAddress),
			descriptor->viewDimension);
	}
}

static void WriteRootDescriptorResourceRow(
	FILE *file, const BindingEvent &event, const char *bindSpace,
	const RootDescriptorState &rootDescriptor,
	const DX12RootParameterSummary *parameter,
	const DX12BufferResourceSummary *resource)
{
	if (!file)
		return;

	const UINT shaderRegister = parameter ? parameter->shaderRegister : UINT_MAX;
	const UINT registerSpace = parameter ? parameter->registerSpace : 0;
	fprintf(file, "%llu,%llu,%s,%u,%u,%u,%u,%u,%u,%p,%s,%llu,0x%llx,0x%llx,0x%llx,",
		static_cast<unsigned long long>(event.serial),
		static_cast<unsigned long long>(event.shaderInfo.psoIndex),
		bindSpace ? bindSpace : "",
		rootDescriptor.rootParameterIndex,
		UINT_MAX,
		static_cast<UINT>(rootDescriptor.type),
		shaderRegister,
		registerSpace,
		0,
		static_cast<void*>(nullptr),
		"ROOT_DESCRIPTOR",
		0ull,
		static_cast<unsigned long long>(rootDescriptor.address),
		0ull,
		0ull);

	if (!resource || !resource->resource) {
		fprintf(file, "root_%u,0,0,0,NONE,0,0,0,0,0,0,0x%llx,0\n",
			static_cast<UINT>(rootDescriptor.type),
			static_cast<unsigned long long>(rootDescriptor.address));
		return;
	}

	const D3D12_RESOURCE_DESC &desc = resource->resourceDesc;
	fprintf(file, "root_%u,%p,0,0,%s,%llu,%u,%u,%u,%u,0x%llx,0x%llx,0\n",
		static_cast<UINT>(rootDescriptor.type),
		resource->resource,
		ResourceDimensionName(desc.Dimension),
		static_cast<unsigned long long>(desc.Width),
		desc.Height,
		desc.DepthOrArraySize,
		desc.MipLevels,
		static_cast<UINT>(desc.Format),
		static_cast<unsigned long long>(desc.Flags),
		static_cast<unsigned long long>(resource->gpuVirtualAddress));
}

static const DX12RootParameterSummary *FindRootParameter(
	const DX12RootSignatureSummary &rootSignature, UINT rootParameterIndex)
{
	for (const DX12RootParameterSummary &parameter : rootSignature.parameters) {
		if (parameter.rootParameterIndex == rootParameterIndex)
			return &parameter;
	}
	return nullptr;
}

static bool IsCompatibleRangeForSpace(const DX12RootDescriptorRangeSummary &range, const char *bindSpace)
{
	if (!bindSpace)
		return true;
	const bool samplerSpace = strstr(bindSpace, "sampler") != nullptr;
	return samplerSpace == (range.rangeType == D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER);
}

static bool GetRootSignatureForEvent(
	const BindingEvent &event, bool compute, DX12RootSignatureSummary *summary)
{
	if (!summary)
		return false;
	ID3D12RootSignature *rootSignature = compute ? event.computeRootSignature : event.graphicsRootSignature;
	if (!rootSignature && event.shaderInfo.psoIndex)
		DX12GetPsoRootSignature(event.shaderInfo.psoIndex, &rootSignature);
	return rootSignature && DX12GetRootSignatureSummary(rootSignature, summary);
}

static void WriteFrameResourceBinding(
	FILE *file, const BindingEvent &event, const char *bindSpace,
	const RootTableState &table, UINT heapType,
	const std::vector<DX12DescriptorHeapSummary> &heaps,
	const std::unordered_map<SIZE_T, const DX12DescriptorSummary*> &descriptorsByCpuHandle,
	std::unordered_set<std::string> *seen)
{
	if (!file || !table.valid || !seen)
		return;

	DX12RootSignatureSummary rootSignature;
	const bool compute = bindSpace && !strncmp(bindSpace, "compute", 7);
	const DX12RootParameterSummary *parameter = nullptr;
	if (GetRootSignatureForEvent(event, compute, &rootSignature))
		parameter = FindRootParameter(rootSignature, table.rootParameterIndex);

	bool wroteRange = false;
	if (parameter && parameter->parameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
		for (const DX12RootDescriptorRangeSummary &range : parameter->ranges) {
			if (!IsCompatibleRangeForSpace(range, bindSpace))
				continue;
			const DX12DescriptorHeapSummary *heap = FindHeapForTableRange(heaps, table, &range, heapType);
			if (!heap)
				continue;
			const UINT64 tableBaseIndex = (table.baseDescriptor.ptr - heap->gpuStart) / heap->increment;
			UINT count = range.numDescriptors == UINT_MAX ? 1 : range.numDescriptors;
			count = min(count, MaxDescriptorsPerRangeDump);
			for (UINT offset = 0; offset < count; ++offset) {
				const UINT64 descriptorIndex = tableBaseIndex + range.effectiveOffset + offset;
				if (descriptorIndex >= heap->numDescriptors)
					break;
				const UINT64 gpuHandle = heap->gpuStart + descriptorIndex * heap->increment;
				const SIZE_T cpuHandle = heap->cpuStart + static_cast<SIZE_T>(descriptorIndex) * heap->increment;
				const DX12DescriptorSummary *descriptor = FindDescriptorByCpuHandle(descriptorsByCpuHandle, cpuHandle);
				if (!InsertBindingSeen(seen, event, bindSpace, table.rootParameterIndex,
						range.rangeIndex, heap->heap, descriptorIndex, false))
					continue;
				WriteDescriptorResourceRow(file, event, bindSpace, table.rootParameterIndex,
					range.rangeIndex, range.rangeType, range.baseShaderRegister + offset,
					range.registerSpace, range.effectiveOffset + offset, heap, descriptorIndex,
					gpuHandle, cpuHandle, descriptor);
				wroteRange = true;
			}
		}
	}

	if (wroteRange)
		return;

	const DX12DescriptorHeapSummary *heap = FindHeapForGpuHandle(heaps, table, heapType);
	if (!heap)
		return;

	const UINT64 descriptorIndex = (table.baseDescriptor.ptr - heap->gpuStart) / heap->increment;
	const SIZE_T cpuHandle = heap->cpuStart + static_cast<SIZE_T>(descriptorIndex) * heap->increment;
	const DX12DescriptorSummary *descriptor = FindDescriptorByCpuHandle(descriptorsByCpuHandle, cpuHandle);
	if (!InsertBindingSeen(seen, event, bindSpace, table.rootParameterIndex,
			UINT_MAX, heap->heap, descriptorIndex, false))
		return;
	WriteDescriptorResourceRow(file, event, bindSpace, table.rootParameterIndex,
		UINT_MAX, UINT_MAX, UINT_MAX, 0, 0, heap, descriptorIndex,
		table.baseDescriptor.ptr, cpuHandle, descriptor);
}

static void WriteFrameRootDescriptor(
	FILE *file, const BindingEvent &event, const char *bindSpace,
	const RootDescriptorState &rootDescriptor,
	std::unordered_set<std::string> *seen)
{
	if (!file || !rootDescriptor.valid || !seen)
		return;

	DX12RootSignatureSummary rootSignature;
	const bool compute = bindSpace && !strncmp(bindSpace, "compute", 7);
	const DX12RootParameterSummary *parameter = nullptr;
	if (GetRootSignatureForEvent(event, compute, &rootSignature))
		parameter = FindRootParameter(rootSignature, rootDescriptor.rootParameterIndex);

	if (!InsertBindingSeen(seen, event, bindSpace, rootDescriptor.rootParameterIndex,
			UINT_MAX, nullptr, rootDescriptor.address, true))
		return;

	DX12BufferResourceSummary resource;
	const bool resolved = DX12ResolveBufferResourceByGpuVa(rootDescriptor.address, 1, &resource);
	WriteRootDescriptorResourceRow(file, event, bindSpace, rootDescriptor,
		parameter, resolved ? &resource : nullptr);
}

static void CollectFrameResourceBinding(
	std::vector<DX12FrameResourceBinding> *bindings,
	const BindingEvent &event, const char *bindSpace,
	const RootTableState &table, UINT heapType,
	const std::vector<DX12DescriptorHeapSummary> &heaps,
	const std::unordered_map<SIZE_T, const DX12DescriptorSummary*> &descriptorsByCpuHandle,
	std::unordered_set<std::string> *seen)
{
	if (!bindings || !table.valid || !seen)
		return;

	DX12RootSignatureSummary rootSignature;
	const bool compute = bindSpace && !strncmp(bindSpace, "compute", 7);
	const DX12RootParameterSummary *parameter = nullptr;
	if (GetRootSignatureForEvent(event, compute, &rootSignature))
		parameter = FindRootParameter(rootSignature, table.rootParameterIndex);

	bool collectedRange = false;
	if (parameter && parameter->parameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE) {
		for (const DX12RootDescriptorRangeSummary &range : parameter->ranges) {
			if (!IsCompatibleRangeForSpace(range, bindSpace))
				continue;
			const DX12DescriptorHeapSummary *heap = FindHeapForTableRange(heaps, table, &range, heapType);
			if (!heap)
				continue;
			const UINT64 tableBaseIndex = (table.baseDescriptor.ptr - heap->gpuStart) / heap->increment;
			UINT count = range.numDescriptors == UINT_MAX ? 1 : range.numDescriptors;
			count = min(count, MaxDescriptorsPerRangeDump);
			for (UINT offset = 0; offset < count; ++offset) {
				const UINT64 descriptorIndex = tableBaseIndex + range.effectiveOffset + offset;
				if (descriptorIndex >= heap->numDescriptors)
					break;
				const UINT64 gpuHandle = heap->gpuStart + descriptorIndex * heap->increment;
				const SIZE_T cpuHandle = heap->cpuStart + static_cast<SIZE_T>(descriptorIndex) * heap->increment;
				const DX12DescriptorSummary *descriptor = FindDescriptorByCpuHandle(descriptorsByCpuHandle, cpuHandle);
				if (!InsertBindingSeen(seen, event, bindSpace, table.rootParameterIndex,
						range.rangeIndex, heap->heap, descriptorIndex, false))
					continue;

				DX12FrameResourceBinding binding;
				binding.eventSerial = event.serial;
				binding.drawId = event.drawId;
				binding.dispatchId = event.dispatchId;
				binding.psoIndex = event.shaderInfo.psoIndex;
				binding.shaderInfo = event.shaderInfo;
				binding.bindSpace = bindSpace ? bindSpace : "";
				binding.rootParameterIndex = table.rootParameterIndex;
				binding.rangeIndex = range.rangeIndex;
				binding.rangeType = range.rangeType;
				binding.shaderRegister = range.baseShaderRegister + offset;
				binding.registerSpace = range.registerSpace;
				binding.descriptorOffset = range.effectiveOffset + offset;
				binding.heap = heap->heap;
				binding.heapType = heap->type;
				binding.descriptorIndex = descriptorIndex;
				binding.gpuHandle = gpuHandle;
				binding.cpuHandle = cpuHandle;
				binding.heapGpuStart = heap->gpuStart;
				if (descriptor) {
					binding.descriptor = *descriptor;
					binding.hasDescriptor = true;
				}
				bindings->push_back(std::move(binding));
				collectedRange = true;
			}
		}
	}

	if (collectedRange)
		return;

	const DX12DescriptorHeapSummary *heap = FindHeapForGpuHandle(heaps, table, heapType);
	if (!heap)
		return;

	const UINT64 descriptorIndex = (table.baseDescriptor.ptr - heap->gpuStart) / heap->increment;
	const SIZE_T cpuHandle = heap->cpuStart + static_cast<SIZE_T>(descriptorIndex) * heap->increment;
	const DX12DescriptorSummary *descriptor = FindDescriptorByCpuHandle(descriptorsByCpuHandle, cpuHandle);
	if (!InsertBindingSeen(seen, event, bindSpace, table.rootParameterIndex,
			UINT_MAX, heap->heap, descriptorIndex, false))
		return;

	DX12FrameResourceBinding binding;
	binding.eventSerial = event.serial;
	binding.drawId = event.drawId;
	binding.dispatchId = event.dispatchId;
	binding.psoIndex = event.shaderInfo.psoIndex;
	binding.shaderInfo = event.shaderInfo;
	binding.bindSpace = bindSpace ? bindSpace : "";
	binding.rootParameterIndex = table.rootParameterIndex;
	binding.heap = heap->heap;
	binding.heapType = heap->type;
	binding.descriptorIndex = descriptorIndex;
	binding.gpuHandle = table.baseDescriptor.ptr;
	binding.cpuHandle = cpuHandle;
	binding.heapGpuStart = heap->gpuStart;
	if (descriptor) {
		binding.descriptor = *descriptor;
		binding.hasDescriptor = true;
	}
	bindings->push_back(std::move(binding));
}

static void CollectFrameRootDescriptorBinding(
	std::vector<DX12FrameResourceBinding> *bindings,
	const BindingEvent &event, const char *bindSpace,
	const RootDescriptorState &rootDescriptor,
	std::unordered_set<std::string> *seen)
{
	if (!bindings || !rootDescriptor.valid || !seen)
		return;
	if (!InsertBindingSeen(seen, event, bindSpace, rootDescriptor.rootParameterIndex,
			UINT_MAX, nullptr, rootDescriptor.address, true))
		return;

	DX12RootSignatureSummary rootSignature;
	const bool compute = bindSpace && !strncmp(bindSpace, "compute", 7);
	const DX12RootParameterSummary *parameter = nullptr;
	if (GetRootSignatureForEvent(event, compute, &rootSignature))
		parameter = FindRootParameter(rootSignature, rootDescriptor.rootParameterIndex);

	DX12FrameResourceBinding binding;
	binding.eventSerial = event.serial;
	binding.drawId = event.drawId;
	binding.dispatchId = event.dispatchId;
	binding.psoIndex = event.shaderInfo.psoIndex;
	binding.shaderInfo = event.shaderInfo;
	binding.bindSpace = bindSpace ? bindSpace : "";
	binding.rootParameterIndex = rootDescriptor.rootParameterIndex;
	binding.rangeIndex = UINT_MAX;
	binding.rangeType = static_cast<UINT>(rootDescriptor.type);
	binding.shaderRegister = parameter ? parameter->shaderRegister : UINT_MAX;
	binding.registerSpace = parameter ? parameter->registerSpace : 0;
	binding.rootDescriptor = true;
	binding.gpuVirtualAddress = rootDescriptor.address;
	bindings->push_back(std::move(binding));
}

void DX12GetCurrentFrameResourceBindings(std::vector<DX12FrameResourceBinding> *bindings)
{
	if (!bindings)
		return;

	std::vector<BindingEvent> events;
	AcquireSRWLockShared(&gBindingLock);
	events = gEvents;
	ReleaseSRWLockShared(&gBindingLock);
	DX12FrameAnalysisLogInfo("Binding resources stage: events snapshot=%zu\n", events.size());

	std::vector<DX12DescriptorSummary> descriptors;
	std::vector<DX12DescriptorHeapSummary> heaps;
	DX12FrameAnalysisLogInfo("Binding resources stage: metadata snapshot begin\n");
	DX12GetResourceMetadataSnapshot(nullptr, &descriptors, nullptr, &heaps);
	DX12FrameAnalysisLogInfo("Binding resources stage: metadata snapshot ready descriptors=%zu heaps=%zu\n",
		descriptors.size(), heaps.size());
	std::unordered_map<SIZE_T, const DX12DescriptorSummary*> descriptorsByCpuHandle;
	BuildDescriptorLookup(descriptors, &descriptorsByCpuHandle);
	DX12FrameAnalysisLogInfo("Binding resources stage: descriptor lookup ready entries=%zu\n",
		descriptorsByCpuHandle.size());

	bindings->clear();
	std::unordered_set<std::string> seen;
	for (const BindingEvent &event : events) {
		if (!IsDrawEvent(event) && !IsDispatchEvent(event))
			continue;
		for (UINT i = 0; i < MaxRootParameters; ++i) {
			CollectFrameResourceBinding(bindings, event, "graphics_cbv_srv_uav",
				event.graphicsTables[i], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
				heaps, descriptorsByCpuHandle, &seen);
			CollectFrameResourceBinding(bindings, event, "graphics_sampler",
				event.graphicsTables[i], D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
				heaps, descriptorsByCpuHandle, &seen);
			CollectFrameResourceBinding(bindings, event, "compute_cbv_srv_uav",
				event.computeTables[i], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
				heaps, descriptorsByCpuHandle, &seen);
			CollectFrameResourceBinding(bindings, event, "compute_sampler",
				event.computeTables[i], D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
				heaps, descriptorsByCpuHandle, &seen);
			CollectFrameRootDescriptorBinding(bindings, event, "graphics_root",
				event.graphicsRootDescriptors[i], &seen);
			CollectFrameRootDescriptorBinding(bindings, event, "compute_root",
				event.computeRootDescriptors[i], &seen);
		}
	}
	DX12FrameAnalysisLogInfo("Binding resources stage: collected bindings=%zu unique=%zu\n",
		bindings->size(), seen.size());
}

static void WriteFrameResourceFile(const wchar_t *dir, const std::vector<BindingEvent> &events)
{
	if (!dir)
		return;

	std::vector<DX12DescriptorSummary> descriptors;
	std::vector<DX12DescriptorHeapSummary> heaps;
	DX12FrameAnalysisLogInfo("Frame resource file stage: metadata snapshot begin events=%zu\n", events.size());
	DX12GetResourceMetadataSnapshot(nullptr, &descriptors, nullptr, &heaps);
	DX12FrameAnalysisLogInfo("Frame resource file stage: metadata snapshot ready descriptors=%zu heaps=%zu\n",
		descriptors.size(), heaps.size());
	std::unordered_map<SIZE_T, const DX12DescriptorSummary*> descriptorsByCpuHandle;
	BuildDescriptorLookup(descriptors, &descriptorsByCpuHandle);
	DX12FrameAnalysisLogInfo("Frame resource file stage: descriptor lookup ready entries=%zu\n",
		descriptorsByCpuHandle.size());

	wchar_t path[MAX_PATH];
	wchar_t summaryDir[MAX_PATH];
	GetSummaryDirectory(dir, summaryDir, ARRAYSIZE(summaryDir));
	swprintf_s(path, L"%s\\CurrentFrameResourcesDX12.txt", summaryDir);
	FILE *file = _wfsopen(path, L"w", _SH_DENYNO);
	if (!file)
		return;
	DX12FrameAnalysisLogInfo("Frame resource file stage: file opened %S\n", path);

	fprintf(file, "DX12 Current Frame Resources\n");
	fprintf(file, "============================\n");
	fprintf(file, "events=%zu descriptor_heaps=%zu descriptors=%zu\n\n",
		events.size(), heaps.size(), descriptors.size());
	fprintf(file,
		"event,pso,bind_space,root_param,range_index,range_type,shader_register,space,descriptor_offset,"
		"heap,heap_type,descriptor_index,gpu_handle,cpu_handle,heap_gpu_start,descriptor_kind,resource,"
		"counter_resource,has_view_desc,resource_dimension,width,height,depth_or_array,mips,format,flags,gpu_va,view_dimension\n");

	std::unordered_set<std::string> seen;
	size_t eventRows = 0;
	for (const BindingEvent &event : events) {
		for (UINT i = 0; i < MaxRootParameters; ++i) {
			WriteFrameResourceBinding(file, event, "graphics_cbv_srv_uav",
				event.graphicsTables[i], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
				heaps, descriptorsByCpuHandle, &seen);
			WriteFrameResourceBinding(file, event, "graphics_sampler",
				event.graphicsTables[i], D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
				heaps, descriptorsByCpuHandle, &seen);
			WriteFrameResourceBinding(file, event, "compute_cbv_srv_uav",
				event.computeTables[i], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
				heaps, descriptorsByCpuHandle, &seen);
			WriteFrameResourceBinding(file, event, "compute_sampler",
				event.computeTables[i], D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
				heaps, descriptorsByCpuHandle, &seen);
			WriteFrameRootDescriptor(file, event, "graphics_root",
				event.graphicsRootDescriptors[i], &seen);
			WriteFrameRootDescriptor(file, event, "compute_root",
				event.computeRootDescriptors[i], &seen);
		}
		eventRows++;
	}
	DX12FrameAnalysisLogInfo("Frame resource file stage: events processed=%zu bindings=%zu\n",
		eventRows, seen.size());

	fclose(file);
	DX12FrameAnalysisLogInfo("Current-frame resources written: %S bindings=%zu heaps=%zu descriptors=%zu\n",
		path, seen.size(), heaps.size(), descriptors.size());
}

void DX12DumpBindingTrace(const wchar_t *dir)
{
	if (!dir)
		return;

	std::vector<BindingEvent> events;
	UINT64 droppedEvents = 0;
	AcquireSRWLockShared(&gBindingLock);
	events = gEvents;
	droppedEvents = gDroppedEvents;
	ReleaseSRWLockShared(&gBindingLock);

	DX12FrameAnalysisLogInfo(
		"DX12 binding trace begin: events=%zu dropped=%llu max_events=%u\n",
		events.size(), static_cast<unsigned long long>(droppedEvents),
		MaxTrackedEvents);
	for (const BindingEvent &event : events) {
		if (IsDrawEvent(event) || IsDispatchEvent(event)) {
			DX12FrameAnalysisManifestWriteCall(
				event.kind.c_str(),
				event.serial,
				event.drawId,
				event.dispatchId,
				event.commandList,
				event.pipelineState,
				event.shaderInfo,
				event.primitiveTopology,
				event.vertexCountPerInstance,
				event.indexCountPerInstance,
				event.startVertexLocation,
				event.startIndexLocation,
				event.baseVertexLocation,
				event.instanceCount,
				event.startInstanceLocation,
				event.threadGroupCountX,
				event.threadGroupCountY,
				event.threadGroupCountZ,
				event.indexBufferValid,
				event.indexBuffer.BufferLocation,
				event.indexBuffer.SizeInBytes,
				event.indexBuffer.Format);
		}

		char vs[32], ps[32], cs[32];
		FormatOptionalHashText(event.shaderInfo.hasVS, event.shaderInfo.vs, vs, ARRAYSIZE(vs));
		FormatOptionalHashText(event.shaderInfo.hasPS, event.shaderInfo.ps, ps, ARRAYSIZE(ps));
		FormatOptionalHashText(event.shaderInfo.hasCS, event.shaderInfo.cs, cs, ARRAYSIZE(cs));
		DX12FrameAnalysisLogInfo(
			"event=%llu kind=%s draw=%llu dispatch=%llu command_list=%p pso=%llu vs=%s ps=%s cs=%s topology=%s vertices=%u indices=%u instances=%u start_vertex=%u start_index=%u base_vertex=%d start_instance=%u groups=%u,%u,%u ib_va=0x%llx ib_size=%u ib_format=%u cbv_srv_uav_heap=%p sampler_heap=%p\n",
			static_cast<unsigned long long>(event.serial),
			event.kind.c_str(),
			static_cast<unsigned long long>(event.drawId),
			static_cast<unsigned long long>(event.dispatchId),
			event.commandList,
			static_cast<unsigned long long>(event.shaderInfo.psoIndex),
			vs, ps, cs,
			TopologyName(event.primitiveTopology),
			event.vertexCountPerInstance,
			event.indexCountPerInstance,
			event.instanceCount,
			event.startVertexLocation,
			event.startIndexLocation,
			event.baseVertexLocation,
			event.startInstanceLocation,
			event.threadGroupCountX,
			event.threadGroupCountY,
			event.threadGroupCountZ,
			static_cast<unsigned long long>(event.indexBufferValid ? event.indexBuffer.BufferLocation : 0),
			event.indexBufferValid ? event.indexBuffer.SizeInBytes : 0,
			event.indexBufferValid ? static_cast<UINT>(event.indexBuffer.Format) : 0,
			event.cbvSrvUavHeap,
			event.samplerHeap);
	}

	DX12FrameAnalysisLogInfo("DX12 binding trace end: events=%zu dropped=%llu\n",
		events.size(), static_cast<unsigned long long>(droppedEvents));
}
