import { readdir, readFile, stat, writeFile } from "node:fs/promises";
import { dirname, extname, join, relative } from "node:path";
import { fileURLToPath } from "node:url";

const __dirname = dirname(fileURLToPath(import.meta.url));
const projectRoot = join(__dirname, "..");
const dumpRoot =
  process.env.DX12_DUMP_DIR ??
  "D:\\Softs\\Steam\\steamapps\\common\\Slay the Spire 2\\ShaderDumpDX12";
const outputFile = join(projectRoot, "src", "data", "dx12DumpData.ts");
const resourceDir = join(dumpRoot, "CurrentFrameResourceFiles");

const reportFiles = [
  "ShaderAnalysis.txt",
  "ShaderUsage.txt",
  "PsoResourceSummaryDX12.txt",
  "CurrentFrameResourcesDX12.txt",
  "CurrentFrameResourceFilesDX12.txt",
  "ResourceMetadataDX12.txt",
  "BindingTraceDX12.txt",
  "pso_log.txt",
];

const csvFiles = ["FrameAnalysisDX12.csv", "DrawCallsDX12.csv", "BuffersDX12.csv"];

const numberLike = new Set([
  "asm_bytes",
  "asm_size",
  "base_vertex",
  "buffer_view_bytes",
  "buffer_view_offset",
  "bytecode_bytes",
  "cbuffer_loads",
  "cbuffers",
  "copy_bytes",
  "count",
  "descriptor_index",
  "descriptor_kind_count",
  "descriptor_rows",
  "descriptors",
  "dispatch_id",
  "dispatch_rows",
  "draw_calls",
  "draw_id",
  "draw_rows",
  "dropped",
  "dxbc",
  "dxil",
  "events",
  "first_element",
  "first_pso",
  "format",
  "groups_x",
  "groups_y",
  "groups_z",
  "height",
  "index",
  "index_count",
  "instance_count",
  "mips",
  "num_descriptors",
  "num_elements",
  "offset",
  "pso",
  "pso_count",
  "psos",
  "raw_buffer_loads",
  "resource_size",
  "root_param",
  "root_size",
  "samplers",
  "sampled",
  "samples",
  "serial",
  "shaders",
  "size",
  "start_index",
  "start_instance",
  "start_vertex",
  "store_outputs",
  "stride",
  "texture_loads",
  "textures",
  "uav",
  "uavs",
  "uses",
  "vertex_count",
  "width",
]);

function isIntegerText(value) {
  return /^-?\d+$/.test(value);
}

function parseValue(key, value) {
  if (value === undefined || value === "") {
    return "";
  }

  if (numberLike.has(key) && isIntegerText(value)) {
    return Number(value);
  }

  return value;
}

function parseCsvLine(line) {
  const values = [];
  let current = "";
  let quoted = false;

  for (let index = 0; index < line.length; index += 1) {
    const char = line[index];
    const next = line[index + 1];

    if (char === '"' && quoted && next === '"') {
      current += '"';
      index += 1;
    } else if (char === '"') {
      quoted = !quoted;
    } else if (char === "," && !quoted) {
      values.push(current);
      current = "";
    } else {
      current += char;
    }
  }

  values.push(current);
  return values;
}

function parseCsvTable(text) {
  const lines = text.split(/\r?\n/).filter((line) => line.trim().length > 0);
  if (lines.length === 0) {
    return { columns: [], rows: [] };
  }

  const columns = parseCsvLine(lines[0]).map((column) => column.trim());
  const rows = lines.slice(1).map((line) => {
    const values = parseCsvLine(line);
    return columns.map((column, index) => parseValue(column, values[index] ?? ""));
  });

  return { columns, rows };
}

function isHeaderLine(line) {
  const columns = line.trim().split(",");
  return (
    columns.length > 1 &&
    columns.every((column) => /^[A-Za-z_][A-Za-z0-9_]*$/.test(column))
  );
}

function parseSummaryLine(line) {
  const result = {};
  for (const part of line.split(/\s+/)) {
    const [key, value] = part.split("=");
    if (!key || value === undefined) {
      continue;
    }
    result[key] = parseValue(key, value);
  }
  return result;
}

function nearestTitle(lines, headerIndex) {
  for (let index = headerIndex - 1; index >= 0; index -= 1) {
    const line = lines[index].trim();
    if (!line || /^=+$/.test(line)) {
      continue;
    }

    if (!line.includes(",") && !line.includes("=")) {
      return line;
    }

    break;
  }

  return "Data";
}

function parseReport(text) {
  const lines = text.split(/\r?\n/);
  const title = lines.find((line) => line.trim().length > 0)?.trim() ?? "Report";
  const summaryLine = lines.find((line) => /\w+=/.test(line) && !line.includes(","));
  const summary = summaryLine ? parseSummaryLine(summaryLine.trim()) : {};
  const sections = [];

  for (let index = 0; index < lines.length; index += 1) {
    const line = lines[index].trim();
    if (!isHeaderLine(line)) {
      continue;
    }

    const columns = parseCsvLine(line);
    const rowLines = [];

    for (let rowIndex = index + 1; rowIndex < lines.length; rowIndex += 1) {
      const row = lines[rowIndex].trim();
      if (!row) {
        break;
      }

      if (isHeaderLine(row)) {
        break;
      }

      if (!row.includes(",") && !row.includes("=")) {
        break;
      }

      rowLines.push(row);
    }

    sections.push({
      title: nearestTitle(lines, index),
      columns,
      rows: rowLines.map((rowLine) => {
        const values = parseCsvLine(rowLine);
        return columns.map((column, columnIndex) =>
          parseValue(column, values[columnIndex] ?? ""),
        );
      }),
    });
  }

  return { title, summary, sections };
}

function parseKeyValueTable(text, title) {
  const objectRows = text
    .split(/\r?\n/)
    .map((line) => line.trim())
    .filter(Boolean)
    .map((line) => {
      const row = {};
      for (const part of line.split(/\s+/)) {
        const [key, value] = part.split("=");
        if (!key || value === undefined) {
          continue;
        }
        row[key] = parseValue(key, value);
      }
      return row;
    })
    .filter((row) => Object.keys(row).length > 0);

  const columns = [...new Set(objectRows.flatMap((row) => Object.keys(row)))];
  const rows = objectRows.map((row) => columns.map((column) => row[column] ?? ""));
  return { title, columns, rows };
}

function rowsToObjects(table) {
  return table.rows.map((row) =>
    Object.fromEntries(table.columns.map((column, index) => [column, row[index] ?? ""])),
  );
}

function sum(rows, key) {
  return rows.reduce((total, row) => total + (Number(row[key]) || 0), 0);
}

function countBy(rows, key) {
  const counts = {};
  for (const row of rows) {
    const value = String(row[key] ?? "unknown") || "unknown";
    counts[value] = (counts[value] ?? 0) + 1;
  }
  return Object.entries(counts)
    .map(([name, count]) => ({ name, count }))
    .sort((left, right) => right.count - left.count || left.name.localeCompare(right.name));
}

function topRows(rows, key, limit = 10) {
  return [...rows]
    .sort((left, right) => (Number(right[key]) || 0) - (Number(left[key]) || 0))
    .slice(0, limit);
}

async function fileRecord(root, entry) {
  const fullPath = join(root, entry.name);
  const info = await stat(fullPath);
  return {
    name: entry.name,
    relativePath: relative(dumpRoot, fullPath).replaceAll("\\", "/"),
    extension: extname(entry.name).toLowerCase() || "(none)",
    size: info.size,
    modified: info.mtime.toISOString(),
  };
}

async function listFiles(root) {
  const entries = await readdir(root, { withFileTypes: true });
  const files = [];

  for (const entry of entries) {
    if (entry.isFile()) {
      files.push(await fileRecord(root, entry));
    }
  }

  return files.sort((left, right) => left.name.localeCompare(right.name));
}

async function main() {
  const rootFiles = await listFiles(dumpRoot);
  const resourceFiles = await listFiles(resourceDir);

  const csv = {};
  for (const file of csvFiles) {
    csv[file] = parseCsvTable(await readFile(join(dumpRoot, file), "utf8"));
  }

  const reports = {};
  for (const file of reportFiles) {
    const text = await readFile(join(dumpRoot, file), "utf8");
    reports[file] = parseReport(text);

    if (file === "pso_log.txt") {
      reports[file].sections = [parseKeyValueTable(text, "PSO Log")];
    }
  }

  const shaderDetailsTable = reports["ShaderAnalysis.txt"].sections.find(
    (section) => section.title === "Shader Details",
  );
  const stageSummaryTable = reports["ShaderAnalysis.txt"].sections.find(
    (section) => section.title === "Stage Summary",
  );
  const largestShadersTable = reports["ShaderAnalysis.txt"].sections.find(
    (section) => section.title === "Largest ASM Files",
  );
  const psoSummaryTable = reports["PsoResourceSummaryDX12.txt"].sections.find(
    (section) => section.title === "PSO Summary",
  );
  const rootUsageTable = reports["PsoResourceSummaryDX12.txt"].sections.find(
    (section) => section.title === "Root Signature Usage",
  );
  const descriptorInventoryTable = reports["PsoResourceSummaryDX12.txt"].sections.find(
    (section) => section.title === "Descriptor Inventory",
  );
  const currentResourcesTable = reports["CurrentFrameResourcesDX12.txt"].sections[0];
  const resourceFileRowsTable = reports["CurrentFrameResourceFilesDX12.txt"].sections[0];
  const bindingTraceTable = reports["BindingTraceDX12.txt"].sections[0];

  const shaderDetails = shaderDetailsTable ? rowsToObjects(shaderDetailsTable) : [];
  const stageSummary = stageSummaryTable ? rowsToObjects(stageSummaryTable) : [];
  const largestShaders = largestShadersTable ? rowsToObjects(largestShadersTable) : [];
  const psoSummary = psoSummaryTable ? rowsToObjects(psoSummaryTable) : [];
  const rootUsage = rootUsageTable ? rowsToObjects(rootUsageTable) : [];
  const descriptorInventory = descriptorInventoryTable
    ? rowsToObjects(descriptorInventoryTable)
    : [];
  const currentResources = currentResourcesTable ? rowsToObjects(currentResourcesTable) : [];
  const resourceFileRows = resourceFileRowsTable ? rowsToObjects(resourceFileRowsTable) : [];
  const bindingTrace = bindingTraceTable ? rowsToObjects(bindingTraceTable) : [];
  const drawCalls = rowsToObjects(csv["DrawCallsDX12.csv"]);
  const buffers = rowsToObjects(csv["BuffersDX12.csv"]);
  const frame = rowsToObjects(csv["FrameAnalysisDX12.csv"])[0] ?? {};

  const shaderFiles = rootFiles
    .filter((file) => /-[c-vp]s\.(asm\.txt|bin)$/i.test(file.name))
    .map((file) => {
      const match = file.name.match(/^([0-9a-f]+)-([a-z]+)\.(asm\.txt|bin)$/i);
      return {
        ...file,
        hash: match?.[1] ?? "",
        stage: match?.[2] ?? "",
        kind: match?.[3] ?? "",
      };
    });

  const overview = {
    dumpRoot,
    generatedAt: new Date().toISOString(),
    fileCount: rootFiles.length,
    resourceFileCount: resourceFiles.length,
    totalRootBytes: sum(rootFiles, "size"),
    totalResourceBytes: sum(resourceFiles, "size"),
    shaderFileCount: shaderFiles.length,
    asmFileCount: shaderFiles.filter((file) => file.kind === "asm.txt").length,
    binFileCount: shaderFiles.filter((file) => file.kind === "bin").length,
    frame,
    shaderSummary: reports["ShaderAnalysis.txt"].summary,
    psoSummary: reports["PsoResourceSummaryDX12.txt"].summary,
    resourceSummary: reports["ResourceMetadataDX12.txt"].summary,
    bindingSummary: reports["BindingTraceDX12.txt"].summary,
    resourceFileSummary: reports["CurrentFrameResourceFilesDX12.txt"].summary,
    stageSummary,
    descriptorInventory: descriptorInventory.map((row) => ({
      name: row.kind,
      count: row.count,
    })),
    drawTypeCounts: countBy(drawCalls, "type"),
    bindingEventCounts: countBy(bindingTrace, "kind"),
    resourceDimensionCounts: countBy(currentResources, "resource_dimension"),
    descriptorKindCounts: countBy(currentResources, "descriptor_kind"),
    bufferRoleCounts: countBy(buffers, "role"),
    resourceFileStatusCounts: countBy(resourceFileRows, "status"),
    resourceFileExtensionCounts: countBy(resourceFiles, "extension"),
    topShadersByAsm: topRows(shaderDetails, "asm_size", 12),
    topShadersByUses: topRows(shaderDetails, "uses", 12),
    largestShaderFiles: largestShaders,
    topPsosByCbufferLoads: topRows(psoSummary, "cbuffer_loads", 12),
    topPsosByTextureLoads: topRows(psoSummary, "texture_loads", 12),
    topRootSignaturesByPsoCount: topRows(rootUsage, "pso_count", 12),
    largestResourceFiles: topRows(resourceFiles, "size", 16),
    largestBuffers: topRows(buffers, "size", 16),
    largestCopiedResources: topRows(resourceFileRows, "copy_bytes", 16),
  };

  const data = {
    overview,
    csv,
    reports,
    files: {
      rootFiles,
      shaderFiles,
      resourceFiles,
    },
  };

  const source = `/* eslint-disable */\n// Generated by scripts/generate-dx12-dump-data.mjs\nexport const dx12DumpData: any = ${JSON.stringify(data)};\n\nexport type Dx12DumpData = any;\n`;

  await writeFile(outputFile, source, "utf8");
  console.log(`Wrote ${relative(projectRoot, outputFile)} from ${dumpRoot}`);
}

main().catch((error) => {
  console.error(error);
  process.exitCode = 1;
});
