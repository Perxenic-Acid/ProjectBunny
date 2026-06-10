<script setup lang="ts">
import { computed, ref } from "vue";
import { invoke } from "@tauri-apps/api/core";
import {
  CircleCheck,
  FolderOpened,
  Refresh,
  Upload,
  Warning,
} from "@element-plus/icons-vue";
import DataTable from "./components/DataTable.vue";

type Diagnostic = {
  level: "info" | "warning" | "error" | string;
  message: string;
  detail: string;
};

type BufferPreview = {
  bufferId: string;
  role: string;
  slot: number;
  stride: number;
  size: number;
  format: string;
  file: string;
  exists: boolean;
};

type DrawCandidate = {
  drawId: number;
  pso: string;
  vs: string;
  ps: string;
  topology: string;
  indexCount: number;
  firstIndex: number;
  baseVertex: number;
  ib?: BufferPreview | null;
  vbs: BufferPreview[];
  exportable: boolean;
  confidence: string;
  hasPositionInput: boolean;
  vsInputs: string[];
  notes: string[];
};

type DumpSummary = {
  rootFileCount: number;
  shaderFileCount: number;
  resourceFileCount: number;
  resourceBytes: number;
  drawRows: number;
  drawIndexedRows: number;
  exportableDraws: number;
  defaultExportableDraws: number;
  skippedDraws: number;
  vertexBuffers: number;
  indexBuffers: number;
  missingBufferFiles: number;
  bindingEvents: number;
};

type DumpInspection = {
  dumpDir: string;
  outputDir: string;
  exists: boolean;
  status: string;
  canExport: boolean;
  summary: DumpSummary;
  diagnostics: Diagnostic[];
  drawCandidates: DrawCandidate[];
};

type ExportOptions = {
  gamePreset: string;
  gameType: string;
  lodName: string;
  overwrite: boolean;
  maxDraws: number;
  includeLowConfidence: boolean;
};

type ExportResult = {
  outputDir: string;
  exported: number;
  skipped: number;
  diagnostics: Diagnostic[];
  exportedSubmeshes: string[];
};

const defaultDumpDir = "D:\\Softs\\Steam\\steamapps\\common\\Slay the Spire 2\\ShaderDumpDX12";

const dumpDir = ref(defaultDumpDir);
const outputDir = ref("");
const inspection = ref<DumpInspection | null>(null);
const exportResult = ref<ExportResult | null>(null);
const busy = ref(false);
const exporting = ref(false);
const errorText = ref("");
const activeTab = ref("diagnostics");

const options = ref<ExportOptions>({
  gamePreset: "ProjectBunnyDX12",
  gameType: "DX12_IA_Inferred",
  lodName: "LOD0",
  overwrite: true,
  maxDraws: 100,
  includeLowConfidence: false,
});

const statusMeta = computed(() => {
  const status = inspection.value?.status ?? "idle";
  if (status === "ready") {
    return {
      label: "Ready",
      type: "success",
      icon: CircleCheck,
      text: "Found exportable indexed draws with copied IA buffers.",
    };
  }

  if (status === "incomplete") {
    return {
      label: "Incomplete",
      type: "warning",
      icon: Warning,
      text: "Draws exist, but default-safe Blender export is blocked by missing or low-confidence geometry data.",
    };
  }

  if (status === "missing_geometry") {
    return {
      label: "No Geometry",
      type: "danger",
      icon: Warning,
      text: "The dump does not contain usable draw-indexed geometry data.",
    };
  }

  if (status === "missing") {
    return {
      label: "Missing",
      type: "danger",
      icon: Warning,
      text: "The selected dump directory does not exist.",
    };
  }

  return {
    label: "Idle",
    type: "info",
    icon: FolderOpened,
    text: "Select or enter a DX12 dump directory to inspect.",
  };
});

const summaryCards = computed(() => {
  const summary = inspection.value?.summary;
  return [
    ["DrawIndexed", summary?.drawIndexedRows ?? 0, `${summary?.drawRows ?? 0} draw rows`],
    [
      "Default Export",
      summary?.defaultExportableDraws ?? 0,
      `${summary?.exportableDraws ?? 0} IA candidates`,
    ],
    ["VB / IB", `${summary?.vertexBuffers ?? 0} / ${summary?.indexBuffers ?? 0}`, "IA buffers"],
    ["Resources", summary?.resourceFileCount ?? 0, formatBytes(summary?.resourceBytes ?? 0)],
    ["Shaders", summary?.shaderFileCount ?? 0, "asm + bytecode"],
    ["Events", summary?.bindingEvents ?? 0, "binding trace"],
  ];
});

const diagnostics = computed(() => inspection.value?.diagnostics ?? []);

const candidateTable = computed(() => {
  const rows = (inspection.value?.drawCandidates ?? []).map((draw) => [
    draw.drawId,
    draw.exportable ? "yes" : "no",
    draw.confidence,
    draw.hasPositionInput ? "yes" : "no",
    draw.indexCount,
    draw.firstIndex,
    draw.pso,
    draw.vs,
    draw.ps,
    draw.ib?.bufferId ?? "",
    draw.vbs.map((vb) => `${vb.slot}:${vb.bufferId}/${vb.stride}`).join("; "),
    draw.vsInputs.join("; "),
    draw.notes.join("; "),
  ]);

  return {
    columns: [
      "draw_id",
      "exportable",
      "confidence",
      "position_input",
      "index_count",
      "first_index",
      "pso",
      "vs",
      "ps",
      "ib",
      "vbs",
      "vs_inputs",
      "notes",
    ],
    rows,
  };
});

const exportDisabled = computed(() => {
  if (!inspection.value || exporting.value) {
    return true;
  }

  if (options.value.includeLowConfidence) {
    return inspection.value.summary.exportableDraws === 0;
  }

  return !inspection.value.canExport;
});

const formatNumber = (value: unknown) => Number(value ?? 0).toLocaleString();

const formatBytes = (value: unknown) => {
  const bytes = Number(value ?? 0);
  if (!Number.isFinite(bytes) || bytes <= 0) {
    return "0 B";
  }

  const units = ["B", "KB", "MB", "GB"];
  let size = bytes;
  let unit = 0;
  while (size >= 1024 && unit < units.length - 1) {
    size /= 1024;
    unit += 1;
  }

  return `${size.toFixed(unit === 0 ? 0 : 1)} ${units[unit]}`;
};

const runInspection = async () => {
  const path = dumpDir.value.trim();
  if (!path) {
    errorText.value = "Dump directory is empty.";
    return;
  }

  busy.value = true;
  errorText.value = "";
  exportResult.value = null;

  try {
    const result = await invoke<DumpInspection>("inspect_dump_directory", { path });
    inspection.value = result;
    dumpDir.value = result.dumpDir;
    if (!outputDir.value.trim()) {
      outputDir.value = result.outputDir;
    }
  } catch (error) {
    errorText.value = String(error);
  } finally {
    busy.value = false;
  }
};

const selectDumpDirectory = async () => {
  errorText.value = "";
  try {
    const selected = await invoke<string | null>("select_dump_directory");
    if (selected) {
      dumpDir.value = selected;
      await runInspection();
    }
  } catch (error) {
    errorText.value = String(error);
  }
};

const selectOutputDirectory = async () => {
  errorText.value = "";
  try {
    const selected = await invoke<string | null>("select_output_directory");
    if (selected) {
      outputDir.value = selected;
    }
  } catch (error) {
    errorText.value = String(error);
  }
};

const runExport = async () => {
  if (!dumpDir.value.trim() || !outputDir.value.trim()) {
    errorText.value = "Dump directory and output directory are required.";
    return;
  }

  exporting.value = true;
  errorText.value = "";
  exportResult.value = null;

  try {
    exportResult.value = await invoke<ExportResult>("export_blender_workspace", {
      dumpDir: dumpDir.value.trim(),
      outputDir: outputDir.value.trim(),
      options: options.value,
    });
  } catch (error) {
    errorText.value = String(error);
  } finally {
    exporting.value = false;
  }
};

runInspection();
</script>

<template>
  <main class="app-shell">
    <header class="app-header">
      <div>
        <p class="eyebrow">ProjectBunny DX12 dump</p>
        <h1>TheHerta4 Workspace Extractor</h1>
        <p class="subtext">
          Converts captured IA buffers into an SSMT-style workspace layout for TheHerta4 scanning.
        </p>
      </div>
      <el-tag :type="statusMeta.type" effect="plain" size="large">
        <el-icon><component :is="statusMeta.icon" /></el-icon>
        {{ statusMeta.label }}
      </el-tag>
    </header>

    <section class="control-panel">
      <div class="path-row">
        <label>Dump Directory</label>
        <el-input v-model="dumpDir" clearable @keyup.enter="runInspection" />
        <el-button :icon="FolderOpened" @click="selectDumpDirectory">Select</el-button>
        <el-button :icon="Refresh" :loading="busy" type="primary" @click="runInspection">
          Inspect
        </el-button>
      </div>

      <div class="path-row">
        <label>Output Workspace</label>
        <el-input v-model="outputDir" clearable />
        <el-button :icon="FolderOpened" @click="selectOutputDirectory">Select</el-button>
        <el-button :icon="Upload" :disabled="exportDisabled" :loading="exporting" type="success" @click="runExport">
          Export
        </el-button>
      </div>

      <div class="option-grid">
        <el-form-item label="GamePreset">
          <el-input v-model="options.gamePreset" />
        </el-form-item>
        <el-form-item label="WorkGameType">
          <el-input v-model="options.gameType" />
        </el-form-item>
        <el-form-item label="LOD">
          <el-input v-model="options.lodName" />
        </el-form-item>
        <el-form-item label="Max Draws">
          <el-input-number v-model="options.maxDraws" :min="1" :max="500" controls-position="right" />
        </el-form-item>
        <el-checkbox v-model="options.overwrite">Overwrite output folder</el-checkbox>
        <el-checkbox v-model="options.includeLowConfidence">Include low-confidence draws</el-checkbox>
      </div>
    </section>

    <el-alert v-if="errorText" :title="errorText" type="error" show-icon class="alert" />

    <section class="status-panel">
      <div class="status-copy">
        <strong>{{ statusMeta.text }}</strong>
        <span>{{ inspection?.dumpDir || dumpDir }}</span>
      </div>
      <div class="metric-grid">
        <article v-for="card in summaryCards" :key="String(card[0])" class="metric-card">
          <span>{{ card[0] }}</span>
          <strong>{{ formatNumber(card[1]) }}</strong>
          <small>{{ card[2] }}</small>
        </article>
      </div>
    </section>

    <el-alert
      v-if="inspection && !inspection.canExport"
      title="Default export is blocked until the dump contains at least one medium-confidence draw with a POSITION-like vertex input. You can inspect low-confidence candidates below."
      type="warning"
      show-icon
      class="alert"
    />

    <section v-if="exportResult" class="result-panel">
      <div>
        <strong>Exported {{ exportResult.exported }} submesh(es)</strong>
        <span>{{ exportResult.outputDir }}</span>
      </div>
      <el-tag effect="plain">{{ exportResult.skipped }} skipped</el-tag>
    </section>

    <el-tabs v-model="activeTab" class="workspace-tabs">
      <el-tab-pane label="Diagnostics" name="diagnostics">
        <section class="diagnostic-list">
          <article v-for="item in diagnostics" :key="`${item.level}-${item.message}-${item.detail}`" class="diagnostic-item">
            <el-tag :type="item.level === 'error' ? 'danger' : item.level === 'warning' ? 'warning' : 'info'" effect="plain">
              {{ item.level }}
            </el-tag>
            <div>
              <strong>{{ item.message }}</strong>
              <span>{{ item.detail }}</span>
            </div>
          </article>
        </section>
      </el-tab-pane>

      <el-tab-pane label="Draw Candidates" name="draws">
        <DataTable
          title="Draw candidates"
          :columns="candidateTable.columns"
          :rows="candidateTable.rows"
          :height="620"
          :page-size="40"
          dense
        />
      </el-tab-pane>

      <el-tab-pane label="Export Result" name="result">
        <DataTable
          title="Exported submeshes"
          :columns="['submesh']"
          :rows="(exportResult?.exportedSubmeshes ?? []).map((item) => [item])"
          :height="520"
          :page-size="50"
          dense
        />
      </el-tab-pane>
    </el-tabs>
  </main>
</template>

<style>
:root {
  color: #172033;
  font-family:
    Inter, ui-sans-serif, system-ui, -apple-system, BlinkMacSystemFont, "Segoe UI", sans-serif;
  font-size: 16px;
  line-height: 1.5;
  text-rendering: optimizeLegibility;
  -webkit-font-smoothing: antialiased;
  -moz-osx-font-smoothing: grayscale;
}

* {
  box-sizing: border-box;
}

body {
  background: #edf1f5;
  margin: 0;
}

button,
input {
  font: inherit;
}

#app {
  min-height: 100vh;
}

.app-shell {
  margin: 0 auto;
  max-width: 1540px;
  padding: 28px;
}

.app-header {
  align-items: flex-start;
  display: flex;
  gap: 24px;
  justify-content: space-between;
  margin-bottom: 18px;
}

.eyebrow {
  color: #2f6f67;
  font-size: 12px;
  font-weight: 700;
  letter-spacing: 0;
  margin: 0 0 4px;
  text-transform: uppercase;
}

h1 {
  color: #101828;
  font-size: 30px;
  line-height: 36px;
  margin: 0;
}

.subtext {
  color: #667085;
  margin: 8px 0 0;
}

.control-panel,
.status-panel,
.result-panel,
.workspace-tabs {
  background: #ffffff;
  border: 1px solid #d9e2ec;
  border-radius: 8px;
}

.control-panel {
  display: grid;
  gap: 14px;
  margin-bottom: 14px;
  padding: 16px;
}

.path-row {
  align-items: center;
  display: grid;
  gap: 10px;
  grid-template-columns: 150px minmax(0, 1fr) auto auto;
}

.path-row label {
  color: #344054;
  font-size: 13px;
  font-weight: 700;
}

.option-grid {
  align-items: center;
  display: grid;
  gap: 10px 14px;
  grid-template-columns: repeat(6, minmax(0, 1fr));
}

.option-grid .el-form-item {
  margin: 0;
}

.alert {
  margin-bottom: 14px;
}

.status-panel {
  display: grid;
  gap: 16px;
  margin-bottom: 14px;
  padding: 16px;
}

.status-copy {
  display: grid;
  gap: 4px;
}

.status-copy strong {
  color: #1d2939;
}

.status-copy span,
.result-panel span {
  color: #667085;
  font-family: "JetBrains Mono", "SFMono-Regular", Consolas, monospace;
  font-size: 12px;
  word-break: break-all;
}

.metric-grid {
  display: grid;
  gap: 12px;
  grid-template-columns: repeat(6, minmax(0, 1fr));
}

.metric-card {
  background: #f8fafc;
  border: 1px solid #e4e7ec;
  border-radius: 8px;
  min-width: 0;
  padding: 12px;
}

.metric-card span,
.metric-card small {
  color: #667085;
  display: block;
  font-size: 12px;
}

.metric-card strong {
  color: #101828;
  display: block;
  font-size: 24px;
  line-height: 30px;
  margin: 4px 0 2px;
}

.result-panel {
  align-items: center;
  display: flex;
  gap: 16px;
  justify-content: space-between;
  margin-bottom: 14px;
  padding: 14px 16px;
}

.result-panel div {
  display: grid;
  gap: 4px;
  min-width: 0;
}

.workspace-tabs {
  padding: 0 18px 18px;
}

.workspace-tabs > .el-tabs__header {
  margin-bottom: 18px;
}

.diagnostic-list {
  display: grid;
  gap: 10px;
}

.diagnostic-item {
  align-items: flex-start;
  background: #f8fafc;
  border: 1px solid #e4e7ec;
  border-radius: 8px;
  display: grid;
  gap: 12px;
  grid-template-columns: auto minmax(0, 1fr);
  padding: 12px;
}

.diagnostic-item div {
  display: grid;
  gap: 2px;
  min-width: 0;
}

.diagnostic-item strong {
  color: #1d2939;
  font-size: 14px;
}

.diagnostic-item span {
  color: #667085;
  font-family: "JetBrains Mono", "SFMono-Regular", Consolas, monospace;
  font-size: 12px;
  word-break: break-all;
}

.el-table {
  --el-table-header-bg-color: #f8fafc;
  --el-table-header-text-color: #344054;
}

@media (max-width: 1180px) {
  .metric-grid,
  .option-grid {
    grid-template-columns: repeat(3, minmax(0, 1fr));
  }

  .path-row {
    grid-template-columns: 130px minmax(0, 1fr);
  }
}

@media (max-width: 760px) {
  .app-shell {
    padding: 16px;
  }

  .app-header,
  .result-panel {
    align-items: stretch;
    flex-direction: column;
  }

  .path-row,
  .metric-grid,
  .option-grid {
    grid-template-columns: 1fr;
  }

  h1 {
    font-size: 24px;
    line-height: 30px;
  }
}
</style>
