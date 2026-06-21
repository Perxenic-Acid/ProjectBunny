#include "DX12FrameAnalysisManifest.h"

#include <stdio.h>
#include <string.h>
#include <wchar.h>

#include "DX12FrameAnalysis.h"
#include "DX12ShaderDump.h"

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

static const char *DxgiFormatName(UINT format)
{
	switch (format) {
	case DXGI_FORMAT_UNKNOWN:
		return "DXGI_FORMAT_UNKNOWN";
	case DXGI_FORMAT_R16_UINT:
		return "DXGI_FORMAT_R16_UINT";
	case DXGI_FORMAT_R32_UINT:
		return "DXGI_FORMAT_R32_UINT";
	case DXGI_FORMAT_R32_FLOAT:
		return "DXGI_FORMAT_R32_FLOAT";
	case DXGI_FORMAT_R32G32_FLOAT:
		return "DXGI_FORMAT_R32G32_FLOAT";
	case DXGI_FORMAT_R32G32B32_FLOAT:
		return "DXGI_FORMAT_R32G32B32_FLOAT";
	case DXGI_FORMAT_R32G32B32A32_FLOAT:
		return "DXGI_FORMAT_R32G32B32A32_FLOAT";
	case DXGI_FORMAT_R16G16_FLOAT:
		return "DXGI_FORMAT_R16G16_FLOAT";
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
		return "DXGI_FORMAT_R16G16B16A16_FLOAT";
	case DXGI_FORMAT_R8G8B8A8_UNORM:
		return "DXGI_FORMAT_R8G8B8A8_UNORM";
	case DXGI_FORMAT_BC1_UNORM:
		return "DXGI_FORMAT_BC1_UNORM";
	case DXGI_FORMAT_BC2_UNORM:
		return "DXGI_FORMAT_BC2_UNORM";
	case DXGI_FORMAT_BC3_UNORM:
		return "DXGI_FORMAT_BC3_UNORM";
	case DXGI_FORMAT_BC4_UNORM:
		return "DXGI_FORMAT_BC4_UNORM";
	case DXGI_FORMAT_BC5_UNORM:
		return "DXGI_FORMAT_BC5_UNORM";
	case DXGI_FORMAT_BC6H_UF16:
		return "DXGI_FORMAT_BC6H_UF16";
	case DXGI_FORMAT_BC7_UNORM:
		return "DXGI_FORMAT_BC7_UNORM";
	default:
		return "DXGI_FORMAT_OTHER";
	}
}

static void FormatShaderHash(UINT64 hash, bool hasHash, char *text, size_t textCount)
{
	if (!text || textCount == 0)
		return;
	if (hasHash)
		sprintf_s(text, textCount, "%016llx", static_cast<unsigned long long>(hash));
	else
		sprintf_s(text, textCount, "-");
}

static void FormatTextPath(const wchar_t *filePath, wchar_t *textPath, size_t textPathCount)
{
	if (!textPath || textPathCount == 0)
		return;
	textPath[0] = L'\0';
	if (!filePath || !filePath[0])
		return;
	wcsncpy_s(textPath, textPathCount, filePath, _TRUNCATE);
	wcscat_s(textPath, textPathCount, L".txt");
}

static void FormatFileHash(const wchar_t *filePath, char *hash, size_t hashCount)
{
	if (!hash || hashCount == 0)
		return;
	hash[0] = '\0';
	if (!filePath || !filePath[0])
		return;

	const wchar_t *name = wcsrchr(filePath, L'\\');
	name = name ? name + 1 : filePath;
	size_t i = 0;
	for (; i < hashCount - 1 && i < 8 && name[i] && name[i] != L'-'; ++i)
		hash[i] = static_cast<char>(name[i]);
	hash[i] = '\0';
}

void DX12FrameAnalysisManifestWriteCall(
	const char *functionName, UINT64 eventSerial, UINT64 drawId, UINT64 dispatchId,
	ID3D12GraphicsCommandList *commandList, ID3D12PipelineState *pipelineState,
	const DX12PsoShaderInfo &shaderInfo, D3D12_PRIMITIVE_TOPOLOGY topology,
	UINT vertexCountPerInstance, UINT indexCountPerInstance, UINT startVertexLocation,
	UINT startIndexLocation, INT baseVertexLocation, UINT instanceCount,
	UINT startInstanceLocation, UINT threadGroupCountX, UINT threadGroupCountY,
	UINT threadGroupCountZ, bool indexBufferValid, D3D12_GPU_VIRTUAL_ADDRESS indexBufferGpuVa,
	UINT indexBufferSize, DXGI_FORMAT indexBufferFormat)
{
	char vs[32], ps[32], cs[32];
	FormatShaderHash(shaderInfo.vs, shaderInfo.hasVS, vs, ARRAYSIZE(vs));
	FormatShaderHash(shaderInfo.ps, shaderInfo.hasPS, ps, ARRAYSIZE(ps));
	FormatShaderHash(shaderInfo.cs, shaderInfo.hasCS, cs, ARRAYSIZE(cs));

	const bool isDispatch = functionName && !strcmp(functionName, "dispatch");
	DX12FrameAnalysisLogEvent(
		"%s function=%s event=%llu draw=%llu dispatch=%llu cmdlist=%p pipeline_state=%p "
		"pso=%llu vs=%s ps=%s cs=%s topology=%s vertex_count=%u index_count=%u "
		"start_vertex=%u start_index=%u base_vertex=%d instance_count=%u "
		"start_instance=%u groups_x=%u groups_y=%u groups_z=%u ib_valid=%u "
		"ib_gpu=0x%llx ib_bytes=%u ib_fmt=%u\n",
		isDispatch ? "call.dispatch" : "call.draw",
		functionName ? functionName : "",
		static_cast<unsigned long long>(eventSerial),
		static_cast<unsigned long long>(drawId),
		static_cast<unsigned long long>(dispatchId),
		commandList,
		pipelineState,
		static_cast<unsigned long long>(shaderInfo.psoIndex),
		vs, ps, cs,
		TopologyName(topology),
		vertexCountPerInstance,
		indexCountPerInstance,
		startVertexLocation,
		startIndexLocation,
		baseVertexLocation,
		instanceCount,
		startInstanceLocation,
		threadGroupCountX,
		threadGroupCountY,
		threadGroupCountZ,
		indexBufferValid ? 1 : 0,
		static_cast<unsigned long long>(indexBufferValid ? indexBufferGpuVa : 0),
		indexBufferValid ? indexBufferSize : 0,
		indexBufferValid ? static_cast<UINT>(indexBufferFormat) : 0);
}

void DX12FrameAnalysisManifestWriteFileDump(
	const wchar_t *filePath, bool isTexture, UINT64 bytes, const char *status,
	const char *note)
{
	char hash[16];
	wchar_t textPath[MAX_PATH];
	FormatFileHash(filePath, hash, ARRAYSIZE(hash));
	FormatTextPath(filePath, textPath, ARRAYSIZE(textPath));

	DX12FrameAnalysisLogEvent(
		"file.dump status=%s kind=%s file=%S text=%S hash=%s bytes=%llu note=%s\n",
		status ? status : "",
		isTexture ? "texture" : "buffer",
		filePath ? filePath : L"",
		isTexture ? L"" : textPath,
		hash,
		static_cast<unsigned long long>(bytes),
		note ? note : "");
}

void DX12FrameAnalysisManifestWriteIaBinding(
	const DX12FrameIaBufferBinding &buffer,
	const D3D12_RESOURCE_DESC &desc, UINT64 sourceOffset, UINT64 copyBytes,
	D3D12_RESOURCE_STATES sourceState, bool hasCurrentState,
	const wchar_t *filePath)
{
	char vs[32], ps[32], cs[32], hash[16];
	char producerCs[32];
	FormatShaderHash(buffer.shaderInfo.vs, buffer.shaderInfo.hasVS, vs, ARRAYSIZE(vs));
	FormatShaderHash(buffer.shaderInfo.ps, buffer.shaderInfo.hasPS, ps, ARRAYSIZE(ps));
	FormatShaderHash(buffer.shaderInfo.cs, buffer.shaderInfo.hasCS, cs, ARRAYSIZE(cs));
	FormatShaderHash(buffer.producerShaderInfo.cs, buffer.producerShaderInfo.hasCS,
		producerCs, ARRAYSIZE(producerCs));
	FormatFileHash(filePath, hash, ARRAYSIZE(hash));

	DX12FrameAnalysisLogEvent(
		"bind.ia event=%llu draw=%llu dispatch=%llu pso=%llu "
		"vs=%s ps=%s cs=%s role=%s slot=%u resource=%p dim=%s gpu=0x%llx "
		"offset=%llu bytes=%llu stride=%u fmt=%u fmt_name=%s state=0x%x state_known=%u "
		"skin_source=%s producer_event=%llu producer_draw=%llu producer_dispatch=%llu "
		"producer_pso=%llu producer_cs=%s producer_bind=%s producer_root=%u "
		"producer_reg=%u file=%S hash=%s\n",
		static_cast<unsigned long long>(buffer.eventSerial),
		static_cast<unsigned long long>(buffer.drawId),
		static_cast<unsigned long long>(buffer.dispatchId),
		static_cast<unsigned long long>(buffer.psoIndex),
		vs, ps, cs,
		buffer.role.c_str(),
		buffer.slot,
		buffer.resource.resource,
		ResourceDimensionName(desc.Dimension),
		static_cast<unsigned long long>(buffer.gpuVa),
		static_cast<unsigned long long>(sourceOffset),
		static_cast<unsigned long long>(copyBytes),
		buffer.stride,
		buffer.format,
		DxgiFormatName(buffer.format),
		static_cast<UINT>(sourceState),
		hasCurrentState ? 1 : 0,
		buffer.skinningClass.empty() ? "unknown" : buffer.skinningClass.c_str(),
		static_cast<unsigned long long>(buffer.producerEventSerial),
		static_cast<unsigned long long>(buffer.producerDrawId),
		static_cast<unsigned long long>(buffer.producerDispatchId),
		static_cast<unsigned long long>(buffer.producerPsoIndex),
		producerCs,
		buffer.producerBindSpace.empty() ? "-" : buffer.producerBindSpace.c_str(),
		buffer.producerRootParameterIndex,
		buffer.producerShaderRegister,
		filePath ? filePath : L"",
		hash);
}

void DX12FrameAnalysisManifestWriteResourceBinding(
	const DX12FrameResourceBinding &binding,
	const D3D12_RESOURCE_DESC &desc, UINT64 sourceOffset, UINT64 copyBytes,
	D3D12_RESOURCE_STATES sourceState, bool hasCurrentState,
	const wchar_t *filePath)
{
	DX12PsoShaderSummary shaders;
	const bool hasShaders = DX12GetPsoShaderSummary(binding.psoIndex, &shaders);
	char vs[32], ps[32], cs[32], hash[16];
	const bool hasVS = binding.shaderInfo.hasVS || (hasShaders && shaders.hasVS);
	const bool hasPS = binding.shaderInfo.hasPS || (hasShaders && shaders.hasPS);
	const bool hasCS = binding.shaderInfo.hasCS || (hasShaders && shaders.hasCS);
	const UINT64 vsHash = binding.shaderInfo.hasVS ? binding.shaderInfo.vs : shaders.vs;
	const UINT64 psHash = binding.shaderInfo.hasPS ? binding.shaderInfo.ps : shaders.ps;
	const UINT64 csHash = binding.shaderInfo.hasCS ? binding.shaderInfo.cs : shaders.cs;
	FormatShaderHash(vsHash, hasVS, vs, ARRAYSIZE(vs));
	FormatShaderHash(psHash, hasPS, ps, ARRAYSIZE(ps));
	FormatShaderHash(csHash, hasCS, cs, ARRAYSIZE(cs));
	FormatFileHash(filePath, hash, ARRAYSIZE(hash));

	const DX12DescriptorSummary &descriptor = binding.descriptor;
	DX12FrameAnalysisLogEvent(
		"bind.resource event=%llu draw=%llu dispatch=%llu pso=%llu "
		"vs=%s ps=%s cs=%s bind=%s root=%u range=%u reg=%u space=%u desc=%llu "
		"kind=%s resource=%p dim=%s width=%llu height=%u fmt=%u fmt_name=%s "
		"view_dimension=%u first_element=%llu num_elements=%u structure_byte_stride=%u "
		"buffer_view_offset=%llu buffer_view_bytes=%llu gpu=0x%llx offset=%llu bytes=%llu "
		"state=0x%x state_known=%u file=%S hash=%s\n",
		static_cast<unsigned long long>(binding.eventSerial),
		static_cast<unsigned long long>(binding.drawId),
		static_cast<unsigned long long>(binding.dispatchId),
		static_cast<unsigned long long>(binding.psoIndex),
		vs, ps, cs,
		binding.bindSpace.c_str(),
		binding.rootParameterIndex,
		binding.rangeIndex,
		binding.shaderRegister,
		binding.registerSpace,
		static_cast<unsigned long long>(binding.descriptorIndex),
		descriptor.kind.c_str(),
		descriptor.resource,
		ResourceDimensionName(desc.Dimension),
		static_cast<unsigned long long>(desc.Width),
		desc.Height,
		static_cast<UINT>(desc.Format),
		DxgiFormatName(static_cast<UINT>(desc.Format)),
		descriptor.viewDimension,
		static_cast<unsigned long long>(descriptor.firstElement),
		descriptor.numElements,
		descriptor.structureByteStride,
		static_cast<unsigned long long>(descriptor.bufferViewOffset),
		static_cast<unsigned long long>(descriptor.bufferViewBytes),
		static_cast<unsigned long long>(descriptor.gpuVirtualAddress),
		static_cast<unsigned long long>(sourceOffset),
		static_cast<unsigned long long>(copyBytes),
		static_cast<UINT>(sourceState),
		hasCurrentState ? 1 : 0,
		filePath ? filePath : L"",
		hash);
}
