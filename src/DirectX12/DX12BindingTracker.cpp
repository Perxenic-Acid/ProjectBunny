#include "DX12BindingTracker.h"

#include <stdio.h>
#include <string.h>

#include <unordered_set>
#include <string>
#include <unordered_map>
#include <vector>

#include "DX12ResourceTracker.h"
#include "DX12ShaderDump.h"
#include "DX12State.h"

static constexpr UINT MaxRootParameters = 64;
static constexpr UINT MaxVertexBufferSlots = 32;
static constexpr UINT MaxTrackedEvents = 20000;

struct RootTableState
{
	bool valid = false;
	UINT rootParameterIndex = 0;
	D3D12_GPU_DESCRIPTOR_HANDLE baseDescriptor = {};
};

struct CommandListBindingState
{
	ID3D12PipelineState *pipelineState = nullptr;
	ID3D12DescriptorHeap *cbvSrvUavHeap = nullptr;
	ID3D12DescriptorHeap *samplerHeap = nullptr;
	RootTableState graphicsTables[MaxRootParameters] = {};
	RootTableState computeTables[MaxRootParameters] = {};
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
	DX12PsoShaderInfo shaderInfo = {};
	ID3D12DescriptorHeap *cbvSrvUavHeap = nullptr;
	ID3D12DescriptorHeap *samplerHeap = nullptr;
	RootTableState graphicsTables[MaxRootParameters] = {};
	RootTableState computeTables[MaxRootParameters] = {};
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
	event.drawId = state.drawSerial;
	event.dispatchId = state.dispatchSerial;
	event.commandList = commandList;
	event.pipelineState = state.pipelineState;
	event.cbvSrvUavHeap = state.cbvSrvUavHeap;
	event.samplerHeap = state.samplerHeap;
	memcpy(event.graphicsTables, state.graphicsTables, sizeof(event.graphicsTables));
	memcpy(event.computeTables, state.computeTables, sizeof(event.computeTables));
	memcpy(event.vertexBuffers, state.vertexBuffers, sizeof(event.vertexBuffers));
	memcpy(event.vertexBufferValid, state.vertexBufferValid, sizeof(event.vertexBufferValid));
	event.indexBuffer = state.indexBuffer;
	event.indexBufferValid = state.indexBufferValid;
	event.primitiveTopology = state.primitiveTopology;
	if (state.pipelineState)
		DX12GetPipelineStateShaderInfo(state.pipelineState, &event.shaderInfo);
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
	state.drawSerial++;
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
	state.drawSerial++;
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
	state.dispatchSerial++;
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
	gDroppedEvents = 0;
	ReleaseSRWLockExclusive(&gBindingLock);
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
	const std::vector<DX12DescriptorSummary> &descriptors, SIZE_T cpuHandle);

static std::string ResourceRefsForEvent(
	const BindingEvent &event,
	const std::vector<DX12DescriptorHeapSummary> &heaps,
	const std::vector<DX12DescriptorSummary> &descriptors)
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
			const DX12DescriptorSummary *descriptor = FindDescriptorByCpuHandle(descriptors, cpuHandle);
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
	UINT64 gpuVa = 0;
	UINT64 size = 0;
	UINT stride = 0;
	UINT slot = 0;
	UINT format = 0;
	bool resolved = false;
	DX12BufferResourceSummary resource = {};
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
	UINT *nextVbId, UINT *nextIbId, const char *role, UINT64 gpuVa, UINT64 size,
	UINT stride, UINT slot, UINT format)
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
			"VB", view.BufferLocation, view.SizeInBytes, view.StrideInBytes, slot, 0);
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
		"IB", event.indexBuffer.BufferLocation, event.indexBuffer.SizeInBytes, 0, 0,
		static_cast<UINT>(event.indexBuffer.Format));
	return row ? row->id : "";
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

	std::vector<FlatBufferRow> buffers;
	std::unordered_map<std::string, size_t> bufferByKey;
	UINT nextVbId = 0;
	UINT nextIbId = 0;
	size_t drawRows = 0;
	size_t dispatchRows = 0;

	wchar_t drawPath[MAX_PATH];
	swprintf_s(drawPath, L"%s\\DrawCallsDX12.csv", dir);
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
			std::string resourceRefs = ResourceRefsForEvent(event, heaps, descriptors);

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
		DX12Log("Draw call CSV written: %S draws=%zu dispatches=%zu buffers=%zu\n",
			drawPath, drawRows, dispatchRows, buffers.size());
	}

	wchar_t bufferPath[MAX_PATH];
	swprintf_s(bufferPath, L"%s\\BuffersDX12.csv", dir);
	FILE *bufferFile = _wfsopen(bufferPath, L"w", _SH_DENYNO);
	if (bufferFile) {
		fprintf(bufferFile,
			"buffer_id,role,resource,file,gpu_va,resource_gpu_va,offset,size,stride,slot,format,"
			"resolved,current_state,has_current_state,heap_type,resource_size\n");
		for (const FlatBufferRow &row : buffers) {
			UINT64 resourceSize = row.resource.hasResourceDesc ? row.resource.resourceDesc.Width : 0;
			fprintf(bufferFile,
				"%s,%s,%p,,0x%llx,0x%llx,%llu,%llu,%u,%u,%u,%u,0x%x,%u,%u,%llu\n",
				row.id.c_str(),
				row.role.c_str(),
				row.resolved ? row.resource.resource : nullptr,
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
				static_cast<unsigned long long>(resourceSize));
		}
		fclose(bufferFile);
		DX12Log("Buffer CSV written: %S rows=%zu\n", bufferPath, buffers.size());
	}

	wchar_t framePath[MAX_PATH];
	swprintf_s(framePath, L"%s\\FrameAnalysisDX12.csv", dir);
	FILE *frameFile = _wfsopen(framePath, L"w", _SH_DENYNO);
	if (frameFile) {
		fprintf(frameFile,
			"events,dropped,max_events,draw_rows,dispatch_rows,descriptor_heaps,descriptors,buffers,"
			"draw_calls_csv,buffers_csv,current_frame_resources,current_frame_resource_files\n");
		fprintf(frameFile, "%zu,%llu,%u,%zu,%zu,%zu,%zu,%zu,DrawCallsDX12.csv,BuffersDX12.csv,CurrentFrameResourcesDX12.txt,CurrentFrameResourceFilesDX12.txt\n",
			events.size(),
			static_cast<unsigned long long>(droppedEvents),
			MaxTrackedEvents,
			drawRows,
			dispatchRows,
			heaps.size(),
			descriptors.size(),
			buffers.size());
		fclose(frameFile);
		DX12Log("Frame analysis CSV written: %S events=%zu buffers=%zu\n",
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
	const std::vector<DX12DescriptorSummary> &descriptors, SIZE_T cpuHandle)
{
	for (const DX12DescriptorSummary &descriptor : descriptors) {
		if (descriptor.cpuHandle == cpuHandle)
			return &descriptor;
	}
	return nullptr;
}

static void WriteFrameResourceBinding(
	FILE *file, const BindingEvent &event, const char *bindSpace,
	const RootTableState &table, UINT heapType,
	const std::vector<DX12DescriptorHeapSummary> &heaps,
	const std::vector<DX12DescriptorSummary> &descriptors,
	std::unordered_set<std::string> *seen)
{
	if (!file || !table.valid || !seen)
		return;

	const DX12DescriptorHeapSummary *heap = FindHeapForGpuHandle(heaps, table, heapType);
	if (!heap)
		return;

	const UINT64 descriptorIndex = (table.baseDescriptor.ptr - heap->gpuStart) / heap->increment;
	const SIZE_T cpuHandle = heap->cpuStart + static_cast<SIZE_T>(descriptorIndex) * heap->increment;
	const DX12DescriptorSummary *descriptor = FindDescriptorByCpuHandle(descriptors, cpuHandle);

	char key[256];
	sprintf_s(key, "%llu|%s|%u|%p|%llu",
		static_cast<unsigned long long>(event.shaderInfo.psoIndex),
		bindSpace,
		table.rootParameterIndex,
		heap->heap,
		static_cast<unsigned long long>(descriptorIndex));
	if (!seen->insert(key).second)
		return;

	fprintf(file, "%llu,%s,%u,%p,%s,%llu,0x%llx,0x%llx,0x%llx,",
		static_cast<unsigned long long>(event.shaderInfo.psoIndex),
		bindSpace,
		table.rootParameterIndex,
		heap->heap,
		HeapTypeName(heap->type),
		static_cast<unsigned long long>(descriptorIndex),
		static_cast<unsigned long long>(table.baseDescriptor.ptr),
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

static void CollectFrameResourceBinding(
	std::vector<DX12FrameResourceBinding> *bindings,
	const BindingEvent &event, const char *bindSpace,
	const RootTableState &table, UINT heapType,
	const std::vector<DX12DescriptorHeapSummary> &heaps,
	const std::vector<DX12DescriptorSummary> &descriptors,
	std::unordered_set<std::string> *seen)
{
	if (!bindings || !table.valid || !seen)
		return;

	const DX12DescriptorHeapSummary *heap = FindHeapForGpuHandle(heaps, table, heapType);
	if (!heap)
		return;

	const UINT64 descriptorIndex = (table.baseDescriptor.ptr - heap->gpuStart) / heap->increment;
	const SIZE_T cpuHandle = heap->cpuStart + static_cast<SIZE_T>(descriptorIndex) * heap->increment;
	const DX12DescriptorSummary *descriptor = FindDescriptorByCpuHandle(descriptors, cpuHandle);

	char key[256];
	sprintf_s(key, "%llu|%s|%u|%p|%llu",
		static_cast<unsigned long long>(event.shaderInfo.psoIndex),
		bindSpace,
		table.rootParameterIndex,
		heap->heap,
		static_cast<unsigned long long>(descriptorIndex));
	if (!seen->insert(key).second)
		return;

	DX12FrameResourceBinding binding;
	binding.psoIndex = event.shaderInfo.psoIndex;
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

void DX12GetCurrentFrameResourceBindings(std::vector<DX12FrameResourceBinding> *bindings)
{
	if (!bindings)
		return;

	std::vector<BindingEvent> events;
	AcquireSRWLockShared(&gBindingLock);
	events = gEvents;
	ReleaseSRWLockShared(&gBindingLock);
	DX12Log("Binding resources stage: events snapshot=%zu\n", events.size());

	std::vector<DX12DescriptorSummary> descriptors;
	std::vector<DX12DescriptorHeapSummary> heaps;
	DX12Log("Binding resources stage: metadata snapshot begin\n");
	DX12GetResourceMetadataSnapshot(nullptr, &descriptors, nullptr, &heaps);
	DX12Log("Binding resources stage: metadata snapshot ready descriptors=%zu heaps=%zu\n",
		descriptors.size(), heaps.size());

	bindings->clear();
	std::unordered_set<std::string> seen;
	for (const BindingEvent &event : events) {
		for (UINT i = 0; i < MaxRootParameters; ++i) {
			CollectFrameResourceBinding(bindings, event, "graphics_cbv_srv_uav",
				event.graphicsTables[i], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
				heaps, descriptors, &seen);
			CollectFrameResourceBinding(bindings, event, "graphics_sampler",
				event.graphicsTables[i], D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
				heaps, descriptors, &seen);
			CollectFrameResourceBinding(bindings, event, "compute_cbv_srv_uav",
				event.computeTables[i], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
				heaps, descriptors, &seen);
			CollectFrameResourceBinding(bindings, event, "compute_sampler",
				event.computeTables[i], D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
				heaps, descriptors, &seen);
		}
	}
	DX12Log("Binding resources stage: collected bindings=%zu unique=%zu\n",
		bindings->size(), seen.size());
}

static void WriteFrameResourceFile(const wchar_t *dir, const std::vector<BindingEvent> &events)
{
	if (!dir)
		return;

	std::vector<DX12DescriptorSummary> descriptors;
	std::vector<DX12DescriptorHeapSummary> heaps;
	DX12Log("Frame resource file stage: metadata snapshot begin events=%zu\n", events.size());
	DX12GetResourceMetadataSnapshot(nullptr, &descriptors, nullptr, &heaps);
	DX12Log("Frame resource file stage: metadata snapshot ready descriptors=%zu heaps=%zu\n",
		descriptors.size(), heaps.size());

	wchar_t path[MAX_PATH];
	swprintf_s(path, L"%s\\CurrentFrameResourcesDX12.txt", dir);
	FILE *file = _wfsopen(path, L"w", _SH_DENYNO);
	if (!file)
		return;
	DX12Log("Frame resource file stage: file opened %S\n", path);

	fprintf(file, "DX12 Current Frame Resources\n");
	fprintf(file, "============================\n");
	fprintf(file, "events=%zu descriptor_heaps=%zu descriptors=%zu\n\n",
		events.size(), heaps.size(), descriptors.size());
	fprintf(file,
		"pso,bind_space,root_param,heap,heap_type,descriptor_index,gpu_handle,cpu_handle,heap_gpu_start,descriptor_kind,resource,counter_resource,has_view_desc,resource_dimension,width,height,depth_or_array,mips,format,flags,gpu_va,view_dimension\n");

	std::unordered_set<std::string> seen;
	size_t eventRows = 0;
	for (const BindingEvent &event : events) {
		for (UINT i = 0; i < MaxRootParameters; ++i) {
			WriteFrameResourceBinding(file, event, "graphics_cbv_srv_uav",
				event.graphicsTables[i], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
				heaps, descriptors, &seen);
			WriteFrameResourceBinding(file, event, "graphics_sampler",
				event.graphicsTables[i], D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
				heaps, descriptors, &seen);
			WriteFrameResourceBinding(file, event, "compute_cbv_srv_uav",
				event.computeTables[i], D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV,
				heaps, descriptors, &seen);
			WriteFrameResourceBinding(file, event, "compute_sampler",
				event.computeTables[i], D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER,
				heaps, descriptors, &seen);
		}
		eventRows++;
	}
	DX12Log("Frame resource file stage: events processed=%zu bindings=%zu\n",
		eventRows, seen.size());

	fclose(file);
	DX12Log("Current-frame resources written: %S bindings=%zu heaps=%zu descriptors=%zu\n",
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

	wchar_t path[MAX_PATH];
	swprintf_s(path, L"%s\\BindingTraceDX12.txt", dir);
	FILE *file = _wfsopen(path, L"w", _SH_DENYNO);
	if (!file)
		return;

	fprintf(file, "DX12 Current Frame Binding Trace\n");
	fprintf(file, "================================\n");
	fprintf(file, "events=%zu dropped=%llu max_events=%u\n\n",
		events.size(),
		static_cast<unsigned long long>(droppedEvents),
		MaxTrackedEvents);

	fprintf(file,
		"serial,kind,draw_id,dispatch_id,command_list,pipeline_state,pso,vs,ps,cs,topology,vertex_count,index_count,start_vertex,start_index,base_vertex,instance_count,start_instance,groups_x,groups_y,groups_z,vb_slots,ib_gpu_va,ib_size,ib_format,cbv_srv_uav_heap,sampler_heap,graphics_tables,compute_tables\n");
	for (const BindingEvent &event : events) {
		fprintf(file, "%llu,%s,%llu,%llu,%p,%p,",
			static_cast<unsigned long long>(event.serial),
			event.kind.c_str(),
			static_cast<unsigned long long>(event.drawId),
			static_cast<unsigned long long>(event.dispatchId),
			event.commandList,
			event.pipelineState);
		WriteShaderInfo(file, event.shaderInfo);
		fprintf(file, ",%s,%u,%u,%u,%u,%d,%u,%u,%u,%u,%u,",
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
			event.threadGroupCountZ);
		WriteVertexBufferSlots(file, event);
		fprintf(file, ",");
		if (event.indexBufferValid) {
			fprintf(file, "0x%llx,%u,%u",
				static_cast<unsigned long long>(event.indexBuffer.BufferLocation),
				event.indexBuffer.SizeInBytes,
				static_cast<UINT>(event.indexBuffer.Format));
		} else {
			fprintf(file, ",,");
		}
		fprintf(file, ",%p,%p,",
			event.cbvSrvUavHeap,
			event.samplerHeap);
		WriteRootTables(file, event.graphicsTables);
		fprintf(file, ",");
		WriteRootTables(file, event.computeTables);
		fprintf(file, "\n");
	}

	fclose(file);
	DX12Log("Current-frame binding trace written: %S events=%zu dropped=%llu\n",
		path, events.size(), static_cast<unsigned long long>(droppedEvents));
	WriteFlatFrameAnalysisFiles(dir, events, droppedEvents);
	WriteFrameResourceFile(dir, events);
}
