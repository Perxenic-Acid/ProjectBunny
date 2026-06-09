#pragma once

#include <d3d12.h>

void DX12RecordGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc);
void DX12RecordComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc);
void DX12DumpCachedShaders();
