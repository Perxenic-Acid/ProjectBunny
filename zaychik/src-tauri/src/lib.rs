mod dx12_dump;

use std::path::PathBuf;

use dx12_dump::{DumpInspection, ExportOptions, ExportResult};
use tauri_plugin_dialog::DialogExt;

#[tauri::command]
fn select_dump_directory(app: tauri::AppHandle) -> Result<Option<String>, String> {
    let folder = app
        .dialog()
        .file()
        .set_title("Select ProjectBunny DX12 dump directory")
        .blocking_pick_folder();

    folder
        .map(|path| {
            path.into_path()
                .map(|path| path.to_string_lossy().to_string())
                .map_err(|e| e.to_string())
        })
        .transpose()
}

#[tauri::command]
fn select_output_directory(app: tauri::AppHandle) -> Result<Option<String>, String> {
    let folder = app
        .dialog()
        .file()
        .set_title("Select TheHerta4 workspace output directory")
        .blocking_pick_folder();

    folder
        .map(|path| {
            path.into_path()
                .map(|path| path.to_string_lossy().to_string())
                .map_err(|e| e.to_string())
        })
        .transpose()
}

#[tauri::command]
fn inspect_dump_directory(path: String) -> Result<DumpInspection, String> {
    dx12_dump::inspect_dump_directory(PathBuf::from(path))
}

#[tauri::command]
fn export_blender_workspace(
    dump_dir: String,
    output_dir: String,
    options: ExportOptions,
) -> Result<ExportResult, String> {
    dx12_dump::export_blender_workspace(PathBuf::from(dump_dir), PathBuf::from(output_dir), options)
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    tauri::Builder::default()
        .plugin(tauri_plugin_opener::init())
        .plugin(tauri_plugin_dialog::init())
        .invoke_handler(tauri::generate_handler![
            select_dump_directory,
            select_output_directory,
            inspect_dump_directory,
            export_blender_workspace
        ])
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
