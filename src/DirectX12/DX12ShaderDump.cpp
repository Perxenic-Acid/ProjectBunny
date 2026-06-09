#include "DX12ShaderDump.h"

#include <Shlwapi.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "DX12State.h"

struct ShaderRecord
{
	std::string stage;
	uint64_t hash = 0;
	std::vector<uint8_t> bytecode;
	UINT64 firstPsoIndex = 0;
	UINT64 useCount = 0;
};

struct PsoRecord
{
	std::string kind;
	UINT64 index = 0;
	std::vector<std::string> shaders;
};

static SRWLOCK gDumpLock = SRWLOCK_INIT;
static std::unordered_map<std::string, ShaderRecord> gShaders;
static std::vector<PsoRecord> gPsos;
static UINT64 gGraphicsPsoCount = 0;
static UINT64 gComputePsoCount = 0;

static uint64_t Fnv1a64(const void *data, size_t size)
{
	const uint8_t *bytes = static_cast<const uint8_t*>(data);
	uint64_t hash = 14695981039346656037ull;
	for (size_t i = 0; i < size; ++i) {
		hash ^= bytes[i];
		hash *= 1099511628211ull;
	}
	return hash;
}

static std::string MakeShaderKey(const char *stage, uint64_t hash)
{
	char key[64];
	sprintf_s(key, "%s_%016llx", stage, static_cast<unsigned long long>(hash));
	return key;
}

static bool GetDumpDirectory(wchar_t *path, size_t pathCount)
{
	if (!GetModuleFileNameW(DX12GetModule(), path, static_cast<DWORD>(pathCount)))
		return false;
	PathRemoveFileSpecW(path);
	PathAppendW(path, L"ShaderDumpDX12");
	return CreateDirectoryW(path, nullptr) || GetLastError() == ERROR_ALREADY_EXISTS;
}

static void RecordShaderLocked(
	const D3D12_SHADER_BYTECODE &bytecode, const char *stage,
	UINT64 psoIndex, PsoRecord &pso)
{
	if (!bytecode.pShaderBytecode || bytecode.BytecodeLength == 0)
		return;

	uint64_t hash = Fnv1a64(bytecode.pShaderBytecode, bytecode.BytecodeLength);
	std::string key = MakeShaderKey(stage, hash);
	pso.shaders.push_back(key);

	auto it = gShaders.find(key);
	if (it == gShaders.end()) {
		ShaderRecord record;
		record.stage = stage;
		record.hash = hash;
		record.bytecode.resize(bytecode.BytecodeLength);
		memcpy(record.bytecode.data(), bytecode.pShaderBytecode, bytecode.BytecodeLength);
		record.firstPsoIndex = psoIndex;
		record.useCount = 1;
		gShaders.emplace(key, std::move(record));
	} else {
		it->second.useCount++;
	}
}

static bool WriteShaderFile(const wchar_t *dir, const ShaderRecord &record)
{
	wchar_t path[MAX_PATH];
	swprintf_s(path, L"%s\\%016llx-%S.bin",
		dir, static_cast<unsigned long long>(record.hash), record.stage.c_str());

	FILE *file = _wfsopen(path, L"wb", _SH_DENYNO);
	if (!file)
		return false;
	fwrite(record.bytecode.data(), 1, record.bytecode.size(), file);
	fclose(file);
	return true;
}

void DX12RecordGraphicsPipelineState(const D3D12_GRAPHICS_PIPELINE_STATE_DESC *desc)
{
	if (!desc)
		return;

	AcquireSRWLockExclusive(&gDumpLock);
	UINT64 psoIndex = ++gGraphicsPsoCount + gComputePsoCount;
	PsoRecord pso;
	pso.kind = "graphics";
	pso.index = psoIndex;
	RecordShaderLocked(desc->VS, "vs", psoIndex, pso);
	RecordShaderLocked(desc->PS, "ps", psoIndex, pso);
	RecordShaderLocked(desc->DS, "ds", psoIndex, pso);
	RecordShaderLocked(desc->HS, "hs", psoIndex, pso);
	RecordShaderLocked(desc->GS, "gs", psoIndex, pso);
	const size_t shadersInPso = pso.shaders.size();
	gPsos.push_back(std::move(pso));
	UINT64 shaderCount = gShaders.size();
	UINT64 psoCount = gPsos.size();
	ReleaseSRWLockExclusive(&gDumpLock);

	DX12Log("Recorded graphics PSO #%llu shaders=%llu cachedShaders=%llu cachedPSOs=%llu\n",
		static_cast<unsigned long long>(psoIndex),
		static_cast<unsigned long long>(shadersInPso),
		static_cast<unsigned long long>(shaderCount),
		static_cast<unsigned long long>(psoCount));
	DX12SetOverlayStatus(L"3DMigoto DX12 hook alive | cached shaders ready");
}

void DX12RecordComputePipelineState(const D3D12_COMPUTE_PIPELINE_STATE_DESC *desc)
{
	if (!desc)
		return;

	AcquireSRWLockExclusive(&gDumpLock);
	UINT64 psoIndex = gGraphicsPsoCount + ++gComputePsoCount;
	PsoRecord pso;
	pso.kind = "compute";
	pso.index = psoIndex;
	RecordShaderLocked(desc->CS, "cs", psoIndex, pso);
	gPsos.push_back(std::move(pso));
	UINT64 shaderCount = gShaders.size();
	UINT64 psoCount = gPsos.size();
	ReleaseSRWLockExclusive(&gDumpLock);

	DX12Log("Recorded compute PSO #%llu cachedShaders=%llu cachedPSOs=%llu\n",
		static_cast<unsigned long long>(psoIndex),
		static_cast<unsigned long long>(shaderCount),
		static_cast<unsigned long long>(psoCount));
	DX12SetOverlayStatus(L"3DMigoto DX12 hook alive | cached shaders ready");
}

void DX12DumpCachedShaders()
{
	wchar_t dir[MAX_PATH];
	if (!GetDumpDirectory(dir, ARRAYSIZE(dir))) {
		DX12Log("Failed to create ShaderDumpDX12 directory\n");
		DX12SetOverlayStatus(L"F8 dump failed: cannot create directory");
		return;
	}

	DX12SetOverlayStatus(L"F8 dump requested");

	AcquireSRWLockShared(&gDumpLock);
	std::vector<ShaderRecord> shaders;
	std::vector<PsoRecord> psos = gPsos;
	shaders.reserve(gShaders.size());
	for (const auto &item : gShaders)
		shaders.push_back(item.second);
	ReleaseSRWLockShared(&gDumpLock);

	UINT writtenShaders = 0;
	for (const ShaderRecord &shader : shaders) {
		if (WriteShaderFile(dir, shader))
			writtenShaders++;
	}

	wchar_t usagePath[MAX_PATH];
	swprintf_s(usagePath, L"%s\\ShaderUsage.txt", dir);
	FILE *usage = _wfsopen(usagePath, L"w", _SH_DENYNO);
	if (usage) {
		fprintf(usage, "hash,stage,size,uses,first_pso,file\n");
		for (const ShaderRecord &shader : shaders) {
			fprintf(usage, "%016llx,%s,%zu,%llu,%llu,%016llx-%s.bin\n",
				static_cast<unsigned long long>(shader.hash),
				shader.stage.c_str(),
				shader.bytecode.size(),
				static_cast<unsigned long long>(shader.useCount),
				static_cast<unsigned long long>(shader.firstPsoIndex),
				static_cast<unsigned long long>(shader.hash),
				shader.stage.c_str());
		}
		fclose(usage);
	}

	wchar_t psoPath[MAX_PATH];
	swprintf_s(psoPath, L"%s\\pso_log.txt", dir);
	FILE *psoLog = _wfsopen(psoPath, L"w", _SH_DENYNO);
	if (psoLog) {
		for (const PsoRecord &pso : psos) {
			fprintf(psoLog, "pso=%llu kind=%s shaders=",
				static_cast<unsigned long long>(pso.index), pso.kind.c_str());
			for (size_t i = 0; i < pso.shaders.size(); ++i) {
				fprintf(psoLog, "%s%s", i ? ";" : "", pso.shaders[i].c_str());
			}
			fprintf(psoLog, "\n");
		}
		fclose(psoLog);
	}

	wchar_t status[128];
	swprintf_s(status, L"F8 dumped %u shaders / %zu PSOs", writtenShaders, psos.size());
	DX12SetOverlayStatus(status);
	DX12Log("F8 shader dump complete: dir=%S shaders=%u/%zu psos=%zu\n",
		dir, writtenShaders, shaders.size(), psos.size());
}
