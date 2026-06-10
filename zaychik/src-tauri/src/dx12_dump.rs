use std::collections::{BTreeMap, HashMap, HashSet};
use std::ffi::OsStr;
use std::fs;
use std::path::{Path, PathBuf};

use serde::{Deserialize, Serialize};
use serde_json::{json, Map, Value};

const DRAW_CALLS_FILE: &str = "DrawCallsDX12.csv";
const BUFFERS_FILE: &str = "BuffersDX12.csv";
const BINDING_TRACE_FILE: &str = "BindingTraceDX12.txt";
const RESOURCE_FOLDER: &str = "CurrentFrameResourceFiles";
const MAX_PREVIEW_DRAWS: usize = 500;
const MAX_EXPORT_DRAWS: usize = 500;

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DumpInspection {
    pub dump_dir: String,
    pub output_dir: String,
    pub exists: bool,
    pub status: String,
    pub can_export: bool,
    pub summary: DumpSummary,
    pub diagnostics: Vec<Diagnostic>,
    pub draw_candidates: Vec<DrawCandidate>,
}

#[derive(Debug, Clone, Serialize, Default)]
#[serde(rename_all = "camelCase")]
pub struct DumpSummary {
    pub root_file_count: usize,
    pub shader_file_count: usize,
    pub resource_file_count: usize,
    pub resource_bytes: u64,
    pub draw_rows: usize,
    pub draw_indexed_rows: usize,
    pub exportable_draws: usize,
    pub default_exportable_draws: usize,
    pub skipped_draws: usize,
    pub vertex_buffers: usize,
    pub index_buffers: usize,
    pub missing_buffer_files: usize,
    pub binding_events: usize,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct Diagnostic {
    pub level: String,
    pub message: String,
    pub detail: String,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct DrawCandidate {
    pub draw_id: u64,
    pub pso: String,
    pub vs: String,
    pub ps: String,
    pub topology: String,
    pub index_count: u64,
    pub first_index: u64,
    pub base_vertex: i64,
    pub ib: Option<BufferPreview>,
    pub vbs: Vec<BufferPreview>,
    pub exportable: bool,
    pub confidence: String,
    pub has_position_input: bool,
    pub vs_inputs: Vec<String>,
    pub notes: Vec<String>,
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct BufferPreview {
    pub buffer_id: String,
    pub role: String,
    pub slot: u32,
    pub stride: u64,
    pub size: u64,
    pub format: String,
    pub file: String,
    pub exists: bool,
}

#[derive(Debug, Clone, Deserialize)]
#[serde(rename_all = "camelCase")]
pub struct ExportOptions {
    #[serde(default = "default_game_preset")]
    pub game_preset: String,
    #[serde(default = "default_game_type")]
    pub game_type: String,
    #[serde(default = "default_lod_name")]
    pub lod_name: String,
    #[serde(default)]
    pub overwrite: bool,
    #[serde(default = "default_max_draws")]
    pub max_draws: usize,
    #[serde(default)]
    pub include_low_confidence: bool,
}

impl Default for ExportOptions {
    fn default() -> Self {
        Self {
            game_preset: default_game_preset(),
            game_type: default_game_type(),
            lod_name: default_lod_name(),
            overwrite: false,
            max_draws: default_max_draws(),
            include_low_confidence: false,
        }
    }
}

#[derive(Debug, Clone, Serialize)]
#[serde(rename_all = "camelCase")]
pub struct ExportResult {
    pub output_dir: String,
    pub exported: usize,
    pub skipped: usize,
    pub diagnostics: Vec<Diagnostic>,
    pub exported_submeshes: Vec<String>,
}

#[derive(Debug, Clone)]
struct DumpModel {
    dump_dir: PathBuf,
    summary: DumpSummary,
    diagnostics: Vec<Diagnostic>,
    draw_candidates: Vec<DrawCandidate>,
}

#[derive(Debug, Clone, Default)]
struct DrawRecord {
    draw_id: u64,
    draw_type: String,
    pso: String,
    vs: String,
    ps: String,
    topology: String,
    index_count: u64,
    start_index: u64,
    base_vertex: i64,
    vb_slots: String,
    ib_resource: String,
}

#[derive(Debug, Clone, Default)]
struct BufferRecord {
    buffer_id: String,
    role: String,
    file: String,
    size: u64,
    stride: u64,
    slot: u32,
    format: String,
}

#[derive(Debug, Clone)]
struct CsvTable {
    rows: Vec<HashMap<String, String>>,
}

pub fn inspect_dump_directory(path: impl AsRef<Path>) -> Result<DumpInspection, String> {
    let dump_dir = path.as_ref().to_path_buf();
    let exists = dump_dir.is_dir();
    let output_dir = default_output_dir(&dump_dir);

    if !exists {
        return Ok(DumpInspection {
            dump_dir: display_path(&dump_dir),
            output_dir: display_path(&output_dir),
            exists,
            status: "missing".to_string(),
            can_export: false,
            summary: DumpSummary::default(),
            diagnostics: vec![Diagnostic::error(
                "Dump directory does not exist.",
                display_path(&dump_dir),
            )],
            draw_candidates: Vec::new(),
        });
    }

    let model = load_dump_model(&dump_dir)?;
    let can_export = model.summary.default_exportable_draws > 0;
    let status = if can_export {
        "ready"
    } else if model.summary.draw_indexed_rows > 0 {
        "incomplete"
    } else {
        "missing_geometry"
    };

    Ok(DumpInspection {
        dump_dir: display_path(&dump_dir),
        output_dir: display_path(&output_dir),
        exists,
        status: status.to_string(),
        can_export,
        summary: model.summary,
        diagnostics: model.diagnostics,
        draw_candidates: model.draw_candidates,
    })
}

pub fn export_blender_workspace(
    dump_dir: impl AsRef<Path>,
    output_dir: impl AsRef<Path>,
    options: ExportOptions,
) -> Result<ExportResult, String> {
    let options = normalize_options(options);
    let model = load_dump_model(dump_dir.as_ref())?;
    let output_dir = output_dir.as_ref().to_path_buf();
    let lod_name = sanitize_name(&options.lod_name, "LOD0");
    let game_preset = sanitize_name(&options.game_preset, "DX12Dump");
    let game_type = sanitize_name(&options.game_type, "DX12_IA");
    let type_dir_name = format!("TYPE_{}", game_type);
    let mut diagnostics = model.diagnostics.clone();

    if output_dir.exists() {
        if !options.overwrite {
            return Err(format!(
                "Output directory already exists. Enable overwrite or choose another folder: {}",
                display_path(&output_dir)
            ));
        }

        fs::remove_dir_all(&output_dir).map_err(|e| {
            format!(
                "Failed to remove existing output directory {}: {}",
                display_path(&output_dir),
                e
            )
        })?;
    }

    fs::create_dir_all(output_dir.join(&lod_name)).map_err(|e| {
        format!(
            "Failed to create output directory {}: {}",
            display_path(&output_dir),
            e
        )
    })?;

    let mut exported = 0usize;
    let mut skipped = 0usize;
    let mut exported_submeshes = Vec::new();
    let mut import_json = Map::new();
    let mut config_json = Vec::<Value>::new();
    let mut used_folders = HashSet::<String>::new();
    let export_limit = options.max_draws.clamp(1, MAX_EXPORT_DRAWS);

    for candidate in &model.draw_candidates {
        if exported >= export_limit {
            skipped += 1;
            continue;
        }

        if !candidate.exportable {
            skipped += 1;
            continue;
        }

        if !options.include_low_confidence && candidate.confidence != "medium" {
            skipped += 1;
            continue;
        }

        let Some(ib) = candidate.ib.as_ref() else {
            skipped += 1;
            continue;
        };

        let folder_name = unique_folder_name(
            &mut used_folders,
            &format!(
                "{}-{}-{}",
                drawib_name(candidate),
                candidate.index_count,
                candidate.first_index
            ),
        );
        let submesh_dir = output_dir.join(&lod_name).join(&folder_name);
        let type_dir = submesh_dir.join(&type_dir_name);
        fs::create_dir_all(&type_dir).map_err(|e| {
            format!(
                "Failed to create submesh directory {}: {}",
                display_path(&type_dir),
                e
            )
        })?;

        let ib_file_name = format!("{}-IndexBuffer.ib", folder_name);
        copy_buffer_file(
            &model.dump_dir,
            &ib.file,
            &type_dir.join(&ib_file_name),
            &mut diagnostics,
        )?;

        let mut category_buffer_list = Vec::new();
        let mut category_hash = BTreeMap::new();
        let mut category_draw_category_map = BTreeMap::new();

        for vb in &candidate.vbs {
            if vb.stride == 0 {
                diagnostics.push(Diagnostic::warning(
                    "Skipped a vertex buffer with zero stride.",
                    format!("draw {} buffer {}", candidate.draw_id, vb.buffer_id),
                ));
                continue;
            }

            let category = category_for_slot(vb.slot);
            let file_name = format!("{}-{}.buf", folder_name, category);
            copy_buffer_file(
                &model.dump_dir,
                &vb.file,
                &type_dir.join(&file_name),
                &mut diagnostics,
            )?;

            category_hash.insert(category.clone(), vb.buffer_id.clone());
            category_draw_category_map.insert(category.clone(), "Normal".to_string());

            category_buffer_list.push(json!({
                "FileName": file_name,
                "Type": "Normal",
                "D3D11ElementList": elements_for_vertex_buffer(vb, &category, candidate),
            }));
        }

        if category_buffer_list.is_empty() {
            skipped += 1;
            diagnostics.push(Diagnostic::warning(
                "Skipped draw because every vertex buffer had zero stride.",
                format!("draw {}", candidate.draw_id),
            ));
            continue;
        }

        let vertex_count = candidate
            .vbs
            .iter()
            .filter(|vb| vb.stride > 0)
            .map(|vb| vb.size / vb.stride)
            .min()
            .unwrap_or(0);

        let submesh_json = json!({
            "GamePreset": game_preset,
            "VertexLimitVB": "",
            "CSOutputVertexLimitVB": "",
            "CategoryHash": category_hash,
            "CategoryDrawCategoryMap": category_draw_category_map,
            "WorkGameType": game_type,
            "GPU-PreSkinning": false,
            "LocalBoundingBoxMin": [],
            "LocalBoundingBoxMax": [],
            "VertexCompressionParams": [],
            "VertexOffset": candidate.base_vertex.max(0),
            "VertexCount": vertex_count,
            "IndexOffset": candidate.first_index,
            "IndexCount": candidate.index_count,
            "CB4Hash": "",
            "BoneMatrixFileName": "",
            "VGOffset": 0,
            "VGCount": 0,
            "VGMap": {},
            "ShapeKeysInfo": {},
            "IndexBufferList": [
                {
                    "DXGI_FORMAT": dxgi_format_name(&ib.format),
                    "FileName": ib_file_name
                }
            ],
            "CategoryBufferList": category_buffer_list,
            "TextureMarkUpInfoList": [],
        });

        let json_path = type_dir.join(format!("{}.json", folder_name));
        write_pretty_json(&json_path, &submesh_json)?;

        let full_name = format!("{}.{}", lod_name, folder_name);
        import_json.insert(full_name.clone(), Value::String(game_type.clone()));
        config_json.push(json!({
            "DrawIB": drawib_name(candidate),
            "Alias": format!("draw_{}", candidate.draw_id),
        }));

        exported_submeshes.push(full_name);
        exported += 1;
    }

    write_pretty_json(&output_dir.join("Import.json"), &Value::Object(import_json))?;
    write_pretty_json(
        &output_dir.join(&lod_name).join("Config.json"),
        &Value::Array(config_json),
    )?;

    let export_report = json!({
        "sourceDumpDir": display_path(&model.dump_dir),
        "outputDir": display_path(&output_dir),
        "lodName": lod_name,
        "gamePreset": game_preset,
        "gameType": game_type,
        "exported": exported,
        "skipped": skipped,
        "notes": [
            "Generated from ProjectBunny DX12 IA dump data.",
            "D3D11ElementList is inferred from IA slot/stride because the DX12 dump does not currently record original input-layout semantic names.",
            "TheHerta4 can scan this workspace structure; Blender import quality depends on whether the dumped buffers contain real POSITION-like vertex data."
        ],
        "diagnostics": diagnostics,
        "submeshes": exported_submeshes,
    });
    write_pretty_json(&output_dir.join("ZaychikExportReport.json"), &export_report)?;

    if exported == 0 {
        diagnostics.push(Diagnostic::error(
            "No submeshes were exported.",
            "No draw had an index buffer, at least one existing vertex buffer, a POSITION-like VS input, and medium confidence.".to_string(),
        ));
    }

    Ok(ExportResult {
        output_dir: display_path(&output_dir),
        exported,
        skipped,
        diagnostics,
        exported_submeshes,
    })
}

fn load_dump_model(dump_dir: &Path) -> Result<DumpModel, String> {
    let mut diagnostics = Vec::new();
    let mut summary = scan_basic_summary(dump_dir)?;

    for required in [DRAW_CALLS_FILE, BUFFERS_FILE] {
        if !dump_dir.join(required).exists() {
            diagnostics.push(Diagnostic::error(
                "Required dump file is missing.",
                required.to_string(),
            ));
        }
    }

    if !dump_dir.join(RESOURCE_FOLDER).is_dir() {
        diagnostics.push(Diagnostic::error(
            "Resource file folder is missing.",
            format!("Expected {}", RESOURCE_FOLDER),
        ));
    }

    let draw_table = read_csv_file(&dump_dir.join(DRAW_CALLS_FILE))?;
    let buffer_table = read_csv_file(&dump_dir.join(BUFFERS_FILE))?;
    summary.draw_rows = draw_table.rows.len();

    let buffers = parse_buffers(&buffer_table);
    summary.vertex_buffers = buffers.values().filter(|b| b.role == "VB").count();
    summary.index_buffers = buffers.values().filter(|b| b.role == "IB").count();

    let draw_records = parse_draws(&draw_table);
    summary.draw_indexed_rows = draw_records
        .iter()
        .filter(|draw| draw.draw_type == "draw_indexed")
        .count();

    let mut draw_candidates = Vec::new();
    let mut missing_buffer_files = 0usize;

    for draw in draw_records
        .iter()
        .filter(|draw| draw.draw_type == "draw_indexed")
    {
        let candidate = build_candidate(dump_dir, draw, &buffers);
        missing_buffer_files += candidate.vbs.iter().filter(|buffer| !buffer.exists).count();
        if candidate.ib.as_ref().is_some_and(|buffer| !buffer.exists) {
            missing_buffer_files += 1;
        }

        if candidate.exportable {
            summary.exportable_draws += 1;
            if candidate.confidence == "medium" {
                summary.default_exportable_draws += 1;
            }
        } else {
            summary.skipped_draws += 1;
        }

        if draw_candidates.len() < MAX_PREVIEW_DRAWS {
            draw_candidates.push(candidate);
        }
    }

    summary.missing_buffer_files = missing_buffer_files;
    summary.binding_events =
        read_binding_event_count(&dump_dir.join(BINDING_TRACE_FILE)).unwrap_or(0);

    push_summary_diagnostics(&summary, &mut diagnostics);
    push_position_diagnostics(&draw_candidates, &summary, &mut diagnostics);

    Ok(DumpModel {
        dump_dir: dump_dir.to_path_buf(),
        summary,
        diagnostics,
        draw_candidates,
    })
}

fn scan_basic_summary(dump_dir: &Path) -> Result<DumpSummary, String> {
    let mut summary = DumpSummary::default();

    for entry in fs::read_dir(dump_dir).map_err(|e| {
        format!(
            "Failed to read dump directory {}: {}",
            display_path(dump_dir),
            e
        )
    })? {
        let entry = entry.map_err(|e| e.to_string())?;
        if entry.file_type().map_err(|e| e.to_string())?.is_file() {
            summary.root_file_count += 1;
            let name = entry.file_name();
            let name = name.to_string_lossy();
            if name.ends_with(".asm.txt") || name.ends_with(".bin") {
                summary.shader_file_count += 1;
            }
        }
    }

    let resource_dir = dump_dir.join(RESOURCE_FOLDER);
    if resource_dir.is_dir() {
        for entry in fs::read_dir(&resource_dir).map_err(|e| {
            format!(
                "Failed to read resource directory {}: {}",
                display_path(&resource_dir),
                e
            )
        })? {
            let entry = entry.map_err(|e| e.to_string())?;
            if entry.file_type().map_err(|e| e.to_string())?.is_file() {
                summary.resource_file_count += 1;
                summary.resource_bytes += entry.metadata().map_err(|e| e.to_string())?.len();
            }
        }
    }

    Ok(summary)
}

fn push_summary_diagnostics(summary: &DumpSummary, diagnostics: &mut Vec<Diagnostic>) {
    if summary.draw_rows == 0 {
        diagnostics.push(Diagnostic::error(
            "DrawCallsDX12.csv has no rows.",
            "The dump cannot be converted to a model workspace without captured draw calls."
                .to_string(),
        ));
    }

    if summary.draw_indexed_rows == 0 {
        diagnostics.push(Diagnostic::error(
            "No draw_indexed rows were captured.",
            "TheHerta4 import needs index-buffer backed triangles.".to_string(),
        ));
    }

    if summary.vertex_buffers == 0 || summary.index_buffers == 0 {
        diagnostics.push(Diagnostic::error(
            "No IA vertex/index buffers were captured.",
            format!(
                "VB rows: {}, IB rows: {}",
                summary.vertex_buffers, summary.index_buffers
            ),
        ));
    }

    if summary.exportable_draws == 0 && summary.draw_indexed_rows > 0 {
        diagnostics.push(Diagnostic::error(
            "No draw has both an index buffer and usable vertex buffers.",
            "Rows with empty vb_slots or missing copied .buf files are intentionally skipped."
                .to_string(),
        ));
    }

    if summary.exportable_draws > 0 && summary.default_exportable_draws == 0 {
        diagnostics.push(Diagnostic::warning(
            "Only low-confidence draw candidates were found.",
            "The dump has copied IA buffers, but no draw has a POSITION-like vertex input. Enable low-confidence export only if you want an experimental workspace shell.".to_string(),
        ));
    }

    if summary.missing_buffer_files > 0 {
        diagnostics.push(Diagnostic::warning(
            "Some buffer rows point to missing files.",
            format!(
                "Missing buffer file references: {}",
                summary.missing_buffer_files
            ),
        ));
    }

    if summary.exportable_draws > 0 {
        diagnostics.push(Diagnostic::info(
            "Workspace export can run.",
            format!(
                "{} default-safe draw(s), {} low-confidence draw(s). Element semantics will be inferred from slot/stride.",
                summary.default_exportable_draws,
                summary.exportable_draws.saturating_sub(summary.default_exportable_draws)
            ),
        ));
    }
}

fn push_position_diagnostics(
    draw_candidates: &[DrawCandidate],
    summary: &DumpSummary,
    diagnostics: &mut Vec<Diagnostic>,
) {
    if summary.exportable_draws == 0 || summary.default_exportable_draws > 0 {
        return;
    }

    let mut slot0_stride_counts = BTreeMap::<u64, usize>::new();
    let mut vs_input_counts = BTreeMap::<String, usize>::new();
    let mut no_position_input = 0usize;
    let mut no_position_stride = 0usize;

    for candidate in draw_candidates
        .iter()
        .filter(|candidate| candidate.exportable)
    {
        if !candidate.has_position_input {
            no_position_input += 1;
        }

        let slot0 = candidate.vbs.iter().find(|buffer| buffer.slot == 0);
        if let Some(slot0) = slot0 {
            *slot0_stride_counts.entry(slot0.stride).or_default() += 1;
            if slot0.stride < 12 {
                no_position_stride += 1;
            }
        } else {
            no_position_stride += 1;
        }

        for input in &candidate.vs_inputs {
            *vs_input_counts.entry(input.clone()).or_default() += 1;
        }
    }

    let stride_text = slot0_stride_counts
        .iter()
        .map(|(stride, count)| format!("{} bytes x{}", stride, count))
        .collect::<Vec<_>>()
        .join(", ");
    let input_text = vs_input_counts
        .iter()
        .take(12)
        .map(|(input, count)| format!("{} x{}", input, count))
        .collect::<Vec<_>>()
        .join(", ");

    diagnostics.push(Diagnostic::warning(
        "Default export is blocked by position detection, not by missing IA files.",
        format!(
            "{} exportable IA draw(s) were found, but {} lack POSITION-like VS input and {} have no slot0 buffer wide enough for xyz. Slot0 stride summary: {}. VS input summary: {}.",
            summary.exportable_draws,
            no_position_input,
            no_position_stride,
            if stride_text.is_empty() { "none".to_string() } else { stride_text },
            if input_text.is_empty() { "no NONE IA inputs, often SV_VertexID/instanced rendering".to_string() } else { input_text },
        ),
    ));
}

fn read_csv_file(path: &Path) -> Result<CsvTable, String> {
    if !path.exists() {
        return Ok(CsvTable { rows: Vec::new() });
    }

    let content = fs::read_to_string(path)
        .map_err(|e| format!("Failed to read {}: {}", display_path(path), e))?;
    let mut records = parse_csv(&content);
    if records.is_empty() {
        return Ok(CsvTable { rows: Vec::new() });
    }

    let headers = records.remove(0);
    let rows = records
        .into_iter()
        .filter(|row| row.iter().any(|value| !value.trim().is_empty()))
        .map(|row| {
            headers
                .iter()
                .enumerate()
                .map(|(index, header)| {
                    (header.clone(), row.get(index).cloned().unwrap_or_default())
                })
                .collect::<HashMap<_, _>>()
        })
        .collect();

    Ok(CsvTable { rows })
}

fn parse_csv(content: &str) -> Vec<Vec<String>> {
    let mut rows = Vec::new();
    let mut row = Vec::new();
    let mut field = String::new();
    let mut chars = content.chars().peekable();
    let mut in_quotes = false;

    while let Some(ch) = chars.next() {
        match ch {
            '"' if in_quotes && chars.peek() == Some(&'"') => {
                chars.next();
                field.push('"');
            }
            '"' => in_quotes = !in_quotes,
            ',' if !in_quotes => {
                row.push(field.trim().to_string());
                field.clear();
            }
            '\n' if !in_quotes => {
                row.push(field.trim_end_matches('\r').trim().to_string());
                field.clear();
                rows.push(row);
                row = Vec::new();
            }
            '\r' if !in_quotes => {}
            _ => field.push(ch),
        }
    }

    if !field.is_empty() || !row.is_empty() {
        row.push(field.trim().to_string());
        rows.push(row);
    }

    rows
}

fn parse_buffers(table: &CsvTable) -> HashMap<String, BufferRecord> {
    table
        .rows
        .iter()
        .filter_map(|row| {
            let buffer_id = csv_value(row, "buffer_id");
            if buffer_id.is_empty() {
                return None;
            }

            Some((
                buffer_id.clone(),
                BufferRecord {
                    buffer_id,
                    role: csv_value(row, "role"),
                    file: csv_value(row, "file"),
                    size: parse_u64(&csv_value(row, "size")),
                    stride: parse_u64(&csv_value(row, "stride")),
                    slot: parse_u32(&csv_value(row, "slot")),
                    format: csv_value(row, "format"),
                },
            ))
        })
        .collect()
}

fn parse_draws(table: &CsvTable) -> Vec<DrawRecord> {
    table
        .rows
        .iter()
        .map(|row| DrawRecord {
            draw_id: parse_u64(&csv_value(row, "draw_id")),
            draw_type: csv_value(row, "type"),
            pso: csv_value(row, "pso"),
            vs: csv_value(row, "vs"),
            ps: csv_value(row, "ps"),
            topology: csv_value(row, "topology"),
            index_count: parse_u64(&csv_value(row, "index_count")),
            start_index: parse_u64(&csv_value(row, "start_index")),
            base_vertex: parse_i64(&csv_value(row, "base_vertex")),
            vb_slots: csv_value(row, "vb_slots"),
            ib_resource: csv_value(row, "ib_resource"),
        })
        .collect()
}

fn build_candidate(
    dump_dir: &Path,
    draw: &DrawRecord,
    buffers: &HashMap<String, BufferRecord>,
) -> DrawCandidate {
    let mut notes = Vec::new();
    let vs_inputs = parse_vs_inputs(dump_dir, &draw.vs);
    let has_position_input = vs_inputs.iter().any(|input| {
        input.starts_with("POSITION")
            || input.starts_with("POSITIONT")
            || input.starts_with("TEXCOORD.xyz")
    });
    let ib = buffers
        .get(&draw.ib_resource)
        .map(|buffer| buffer_preview(dump_dir, buffer));

    if draw.ib_resource.is_empty() {
        notes.push("draw row has no ib_resource".to_string());
    } else if ib.is_none() {
        notes.push(format!(
            "ib_resource {} was not found in BuffersDX12.csv",
            draw.ib_resource
        ));
    }

    let mut vbs = parse_vb_slots(&draw.vb_slots)
        .into_iter()
        .filter_map(|(_, buffer_id)| {
            let buffer = buffers.get(&buffer_id);
            if buffer.is_none() {
                notes.push(format!("VB {} was not found in BuffersDX12.csv", buffer_id));
            }
            buffer.map(|record| buffer_preview(dump_dir, record))
        })
        .collect::<Vec<_>>();

    vbs.sort_by_key(|buffer| buffer.slot);

    if vbs.is_empty() {
        notes.push("draw row has empty vb_slots".to_string());
    }

    for vb in &vbs {
        if vb.stride == 0 {
            notes.push(format!("{} has zero stride", vb.buffer_id));
        }
        if !vb.exists {
            notes.push(format!("{} file is missing", vb.buffer_id));
        }
    }

    if ib.as_ref().is_some_and(|buffer| !buffer.exists) {
        notes.push("index buffer file is missing".to_string());
    }

    if draw.topology != "TRIANGLELIST" {
        notes.push(format!("topology is {}, not TRIANGLELIST", draw.topology));
    }

    if draw.vs.is_empty() {
        notes.push("draw row has no vertex shader hash".to_string());
    } else if vs_inputs.is_empty() {
        notes.push("vertex shader input signature was not found or has no IA inputs".to_string());
    } else if !has_position_input {
        notes.push(format!(
            "vertex shader inputs do not include POSITION ({})",
            vs_inputs.join(", ")
        ));
    }

    let has_usable_vb = vbs.iter().any(|buffer| buffer.exists && buffer.stride > 0);
    let has_usable_ib = ib.as_ref().is_some_and(|buffer| buffer.exists);
    let has_position_buffer = vbs
        .iter()
        .any(|buffer| buffer.slot == 0 && buffer.exists && buffer.stride >= 12);
    let exportable = has_usable_vb && has_usable_ib && draw.index_count > 0;
    let confidence = if exportable
        && draw.topology == "TRIANGLELIST"
        && has_position_input
        && has_position_buffer
    {
        "medium"
    } else if exportable {
        "low"
    } else {
        "none"
    };

    if confidence == "medium" {
        notes.push(
            "IA buffers exist; D3D element semantics are inferred, not captured.".to_string(),
        );
    }

    DrawCandidate {
        draw_id: draw.draw_id,
        pso: draw.pso.clone(),
        vs: draw.vs.clone(),
        ps: draw.ps.clone(),
        topology: draw.topology.clone(),
        index_count: draw.index_count,
        first_index: draw.start_index,
        base_vertex: draw.base_vertex,
        ib,
        vbs,
        exportable,
        confidence: confidence.to_string(),
        has_position_input,
        vs_inputs,
        notes,
    }
}

fn parse_vb_slots(value: &str) -> Vec<(u32, String)> {
    value
        .split(';')
        .filter_map(|item| {
            let (slot, buffer_id) = item.split_once(':')?;
            Some((parse_u32(slot), buffer_id.trim().to_string()))
        })
        .filter(|(_, buffer_id)| !buffer_id.is_empty())
        .collect()
}

fn buffer_preview(dump_dir: &Path, buffer: &BufferRecord) -> BufferPreview {
    let file_path = dump_dir.join(&buffer.file);
    BufferPreview {
        buffer_id: buffer.buffer_id.clone(),
        role: buffer.role.clone(),
        slot: buffer.slot,
        stride: buffer.stride,
        size: buffer.size,
        format: buffer.format.clone(),
        file: buffer.file.clone(),
        exists: file_path.is_file(),
    }
}

fn read_binding_event_count(path: &Path) -> Option<usize> {
    let content = fs::read_to_string(path).ok()?;
    for line in content.lines() {
        for item in line.split_whitespace() {
            let (key, value) = item.split_once('=')?;
            if key == "events" {
                return value.parse().ok();
            }
        }
    }

    None
}

fn parse_vs_inputs(dump_dir: &Path, vs_hash: &str) -> Vec<String> {
    if vs_hash.trim().is_empty() {
        return Vec::new();
    }

    let path = dump_dir.join(format!("{}-vs.asm.txt", vs_hash.trim()));
    let Ok(content) = fs::read_to_string(path) else {
        return Vec::new();
    };

    let mut in_input_signature = false;
    let mut saw_column_header = false;
    let mut result = Vec::new();

    for line in content.lines() {
        let line = line.trim();
        if line == "; Input signature:" {
            in_input_signature = true;
            saw_column_header = false;
            continue;
        }

        if !in_input_signature {
            continue;
        }

        if line == "; Output signature:" {
            break;
        }

        let row = line.trim_start_matches(';').trim();
        if row.is_empty() {
            continue;
        }

        if row.starts_with("Name") {
            saw_column_header = true;
            continue;
        }

        if !saw_column_header || row.starts_with("---") {
            continue;
        }

        let columns = row.split_whitespace().collect::<Vec<_>>();
        if columns.len() < 6 {
            continue;
        }

        let name = columns[0];
        let index = columns[1];
        let mask = columns[2];
        let sys_value = columns[4];
        if sys_value != "NONE" {
            continue;
        }

        result.push(format!(
            "{}{}.{}",
            name,
            if index == "0" { "" } else { index },
            mask
        ));
    }

    result
}

fn elements_for_vertex_buffer(
    vb: &BufferPreview,
    category: &str,
    candidate: &DrawCandidate,
) -> Vec<Value> {
    if vb.slot == 0 && candidate.has_position_input && vb.stride >= 12 {
        let mut elements = vec![element_json(
            "POSITION",
            0,
            "R32G32B32_FLOAT",
            12,
            vb.slot,
            category,
        )];
        append_padding_element(&mut elements, vb.stride - 12, vb.slot, category);
        return elements;
    }

    if vb.slot == 2 && vb.stride >= 12 {
        let mut elements = vec![element_json(
            "NORMAL",
            0,
            "R32G32B32_FLOAT",
            12,
            vb.slot,
            category,
        )];
        append_padding_element(&mut elements, vb.stride - 12, vb.slot, category);
        return elements;
    }

    if matches!(vb.stride, 8 | 16) {
        return vec![element_json(
            "TEXCOORD",
            texcoord_index_for_slot(vb.slot),
            if vb.stride == 8 {
                "R32G32_FLOAT"
            } else {
                "R32G32B32A32_FLOAT"
            },
            vb.stride,
            vb.slot,
            category,
        )];
    }

    if vb.stride == 12 {
        return vec![
            element_json(
                "TEXCOORD",
                texcoord_index_for_slot(vb.slot),
                "R32G32_FLOAT",
                8,
                vb.slot,
                category,
            ),
            element_json(
                "COLOR",
                color_index_for_slot(vb.slot),
                "R8_UINT",
                4,
                vb.slot,
                category,
            ),
        ];
    }

    vec![element_json(
        "COLOR",
        color_index_for_slot(vb.slot),
        "R8_UINT",
        vb.stride,
        vb.slot,
        category,
    )]
}

fn append_padding_element(elements: &mut Vec<Value>, byte_width: u64, slot: u32, category: &str) {
    if byte_width == 0 {
        return;
    }

    elements.push(element_json(
        "COLOR",
        color_index_for_slot(slot),
        "R8_UINT",
        byte_width,
        slot,
        category,
    ));
}

fn element_json(
    semantic_name: &str,
    semantic_index: u32,
    format: &str,
    byte_width: u64,
    slot: u32,
    category: &str,
) -> Value {
    json!({
        "SemanticName": semantic_name,
        "SemanticIndex": semantic_index,
        "Format": format,
        "ByteWidth": byte_width.to_string(),
        "ExtractSlot": format!("vb{}", slot),
        "ExtractTechnique": "trianglelist",
        "Category": category,
        "DrawCategory": category,
    })
}

fn texcoord_index_for_slot(slot: u32) -> u32 {
    slot.saturating_sub(1)
}

fn color_index_for_slot(slot: u32) -> u32 {
    slot + 16
}

fn category_for_slot(slot: u32) -> String {
    match slot {
        0 => "Position".to_string(),
        1 => "Texcoord".to_string(),
        2 => "Normal".to_string(),
        3 => "Color".to_string(),
        _ => format!("Slot{}", slot),
    }
}

fn copy_buffer_file(
    dump_dir: &Path,
    relative_file: &str,
    dest: &Path,
    diagnostics: &mut Vec<Diagnostic>,
) -> Result<(), String> {
    let source = dump_dir.join(relative_file);
    if !source.is_file() {
        return Err(format!("Missing buffer file: {}", display_path(&source)));
    }

    if let Some(parent) = dest.parent() {
        fs::create_dir_all(parent).map_err(|e| {
            format!(
                "Failed to create destination directory {}: {}",
                display_path(parent),
                e
            )
        })?;
    }

    fs::copy(&source, dest).map_err(|e| {
        format!(
            "Failed to copy {} to {}: {}",
            display_path(&source),
            display_path(dest),
            e
        )
    })?;

    if dest.extension() == Some(OsStr::new("ib")) {
        diagnostics.push(Diagnostic::info("Copied index buffer.", display_path(dest)));
    }

    Ok(())
}

fn write_pretty_json(path: &Path, value: &Value) -> Result<(), String> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent).map_err(|e| {
            format!(
                "Failed to create JSON parent directory {}: {}",
                display_path(parent),
                e
            )
        })?;
    }

    let content = serde_json::to_string_pretty(value).map_err(|e| e.to_string())?;
    fs::write(path, content).map_err(|e| format!("Failed to write {}: {}", display_path(path), e))
}

fn dxgi_format_name(format: &str) -> String {
    match format.trim() {
        "57" => "R16_UINT",
        "42" => "R32_UINT",
        "DXGI_FORMAT_R16_UINT" => "R16_UINT",
        "DXGI_FORMAT_R32_UINT" => "R32_UINT",
        "R16_UINT" => "R16_UINT",
        "R32_UINT" => "R32_UINT",
        _ => "R32_UINT",
    }
    .to_string()
}

fn drawib_name(candidate: &DrawCandidate) -> String {
    if let Some(ib) = candidate.ib.as_ref() {
        let suffix = ib
            .buffer_id
            .strip_prefix("ib_")
            .unwrap_or(&ib.buffer_id)
            .trim();
        return format!("dx12ib{:0>6}", suffix);
    }

    format!("dx12draw{:0>6}", candidate.draw_id)
}

fn unique_folder_name(used: &mut HashSet<String>, desired: &str) -> String {
    let base = sanitize_name(desired, "submesh");
    if used.insert(base.clone()) {
        return base;
    }

    for index in 1.. {
        let candidate = format!("{}-{}", base, index);
        if used.insert(candidate.clone()) {
            return candidate;
        }
    }

    unreachable!()
}

fn sanitize_name(value: &str, fallback: &str) -> String {
    let sanitized = value
        .chars()
        .map(|ch| {
            if ch.is_ascii_alphanumeric() || matches!(ch, '_' | '-' | '.') {
                ch
            } else {
                '_'
            }
        })
        .collect::<String>()
        .trim_matches('_')
        .to_string();

    if sanitized.is_empty() {
        fallback.to_string()
    } else {
        sanitized
    }
}

fn normalize_options(mut options: ExportOptions) -> ExportOptions {
    if options.game_preset.trim().is_empty() {
        options.game_preset = default_game_preset();
    }
    if options.game_type.trim().is_empty() {
        options.game_type = default_game_type();
    }
    if options.lod_name.trim().is_empty() {
        options.lod_name = default_lod_name();
    }
    if options.max_draws == 0 {
        options.max_draws = default_max_draws();
    }
    options
}

fn default_output_dir(dump_dir: &Path) -> PathBuf {
    dump_dir
        .parent()
        .unwrap_or(dump_dir)
        .join("Zaychik_TheHerta4_Workspace")
}

fn display_path(path: &Path) -> String {
    path.to_string_lossy().to_string()
}

fn csv_value(row: &HashMap<String, String>, key: &str) -> String {
    row.get(key).cloned().unwrap_or_default()
}

fn parse_u64(value: &str) -> u64 {
    value.trim().parse::<u64>().unwrap_or(0)
}

fn parse_u32(value: &str) -> u32 {
    value.trim().parse::<u32>().unwrap_or(0)
}

fn parse_i64(value: &str) -> i64 {
    value.trim().parse::<i64>().unwrap_or(0)
}

fn default_game_preset() -> String {
    "ProjectBunnyDX12".to_string()
}

fn default_game_type() -> String {
    "DX12_IA_Inferred".to_string()
}

fn default_lod_name() -> String {
    "LOD0".to_string()
}

fn default_max_draws() -> usize {
    100
}

impl Diagnostic {
    fn error(message: impl Into<String>, detail: impl Into<String>) -> Self {
        Self {
            level: "error".to_string(),
            message: message.into(),
            detail: detail.into(),
        }
    }

    fn warning(message: impl Into<String>, detail: impl Into<String>) -> Self {
        Self {
            level: "warning".to_string(),
            message: message.into(),
            detail: detail.into(),
        }
    }

    fn info(message: impl Into<String>, detail: impl Into<String>) -> Self {
        Self {
            level: "info".to_string(),
            message: message.into(),
            detail: detail.into(),
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::time::{SystemTime, UNIX_EPOCH};

    #[test]
    fn exports_position_draw_to_theherta_workspace_layout() {
        let root = std::env::temp_dir().join(format!(
            "zaychik-dx12-test-{}",
            SystemTime::now()
                .duration_since(UNIX_EPOCH)
                .unwrap()
                .as_nanos()
        ));
        let dump = root.join("dump");
        let resources = dump.join(RESOURCE_FOLDER);
        fs::create_dir_all(&resources).unwrap();

        fs::write(
            dump.join(DRAW_CALLS_FILE),
            "draw_id,dispatch_id,type,serial,command_list,pipeline_state,pso,vs,ps,cs,topology,vertex_count,index_count,start_vertex,start_index,base_vertex,instance_count,start_instance,groups_x,groups_y,groups_z,vb_slots,ib_resource,resource_refs\n1,0,draw_indexed,1,cmd,pipeline,7,abc,def,,TRIANGLELIST,0,3,0,0,0,1,0,0,0,0,0:vb_1,ib_1,\n",
        )
        .unwrap();
        fs::write(
            dump.join(BUFFERS_FILE),
            "buffer_id,role,resource,file,gpu_va,resource_gpu_va,offset,size,stride,slot,format,resolved,current_state,has_current_state,heap_type,resource_size\nvb_1,VB,res,CurrentFrameResourceFiles\\vb.buf,0x1,0x1,0,36,12,0,0,1,0x0,1,0,36\nib_1,IB,res,CurrentFrameResourceFiles\\ib.buf,0x2,0x2,0,6,0,0,57,1,0x0,1,0,6\n",
        )
        .unwrap();
        fs::write(
            dump.join(BINDING_TRACE_FILE),
            "DX12 Current Frame Binding Trace\n================================\nevents=1 dropped=0 max_events=20000\n",
        )
        .unwrap();
        fs::write(
            dump.join("abc-vs.asm.txt"),
            ";\n; Input signature:\n;\n; Name                 Index   Mask Register SysValue  Format   Used\n; -------------------- ----- ------ -------- -------- ------- ------\n; POSITION                 0   xyz         0     NONE   float   xyz \n;\n; Output signature:\n",
        )
        .unwrap();
        fs::write(resources.join("vb.buf"), vec![0u8; 36]).unwrap();
        fs::write(resources.join("ib.buf"), vec![0u8; 6]).unwrap();

        let inspection = inspect_dump_directory(&dump).unwrap();
        assert_eq!(inspection.status, "ready");
        assert_eq!(inspection.summary.exportable_draws, 1);

        let output = root.join("workspace");
        let result = export_blender_workspace(
            &dump,
            &output,
            ExportOptions {
                overwrite: true,
                ..ExportOptions::default()
            },
        )
        .unwrap();

        assert_eq!(result.exported, 1);
        assert!(output.join("Import.json").is_file());
        assert!(output
            .join("LOD0")
            .join("dx12ib000001-3-0")
            .join("TYPE_DX12_IA_Inferred")
            .join("dx12ib000001-3-0.json")
            .is_file());

        let _ = fs::remove_dir_all(root);
    }
}
