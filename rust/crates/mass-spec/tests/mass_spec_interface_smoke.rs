use serde_json::json;
use std::collections::BTreeSet;
use std::fs;
use streamfind_rust_core::{OperationRegistry, Project, ProjectOptions};

#[test]
fn mass_spec_interface_matches_catalogue_and_initializes_schema() {
    let entries = streamfind_rust_test_support::catalogue_entries();
    let expected = entries
        .iter()
        .filter(|entry| entry["kind"] == "operation" && entry["domain"] == "mass_spec")
        .count();

    let mut operations = OperationRegistry::default();
    streamfind_rust_mass_spec::register_operations(&mut operations).unwrap();
    assert_eq!(operations.list("mass_spec").len(), expected);
    for operation in operations.list("mass_spec") {
        let id = operation["id"].as_str().unwrap();
        let catalogue_entry = entries
            .iter()
            .find(|entry| entry["canonical_id"] == id)
            .unwrap_or_else(|| panic!("missing catalogue entry for {id}"));
        assert_eq!(catalogue_entry["kind"], "operation");
        assert_eq!(catalogue_entry["domain"], "mass_spec");
        let expected_module = if id.contains("chromatograms") {
            "mass_spec.chromatograms"
        } else if id.contains("features")
            || id.contains("suspects")
            || id.contains("internal_standards")
            || id.contains("transformation_products")
        {
            "mass_spec.nta"
        } else {
            "mass_spec.base"
        };
        assert_eq!(catalogue_entry["module_id"], expected_module);
        let actual: BTreeSet<String> = operation["parameters"]
            .as_array()
            .unwrap()
            .iter()
            .map(|parameter| parameter["name"].as_str().unwrap().to_owned())
            .collect();
        let expected: BTreeSet<String> = catalogue_entry["parameters"]
            .as_array()
            .unwrap()
            .iter()
            .map(|parameter| parameter["name"].as_str().unwrap().to_owned())
            .collect();
        assert_eq!(actual, expected, "parameter mismatch for {id}");
    }

    let database = streamfind_rust_test_support::tmp_projects_dir()
        .join("streamfind-rust-mass-spec-interface.duckdb");
    let _ = fs::remove_file(&database);
    let mut project = Project::create(ProjectOptions {
        database_path: database.clone(),
        domain: "mass_spec".into(),
        create_if_missing: false,
        read_only: false,
    })
    .unwrap();

    let analyses = project
        .run_operation("mass_spec.get_analyses_info", &json!({}), &operations)
        .unwrap();
    assert_eq!(analyses["row_count"], 0);
    for table in [
        "MASS_SPEC_ANALYSES",
        "MASS_SPEC_SPECTRA_HEADERS",
        "MASS_SPEC_CHROMATOGRAMS_HEADERS",
    ] {
        assert!(project
            .list_tables()
            .unwrap()
            .iter()
            .any(|name| name == table));
    }
    drop(project);
    fs::remove_file(database).unwrap();
}
