use std::time::{SystemTime, UNIX_EPOCH};

use streamfind_rust_core::{Project, ProjectOptions, ProjectTableStore};

fn project() -> (Project, std::path::PathBuf) {
    let stamp = SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap()
        .as_nanos();
    let path = streamfind_rust_test_support::tmp_projects_dir()
        .join(format!("manifest-proof-{stamp}.duckdb"));
    let project = Project::create(ProjectOptions {
        database_path: path.clone(),
        domain: "mass_spec".into(),
        create_if_missing: false,
        read_only: false,
    })
    .unwrap();
    (project, path)
}

fn sql_type(kind: &str) -> &'static str {
    match kind.rsplit('#').next().unwrap_or(kind) {
        "boolean" => "BOOLEAN",
        "integer" => "INTEGER",
        "real" => "DOUBLE",
        "timestamp" => "TIMESTAMP",
        _ => "VARCHAR",
    }
}

fn install(project: &mut Project, omit_table: bool, omit_column: bool, wrong_type: bool) {
    let manifest = streamfind_rust_core::catalogue::table_manifest("mass_spec").unwrap();
    let first_table = manifest
        .iter()
        .find(|table| table.0.starts_with("MASS_SPEC_"))
        .map(|table| table.0.clone());
    let first_column = manifest
        .iter()
        .find(|table| table.0.starts_with("MASS_SPEC_"))
        .and_then(|table| table.1.first().map(|column| column.0.clone()));
    for (table, columns) in manifest {
        if omit_table && Some(table.clone()) == first_table {
            continue;
        }
        let definitions = columns
            .into_iter()
            .filter(|(name, _)| !(omit_column && Some(name.clone()) == first_column))
            .map(|(name, kind)| {
                let ty = if wrong_type && Some(name.clone()) == first_column {
                    "INTEGER"
                } else {
                    sql_type(&kind)
                };
                format!("\"{name}\" {ty}")
            })
            .collect::<Vec<_>>();
        project
            .execute_sql(&format!(
                "CREATE TABLE IF NOT EXISTS \"{table}\" ({})",
                definitions.join(", ")
            ))
            .unwrap();
    }
}

#[test]
fn valid_generated_manifest_is_accepted() {
    let (mut project, path) = project();
    install(&mut project, false, false, false);
    ProjectTableStore::new(&project)
        .require_manifest("mass_spec")
        .unwrap();
    drop(project);
    std::fs::remove_file(path).unwrap();
}

#[test]
fn missing_table_is_rejected() {
    let (mut project, path) = project();
    install(&mut project, true, false, false);
    assert!(ProjectTableStore::new(&project)
        .require_manifest("mass_spec")
        .is_err());
    drop(project);
    std::fs::remove_file(path).unwrap();
}

#[test]
fn missing_column_is_rejected() {
    let (mut project, path) = project();
    install(&mut project, false, true, false);
    assert!(ProjectTableStore::new(&project)
        .require_manifest("mass_spec")
        .is_err());
    drop(project);
    std::fs::remove_file(path).unwrap();
}

#[test]
fn incompatible_column_type_is_rejected() {
    let (mut project, path) = project();
    install(&mut project, false, false, true);
    assert!(ProjectTableStore::new(&project)
        .require_manifest("mass_spec")
        .is_err());
    drop(project);
    std::fs::remove_file(path).unwrap();
}
