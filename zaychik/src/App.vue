<script setup lang="ts">
import { computed, ref } from "vue";
import DataTable from "./components/DataTable.vue";
import { dx12DumpData } from "./data/dx12DumpData";

type CompactTable = {
  title?: string;
  columns: string[];
  rows: unknown[][];
};

type CountItem = {
  name: string;
  count: number;
};

const activeTab = ref("overview");
const activeShaderStage = ref("all");
const activeReportFile = ref("ShaderAnalysis.txt");
const activeReportSection = ref("0");

const data = dx12DumpData;
const overview = data.overview;

const reportOptions = Object.entries(data.reports).map(([file, report]: [string, any]) => ({
  file,
  title: report.title,
  sections: report.sections.map((section: CompactTable, index: number) => ({
    index: String(index),
    title: section.title || "Data",
    rows: section.rows.length,
  })),
}));

const currentReport = computed(() => data.reports[activeReportFile.value]);

const currentReportSection = computed<CompactTable>(() => {
  const sections = currentReport.value?.sections ?? [];
  return sections[Number(activeReportSection.value)] ?? sections[0] ?? { columns: [], rows: [] };
});

const shaderDetails = computed<CompactTable>(() => {
  const section = data.reports["ShaderAnalysis.txt"].sections.find(
    (item: CompactTable) => item.title === "Shader Details",
  );
  if (!section || activeShaderStage.value === "all") {
    return section;
  }

  const stageIndex = section.columns.indexOf("stage");
  return {
    ...section,
    rows: section.rows.filter((row: unknown[]) => row[stageIndex] === activeShaderStage.value),
  };
});

const tableFromObjects = (rows: Record<string, unknown>[], title: string): CompactTable => {
  const columns = [...new Set(rows.flatMap((row) => Object.keys(row)))];
  return {
    title,
    columns,
    rows: rows.map((row) => columns.map((column) => row[column] ?? "")),
  };
};

const csvTable = (file: string) => data.csv[file] as CompactTable;

const reportSection = (file: string, title: string) =>
  data.reports[file].sections.find((section: CompactTable) => section.title === title) as
    | CompactTable
    | undefined;

const psoSummary = computed(
  () => reportSection("PsoResourceSummaryDX12.txt", "PSO Summary") ?? { columns: [], rows: [] },
);

const rootUsage = computed(
  () =>
    reportSection("PsoResourceSummaryDX12.txt", "Root Signature Usage") ?? {
      columns: [],
      rows: [],
    },
);

const currentResources = computed(
  () => data.reports["CurrentFrameResourcesDX12.txt"].sections[0] as CompactTable,
);

const resourceFiles = computed(
  () => data.reports["CurrentFrameResourceFilesDX12.txt"].sections[0] as CompactTable,
);

const bindingTrace = computed(() => data.reports["BindingTraceDX12.txt"].sections[0] as CompactTable);

const resourceMetadataSections = computed(
  () => data.reports["ResourceMetadataDX12.txt"].sections as CompactTable[],
);

const fileTables = computed(() => ({
  root: tableFromObjects(data.files.rootFiles, "Dump root files"),
  shaders: tableFromObjects(data.files.shaderFiles, "Shader asm/bin files"),
  resources: tableFromObjects(data.files.resourceFiles, "Current frame resource files"),
}));

const formatNumber = (value: unknown) => Number(value ?? 0).toLocaleString();

const formatBytes = (value: unknown) => {
  const bytes = Number(value ?? 0);
  if (!Number.isFinite(bytes)) {
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

const summaryCards = computed(() => [
  { label: "Events", value: overview.frame.events, detail: `${formatNumber(overview.frame.dropped)} dropped` },
  { label: "Draw Rows", value: overview.frame.draw_rows, detail: `${formatNumber(overview.frame.dispatch_rows)} dispatch rows` },
  { label: "Shaders", value: overview.shaderSummary.shaders, detail: `${formatNumber(overview.shaderSummary.dxil)} DXIL / ${formatNumber(overview.shaderSummary.dxbc)} DXBC` },
  { label: "PSOs", value: overview.psoSummary.psos, detail: `${formatNumber(overview.psoSummary.root_signatures)} root signatures` },
  { label: "Descriptors", value: overview.psoSummary.descriptors, detail: `${formatNumber(overview.psoSummary.srv)} SRV / ${formatNumber(overview.psoSummary.uav)} UAV` },
  { label: "Resource Files", value: overview.resourceFileCount, detail: formatBytes(overview.totalResourceBytes) },
]);

const ratio = (count: number, total: number) => {
  if (!total) {
    return 0;
  }

  return Math.max(3, Math.round((count / total) * 100));
};

const maxCount = (items: CountItem[]) =>
  items.reduce((max, item) => Math.max(max, Number(item.count) || 0), 0);

const onReportFileChange = () => {
  activeReportSection.value = "0";
};
</script>

<template>
  <main class="app-shell">
    <header class="app-header">
      <div>
        <p class="eyebrow">DX12 dump analysis snapshot</p>
        <h1>Slay the Spire 2 ShaderDumpDX12</h1>
        <p class="dump-path">{{ overview.dumpRoot }}</p>
      </div>
      <div class="header-meta">
        <el-tag effect="plain">Generated {{ new Date(overview.generatedAt).toLocaleString() }}</el-tag>
        <el-tag effect="plain">{{ formatNumber(overview.fileCount) }} root files</el-tag>
      </div>
    </header>

    <section class="metric-grid">
      <article v-for="card in summaryCards" :key="card.label" class="metric-card">
        <span>{{ card.label }}</span>
        <strong>{{ formatNumber(card.value) }}</strong>
        <small>{{ card.detail }}</small>
      </article>
    </section>

    <el-tabs v-model="activeTab" class="analysis-tabs">
      <el-tab-pane label="Overview" name="overview">
        <section class="overview-layout">
          <div class="panel">
            <div class="panel-heading">
              <h2>Shader Stages</h2>
              <span>bytecode + asm footprint</span>
            </div>
            <DataTable
              :columns="reportSection('ShaderAnalysis.txt', 'Stage Summary')?.columns ?? []"
              :rows="reportSection('ShaderAnalysis.txt', 'Stage Summary')?.rows ?? []"
              :height="210"
              :page-size="10"
              dense
            />
          </div>

          <div class="panel">
            <div class="panel-heading">
              <h2>Descriptor Inventory</h2>
              <span>PSO resource summary</span>
            </div>
            <div class="bar-list">
              <div
                v-for="item in overview.descriptorInventory"
                :key="item.name"
                class="bar-row"
              >
                <div>
                  <span>{{ item.name }}</span>
                  <strong>{{ formatNumber(item.count) }}</strong>
                </div>
                <el-progress
                  :percentage="ratio(item.count, maxCount(overview.descriptorInventory))"
                  :show-text="false"
                  :stroke-width="8"
                />
              </div>
            </div>
          </div>

          <div class="panel">
            <div class="panel-heading">
              <h2>Binding Events</h2>
              <span>captured command stream</span>
            </div>
            <div class="chip-list">
              <el-tag
                v-for="item in overview.bindingEventCounts"
                :key="item.name"
                effect="plain"
                round
              >
                {{ item.name }}: {{ formatNumber(item.count) }}
              </el-tag>
            </div>
          </div>

          <div class="panel">
            <div class="panel-heading">
              <h2>Resource Files</h2>
              <span>copied frame data</span>
            </div>
            <div class="chip-list">
              <el-tag
                v-for="item in overview.resourceFileExtensionCounts"
                :key="item.name"
                effect="plain"
                round
              >
                {{ item.name }}: {{ formatNumber(item.count) }}
              </el-tag>
            </div>
          </div>
        </section>

        <section class="panel wide-panel">
          <div class="panel-heading">
            <h2>Largest Copied Resources</h2>
            <span>from CurrentFrameResourceFilesDX12</span>
          </div>
          <DataTable
            :columns="tableFromObjects(overview.largestCopiedResources, 'Largest resources').columns"
            :rows="tableFromObjects(overview.largestCopiedResources, 'Largest resources').rows"
            :height="360"
            :page-size="16"
            dense
          />
        </section>
      </el-tab-pane>

      <el-tab-pane label="Draw / Dispatch" name="draws">
        <section class="split-panels">
          <div class="panel">
            <div class="panel-heading">
              <h2>Draw Call Types</h2>
              <span>DrawCallsDX12.csv</span>
            </div>
            <div class="chip-list">
              <el-tag v-for="item in overview.drawTypeCounts" :key="item.name" effect="plain" round>
                {{ item.name }}: {{ formatNumber(item.count) }}
              </el-tag>
            </div>
          </div>
          <div class="panel">
            <div class="panel-heading">
              <h2>Buffer Roles</h2>
              <span>BuffersDX12.csv</span>
            </div>
            <div class="chip-list">
              <el-tag v-for="item in overview.bufferRoleCounts" :key="item.name" effect="plain" round>
                {{ item.name }}: {{ formatNumber(item.count) }}
              </el-tag>
            </div>
          </div>
        </section>

        <DataTable title="DrawCallsDX12.csv" v-bind="csvTable('DrawCallsDX12.csv')" />
        <DataTable title="BuffersDX12.csv" v-bind="csvTable('BuffersDX12.csv')" :height="420" />
      </el-tab-pane>

      <el-tab-pane label="Shaders" name="shaders">
        <section class="toolbar-panel">
          <el-radio-group v-model="activeShaderStage">
            <el-radio-button label="all">All</el-radio-button>
            <el-radio-button label="vs">VS</el-radio-button>
            <el-radio-button label="ps">PS</el-radio-button>
            <el-radio-button label="cs">CS</el-radio-button>
          </el-radio-group>
          <div class="chip-list inline">
            <el-tag effect="plain">{{ formatNumber(overview.shaderSummary.sampled) }} sampled</el-tag>
            <el-tag effect="plain">{{ formatNumber(overview.shaderSummary.uav) }} UAV writers</el-tag>
            <el-tag effect="plain">{{ formatNumber(overview.shaderSummary.discard) }} discard</el-tag>
          </div>
        </section>

        <section class="split-panels">
          <div class="panel">
            <div class="panel-heading">
              <h2>Largest ASM</h2>
              <span>top generated assembly files</span>
            </div>
            <DataTable
              :columns="reportSection('ShaderAnalysis.txt', 'Largest ASM Files')?.columns ?? []"
              :rows="reportSection('ShaderAnalysis.txt', 'Largest ASM Files')?.rows ?? []"
              :height="360"
              :page-size="20"
              dense
            />
          </div>
          <div class="panel">
            <div class="panel-heading">
              <h2>Most Used Shaders</h2>
              <span>usage counts across PSOs</span>
            </div>
            <DataTable
              :columns="tableFromObjects(overview.topShadersByUses, 'Top shader usage').columns"
              :rows="tableFromObjects(overview.topShadersByUses, 'Top shader usage').rows"
              :height="360"
              :page-size="12"
              dense
            />
          </div>
        </section>

        <DataTable title="Shader Details" v-bind="shaderDetails" :height="560" />
      </el-tab-pane>

      <el-tab-pane label="PSO / Roots" name="pso">
        <section class="split-panels">
          <div class="panel">
            <div class="panel-heading">
              <h2>PSO Load Hotspots</h2>
              <span>highest cbuffer load counts</span>
            </div>
            <DataTable
              :columns="tableFromObjects(overview.topPsosByCbufferLoads, 'Top PSOs').columns"
              :rows="tableFromObjects(overview.topPsosByCbufferLoads, 'Top PSOs').rows"
              :height="360"
              :page-size="12"
              dense
            />
          </div>
          <div class="panel">
            <div class="panel-heading">
              <h2>Root Signature Reuse</h2>
              <span>top PSO counts</span>
            </div>
            <DataTable
              :columns="tableFromObjects(overview.topRootSignaturesByPsoCount, 'Top roots').columns"
              :rows="tableFromObjects(overview.topRootSignaturesByPsoCount, 'Top roots').rows"
              :height="360"
              :page-size="12"
              dense
            />
          </div>
        </section>

        <DataTable title="PSO Summary" v-bind="psoSummary" />
        <DataTable title="Root Signature Usage" v-bind="rootUsage" :height="460" />
      </el-tab-pane>

      <el-tab-pane label="Resources" name="resources">
        <section class="split-panels">
          <div class="panel">
            <div class="panel-heading">
              <h2>Descriptor Kinds</h2>
              <span>current frame bindings</span>
            </div>
            <div class="chip-list">
              <el-tag
                v-for="item in overview.descriptorKindCounts"
                :key="item.name"
                effect="plain"
                round
              >
                {{ item.name }}: {{ formatNumber(item.count) }}
              </el-tag>
            </div>
          </div>
          <div class="panel">
            <div class="panel-heading">
              <h2>Resource Dimensions</h2>
              <span>current frame bindings</span>
            </div>
            <div class="chip-list">
              <el-tag
                v-for="item in overview.resourceDimensionCounts"
                :key="item.name"
                effect="plain"
                round
              >
                {{ item.name }}: {{ formatNumber(item.count) }}
              </el-tag>
            </div>
          </div>
        </section>

        <DataTable title="CurrentFrameResourcesDX12.txt" v-bind="currentResources" />
        <DataTable title="CurrentFrameResourceFilesDX12.txt" v-bind="resourceFiles" />
      </el-tab-pane>

      <el-tab-pane label="Metadata" name="metadata">
        <el-collapse accordion>
          <el-collapse-item
            v-for="section in resourceMetadataSections"
            :key="section.title"
            :title="`${section.title} (${formatNumber(section.rows.length)})`"
          >
            <DataTable v-bind="section" :height="560" />
          </el-collapse-item>
        </el-collapse>
      </el-tab-pane>

      <el-tab-pane label="Binding Trace" name="trace">
        <DataTable title="BindingTraceDX12.txt" v-bind="bindingTrace" :height="680" />
      </el-tab-pane>

      <el-tab-pane label="Files" name="files">
        <el-tabs>
          <el-tab-pane label="Root Files">
            <DataTable v-bind="fileTables.root" />
          </el-tab-pane>
          <el-tab-pane label="Shader Files">
            <DataTable v-bind="fileTables.shaders" />
          </el-tab-pane>
          <el-tab-pane label="Resource Files">
            <DataTable v-bind="fileTables.resources" />
          </el-tab-pane>
        </el-tabs>
      </el-tab-pane>

      <el-tab-pane label="Reports" name="reports">
        <section class="report-toolbar">
          <el-select v-model="activeReportFile" filterable @change="onReportFileChange">
            <el-option
              v-for="report in reportOptions"
              :key="report.file"
              :label="report.file"
              :value="report.file"
            />
          </el-select>
          <el-select v-model="activeReportSection" filterable>
            <el-option
              v-for="section in reportOptions.find((item) => item.file === activeReportFile)?.sections ?? []"
              :key="section.index"
              :label="`${section.title} (${formatNumber(section.rows)})`"
              :value="section.index"
            />
          </el-select>
        </section>

        <DataTable
          :title="`${activeReportFile} / ${currentReportSection.title}`"
          v-bind="currentReportSection"
          :height="660"
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
  background: #eef2f6;
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
  max-width: 1680px;
  padding: 28px;
}

.app-header {
  align-items: flex-start;
  display: flex;
  gap: 24px;
  justify-content: space-between;
  margin-bottom: 20px;
}

.eyebrow {
  color: #3066be;
  font-size: 12px;
  font-weight: 700;
  letter-spacing: 0;
  margin: 0 0 4px;
  text-transform: uppercase;
}

h1 {
  color: #111827;
  font-size: 30px;
  line-height: 36px;
  margin: 0;
}

.dump-path {
  color: #667085;
  font-family: "JetBrains Mono", "SFMono-Regular", Consolas, monospace;
  font-size: 13px;
  margin: 8px 0 0;
  word-break: break-all;
}

.header-meta {
  display: flex;
  flex-wrap: wrap;
  gap: 8px;
  justify-content: flex-end;
}

.metric-grid {
  display: grid;
  gap: 12px;
  grid-template-columns: repeat(6, minmax(0, 1fr));
  margin-bottom: 16px;
}

.metric-card,
.panel,
.toolbar-panel {
  background: #ffffff;
  border: 1px solid #d9e2ec;
  border-radius: 8px;
}

.metric-card {
  min-width: 0;
  padding: 14px;
}

.metric-card span,
.metric-card small {
  color: #667085;
  display: block;
  font-size: 12px;
}

.metric-card strong {
  color: #111827;
  display: block;
  font-size: 25px;
  line-height: 32px;
  margin: 4px 0 2px;
}

.analysis-tabs {
  background: #ffffff;
  border: 1px solid #d9e2ec;
  border-radius: 8px;
  padding: 0 18px 18px;
}

.analysis-tabs > .el-tabs__header {
  margin-bottom: 18px;
}

.overview-layout,
.split-panels {
  display: grid;
  gap: 14px;
  grid-template-columns: repeat(2, minmax(0, 1fr));
  margin-bottom: 14px;
}

.panel,
.toolbar-panel {
  min-width: 0;
  padding: 16px;
}

.wide-panel {
  margin-top: 14px;
}

.panel-heading {
  align-items: flex-start;
  display: flex;
  gap: 12px;
  justify-content: space-between;
  margin-bottom: 12px;
}

.panel-heading h2 {
  color: #172033;
  font-size: 16px;
  line-height: 22px;
  margin: 0;
}

.panel-heading span {
  color: #667085;
  font-size: 12px;
  text-align: right;
}

.bar-list,
.chip-list {
  display: flex;
  flex-direction: column;
  gap: 10px;
}

.chip-list {
  align-items: flex-start;
  flex-direction: row;
  flex-wrap: wrap;
}

.chip-list.inline {
  align-items: center;
}

.bar-row {
  display: grid;
  gap: 6px;
}

.bar-row div {
  align-items: center;
  display: flex;
  justify-content: space-between;
}

.bar-row span {
  color: #475467;
  font-size: 13px;
}

.bar-row strong {
  color: #172033;
  font-size: 13px;
}

.toolbar-panel,
.report-toolbar {
  align-items: center;
  display: flex;
  flex-wrap: wrap;
  gap: 12px;
  justify-content: space-between;
  margin-bottom: 14px;
}

.report-toolbar {
  justify-content: flex-start;
}

.report-toolbar .el-select {
  width: min(420px, 100%);
}

.el-table {
  --el-table-header-bg-color: #f8fafc;
  --el-table-header-text-color: #344054;
}

@media (max-width: 1200px) {
  .metric-grid {
    grid-template-columns: repeat(3, minmax(0, 1fr));
  }
}

@media (max-width: 900px) {
  .app-header,
  .toolbar-panel {
    align-items: stretch;
    flex-direction: column;
  }

  .header-meta {
    justify-content: flex-start;
  }

  .overview-layout,
  .split-panels {
    grid-template-columns: 1fr;
  }
}

@media (max-width: 620px) {
  .app-shell {
    padding: 16px;
  }

  h1 {
    font-size: 24px;
    line-height: 30px;
  }

  .metric-grid {
    grid-template-columns: 1fr;
  }
}
</style>
