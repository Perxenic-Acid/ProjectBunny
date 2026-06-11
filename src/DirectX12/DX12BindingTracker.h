#pragma once

#include <Windows.h>
#include <d3d12.h>

#include <string>
#include <vector>

#include "DX12ResourceTracker.h"
#include "DX12ShaderDump.h"

struct DX12FrameResourceBinding
{
	UINT64 psoIndex = 0;
	std::string bindSpace;
	UINT rootParameterIndex = 0;
	ID3D12DescriptorHeap *heap = nullptr;
	UINT heapType = 0;
	UINT64 descriptorIndex = 0;
	UINT64 gpuHandle = 0;
	SIZE_T cpuHandle = 0;
	UINT64 heapGpuStart = 0;
	DX12DescriptorSummary descriptor;
	bool hasDescriptor = false;
};

struct DX12FrameIaBufferBinding
{
	std::string bufferId;
	std::string role;
	UINT64 gpuVa = 0;
	UINT64 size = 0;
	UINT stride = 0;
	UINT slot = 0;
	UINT format = 0;
	bool resolved = false;
	DX12BufferResourceSummary resource;
};

void DX12BindingRegisterCommandList(ID3D12GraphicsCommandList *commandList);
void DX12BindingResetCommandList(
	ID3D12GraphicsCommandList *commandList, ID3D12PipelineState *initialState);
void DX12BindingSetPipelineState(
	ID3D12GraphicsCommandList *commandList, ID3D12PipelineState *pipelineState);
void DX12BindingRecordStateEvent(ID3D12GraphicsCommandList *commandList, const char *kind);
void DX12BindingSetDescriptorHeaps(
	ID3D12GraphicsCommandList *commandList, UINT count,
	ID3D12DescriptorHeap *const *heaps);
void DX12BindingSetGraphicsRootDescriptorTable(
	ID3D12GraphicsCommandList *commandList, UINT rootParameterIndex,
	D3D12_GPU_DESCRIPTOR_HANDLE baseDescriptor);
void DX12BindingSetComputeRootDescriptorTable(
	ID3D12GraphicsCommandList *commandList, UINT rootParameterIndex,
	D3D12_GPU_DESCRIPTOR_HANDLE baseDescriptor);
void DX12BindingSetPrimitiveTopology(
	ID3D12GraphicsCommandList *commandList, D3D12_PRIMITIVE_TOPOLOGY topology);
void DX12BindingSetIndexBuffer(
	ID3D12GraphicsCommandList *commandList, const D3D12_INDEX_BUFFER_VIEW *view);
void DX12BindingSetVertexBuffers(
	ID3D12GraphicsCommandList *commandList, UINT startSlot, UINT count,
	const D3D12_VERTEX_BUFFER_VIEW *views);
void DX12BindingRecordDrawInstanced(
	ID3D12GraphicsCommandList *commandList, UINT vertexCountPerInstance,
	UINT instanceCount, UINT startVertexLocation, UINT startInstanceLocation);
void DX12BindingRecordDrawIndexedInstanced(
	ID3D12GraphicsCommandList *commandList, UINT indexCountPerInstance,
	UINT instanceCount, UINT startIndexLocation, INT baseVertexLocation,
	UINT startInstanceLocation);
void DX12BindingRecordDispatch(
	ID3D12GraphicsCommandList *commandList, UINT threadGroupCountX,
	UINT threadGroupCountY, UINT threadGroupCountZ);
void DX12BindingBeginFrame();
void DX12GetCurrentFrameShaderInfos(std::vector<DX12PsoShaderInfo> *shaderInfos);
void DX12GetCurrentFrameResourceBindings(std::vector<DX12FrameResourceBinding> *bindings);
void DX12GetCurrentFrameIaBuffers(std::vector<DX12FrameIaBufferBinding> *buffers);
void DX12BuildIaBufferFileName(
	const DX12FrameIaBufferBinding &buffer, wchar_t *fileName, size_t fileNameCount);
void DX12DumpBindingTrace(const wchar_t *dir);
