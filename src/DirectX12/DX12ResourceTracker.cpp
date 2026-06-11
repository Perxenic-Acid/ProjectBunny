#include "DX12ResourceTracker.h"

#include <Shlwapi.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "DX12FrameAnalysis.h"
#include "DX12State.h"

static void GetSummaryDirectory(const wchar_t *dir, wchar_t *path, size_t pathCount)
{
	if (!dir || !path || pathCount == 0)
		return;
	swprintf_s(path, pathCount, L"%s\\summary", dir);
	CreateDirectoryW(path, nullptr);
}

typedef HRESULT(STDMETHODCALLTYPE *PFN_CREATE_DESCRIPTOR_HEAP)(
	ID3D12Device*, const D3D12_DESCRIPTOR_HEAP_DESC*, REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CREATE_ROOT_SIGNATURE)(
	ID3D12Device*, UINT, const void*, SIZE_T, REFIID, void**);
typedef void(STDMETHODCALLTYPE *PFN_CREATE_CONSTANT_BUFFER_VIEW)(
	ID3D12Device*, const D3D12_CONSTANT_BUFFER_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
typedef void(STDMETHODCALLTYPE *PFN_CREATE_SHADER_RESOURCE_VIEW)(
	ID3D12Device*, ID3D12Resource*, const D3D12_SHADER_RESOURCE_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
typedef void(STDMETHODCALLTYPE *PFN_CREATE_UNORDERED_ACCESS_VIEW)(
	ID3D12Device*, ID3D12Resource*, ID3D12Resource*, const D3D12_UNORDERED_ACCESS_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
typedef void(STDMETHODCALLTYPE *PFN_CREATE_RENDER_TARGET_VIEW)(
	ID3D12Device*, ID3D12Resource*, const D3D12_RENDER_TARGET_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
typedef void(STDMETHODCALLTYPE *PFN_CREATE_DEPTH_STENCIL_VIEW)(
	ID3D12Device*, ID3D12Resource*, const D3D12_DEPTH_STENCIL_VIEW_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
typedef void(STDMETHODCALLTYPE *PFN_CREATE_SAMPLER)(
	ID3D12Device*, const D3D12_SAMPLER_DESC*, D3D12_CPU_DESCRIPTOR_HANDLE);
typedef void(STDMETHODCALLTYPE *PFN_COPY_DESCRIPTORS)(
	ID3D12Device*, UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*,
	UINT, const D3D12_CPU_DESCRIPTOR_HANDLE*, const UINT*, D3D12_DESCRIPTOR_HEAP_TYPE);
typedef void(STDMETHODCALLTYPE *PFN_COPY_DESCRIPTORS_SIMPLE)(
	ID3D12Device*, UINT, D3D12_CPU_DESCRIPTOR_HANDLE, D3D12_CPU_DESCRIPTOR_HANDLE,
	D3D12_DESCRIPTOR_HEAP_TYPE);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CREATE_COMMITTED_RESOURCE)(
	ID3D12Device*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
	const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*,
	REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CREATE_PLACED_RESOURCE)(
	ID3D12Device*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC*,
	D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CREATE_RESERVED_RESOURCE)(
	ID3D12Device*, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
	const D3D12_CLEAR_VALUE*, REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CREATE_COMMITTED_RESOURCE1)(
	ID3D12Device4*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
	const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*,
	ID3D12ProtectedResourceSession*, REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CREATE_RESERVED_RESOURCE1)(
	ID3D12Device4*, const D3D12_RESOURCE_DESC*, D3D12_RESOURCE_STATES,
	const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CREATE_COMMITTED_RESOURCE2)(
	ID3D12Device8*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
	const D3D12_RESOURCE_DESC1*, D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*,
	ID3D12ProtectedResourceSession*, REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CREATE_PLACED_RESOURCE1)(
	ID3D12Device8*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC1*,
	D3D12_RESOURCE_STATES, const D3D12_CLEAR_VALUE*, REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CREATE_COMMITTED_RESOURCE3)(
	ID3D12Device10*, const D3D12_HEAP_PROPERTIES*, D3D12_HEAP_FLAGS,
	const D3D12_RESOURCE_DESC1*, D3D12_BARRIER_LAYOUT, const D3D12_CLEAR_VALUE*,
	ID3D12ProtectedResourceSession*, UINT32, const DXGI_FORMAT*, REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CREATE_PLACED_RESOURCE2)(
	ID3D12Device10*, ID3D12Heap*, UINT64, const D3D12_RESOURCE_DESC1*,
	D3D12_BARRIER_LAYOUT, const D3D12_CLEAR_VALUE*, UINT32, const DXGI_FORMAT*,
	REFIID, void**);
typedef HRESULT(STDMETHODCALLTYPE *PFN_CREATE_RESERVED_RESOURCE2)(
	ID3D12Device10*, const D3D12_RESOURCE_DESC*, D3D12_BARRIER_LAYOUT,
	const D3D12_CLEAR_VALUE*, ID3D12ProtectedResourceSession*, UINT32,
	const DXGI_FORMAT*, REFIID, void**);

static PFN_CREATE_DESCRIPTOR_HEAP gOrigCreateDescriptorHeap = nullptr;
static PFN_CREATE_ROOT_SIGNATURE gOrigCreateRootSignature = nullptr;
static PFN_CREATE_CONSTANT_BUFFER_VIEW gOrigCreateConstantBufferView = nullptr;
static PFN_CREATE_SHADER_RESOURCE_VIEW gOrigCreateShaderResourceView = nullptr;
static PFN_CREATE_UNORDERED_ACCESS_VIEW gOrigCreateUnorderedAccessView = nullptr;
static PFN_CREATE_RENDER_TARGET_VIEW gOrigCreateRenderTargetView = nullptr;
static PFN_CREATE_DEPTH_STENCIL_VIEW gOrigCreateDepthStencilView = nullptr;
static PFN_CREATE_SAMPLER gOrigCreateSampler = nullptr;
static PFN_COPY_DESCRIPTORS gOrigCopyDescriptors = nullptr;
static PFN_COPY_DESCRIPTORS_SIMPLE gOrigCopyDescriptorsSimple = nullptr;
static PFN_CREATE_COMMITTED_RESOURCE gOrigCreateCommittedResource = nullptr;
static PFN_CREATE_PLACED_RESOURCE gOrigCreatePlacedResource = nullptr;
static PFN_CREATE_RESERVED_RESOURCE gOrigCreateReservedResource = nullptr;
static PFN_CREATE_COMMITTED_RESOURCE1 gOrigCreateCommittedResource1 = nullptr;
static PFN_CREATE_RESERVED_RESOURCE1 gOrigCreateReservedResource1 = nullptr;
static PFN_CREATE_COMMITTED_RESOURCE2 gOrigCreateCommittedResource2 = nullptr;
static PFN_CREATE_PLACED_RESOURCE1 gOrigCreatePlacedResource1 = nullptr;
static PFN_CREATE_COMMITTED_RESOURCE3 gOrigCreateCommittedResource3 = nullptr;
static PFN_CREATE_PLACED_RESOURCE2 gOrigCreatePlacedResource2 = nullptr;
static PFN_CREATE_RESERVED_RESOURCE2 gOrigCreateReservedResource2 = nullptr;

struct RootSignatureRecord
{
	ID3D12RootSignature *rootSignature = nullptr;
	UINT64 hash = 0;
	SIZE_T size = 0;
	UINT nodeMask = 0;
};

struct DescriptorHeapRecord
{
	ID3D12DescriptorHeap *heap = nullptr;
	D3D12_DESCRIPTOR_HEAP_DESC desc = {};
	SIZE_T cpuStart = 0;
	UINT64 gpuStart = 0;
	UINT increment = 0;
};

struct DescriptorRecord
{
	std::string kind;
	SIZE_T cpuHandle = 0;
	ID3D12Resource *resource = nullptr;
	ID3D12Resource *counterResource = nullptr;
	D3D12_RESOURCE_DESC resourceDesc = {};
	bool hasResourceDesc = false;
	UINT resourceHeapType = 0;
	bool hasResourceHeapType = false;
	UINT64 gpuVirtualAddress = 0;
	UINT64 resourceOffset = 0;
	UINT64 viewSize = 0;
	D3D12_RESOURCE_STATES currentState = D3D12_RESOURCE_STATE_COMMON;
	bool hasCurrentState = false;
	D3D12_CONSTANT_BUFFER_VIEW_DESC cbv = {};
	D3D12_SHADER_RESOURCE_VIEW_DESC srv = {};
	D3D12_UNORDERED_ACCESS_VIEW_DESC uav = {};
	D3D12_RENDER_TARGET_VIEW_DESC rtv = {};
	D3D12_DEPTH_STENCIL_VIEW_DESC dsv = {};
	D3D12_SAMPLER_DESC sampler = {};
	bool hasDesc = false;
};

struct ResourceRecord
{
	ID3D12Resource *resource = nullptr;
	D3D12_RESOURCE_DESC desc = {};
	D3D12_HEAP_TYPE heapType = D3D12_HEAP_TYPE_DEFAULT;
	bool hasHeapType = false;
	UINT64 gpuVirtualAddress = 0;
	UINT64 size = 0;
	D3D12_RESOURCE_STATES currentState = D3D12_RESOURCE_STATE_COMMON;
	bool hasCurrentState = false;
};

struct PsoRootRecord
{
	UINT64 psoIndex = 0;
	std::string kind;
	ID3D12PipelineState *pipelineState = nullptr;
	ID3D12RootSignature *rootSignature = nullptr;
};

static SRWLOCK gResourceLock = SRWLOCK_INIT;
static std::vector<RootSignatureRecord> gRootSignatures;
static std::vector<DescriptorHeapRecord> gDescriptorHeaps;
static std::vector<DescriptorRecord> gDescriptors;
static std::unordered_map<SIZE_T, size_t> gDescriptorByCpuHandle;
static std::vector<PsoRootRecord> gPsoRoots;
static std::vector<ResourceRecord> gResources;
static std::unordered_map<ID3D12Resource*, size_t> gResourceByPtr;
static bool gCleanupRegistered = false;
static UINT64 gResourcesRecordedFromCreate = 0;

static UINT64 Fnv1a64(const void *data, size_t size)
{
	const uint8_t *bytes = static_cast<const uint8_t*>(data);
	UINT64 hash = 14695981039346656037ull;
	for (size_t i = 0; i < size; ++i) {
		hash ^= bytes[i];
		hash *= 1099511628211ull;
	}
	return hash;
}

static const char *DescriptorHeapTypeName(D3D12_DESCRIPTOR_HEAP_TYPE type)
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

static D3D12_RESOURCE_DESC ResourceDescFromDesc1(const D3D12_RESOURCE_DESC1 *desc)
{
	D3D12_RESOURCE_DESC out = {};
	if (!desc)
		return out;
	out.Dimension = desc->Dimension;
	out.Alignment = desc->Alignment;
	out.Width = desc->Width;
	out.Height = desc->Height;
	out.DepthOrArraySize = desc->DepthOrArraySize;
	out.MipLevels = desc->MipLevels;
	out.Format = desc->Format;
	out.SampleDesc = desc->SampleDesc;
	out.Layout = desc->Layout;
	out.Flags = desc->Flags;
	return out;
}

static D3D12_RESOURCE_STATES ResourceStateFromLayout(D3D12_BARRIER_LAYOUT layout)
{
	switch (layout) {
	case D3D12_BARRIER_LAYOUT_COMMON:
		return D3D12_RESOURCE_STATE_COMMON;
	case D3D12_BARRIER_LAYOUT_GENERIC_READ:
		return D3D12_RESOURCE_STATE_GENERIC_READ;
	case D3D12_BARRIER_LAYOUT_RENDER_TARGET:
		return D3D12_RESOURCE_STATE_RENDER_TARGET;
	case D3D12_BARRIER_LAYOUT_UNORDERED_ACCESS:
		return D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
	case D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_WRITE:
		return D3D12_RESOURCE_STATE_DEPTH_WRITE;
	case D3D12_BARRIER_LAYOUT_DEPTH_STENCIL_READ:
		return D3D12_RESOURCE_STATE_DEPTH_READ;
	case D3D12_BARRIER_LAYOUT_SHADER_RESOURCE:
		return D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE |
			D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
	case D3D12_BARRIER_LAYOUT_COPY_SOURCE:
		return D3D12_RESOURCE_STATE_COPY_SOURCE;
	case D3D12_BARRIER_LAYOUT_COPY_DEST:
		return D3D12_RESOURCE_STATE_COPY_DEST;
	case D3D12_BARRIER_LAYOUT_RESOLVE_SOURCE:
		return D3D12_RESOURCE_STATE_RESOLVE_SOURCE;
	case D3D12_BARRIER_LAYOUT_RESOLVE_DEST:
		return D3D12_RESOURCE_STATE_RESOLVE_DEST;
	default:
		return D3D12_RESOURCE_STATE_COMMON;
	}
}

static ID3D12Resource *CanonicalResource(ID3D12Resource *resource)
{
	if (!resource)
		return nullptr;

	IUnknown *unknown = nullptr;
	if (SUCCEEDED(resource->QueryInterface(IID_PPV_ARGS(&unknown))) && unknown) {
		ID3D12Resource *canonical = reinterpret_cast<ID3D12Resource*>(unknown);
		unknown->Release();
		return canonical;
	}
	return resource;
}

static void RecordResource(
	ID3D12Resource *resource, const D3D12_RESOURCE_DESC *desc,
	const D3D12_HEAP_PROPERTIES *heapProperties,
	D3D12_RESOURCE_STATES initialState)
{
	if (!resource)
		return;

	ID3D12Resource *canonical = CanonicalResource(resource);
	ResourceRecord record;
	record.resource = resource;
	record.desc = desc ? *desc : resource->GetDesc();
	if (heapProperties) {
		record.heapType = heapProperties->Type;
		record.hasHeapType = true;
	}

	if (record.hasHeapType && record.heapType == D3D12_HEAP_TYPE_READBACK)
		return;

	record.currentState = initialState;
	record.hasCurrentState = true;
	if (record.desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
		record.gpuVirtualAddress = resource->GetGPUVirtualAddress();
		record.size = record.desc.Width;
	}

	AcquireSRWLockExclusive(&gResourceLock);
	auto found = gResourceByPtr.find(canonical);
	if (found != gResourceByPtr.end()) {
		gResources[found->second] = record;
	} else {
		gResourceByPtr[canonical] = gResources.size();
		gResources.push_back(record);
	}
	gResourcesRecordedFromCreate++;
	ReleaseSRWLockExclusive(&gResourceLock);
}

static void CleanupTrackedResources()
{
	AcquireSRWLockExclusive(&gResourceLock);
	gResources.clear();
	gResourceByPtr.clear();
	ReleaseSRWLockExclusive(&gResourceLock);
}

static void FillResourceInfo(DescriptorRecord *record, ID3D12Resource *resource)
{
	if (!record || !resource)
		return;

	record->resource = resource;
	record->resourceDesc = resource->GetDesc();
	record->hasResourceDesc = true;
	if (record->resourceDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
		record->gpuVirtualAddress = resource->GetGPUVirtualAddress();

	ID3D12Resource *canonical = CanonicalResource(resource);
	AcquireSRWLockShared(&gResourceLock);
	auto found = gResourceByPtr.find(canonical);
	if (found != gResourceByPtr.end() && found->second < gResources.size()) {
		const ResourceRecord &resourceRecord = gResources[found->second];
		if (resourceRecord.hasHeapType) {
			record->resourceHeapType = static_cast<UINT>(resourceRecord.heapType);
			record->hasResourceHeapType = true;
		}
		record->currentState = resourceRecord.currentState;
		record->hasCurrentState = resourceRecord.hasCurrentState;
	}
	ReleaseSRWLockShared(&gResourceLock);
}

static bool ResolveBufferByGpuVa(UINT64 gpuVa, UINT64 size, DescriptorRecord *record)
{
	if (!record || gpuVa == 0)
		return false;

	bool resolved = false;
	AcquireSRWLockShared(&gResourceLock);
	for (const ResourceRecord &resource : gResources) {
		if (resource.desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
			resource.gpuVirtualAddress == 0 || resource.size == 0)
			continue;
		const UINT64 begin = resource.gpuVirtualAddress;
		const UINT64 end = begin + resource.size;
		const UINT64 requestedEnd = gpuVa + size;
		if (gpuVa >= begin && requestedEnd <= end) {
			record->resource = resource.resource;
			record->resourceDesc = resource.desc;
			record->hasResourceDesc = true;
			record->resourceOffset = gpuVa - begin;
			record->viewSize = size;
			if (resource.hasHeapType) {
				record->resourceHeapType = static_cast<UINT>(resource.heapType);
				record->hasResourceHeapType = true;
			}
			record->currentState = resource.currentState;
			record->hasCurrentState = resource.hasCurrentState;
			resolved = true;
			break;
		}
	}
	ReleaseSRWLockShared(&gResourceLock);
	return resolved;
}

static UINT DxgiFormatBytesPerElement(DXGI_FORMAT format)
{
	switch (format) {
	case DXGI_FORMAT_R32G32B32A32_TYPELESS:
	case DXGI_FORMAT_R32G32B32A32_FLOAT:
	case DXGI_FORMAT_R32G32B32A32_UINT:
	case DXGI_FORMAT_R32G32B32A32_SINT:
		return 16;
	case DXGI_FORMAT_R32G32B32_TYPELESS:
	case DXGI_FORMAT_R32G32B32_FLOAT:
	case DXGI_FORMAT_R32G32B32_UINT:
	case DXGI_FORMAT_R32G32B32_SINT:
		return 12;
	case DXGI_FORMAT_R16G16B16A16_TYPELESS:
	case DXGI_FORMAT_R16G16B16A16_FLOAT:
	case DXGI_FORMAT_R16G16B16A16_UNORM:
	case DXGI_FORMAT_R16G16B16A16_UINT:
	case DXGI_FORMAT_R16G16B16A16_SNORM:
	case DXGI_FORMAT_R16G16B16A16_SINT:
	case DXGI_FORMAT_R32G32_TYPELESS:
	case DXGI_FORMAT_R32G32_FLOAT:
	case DXGI_FORMAT_R32G32_UINT:
	case DXGI_FORMAT_R32G32_SINT:
	case DXGI_FORMAT_R32G8X24_TYPELESS:
	case DXGI_FORMAT_D32_FLOAT_S8X24_UINT:
	case DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS:
	case DXGI_FORMAT_X32_TYPELESS_G8X24_UINT:
		return 8;
	case DXGI_FORMAT_R10G10B10A2_TYPELESS:
	case DXGI_FORMAT_R10G10B10A2_UNORM:
	case DXGI_FORMAT_R10G10B10A2_UINT:
	case DXGI_FORMAT_R11G11B10_FLOAT:
	case DXGI_FORMAT_R8G8B8A8_TYPELESS:
	case DXGI_FORMAT_R8G8B8A8_UNORM:
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
	case DXGI_FORMAT_R8G8B8A8_UINT:
	case DXGI_FORMAT_R8G8B8A8_SNORM:
	case DXGI_FORMAT_R8G8B8A8_SINT:
	case DXGI_FORMAT_R16G16_TYPELESS:
	case DXGI_FORMAT_R16G16_FLOAT:
	case DXGI_FORMAT_R16G16_UNORM:
	case DXGI_FORMAT_R16G16_UINT:
	case DXGI_FORMAT_R16G16_SNORM:
	case DXGI_FORMAT_R16G16_SINT:
	case DXGI_FORMAT_R32_TYPELESS:
	case DXGI_FORMAT_D32_FLOAT:
	case DXGI_FORMAT_R32_FLOAT:
	case DXGI_FORMAT_R32_UINT:
	case DXGI_FORMAT_R32_SINT:
	case DXGI_FORMAT_R24G8_TYPELESS:
	case DXGI_FORMAT_D24_UNORM_S8_UINT:
	case DXGI_FORMAT_R24_UNORM_X8_TYPELESS:
	case DXGI_FORMAT_X24_TYPELESS_G8_UINT:
	case DXGI_FORMAT_R9G9B9E5_SHAREDEXP:
	case DXGI_FORMAT_R8G8_B8G8_UNORM:
	case DXGI_FORMAT_G8R8_G8B8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_UNORM:
	case DXGI_FORMAT_B8G8R8X8_UNORM:
	case DXGI_FORMAT_B8G8R8A8_TYPELESS:
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
	case DXGI_FORMAT_B8G8R8X8_TYPELESS:
	case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
		return 4;
	case DXGI_FORMAT_R8G8_TYPELESS:
	case DXGI_FORMAT_R8G8_UNORM:
	case DXGI_FORMAT_R8G8_UINT:
	case DXGI_FORMAT_R8G8_SNORM:
	case DXGI_FORMAT_R8G8_SINT:
	case DXGI_FORMAT_R16_TYPELESS:
	case DXGI_FORMAT_R16_FLOAT:
	case DXGI_FORMAT_D16_UNORM:
	case DXGI_FORMAT_R16_UNORM:
	case DXGI_FORMAT_R16_UINT:
	case DXGI_FORMAT_R16_SNORM:
	case DXGI_FORMAT_R16_SINT:
	case DXGI_FORMAT_B5G6R5_UNORM:
	case DXGI_FORMAT_B5G5R5A1_UNORM:
	case DXGI_FORMAT_B4G4R4A4_UNORM:
		return 2;
	case DXGI_FORMAT_R8_TYPELESS:
	case DXGI_FORMAT_R8_UNORM:
	case DXGI_FORMAT_R8_UINT:
	case DXGI_FORMAT_R8_SNORM:
	case DXGI_FORMAT_R8_SINT:
	case DXGI_FORMAT_A8_UNORM:
		return 1;
	default:
		return 0;
	}
}

static UINT SrvBufferBytesPerElement(const D3D12_SHADER_RESOURCE_VIEW_DESC &desc)
{
	if (desc.Buffer.StructureByteStride)
		return desc.Buffer.StructureByteStride;
	if (desc.Buffer.Flags & D3D12_BUFFER_SRV_FLAG_RAW)
		return 4;
	return DxgiFormatBytesPerElement(desc.Format);
}

static UINT UavBufferBytesPerElement(const D3D12_UNORDERED_ACCESS_VIEW_DESC &desc)
{
	if (desc.Buffer.StructureByteStride)
		return desc.Buffer.StructureByteStride;
	if (desc.Buffer.Flags & D3D12_BUFFER_UAV_FLAG_RAW)
		return 4;
	return DxgiFormatBytesPerElement(desc.Format);
}

static void FillSrvBufferView(DescriptorRecord *record)
{
	if (!record || !record->hasDesc ||
		record->srv.ViewDimension != D3D12_SRV_DIMENSION_BUFFER)
		return;

	const D3D12_BUFFER_SRV &buffer = record->srv.Buffer;
	const UINT bytesPerElement = SrvBufferBytesPerElement(record->srv);
	record->resourceOffset = buffer.FirstElement * bytesPerElement;
	record->viewSize = static_cast<UINT64>(buffer.NumElements) * bytesPerElement;
	if (record->viewSize == 0 && record->resourceDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
		record->viewSize = record->resourceDesc.Width > record->resourceOffset ?
			record->resourceDesc.Width - record->resourceOffset : 0;
}

static void FillUavBufferView(DescriptorRecord *record)
{
	if (!record || !record->hasDesc ||
		record->uav.ViewDimension != D3D12_UAV_DIMENSION_BUFFER)
		return;

	const D3D12_BUFFER_UAV &buffer = record->uav.Buffer;
	const UINT bytesPerElement = UavBufferBytesPerElement(record->uav);
	record->resourceOffset = buffer.FirstElement * bytesPerElement;
	record->viewSize = static_cast<UINT64>(buffer.NumElements) * bytesPerElement;
	if (record->viewSize == 0 && record->resourceDesc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER)
		record->viewSize = record->resourceDesc.Width > record->resourceOffset ?
			record->resourceDesc.Width - record->resourceOffset : 0;
}

static void UpdateDescriptorResourceStateLocked(
	ID3D12Resource *resource, D3D12_RESOURCE_STATES state, bool hasState)
{
	if (!resource)
		return;

	for (DescriptorRecord &descriptor : gDescriptors) {
		if (descriptor.resource == resource) {
			descriptor.currentState = state;
			descriptor.hasCurrentState = hasState;
		}
	}
}

static void RecordDescriptor(DescriptorRecord &&record)
{
	AcquireSRWLockExclusive(&gResourceLock);
	auto found = gDescriptorByCpuHandle.find(record.cpuHandle);
	if (found != gDescriptorByCpuHandle.end()) {
		gDescriptors[found->second] = std::move(record);
	} else {
		gDescriptorByCpuHandle[record.cpuHandle] = gDescriptors.size();
		gDescriptors.push_back(std::move(record));
	}
	ReleaseSRWLockExclusive(&gResourceLock);
}

static bool FindLatestDescriptorLocked(SIZE_T cpuHandle, DescriptorRecord *record)
{
	if (!record)
		return false;

	auto found = gDescriptorByCpuHandle.find(cpuHandle);
	if (found == gDescriptorByCpuHandle.end() || found->second >= gDescriptors.size())
		return false;

	*record = gDescriptors[found->second];
	return true;
}

static void RecordDescriptorLocked(DescriptorRecord &&record)
{
	auto found = gDescriptorByCpuHandle.find(record.cpuHandle);
	if (found != gDescriptorByCpuHandle.end()) {
		gDescriptors[found->second] = std::move(record);
	} else {
		gDescriptorByCpuHandle[record.cpuHandle] = gDescriptors.size();
		gDescriptors.push_back(std::move(record));
	}
}

static void RecordDescriptorCopyRange(
	ID3D12Device *device, D3D12_DESCRIPTOR_HEAP_TYPE type,
	D3D12_CPU_DESCRIPTOR_HANDLE destStart, D3D12_CPU_DESCRIPTOR_HANDLE srcStart,
	UINT count)
{
	if (!device || count == 0)
		return;

	const UINT increment = device->GetDescriptorHandleIncrementSize(type);
	if (increment == 0)
		return;

	AcquireSRWLockExclusive(&gResourceLock);
	for (UINT i = 0; i < count; ++i) {
		const SIZE_T srcHandle = srcStart.ptr + static_cast<SIZE_T>(i) * increment;
		const SIZE_T destHandle = destStart.ptr + static_cast<SIZE_T>(i) * increment;
		DescriptorRecord copied;
		if (FindLatestDescriptorLocked(srcHandle, &copied)) {
			copied.cpuHandle = destHandle;
			RecordDescriptorLocked(std::move(copied));
		}
	}
	ReleaseSRWLockExclusive(&gResourceLock);
}

static HRESULT STDMETHODCALLTYPE HookedCreateDescriptorHeap(
	ID3D12Device *device, const D3D12_DESCRIPTOR_HEAP_DESC *desc, REFIID riid, void **heap)
{
	HRESULT hr = gOrigCreateDescriptorHeap(device, desc, riid, heap);
	if (SUCCEEDED(hr) && desc && heap && *heap) {
		ID3D12DescriptorHeap *descriptorHeap = nullptr;
		if (SUCCEEDED(static_cast<IUnknown*>(*heap)->QueryInterface(IID_PPV_ARGS(&descriptorHeap)))) {
			DescriptorHeapRecord record;
			record.heap = descriptorHeap;
			record.desc = *desc;
			record.cpuStart = descriptorHeap->GetCPUDescriptorHandleForHeapStart().ptr;
			record.gpuStart = descriptorHeap->GetGPUDescriptorHandleForHeapStart().ptr;
			record.increment = device->GetDescriptorHandleIncrementSize(desc->Type);
			descriptorHeap->Release();

			AcquireSRWLockExclusive(&gResourceLock);
			gDescriptorHeaps.push_back(record);
			ReleaseSRWLockExclusive(&gResourceLock);
		}
	}
	return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreateRootSignature(
	ID3D12Device *device, UINT nodeMask, const void *blob, SIZE_T blobLength,
	REFIID riid, void **rootSignature)
{
	HRESULT hr = gOrigCreateRootSignature(device, nodeMask, blob, blobLength, riid, rootSignature);
	if (SUCCEEDED(hr) && blob && blobLength && rootSignature && *rootSignature) {
		RootSignatureRecord record;
		record.rootSignature = static_cast<ID3D12RootSignature*>(*rootSignature);
		record.hash = Fnv1a64(blob, blobLength);
		record.size = blobLength;
		record.nodeMask = nodeMask;

		AcquireSRWLockExclusive(&gResourceLock);
		gRootSignatures.push_back(record);
		ReleaseSRWLockExclusive(&gResourceLock);
	}
	return hr;
}

static void STDMETHODCALLTYPE HookedCreateConstantBufferView(
	ID3D12Device *device, const D3D12_CONSTANT_BUFFER_VIEW_DESC *desc,
	D3D12_CPU_DESCRIPTOR_HANDLE destDescriptor)
{
	gOrigCreateConstantBufferView(device, desc, destDescriptor);
	if (desc) {
		DescriptorRecord record;
		record.kind = "CBV";
		record.cpuHandle = destDescriptor.ptr;
		record.cbv = *desc;
		record.hasDesc = true;
		record.gpuVirtualAddress = desc->BufferLocation;
		record.viewSize = desc->SizeInBytes;
		ResolveBufferByGpuVa(desc->BufferLocation, desc->SizeInBytes, &record);
		RecordDescriptor(std::move(record));
	}
}

static void STDMETHODCALLTYPE HookedCreateShaderResourceView(
	ID3D12Device *device, ID3D12Resource *resource,
	const D3D12_SHADER_RESOURCE_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE destDescriptor)
{
	gOrigCreateShaderResourceView(device, resource, desc, destDescriptor);
	DescriptorRecord record;
	record.kind = "SRV";
	record.cpuHandle = destDescriptor.ptr;
	FillResourceInfo(&record, resource);
	if (desc) {
		record.srv = *desc;
		record.hasDesc = true;
		FillSrvBufferView(&record);
	}
	RecordDescriptor(std::move(record));
}

static void STDMETHODCALLTYPE HookedCreateUnorderedAccessView(
	ID3D12Device *device, ID3D12Resource *resource, ID3D12Resource *counterResource,
	const D3D12_UNORDERED_ACCESS_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE destDescriptor)
{
	gOrigCreateUnorderedAccessView(device, resource, counterResource, desc, destDescriptor);
	DescriptorRecord record;
	record.kind = "UAV";
	record.cpuHandle = destDescriptor.ptr;
	record.counterResource = counterResource;
	FillResourceInfo(&record, resource);
	if (desc) {
		record.uav = *desc;
		record.hasDesc = true;
		FillUavBufferView(&record);
	}
	RecordDescriptor(std::move(record));
}

static void STDMETHODCALLTYPE HookedCreateRenderTargetView(
	ID3D12Device *device, ID3D12Resource *resource,
	const D3D12_RENDER_TARGET_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE destDescriptor)
{
	gOrigCreateRenderTargetView(device, resource, desc, destDescriptor);
	DescriptorRecord record;
	record.kind = "RTV";
	record.cpuHandle = destDescriptor.ptr;
	FillResourceInfo(&record, resource);
	if (desc) {
		record.rtv = *desc;
		record.hasDesc = true;
	}
	RecordDescriptor(std::move(record));
}

static void STDMETHODCALLTYPE HookedCreateDepthStencilView(
	ID3D12Device *device, ID3D12Resource *resource,
	const D3D12_DEPTH_STENCIL_VIEW_DESC *desc, D3D12_CPU_DESCRIPTOR_HANDLE destDescriptor)
{
	gOrigCreateDepthStencilView(device, resource, desc, destDescriptor);
	DescriptorRecord record;
	record.kind = "DSV";
	record.cpuHandle = destDescriptor.ptr;
	FillResourceInfo(&record, resource);
	if (desc) {
		record.dsv = *desc;
		record.hasDesc = true;
	}
	RecordDescriptor(std::move(record));
}

static void STDMETHODCALLTYPE HookedCreateSampler(
	ID3D12Device *device, const D3D12_SAMPLER_DESC *desc,
	D3D12_CPU_DESCRIPTOR_HANDLE destDescriptor)
{
	gOrigCreateSampler(device, desc, destDescriptor);
	if (desc) {
		DescriptorRecord record;
		record.kind = "Sampler";
		record.cpuHandle = destDescriptor.ptr;
		record.sampler = *desc;
		record.hasDesc = true;
		RecordDescriptor(std::move(record));
	}
}

static void STDMETHODCALLTYPE HookedCopyDescriptors(
	ID3D12Device *device, UINT numDestDescriptorRanges,
	const D3D12_CPU_DESCRIPTOR_HANDLE *destDescriptorRangeStarts,
	const UINT *destDescriptorRangeSizes, UINT numSrcDescriptorRanges,
	const D3D12_CPU_DESCRIPTOR_HANDLE *srcDescriptorRangeStarts,
	const UINT *srcDescriptorRangeSizes, D3D12_DESCRIPTOR_HEAP_TYPE descriptorHeapsType)
{
	gOrigCopyDescriptors(device, numDestDescriptorRanges, destDescriptorRangeStarts,
		destDescriptorRangeSizes, numSrcDescriptorRanges, srcDescriptorRangeStarts,
		srcDescriptorRangeSizes, descriptorHeapsType);

	if (!destDescriptorRangeStarts || !srcDescriptorRangeStarts)
		return;

	UINT destRange = 0;
	UINT srcRange = 0;
	UINT destOffset = 0;
	UINT srcOffset = 0;
	while (destRange < numDestDescriptorRanges && srcRange < numSrcDescriptorRanges) {
		const UINT destRangeSize = destDescriptorRangeSizes ? destDescriptorRangeSizes[destRange] : 1;
		const UINT srcRangeSize = srcDescriptorRangeSizes ? srcDescriptorRangeSizes[srcRange] : 1;
		if (destOffset >= destRangeSize) {
			destRange++;
			destOffset = 0;
			continue;
		}
		if (srcOffset >= srcRangeSize) {
			srcRange++;
			srcOffset = 0;
			continue;
		}

		const UINT count = min(destRangeSize - destOffset, srcRangeSize - srcOffset);
		const UINT increment = device ? device->GetDescriptorHandleIncrementSize(descriptorHeapsType) : 0;
		if (increment) {
			D3D12_CPU_DESCRIPTOR_HANDLE dest = destDescriptorRangeStarts[destRange];
			D3D12_CPU_DESCRIPTOR_HANDLE src = srcDescriptorRangeStarts[srcRange];
			dest.ptr += static_cast<SIZE_T>(destOffset) * increment;
			src.ptr += static_cast<SIZE_T>(srcOffset) * increment;
			RecordDescriptorCopyRange(device, descriptorHeapsType, dest, src, count);
		}
		destOffset += count;
		srcOffset += count;
	}
}

static void STDMETHODCALLTYPE HookedCopyDescriptorsSimple(
	ID3D12Device *device, UINT numDescriptors, D3D12_CPU_DESCRIPTOR_HANDLE destDescriptorRangeStart,
	D3D12_CPU_DESCRIPTOR_HANDLE srcDescriptorRangeStart, D3D12_DESCRIPTOR_HEAP_TYPE descriptorHeapsType)
{
	gOrigCopyDescriptorsSimple(device, numDescriptors, destDescriptorRangeStart,
		srcDescriptorRangeStart, descriptorHeapsType);
	RecordDescriptorCopyRange(device, descriptorHeapsType, destDescriptorRangeStart,
		srcDescriptorRangeStart, numDescriptors);
}

static HRESULT STDMETHODCALLTYPE HookedCreateCommittedResource(
	ID3D12Device *device, const D3D12_HEAP_PROPERTIES *heapProperties,
	D3D12_HEAP_FLAGS heapFlags, const D3D12_RESOURCE_DESC *desc,
	D3D12_RESOURCE_STATES initialState, const D3D12_CLEAR_VALUE *optimizedClearValue,
	REFIID riid, void **resource)
{
	HRESULT hr = gOrigCreateCommittedResource(device, heapProperties, heapFlags, desc,
		initialState, optimizedClearValue, riid, resource);
	if (SUCCEEDED(hr) && resource && *resource) {
		ID3D12Resource *d3dResource = nullptr;
		if (SUCCEEDED(static_cast<IUnknown*>(*resource)->QueryInterface(IID_PPV_ARGS(&d3dResource)))) {
			RecordResource(d3dResource, desc, heapProperties, initialState);
			d3dResource->Release();
		}
	}
	return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreatePlacedResource(
	ID3D12Device *device, ID3D12Heap *heap, UINT64 heapOffset,
	const D3D12_RESOURCE_DESC *desc, D3D12_RESOURCE_STATES initialState,
	const D3D12_CLEAR_VALUE *optimizedClearValue, REFIID riid, void **resource)
{
	HRESULT hr = gOrigCreatePlacedResource(device, heap, heapOffset, desc,
		initialState, optimizedClearValue, riid, resource);
	if (SUCCEEDED(hr) && resource && *resource) {
		ID3D12Resource *d3dResource = nullptr;
		if (SUCCEEDED(static_cast<IUnknown*>(*resource)->QueryInterface(IID_PPV_ARGS(&d3dResource)))) {
			RecordResource(d3dResource, desc, nullptr, initialState);
			d3dResource->Release();
		}
	}
	return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreateReservedResource(
	ID3D12Device *device, const D3D12_RESOURCE_DESC *desc,
	D3D12_RESOURCE_STATES initialState, const D3D12_CLEAR_VALUE *optimizedClearValue,
	REFIID riid, void **resource)
{
	HRESULT hr = gOrigCreateReservedResource(device, desc, initialState,
		optimizedClearValue, riid, resource);
	if (SUCCEEDED(hr) && resource && *resource) {
		ID3D12Resource *d3dResource = nullptr;
		if (SUCCEEDED(static_cast<IUnknown*>(*resource)->QueryInterface(IID_PPV_ARGS(&d3dResource)))) {
			RecordResource(d3dResource, desc, nullptr, initialState);
			d3dResource->Release();
		}
	}
	return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreateCommittedResource1(
	ID3D12Device4 *device, const D3D12_HEAP_PROPERTIES *heapProperties,
	D3D12_HEAP_FLAGS heapFlags, const D3D12_RESOURCE_DESC *desc,
	D3D12_RESOURCE_STATES initialState, const D3D12_CLEAR_VALUE *optimizedClearValue,
	ID3D12ProtectedResourceSession *protectedSession, REFIID riid, void **resource)
{
	HRESULT hr = gOrigCreateCommittedResource1(device, heapProperties, heapFlags, desc,
		initialState, optimizedClearValue, protectedSession, riid, resource);
	if (SUCCEEDED(hr) && resource && *resource) {
		ID3D12Resource *d3dResource = nullptr;
		if (SUCCEEDED(static_cast<IUnknown*>(*resource)->QueryInterface(IID_PPV_ARGS(&d3dResource)))) {
			RecordResource(d3dResource, desc, heapProperties, initialState);
			d3dResource->Release();
		}
	}
	return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreateReservedResource1(
	ID3D12Device4 *device, const D3D12_RESOURCE_DESC *desc,
	D3D12_RESOURCE_STATES initialState, const D3D12_CLEAR_VALUE *optimizedClearValue,
	ID3D12ProtectedResourceSession *protectedSession, REFIID riid, void **resource)
{
	HRESULT hr = gOrigCreateReservedResource1(device, desc, initialState,
		optimizedClearValue, protectedSession, riid, resource);
	if (SUCCEEDED(hr) && resource && *resource) {
		ID3D12Resource *d3dResource = nullptr;
		if (SUCCEEDED(static_cast<IUnknown*>(*resource)->QueryInterface(IID_PPV_ARGS(&d3dResource)))) {
			RecordResource(d3dResource, desc, nullptr, initialState);
			d3dResource->Release();
		}
	}
	return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreateCommittedResource2(
	ID3D12Device8 *device, const D3D12_HEAP_PROPERTIES *heapProperties,
	D3D12_HEAP_FLAGS heapFlags, const D3D12_RESOURCE_DESC1 *desc,
	D3D12_RESOURCE_STATES initialState, const D3D12_CLEAR_VALUE *optimizedClearValue,
	ID3D12ProtectedResourceSession *protectedSession, REFIID riid, void **resource)
{
	HRESULT hr = gOrigCreateCommittedResource2(device, heapProperties, heapFlags, desc,
		initialState, optimizedClearValue, protectedSession, riid, resource);
	if (SUCCEEDED(hr) && resource && *resource) {
		ID3D12Resource *d3dResource = nullptr;
		if (SUCCEEDED(static_cast<IUnknown*>(*resource)->QueryInterface(IID_PPV_ARGS(&d3dResource)))) {
			D3D12_RESOURCE_DESC desc0 = ResourceDescFromDesc1(desc);
			RecordResource(d3dResource, desc ? &desc0 : nullptr, heapProperties, initialState);
			d3dResource->Release();
		}
	}
	return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreatePlacedResource1(
	ID3D12Device8 *device, ID3D12Heap *heap, UINT64 heapOffset,
	const D3D12_RESOURCE_DESC1 *desc, D3D12_RESOURCE_STATES initialState,
	const D3D12_CLEAR_VALUE *optimizedClearValue, REFIID riid, void **resource)
{
	HRESULT hr = gOrigCreatePlacedResource1(device, heap, heapOffset, desc,
		initialState, optimizedClearValue, riid, resource);
	if (SUCCEEDED(hr) && resource && *resource) {
		ID3D12Resource *d3dResource = nullptr;
		if (SUCCEEDED(static_cast<IUnknown*>(*resource)->QueryInterface(IID_PPV_ARGS(&d3dResource)))) {
			D3D12_RESOURCE_DESC desc0 = ResourceDescFromDesc1(desc);
			RecordResource(d3dResource, desc ? &desc0 : nullptr, nullptr, initialState);
			d3dResource->Release();
		}
	}
	return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreateCommittedResource3(
	ID3D12Device10 *device, const D3D12_HEAP_PROPERTIES *heapProperties,
	D3D12_HEAP_FLAGS heapFlags, const D3D12_RESOURCE_DESC1 *desc,
	D3D12_BARRIER_LAYOUT initialLayout, const D3D12_CLEAR_VALUE *optimizedClearValue,
	ID3D12ProtectedResourceSession *protectedSession, UINT32 numCastableFormats,
	const DXGI_FORMAT *castableFormats, REFIID riid, void **resource)
{
	HRESULT hr = gOrigCreateCommittedResource3(device, heapProperties, heapFlags, desc,
		initialLayout, optimizedClearValue, protectedSession, numCastableFormats,
		castableFormats, riid, resource);
	if (SUCCEEDED(hr) && resource && *resource) {
		ID3D12Resource *d3dResource = nullptr;
		if (SUCCEEDED(static_cast<IUnknown*>(*resource)->QueryInterface(IID_PPV_ARGS(&d3dResource)))) {
			D3D12_RESOURCE_DESC desc0 = ResourceDescFromDesc1(desc);
			RecordResource(d3dResource, desc ? &desc0 : nullptr, heapProperties,
				ResourceStateFromLayout(initialLayout));
			d3dResource->Release();
		}
	}
	return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreatePlacedResource2(
	ID3D12Device10 *device, ID3D12Heap *heap, UINT64 heapOffset,
	const D3D12_RESOURCE_DESC1 *desc, D3D12_BARRIER_LAYOUT initialLayout,
	const D3D12_CLEAR_VALUE *optimizedClearValue, UINT32 numCastableFormats,
	const DXGI_FORMAT *castableFormats, REFIID riid, void **resource)
{
	HRESULT hr = gOrigCreatePlacedResource2(device, heap, heapOffset, desc,
		initialLayout, optimizedClearValue, numCastableFormats, castableFormats,
		riid, resource);
	if (SUCCEEDED(hr) && resource && *resource) {
		ID3D12Resource *d3dResource = nullptr;
		if (SUCCEEDED(static_cast<IUnknown*>(*resource)->QueryInterface(IID_PPV_ARGS(&d3dResource)))) {
			D3D12_RESOURCE_DESC desc0 = ResourceDescFromDesc1(desc);
			RecordResource(d3dResource, desc ? &desc0 : nullptr, nullptr,
				ResourceStateFromLayout(initialLayout));
			d3dResource->Release();
		}
	}
	return hr;
}

static HRESULT STDMETHODCALLTYPE HookedCreateReservedResource2(
	ID3D12Device10 *device, const D3D12_RESOURCE_DESC *desc,
	D3D12_BARRIER_LAYOUT initialLayout, const D3D12_CLEAR_VALUE *optimizedClearValue,
	ID3D12ProtectedResourceSession *protectedSession, UINT32 numCastableFormats,
	const DXGI_FORMAT *castableFormats, REFIID riid, void **resource)
{
	HRESULT hr = gOrigCreateReservedResource2(device, desc, initialLayout,
		optimizedClearValue, protectedSession, numCastableFormats, castableFormats,
		riid, resource);
	if (SUCCEEDED(hr) && resource && *resource) {
		ID3D12Resource *d3dResource = nullptr;
		if (SUCCEEDED(static_cast<IUnknown*>(*resource)->QueryInterface(IID_PPV_ARGS(&d3dResource)))) {
			RecordResource(d3dResource, desc, nullptr, ResourceStateFromLayout(initialLayout));
			d3dResource->Release();
		}
	}
	return hr;
}

void DX12HookResourceMetadata(ID3D12Device *device)
{
	if (!device)
		return;

	if (!gCleanupRegistered) {
		atexit(CleanupTrackedResources);
		gCleanupRegistered = true;
		DX12Log("Resource metadata tracking uses non-owning resource pointers\n");
	}

	void **vtable = *reinterpret_cast<void***>(device);
	if (!vtable)
		return;

	DX12HookFunction(reinterpret_cast<void**>(&gOrigCreateDescriptorHeap),
		vtable[14], HookedCreateDescriptorHeap, "ID3D12Device::CreateDescriptorHeap");
	DX12HookFunction(reinterpret_cast<void**>(&gOrigCreateRootSignature),
		vtable[16], HookedCreateRootSignature, "ID3D12Device::CreateRootSignature");
	DX12HookFunction(reinterpret_cast<void**>(&gOrigCreateConstantBufferView),
		vtable[17], HookedCreateConstantBufferView, "ID3D12Device::CreateConstantBufferView");
	DX12HookFunction(reinterpret_cast<void**>(&gOrigCreateShaderResourceView),
		vtable[18], HookedCreateShaderResourceView, "ID3D12Device::CreateShaderResourceView");
	DX12HookFunction(reinterpret_cast<void**>(&gOrigCreateUnorderedAccessView),
		vtable[19], HookedCreateUnorderedAccessView, "ID3D12Device::CreateUnorderedAccessView");
	DX12HookFunction(reinterpret_cast<void**>(&gOrigCreateRenderTargetView),
		vtable[20], HookedCreateRenderTargetView, "ID3D12Device::CreateRenderTargetView");
	DX12HookFunction(reinterpret_cast<void**>(&gOrigCreateDepthStencilView),
		vtable[21], HookedCreateDepthStencilView, "ID3D12Device::CreateDepthStencilView");
	DX12HookFunction(reinterpret_cast<void**>(&gOrigCreateSampler),
		vtable[22], HookedCreateSampler, "ID3D12Device::CreateSampler");
	DX12HookFunction(reinterpret_cast<void**>(&gOrigCopyDescriptors),
		vtable[23], HookedCopyDescriptors, "ID3D12Device::CopyDescriptors");
	DX12HookFunction(reinterpret_cast<void**>(&gOrigCopyDescriptorsSimple),
		vtable[24], HookedCopyDescriptorsSimple, "ID3D12Device::CopyDescriptorsSimple");
	DX12HookFunction(reinterpret_cast<void**>(&gOrigCreateCommittedResource),
		vtable[27], HookedCreateCommittedResource, "ID3D12Device::CreateCommittedResource");
	DX12HookFunction(reinterpret_cast<void**>(&gOrigCreatePlacedResource),
		vtable[29], HookedCreatePlacedResource, "ID3D12Device::CreatePlacedResource");
	DX12HookFunction(reinterpret_cast<void**>(&gOrigCreateReservedResource),
		vtable[30], HookedCreateReservedResource, "ID3D12Device::CreateReservedResource");

	ID3D12Device4 *device4 = nullptr;
	if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device4)))) {
		void **vtable4 = *reinterpret_cast<void***>(device4);
		if (vtable4) {
			DX12HookFunction(reinterpret_cast<void**>(&gOrigCreateCommittedResource1),
				vtable4[53], HookedCreateCommittedResource1,
				"ID3D12Device4::CreateCommittedResource1");
			DX12HookFunction(reinterpret_cast<void**>(&gOrigCreateReservedResource1),
				vtable4[55], HookedCreateReservedResource1,
				"ID3D12Device4::CreateReservedResource1");
		}
		device4->Release();
	}

	ID3D12Device8 *device8 = nullptr;
	if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device8)))) {
		void **vtable8 = *reinterpret_cast<void***>(device8);
		if (vtable8) {
			DX12HookFunction(reinterpret_cast<void**>(&gOrigCreateCommittedResource2),
				vtable8[69], HookedCreateCommittedResource2,
				"ID3D12Device8::CreateCommittedResource2");
			DX12HookFunction(reinterpret_cast<void**>(&gOrigCreatePlacedResource1),
				vtable8[70], HookedCreatePlacedResource1,
				"ID3D12Device8::CreatePlacedResource1");
		}
		device8->Release();
	}

	ID3D12Device10 *device10 = nullptr;
	if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&device10)))) {
		void **vtable10 = *reinterpret_cast<void***>(device10);
		if (vtable10) {
			DX12HookFunction(reinterpret_cast<void**>(&gOrigCreateCommittedResource3),
				vtable10[76], HookedCreateCommittedResource3,
				"ID3D12Device10::CreateCommittedResource3");
			DX12HookFunction(reinterpret_cast<void**>(&gOrigCreatePlacedResource2),
				vtable10[77], HookedCreatePlacedResource2,
				"ID3D12Device10::CreatePlacedResource2");
			DX12HookFunction(reinterpret_cast<void**>(&gOrigCreateReservedResource2),
				vtable10[78], HookedCreateReservedResource2,
				"ID3D12Device10::CreateReservedResource2");
		}
		device10->Release();
	}
}

void DX12RecordPsoRootSignature(
	UINT64 psoIndex, const char *kind, ID3D12PipelineState *pipelineState,
	ID3D12RootSignature *rootSignature)
{
	PsoRootRecord record;
	record.psoIndex = psoIndex;
	record.kind = kind ? kind : "";
	record.pipelineState = pipelineState;
	record.rootSignature = rootSignature;

	AcquireSRWLockExclusive(&gResourceLock);
	gPsoRoots.push_back(record);
	ReleaseSRWLockExclusive(&gResourceLock);
}

void DX12RecordResourceBarrier(UINT numBarriers, const D3D12_RESOURCE_BARRIER *barriers)
{
	if (!barriers || numBarriers == 0)
		return;

	AcquireSRWLockExclusive(&gResourceLock);
	for (UINT i = 0; i < numBarriers; ++i) {
		const D3D12_RESOURCE_BARRIER &barrier = barriers[i];
		if (barrier.Type != D3D12_RESOURCE_BARRIER_TYPE_TRANSITION ||
			!barrier.Transition.pResource)
			continue;

		ID3D12Resource *canonical = CanonicalResource(barrier.Transition.pResource);
		auto found = gResourceByPtr.find(canonical);
		if (found == gResourceByPtr.end() || found->second >= gResources.size())
			continue;

		ResourceRecord &record = gResources[found->second];
		if (barrier.Transition.Subresource == D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES ||
			record.desc.Dimension == D3D12_RESOURCE_DIMENSION_BUFFER) {
			record.currentState = barrier.Transition.StateAfter;
			record.hasCurrentState = true;
			UpdateDescriptorResourceStateLocked(
				record.resource, record.currentState, record.hasCurrentState);
		} else {
			record.hasCurrentState = false;
			UpdateDescriptorResourceStateLocked(
				record.resource, record.currentState, record.hasCurrentState);
		}
	}
	ReleaseSRWLockExclusive(&gResourceLock);
}

bool DX12ResolveBufferResourceByGpuVa(
	UINT64 gpuVirtualAddress, UINT64 size, DX12BufferResourceSummary *summary)
{
	if (!summary || gpuVirtualAddress == 0)
		return false;

	*summary = DX12BufferResourceSummary();
	bool resolved = false;
	AcquireSRWLockShared(&gResourceLock);
	for (const ResourceRecord &resource : gResources) {
		if (resource.desc.Dimension != D3D12_RESOURCE_DIMENSION_BUFFER ||
			resource.gpuVirtualAddress == 0 || resource.size == 0)
			continue;
		const UINT64 begin = resource.gpuVirtualAddress;
		const UINT64 end = begin + resource.size;
		const UINT64 requestedEnd = gpuVirtualAddress + size;
		if (gpuVirtualAddress >= begin && requestedEnd <= end) {
			summary->resource = resource.resource;
			summary->resourceDesc = resource.desc;
			summary->hasResourceDesc = true;
			summary->gpuVirtualAddress = resource.gpuVirtualAddress;
			summary->resourceOffset = gpuVirtualAddress - begin;
			summary->viewSize = size;
			if (resource.hasHeapType) {
				summary->resourceHeapType = static_cast<UINT>(resource.heapType);
				summary->hasResourceHeapType = true;
			}
			summary->currentState = static_cast<UINT>(resource.currentState);
			summary->hasCurrentState = resource.hasCurrentState;
			resolved = true;
			break;
		}
	}
	ReleaseSRWLockShared(&gResourceLock);
	return resolved;
}

static void FillDescriptorSummary(DX12DescriptorSummary *summary, const DescriptorRecord &record)
{
	if (!summary)
		return;

	summary->kind = record.kind;
	summary->cpuHandle = record.cpuHandle;
	summary->resource = record.resource;
	summary->counterResource = record.counterResource;
	summary->resourceDesc = record.resourceDesc;
	summary->hasResourceDesc = record.hasResourceDesc;
	summary->resourceHeapType = record.resourceHeapType;
	summary->hasResourceHeapType = record.hasResourceHeapType;
	summary->gpuVirtualAddress = record.gpuVirtualAddress;
	summary->resourceOffset = record.resourceOffset;
	summary->viewSize = record.viewSize;
	summary->currentState = static_cast<UINT>(record.currentState);
	summary->hasCurrentState = record.hasCurrentState;
	summary->hasDesc = record.hasDesc;

	if (record.kind == "CBV") {
		summary->cbvSize = record.cbv.SizeInBytes;
	} else if (record.kind == "SRV" && record.hasDesc) {
		summary->viewFormat = static_cast<UINT>(record.srv.Format);
		summary->viewDimension = static_cast<UINT>(record.srv.ViewDimension);
		if (record.srv.ViewDimension == D3D12_SRV_DIMENSION_BUFFER) {
			summary->firstElement = record.srv.Buffer.FirstElement;
			summary->numElements = record.srv.Buffer.NumElements;
			summary->structureByteStride = record.srv.Buffer.StructureByteStride;
			summary->bufferViewOffset = record.resourceOffset;
			summary->bufferViewBytes = record.viewSize;
		}
	} else if (record.kind == "UAV" && record.hasDesc) {
		summary->viewFormat = static_cast<UINT>(record.uav.Format);
		summary->viewDimension = static_cast<UINT>(record.uav.ViewDimension);
		if (record.uav.ViewDimension == D3D12_UAV_DIMENSION_BUFFER) {
			summary->firstElement = record.uav.Buffer.FirstElement;
			summary->numElements = record.uav.Buffer.NumElements;
			summary->structureByteStride = record.uav.Buffer.StructureByteStride;
			summary->bufferViewOffset = record.resourceOffset;
			summary->bufferViewBytes = record.viewSize;
		}
	} else if (record.kind == "RTV" && record.hasDesc) {
		summary->viewFormat = static_cast<UINT>(record.rtv.Format);
		summary->viewDimension = static_cast<UINT>(record.rtv.ViewDimension);
	} else if (record.kind == "DSV" && record.hasDesc) {
		summary->viewFormat = static_cast<UINT>(record.dsv.Format);
		summary->viewDimension = static_cast<UINT>(record.dsv.ViewDimension);
	}
}

void DX12GetResourceMetadataSnapshot(
	std::vector<DX12RootSignatureSummary> *rootSignatures,
	std::vector<DX12DescriptorSummary> *descriptors,
	std::vector<DX12PsoRootSummary> *psoRoots)
{
	DX12GetResourceMetadataSnapshot(rootSignatures, descriptors, psoRoots, nullptr);
}

void DX12GetResourceMetadataSnapshot(
	std::vector<DX12RootSignatureSummary> *rootSignatures,
	std::vector<DX12DescriptorSummary> *descriptors,
	std::vector<DX12PsoRootSummary> *psoRoots,
	std::vector<DX12DescriptorHeapSummary> *descriptorHeaps)
{
	AcquireSRWLockShared(&gResourceLock);
	size_t descriptorStateKnown = 0;
	for (const DescriptorRecord &record : gDescriptors) {
		if (record.hasCurrentState)
			descriptorStateKnown++;
	}
	DX12FrameAnalysisLogInfo("Metadata snapshot: begin roots=%zu descriptors=%zu descriptorStates=%zu resources=%zu resourcesFromCreate=%llu psoRoots=%zu heaps=%zu requested=%u%u%u%u\n",
		gRootSignatures.size(), gDescriptors.size(), descriptorStateKnown,
		gResources.size(),
		static_cast<unsigned long long>(gResourcesRecordedFromCreate),
		gPsoRoots.size(), gDescriptorHeaps.size(),
		rootSignatures ? 1 : 0,
		descriptors ? 1 : 0,
		psoRoots ? 1 : 0,
		descriptorHeaps ? 1 : 0);

	if (rootSignatures) {
		rootSignatures->clear();
		rootSignatures->reserve(gRootSignatures.size());
		for (const RootSignatureRecord &record : gRootSignatures) {
			DX12RootSignatureSummary summary;
			summary.rootSignature = record.rootSignature;
			summary.hash = record.hash;
			summary.size = record.size;
			summary.nodeMask = record.nodeMask;
			rootSignatures->push_back(summary);
		}
		DX12FrameAnalysisLogInfo("Metadata snapshot: roots copied=%zu\n", rootSignatures->size());
	}

	if (descriptors) {
		descriptors->clear();
		descriptors->reserve(gDescriptors.size());
		for (const DescriptorRecord &record : gDescriptors) {
			DX12DescriptorSummary summary;
			FillDescriptorSummary(&summary, record);
			descriptors->push_back(summary);
		}
		DX12FrameAnalysisLogInfo("Metadata snapshot: descriptors copied=%zu\n", descriptors->size());
	}

	if (psoRoots) {
		psoRoots->clear();
		psoRoots->reserve(gPsoRoots.size());
		for (const PsoRootRecord &record : gPsoRoots) {
			DX12PsoRootSummary summary;
			summary.psoIndex = record.psoIndex;
			summary.kind = record.kind;
			summary.pipelineState = record.pipelineState;
			summary.rootSignature = record.rootSignature;
			psoRoots->push_back(summary);
		}
		DX12FrameAnalysisLogInfo("Metadata snapshot: psoRoots copied=%zu\n", psoRoots->size());
	}

	if (descriptorHeaps) {
		descriptorHeaps->clear();
		descriptorHeaps->reserve(gDescriptorHeaps.size());
		for (const DescriptorHeapRecord &record : gDescriptorHeaps) {
			DX12DescriptorHeapSummary summary;
			summary.heap = record.heap;
			summary.type = static_cast<UINT>(record.desc.Type);
			summary.numDescriptors = record.desc.NumDescriptors;
			summary.flags = static_cast<UINT>(record.desc.Flags);
			summary.nodeMask = record.desc.NodeMask;
			summary.cpuStart = record.cpuStart;
			summary.gpuStart = record.gpuStart;
			summary.increment = record.increment;
			descriptorHeaps->push_back(summary);
		}
		DX12FrameAnalysisLogInfo("Metadata snapshot: heaps copied=%zu\n", descriptorHeaps->size());
	}

	ReleaseSRWLockShared(&gResourceLock);
	DX12FrameAnalysisLogInfo("Metadata snapshot: complete\n");
}

static void WriteResourceDesc(FILE *file, const DescriptorRecord &record)
{
	if (!record.hasResourceDesc) {
		fprintf(file, ",,,,,,,,,");
		return;
	}

	const D3D12_RESOURCE_DESC &desc = record.resourceDesc;
	fprintf(file, ",%s,%llu,%u,%u,%u,%u,0x%llx,0x%llx,%llu",
		ResourceDimensionName(desc.Dimension),
		static_cast<unsigned long long>(desc.Width),
		desc.Height,
		desc.DepthOrArraySize,
		desc.MipLevels,
		static_cast<UINT>(desc.Format),
		static_cast<unsigned long long>(desc.Flags),
		static_cast<unsigned long long>(record.gpuVirtualAddress),
		static_cast<unsigned long long>(desc.SampleDesc.Count));
}

void DX12DumpResourceMetadata(const wchar_t *dir)
{
	if (!dir)
		return;

	std::vector<RootSignatureRecord> rootSignatures;
	std::vector<DescriptorHeapRecord> descriptorHeaps;
	std::vector<DescriptorRecord> descriptors;
	std::vector<PsoRootRecord> psoRoots;

	AcquireSRWLockShared(&gResourceLock);
	rootSignatures = gRootSignatures;
	descriptorHeaps = gDescriptorHeaps;
	descriptors = gDescriptors;
	psoRoots = gPsoRoots;
	ReleaseSRWLockShared(&gResourceLock);

	wchar_t path[MAX_PATH];
	wchar_t summaryDir[MAX_PATH];
	GetSummaryDirectory(dir, summaryDir, ARRAYSIZE(summaryDir));
	swprintf_s(path, L"%s\\ResourceMetadataDX12.txt", summaryDir);
	FILE *file = _wfsopen(path, L"w", _SH_DENYNO);
	if (!file)
		return;

	fprintf(file, "DX12 Resource Metadata\n");
	fprintf(file, "======================\n");
	fprintf(file, "root_signatures=%zu descriptor_heaps=%zu descriptors=%zu pso_roots=%zu\n\n",
		rootSignatures.size(), descriptorHeaps.size(), descriptors.size(), psoRoots.size());

	fprintf(file, "PSO Root Signatures\n");
	fprintf(file, "pso,kind,pipeline_state,root_signature\n");
	for (const PsoRootRecord &record : psoRoots) {
		fprintf(file, "%llu,%s,%p,%p\n",
			static_cast<unsigned long long>(record.psoIndex),
			record.kind.c_str(), record.pipelineState, record.rootSignature);
	}

	fprintf(file, "\nRoot Signatures\n");
	fprintf(file, "index,root_signature,hash,size,node_mask\n");
	for (size_t i = 0; i < rootSignatures.size(); ++i) {
		const RootSignatureRecord &record = rootSignatures[i];
		fprintf(file, "%zu,%p,%016llx,%zu,%u\n",
			i,
			record.rootSignature,
			static_cast<unsigned long long>(record.hash),
			record.size,
			record.nodeMask);
	}

	fprintf(file, "\nDescriptor Heaps\n");
	fprintf(file, "index,heap,type,num_descriptors,flags,node_mask,cpu_start,gpu_start,increment\n");
	for (size_t i = 0; i < descriptorHeaps.size(); ++i) {
		const DescriptorHeapRecord &record = descriptorHeaps[i];
		fprintf(file, "%zu,%p,%s,%u,0x%x,%u,0x%llx,0x%llx,%u\n",
			i,
			record.heap,
			DescriptorHeapTypeName(record.desc.Type),
			record.desc.NumDescriptors,
			static_cast<UINT>(record.desc.Flags),
			record.desc.NodeMask,
			static_cast<unsigned long long>(record.cpuStart),
			static_cast<unsigned long long>(record.gpuStart),
			record.increment);
	}

	fprintf(file, "\nDescriptors\n");
	fprintf(file,
		"index,kind,cpu_handle,resource,counter_resource,resource_dimension,width,height,depth_or_array,mips,format,flags,gpu_va,sample_count,resource_offset,view_size,heap_type,current_state,has_current_state,has_view_desc,view_format,view_dimension,cbv_size,first_element,num_elements,stride,buffer_view_offset,buffer_view_bytes\n");
	for (size_t i = 0; i < descriptors.size(); ++i) {
		const DescriptorRecord &record = descriptors[i];
		fprintf(file, "%zu,%s,0x%llx,%p,%p",
			i,
			record.kind.c_str(),
			static_cast<unsigned long long>(record.cpuHandle),
			record.resource,
			record.counterResource);
		WriteResourceDesc(file, record);

		UINT viewFormat = 0;
		UINT viewDimension = 0;
		UINT cbvSize = 0;
		UINT64 firstElement = 0;
		UINT numElements = 0;
		UINT stride = 0;
		UINT64 bufferViewOffset = 0;
		UINT64 bufferViewBytes = 0;
		if (record.kind == "CBV") {
			cbvSize = record.cbv.SizeInBytes;
		} else if (record.kind == "SRV" && record.hasDesc) {
			viewFormat = static_cast<UINT>(record.srv.Format);
			viewDimension = static_cast<UINT>(record.srv.ViewDimension);
			if (record.srv.ViewDimension == D3D12_SRV_DIMENSION_BUFFER) {
				firstElement = record.srv.Buffer.FirstElement;
				numElements = record.srv.Buffer.NumElements;
				stride = record.srv.Buffer.StructureByteStride;
				bufferViewOffset = record.resourceOffset;
				bufferViewBytes = record.viewSize;
			}
		} else if (record.kind == "UAV" && record.hasDesc) {
			viewFormat = static_cast<UINT>(record.uav.Format);
			viewDimension = static_cast<UINT>(record.uav.ViewDimension);
			if (record.uav.ViewDimension == D3D12_UAV_DIMENSION_BUFFER) {
				firstElement = record.uav.Buffer.FirstElement;
				numElements = record.uav.Buffer.NumElements;
				stride = record.uav.Buffer.StructureByteStride;
				bufferViewOffset = record.resourceOffset;
				bufferViewBytes = record.viewSize;
			}
		} else if (record.kind == "RTV" && record.hasDesc) {
			viewFormat = static_cast<UINT>(record.rtv.Format);
			viewDimension = static_cast<UINT>(record.rtv.ViewDimension);
		} else if (record.kind == "DSV" && record.hasDesc) {
			viewFormat = static_cast<UINT>(record.dsv.Format);
			viewDimension = static_cast<UINT>(record.dsv.ViewDimension);
		}

		fprintf(file, ",%llu,%llu,%u,0x%x,%u,%u,%u,%u,%u,%llu,%u,%u,%llu,%llu\n",
			static_cast<unsigned long long>(record.resourceOffset),
			static_cast<unsigned long long>(record.viewSize),
			record.hasResourceHeapType ? record.resourceHeapType : 0,
			static_cast<UINT>(record.currentState),
			record.hasCurrentState ? 1 : 0,
			record.hasDesc ? 1 : 0,
			viewFormat,
			viewDimension,
			cbvSize,
			static_cast<unsigned long long>(firstElement),
			numElements,
			stride,
			static_cast<unsigned long long>(bufferViewOffset),
			static_cast<unsigned long long>(bufferViewBytes));
	}

	fclose(file);
	DX12FrameAnalysisLogInfo("Resource metadata written: %S roots=%zu heaps=%zu descriptors=%zu psoRoots=%zu\n",
		path, rootSignatures.size(), descriptorHeaps.size(), descriptors.size(), psoRoots.size());
}
