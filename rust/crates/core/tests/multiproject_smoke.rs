use serde_json::{json, Value};
use std::fs;
use streamfind_rust_core::{ErrorCode, Project, ProjectOptions};

fn fixture() -> Value {
    serde_json::from_str(include_str!(
        "../../../../cpp/tests/fixtures/project/multiproject.json"
    ))
    .unwrap()
}

fn domain_schema_fixture() -> Value {
    serde_json::from_str(include_str!(
        "../../../../cpp/tests/fixtures/project/domain_schema_manifest.json"
    ))
    .unwrap()
}

#[test]
fn one_project_owns_each_duckdb_file() {
    let expected = fixture();
    let root = streamfind_rust_test_support::tmp_projects_dir();
    let first_path = root.join("streamfind-rust-project-a.duckdb");
    let second_path = root.join("streamfind-rust-project-b.duckdb");
    let _ = fs::remove_file(&first_path);
    let _ = fs::remove_file(&second_path);
    let first_spec = &expected["databases"][0];
    let second_spec = &expected["databases"][1];
    let first_options = ProjectOptions {
        database_path: first_path.clone(),
        domain: first_spec["domain_id"].as_str().unwrap().into(),
        create_if_missing: false,
        read_only: false,
    };
    let second_options = ProjectOptions {
        database_path: second_path.clone(),
        domain: second_spec["domain_id"].as_str().unwrap().into(),
        create_if_missing: false,
        read_only: false,
    };
    let mut first = Project::create(first_options.clone()).unwrap();
    first.set_metadata(json!({"owner": "project-a"})).unwrap();
    let schema = domain_schema_fixture();
    assert!(first.validate().is_err());
    for table in schema["domains"]["mass_spec"]["required_tables"]
        .as_array()
        .unwrap()
    {
        first
            .execute_sql(&format!(
                "CREATE TABLE {} (marker VARCHAR)",
                table.as_str().unwrap()
            ))
            .unwrap();
    }
    first.validate().unwrap();
    let same_file = Project::create(ProjectOptions {
        database_path: first_path.clone(),
        domain: "mass_spec".into(),
        create_if_missing: false,
        read_only: false,
    });
    match same_file {
        Err(error) => assert_eq!(error.code, ErrorCode::ProjectAlreadyExists),
        Ok(_) => panic!("second project was accepted in one DuckDB file"),
    }
    let mut second = Project::create(second_options.clone()).unwrap();
    second.set_metadata(json!({"owner": "project-b"})).unwrap();
    assert_eq!(first.get_domain(), first_spec["domain_id"]);
    assert_eq!(second.get_domain(), second_spec["domain_id"]);
    assert_eq!(first.get_metadata()["owner"], "project-a");
    assert_eq!(second.get_metadata()["owner"], "project-b");
    first.close();
    second.close();
    let reopened_first = Project::open(first_options).unwrap();
    let reopened_second = Project::open(second_options).unwrap();
    assert_eq!(reopened_first.get_domain(), first_spec["domain_id"]);
    assert_eq!(reopened_second.get_domain(), second_spec["domain_id"]);
    reopened_first.close();
    reopened_second.close();

    let invalid_path = root.join("streamfind-rust-invalid-registry.duckdb");
    let _ = fs::remove_file(&invalid_path);
    let invalid_options = ProjectOptions {
        database_path: invalid_path.clone(),
        domain: "raman".into(),
        create_if_missing: false,
        read_only: false,
    };
    let invalid = Project::create(invalid_options.clone()).unwrap();
    invalid
        .execute_sql("CREATE TABLE PROJECTS (marker VARCHAR NOT NULL, domain_id VARCHAR NOT NULL)")
        .unwrap();
    invalid.close();
    let error = match Project::open(invalid_options) {
        Ok(_) => panic!("legacy PROJECTS registry was accepted"),
        Err(error) => error,
    };
    assert_eq!(error.code, ErrorCode::SchemaMismatch);
    fs::remove_file(invalid_path).unwrap();

    fs::remove_file(first_path).unwrap();
    fs::remove_file(second_path).unwrap();
}
